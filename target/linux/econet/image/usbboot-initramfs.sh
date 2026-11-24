#!/bin/sh
# ============================================================================
#  GS2107 通用引导/救援/网络安装 initramfs 组装器
#  ChinaMobile GS2107 · EcoNet EN7528 · OpenWrt target econet/en7528
#
#  产出：一个 cpio（newc）归档 = 精简 rootfs（busybox + musl + eth 驱动模块）
#        + 我们的 /init 探测脚本。
#  用途：喂给 OpenWrt 的 CONFIG_EXTERNAL_CPIO（Target Images → ramdisk →
#        Use external cpio），由 target/linux/econet/Makefile 里的
#        INITRAMFS_INIT_FILE 指向同一个 /init 源文件，两处保持一致。
#
#  为什么不用默认的"整棵 TARGET_DIR 打进 initramfs"：
#        TARGET_DIR 约 14MB（含 wifi kmod/dnsmasq/firewall4/...），
#        cpio→LZMA 后 4194239B，而构建尾段把它 LZMA 后要能塞进
#        tclinux-hdr2.py 的 --pad 0x500000（5MB）预算 —— 见 image/Makefile 的
#        Build/tclinux-hdr2-initramfs-trx。USB-boot 引导内核只需要
#        busybox + musl + eth 模块（约 1.5MB），完整系统在 U 盘。
#
#  ---- 体积预算（本脚本构建后自检，超了直接失败）--------------------------
#    pad target 0x500000 = 5,242,880 B；HDR2 头占 256 B。
#    可用 kernel 段 = 5,242,624 B（LZMA 压缩后）。
#    最近实测：4,299,056 B（未含 wget/insmod/eth.ko）→ 余量约 921 KiB。
#    wget(+41K) tftp(+11K) insmod/lsmod(+26K) econet-eth.ko(44K) 压缩后
#    增量预计 60~90KB，仍在预算内；但**每次改完必须看脚本尾部打印的
#    "余量" 行**，并跑 tools/hdr2.py 复算。
#
#  用法：
#      ./usbboot-initramfs.sh            # 用默认路径
#      TARGET_DIR=... STAGE=... OUT=... ./usbboot-initramfs.sh
#
#  环境变量：
#      TARGET_DIR   OpenWrt 已构建的 rootfs（默认 build_dir/target-*/root.orig-econet）
#      STAGE        中间 staging 目录
#      OUT          输出 cpio 路径（按当前目录解析）；写进 CONFIG_EXTERNAL_CPIO 时，在主树里的用相对 TOPDIR 的写法
#      INIT_SRC     /init 脚本源（默认 target/linux/econet/other-files/gs2107-usbboot-init）
#      RESCUE_PW    救援 shell 的 root 口令（必填，tools/openwrt/build.sh 从凭据真源 var/credentials/gs2107/rescue.json 传入）
#      RESCUE_SALT  口令哈希的固定盐（必填，来源同上；固定是为了同一提交重复构建逐字节相同）
#      KVER         内核版本目录名（默认自动探测 build_dir 下的 linux-*/lib/modules/*）
#      MAX_KERNEL   体积门禁上限（默认 5242624 = 5MB pad − 256B 头）
# ============================================================================
set -eu

TOPDIR=$(cd "$(dirname "$0")/../../../.." && pwd)
TARGET_DIR=${TARGET_DIR:-$TOPDIR/build_dir/target-mipsel_24kc_musl/root.orig-econet}
STAGE=${STAGE:-$TOPDIR/build_dir/target-mipsel_24kc_musl/gs2107-usbboot-initramfs}
OUT=${OUT:-$STAGE.cpio}
INIT_SRC=${INIT_SRC:-$TOPDIR/target/linux/econet/other-files/gs2107-usbboot-init}
: "${RESCUE_PW:?缺 RESCUE_PW：救援口令由 tools/openwrt/build.sh 从 var/credentials/gs2107/rescue.json 传入}"
: "${RESCUE_SALT:?缺 RESCUE_SALT：口令哈希的盐，来源同 RESCUE_PW}"
CPIO=${CPIO:-$TOPDIR/staging_dir/host/bin/cpio}
[ -x "$CPIO" ] || CPIO=$(command -v cpio)

# 体积上限的唯一真源 = image/Makefile 里 Build/tclinux-hdr2-initramfs-trx 的
# `--pad 0x…`（HDR2 头 256B 要额外扣掉）。不在这里再写一份常量：改 pad 时
# 若忘了同步，本脚本会给出与实际镜像不符的"余量"，那种乐观估算比没有更糟。
# 注意跳过注释行：Makefile 的说明文字里也出现了 "--pad 0x500000"。
MKFILE=$(dirname "$0")/Makefile
PAD_OFFSET=$(grep -v '^[[:space:]]*#' "$MKFILE" \
	| sed -n 's/.*--pad \(0x[0-9a-fA-F]*\).*/\1/p' | head -1)
if [ -n "$PAD_OFFSET" ]; then
	MAX_KERNEL=${MAX_KERNEL:-$((PAD_OFFSET - 0x100))}
else
	PAD_OFFSET=unknown
	MAX_KERNEL=${MAX_KERNEL:-5242624}
	echo "!! 警告：无法从 $MKFILE 解析 --pad，回退默认上限 ${MAX_KERNEL}B" >&2
	echo "!!       请确认 Build/tclinux-hdr2-initramfs-trx 里仍有 --pad 0x…" >&2
fi

[ -d "$TARGET_DIR" ] || { echo "TARGET_DIR 不存在: $TARGET_DIR" >&2; exit 1; }
[ -r "$INIT_SRC" ]   || { echo "init 脚本不存在: $INIT_SRC" >&2; exit 1; }

# 内核版本目录（/lib/modules/<kver>）：优先取已构建 rootfs 里的，取不到退 build_dir
KVER=${KVER:-}
if [ -z "$KVER" ]; then
	for _d in "$TARGET_DIR"/lib/modules/*/; do
		[ -d "$_d" ] && { KVER=${_d%/}; KVER=${KVER##*/}; break; }
	done
fi
if [ -z "$KVER" ]; then
	for _d in "$TOPDIR"/build_dir/target-*/linux-*/linux-*/lib/modules/*/; do
		[ -d "$_d" ] && { KVER=${_d%/}; KVER=${KVER##*/}; break; }
	done
fi
[ -n "$KVER" ] || { echo "无法确定内核版本目录（KVER）" >&2; exit 1; }

echo "== GS2107 usbboot initramfs =="
echo "   TOPDIR     = $TOPDIR"
echo "   TARGET_DIR = $TARGET_DIR"
echo "   KVER       = $KVER"
echo "   STAGE      = $STAGE"
echo "   OUT        = $OUT"

rm -rf "$STAGE"
mkdir -p "$STAGE"
for d in bin sbin usr/bin usr/sbin usr/share/udhcpc lib etc etc/dropbear \
         dev proc sys run tmp mnt new_root root "lib/modules/$KVER"; do
	mkdir -p "$STAGE/$d"
done

# ---- busybox + musl（全部取自 OpenWrt 已构建产物）--------------------------
# busybox 优先取 busybox 编译目录（最新，含 telnetd 等本板需要的 applet），
# 取不到再退回 TARGET_DIR（整包安装后的副本）
BB_SRC=""
for _c in "$TOPDIR"/build_dir/target-*/busybox-default/busybox-*/busybox; do
	if [ -x "$_c" ]; then BB_SRC=$_c; break; fi
done
[ -n "$BB_SRC" ] || BB_SRC="$TARGET_DIR/bin/busybox"
[ -x "$BB_SRC" ] || { echo "找不到 busybox" >&2; exit 1; }
echo "   busybox  = $BB_SRC"
cp -a "$BB_SRC"                      "$STAGE/bin/busybox"
cp -a "$TARGET_DIR/lib/libc.so"      "$STAGE/lib/libc.so"
# 开 telnetd 后 busybox 需要 libgcc 的软浮点辅助例程（__floatsidf/__nedf2/__bswapsi2）
cp -a "$TARGET_DIR/lib/libgcc_s.so.1" "$STAGE/lib/libgcc_s.so.1"
ln -sf libc.so "$STAGE/lib/ld-musl-mipsel-sf.so.1"
cp -a "$TARGET_DIR/usr/share/udhcpc/default.script" "$STAGE/usr/share/udhcpc/default.script"
chmod 0755 "$STAGE/usr/share/udhcpc/default.script"

# ---- 以太网驱动模块（**救援/网络安装模式的命门**）--------------------------
# econet-eth 在 OpenWrt 里是 KernelPackage（模块），不在内核内建里。
# initramfs 没有 /lib/modules → 没有 eth0 → 救援模式无网络（HANDOFF-20261108
# §4.2 的"三缺"之外的第 4 缺）。这里把 .ko 单独打进来，/init 的
# load_eth_module() 在起网前 insmod 它。
MOD_SRC=""
for _c in "$TARGET_DIR/lib/modules/$KVER/econet-eth.ko" \
          "$TOPDIR"/build_dir/target-*/linux-*/econet-eth*/econet-eth.ko; do
	[ -f "$_c" ] && { MOD_SRC=$_c; break; }
done
if [ -n "$MOD_SRC" ]; then
	echo "   eth mod  = $MOD_SRC"
	cp -a "$MOD_SRC" "$STAGE/lib/modules/$KVER/econet-eth.ko"
else
	# 不静默跳过：救援模式会因此没有网络，必须在构建日志里高声说明。
	echo "!! 警告：找不到 econet-eth.ko —— 救援/网络安装模式将没有 eth0。" >&2
	echo "!!       请先构建 kmod-econet-eth（make package/kernel/econet-eth/compile）。" >&2
fi

# ---- applet 软链 ----------------------------------------------------------
# 集合以 busybox 的实际编译配置为准（build_dir 的 .config）：wget/tftp/insmod
# 这几个是 isntall 模式必需的，靠 Config-defaults.in + .config 两处同时开。
for a in ash awk basename cat chmod chown clear cmp cp cut date dd df dirname dmesg \
         du echo env expr false find free grep gunzip gzip head hostname id insmod \
         kill killall less ln login logger ls lsmod md5sum mkdir mknod mv nice \
         nslookup passwd ping ping6 ps pwd readlink reboot rm rmdir sed seq setsid \
         sha256sum sh sleep sort sync tail tar tee telnetd test tftp touch tr true \
         umount uniq vi wc wget which xargs yes zcat; do
	ln -sf busybox "$STAGE/bin/$a"
done
for a in halt hwclock ifconfig ip mount poweroff reboot route swapon switch_root \
         telnetd udhcpc umount; do
	ln -sf ../bin/busybox "$STAGE/sbin/$a"
done
# 注意：不建 /sbin/init —— 引导内核的 /init 就是我们的探测脚本

# ---- /init（探测脚本）-----------------------------------------------------
install -m 0755 "$INIT_SRC" "$STAGE/init"

# ---- /etc -----------------------------------------------------------------
# 救援口令：root/<RESCUE_PW>（dropbear 拒绝空口令，见 svr-authpasswd.c）
HASH=$(printf '%s\n' "$RESCUE_PW" | openssl passwd -6 -salt "$RESCUE_SALT" -stdin 2>/dev/null) || HASH=""
[ -n "$HASH" ] || { echo "openssl passwd -6 失败，无法生成口令散列" >&2; exit 1; }

cat >"$STAGE/etc/passwd" <<EOF
root:$HASH:0:0:GS2107 rescue:/root:/bin/ash
nobody:*:65534:65534:nobody:/var:/bin/false
EOF
cat >"$STAGE/etc/shadow" <<EOF
root:$HASH:19000:0:99999:7:::
nobody:*:19000:0:99999:7:::
EOF
printf 'root:x:0:\nnogroup:x:65534:\n' >"$STAGE/etc/group"
printf '/bin/ash\n/bin/sh\n'         >"$STAGE/etc/shells"
printf '127.0.0.1\tlocalhost\n'      >"$STAGE/etc/hosts"
printf 'nameserver 8.8.8.8\n'        >"$STAGE/etc/resolv.conf"
printf 'gs2107\n'                    >"$STAGE/etc/hostname"
printf 'PS1="[gs2107-rescue] \\w # "\n' >"$STAGE/etc/profile"
printf 'tcp/23\ttelnet\n'            >"$STAGE/etc/services"
cat >"$STAGE/etc/banner" <<'EOF'
==============================================================
 GS2107 initramfs rescue shell  (fallback 2)
 U 盘 rootfs 与板载 rootfs 都没起来 —— 你现在跑在内置
 initramfs 里。可用命令： wget / tftp / insmod / ip / udhcpc

 手工起 U 盘系统：
   mount -t squashfs /dev/sda1 /mnt && switch_root /mnt /sbin/init
 或 reboot 重新探测（命令走 bootargs 的 gs_boot=）。
==============================================================
EOF
chmod 0644 "$STAGE"/etc/passwd "$STAGE"/etc/group "$STAGE"/etc/shells \
            "$STAGE"/etc/hosts "$STAGE"/etc/resolv.conf "$STAGE"/etc/hostname \
            "$STAGE"/etc/profile "$STAGE"/etc/services "$STAGE"/etc/banner
chmod 0600 "$STAGE/etc/shadow"

# ---- 救援服务说明 -----------------------------------------------------------
# 网络救援用 busybox telnetd（已随 busybox 内置，无需额外二进制/主机密钥）。
# 未打包 dropbear：dropbear(269KB) + libgcc_s.so.1(131KB) 会把 initramfs
# 撑出 tclinux-hdr2.py 的 --pad 0x500000（5MB）kernel 段预算。
# 若将来抬高该上限，把 dropbear 加回来即可（/init 里已有 telnetd 分支）。

# ---- 目录权限 -------------------------------------------------------------
chmod 0755 "$STAGE" "$STAGE"/bin "$STAGE"/sbin "$STAGE"/usr "$STAGE"/usr/bin \
            "$STAGE"/usr/sbin "$STAGE"/usr/share "$STAGE"/usr/share/udhcpc \
            "$STAGE"/lib "$STAGE"/lib/modules "$STAGE/lib/modules/$KVER" \
            "$STAGE"/etc "$STAGE"/etc/dropbear "$STAGE"/dev \
            "$STAGE"/proc "$STAGE"/sys "$STAGE"/run "$STAGE"/tmp "$STAGE"/mnt \
            "$STAGE"/new_root "$STAGE"/root
chmod 1777 "$STAGE"/tmp
chmod 0700 "$STAGE"/root

# ---- 打 cpio（newc，与内核 initramfs 解包器兼容）-------------------------
# newc 头记着每个条目的修改时间，--reproducible 只归一 inode 与设备号：打包前把修改时间统一成 SOURCE_DATE_EPOCH（tools/openwrt/build.sh 按同一个脚本算好传进来，单独运行时这里现算；与 OpenWrt 顶层一样取主树的提交时间），同一提交、同样的输入重新生成时逐字节相同
: "${SOURCE_DATE_EPOCH:=$("$TOPDIR/scripts/get_source_date_epoch.sh" "$TOPDIR")}"
[ -n "$SOURCE_DATE_EPOCH" ] || { echo "取不到 SOURCE_DATE_EPOCH（$TOPDIR/scripts/get_source_date_epoch.sh）" >&2; exit 1; }
find "$STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
rm -f "$OUT"
( cd "$STAGE" && find . | LC_ALL=C sort | \
	"$CPIO" --reproducible -o -H newc -R 0:0 >"$OUT" )

_sz=$(wc -c <"$OUT")
echo "== 完成 =="
echo "   cpio      = $OUT ($_sz B)"
du -sh "$STAGE" | sed 's/^/   staging   = /'
ls -l "$STAGE/lib/modules/$KVER/" 2>/dev/null | sed 's/^/   module    = /'

# 内核本体（vmlinuz.bin：已 LZMA、不含 initramfs）——体积估算的被加数
KERNEL_BODY=""
for _c in "$TOPDIR"/build_dir/target-*/linux-*/vmlinuz.bin; do
	[ -f "$_c" ] && { KERNEL_BODY=$_c; break; }
done
_kb=""
if [ -n "$KERNEL_BODY" ]; then
	_kb=$(wc -c <"$KERNEL_BODY")
elif [ -n "${KERNEL_BODY_BYTES:-}" ]; then
	_kb=$KERNEL_BODY_BYTES
else
	# 不猜魔法数：拿不到内核本体就没法给出可信估算，"看起来还有余量"的
	# 错误结论比直接说"跳过"危险得多。
	echo "!! 找不到 vmlinuz.bin，无法估算 kernel 段体积（跳过门禁）" >&2
	echo "!! 先 make target/linux/compile；或传 KERNEL_BODY_BYTES=<字节数>" >&2
fi

# ---- 体积门禁 -------------------------------------------------------------
# 真正的约束是 **LZMA 后的整个 kernel 段**（内核本体 + 内嵌 initramfs）。
#
# 估算式：kernel 本体尺寸（vmlinuz.bin，已 LZMA）+ 本轮 cpio 的 xz 尺寸，
# 再 +2% 覆盖 xz -9 与内核 LZMA 压缩率的差异。
#
# 为什么必须加"内核本体"：只算 cpio 会得出"余量 4.6MB"的假象（内核本体
# 3.78MB 才是大头）。实测：cpio 1491968B → xz 556116B，内核本体 3780176B，
# 估算 4337348B vs 真正构建出的 4374599B（+0.86%），余量 847.7KiB。
#
# 注意这是**估算**。权威判定是构建产物本身：跑
#   tools/gs2107-verify-trx.py <trx>
# 它直接读 HDR2 头与真实 payload，并且校验 JAMCRC/loadaddr/padding。
if command -v xz >/dev/null 2>&1 && [ -n "$_kb" ]; then
	_packed=$(xz -9 -T1 -c "$OUT" 2>/dev/null | wc -c)
	_est=$(( _kb + (_packed * 102 + 99) / 100 ))
	_left=$(( MAX_KERNEL - _est ))
	echo "   kernel 本体   = $_kb B（$( [ -n "$KERNEL_BODY" ] && basename "$KERNEL_BODY" || echo 外部传入)）"
	echo "   initramfs xz  = $_packed B  (cpio $_sz B → 压缩率 $(awk -v a="$_sz" -v b="$_packed" 'BEGIN{printf "%.1f%%", b*100/a}'))"
	echo "   kernel 段估算 = $_est B（含 2% 压缩率余量）"
	if [ "$_left" -lt 0 ]; then
		echo "!! 体积超限：估算 ${_est}B > 上限 ${MAX_KERNEL}B（超出 $((-_left))B）" >&2
		echo "!! 处置：砍 applet / 去掉 tftp / 提高 image/Makefile 的 --pad" >&2
		exit 1
	fi
	echo "   余量          = $_left B（上限 $MAX_KERNEL B = pad ${PAD_OFFSET} − 0x100 头）"
	echo "   利用率        = $(awk -v a="$_est" -v b="$MAX_KERNEL" 'BEGIN{printf "%.1f%%", a*100/b}')"
	echo "   权威判定请跑  = tools/gs2107-verify-trx.py <trx>（读真实 HDR2 头）"
else
	echo "   (跳过体积估算：无 xz 或未拿到内核本体尺寸；上限 $MAX_KERNEL B)"
fi

echo "   把这一行写进 .config（在主树里的写相对 TOPDIR 的路径）："
echo "   CONFIG_EXTERNAL_CPIO=\"${OUT#"$TOPDIR"/}\""

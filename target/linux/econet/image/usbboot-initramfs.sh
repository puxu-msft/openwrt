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
#      OUT          输出 cpio 路径（= CONFIG_EXTERNAL_CPIO 要填的值）
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
MAX_KERNEL=${MAX_KERNEL:-5242624}

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
rm -f "$OUT"
( cd "$STAGE" && find . | LC_ALL=C sort | \
	"$CPIO" --reproducible -o -H newc -R 0:0 >"$OUT" )

_sz=$(wc -c <"$OUT")
echo "== 完成 =="
echo "   cpio      = $OUT ($_sz B)"
du -sh "$STAGE" | sed 's/^/   staging   = /'
ls -l "$STAGE/lib/modules/$KVER/" 2>/dev/null | sed 's/^/   module    = /'

# ---- 体积门禁 -------------------------------------------------------------
# 真正的约束是 **LZMA 后的整个 kernel 段**（内核本体 + 内嵌 initramfs），
# 必须 < MAX_KERNEL。
#
# 两种量法，优先用精确的：
#   ① 已有 vmlinuz.bin-initramfs（上次构建产物）= 上一轮真实的 LZMA 段大小。
#      注意它**不含**本轮对 cpio 的改动，所以还要加上"本轮 cpio 压缩后
#      与上轮 cpio 压缩后的差"。上轮 cpio 无法追溯时退而用 ②。
#   ② vmlinuz.bin（内核本体，不含 initramfs）+ 本轮 cpio 的 xz 尺寸。
#      实测偏差约 +1%（xz -9 的压缩率比内核 LZMA 略好），故加 2% 余量。
#
# 实测参考（2026-11-08，含 wget/tftp/insmod + econet-eth.ko）：
#   vmlinuz.bin = 3780176，cpio = 1491968 → xz 556116
#   估算 4337348 vs 实际 4374599（+0.86%）；实际余量 847.7 KiB（利用率 83.4%）
KERNEL_BODY=""
for _c in "$TOPDIR"/build_dir/target-*/linux-*/vmlinuz.bin; do
	[ -f "$_c" ] && { KERNEL_BODY=$_c; break; }
done
if [ -n "$KERNEL_BODY" ]; then
	_kb=$(wc -c <"$KERNEL_BODY")
else
	_kb=3900000
	echo "   (未找到 vmlinuz.bin，内核本体按 ${_kb}B 保守估)"
fi
if command -v xz >/dev/null 2>&1; then
	_packed=$(xz -9 -T1 -c "$OUT" 2>/dev/null | wc -c)
	# +2% 覆盖 xz -9 与内核 LZMA 的压缩率差异（实测偏差约 1%）
	_est=$(( _kb + (_packed * 102 + 99) / 100 ))
	_left=$(( MAX_KERNEL - _est ))
	echo "   kernel 本体   = $_kb B（$( [ -n "$KERNEL_BODY" ] && basename "$KERNEL_BODY" || echo 保守估）)"
	echo "   initramfs xz  = $_packed B  (cpio $_sz B → 压缩率 $(awk -v a="$_sz" -v b="$_packed" 'BEGIN{printf "%.1f%%", b*100/a}'))"
	echo "   kernel 段估算 = $_est B（含 2% 压缩率余量）"
	if [ "$_left" -lt 0 ]; then
		echo "!! 体积超限：估算 ${_est}B > 上限 ${MAX_KERNEL}B（超出 $((-_left))B）" >&2
		echo "!! 处置：砍 applet / 去掉 tftp / 提高 image/Makefile 的 --pad" >&2
		exit 1
	fi
	echo "   余量          = $_left B（上限 $MAX_KERNEL B = 0x500000 pad − 0x100 头）"
	echo "   利用率        = $(awk -v a="$_est" -v b="$MAX_KERNEL" 'BEGIN{printf "%.1f%%", a*100/b}')"
else
	echo "   (无 xz，跳过体积估算；上限 $MAX_KERNEL B)"
fi

echo "   把这一行写进 .config："
echo "   CONFIG_EXTERNAL_CPIO=\"$OUT\""

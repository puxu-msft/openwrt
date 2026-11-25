# chinamobile,gs2107：内核 FIT 在裸分区 kernel（U-Boot 直接读），squashfs 根与 overlay 是 UBI 设备 ubi_rootfs（mtd-concat 拼成）上的卷 rootfs、rootfs_data，按上游 nand.sh 升级、按上游路径保留配置。
# 镜像必须带元数据：REQUIRE_IMAGE_METADATA 让没有元数据的镜像判为无效（fwtool.sh 的 fwtool_check_image，-F 能越过），gs2107_check_image 再把它判为不可强刷（74），旧布局（裸 mtd4 根）的私有 tar 都没有元数据；反过来，新镜像的 compat_version 2.0 让仍是旧布局（设备侧 compat_version 1.0）的设备拒绝它。
case "$(board_name)" in
chinamobile,gs2107)
	REQUIRE_IMAGE_METADATA=1
	;;
esac

# stage2 在停掉服务之后、切进 ramfs 执行 do_stage2 之前调用这里。stage2 的输出原本只进 procd 的 /dev/console（串口），本机常态没有串口：把输出改写进内核日志，控制台级别放到 8（/dev/kmsg 不带前缀的写入是级别 4，原来的 conloglevel=4 会把它滤掉）、关掉 /dev/kmsg 的写入限速，pstore 的 console 记录才收得全；之后 exec 的 do_stage2 继承这两个描述符。热复位后的第一次启动读 /sys/fs/pstore/console-ramoops-0。
platform_pre_upgrade() {
	case "$(board_name)" in
	chinamobile,gs2107)
		echo on > /proc/sys/kernel/printk_devkmsg
		echo 8 > /proc/sys/kernel/printk
		exec >/dev/kmsg 2>&1
		gs2107_rescue_pre_upgrade
		;;
	esac
}

# 在自救系统里升级（只经网络载入，内核命令行带 gs2107.recovery=1）时，这里还在完整的根文件系统里，切 ramfs 之前做两件事：
# 一、丢掉自救系统自己的配置包：/tmp/sysupgrade.tgz 若是自救系统的配置（93_gs2107-recovery-mode 加的 WAN SSH 规则 gs2107_recovery_ssh 在 etc/config/firewall 里，同包还有 hostname gs2107-recovery 与口令登录），nand.sh 会把它放进新的 rootfs_data，主系统第一次起来就带着这些。自救系统的根是 tmpfs，sysupgrade 本来不为它存配置包（rootfs_type 为 tmpfs 时 create_backup_archive 不存），这里防的是经 -f 误传进来的这种包；-f 传进来的主系统配置不受影响。
# 二、把 env 的 recovery 清成 0、清掉启动计数（同 gs2107-recovery clear）：否则写完重启又进网络救援模式。写到一半断电时 kernel 已失效，U-Boot 的 gs_nand_boot 起不来，gs_boot 接着就走网络救援模式，不靠 recovery=1。
gs2107_rescue_pre_upgrade() {
	local conf=/tmp/sysupgrade.tgz

	. /lib/functions/gs2107.sh
	gs2107_recovery_mode || return 0
	if [ -f "$conf" ] && tar xzOf "$conf" etc/config/firewall 2>/dev/null | grep -q gs2107_recovery_ssh; then
		rm -f "$conf"
		echo "GS2107: dropped $conf, it is the rescue system's own configuration; pass the main system's backup with sysupgrade -f to keep it"
	fi
	gs2107_bootcount_clear sysupgrade recovery=0 ||
		echo "GS2107: recovery or the boot counter not cleared; the next boot may go to network rescue mode first"
}

# gs2107 只按两种镜像升级：sysupgrade.bin（tar，kernel 写裸分区 kernel、root 写 UBI 卷 rootfs，出自同一次构建）随时可用；factory-ubi.bin（整个 ubi_rootfs 的 ubinize 镜像，不含内核）只在自救系统里用（设备 docs/33 §4.3；自救系统只经网络载入，板载闪存上没有自救镜像）。
# nand.sh 另认的两种对本板有破坏性，返回 74（validate_firmware_image 判为不可强刷，-F 也不放行）：单独的 FIT 会被 nand_upgrade_fit 写成 UBI 卷 kernel（U-Boot 读的是裸分区）并删掉卷 rootfs、rootfs_data；单独的 UBIFS 会被写成卷 rootfs，根不再是 squashfs。
# factory-ubi.bin 在主系统上返回 1：它只重写 ubi_rootfs、不碰 kernel 分区，内核与根就不再配套；自救模式的判定在 lib/functions/gs2107.sh，stage1 的 validate_firmware_image 不 source 它，这里自己 source。
# 没有元数据的镜像（旧布局的私有 tar、单独的 FIT）与缺 kernel 或 ubi_rootfs 分区的系统（布局还没迁移、或设备树不对）都返回 74：前者刷进去是带 root=/dev/mtdblock4 的旧内核，新布局下挂根失败；后者 nand.sh 找不到 ubi_rootfs 会静默改用 CI_UBIPART=rootfs，而 kernel 分区在那之前已被擦掉。
gs2107_check_image() {
	local type part

	fwtool -q -i /dev/null "$1" || {
		echo "GS2107: the image has no fwtool metadata (an old-layout private tar or a bare FIT); upgrade with the sysupgrade.bin built for the UBI layout"
		return 74
	}
	for part in kernel ubi_rootfs; do
		[ -n "$(find_mtd_index "$part")" ] || {
			echo "GS2107: this system has no MTD partition named $part; the layout is not the UBI one (migrate it first, devices/openwrt/gs2107 docs/33 §5)"
			return 74
		}
	done
	type="$(identify "$1" "$(identify_if_gzip "$1")cat" "")"
	case "$type" in
	fit|ubifs)
		echo "GS2107: a bare $type image would replace the UBI volumes rootfs and rootfs_data; upgrade with the sysupgrade.bin of the same build, or write only the kernel partition with the host tool gs2107-install-fit.sh --target kernel"
		return 74
		;;
	ubi)
		. /lib/functions/gs2107.sh
		gs2107_recovery_mode || {
			echo "GS2107: factory-ubi.bin rewrites only ubi_rootfs and leaves the kernel partition alone; use it from the rescue system after writing the kernel of the same build with gs2107-install-fit.sh --target kernel, or upgrade with sysupgrade.bin"
			return 1
		}
		;;
	esac

	nand_do_platform_check chinamobile,gs2107 "$1"
}

# 镜像里 kernel 的长度：nand_upgrade_tar 写 kernel 分区的条件就是它不为 0；factory-ubi.bin 不是 tar，输出 0。
gs2107_tar_kernel_length() {
	local cmd="$(identify_if_gzip "$1")cat"
	local dir

	[ "$(identify "$1" "$cmd" "")" = ubi ] && { echo 0; return; }
	dir="$($cmd < "$1" | tar tf - | grep -m 1 '^sysupgrade-.*/$')"
	($cmd < "$1" | tar xOf - "${dir}kernel" | wc -c) 2>/dev/null
}

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	chinamobile,gs2107)
		gs2107_check_image "$1"
		return $?
		;;
	chinamobile,gs3101|\
	dasan,h660gm-a-airtel|\
	dasan,h660gm-a-generic|\
	jiofiber,jcow407|\
	jiofiber,jcow414)
		return 0
		;;
	esac

	return 1
}

platform_do_upgrade() {
	local board=$(board_name)

	case "$board" in
	chinamobile,gs2107)
		# fwtool 元数据尾巴会让 ubiformat 判 factory-ubi.bin 的长度不是擦除块的整数倍，先去掉；sysupgrade.bin 的 tar 去掉尾巴也无妨。上游 sophos,apx120 同法。
		fwtool -q -t -i /dev/null "$1" || true
		# nand.sh 用 mtd write 写 kernel，只擦到新镜像的末尾：新内核比旧的短时旧内核的尾巴留在分区里，整区读回（部署核对、box-health 的周巡读）就对不上。镜像带 kernel 时先整区擦掉 kernel 分区（原来的私有部署用 mtd -e kernel，同样整区擦）。nand.sh 随后本来就先清 kernel 头 4 KiB，这里只是把 kernel 失效提前几步：掉电时 U-Boot 的 gs_nand_boot 起不来，bootcmd 每 5 s 重跑一遍启动顺序，每一轮都有网络探测（链路起来后约 3 s，主机先起好服务就能送自救 FIT 进内存）。factory-ubi.bin 不含内核，不碰 kernel 分区。
		if [ "$(gs2107_tar_kernel_length "$1")" -gt 0 ] 2>/dev/null; then
			mtd erase kernel || echo "GS2107: erasing the kernel partition failed, nand.sh writes the kernel anyway"
		fi
		CI_UBIPART="ubi_rootfs"
		nand_do_upgrade "$1"
		;;
	chinamobile,gs3101|\
	dasan,h660gm-a-airtel|\
	dasan,h660gm-a-generic|\
	jiofiber,jcow407|\
	jiofiber,jcow414)
		CI_KERNPART="tclinux_kernel"
		nand_do_upgrade "$1"
		;;
	esac
}

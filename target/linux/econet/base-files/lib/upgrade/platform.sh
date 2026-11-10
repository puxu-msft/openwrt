gs2107_require_uboot() {
	[ -s /proc/device-tree/chosen/u-boot,version ] || {
		v "GS2107 sysupgrade requires the U-Boot single-slot chain; use tcboot/UART recovery"
		return 1
	}
}

gs2107_mtd_size() {
	local part="$1"
	local hex
	hex=$(awk -v p="$part" '{ gsub(/"/, "", $4); if ($4 == p) print $2 }' /proc/mtd)
	[ -n "$hex" ] || return 1
	echo $((0x$hex))
}

gs2107_image_dir() {
	tar tf "$1" 2>/dev/null | sed -n 's|^\(sysupgrade-[^/]*\)/kernel$|\1|p' | head -1
}

gs2107_check_image() {
	local tar="$1"
	local dir kmagic rmagic ksize rsize kpart rpart

	gs2107_require_uboot || return 1
	dir=$(gs2107_image_dir "$tar")
	[ -n "$dir" ] || { v "GS2107 image: missing sysupgrade-*/kernel"; return 1; }
	tar tf "$tar" | grep -q "^$dir/root$" || {
		v "GS2107 image: missing $dir/root"
		return 1
	}
	[ "$(tar xOf "$tar" "$dir/CONTROL" 2>/dev/null | sed -n 's/^BOARD=//p')" = "chinamobile,gs2107" ] || {
		v "GS2107 image: CONTROL board mismatch"
		return 1
	}

	kmagic=$(tar xOf "$tar" "$dir/kernel" | dd bs=4 count=1 2>/dev/null | hexdump -v -n 4 -e '1/1 "%02x"')
	[ "$kmagic" = "d00dfeed" ] || {
		v "GS2107 image: kernel is not a FIT ($kmagic)"
		return 1
	}
	rmagic=$(tar xOf "$tar" "$dir/root" | dd bs=4 count=1 2>/dev/null | hexdump -v -n 4 -e '1/1 "%02x"')
	case "$rmagic" in
	68737173|73717368) ;;
	*)
		v "GS2107 image: root is not squashfs ($rmagic)"
		return 1
		;;
	esac

	ksize=$(tar xOf "$tar" "$dir/kernel" | wc -c)
	rsize=$(tar xOf "$tar" "$dir/root" | wc -c)
	kpart=$(gs2107_mtd_size kernel) || { v "GS2107 image: kernel partition not found"; return 1; }
	rpart=$(gs2107_mtd_size rootfs) || { v "GS2107 image: rootfs partition not found"; return 1; }
	[ "$ksize" -le "$kpart" ] || {
		v "GS2107 image: kernel $ksize > partition $kpart"
		return 1
	}
	[ "$rsize" -le "$rpart" ] || {
		v "GS2107 image: rootfs $rsize > partition $rpart"
		return 1
	}
	return 0
}

gs2107_do_upgrade() {
	local tar="$1"
	local dir tmp=/tmp/sysupgrade-gs2107

	gs2107_require_uboot || return 1
	dir=$(gs2107_image_dir "$tar")
	[ -n "$dir" ] || return 1
	rm -rf "$tmp"
	mkdir -p "$tmp"
	tar xf "$tar" -C "$tmp" "$dir/kernel" "$dir/root" || return 1

	v "GS2107: writing kernel FIT to mtd3 (kernel)"
	mtd -e kernel write "$tmp/$dir/kernel" kernel || return 1
	v "GS2107: writing rootfs to mtd4 (rootfs)"
	mtd -e rootfs write "$tmp/$dir/root" rootfs || return 1
	sync
	rm -rf "$tmp"
}

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	chinamobile,gs2107)
		gs2107_check_image "$1"
		return $?
		;;
	esac

	case "$board" in
	chinamobile,gs2101|\
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
		gs2107_do_upgrade "$1"
		return $?
		;;
	esac

	case "$board" in
	chinamobile,gs2101|\
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

# ZX279133 板子上本系统从内存启动、板载闪存里仍是原厂固件，还没有可升级的闪存布局，sysupgrade 一律拒绝。
platform_check_image() {
	echo "Sysupgrade is not supported on this board yet." >&2
	return 1
}

platform_do_upgrade() {
	return 1
}

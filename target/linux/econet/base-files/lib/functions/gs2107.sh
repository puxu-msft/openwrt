# GS2107 板级共用函数：自救模式判定、U-Boot env 的安全写入、SSH 会话判定。由 gs2107-bootok、gs2107-recovery、gs2107-recovery-return、uci-defaults/93_gs2107-recovery-mode 与 hotplug.d/ieee80211/90_gs2107-recovery-wifi source。
# 调用方先 source /lib/functions.sh（用到其中的 find_mtd_index；rc.common 与 hotplug-call 已经 source 过）。
# GS2107_CMDLINE、GS2107_FW_ENV_CONFIG、GS2107_PROC、GS2107_RUN 只供主机侧离线测试替换路径，设备上不设。

# 自救镜像（mtd8，U-Boot 的 gs_rescue_boot）启动时内核命令行带 gs2107.recovery=1，主系统（gs_nand_boot）不带。自救镜像与主系统共用同一份 rootfs，自救模式的行为都以此为开关。
# 还要求根文件系统在内存里（initramfs 经 /init switch_root 到 tmpfs）：ramdisk 没被内核用上时，内核会照命令行挂 NAND 上的 rootfs，带着 gs2107.recovery=1 进主系统，这时根是 overlay，不能把自救模式的配置写进持久 overlay。
gs2107_recovery_mode() {
	local cmdline="" rootfs=""

	{ read -r cmdline < "${GS2107_CMDLINE:-/proc/cmdline}"; } 2>/dev/null
	case " $cmdline " in
	*" gs2107.recovery=1 "*) ;;
	*) return 1 ;;
	esac
	# /proc/mounts 里 "/" 可能出现多次（rootfs 之上再挂 tmpfs），取最后一条
	rootfs="$(awk '$2 == "/" { t = $3 } END { print t }' "${GS2107_PROC:-/proc}/mounts" 2>/dev/null)"
	case "$rootfs" in
	tmpfs|rootfs|ramfs) return 0 ;;
	esac
	return 1
}

# 打印内核命令行里 $1=<值> 的值（同一项出现多次时取最后一个）；没有这一项时返回 1。
gs2107_cmdline_value() {
	local cmdline=

	{ read -r cmdline < "${GS2107_CMDLINE:-/proc/cmdline}"; } 2>/dev/null
	cmdline=" $cmdline "
	case "$cmdline" in
	*" $1="*) ;;
	*) return 1 ;;
	esac
	cmdline="${cmdline##*" $1="}"
	printf '%s\n' "${cmdline%%" "*}"
}

# 让 fw_env.config 指向 uboot_env 分区：按标签找分区，mtd 编号变了也能跟上；内容一致时不重写。
gs2107_env_config() {
	local config="${GS2107_FW_ENV_CONFIG:-/etc/fw_env.config}" idx want

	idx="$(find_mtd_index uboot_env)"
	[ -n "$idx" ] || return 1
	want="/dev/mtd$idx 0x0 0x20000 0x20000
/dev/mtd$idx 0x20000 0x20000 0x20000"
	# 逐字段比较：uboot-envtools 写出的行尾带空白
	[ "$(awk '{$1=$1; print}' "$config" 2>/dev/null)" = "$want" ] && return 0
	printf '%s\n' "$want" > "$config.new" && mv "$config.new" "$config"
}

# 把 U-Boot env 设成给定的值。$1 是 logger 标签，其余参数是 名=值。不需要写或写成功返回 0；分区找不到、身份门不通过或 fw_setenv 失败返回 1，都已记日志。
# 身份门：只有 env 读回来是 GS2107 的 env（有 gs_boot、没有 Bad CRC）才写。CRC 坏或分区不对时，fw_setenv 会存进它内置的默认 env（bootcmd=run distro_bootcmd），板子就起不来了。
# 每次 fw_setenv 都整份重写一份 env 副本（擦一个 128 KiB NAND 块），所以只写与现值不同的项，并合成一次 fw_setenv -s - 调用。
gs2107_env_set() {
	local tag="$1" kv changes=""

	shift
	if ! gs2107_env_config; then
		logger -t "$tag" "uboot_env partition not found; not writing"
		return 1
	fi
	if ! fw_printenv -n gs_boot >/dev/null 2>&1 ||
	   fw_printenv -n bootcount 2>&1 | grep -q "Bad CRC"; then
		logger -t "$tag" "U-Boot env does not read back as the GS2107 env; not writing"
		return 1
	fi

	for kv in "$@"; do
		[ "$(fw_printenv -n "${kv%%=*}" 2>/dev/null)" = "${kv#*=}" ] ||
			changes="$changes${kv%%=*} ${kv#*=}
"
	done
	[ -n "$changes" ] || return 0

	if printf '%s' "$changes" | fw_setenv -s -; then
		logger -t "$tag" "set $(printf '%s' "$changes" | tr '\n' ' ')"
	else
		logger -t "$tag" "fw_setenv failed"
		return 1
	fi
}

# 有没有已登录并在用的 SSH 会话。依据 dropbear 2026.94 源码：
# - 监听进程由 procd 拉起，用 -P 把自己的 PID 写进 /var/run/dropbear.<段名>.pid（src/svr-main.c:206）；每接受一个连接就 fork 一个会话进程（svr-main.c:313），会话进程 fexecve 重新执行 dropbear 自身（svr-main.c:383，argv[0] 不变，busybox pidof 按 argv0 仍把它认作 dropbear）。
# - 认证完成前收到频道类报文，会话进程直接 dropbear_exit 断开连接（src/process-packet.c:139-140），所以 shell/命令只能在认证之后启动：打开 shell 或命令通道时，会话进程才 fork 出 shell/命令子进程（src/svr-chansession.c 的 noptycommand:771、ptycommand:813）。
# 判据：某个“不是监听进程”的 dropbear 进程有子进程。未认证的连接（含端口扫描）没有子进程，不算。不依赖会话进程的父进程是当前监听进程：dropbear 重载、监听进程重启后，旧会话进程被 init 收养，仍能认出来。
# 只开端口转发（ssh -N）的会话没有子进程，不算；这样长时间使用时先 gs2107-recovery hold。
gs2107_ssh_session_active() {
	local proc="${GS2107_PROC:-/proc}" listeners sessions="" pid st line

	listeners=" $(cat "${GS2107_RUN:-/var/run}"/dropbear.*.pid 2>/dev/null | tr '\n' ' ') "
	for pid in $(pidof dropbear); do
		case "$listeners" in
		*" $pid "*) ;;
		*) sessions="$sessions $pid" ;;
		esac
	done
	# 只有监听进程时不必扫进程表：空闲时每次检查只花一次 pidof
	[ -n "$sessions" ] || return 1
	for st in "$proc"/[0-9]*/stat; do
		{ read -r line < "$st"; } 2>/dev/null || continue
		pid="${st%/stat}"
		# 第 2 个字段 (comm) 可能含空格与括号，从最后一个 ") " 之后取：state ppid …
		line="${line##*) }"
		line="${line#* }"
		echo "${pid##*/} ${line%% *}"
	done | awk -v sessions="$sessions " '
		index(sessions, " " $2 " ") { found = 1 }
		END { exit !found }'
}

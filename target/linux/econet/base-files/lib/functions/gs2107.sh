# GS2107 板级共用函数：自救模式判定、U-Boot env 的安全写入、U-Boot 启动计数清零、SSH 会话判定。由 gs2107-bootok、gs2107-recovery、gs2107-recovery-return、uci-defaults/93_gs2107-recovery-mode 与 hotplug.d/ieee80211/90_gs2107-recovery-wifi source。
# 调用方先 source /lib/functions.sh（用到其中的 find_mtd_index；rc.common 与 hotplug-call 已经 source 过）。
# GS2107_CMDLINE、GS2107_FW_ENV_CONFIG、GS2107_KMSG、GS2107_PROC、GS2107_RUN、GS2107_SYS 只供主机侧离线测试替换路径（devices/gs2107/tools/openwrt/check_bootcount_clear.sh），设备上不设。

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

# 把失败记成 user.err 写进内核日志（$1 标签，$2 消息）：logd 会收进 logread，控制台记录又进 pstore，热复位后的下一次启动仍读得到。用于 env 与启动计数写不进去的情形：例如 mtd5 的某一块磨坏后，那份副本永远存不进去，U-Boot 与 fw_setenv 都只往那一份写，env 就冻结在另一份上，要看得见。
gs2107_log_err() {
	printf '<11>%s: %s\n' "$1" "$2" 2>/dev/null > "${GS2107_KMSG:-/dev/kmsg}" || logger -p user.err -t "$1" "$2"
}

# 把 U-Boot env 设成给定的值。$1 是 logger 标签，其余参数是 名=值。不需要写或写成功返回 0；分区找不到、身份门不通过或 fw_setenv 失败返回 1，都已记日志。
# 身份门：只有 env 读回来是 GS2107 的 env（有 gs_boot、没有 Bad CRC）才写。CRC 坏或分区不对时，fw_setenv 会存进它内置的默认 env（bootcmd=run distro_bootcmd），板子就起不来了。
# 每次 fw_setenv 都整份重写一份 env 副本（擦一个 128 KiB NAND 块），所以只写与现值不同的项，并合成一次 fw_setenv -s - 调用。
gs2107_env_set() {
	local tag="$1" kv changes=""

	shift
	if ! gs2107_env_config; then
		gs2107_log_err "$tag" "uboot_env partition not found; not writing"
		return 1
	fi
	if ! fw_printenv -n gs_boot >/dev/null 2>&1 ||
	   fw_printenv -n bootcount 2>&1 | grep -q "Bad CRC"; then
		gs2107_log_err "$tag" "U-Boot env does not read back as the GS2107 env; not writing"
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
		gs2107_log_err "$tag" "fw_setenv failed"
		return 1
	fi
}

# U-Boot 把启动计数放在保留内存时（BOOTCOUNT_GENERIC，DTS 的 bootcount 节点），由 uboot-bootcount 驱动读写：打印它的 sysfs 属性 count 的路径；驱动没加载或设备树没有这个节点时返回 1。
gs2107_bootcount_ram() {
	local f

	for f in "${GS2107_SYS:-/sys}"/bus/platform/drivers/uboot-bootcount/*/count; do
		[ -f "$f" ] || continue
		echo "$f"
		return 0
	done
	return 1
}

# 清零 U-Boot 的启动计数。$1 是日志标签，其余参数是要一并写进 env 的 名=值（经 gs2107_env_set 合成同一次 fw_setenv）。都写好或不需要写返回 0；任何一处写不进去、或没有内存接口时返回 1，都已记日志。
# 计数在哪取决于 mtd2 上的 U-Boot：新 U-Boot（BOOTCOUNT_GENERIC）在保留内存，旧 U-Boot（BOOTCOUNT_ENV）在 env 的 bootcount。镜像要同时兼容两者（先部署 Linux 镜像、再写 mtd2）：env 的 bootcount 只在不是 0 时清——旧 U-Boot 每次启动存进 N≥1；新 U-Boot 不从 env 读计数，平常也不保存 env，这一项不再变化（只有 gs_fallback 保存 env 时会顺带存进当时内存里的值，之后由这里清一次），不为它擦写 NAND。内存里的计数不是 0 就清（旧 U-Boot 下魔数无效读作 0，不写）。
# 先写 env 再清内存：自救模式下 env 写不进去时回不了主系统（recovery 仍是 1），内存计数也留着，自救系统反复崩溃时照样累加到 bootlimit、轮回主系统；主系统下 env 写不进去也照清内存计数，免得新 U-Boot 下 env 坏了就每 bootlimit 次热复位进一次自救镜像。
# 本镜像的设备树与包里都带着内存接口，接口不在说明驱动没加载或镜像不配套：新 U-Boot 下计数清不掉，所以记 err（旧 U-Boot 下无害，env 照常清）。
gs2107_bootcount_clear() {
	local tag="$1" ram count ret=0

	shift
	# 读 env 之前先让 fw_env.config 指对分区；找不到分区时这里读不到，交给 gs2107_env_set 报
	gs2107_env_config 2>/dev/null
	case "$(fw_printenv -n bootcount 2>/dev/null)" in
	''|0) ;;
	*) set -- bootcount=0 "$@" ;;
	esac
	if [ $# -gt 0 ] && ! gs2107_env_set "$tag" "$@"; then
		gs2107_recovery_mode && return 1
		ret=1
	fi

	if ! ram="$(gs2107_bootcount_ram)"; then
		gs2107_log_err "$tag" "no RAM boot counter (uboot-bootcount not bound); only the U-Boot env one was handled"
		return 1
	fi
	count="$(cat "$ram" 2>/dev/null)"
	[ "$count" != 0 ] || return $ret
	if echo 0 2>/dev/null > "$ram"; then
		logger -t "$tag" "boot counter in RAM: ${count:-unreadable} -> 0"
	else
		gs2107_log_err "$tag" "could not clear the boot counter in RAM ($ram)"
		ret=1
	fi
	return $ret
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

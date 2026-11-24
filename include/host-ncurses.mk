# SPDX-License-Identifier: GPL-2.0-only
#
# 宿主 ncurses 的来源，只作用于两处：prereq 的 ncurses 检查（include/prereq-build.mk）与 kconfig 的 ncurses 前端 mconf、nconf 的宿主编译（include/toplevel.mk）；交叉编译与别的主机构建看不到这些路径。
# HOST_NCURSES_PREFIX 下同时有 include/ncurses.h、lib/libncurses.so（prereq 链接 -lncurses）与 lib/pkgconfig/ncursesw.pc 时用这一份：prereq 的测试程序带它的 -I/-L；mconf、nconf 经 PKG_CONFIG_PATH 先找到它的 .pc，链接时加指向它 lib 的 RUNPATH，运行期不落到系统的同名库。
# 缺省是 linuxbrew 的 opt 路径（brew 升级 ncurses 后不变）；可在环境或 make 命令行里改，相对路径按 TOPDIR 解析。指向没有 ncurses 的目录或置空时回落：下面的变量都为空，按编译器与 pkg-config 的默认搜索路径找（上游原来的行为）。
# 换前缀、回落或 brew 升级 ncurses 不会让已编好的 mconf、nconf 与 scripts/config 里缓存的 *conf-cflags、*conf-libs（记的是 .pc 给出的 Cellar 版本路径）自动更新，要生效先 make config-clean。

HOST_NCURSES_PREFIX ?= /home/linuxbrew/.linuxbrew/opt/ncurses
host_ncurses_dir:=$(abspath $(HOST_NCURSES_PREFIX))

ifneq ($(and $(host_ncurses_dir),$(wildcard $(host_ncurses_dir)/include/ncurses.h),$(wildcard $(host_ncurses_dir)/lib/libncurses.so),$(wildcard $(host_ncurses_dir)/lib/pkgconfig/ncursesw.pc)),)
  HOST_NCURSES_FLAGS:=-I$(host_ncurses_dir)/include -L$(host_ncurses_dir)/lib
  HOST_NCURSES_PKG_CONFIG_PATH:=$(host_ncurses_dir)/lib/pkgconfig
  HOST_NCURSES_RPATH:=-Wl,-rpath,$(host_ncurses_dir)/lib
else
  HOST_NCURSES_FLAGS:=
  HOST_NCURSES_PKG_CONFIG_PATH:=
  HOST_NCURSES_RPATH:=
endif

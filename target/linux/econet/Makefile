# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2009-2025 OpenWrt.org

include $(TOPDIR)/rules.mk

BOARD:=econet
BOARDNAME:=EcoNet EN75xx MIPS
FEATURES:=dt source-only squashfs nand usb
SUBTARGETS:=en751221 en751627 en7528

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware image for EcoNet EN75xx MIPS based boards.
endef

# include the profiles
include $(INCLUDE_DIR)/target.mk

# ---------------------------------------------------------------------------
# GS2107 "U 盘优先启动" 引导内核（USB-BOOT-PLAN.md §0）
#
# include/kernel-defaults.mk 里的默认值是 target/linux/generic/other-files/init
# （把整棵 rootfs 拷到 /new_root 再 switch_root 到 /sbin/init）——那是给
# "initramfs = 完整 rootfs" 的场景用的。GS2107 的 initramfs 是精简 rootfs，
# /init 必须换成我们的探测脚本（U盘优先 → 板载回落 → 驻留救援）。
#
# 覆盖点放在 target.mk 之后：kernel-defaults.mk 里 INITRAMFS_INIT_FILE 是
# `:=` 简单赋值，后赋值者胜。
#
# 注：本树当前用 CONFIG_EXTERNAL_CPIO 走精简 initramfs（cpio 里已含本脚本），
#     此覆盖是给"改回整棵 TARGET_DIR 内嵌"时的等价生效路径，两处同源不漂移。
# ---------------------------------------------------------------------------
INITRAMFS_INIT_FILE := $(PLATFORM_DIR)/other-files/gs2107-usbboot-init

# nand-utils is used by base-files/sbin/en75_chboot
DEFAULT_PACKAGES += nand-utils \
	kmod-econet-eth \
	kmod-leds-gpio \
	kmod-gpio-button-hotplug

$(eval $(call BuildTarget))

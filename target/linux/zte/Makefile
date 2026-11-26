# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

BOARD:=zte
BOARDNAME:=ZTE ZX279133
SUBTARGETS:=zx279133
FEATURES:=dt gpio ramdisk usb source-only
KERNEL_PATCHVER:=6.18

define Target/Description
	Build images for boards based on the ZTE ZX279133 SoC.
endef

include $(INCLUDE_DIR)/target.mk

$(eval $(call BuildTarget))

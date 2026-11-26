# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

# 第一版只出内存启动件：未压缩的 arm64 Image（内嵌 initramfs）加单独的板级 DTB，交给能分别接收两者的加载方式（从原厂系统 kexec）；不出任何写闪存的镜像。
define Device/Default
  PROFILES = Default $$(DEVICE_NAME)
  KERNEL_NAME := Image
  KERNEL := kernel-bin
  KERNEL_INITRAMFS := kernel-bin
  DEVICE_DTS_DIR := ../dts
  FILESYSTEMS :=
  IMAGES :=
  ARTIFACTS := board.dtb
  ARTIFACT/board.dtb := append-dtb
endef

define Device/zte_f7615mv3
  DEVICE_VENDOR := ZTE
  DEVICE_MODEL := ZXHN F7615MV3
  DEVICE_DTS := zx279133-f7615mv3
  SUPPORTED_DEVICES := zte,f7615mv3
endef
TARGET_DEVICES += zte_f7615mv3

# 框架只在设备有 IMAGES 时定义 <设备>-images，ARTIFACTS 的规则却依赖它；这里的设备不出 IMAGES，补一个空目标
$(foreach device,$(TARGET_DEVICES),$(eval $(device)-images:))

$(eval $(call BuildImage))

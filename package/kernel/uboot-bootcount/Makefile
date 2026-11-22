include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=uboot-bootcount
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/uboot-bootcount
  SUBMENU:=Other modules
  TITLE:=U-Boot boot counter in reserved RAM
  FILES:=$(PKG_BUILD_DIR)/uboot-bootcount.ko
  AUTOLOAD:=$(call AutoProbe,uboot-bootcount)
endef

define KernelPackage/uboot-bootcount/description
  读写 U-Boot 存在保留内存里的启动计数（CONFIG_BOOTCOUNT_GENERIC 的双字格式：计数 + 魔数 0xB001C041，小端）。
  设备树节点 compatible = "u-boot,bootcount-mem"，memory-region 指向带 no-map 的保留内存。
  sysfs 属性 count 读出计数（魔数不对时为 0），写入 N 时存计数 N 与魔数并读回核对。
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,uboot-bootcount))

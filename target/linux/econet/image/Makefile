include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

DEVICE_VARS += TRX_MODEL TRX_LOADADDR

define Target/Description
	Build firmware images for EcoNet MIPS based boards.
endef

# tclinux-trx is the default format used in the SDK
define Build/tclinux-trx
  ./tclinux-trx.sh --kernel $@ --rootfs $(IMAGE_ROOTFS) \
    --version $(VERSION_DIST)-$(REVISION) --endian $(TRX_ENDIAN) \
    $(if $(TRX_MODEL),--model $(TRX_MODEL)) \
    $(if $(TRX_LOADADDR),--loadaddr $(TRX_LOADADDR)) > $@.new
	mv $@.new $@
endef

# Build TRX without rootfs, for use with UBI root
define Build/kernel-trx
  ./tclinux-trx.sh --kernel $@ \
    --version $(VERSION_DIST)-$(REVISION) --endian $(TRX_ENDIAN) \
    $(if $(TRX_MODEL),--model $(TRX_MODEL)) \
    $(if $(TRX_LOADADDR),--loadaddr $(TRX_LOADADDR)) > $@.new
	mv $@.new $@
endef

# If TCSUPPORT_FREE_BOOTBASE is enabled, bootloader will decompress the kernel
# at 0x80002000 instead of 0x80020000.
#
# You can tell it by looking at the bootloader version string
# " at %s version %s free bootbase".
define Build/tclinux-free-bootbase-jump
	@[ "$(KERNEL_LOADADDR)" = 0x80020000 ] || { \
		$(call ERROR_MESSAGE,WARNING: KERNEL_LOADADDR changed); \
		rm -f $@; \
	}
	printf '$(if $(CONFIG_BIG_ENDIAN),\010\000\200\000,\000\200\000\010)' > $@.new
	dd if=/dev/zero bs=122876 count=1 >> $@.new
	cat $@ >> $@.new
	mv $@.new $@
endef

# tclinux bootloader requires LZMA, BUT only provides 7.5MB of space
# to decompress into. So we use vmlinuz and decompress twice.
define Device/Default
  DEVICE_DTS_DIR := ../dts
  BLOCKSIZE := 128k
  PAGESIZE := 2048
  # decompression buffer limit, the size of LZMA-compressed data is read from
  # the trx header
  KERNEL_SIZE := 7480k
  KERNEL_NAME := vmlinuz.bin
  KERNEL_LOADADDR := 0x80020000
  KERNEL := kernel-bin | append-dtb
  UBINIZE_OPTS := -E 5
  TRX_MODEL :=
  TRX_LOADADDR :=
endef

# Kernel in a raw TRX partition, root squashfs in UBI. FACTORY_SIZE must be the
# size of the partition the .trx is written into; that write is linear, so an
# oversized image overruns it into the next partition.
define Device/tclinux-ubi
  KERNEL_SIZE := 4096k
  KERNEL := kernel-bin | append-dtb | lzma | kernel-trx
  KERNEL_INITRAMFS := kernel-bin | append-dtb
  IMAGES := tclinux.trx sysupgrade.bin
  IMAGE/tclinux.trx := append-kernel | pad-to $$$$(KERNEL_SIZE) | append-ubi | \
    check-size $$$$(FACTORY_SIZE)
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
endef

include $(SUBTARGET).mk

$(eval $(call BuildImage))

# GS2107 本地步骤：用 Python 实现 tclinux-trx（HDR2 + CRC-32/JAMCRC），
# 不依赖主机上的 zytrx（OpenWrt 树内没有该工具）。
# 与厂商镜像往返验证字节级一致（2026-11-06，devices/gs2107/tools/hdr2.py 同算法）。
define Build/tclinux-hdr2-trx
	python3 $(TOPDIR)/target/linux/$(BOARD)/image/tclinux-hdr2.py --kernel $@ \
		--rootfs $(IMAGE_ROOTFS) --version $(VERSION_DIST)-$(REVISION) \
		--endian $(TRX_ENDIAN) --loadaddr $(TRX_LOADADDR) \
		$(if $(TRX_MODEL),--model $(TRX_MODEL)) > $@.new
	mv $@.new $@
endef

# GS2107 "U 盘优先启动" 引导内核（USB-BOOT-PLAN.md §0）
# 与 tclinux-hdr2-trx 的差别：
#   1) kernel 取 **initramfs 内核**（$(KDIR)/tmp/$(KERNEL_INITRAMFS_IMAGE)，
#      即 bin/targets/ 下的 *-initramfs-kernel.bin），不是普通内核；
#   2) **不给 --rootfs**：rootfs 段为空，root 由 initramfs /init 在运行期探测；
#   3) --pad 0x500000：该内核内嵌 initramfs 后 LZMA 出来 4288266B，超过上游
#      固定的 4MB 垫目标。这里没有 rootfs 段，padding 只剩 "A-length bug"
#      规避一个用途（tclinux-trx.sh 原注：LZMA 解压器到流尾自停），故放到 5MB；
#      镜像总长 5MB，远小于 mtd4(tclinux) 的 24MB。
# 注意：define 体内的设备变量用**单** $（make 展开一次即到位）；写 $$ 会原样
#       吐给 shell 造成 "syntax error near unexpected token )"。
define Build/tclinux-hdr2-initramfs-trx
	python3 $(TOPDIR)/target/linux/$(BOARD)/image/tclinux-hdr2.py \
		--kernel $(KDIR)/tmp/$(KERNEL_INITRAMFS_IMAGE) \
		--version $(VERSION_DIST)-$(REVISION) \
		--endian $(TRX_ENDIAN) --loadaddr $(TRX_LOADADDR) --pad 0x500000 \
		$(if $(TRX_MODEL),--model $(TRX_MODEL)) > $@.new
	mv $@.new $@
endef

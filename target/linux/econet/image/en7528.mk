TRX_ENDIAN := le

define Device/en7528_generic
  DEVICE_VENDOR := EN7528
  DEVICE_MODEL := Generic
  DEVICE_DTS := en7528_generic
endef
TARGET_DEVICES += en7528_generic

define Device/dasan_h660gm-a
  $(call Device/tclinux-ubi)
  DEVICE_VENDOR := DASAN
  DEVICE_MODEL := H660GM-A
  FACTORY_SIZE := 32m
  TRX_MODEL := Dewberry
  DEVICE_PACKAGES := kmod-usb2 kmod-mt7603 kmod-mt7615e kmod-mt7663-firmware-ap
endef

define Device/dasan_h660gm-a-airtel
  $(Device/dasan_h660gm-a)
  DEVICE_VARIANT := Airtel
  DEVICE_DTS := en7528_dasan_h660gm-a-airtel
endef
TARGET_DEVICES += dasan_h660gm-a-airtel

define Device/dasan_h660gm-a-generic
  $(Device/dasan_h660gm-a)
  DEVICE_VARIANT := Generic
  DEVICE_DTS := en7528_dasan_h660gm-a-generic
endef
TARGET_DEVICES += dasan_h660gm-a-generic

define Device/jiofiber_en7528
  $(call Device/tclinux-ubi)
  DEVICE_VENDOR := JioFiber
  FACTORY_SIZE := 40m
  TRX_LOADADDR := 0x80002000
  KERNEL := kernel-bin | append-dtb | tclinux-free-bootbase-jump | lzma | \
    kernel-trx
  DEVICE_PACKAGES := kmod-usb3 kmod-mt7603 kmod-mt7615e kmod-mt7663-firmware-ap
endef

define Device/jiofiber_jcow414
  $(Device/jiofiber_en7528)
  DEVICE_MODEL := JCOW414
  DEVICE_ALT0_VENDOR := JioFiber
  DEVICE_ALT0_MODEL := JCO4032
  DEVICE_DTS := en7528_jiofiber_jcow414
endef
TARGET_DEVICES += jiofiber_jcow414

define Device/jiofiber_jcow407
  $(Device/jiofiber_en7528)
  DEVICE_MODEL := JCOW407
  DEVICE_ALT0_VENDOR := JioFiber
  DEVICE_ALT0_MODEL := JCOW407-IN
  DEVICE_DTS := en7528_jiofiber_jcow407
endef
TARGET_DEVICES += jiofiber_jcow407

# GS2107（中移杭研智能家庭网关）—— 自维护板级，设备文档在 devices/openwrt/gs2107/（docs/33 是本布局与升级路径的设计）
#  - 启动链：tcboot（free bootbase，解压目标 0x80002000）→ mtd2 里的 U-Boot → kernel 分区的内核 FIT
#  - 根：squashfs 是 UBI 设备 ubi_rootfs（DTS 里 ubi_seg0 + ubi_seg1 拼成的 mtd-concat）的卷 rootfs，overlay 是卷 rootfs_data
#  - WiFi: MT7603E(2.4G) + MT7663E(5G) 均 PCIe
#
# 产物：
#   kernel.bin（KERNEL_INSTALL）—— 内核 FIT（自解压的 vmlinuz.bin 以 compression = none 放入 + DTB，load/entry 0x80020000），写 kernel 分区或经 TFTP 启动
#   sysupgrade.bin                —— 标准 sysupgrade：tar（CONTROL、kernel = 内核 FIT、root = squashfs）+ fwtool 元数据（compat_version 2.0）
#   factory-ubi.bin               —— 整个 ubi_rootfs 的 ubinize 镜像（卷 rootfs + autoresize 的 rootfs_data + EOF 标记）+ fwtool 元数据，UBI 不可用时从自救系统 sysupgrade
#   tclinux.trx                   —— tcboot 时代的常规 HDR2 镜像（kernel + squashfs rootfs 段）；只保留格式，新布局下起不了 NAND 根
#
# tcboot 时代 U 盘优先的桥接启动件（内嵌五模式 /init 的 initramfs 内核与它的 HDR2 镜像）不在缺省构建里：救援 initramfs 改由 openwrt 组的按需工具 devices/openwrt/tools/bridge-initramfs/ 构建（设备 docs/36 §10 D3）。
# 不另写 KERNEL_INITRAMFS：配置开了 ramdisk（CONFIG_TARGET_ROOTFS_INITRAMFS）时取缺省的 KERNEL；GS2107 的配置基线不开它，不产出 initramfs-kernel.bin（设备 docs/36 §10 D3）。

define Device/chinamobile_gs2107
	DEVICE_VENDOR := ChinaMobile
	DEVICE_MODEL := GS2107
	DEVICE_DTS := en7528_gs2107
	# 2.0：根从裸 mtd4 搬进 UBI。旧布局的设备（compat_version 1.0）在 sysupgrade 时拒绝本镜像，要先经自救系统迁移（设备 docs/33 §5）。
	DEVICE_COMPAT_VERSION := 2.0
	DEVICE_COMPAT_MESSAGE := Root moved into UBI (squashfs volume rootfs on the ubi_rootfs mtd-concat). Migrate through the rescue system as described in devices/openwrt/gs2107/docs/33-ROOTFS-IN-UBI.md
	TRX_LOADADDR := 0x80002000
	TRX_MODEL := GS2107
	# kernel 分区 8 MiB；框架在 KERNEL 之后按 KERNEL_SIZE 做 check-size。
	KERNEL_SIZE := 8192k
	KERNEL := kernel-bin | fit none $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb
	KERNEL_INSTALL := 1
	IMAGES := sysupgrade.bin factory-ubi.bin tclinux.trx
	IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
	# 214016k = 0xD100000，ubi_rootfs 的大小
	IMAGE/factory-ubi.bin := append-ubi | check-size 214016k | append-metadata
	# tclinux.trx 的内核段仍是改用 FIT 之前的格式（原始内核 + DTB，前置 free bootbase 跳转后 LZMA 压缩，由 tcboot 解压）：KERNEL 已是 FIT，这里从原始内核重做，不取 append-kernel。
	IMAGE/tclinux.trx := copy-file $$(KDIR)/$$(KERNEL_NAME) | append-dtb | tclinux-free-bootbase-jump | lzma | tclinux-hdr2-trx
	# block-mount：提供 block 命令与 /etc/config/fstab；11-08 实测缺了它 fstools 的 mount_root 找不到 rootfs_data，overlay 退回 tmpfs（/tmp/root），配置每次重启全丢。
	# kmod-uboot-bootcount：U-Boot 把启动计数放在保留内存时（DTS 的 bootcount 节点），gs2107-bootok 经它清零；镜像里少了它，计数清不掉，连续 bootlimit 次热复位后 gs_fallback 就把 recovery 置 1、改走网络救援模式。
	DEVICE_PACKAGES := kmod-usb2 kmod-mt7603 kmod-mt7615e kmod-mt7663-firmware-ap \
		block-mount uboot-envtools kmod-uboot-bootcount
endef
TARGET_DEVICES += chinamobile_gs2107

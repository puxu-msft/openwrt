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

# GS2107（中移杭研智能家庭网关）—— 自维护板级
# 事实依据：devices/gs2107/{DEVICE-PROFILE,OPENWRT-FEASIBILITY,OPENWRT-PORT-PLAN}.md
#  - bootloader 为 free bootbase 变体（解压目标 0x80002000），与 JioFiber 同模式
#  - 镜像写到 tclinux 分区（24MB）：HDR2 头 + kernel(LZMA, 垫 4MB) + rootfs(squashfs)
#  - WiFi: MT7603E(2.4G) + MT7663E(5G) 均 PCIe
#  - mtd8(java) 严禁触碰 = CoAP RCE 回退通道
#
# [v2] 两种镜像（USB-BOOT-PLAN.md §0）：
#   tclinux.trx                    —— 常规镜像（kernel + squashfs rootfs 段）
#   tclinux-usbboot-initramfs.trx  —— U 盘优先引导内核：kernel 段里内嵌 initramfs
#                                     （/init 探测脚本：U盘→板载→驻留救援），
#                                     **无 rootfs 段**（rootfs 运行期才定）。
#                                     内核由 CONFIG_EXTERNAL_CPIO 指定，
#                                     见 target/linux/econet/image/usbboot-initramfs.sh
define Device/chinamobile_gs2107
	DEVICE_VENDOR := ChinaMobile
	DEVICE_MODEL := GS2107
	DEVICE_DTS := en7528_gs2107
	TRX_LOADADDR := 0x80002000
	TRX_MODEL := GS2107
	KERNEL := kernel-bin | append-dtb | tclinux-free-bootbase-jump | lzma
	KERNEL_INITRAMFS := kernel-bin | append-dtb | tclinux-free-bootbase-jump | lzma
	IMAGES := tclinux.trx tclinux-usbboot-initramfs.trx
	IMAGE/tclinux.trx := append-kernel | tclinux-hdr2-trx
	IMAGE/tclinux-usbboot-initramfs.trx := tclinux-hdr2-initramfs-trx
	# ★ 11-08 加 block-mount：没有它 rootfs 里就没有 `block` 命令、也没有
	#   /etc/config/fstab，fstools 的 mount_root 找不到 rootfs_data 分区 →
	#   退回 tmpfs overlay（/tmp/root），配置每次重启全丢。
	#   DTS 里 rootfs_data（mtd7 @0x980000，15MB）和内核 CONFIG_MTD_UBI=y
	#   都已就位，缺的只是用户态这一环。
	DEVICE_PACKAGES := kmod-usb2 kmod-mt7603 kmod-mt7615e kmod-mt7663-firmware-ap \
		block-mount uboot-envtools
endef
TARGET_DEVICES += chinamobile_gs2107

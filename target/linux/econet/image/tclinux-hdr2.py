#!/usr/bin/env python3
"""tclinux HDR2/TRX 镜像构建器（GS2107 / OpenWrt econet en7528 本地版）
=====================================================================

用途：替代上游 target/linux/econet/image/tclinux-trx.sh——该脚本依赖主机上的
zytrx 工具（OpenWrt 树内没有、需外部安装）仅为了算一个 CRC-32/JAMCRC。
本实现直接用 Python zlib 计算，**已与 GS2107 厂商镜像往返验证字节级一致**。

格式（2026-11-06 实证，见 devices/gs2107/tools/hdr2.py 与 OPENWRT-FEASIBILITY.md）：
  HDR2 头 256B（小端字段）：
    0x00 4B   魔数 "HDR2"（大端板子为 "2RDH"）
    0x04 u32  头长 0x100
    0x08 u32  镜像总长（头+kernel段+rootfs段）
    0x0c u32  CRC-32/JAMCRC（= zlib.crc32(payload) ^ 0xFFFFFFFF），覆盖 kernel段+padding+rootfs段
    0x10 32B  版本串
    0x30 32B  customer version（上游写 0；厂商镜像此处为 0a000000）
    0x50 u32  kernel 段长度（**含 0xFF padding**，规避 bootloader "A-length bug"）
    0x54 u32  rootfs 段长度
    0x58 u32  romfile 长度（0）
    0x5c 32B  model 串
    0x7c u32  loadaddr（bootloader 解压目标地址；GS2107 = 0x80002000）
    0x80 128B 保留（0）
  随后：kernel（LZMA）+ 0xFF padding 到 4MB + rootfs（squashfs）

用法（与上游 tclinux-trx.sh 参数兼容，--endian 默认 le）：
  tclinux-hdr2.py --kernel vmlinuz.bin.lzma --rootfs rootfs.squashfs \
      --version "OpenWrt-r1" --endian le --loadaddr 0x80002000 [--model GS2107]
"""
import argparse
import sys
import zlib

HDRLEN = 256
# 与上游 tclinux-trx.sh 一致：kernel 段垫到 4MB，rootfs 段从这里起。
# 该 padding 的唯一作用是规避 bootloader 的 "A-length bug"（从 A 槽读长度却启
# B 槽）——脚本注释原话 "the LZMA decompressor stops when it's done anyway"，
# 即长度只要"够大且恒定"就行，具体数值不限 4MB。
PAD_ROOTFS_OFFSET_TO = 4 * 1024 * 1024


def hexdump_field(val: int) -> bytes:
    """32 位值按端序输出 4 字节。"""
    return val.to_bytes(4, "big")


def build_header(endian: str, total_len: int, jamcrc: int, version: str,
                 kernel_len: int, rootfs_len: int, model: str, loadaddr: int) -> bytes:
    magic = b"HDR2" if endian == "le" else b"2RDH"
    bo = "little" if endian == "le" else "big"

    def u32(v):
        return v.to_bytes(4, bo)

    h = bytearray(HDRLEN)
    h[0x00:0x04] = magic
    h[0x04:0x08] = u32(HDRLEN)
    h[0x08:0x0C] = u32(total_len)
    h[0x0C:0x10] = u32(jamcrc)
    vb = version.encode()[:31]
    h[0x10:0x10 + len(vb)] = vb
    h[0x30:0x50] = b"\x00" * 32          # customer version
    h[0x50:0x54] = u32(kernel_len)
    h[0x54:0x58] = u32(rootfs_len)
    h[0x58:0x5C] = u32(0)                # romfile length
    mb = model.encode()[:31]
    h[0x5C:0x5C + len(mb)] = mb
    h[0x7C:0x80] = u32(loadaddr)
    return bytes(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True, help="LZMA 压缩后的内核文件")
    ap.add_argument("--rootfs", help="squashfs 根文件系统（可省，仅内核镜像）")
    ap.add_argument("--version", required=True, help="版本串（≤31 字符）")
    ap.add_argument("--endian", default="le", choices=["le", "be"])
    ap.add_argument("--model", default="")
    ap.add_argument("--loadaddr", default="0x80020000")
    a = ap.parse_args()

    loadaddr = int(a.loadaddr, 0)
    kernel = open(a.kernel, "rb").read()
    rootfs = open(a.rootfs, "rb").read() if a.rootfs else b""

    header_plus_kernel = HDRLEN + len(kernel)
    if PAD_ROOTFS_OFFSET_TO <= header_plus_kernel:
        sys.exit(f"kernel is too large ({len(kernel)}B > {PAD_ROOTFS_OFFSET_TO - HDRLEN}B)")
    padding = b"\xff" * (PAD_ROOTFS_OFFSET_TO - header_plus_kernel)

    padded_kernel_len = len(kernel) + len(padding)
    rootfs_len = len(rootfs)
    total_len = PAD_ROOTFS_OFFSET_TO + rootfs_len

    payload = kernel + padding + rootfs
    jamcrc = (zlib.crc32(payload) & 0xFFFFFFFF) ^ 0xFFFFFFFF

    hdr = build_header(a.endian, total_len, jamcrc, a.version,
                       padded_kernel_len, rootfs_len, a.model, loadaddr)

    sys.stderr.write(
        f"endian={a.endian} kernel={len(kernel)}B padding={len(padding)}B "
        f"rootfs={rootfs_len}B total={total_len}B jamcrc=0x{jamcrc:08x} "
        f"loadaddr=0x{loadaddr:08x}\n")
    sys.stdout.buffer.write(hdr + payload)


if __name__ == "__main__":
    main()

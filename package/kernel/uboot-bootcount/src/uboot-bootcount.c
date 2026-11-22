// SPDX-License-Identifier: GPL-2.0-only
/*
 * U-Boot 内存启动计数的读写。
 *
 * U-Boot 的 CONFIG_BOOTCOUNT_GENERIC（与 DM 的 bootcount-mem 驱动同一格式）把启动计数放在热复位后内容保持的一段内存里：两个 32 位字，偏移 0 是计数，偏移 4 是魔数 CONFIG_SYS_BOOTCOUNT_MAGIC（缺省 0xB001C041），字节序按 CONFIG_SYS_BOOTCOUNT_LE（缺省小端）。U-Boot 每次启动读出、加 1 再存，魔数不符（冷启动后内存内容随机）时按 0 算，超过 bootlimit 就走 altbootcmd（U-Boot drivers/bootcount/bootcount.c 的 bootcount_load/bootcount_store）。
 * 系统起来后由用户态清零，所以这里只管这 8 个字节：sysfs 属性 count 读出魔数正确时的计数、魔数不对时读 0；写入 N 时存计数 N 与魔数并读回核对，核对不过返回 -EIO。
 * 地址取自设备节点 memory-region 指向的保留内存（应带 no-map，内核不把它当普通内存分配）。映射用 ioremap（不缓存；MIPS 上物理地址在低 512 MiB 时直接落到 KSEG1）：写回式数据缓存里的脏行在热复位时不会写回 DRAM，经缓存写的清零会丢；U-Boot 侧同样经不缓存的地址访问。
 * compatible 用 "u-boot,bootcount-mem"：格式是 U-Boot 的，名字取它 DM 驱动的名字；U-Boot 自己的 "u-boot,bootcount" 绑定用 reg 直接给地址，这里按 Linux 保留内存的惯例用 memory-region，属性不同，所以不复用那个 compatible。
 */

#include <linux/io.h>
#include <linux/kstrtox.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#define UBOOT_BOOTCOUNT_MAGIC		0xB001C041
#define UBOOT_BOOTCOUNT_OFF_COUNT	0x0
#define UBOOT_BOOTCOUNT_OFF_MAGIC	0x4
#define UBOOT_BOOTCOUNT_SIZE		8

struct uboot_bootcount {
	void __iomem *base;
	/* 计数与魔数两个字作为一个整体读写 */
	struct mutex lock;
};

/* 魔数正确时返回 true 并把计数放进 *count；调用方持锁 */
static bool uboot_bootcount_load(struct uboot_bootcount *bc, u32 *count)
{
	if (readl(bc->base + UBOOT_BOOTCOUNT_OFF_MAGIC) != UBOOT_BOOTCOUNT_MAGIC)
		return false;
	*count = readl(bc->base + UBOOT_BOOTCOUNT_OFF_COUNT);
	return true;
}

static ssize_t count_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct uboot_bootcount *bc = dev_get_drvdata(dev);
	u32 count = 0;

	mutex_lock(&bc->lock);
	uboot_bootcount_load(bc, &count);
	mutex_unlock(&bc->lock);

	return sysfs_emit(buf, "%u\n", count);
}

static ssize_t count_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct uboot_bootcount *bc = dev_get_drvdata(dev);
	u32 count, readback = 0;
	bool valid;
	int ret;

	ret = kstrtou32(buf, 0, &count);
	if (ret)
		return ret;

	mutex_lock(&bc->lock);
	writel(count, bc->base + UBOOT_BOOTCOUNT_OFF_COUNT);
	writel(UBOOT_BOOTCOUNT_MAGIC, bc->base + UBOOT_BOOTCOUNT_OFF_MAGIC);
	valid = uboot_bootcount_load(bc, &readback);
	mutex_unlock(&bc->lock);

	if (!valid) {
		dev_err(dev, "wrote %u, but the magic did not read back\n", count);
		return -EIO;
	}
	if (readback != count) {
		dev_err(dev, "wrote %u, read back %u\n", count, readback);
		return -EIO;
	}
	return len;
}
static DEVICE_ATTR_RW(count);

static struct attribute *uboot_bootcount_attrs[] = {
	&dev_attr_count.attr,
	NULL
};
ATTRIBUTE_GROUPS(uboot_bootcount);

static int uboot_bootcount_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uboot_bootcount *bc;
	struct resource res;
	u32 count;
	int ret;

	ret = of_reserved_mem_region_to_resource(dev->of_node, 0, &res);
	if (ret)
		return dev_err_probe(dev, ret, "no usable memory-region\n");
	if (resource_size(&res) < UBOOT_BOOTCOUNT_SIZE)
		return dev_err_probe(dev, -EINVAL, "memory-region %pR is smaller than %d bytes\n",
				     &res, UBOOT_BOOTCOUNT_SIZE);

	bc = devm_kzalloc(dev, sizeof(*bc), GFP_KERNEL);
	if (!bc)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &bc->lock);
	if (ret)
		return ret;

	bc->base = devm_ioremap(dev, res.start, UBOOT_BOOTCOUNT_SIZE);
	if (!bc->base) {
		/* dev_err_probe 不打印 -ENOMEM，这里直接报 */
		dev_err(dev, "cannot map %pR\n", &res);
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, bc);

	/* 开机时的值记进内核日志：U-Boot 这一次存的计数，集成测试与排查时不必另读 */
	if (uboot_bootcount_load(bc, &count))
		dev_info(dev, "boot count %u at %pa\n", count, &res.start);
	else
		dev_info(dev, "no valid boot count at %pa (cold boot, or U-Boot does not keep it in RAM)\n",
			 &res.start);
	return 0;
}

static const struct of_device_id uboot_bootcount_of_match[] = {
	{ .compatible = "u-boot,bootcount-mem" },
	{ }
};
MODULE_DEVICE_TABLE(of, uboot_bootcount_of_match);

static struct platform_driver uboot_bootcount_driver = {
	.probe = uboot_bootcount_probe,
	.driver = {
		.name = "uboot-bootcount",
		.of_match_table = uboot_bootcount_of_match,
		.dev_groups = uboot_bootcount_groups,
	},
};
module_platform_driver(uboot_bootcount_driver);

MODULE_DESCRIPTION("U-Boot boot counter in reserved RAM");
MODULE_LICENSE("GPL");

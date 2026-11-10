// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hardware watchdog for the EcoNet EN7528 SoC.
 *
 * The watchdog is timer 5 of the SoC timer block based at 0x1fbf0100:
 * control register +0x00 carries the per-timer enable bits (bit 5 for
 * timer 5), the watchdog reset enable (bit 25) and a timer interrupt bit
 * (bit 21); +0x2c is the 32-bit reload value, +0x30 the current value and
 * +0x38 the key register (write 1 to reload).
 *
 * The counter runs at 112.5 MHz on this board. Measured by enabling timer 5
 * without the reset bit and sampling +0x30: 12,382,778 counts in 110.07 ms
 * (112,499,659 Hz). A 32-bit reload value therefore limits the timeout to
 * 38 seconds.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/watchdog.h>

#define EN7528_WDT_BASE		0x1fbf0100
#define EN7528_WDT_SIZE		0x3c

#define EN7528_WDT_CTRL		0x00
#define EN7528_WDT_LOAD		0x2c
#define EN7528_WDT_CUR		0x30
#define EN7528_WDT_RELOAD	0x38

#define EN7528_WDT_TIMER_ENABLE	BIT(5)
#define EN7528_WDT_IRQ_ENABLE	BIT(21)
#define EN7528_WDT_RESET_ENABLE	BIT(25)

#define EN7528_WDT_TICK_HZ	112500000U
#define EN7528_WDT_HEARTBEAT	15

static unsigned int tick_hz = EN7528_WDT_TICK_HZ;
module_param(tick_hz, uint, 0444);
MODULE_PARM_DESC(tick_hz, "Watchdog counter tick rate in Hz");

static int heartbeat = EN7528_WDT_HEARTBEAT;
module_param(heartbeat, int, 0444);
MODULE_PARM_DESC(heartbeat, "Initial watchdog timeout in seconds");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0444);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started");

struct en7528_wdt {
	struct watchdog_device wdd;
	void __iomem *base;
};

static int en7528_wdt_start(struct watchdog_device *wdd)
{
	struct en7528_wdt *wdt = watchdog_get_drvdata(wdd);
	u64 count = (u64)wdd->timeout * tick_hz;
	u32 ctrl;

	if (count > U32_MAX)
		return -EINVAL;

	writel((u32)count, wdt->base + EN7528_WDT_LOAD);

	ctrl = readl(wdt->base + EN7528_WDT_CTRL);
	ctrl &= ~EN7528_WDT_IRQ_ENABLE;
	ctrl |= EN7528_WDT_TIMER_ENABLE | EN7528_WDT_RESET_ENABLE;
	writel(ctrl, wdt->base + EN7528_WDT_CTRL);

	return 0;
}

static int en7528_wdt_stop(struct watchdog_device *wdd)
{
	struct en7528_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 ctrl;

	ctrl = readl(wdt->base + EN7528_WDT_CTRL);
	ctrl &= ~(EN7528_WDT_TIMER_ENABLE | EN7528_WDT_IRQ_ENABLE |
		  EN7528_WDT_RESET_ENABLE);
	writel(ctrl, wdt->base + EN7528_WDT_CTRL);

	return 0;
}

static int en7528_wdt_ping(struct watchdog_device *wdd)
{
	struct en7528_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(1, wdt->base + EN7528_WDT_RELOAD);

	return 0;
}

static int en7528_wdt_set_timeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	wdd->timeout = timeout;

	if (watchdog_active(wdd)) {
		en7528_wdt_stop(wdd);
		return en7528_wdt_start(wdd);
	}

	return 0;
}

static unsigned int en7528_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct en7528_wdt *wdt = watchdog_get_drvdata(wdd);

	return readl(wdt->base + EN7528_WDT_CUR) / tick_hz;
}

static const struct watchdog_info en7528_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
	.identity = "EN7528 watchdog",
};

static const struct watchdog_ops en7528_wdt_ops = {
	.owner = THIS_MODULE,
	.start = en7528_wdt_start,
	.stop = en7528_wdt_stop,
	.ping = en7528_wdt_ping,
	.set_timeout = en7528_wdt_set_timeout,
	.get_timeleft = en7528_wdt_get_timeleft,
};

static struct en7528_wdt *en7528_wdt;

static int __init en7528_wdt_init(void)
{
	int ret;

	if (!tick_hz)
		return -EINVAL;

	en7528_wdt = kzalloc(sizeof(*en7528_wdt), GFP_KERNEL);
	if (!en7528_wdt)
		return -ENOMEM;

	en7528_wdt->base = ioremap(EN7528_WDT_BASE, EN7528_WDT_SIZE);
	if (!en7528_wdt->base) {
		ret = -ENOMEM;
		goto err_free;
	}

	en7528_wdt->wdd.info = &en7528_wdt_info;
	en7528_wdt->wdd.ops = &en7528_wdt_ops;
	en7528_wdt->wdd.min_timeout = 1;
	en7528_wdt->wdd.max_timeout = U32_MAX / tick_hz;

	if (heartbeat < 1) {
		pr_warn("EN7528 watchdog: heartbeat %d clamped to 1 s\n",
			heartbeat);
		heartbeat = 1;
	} else if ((u32)heartbeat > en7528_wdt->wdd.max_timeout) {
		pr_warn("EN7528 watchdog: heartbeat %d clamped to %u s\n",
			heartbeat, en7528_wdt->wdd.max_timeout);
		heartbeat = en7528_wdt->wdd.max_timeout;
	}
	en7528_wdt->wdd.timeout = heartbeat;

	watchdog_set_drvdata(&en7528_wdt->wdd, en7528_wdt);
	watchdog_set_nowayout(&en7528_wdt->wdd, nowayout);
	watchdog_stop_on_unregister(&en7528_wdt->wdd);

	ret = watchdog_register_device(&en7528_wdt->wdd);
	if (ret)
		goto err_unmap;

	pr_info("EN7528 watchdog: %u Hz, timeout 1..%u s\n", tick_hz,
		en7528_wdt->wdd.max_timeout);

	return 0;

err_unmap:
	iounmap(en7528_wdt->base);
err_free:
	kfree(en7528_wdt);
	en7528_wdt = NULL;
	return ret;
}

static void __exit en7528_wdt_exit(void)
{
	/*
	 * Unload is an explicit administrative action. Disarm directly even
	 * under nowayout: the core refuses to stop a nowayout watchdog and
	 * would otherwise leave it armed without a driver to feed it.
	 */
	if (watchdog_active(&en7528_wdt->wdd))
		pr_warn("EN7528 watchdog: disarming on driver unload\n");
	en7528_wdt_stop(&en7528_wdt->wdd);

	watchdog_unregister_device(&en7528_wdt->wdd);
	iounmap(en7528_wdt->base);
	kfree(en7528_wdt);
}

module_init(en7528_wdt_init);
module_exit(en7528_wdt_exit);

MODULE_AUTHOR("homeiot");
MODULE_DESCRIPTION("EcoNet EN7528 hardware watchdog");
MODULE_LICENSE("GPL");

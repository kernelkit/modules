// SPDX-License-Identifier: GPL-2.0

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#include "pg210-ptp.h"
#include "pg210-ptp-regs.h"

struct pg210_ptp_port {
	int idx;
	u64 nom_period;
};

struct pg210_ptp {
	struct device *dev;

	struct clk *ts_clk;
	u64 nom_period;

	spinlock_t reg_lock;
	struct regmap *regs;

	unsigned long active_ports;
	struct pg210_ptp_port port[PG210_PTP_PORT_MAX];

	struct ptp_clock_info ptp_info;
	struct ptp_clock *ptp_clock;
};
#define pg210_ptp_from_port(_p)					\
	container_of((_p), struct pg210_ptp, port[(_p)->idx])

#define pg210_ptp_from_info(_info)				\
	container_of((_info), struct pg210_ptp, ptp_info)

static const struct regmap_config pg210_ptp_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= PG210_PTP_REGMAP_MAX,
};

static int pg210_ptp_gettimex(struct ptp_clock_info *info,
			     struct timespec64 *ts,
			     struct ptp_system_timestamp *sts)
{
	struct pg210_ptp *priv = pg210_ptp_from_info(info);
	unsigned long flags;
	u32 ns, sec[2];
	int err;

	spin_lock_irqsave(&priv->reg_lock, flags);

	ptp_read_system_prets(sts);
	err = regmap_write(priv->regs, PG210_PTP_TOD_SNAPSHOT,
			   PG210_PTP_TOD_SNAPSHOT_ALL);
	ptp_read_system_postts(sts);

	err = err ? : regmap_read(priv->regs, PG210_PTP_TOD_SYS_SEC_LO, &sec[0]);
	err = err ? : regmap_read(priv->regs, PG210_PTP_TOD_SYS_SEC_HI, &sec[1]);
	err = err ? : regmap_read(priv->regs, PG210_PTP_TOD_SYS_NS, &ns);

	spin_unlock_irqrestore(&priv->reg_lock, flags);

	if (err)
		return err;

	ts->tv_sec = ((u64)(sec[1] & PG210_PTP_TOD_SEC_HI_MASK) << 32) | sec[0];
	ts->tv_nsec = ns & PG210_PTP_TOD_NS_MASK;
	return 0;
}

static int pg210_ptp_settime(struct ptp_clock_info *info,
			    const struct timespec64 *ts)
{
	struct pg210_ptp *priv = pg210_ptp_from_info(info);
	unsigned long flags;
	int err;

	spin_lock_irqsave(&priv->reg_lock, flags);
	err = regmap_write(priv->regs, PG210_PTP_TOD_SW_SEC_LO,
			   lower_32_bits(ts->tv_sec));
	err = err ? : regmap_write(priv->regs, PG210_PTP_TOD_SW_SEC_HI,
				   upper_32_bits(ts->tv_sec) & PG210_PTP_TOD_SEC_HI_MASK);
	err = err ? : regmap_write(priv->regs, PG210_PTP_TOD_SW_NS,
				   ts->tv_nsec & PG210_PTP_TOD_NS_MASK);
	err = err ? : regmap_write(priv->regs, PG210_PTP_TOD_SW_LOAD,
				   PG210_PTP_TOD_SW_LOAD_TOD);
	spin_unlock_irqrestore(&priv->reg_lock, flags);
	return err;
}

static int pg210_ptp_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	struct pg210_ptp *priv = pg210_ptp_from_info(info);
	unsigned long flags;
	u64 period;
	int err;

	period = adjust_by_scaled_ppm(priv->nom_period, scaled_ppm);

	spin_lock_irqsave(&priv->reg_lock, flags);
	err = regmap_write(priv->regs, PG210_PTP_TOD_SYS_PERIOD_LO,
			   lower_32_bits(period));
	err = err ? : regmap_write(priv->regs, PG210_PTP_TOD_SYS_PERIOD_HI,
				   upper_32_bits(period) & PG210_PTP_TOD_SYS_PERIOD_HI_MASK);
	spin_unlock_irqrestore(&priv->reg_lock, flags);

	return err;
}

static int pg210_ptp_adjtime(struct ptp_clock_info *info, s64 delta)
{
	struct timespec64 ts;
	int ret;

	/* TODO: for |delta| below ~1s, program TOD_{SEC,NS}_SYS_OFFSET and
	 * trigger TOD_SW_LOAD[OFFSET] instead of a full get/set.
	 */
	ret = pg210_ptp_gettimex(info, &ts, NULL);
	if (ret)
		return ret;

	ts = timespec64_add(ts, ns_to_timespec64(delta));
	return pg210_ptp_settime(info, &ts);
}

static int pg210_ptp_clock_register(struct pg210_ptp *priv)
{
	priv->ptp_info = (struct ptp_clock_info) {
		.owner		= THIS_MODULE,
		.max_adj	= 100000000,	/* placeholder; refine per ts_clk */
		.adjfine	= pg210_ptp_adjfine,
		.adjtime	= pg210_ptp_adjtime,
		.gettimex64	= pg210_ptp_gettimex,
		.settime64	= pg210_ptp_settime,
	};

	strncpy(priv->ptp_info.name, dev_name(priv->dev), sizeof(priv->ptp_info.name));

	priv->ptp_clock = ptp_clock_register(&priv->ptp_info, priv->dev);
	if (IS_ERR(priv->ptp_clock))
		return PTR_ERR(priv->ptp_clock);

	return 0;
}

static void pg210_ptp_clock_unregister(struct pg210_ptp *priv)
{
	if (!IS_ERR_OR_NULL(priv->ptp_clock))
		ptp_clock_unregister(priv->ptp_clock);
}

static int pg210_ptp_port_read(struct pg210_ptp_port *port, u8 reg, u32 *val)
{
	struct pg210_ptp *priv = pg210_ptp_from_port(port);

	return regmap_read(priv->regs, PG210_PTP_PORT_REG(port->idx, reg), val);
}

static int pg210_ptp_port_write(struct pg210_ptp_port *port, u8 reg, u32 val)
{
	struct pg210_ptp *priv = pg210_ptp_from_port(port);

	return regmap_write(priv->regs, PG210_PTP_PORT_REG(port->idx, reg), val);
}

static int pg210_ptp_port_probe(struct pg210_ptp *priv, int idx)
{
	struct pg210_ptp_port *port = &priv->port[idx];
	u32 nom_period_tx[2], nom_period_rx[2];
	int err;

	port->idx = idx;

	err = pg210_ptp_port_read(port, PG210_PTP_CORE_TX_PERIOD_LO, &nom_period_tx[0]);
	err = err ? : pg210_ptp_port_read(port, PG210_PTP_CORE_TX_PERIOD_HI, &nom_period_tx[1]);
	err = err ? : pg210_ptp_port_read(port, PG210_PTP_CORE_RX_PERIOD_LO, &nom_period_rx[0]);
	err = err ? : pg210_ptp_port_read(port, PG210_PTP_CORE_RX_PERIOD_HI, &nom_period_rx[1]);
	if (err)
		return err;

	if ((!nom_period_tx[0] && !nom_period_tx[1]) ||
	    (!nom_period_rx[0] && !nom_period_rx[1]))
		/* Tx or Rx clock not connected => unused */
		return 0;

	if ((nom_period_tx[0] != nom_period_rx[0]) ||
	    (nom_period_tx[1] != nom_period_rx[1])) {
		dev_warn(priv->dev,
			 "Asymmetric rx/tx clocks are not supported, skipping port %d\n",
			 idx);
		return 0;
	}

	err = pg210_ptp_port_write(port, PG210_PTP_TX_PERIOD_LO, nom_period_tx[0]);
	err = err ? : pg210_ptp_port_write(port, PG210_PTP_TX_PERIOD_HI, nom_period_tx[1]);
	err = err ? : pg210_ptp_port_write(port, PG210_PTP_RX_PERIOD_LO, nom_period_tx[0]);
	err = err ? : pg210_ptp_port_write(port, PG210_PTP_RX_PERIOD_HI, nom_period_tx[1]);
	if (err)
		return err;

	port->nom_period = ((u64)(nom_period_tx[1]) << 32) | (u64)(nom_period_tx[0]);
	priv->active_ports |= BIT(idx);

	return 0;
}

static int pg210_ptp_probe_ports(struct pg210_ptp *priv)
{
	int err, i;

	for (i = 0; i < ARRAY_SIZE(priv->port); i++) {
		err = pg210_ptp_port_probe(priv, i);
		if (err)
			break;
	}

	if (err)
		return err;

	if (!priv->active_ports) {
		dev_err(priv->dev, "No active ports found\n");
		return -ENODEV;
	}

	/* TODO enable all ports */
	return 0;
}

static int pg210_ptp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pg210_ptp *priv;
	void __iomem *base;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->dev = dev;
	spin_lock_init(&priv->reg_lock);

	priv->ts_clk = devm_clk_get_enabled(dev, "ts_clk");
	if (IS_ERR(priv->ts_clk))
		return dev_err_probe(dev, PTR_ERR(priv->ts_clk),
				     "Input clock not found\n");

	priv->nom_period = mul_u64_u64_div_u64(NSEC_PER_SEC, 1ULL << 48,
					       clk_get_rate(priv->ts_clk));

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base),
				     "I/O memory not available\n");

	priv->regs = devm_regmap_init_mmio(dev, base, &pg210_ptp_regmap_config);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "Failed to initialize regmap\n");

	err = pg210_ptp_probe_ports(priv);
	if (err)
		return err;

	err = regmap_write(priv->regs, PG210_PTP_TOD_CONFIG,
			   PG210_PTP_TOD_CONFIG_EN_SYS_TIMER |
			   PG210_PTP_TOD_CONFIG_AUTO_REFRESH |
			   PG210_PTP_TOD_OVERWRITE_NONE |
			   FIELD_PREP(PG210_PTP_TOD_CONFIG_PORT_MASK, priv->active_ports));
	if (err)
		return dev_err_probe(dev, err, "Failed to enable clocks\n");

	err = pg210_ptp_clock_register(priv);
	if (err)
		return dev_err_probe(dev, err, "Failed to register PHC\n");

	dev_info(dev, "%u ports active\n",
		 bitmap_weight(&priv->active_ports, PG210_PTP_PORT_MAX));

	return 0;
}

static void pg210_ptp_remove(struct platform_device *pdev)
{
	struct pg210_ptp *priv = platform_get_drvdata(pdev);

	pg210_ptp_clock_unregister(priv);
}

static struct platform_driver pg210_ptp_driver = {
	.probe	= pg210_ptp_probe,
	.remove	= pg210_ptp_remove,
	.driver	= {
		.name = "pg210-ptp",
	},
};
module_platform_driver(pg210_ptp_driver);

/**
 * pg210_ptp_gettime() - Read the current time of a PG210 timer
 * @dev: The timer's device
 * @ts:  Current time
 *
 * Allows drivers of hardware that is fed by the timer's time of day
 * (e.g. to schedule events relative to it) to read the time without
 * having to go via the PHC's character device.
 *
 * Return: 0 on success, -ENODEV if @dev is not bound to this driver.
 */
int pg210_ptp_gettime(struct device *dev, struct timespec64 *ts)
{
	struct pg210_ptp *priv;

	if (dev->driver != &pg210_ptp_driver.driver)
		return -ENODEV;

	priv = dev_get_drvdata(dev);
	return pg210_ptp_gettimex(&priv->ptp_info, ts, NULL);
}
EXPORT_SYMBOL_GPL(pg210_ptp_gettime);

MODULE_DESCRIPTION("AMD/Xilinx PG210 PTP 1588 Timer Syncer");
MODULE_LICENSE("GPL");

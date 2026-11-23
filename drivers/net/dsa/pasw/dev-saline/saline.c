#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/units.h>

#include <linux/platform_data/dsa.h>

#include "../pasw.h"
#include "../pasw-abi.h"

#define SALINE_MACS 4

enum saline_tx_ts_reg {
	TX_TS_PORT_STRIDE = 0x20,
	TX_TS_PORT_NS     = 0x00,
	TX_TS_PORT_SEC_LO = 0x04,
	TX_TS_PORT_SEC_HI = 0x08,
	TX_TS_PORT_CTRL   = 0x0c,
	TX_TS_PORT_CTRL__IRQ_EN = BIT(0),
	TX_TS_IRQ_STATUS  = 0x80,
	TX_TS_IRQ_PENDING = 0x84,
};
#define saline_tx_ts_port_reg(_p, _reg) \
	((_p) * TX_TS_PORT_STRIDE + (_reg))

/* A timestamp normally shows up within microseconds of the request,
 * so poll quickly for a while before backing off to once per jiffy.
 */
#define SALINE_TX_TS_POLL_US	10
#define SALINE_TX_TS_SPIN_US	1000
#define SALINE_TX_TS_TIMEOUT_MS	20

enum xlnx_xgmac_reg {
	CONF_TX_1        = 0x000c,
	CONF_TX_1__ENABLE        = BIT(0),
	CONF_RX_1        = 0x0014,
	CONF_RX_1__ENABLE        = BIT(0),
	TICK             = 0x0020,
	TICK__TRIGGER            = BIT(0),

	STAT_RX_1 = 0x0404,
	STAT_RX_1__STATUS = BIT(0)
};

struct xlnx_xgmac_cntr {
	u16 offs;
	const char *name;
};

const struct xlnx_xgmac_cntr saline_mac_counters[] = {
	{ .offs = 0x0500, .name = "status-cycle-count" },
	{ .offs = 0x0648, .name = "rx-framing-err" },
	{ .offs = 0x0660, .name = "rx-bad-code" },
	{ .offs = 0x0668, .name = "rx-error" },
	{ .offs = 0x0670, .name = "rx-rsfec-corrected-cw-inc" },
	{ .offs = 0x0678, .name = "rx-rsfec-uncorrected-cw-inc" },
	{ .offs = 0x0680, .name = "rx-rsfec-err-count0-inc" },
	{ .offs = 0x06a0, .name = "tx-frame-error" },
	{ .offs = 0x0700, .name = "tx-total-packets" },
	{ .offs = 0x0708, .name = "tx-total-good-packets" },
	{ .offs = 0x0710, .name = "tx-total-bytes" },
	{ .offs = 0x0718, .name = "tx-total-good-bytes" },
	{ .offs = 0x0720, .name = "tx-packet-64-bytes" },
	{ .offs = 0x0728, .name = "tx-packet-65-127-bytes" },
	{ .offs = 0x0730, .name = "tx-packet-128-255-bytes" },
	{ .offs = 0x0738, .name = "tx-packet-256-511-bytes" },
	{ .offs = 0x0740, .name = "tx-packet-512-1023-bytes" },
	{ .offs = 0x0748, .name = "tx-packet-1024-1518-bytes" },
	{ .offs = 0x0750, .name = "tx-packet-1519-1522-bytes" },
	{ .offs = 0x0758, .name = "tx-packet-1523-1548-bytes" },
	{ .offs = 0x0760, .name = "tx-packet-1549-2047-bytes" },
	{ .offs = 0x0768, .name = "tx-packet-2048-4095-bytes" },
	{ .offs = 0x0770, .name = "tx-packet-4096-8191-bytes" },
	{ .offs = 0x0778, .name = "tx-packet-8192-9215-bytes" },
	{ .offs = 0x0780, .name = "tx-packet-large" },
	{ .offs = 0x0788, .name = "tx-packet-small" },
	{ .offs = 0x07b8, .name = "tx-bad-fcs" },
	{ .offs = 0x07d0, .name = "tx-unicast" },
	{ .offs = 0x07d8, .name = "tx-multicast" },
	{ .offs = 0x07e0, .name = "tx-broadcast" },
	{ .offs = 0x07e8, .name = "tx-vlan" },
	{ .offs = 0x07f0, .name = "tx-pause" },
	{ .offs = 0x07f8, .name = "tx-user-pause" },
	{ .offs = 0x0808, .name = "rx-total-packets" },
	{ .offs = 0x0810, .name = "rx-total-good-packets" },
	{ .offs = 0x0818, .name = "rx-total-bytes" },
	{ .offs = 0x0820, .name = "rx-total-good-bytes" },
	{ .offs = 0x0828, .name = "rx-packet-64-bytes" },
	{ .offs = 0x0830, .name = "rx-packet-65-127-bytes" },
	{ .offs = 0x0838, .name = "rx-packet-128-255-bytes" },
	{ .offs = 0x0840, .name = "rx-packet-256-511-bytes" },
	{ .offs = 0x0848, .name = "rx-packet-512-1023-bytes" },
	{ .offs = 0x0850, .name = "rx-packet-1024-1518-bytes" },
	{ .offs = 0x0858, .name = "rx-packet-1519-1522-bytes" },
	{ .offs = 0x0860, .name = "rx-packet-1523-1548-bytes" },
	{ .offs = 0x0868, .name = "rx-packet-1549-2047-bytes" },
	{ .offs = 0x0870, .name = "rx-packet-2048-4095-bytes" },
	{ .offs = 0x0878, .name = "rx-packet-4096-8191-bytes" },
	{ .offs = 0x0880, .name = "rx-packet-8192-9215-bytes" },
	{ .offs = 0x0888, .name = "rx-packet-large" },
	{ .offs = 0x0890, .name = "rx-packet-small" },
	{ .offs = 0x0898, .name = "rx-undersize" },
	{ .offs = 0x08a0, .name = "rx-fragment" },
	{ .offs = 0x08a8, .name = "rx-oversize" },
	{ .offs = 0x08b0, .name = "rx-toolong" },
	{ .offs = 0x08b8, .name = "rx-jabber" },
	{ .offs = 0x08c0, .name = "rx-bad-fcs" },
	{ .offs = 0x08c8, .name = "rx-packet-bad-fcs" },
	{ .offs = 0x08d0, .name = "rx-stomped-fcs" },
	{ .offs = 0x08d8, .name = "rx-unicast" },
	{ .offs = 0x08e0, .name = "rx-multicast" },
	{ .offs = 0x08e8, .name = "rx-broadcast" },
	{ .offs = 0x08f0, .name = "rx-vlan" },
	{ .offs = 0x08f8, .name = "rx-pause" },
	{ .offs = 0x0900, .name = "rx-user-pause" },
	{ .offs = 0x0908, .name = "rx-inrangeerr" },
	{ .offs = 0x0910, .name = "rx-truncated" },
	{ .offs = 0x0918, .name = "rx-test-pattern-mismatch" },
	{ .offs = 0x0920, .name = "fec-inc-correct-count" },
	{ .offs = 0x0928, .name = "fec-inc-cant-correct-count" },
	{ .offs = 0x0980, .name = "tx-mm-status" },
	{ .offs = 0x0988, .name = "tx-mm-status" },
	{ .offs = 0x0990, .name = "tx-mm-hold" },
	{ .offs = 0x0998, .name = "rx-mm-assembly-error" },
	{ .offs = 0x09a0, .name = "rx-mm-frame-smd-error" },
	{ .offs = 0x09a8, .name = "rx-mm-frame-assembly-ok" },
	{ .offs = 0x09b0, .name = "rx-mm-fragment" },
};

struct saline_hw_bar0 {
	struct {
		u8 regs[SZ_64K];
	} mac[SALINE_MACS];
	u8 reserved_0x40000[SZ_64K][2];
	struct {
		u8 regs[SZ_64K];
	} i2c;
	struct {
		u8 regs[SZ_64K];
	} stat;
	u8 reserved_0x80000[SZ_64K][2];
	struct {
		u8 regs[SZ_64K];
	} ptp_timer;
	struct {
		u8 regs[SZ_64K];
	} ptp_tx_ts;
	struct {
		u8 regs[SZ_64K];
	} pasw;
} __packed;

#define saline_iomap_bar0(_ssw, _seg)					\
	pcim_iomap_range(to_pci_dev((_ssw)->sw.dev), 0,			\
			 offsetof(struct saline_hw_bar0, _seg),		\
			 offsetofend(struct saline_hw_bar0, _seg));

struct saline_mac {
	int idx;

	struct regmap *regs;
	u64 cntr_acc[ARRAY_SIZE(saline_mac_counters)];

	struct {
		struct sk_buff *skb;
		ktime_t deadline;
	} ts;
};

/* From pg210-ptp.h, which is not on this module's include path */
int pg210_ptp_gettime(struct device *dev, struct timespec64 *ts);

struct saline {
	struct pasw sw;
	struct platform_device *xdma;
	struct platform_device *ptp;
	typeof(&pg210_ptp_gettime) ptp_gettime;

	void __iomem *iomem;

	struct {
		struct regmap *regs;
		struct delayed_work dwork;
		spinlock_t lock;
	} tx_ts;
};

static int saline_mac_read_status(struct phy_device *phydev)
{
	struct saline_mac *smac = phydev->priv;
	u32 stat;
	int err;

	phydev->speed = SPEED_UNKNOWN;
	phydev->duplex = DUPLEX_UNKNOWN;
	phydev->link = false;
	phydev->pause = false;
	phydev->asym_pause = false;

	err = regmap_read(smac->regs, STAT_RX_1, &stat);
	if (err)
		return err;

	if (FIELD_GET(STAT_RX_1__STATUS, stat)) {
		phydev->speed = SPEED_10000;
		phydev->duplex = DUPLEX_FULL;
		phydev->link = true;
	}

	return 0;
}

static int saline_mac_config_aneg(struct phy_device *phydev)
{
	return 0;
}

static int saline_mac_config_init(struct phy_device *phydev)
{
	struct saline_mac *smac = phydev->priv;
	u32 stat;

	/* Clear any old latched status bits on reconfig */
	return regmap_read(smac->regs, STAT_RX_1, &stat);
}

static int saline_mac_resume(struct phy_device *phydev)
{
	struct saline_mac *smac = phydev->priv;
	int err;

	err = regmap_update_bits(smac->regs, CONF_TX_1,
				 CONF_TX_1__ENABLE, CONF_TX_1__ENABLE);
	err = err ? : regmap_update_bits(smac->regs, CONF_RX_1,
					 CONF_RX_1__ENABLE, CONF_RX_1__ENABLE);
	return err;
}

static int saline_mac_suspend(struct phy_device *phydev)
{
	struct saline_mac *smac = phydev->priv;
	int err;

	err = regmap_update_bits(smac->regs, CONF_TX_1,
				 CONF_TX_1__ENABLE, 0);
	err = err ? : regmap_update_bits(smac->regs, CONF_RX_1,
					 CONF_RX_1__ENABLE, 0);
	return err;
}

static int saline_mac_get_features(struct phy_device *phydev)
{
	unsigned long *supported = phydev->supported;

	linkmode_set_bit(ETHTOOL_LINK_MODE_10000baseCR_Full_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10000baseSR_Full_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10000baseLR_Full_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10000baseLRM_Full_BIT, supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10000baseER_Full_BIT, supported);
	return 0;
}

static int saline_mac_probe(struct phy_device *phydev)
{
	static const struct regmap_config regs_config = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.max_register = SZ_16K - 1,
	};

	struct saline *ssw = phydev->mdio.bus->priv;
	struct saline_mac *smac;
	void __iomem *iomem;

	smac = devm_kzalloc(&phydev->mdio.dev, sizeof(*smac), GFP_KERNEL);
	if (!smac)
		return -ENOMEM;

	smac->idx = phydev->mdio.addr;

	iomem = saline_iomap_bar0(ssw, mac[smac->idx]);
	if (IS_ERR(iomem)) {
		dev_err(ssw->sw.dev, "Unable to remap MAC%d I/O memory: %pe",
			smac->idx, iomem);
		return PTR_ERR(iomem);
	}

	smac->regs = devm_regmap_init_mmio(&phydev->mdio.dev, iomem, &regs_config);
	if (IS_ERR(smac->regs)) {
		dev_err(&phydev->mdio.dev, "Unable to create regmap: %pe", smac->regs);
		return PTR_ERR(smac->regs);
	}

	phydev->priv = smac;

	phydev->interface = PHY_INTERFACE_MODE_INTERNAL;
	return 0;
}

static struct phy_driver saline_mac_driver = {
#define SALINE_MAC_PHYID 0xfffffffe
	.phy_id = SALINE_MAC_PHYID,
	.phy_id_mask = -1,
	.name = "Saline XGMAC",
	.probe = saline_mac_probe,
	.get_features = saline_mac_get_features,
	.resume = saline_mac_resume,
	.suspend = saline_mac_suspend,
	.config_init = saline_mac_config_init,
	.config_aneg = saline_mac_config_aneg,
	.read_status = saline_mac_read_status,
};

static struct saline_mac *saline_mac_from_dp(struct dsa_port *dp)
{
	if (dsa_is_user_port(dp->ds, dp->index) && dp->user->phydev)
		return dp->user->phydev->priv;

	return NULL;
}

static struct saline *saline_from_ds(struct dsa_switch *ds)
{
	struct pasw *sw = ds->priv;

	return container_of(sw, struct saline, sw);
}

static int saline_dso_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct saline_mac *smac;

	smac = saline_mac_from_dp(dsa_to_port(ds, port));
	if (!smac)
		return -EOPNOTSUPP;

	if (sset != ETH_SS_STATS)
		return 0;

	return ARRAY_SIZE(saline_mac_counters);
}

static void saline_dso_get_strings(struct dsa_switch *ds, int port,
				   u32 stringset, uint8_t *data)
{
	const struct xlnx_xgmac_cntr *cntr;
	struct saline_mac *smac;
	int i;

	smac = saline_mac_from_dp(dsa_to_port(ds, port));
	if (!smac)
		return;

	for (i = 0, cntr = saline_mac_counters;
	     i < ARRAY_SIZE(saline_mac_counters); i++, cntr++)
		ethtool_puts(&data, cntr->name);
}

static void saline_dso_get_ethtool_stats(struct dsa_switch *ds,
					 int port, uint64_t *data)
{
	struct saline *ssw = saline_from_ds(ds);
	const struct xlnx_xgmac_cntr *cntr;
	struct saline_mac *smac;
	u32 lsb, msb;
	int err, i;

	smac = saline_mac_from_dp(dsa_to_port(ds, port));
	if (!smac)
		return;

	/* Atomically latch accumulated counters to the counter
	 * registers and clear the accumulators
	 */
	err = regmap_update_bits(smac->regs, TICK, TICK__TRIGGER, TICK__TRIGGER);
	if (err) {
		dev_err(ssw->sw.dev, "Failed to trigger MAC counter update on p%d: %pe\n",
			port, ERR_PTR(err));
		return;
	}

	for (i = 0, cntr = saline_mac_counters;
	     i < ARRAY_SIZE(saline_mac_counters); i++, cntr++, data++) {
		err = err ? : regmap_read(smac->regs, cntr->offs + 0, &lsb);
		err = err ? : regmap_read(smac->regs, cntr->offs + 4, &msb);
		if (err)
			break;

		smac->cntr_acc[i] += ((u64)msb << 32) | (u64)lsb;
		*data = smac->cntr_acc[i];
	}

	if (err) {
		dev_err(ssw->sw.dev, "Failed to read MAC counters on p%d: %pe\n",
			port, ERR_PTR(err));
	}
}

static void saline_tx_ts_drop(struct saline_mac *smac)
{
	kfree_skb(smac->ts.skb);
	smac->ts.skb = NULL;
}

static void saline_tx_ts_complete(struct saline *ssw, struct saline_mac *smac)
{
	struct skb_shared_hwtstamps shwt;
	int err, port = smac->idx;
	u32 ns, sec[2];

	err = regmap_read(ssw->tx_ts.regs,
			  saline_tx_ts_port_reg(port, TX_TS_PORT_NS), &ns);
	err = err ? : regmap_read(ssw->tx_ts.regs,
				  saline_tx_ts_port_reg(port, TX_TS_PORT_SEC_LO), &sec[0]);
	err = err ? : regmap_read(ssw->tx_ts.regs,
				  saline_tx_ts_port_reg(port, TX_TS_PORT_SEC_HI), &sec[1]);
	if (err) {
		saline_tx_ts_drop(smac);
		return;
	}

	shwt.hwtstamp = ktime_set(((u64)sec[1] << 32) | ((u64)sec[0]), ns);
	skb_complete_tx_timestamp(smac->ts.skb, &shwt);
	smac->ts.skb = NULL;
}

/* Complete or expire outstanding requests. A status bit for a port
 * without a request belongs to a request that has already expired,
 * and is discarded. Returns true if any request is still outstanding.
 */
static bool saline_tx_ts_drain(struct saline *ssw)
{
	ktime_t now = ktime_get();
	struct saline_mac *smac;
	bool pending = false;
	u32 status;
	int i;

	lockdep_assert_held(&ssw->tx_ts.lock);

	if (regmap_read(ssw->tx_ts.regs, TX_TS_IRQ_STATUS, &status))
		status = 0;

	for (i = 0; i < SALINE_MACS; i++) {
		smac = saline_mac_from_dp(dsa_to_port(&ssw->sw.ds, i));
		if (!smac || !smac->ts.skb)
			continue;

		if (status & BIT(i)) {
			saline_tx_ts_complete(ssw, smac);
		} else if (ktime_after(now, smac->ts.deadline)) {
			dev_warn_ratelimited(ssw->sw.dev,
					     "Tx timestamp timed out on port %d\n", i);
			saline_tx_ts_drop(smac);
		} else {
			pending = true;
		}
	}

	return pending;
}

static bool saline_tx_ts_drain_locked(struct saline *ssw)
{
	bool pending;

	spin_lock_bh(&ssw->tx_ts.lock);
	pending = saline_tx_ts_drain(ssw);
	spin_unlock_bh(&ssw->tx_ts.lock);

	return pending;
}

static void saline_tx_ts_poll(struct work_struct *work)
{
	struct saline *ssw = container_of(work, struct saline, tx_ts.dwork.work);
	bool pending;

	if (read_poll_timeout(saline_tx_ts_drain_locked, pending, !pending,
			      SALINE_TX_TS_POLL_US, SALINE_TX_TS_SPIN_US, false, ssw))
		queue_delayed_work(system_highpri_wq, &ssw->tx_ts.dwork, 1);
}

static void saline_dso_port_txtstamp(struct dsa_switch *ds, int port,
				     struct sk_buff *skb)
{
	struct saline *ssw = saline_from_ds(ds);
	struct saline_mac *smac;
	struct sk_buff *clone;

	smac = saline_mac_from_dp(dsa_to_port(ds, port));
	if (!smac)
		return;

	clone = skb_clone_sk(skb);
	if (!clone)
		return;

	spin_lock_bh(&ssw->tx_ts.lock);
	saline_tx_ts_drain(ssw);
	if (!smac->ts.skb) {
		smac->ts.skb = clone;
		smac->ts.deadline = ktime_add_ms(ktime_get(), SALINE_TX_TS_TIMEOUT_MS);
		clone = NULL;
	}
	spin_unlock_bh(&ssw->tx_ts.lock);

	if (clone) {
		/* The hardware holds a single timestamp per port, so
		 * don't let the tagger request a new one until the
		 * outstanding one has been read.
		 */
		skb_shinfo(skb)->tx_flags &= ~SKBTX_HW_TSTAMP_NOBPF;

		dev_err_ratelimited(ssw->sw.dev,
				    "Ignoring concurrent timestamping request on port %d\n",
				    port);
		kfree_skb(clone);
		return;
	}

	skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	mod_delayed_work(system_highpri_wq, &ssw->tx_ts.dwork, 0);
}

static int saline_dso_port_hwtstamp_set(struct dsa_switch *ds, int port,
					struct kernel_hwtstamp_config *config,
					struct netlink_ext_ack *extack)
{
	struct saline *ssw = saline_from_ds(ds);
	int err;

	switch (config->tx_type) {
	case HWTSTAMP_TX_ON:
		if (config->rx_filter != HWTSTAMP_FILTER_PTP_V2_L2_EVENT)
			return -ERANGE;
		err = regmap_set_bits(ssw->tx_ts.regs,
				      saline_tx_ts_port_reg(port, TX_TS_PORT_CTRL),
				      TX_TS_PORT_CTRL__IRQ_EN);
		break;
	case HWTSTAMP_TX_OFF:
		err = regmap_clear_bits(ssw->tx_ts.regs,
					saline_tx_ts_port_reg(port, TX_TS_PORT_CTRL),
					TX_TS_PORT_CTRL__IRQ_EN);
		break;
	default:
		return -ERANGE;
	}

	return err;
}

static int saline_dso_port_hwtstamp_get(struct dsa_switch *ds, int port,
					struct kernel_hwtstamp_config *config)
{
	struct saline *ssw = saline_from_ds(ds);
	int ret;

	ret = regmap_test_bits(ssw->tx_ts.regs,
			       saline_tx_ts_port_reg(port, TX_TS_PORT_CTRL),
			       TX_TS_PORT_CTRL__IRQ_EN);
	if (ret < 0)
		return ret;

	*config = (struct kernel_hwtstamp_config) {
		.flags = 0,
		.tx_type = ret ? HWTSTAMP_TX_ON : HWTSTAMP_TX_OFF,
		.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT,
	};

	return 0;
}

static int saline_dso_get_ts_info(struct dsa_switch *ds, int port,
				  struct kernel_ethtool_ts_info *info)
{
	struct saline *ssw = saline_from_ds(ds);
	int phc_index;

	if (!ssw->ptp)
		return -EOPNOTSUPP;

	phc_index = ptp_clock_index_by_dev(&ssw->ptp->dev);
	if (phc_index < 0)
		return -EOPNOTSUPP;

	info->so_timestamping =
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;
	info->tx_types =
		BIT(HWTSTAMP_TX_ON) |
		BIT(HWTSTAMP_TX_OFF);
	info->rx_filters =
		BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT);
	info->phc_index = phc_index;
	return 0;
}


static const struct dsa_switch_ops saline_dsos = {
	.get_sset_count      = saline_dso_get_sset_count,
	.get_strings         = saline_dso_get_strings,
	.get_ethtool_stats   = saline_dso_get_ethtool_stats,

	.get_ts_info         = saline_dso_get_ts_info,
	.port_hwtstamp_get   = saline_dso_port_hwtstamp_get,
	.port_hwtstamp_set   = saline_dso_port_hwtstamp_set,
	.port_txtstamp       = saline_dso_port_txtstamp,
};

static unsigned long saline_plmo_mac_get_caps(struct phylink_config *config,
					      phy_interface_t interface)
{
	return MAC_10000FD;
}

static void saline_plmo_mac_config(struct phylink_config *config,
				   unsigned int mode,
				   const struct phylink_link_state *state)
{
}

static void saline_plmo_mac_link_down(struct phylink_config *config,
				      unsigned int mode,
				      phy_interface_t interface)
{
}

static void saline_plmo_mac_link_up(struct phylink_config *config,
				    struct phy_device *phydev,
				    unsigned int mode,
				    phy_interface_t interface,
				    int speed, int duplex,
				    bool tx_pause, bool rx_pause)
{
}

static const struct phylink_mac_ops saline_plmos = {
	.mac_get_caps = saline_plmo_mac_get_caps,
	.mac_config = saline_plmo_mac_config,
	.mac_link_down = saline_plmo_mac_link_down,
	.mac_link_up = saline_plmo_mac_link_up,
};

static int saline_phy_read_c22(struct mii_bus *bus, int port, int regnum)
{
	switch (regnum) {
	case MII_PHYSID1:
		return SALINE_MAC_PHYID >> 16;
	case MII_PHYSID2:
		return SALINE_MAC_PHYID & 0xffff;
	default:
		break;
	}

	return 0xffff;
}

static int saline_phy_write_c22(struct mii_bus *bus, int port, int regnum, u16 val)
{
	return -EOPNOTSUPP;
}

static int saline_probe_macs(struct saline *ssw)
{
	static int idx;

	struct mii_bus *bus;
	int err;

	bus = devm_mdiobus_alloc(ssw->sw.dev);
	if (!bus)
		return -ENOMEM;

	bus->priv = ssw;
	bus->name = KBUILD_MODNAME "-mdio";
	snprintf(bus->id, MII_BUS_ID_SIZE, KBUILD_MODNAME "-mdio-%d", idx++);
	bus->read = saline_phy_read_c22;
	bus->write = saline_phy_write_c22;
	bus->parent = ssw->sw.dev;
	bus->phy_mask = GENMASK(31, SALINE_MACS);

	err = devm_mdiobus_register(ssw->sw.dev, bus);
	if (err) {
		dev_err(ssw->sw.dev, "Failed to register glue MDIO bus: %pe\n",
			ERR_PTR(err));
		return err;
	}

	ssw->sw.mac_ops = &saline_plmos;
	ssw->sw.mdio = bus;
	return 0;
}

/* Generated from saline-{regs,conf}.yml
 * static const struct pasw_abi saline_pasw_abi;
 */
#include "saline-abi.c"

static int saline_probe_sw(struct saline *ssw)
{
	static const struct regmap_config regs_config = {
		.name = "pasw",
		.reg_bits = 32,
		.val_bits = 32,
		.reg_shift  = -2,
		.max_register = SZ_16K - 1,
	};

	struct dsa_chip_data *chip;

	chip = devm_kzalloc(ssw->sw.dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	*chip = (struct dsa_chip_data){
		.netdev = {
			[4] = &ssw->xdma->dev,
		},
		.port_names = {
			[0] = "e0",
			[1] = "e1",
			[2] = "e2",
			[3] = "e3",
			[4] = "cpu",
		},
	};
	ssw->sw.dev->platform_data = chip;

	ssw->sw.regs = devm_regmap_init_mmio(ssw->sw.dev, ssw->iomem, &regs_config);
	if (IS_ERR(ssw->sw.regs)) {
		dev_err(ssw->sw.dev, "Unable to create regmap: %pe", ssw->sw.regs);
		return PTR_ERR(ssw->sw.regs);
	}

	ssw->sw.abi = &saline_pasw_abi;
	ssw->sw.switch_ops = &saline_dsos;
	return pasw_init(&ssw->sw);
}

static int saline_ptp_gettime(struct pasw *sw, struct timespec64 *ts)
{
	struct saline *ssw = container_of(sw, struct saline, sw);

	return ssw->ptp_gettime(&ssw->ptp->dev, ts);
}

static void saline_tx_ts_cancel(void *data)
{
	struct saline *ssw = data;

	cancel_delayed_work_sync(&ssw->tx_ts.dwork);
}

static int saline_probe_ptp(struct saline *ssw)
{
	static const struct regmap_config tx_ts_regs_config = {
		.name = "tx_ts",
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.max_register = SZ_16K - 1,
	};

	struct pci_dev *pdev = to_pci_dev(ssw->sw.dev);
	void __iomem *tx_ts_iomem;
	struct resource res;
	const char *name;
	int err;

	tx_ts_iomem = saline_iomap_bar0(ssw, ptp_tx_ts);
	if (IS_ERR(tx_ts_iomem)) {
		dev_err(ssw->sw.dev, "Unable to remap Tx timestamp I/O memory: %pe", tx_ts_iomem);
		return PTR_ERR(tx_ts_iomem);
	}

	ssw->tx_ts.regs = devm_regmap_init_mmio(ssw->sw.dev, tx_ts_iomem, &tx_ts_regs_config);
	if (IS_ERR(ssw->tx_ts.regs)) {
		dev_err(ssw->sw.dev, "Unable to create Tx timestamp regmap: %pe", ssw->tx_ts.regs);
		return PTR_ERR(ssw->tx_ts.regs);
	}

	spin_lock_init(&ssw->tx_ts.lock);
	INIT_DELAYED_WORK(&ssw->tx_ts.dwork, saline_tx_ts_poll);

	err = devm_add_action_or_reset(ssw->sw.dev, saline_tx_ts_cancel, ssw);
	if (err)
		return err;

	/* Can't rely on auto-loading since the generated MODALIAS is
	 * based on the device name, ignoring the driver override.
	 */
	err = request_module("pg210-ptp");
	if (err) {
		dev_err(ssw->sw.dev, "Unable to load PG210 PTP timer syncer driver: %pe\n",
			ERR_PTR(err));
		return err;
	}

	res = (struct resource) {
		.parent = &pdev->resource[0],
		.flags = IORESOURCE_MEM,

		.start = pci_resource_start(pdev, 0) + offsetof(struct saline_hw_bar0, ptp_timer),
		.end   = pci_resource_start(pdev, 0) + offsetofend(struct saline_hw_bar0, ptp_timer),
	};

	name = devm_kasprintf(ssw->sw.dev, GFP_KERNEL, "%s.ptp", dev_name(ssw->sw.dev));
	if (!name)
		return -ENOMEM;

	ssw->ptp = platform_device_alloc(name, PLATFORM_DEVID_NONE);
	if (!ssw->ptp) {
		dev_err(ssw->sw.dev, "Failed to allocate PG210 PTP device\n");
		return -ENOMEM;
	}

	err = device_set_driver_override(&ssw->ptp->dev, "pg210-ptp");
	err = err ? : platform_device_add_resources(ssw->ptp, &res, 1);
	err = err ? : platform_device_add(ssw->ptp);
	if (err) {
		dev_err(ssw->sw.dev, "Failed to register PG210 PTP device: %pe\n", ERR_PTR(err));
		return err;
	}

	/* The switch core's notion of time (used for scheduled
	 * traffic) is fed from the timer. Resolve the accessor at
	 * runtime, since the module is loaded on demand.
	 */
	ssw->ptp_gettime = symbol_get(pg210_ptp_gettime);
	if (ssw->ptp_gettime)
		ssw->sw.gettime = saline_ptp_gettime;
	else
		dev_warn(ssw->sw.dev, "PG210 PTP time unavailable, scheduled traffic disabled\n");

	return 0;
}

static void saline_remove_ptp(struct saline *ssw)
{
	if (ssw->ptp_gettime)
		symbol_put(pg210_ptp_gettime);

	platform_device_unregister(ssw->ptp);
}

static int saline_probe_xdma(struct saline *ssw)
{
	struct pci_dev *pdev = to_pci_dev(ssw->sw.dev);
	struct resource res[2];
	const char *name;
	int err;

	/* Can't rely on auto-loading since the generated MODALIAS is
	 * based on the device name, ignoring the driver override.
	 */
	err = request_module("xdma-net");
	if (err) {
		dev_err(ssw->sw.dev, "Unable to load XDMA network driver: %pe\n",
			ERR_PTR(err));
		return err;
	}

	res[0] = (struct resource) {
		.parent = &pdev->resource[1],
		.flags = IORESOURCE_MEM,

		.start = pci_resource_start(pdev, 1),
		.end   = pci_resource_end(pdev, 1),
	};

	res[1] = (struct resource) {
		.flags = IORESOURCE_IRQ,

		.start = pci_irq_vector(pdev, 0),
		.end   = pci_irq_vector(pdev, 0),
	};

	name = devm_kasprintf(ssw->sw.dev, GFP_KERNEL, "%s.xdma-net", dev_name(ssw->sw.dev));
	if (!name)
		return -ENOMEM;

	ssw->xdma = platform_device_alloc(name, PLATFORM_DEVID_NONE);
	if (!ssw->xdma) {
		dev_err(ssw->sw.dev, "Failed to allocate XDMA device\n");
		return -ENOMEM;
	}

	err = device_set_driver_override(&ssw->xdma->dev, "xdma-net");
	err = err ? : platform_device_add_resources(ssw->xdma, res, 2);
	err = err ? : platform_device_add(ssw->xdma);
	if (err) {
		dev_err(ssw->sw.dev, "Failed to register XDMA netdev: %pe\n", ERR_PTR(err));
		return err;
	}

	return 0;
}

struct saline_inputclk {
	const char *subdev;
	const char *conid;
	unsigned long freq;
};

static const struct saline_inputclk saline_iclks[] = {
	{ .subdev = NULL,   .conid = "core",   .freq = 125 * HZ_PER_MHZ },
	{ .subdev = ".ptp", .conid = "ts_clk", .freq = 250 * HZ_PER_MHZ },
};

static int saline_probe_clk(struct saline *ssw)
{
	const struct saline_inputclk *iclk;
	const char *clkname, *devname;
	struct clk_hw *clkhw;
	int i, err;

	for (i = 0, iclk = saline_iclks; i < ARRAY_SIZE(saline_iclks); i++, iclk++) {
		clkname = devm_kasprintf(ssw->sw.dev, GFP_KERNEL, "%s%s-%s",
					 dev_name(ssw->sw.dev),
					 iclk->subdev ? : "",
					 iclk->conid);
		if (!clkname)
			return -ENOMEM;

		clkhw = devm_clk_hw_register_fixed_rate(ssw->sw.dev, clkname, NULL,
							0, iclk->freq);
		if (IS_ERR(clkhw)) {
			dev_err(ssw->sw.dev, "Unable to register %s clock: %pe",
				clkname, clkhw);
			return PTR_ERR(clkhw);
		}

		devname = devm_kasprintf(ssw->sw.dev, GFP_KERNEL, "%s%s",
					 dev_name(ssw->sw.dev), iclk->subdev ? : "");
		if (!devname)
			return -ENOMEM;

		err = devm_clk_hw_register_clkdev(ssw->sw.dev, clkhw,
						  iclk->conid, devname);
		if (err) {
			dev_err(ssw->sw.dev, "Unable to register %s clock connection to %s: %pe",
				iclk->conid, devname, ERR_PTR(err));
			return err;
		}
	}

	return 0;
}

static int saline_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct saline *ssw;
	int err, irqs;

	ssw = devm_kzalloc(&pdev->dev, sizeof(*ssw), GFP_KERNEL);
	if (!ssw)
		return -ENOMEM;

	ssw->sw.dev = &pdev->dev;
	pci_set_drvdata(pdev, ssw);

	err = pcim_enable_device(pdev);
	if (err)
		goto err;

	irqs = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (irqs < 0) {
		err = irqs;
		dev_err(ssw->sw.dev, "IRQ allocation failed: %pe\n", ERR_PTR(err));
		goto err;
	}

	err = dma_set_mask_and_coherent(ssw->sw.dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(ssw->sw.dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(ssw->sw.dev, "DMA configuration failed: %pe\n",
				ERR_PTR(err));
			goto err;
		}
	}

	/* ssw->bar0 = pcim_iomap_region(pdev, 0, "saline"); */
	ssw->iomem = saline_iomap_bar0(ssw, pasw);
	if (IS_ERR(ssw->iomem)) {
		dev_err(ssw->sw.dev, "Unable to remap switch I/O memory: %pe", ssw->iomem);
		err = PTR_ERR(ssw->iomem);
		goto err;
	}

	err = saline_probe_clk(ssw);
	if (err)
		goto err;

	err = saline_probe_ptp(ssw);
	if (err)
		goto err;

	err = saline_probe_xdma(ssw);
	if (err)
		goto err_remove_ptp;

	err = saline_probe_macs(ssw);
	if (err)
		goto err_remove_xdma;

	err = saline_probe_sw(ssw);
	if (err)
		goto err_remove_xdma;

	return 0;

err_remove_xdma:
	platform_device_unregister(ssw->xdma);
err_remove_ptp:
	saline_remove_ptp(ssw);
err:
	return err;
}

static void saline_remove(struct pci_dev *pdev)
{
	struct saline *ssw = pci_get_drvdata(pdev);

	platform_device_unregister(ssw->xdma);
	saline_remove_ptp(ssw);
}

static const struct pci_device_id saline_ids[] = {
    { PCI_DEVICE(0x10ee, 0x9024) },
    { PCI_DEVICE(0x10ee, 0x9034) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, saline_ids);

static struct pci_driver saline_driver = {
    .name = "pasw-saline",
    .id_table = saline_ids,
    .probe = saline_probe,
    .remove = saline_remove,
};

static int __init saline_init(void)
{
	int err;

	err = phy_drivers_register(&saline_mac_driver, 1, THIS_MODULE);
	if (err)
		return err;

	err = pci_register_driver(&saline_driver);
	if (err) {
		phy_drivers_unregister(&saline_mac_driver, 1);
		return err;
	}

	return 0;
}

static void __exit saline_exit(void)
{
    pci_unregister_driver(&saline_driver);
    phy_drivers_unregister(&saline_mac_driver, 1);
}

module_init(saline_init);
module_exit(saline_exit);
/* module_pci_driver(saline_driver); */

MODULE_DEVICE_TABLE(pci, saline_ids);
MODULE_AUTHOR("Tobias Waldekranz <tobias@waldekranz.com>");
MODULE_DESCRIPTION("Saline SmartNIC");
MODULE_LICENSE("GPL");

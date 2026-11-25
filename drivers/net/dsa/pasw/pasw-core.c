// SPDX-License-Identifier: GPL-2.0-only

#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/module.h>
#include <linux/dsa/tag_pasw.h>

#include "pasw.h"
#include "pasw-abi.h"
#include "pasw-clk.h"
#include "pasw-l2.h"
#include "pasw-vlan.h"

static int pasw_port_enter_standalone(struct pasw *sw, int port)
{
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	u16 pvid;
	int err;

	err = pasw_vlan_port_set_standalone(sw, port, &pvid);
	if (err)
		return err;

	err = pasw_row_load(&spe, sw);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->learning_en, 0);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->vlan_assignment, 1);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->default_vid, pvid);
	err = err ? : pasw_row_store(&spe, sw);
	if (err)
		return err;

	return 0;
}

static void pasw_dso_port_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct pasw *sw = ds->priv;
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	enum pasw_spt_state spt;
	int err;

	if (!pasw_field_ok(&pasw_t(sw, src_port)->spt)) {
		dev_warn_once(sw->dev, "Core does not support STP filtering\n");
		return;
	}

	switch (state) {
	case BR_STATE_DISABLED:
		spt = PASW_SPT_DISABLED;
		break;
	case BR_STATE_LISTENING:
		spt = PASW_SPT_LISTENING;
		break;
	case BR_STATE_LEARNING:
		spt = PASW_SPT_LEARNING;
		break;
	case BR_STATE_FORWARDING:
		spt = PASW_SPT_FORWARDING;
		break;
	case BR_STATE_BLOCKING:
		spt = PASW_SPT_BLOCKING;
		break;
	default:
		return;
	}

	err = pasw_row_load(&spe, sw);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->spt, spt);
	err = err ? : pasw_row_store(&spe, sw);
	if (err) {
		dev_err(sw->dev, "Failed to set STP state %u on p%d: %pe\n",
			state, port, ERR_PTR(err));
	}
}

static void pasw_dso_port_fast_age(struct dsa_switch *ds, int port)
{
	pasw_l2_flush_port(ds->priv, port, false);
}

static int pasw_dso_port_fdb_add(struct dsa_switch *ds, int port,
				 const unsigned char *addr, u16 vid,
				 struct dsa_db db)
{
	if (db.type != DSA_DB_BRIDGE)
		return 0;

	return pasw_l2_fdb_add_static(ds->priv, port, 0, addr);
}

static int pasw_dso_port_fdb_del(struct dsa_switch *ds, int port,
				 const unsigned char *addr, u16 vid,
				 struct dsa_db db)
{
	if (db.type != DSA_DB_BRIDGE)
		return 0;

	return pasw_l2_fdb_del_static(ds->priv, port, 0, addr);
}

static int pasw_dso_port_fdb_dump(struct dsa_switch *ds, int port,
				  dsa_fdb_dump_cb_t *cb, void *data)
{
	struct pasw_l2_match match = {
		.status = PASW_L2_VALID_BITS | PASW_L2_STATIC_BIT,
		.port = {
			.match = true,
			.val = port,
		}
	};
	struct pasw *sw = ds->priv;
	struct pasw_l2_iter iter;
	u8 addr[ETH_ALEN];
	int err, status;
	u64 addr64;

	pasw_l2_iter_init(sw, &iter, &match);
	while ((err = pasw_l2_iter_next(sw, &iter)) == 1) {
		status = pasw_row_geti(&iter.entry->aging, &pasw_t(sw, l2_aging)->status);

		pasw_row_get(&iter.entry->addr, &pasw_t(sw, l2_da_hash)->mac_addr, &addr64);
		u64_to_ether_addr(addr64, addr);

		err = cb(addr, 0, status == PASW_L2_STATIC, data);
		if (err)
			return err;
	}

	return err;
}

static int pasw_dso_port_pre_bridge_flags(struct dsa_switch *ds, int port,
					  struct switchdev_brport_flags flags,
					  struct netlink_ext_ack *extack)
{
	const unsigned long supported =
		BR_LEARNING |
		BR_FLOOD |
		BR_MCAST_FLOOD |
		BR_BCAST_FLOOD;
	unsigned long flag;

	if (flags.mask & ~(supported))
		return -EINVAL;

	for_each_set_bit(flag, &flags.mask, BITS_PER_LONG) {
		if (flags.val & BIT(flag))
			continue;

		switch (BIT(flag)) {
		case BR_LEARNING:
			continue;

		/* TODO: We could potentially support these using the
		 * masking bits in the L2 collision table.
		 */
		case BR_FLOOD:
			NL_SET_ERR_MSG_MOD(extack, "Unicast flooding can not be disabled");
			break;
		case BR_MCAST_FLOOD:
			NL_SET_ERR_MSG_MOD(extack, "Multicast flooding can not be disabled");
			break;
		case BR_BCAST_FLOOD:
			NL_SET_ERR_MSG_MOD(extack, "Broadcast flooding can not be disabled");
			break;
		}

		return -EINVAL;
	}

	return 0;
}

static int pasw_dso_port_bridge_flags(struct dsa_switch *ds, int port,
				      struct switchdev_brport_flags flags,
				      struct netlink_ext_ack *extack)
{
	struct pasw *sw = ds->priv;
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	u64 learning = !!(flags.val & BR_LEARNING);
	int err;

	if (!(flags.mask & BR_LEARNING))
		return 0;

	err = pasw_row_load(&spe, sw);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->learning_en, learning);
	return err ? : pasw_row_store(&spe, sw);
}

static int pasw_dso_port_bridge_join(struct dsa_switch *ds, int port,
				     struct dsa_bridge bridge,
				     bool *tx_fwd_offload,
				     struct netlink_ext_ack *extack)
{
	struct pasw *sw = ds->priv;
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	u16 pvid;
	int err;

	err = pasw_vlan_port_set_bridged(sw, port, bridge.num, &pvid);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Internal VLAN setup failed");
		return err;
	}

	err = pasw_row_load(&spe, sw);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->learning_en, 1);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->vlan_assignment, 1);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->default_vid, pvid);
	err = err ? : pasw_row_store(&spe, sw);
	if (err)
		return err;

	return 0;
}

static void pasw_dso_port_bridge_leave(struct dsa_switch *ds, int port,
				       struct dsa_bridge bridge)
{
	struct pasw *sw = ds->priv;
	int err;

	err = pasw_port_enter_standalone(sw, port);
	if (err) {
		dev_err(sw->dev, "Unable to detach p%d from %s: %pe\n",
			port, netdev_name(bridge.dev), ERR_PTR(err));
	}

	err = pasw_l2_flush_port(sw, port, true);
	if (err) {
		dev_err(sw->dev, "Unable to flush FDB during detach of p%d from %s: %pe\n",
			port, netdev_name(bridge.dev), ERR_PTR(err));
	}
}

static int pasw_dso_set_ageing_time(struct dsa_switch *ds, unsigned int msecs)
{
	struct pasw *sw = ds->priv;

	return pasw_l2_set_aging_time(sw, msecs);
}

static int pasw_dso_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct pasw *sw = ds->priv;

	return sw->switch_ops->get_sset_count ?
		sw->switch_ops->get_sset_count(ds, port, sset) : -EOPNOTSUPP;
}

static void pasw_dso_get_strings(struct dsa_switch *ds, int port,
			       u32 stringset, uint8_t *data)
{
	struct pasw *sw = ds->priv;

	return sw->switch_ops->get_strings ?
		sw->switch_ops->get_strings(ds, port, stringset, data) : -EOPNOTSUPP;
}

static void pasw_dso_get_ethtool_stats(struct dsa_switch *ds,
				       int port, uint64_t *data)
{
	struct pasw *sw = ds->priv;

	return sw->switch_ops->get_ethtool_stats ?
		sw->switch_ops->get_ethtool_stats(ds, port, data) : -EOPNOTSUPP;
}

static void pasw_dso_port_txtstamp(struct dsa_switch *ds, int port,
				   struct sk_buff *skb)
{
	struct pasw *sw = ds->priv;

	return sw->switch_ops->port_txtstamp ?
		sw->switch_ops->port_txtstamp(ds, port, skb) : -EOPNOTSUPP;
}

static int pasw_dso_port_hwtstamp_set(struct dsa_switch *ds, int port,
				      struct kernel_hwtstamp_config *config,
				      struct netlink_ext_ack *extack)
{
	struct pasw *sw = ds->priv;

	return sw->switch_ops->port_hwtstamp_set ?
		sw->switch_ops->port_hwtstamp_set(ds, port, config, extack) : -EOPNOTSUPP;
}

static int pasw_dso_port_hwtstamp_get(struct dsa_switch *ds, int port,
				      struct kernel_hwtstamp_config *config)
{
	struct pasw *sw = ds->priv;

	return sw->switch_ops->port_hwtstamp_get ?
		sw->switch_ops->port_hwtstamp_get(ds, port, config) : -EOPNOTSUPP;
}

static int pasw_dso_get_ts_info(struct dsa_switch *ds, int port,
				struct kernel_ethtool_ts_info *info)
{
	struct pasw *sw = ds->priv;

	return sw->switch_ops->get_ts_info ?
		sw->switch_ops->get_ts_info(ds, port, info) : -EOPNOTSUPP;
}

static int pasw_dso_port_enable(struct dsa_switch *ds, int port,
				struct phy_device *phy)
{
	return 0;
}

static void pasw_dso_port_disable(struct dsa_switch *ds, int port)
{
}

static int pasw_dso_port_setup(struct dsa_switch *ds, int port)
{
	if (!dsa_port_is_user(dsa_to_port(ds, port)))
		return 0;

	return pasw_port_enter_standalone(ds->priv, port);
}

static void pasw_dso_phylink_get_caps(struct dsa_switch *ds, int port,
				      struct phylink_config *config)
{
	__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
}

static int pasw_dso_setup(struct dsa_switch *ds)
{
	struct pasw *sw = ds->priv;
	int err;

	err = pasw_debugfs_setup(sw);
	err = err ? : pasw_clk_setup(sw);
	err = err ? : pasw_tc_setup(sw);
	err = err ? : pasw_vlan_setup(sw);
	err = err ? : pasw_l2_setup(sw);
	return err;
}

static enum dsa_tag_protocol pasw_dso_get_tag_protocol(struct dsa_switch *ds,
						       int port,
						       enum dsa_tag_protocol mp)
{
	struct pasw *sw = ds->priv;

	switch (sw->abi->attr.cpu_port_format) {
	case PASW_TAG_2B_PORT_QUEUE:
		return DSA_TAG_PROTO_PASW_PQ;
	default:
		break;
	};

	return DSA_TAG_PROTO_NONE;
}

static int pasw_dso_connect_tag_protocol(struct dsa_switch *ds,
					 enum dsa_tag_protocol proto)
{
	struct pasw *sw = ds->priv;

	switch (sw->abi->attr.cpu_port_format) {
	case PASW_TAG_2B_PORT_QUEUE:
		if (proto == DSA_TAG_PROTO_PASW_PQ)
			return 0;
		break;
	default:
		break;
	}

	return -EPROTONOSUPPORT;
}

static const struct dsa_switch_ops pasw_dsos = {
	.get_tag_protocol      = pasw_dso_get_tag_protocol,
	.connect_tag_protocol  = pasw_dso_connect_tag_protocol,
	.phylink_get_caps      = pasw_dso_phylink_get_caps,
	.setup                 = pasw_dso_setup,
	.port_setup            = pasw_dso_port_setup,
	.port_enable           = pasw_dso_port_enable,
	.port_disable          = pasw_dso_port_disable,
	/* Stats */
	.get_sset_count        = pasw_dso_get_sset_count,
	.get_strings           = pasw_dso_get_strings,
	.get_ethtool_stats     = pasw_dso_get_ethtool_stats,
	/* Bridge */
	.set_ageing_time       = pasw_dso_set_ageing_time,
	.port_bridge_join      = pasw_dso_port_bridge_join,
	.port_bridge_leave     = pasw_dso_port_bridge_leave,
	.port_pre_bridge_flags = pasw_dso_port_pre_bridge_flags,
	.port_bridge_flags     = pasw_dso_port_bridge_flags,
	.port_fdb_dump         = pasw_dso_port_fdb_dump,
	.port_fdb_add          = pasw_dso_port_fdb_add,
	.port_fdb_del          = pasw_dso_port_fdb_del,
/* 	.port_mdb_add          = pasw_dso_port_mdb_add, */
/* 	.port_mdb_del          = pasw_dso_port_mdb_del, */
	.port_fast_age         = pasw_dso_port_fast_age,
	.port_stp_state_set    = pasw_dso_port_stp_state_set,
/* 	/\* VLAN Bridge *\/ */
/* 	.port_vlan_add       = pasw_dso_port_vlan_add, */
/* 	.port_vlan_del       = pasw_dso_port_vlan_del, */
/* 	.port_vlan_filtering = pasw_dso_port_vlan_filtering, */
	/* PTP */
	.get_ts_info         = pasw_dso_get_ts_info,
	.port_hwtstamp_get   = pasw_dso_port_hwtstamp_get,
	.port_hwtstamp_set   = pasw_dso_port_hwtstamp_set,
/* 	.port_rxtstamp       = pasw_dso_port_rxtstamp, */
	.port_txtstamp       = pasw_dso_port_txtstamp,
	/* TC */
	.port_setup_tc       = pasw_dso_port_setup_tc,
/* 	.cls_flower_add      = pasw_dso_cls_flower_add, */
/* 	.cls_flower_del      = pasw_dso_cls_flower_del, */
/* 	.cls_flower_stats    = pasw_dso_cls_flower_stats, */
	/* DCB */
	.port_get_default_prio = pasw_dso_port_get_default_prio,
	.port_set_default_prio = pasw_dso_port_set_default_prio,
	.port_get_apptrust     = pasw_dso_port_get_apptrust,
	.port_set_apptrust     = pasw_dso_port_set_apptrust,
	.port_get_dscp_prio    = pasw_dso_port_get_dscp_prio,
	.port_add_dscp_prio    = pasw_dso_port_add_dscp_prio,
	.port_del_dscp_prio    = pasw_dso_port_del_dscp_prio,
	.port_get_pcp_prio     = pasw_dso_port_get_pcp_prio,
	.port_add_pcp_prio     = pasw_dso_port_add_pcp_prio,
	.port_del_pcp_prio     = pasw_dso_port_del_pcp_prio,
};

/* Queues are scheduled in strict priority order, with queue 0 having
 * the highest priority. Linux priorities (and traffic classes) work
 * the other way around, so that is where the two are flipped.
 */
u8 pasw_queue_from_prio(struct pasw *sw, u8 prio)
{
	u8 last = sw->abi->attr.nr_of_queues - 1;

	return last - min(prio, last);
}

u8 pasw_prio_from_queue(struct pasw *sw, u8 q)
{
	return sw->abi->attr.nr_of_queues - 1 - q;
}

int pasw_init(struct pasw *sw)
{
	int err;

	sw->ds.priv = sw;
	sw->ds.dev = sw->dev;
	sw->ds.ops = &pasw_dsos;

	sw->ds.num_ports = sw->abi->attr.nr_of_ports;
	sw->ds.num_tx_queues = sw->abi->attr.nr_of_queues;
	sw->ds.user_mii_bus = sw->mdio;
	sw->ds.phylink_mac_ops = sw->mac_ops;

	sw->ds.fdb_isolation = 1;
	sw->ds.max_num_bridges = sw->abi->attr.nr_of_l2_gids;
	sw->ds.dscp_prio_mapping_is_global = 1;
	sw->ds.pcp_prio_mapping_is_global = 1;
	sw->ds.pcp_prio_mapping_ignores_dei = 1;

	err = dsa_register_switch(&sw->ds);
	if (err && (err != -EPROBE_DEFER)) {
		dev_err(sw->dev, "Failed to register switch: %pe", ERR_PTR(err));
	}

	return err;
}
EXPORT_SYMBOL_GPL(pasw_init);

void pasw_exit(struct pasw *sw)
{

}
EXPORT_SYMBOL_GPL(pasw_exit);

static int __init pasw_module_init(void)
{
	pasw_module_init_debugfs();
	return 0;
}

static void __exit pasw_module_exit(void)
{
	pasw_module_exit_debugfs();
}

module_init(pasw_module_init);
module_exit(pasw_module_exit);

MODULE_AUTHOR("Tobias Waldekranz <tobias@waldekranz.com>");
MODULE_DESCRIPTION("Packet Architects FlexSwitch driver");
MODULE_LICENSE("GPL");

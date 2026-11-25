/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _PASW_H
#define _PASW_H

#include <net/dsa.h>

struct dentry;
struct device;
struct dsa_switch_ops;
struct mii_bus;
struct phylink_mac_ops;
struct regmap;

struct pasw_abi;
struct pasw_clks;
struct pasw_tc;
struct pasw_vlans;

struct pasw {
	struct device *dev;
	struct regmap *regs;
	const struct pasw_abi *abi;

	/* FlexSwitch is (potentially) a soft core. For maximum
	 * interoperability with various FPGAs and ASIC processes, its
	 * ports are just AXI-S interfaces, which can be attached to
	 * any MAC, DMA engine, etc.
	 *
	 * Each device is therefore responsible for providing:
	 *
	 * - DSA switch ops for MAC related callbacks (e.g. ethtool stats)
	 * - A phylink MAC implementation
	 * - An MDIO bus with a PHY device for each user port, mapped
         *   1:1 with the pasw port number
	 */
	const struct dsa_switch_ops *switch_ops;
	const struct phylink_mac_ops *mac_ops;
	struct mii_bus *mdio;

	/* Optional: Read the time of the PTP clock feeding the core,
	 * required to offload scheduled traffic (taprio).
	 */
	int (*gettime)(struct pasw *sw, struct timespec64 *ts);

	struct dsa_switch ds;
	struct dentry *debugfs;

	struct pasw_vlans *vlans;
	struct pasw_clks *clks;
	struct pasw_tc *tc;
};

/* pasw-core.c */
int pasw_init(struct pasw *sw);
void pasw_exit(struct pasw *sw);
u8 pasw_queue_from_prio(struct pasw *sw, u8 prio);
u8 pasw_prio_from_queue(struct pasw *sw, u8 q);

/* pasw-dcb.c */
int pasw_dso_port_get_pcp_prio(struct dsa_switch *ds, int port, u8 pcp, u8 dei);
int pasw_dso_port_add_pcp_prio(struct dsa_switch *ds, int port, u8 pcp, u8 dei, u8 prio);
int pasw_dso_port_del_pcp_prio(struct dsa_switch *ds, int port, u8 pcp, u8 dei, u8 prio);
int pasw_dso_port_get_dscp_prio(struct dsa_switch *ds, int port, u8 dscp);
int pasw_dso_port_add_dscp_prio(struct dsa_switch *ds, int port, u8 dscp, u8 prio);
int pasw_dso_port_del_dscp_prio(struct dsa_switch *ds, int port, u8 dscp, u8 prio);
int pasw_dso_port_get_default_prio(struct dsa_switch *ds, int port);
int pasw_dso_port_set_default_prio(struct dsa_switch *ds, int port, u8 prio);
int pasw_dso_port_set_apptrust(struct dsa_switch *ds, int port, const u8 *sel, int nsel);
int pasw_dso_port_get_apptrust(struct dsa_switch *ds, int port, u8 *sel, int *nsel);

/* pasw-tc.c */
int pasw_tc_setup(struct pasw *sw);
int pasw_dso_port_setup_tc(struct dsa_switch *ds, int port,
			   enum tc_setup_type type, void *type_data);

/* pasw-debugfs.c */
#ifdef CONFIG_DEBUG_FS
int pasw_debugfs_setup(struct pasw *sw);
int __init pasw_module_init_debugfs(void);
void __exit pasw_module_exit_debugfs(void);
#else
static inline int pasw_debugfs_setup(struct pasw *sw) { return 0; }
static inline int __init pasw_module_init_debugfs(void) { return 0; }
static inline void __exit pasw_module_exit_debugfs(void) {}
#endif	/* CONFIG_DEBUG_FS */

enum pasw_spt_state {
	PASW_SPT_DISABLED   = 0,
	PASW_SPT_BLOCKING   = 1,
	PASW_SPT_LISTENING  = 2,
	PASW_SPT_LEARNING   = 3,
	PASW_SPT_FORWARDING = 4
};

#endif	/* _PASW_H */

// SPDX-License-Identifier: GPL-2.0-only

#include <linux/netdevice.h>

#include "pasw.h"
#include "pasw-abi.h"
#include "pasw-vlan.h"

enum pasw_vlan_mode {
	PASW_VLAN_UNUSED,
	PASW_VLAN_PORT,
	PASW_VLAN_BR,
	/* PASW_VLAN_1D_BR, */
	/* PASW_VLAN_1Q_BR, */
};

struct pasw_vlan {
	enum pasw_vlan_mode mode;
	u16 vid;
	u16 refcnt;
	unsigned brnum;

	struct pasw_row ve;
};

struct pasw_vlan_port {
	struct pasw_vlan *vlan;
};

struct pasw_vlans {
	struct pasw_vlan *vlan;
	struct pasw_vlan_port *port;
};
#define pasw_vlan_foreach(_sw, _vlan)					\
	for ((_vlan) = (_sw)->vlans->vlan;				\
	     (_vlan) < &(_sw)->vlans->vlan[pasw_table(_sw, vlan)->len];	\
	     (_vlan)++)
#define pasw_vlan_foreach_reverse(_sw, _vlan)				\
	for ((_vlan) = &(_sw)->vlans->vlan[pasw_table(_sw, vlan)->len - 1]; \
	     (_vlan) >= (_sw)->vlans->vlan;				\
	     (_vlan)--)

static int pasw_vlan_flush(struct pasw *sw, struct pasw_vlan *vlan)
{
	memset(vlan->ve.word, 0, sizeof(vlan->ve.word));
	return pasw_row_store(&vlan->ve, sw);
}

static int pasw_vlan_put(struct pasw *sw, struct pasw_vlan *vlan)
{
	int err;

	if (--vlan->refcnt)
		return 0;

	err = pasw_vlan_flush(sw, vlan);
	if (err)
		return err;

	vlan->mode = PASW_VLAN_UNUSED;
	return 0;
}

static struct pasw_vlan *pasw_vlan_get(struct pasw *sw, unsigned *brnum)
{
	struct pasw_vlan *vlan;

	if (brnum) {
		pasw_vlan_foreach_reverse(sw, vlan) {
			if (vlan->mode == PASW_VLAN_BR &&
			    vlan->brnum == *brnum) {
				vlan->refcnt++;
				return vlan;
			}
		}
	}

	pasw_vlan_foreach_reverse(sw, vlan) {
		if (vlan->mode != PASW_VLAN_UNUSED)
			continue;

		vlan->mode = brnum ? PASW_VLAN_BR : PASW_VLAN_PORT;
		vlan->brnum = brnum ? *brnum : 0;
		if (brnum) {
			/* For now, the CPU is always a member of all
			 * non-VLAN-filtering bridges.
			 */
			pasw_row_mod(&vlan->ve, &pasw_t(sw, vlan)->port_mask,
				     0, BIT(sw->abi->attr.cpu_port));
		}

		vlan->refcnt++;
		return vlan;
	}

	return NULL;
}

int pasw_vlan_port_set_bridged(struct pasw *sw, int port, unsigned brnum, u16 *pvid)
{
	struct pasw_vlan_port *vp = &sw->vlans->port[port];
	struct pasw_vlan *vlan;
	int err;

	if (vp->vlan) {
		err = pasw_vlan_put(sw, vp->vlan);
		if (err)
			return err;
	}

	vlan = pasw_vlan_get(sw, &brnum);
	if (!vlan)
		return -ENOSPC;

	err = pasw_row_mod(&vlan->ve, &pasw_t(sw, vlan)->port_mask, 0, BIT(port));
	err = err ? : pasw_row_store(&vlan->ve, sw);
	if (err)
		goto err_put;

	sw->vlans->port[port].vlan = vlan;
	*pvid = vlan->vid;
	return 0;
err_put:
	pasw_vlan_put(sw, vlan);
	return err;
}

int pasw_vlan_port_set_standalone(struct pasw *sw, int port, u16 *pvid)
{
	struct pasw_vlan_port *vp = &sw->vlans->port[port];
	struct pasw_vlan *vlan;
	int err;

	if (vp->vlan) {
		err = pasw_vlan_put(sw, vp->vlan);
		if (err)
			return err;
	}

	vlan = pasw_vlan_get(sw, NULL);
	if (!vlan)
		return -ENOSPC;


	err = pasw_row_set(&vlan->ve, &pasw_t(sw, vlan)->port_mask,
			   BIT(sw->abi->attr.cpu_port) | BIT(port));
	err = err ? : pasw_row_store(&vlan->ve, sw);
	if (err)
		goto err_put;

	sw->vlans->port[port].vlan = vlan;
	*pvid = vlan->vid;
	return 0;
err_put:
	pasw_vlan_put(sw, vlan);
	return err;
}

int pasw_vlan_setup(struct pasw *sw)
{
	struct pasw_vlan *vlan;
	int err;
	u16 i;

	sw->vlans = devm_kzalloc(sw->dev, sizeof(*sw->vlans), GFP_KERNEL);
	if (!sw->vlans)
		return -ENOMEM;

	sw->vlans->vlan = devm_kzalloc(sw->dev,
				       pasw_table(sw, vlan)->len * sizeof(*sw->vlans->vlan),
				       GFP_KERNEL);
	if (!sw->vlans->vlan)
		return -ENOMEM;

	i = 0;
	pasw_vlan_foreach(sw, vlan) {
		vlan->vid = i;
		vlan->ve = (struct pasw_row) {
			.idx = i,
			.table = pasw_table(sw, vlan),
		};

		err = pasw_vlan_flush(sw, vlan);
		if (err)
			return err;

		i++;
	}

	sw->vlans->port = devm_kzalloc(sw->dev,
				       sw->abi->attr.nr_of_ports * sizeof(*sw->vlans->port),
				       GFP_KERNEL);
	if (!sw->vlans->port)
		return -ENOMEM;

	return 0;
}

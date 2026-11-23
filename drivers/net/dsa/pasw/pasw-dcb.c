#include "pasw.h"
#include "pasw-abi.h"

static int pasw_pcp_get_queue(struct pasw *sw, u8 pcp)
{
	struct pasw_row pcpe = { .idx = pcp, .table = pasw_table(sw, vlan_pcp_to_queue) };
	int err;

	if (!pasw_table_ok(pasw_table(sw, vlan_pcp_to_queue)))
		return -EOPNOTSUPP;

	err = pasw_row_load(&pcpe, sw);
	return err ? : pasw_row_geti(&pcpe, &pasw_t(sw, vlan_pcp_to_queue)->p_queue);
}

static int pasw_pcp_set_queue(struct pasw *sw, u8 pcp, u8 q)
{
	struct pasw_row pcpe = { .idx = pcp, .table = pasw_table(sw, vlan_pcp_to_queue) };
	int err;

	if (!pasw_table_ok(pasw_table(sw, vlan_pcp_to_queue)))
		return -EOPNOTSUPP;

	err = pasw_row_load(&pcpe, sw);
	err = err ? : pasw_row_set(&pcpe, &pasw_t(sw, vlan_pcp_to_queue)->p_queue, q);
	return err ? : pasw_row_store(&pcpe, sw);
}

int pasw_dso_port_get_pcp_prio(struct dsa_switch *ds, int port, u8 pcp, u8 dei)
{
	int q;

	q = pasw_pcp_get_queue(ds->priv, pcp);
	if (q < 0)
		return q;

	return pasw_prio_from_queue(ds->priv, q);
}

int pasw_dso_port_add_pcp_prio(struct dsa_switch *ds, int port, u8 pcp, u8 dei, u8 prio)
{
	return pasw_pcp_set_queue(ds->priv, pcp, pasw_queue_from_prio(ds->priv, prio));
}

int pasw_dso_port_del_pcp_prio(struct dsa_switch *ds, int port, u8 pcp, u8 dei, u8 prio)
{
	return pasw_pcp_set_queue(ds->priv, pcp, pasw_queue_from_prio(ds->priv, 0));
}

static int pasw_dscp_get_queue(struct pasw *sw, u8 dscp)
{
	struct pasw_row ip4;
	int err;

	/* DCB does not discriminate v4 from v6 mappings, so we only
	 * advertise support if we can apply the mapping to both.
	 */
	if (!pasw_table_ok(pasw_table(sw, ipv4_tos_to_queue)) ||
	    !pasw_table_ok(pasw_table(sw, ipv6_cos_to_queue)))
		return -EOPNOTSUPP;

	ip4 = (struct pasw_row) {
		.table = pasw_table(sw, ipv4_tos_to_queue),
		.idx = dscp << 2,
	};

	err = pasw_row_load(&ip4, sw);
	return err ? : pasw_row_geti(&ip4, &pasw_t(sw, ipv4_tos_to_queue)->p_queue);
}

static int pasw_dscp_set_queue(struct pasw *sw, u8 dscp, u8 q)
{
	struct pasw_row ip4, ip6;
	int err, ecn;

	if (!pasw_table_ok(pasw_table(sw, ipv4_tos_to_queue)) ||
	    !pasw_table_ok(pasw_table(sw, ipv6_cos_to_queue)))
		return -EOPNOTSUPP;

	ip4 = (struct pasw_row) { .table = pasw_table(sw, ipv4_tos_to_queue) };
	ip6 = (struct pasw_row) { .table = pasw_table(sw, ipv6_cos_to_queue) };

	/* FlexSwitch maps the full TOS/COS octet, so we roll out the
	 * selected queue to all matching ECNs, for both v4 and v6.
	 */
	for (ecn = 0; ecn < 4; ecn++) {
		ip4.idx = ip6.idx = (dscp << 2) | ecn;

		err = pasw_row_load(&ip4, sw);
		err = err ? : pasw_row_set(&ip4, &pasw_t(sw, ipv4_tos_to_queue)->p_queue, q);
		err = err ? : pasw_row_store(&ip4, sw);
		if (err)
			return err;

		err = pasw_row_load(&ip6, sw);
		err = err ? : pasw_row_set(&ip6, &pasw_t(sw, ipv6_cos_to_queue)->p_queue, q);
		err = err ? : pasw_row_store(&ip6, sw);
		if (err)
			return err;

	}
	return 0;
}

int pasw_dso_port_get_dscp_prio(struct dsa_switch *ds, int port, u8 dscp)
{
	int q;

	q = pasw_dscp_get_queue(ds->priv, dscp);
	if (q < 0)
		return q;

	return pasw_prio_from_queue(ds->priv, q);
}

int pasw_dso_port_add_dscp_prio(struct dsa_switch *ds, int port, u8 dscp, u8 prio)
{
	return pasw_dscp_set_queue(ds->priv, dscp, pasw_queue_from_prio(ds->priv, prio));
}

int pasw_dso_port_del_dscp_prio(struct dsa_switch *ds, int port, u8 dscp, u8 prio)
{
	return pasw_dscp_set_queue(ds->priv, dscp, pasw_queue_from_prio(ds->priv, 0));
}

int pasw_dso_port_get_default_prio(struct dsa_switch *ds, int port)
{
	struct pasw *sw = ds->priv;
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	int err;

	/* CAUTION: FlexSwitch does not offer a way of setting the
	 * default internal prio (queue), only the default PCP. So the
	 * "default-prio" configured by a user is mapped to the actual
	 * prio via the VLAN PCP To Queue Mapping Table.
	 */
	err = pasw_row_load(&spe, sw);
	return err ? : pasw_row_geti(&spe, &pasw_t(sw, src_port)->default_pcp);
}

int pasw_dso_port_set_default_prio(struct dsa_switch *ds, int port, u8 prio)
{
	struct pasw *sw = ds->priv;
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	int err;

	err = pasw_row_load(&spe, sw);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->default_pcp, prio);
	return err ? : pasw_row_store(&spe, sw);
}

static int pasw_port_get_priofroml3(struct pasw *sw, int port)
{
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	int err;

	err = pasw_row_load(&spe, sw);
	return err ? : pasw_row_geti(&spe, &pasw_t(sw, src_port)->prio_from_l3);
}

static int pasw_port_set_priofroml3(struct pasw *sw, int port, bool use)
{
	struct pasw_row spe = { .idx = port, .table = pasw_table(sw, src_port) };
	int err;

	if (!pasw_field_ok(&pasw_t(sw, src_port)->prio_from_l3))
		return -EOPNOTSUPP;

	err = pasw_row_load(&spe, sw);
	err = err ? : pasw_row_set(&spe, &pasw_t(sw, src_port)->prio_from_l3, use ? 1 : 0);
	return err ? : pasw_row_store(&spe, sw);
}

int pasw_dso_port_set_apptrust(struct dsa_switch *ds, int port, const u8 *sel, int nsel)
{
	switch (nsel) {
	case 2:
		if (sel[0] == IEEE_8021QAZ_APP_SEL_DSCP &&
		    sel[1] == DCB_APP_SEL_PCP)
			return pasw_port_set_priofroml3(ds->priv, port, true);
		break;
	case 1:
		if (sel[0] == DCB_APP_SEL_PCP)
			return pasw_port_set_priofroml3(ds->priv, port, false);
		break;
	}

	return -EOPNOTSUPP;
}

int pasw_dso_port_get_apptrust(struct dsa_switch *ds, int port, u8 *sel, int *nsel)
{
	int use_l3;

	use_l3 = pasw_port_get_priofroml3(ds->priv, port);
	switch (use_l3) {
	case 1:
		sel[0] = IEEE_8021QAZ_APP_SEL_DSCP;
		sel[1] = DCB_APP_SEL_PCP;
		*nsel = 2;
		break;
	case 0:
	case -EOPNOTSUPP:
		sel[0] = DCB_APP_SEL_PCP;
		*nsel = 1;
		break;
	default:
		return use_l3;
	}

	return 0;
}

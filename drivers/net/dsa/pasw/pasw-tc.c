#include <linux/math64.h>
#include <linux/units.h>
#include <net/pkt_cls.h>
#include <net/pkt_sched.h>

#include "pasw.h"
#include "pasw-abi.h"
#include "pasw-clk.h"

/* The shapers' tick rates are derived from PTP tick 0, which is set
 * up to run at 1 MHz. Each subsequent tick is 10x slower.
 */
#define PASW_TC_SHAPER_TICK_HZ	   (1 * HZ_PER_MHZ)

/* The minimum number of tokens in a bucket required for a queue to
 * be eligible for transmission. As there is no notion of negative
 * credit, this must leave enough room for the largest frame to be
 * deducted from it. The bucket's capacity is then set relative to
 * this, according to the configured burst size/hicredit.
 */
#define PASW_TC_SHAPER_THRESHOLD   12500

/* Rounding to a whole number of tokens per tick is accurate to within
 * 1% from here.
 */
#define PASW_TC_SHAPER_MIN_TOKENS  50

/* Default IFG correction, i.e. FCS + preamble + IFG */
#define PASW_TC_SHAPER_IFG_DEFAULT 24

/* Minimum gate control list entry interval, as per the datasheet */
#define PASW_TC_GATE_MIN_INTERVAL_NS 328

#define PASW_TC_GATE_MARGIN_NS	   (100 * NSEC_PER_MSEC)

struct pasw_tc_gcl {
	u16 start;
	u16 len;
};

struct pasw_tc_sched {
	struct pasw_tc_gcl cur;

	/* The schedule which was replaced by cur. It remains
	 * operational until the base time of cur is reached, so its
	 * list entries may not be reused before then.
	 */
	struct pasw_tc_gcl prev;
};

enum pasw_tc_shaper_type {
	PASW_TC_SHAPER_NONE,
	PASW_TC_SHAPER_CBS,
	PASW_TC_SHAPER_TBF,
};

struct pasw_tc {
	/* Bucket capacity and threshold are global, so all active
	 * shapers have to agree on the burst size.
	 */
	u32 burst;
	unsigned int nr_shapers;

	/* Indexed by port * nr_of_queues + queue, NULL if the core
	 * has no queue shapers.
	 */
	enum pasw_tc_shaper_type *shaper;

	/* Gate control list entries are allocated from a memory that
	 * is shared by all ports. NULL if the core has no gates.
	 */
	unsigned long *gcl_used;
	struct pasw_tc_sched *sched;
};

/* TXQ N is always TC N (enforced below), and TC N always uses the
 * same hardware queue as priority N.
 */
static int pasw_tc_mqprio_validate(struct pasw *sw,
				   const struct tc_mqprio_qopt_offload *mqprio,
				   struct netlink_ext_ack *extack)
{
	const struct tc_mqprio_qopt *qopt = &mqprio->qopt;
	int tc;

	if (qopt->num_tc > sw->abi->attr.nr_of_queues) {
		NL_SET_ERR_MSG_FMT_MOD(extack, "At most %u traffic classes are supported",
				       sw->abi->attr.nr_of_queues);
		return -EOPNOTSUPP;
	}

	if ((mqprio->flags & TC_MQPRIO_F_SHAPER) &&
	    mqprio->shaper != TC_MQPRIO_SHAPER_DCB) {
		NL_SET_ERR_MSG_MOD(extack, "Rate limiting traffic classes is not supported");
		return -EOPNOTSUPP;
	}

	if (mqprio->preemptible_tcs) {
		NL_SET_ERR_MSG_MOD(extack, "Frame preemption is not supported");
		return -EOPNOTSUPP;
	}

	for (tc = 0; tc < qopt->num_tc; tc++) {
		if (qopt->count[tc] != 1 || qopt->offset[tc] != tc) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Each traffic class must map to exactly one TXQ, "
					   "with TC N mapped to TXQ N (queues 1@0 1@1 ...)");
			return -EOPNOTSUPP;
		}
	}

	return 0;
}

static int pasw_tc_mqprio(struct pasw *sw, int port,
			  struct tc_mqprio_qopt_offload *mqprio)
{
	struct net_device *user = dsa_to_port(&sw->ds, port)->user;
	struct tc_mqprio_qopt *qopt = &mqprio->qopt;
	int err, tc;

	if (!qopt->num_tc) {
		netdev_reset_tc(user);
		return 0;
	}

	err = pasw_tc_mqprio_validate(sw, mqprio, mqprio->extack);
	if (err)
		return err;

	/* The hardware's queue assignment is fixed, so there is
	 * nothing to configure beyond letting the stack know about
	 * the TC layout. The tagger then uses that to select the
	 * corresponding queue for locally originated traffic.
	 */
	err = netdev_set_num_tc(user, qopt->num_tc);
	if (err)
		return err;

	for (tc = 0; tc < qopt->num_tc; tc++) {
		err = netdev_set_tc_queue(user, tc, 1, tc);
		if (err)
			goto err_reset;
	}

	qopt->hw = TC_MQPRIO_HW_OFFLOAD_TCS;
	return 0;

err_reset:
	netdev_reset_tc(user);
	return err;
}

/* Neither cbs nor tbf provide an extack to offloading drivers, and
 * tbf does not even propagate errors, so make sure that the user has
 * some way of discovering the reason for a failure.
 */
static void pasw_tc_report(struct pasw *sw, int port, const char *qdisc,
			   const struct netlink_ext_ack *extack, int err)
{
	if (!err)
		return;

	netdev_err(dsa_to_port(&sw->ds, port)->user, "Unable to offload %s: %s (%pe)\n",
		   qdisc, extack->_msg ? : "", ERR_PTR(err));
}

static u32 pasw_tc_shaper_idx(struct pasw *sw, int port, u8 txq)
{
	return port * sw->abi->attr.nr_of_queues + pasw_queue_from_prio(sw, txq);
}

static enum pasw_tc_shaper_type *pasw_tc_shaper_find(struct pasw *sw, int port, u8 txq)
{
	if (!sw->tc->shaper || txq >= sw->abi->attr.nr_of_queues)
		return NULL;

	return &sw->tc->shaper[pasw_tc_shaper_idx(sw, port, txq)];
}

static int pasw_tc_shaper_enable(struct pasw *sw, u32 idx, bool enable)
{
	struct pasw_row en = { .table = pasw_table(sw, shaper_en) };
	int err;

	err = pasw_row_load(&en, sw);
	err = err ? : pasw_row_mod(&en, &pasw_t(sw, shaper_en)->enable,
				   BIT_ULL(idx), enable ? BIT_ULL(idx) : 0);
	return err ? : pasw_row_store(&en, sw);
}

/* A tbf bucket keeps its tokens while the queue is empty, so use the
 * slowest tick that does not overflow the bucket between two ticks,
 * as that yields the best resolution. A cbs bucket is reset to the
 * threshold, which discards up to one tick's worth of tokens, so use
 * the fastest tick that is still accurate to within 1%.
 */
static int pasw_tc_shaper_tick(struct pasw *sw, enum pasw_tc_shaper_type type,
			       u64 rate, u32 burst, u8 *tick, u32 *tokens)
{
	const struct pasw_field *tokf = &pasw_t(sw, shaper_rate)->tokens;
	int i, n = sw->abi->attr.nr_of_ptp_ticks, sel = -1;
	bool cbs = type == PASW_TC_SHAPER_CBS;
	unsigned long tick_rate;
	u64 tok;

	for (i = cbs ? 0 : n - 1; i >= 0 && i < n; i += cbs ? 1 : -1) {
		tick_rate = pasw_tick_rate(pasw_tick_get(sw, PASW_PTP_TICK, i));
		if (!tick_rate)
			continue;

		tok = DIV64_U64_ROUND_CLOSEST(rate, tick_rate);
		if (!tok || !pasw_field_fits(tokf, tok))
			continue;

		sel = i;
		*tokens = tok;
		if (cbs ? tok >= PASW_TC_SHAPER_MIN_TOKENS : tok <= max(burst, 1U))
			break;
	}

	if (sel < 0)
		return -ERANGE;

	*tick = sel;
	return 0;
}

static int pasw_tc_shaper_set(struct pasw *sw, int port, u8 txq,
			      enum pasw_tc_shaper_type type, u64 rate, u32 burst,
			      u8 ifg, struct netlink_ext_ack *extack)
{
	const struct pasw_t_shaper_rate *t = pasw_t(sw, shaper_rate);
	struct pasw_row rr = { .table = &t->table };
	enum pasw_tc_shaper_type *shaper;
	unsigned int others;
	u32 tokens;
	u8 tick;
	int err;

	if (!sw->tc->shaper) {
		NL_SET_ERR_MSG_MOD(extack, "Core does not support queue shapers");
		return -EOPNOTSUPP;
	}

	if (txq >= sw->abi->attr.nr_of_queues) {
		NL_SET_ERR_MSG_FMT_MOD(extack, "Only TXQs 0-%u can be shaped",
				       sw->abi->attr.nr_of_queues - 1);
		return -EOPNOTSUPP;
	}

	rr.idx = pasw_tc_shaper_idx(sw, port, txq);
	shaper = &sw->tc->shaper[rr.idx];

	others = sw->tc->nr_shapers - (*shaper != PASW_TC_SHAPER_NONE);
	if (others && burst != sw->tc->burst) {
		NL_SET_ERR_MSG_FMT_MOD(extack, "Burst/hicredit must match other shapers' (%u)",
				       sw->tc->burst);
		return -EOPNOTSUPP;
	}

	if (!pasw_field_fits(&sw->abi->var.shaper_capacity.field,
			     PASW_TC_SHAPER_THRESHOLD + (u64)burst)) {
		NL_SET_ERR_MSG_MOD(extack, "Burst/hicredit is too large");
		return -ERANGE;
	}

	if (!pasw_field_fits(&t->ifg_correction, ifg)) {
		NL_SET_ERR_MSG_MOD(extack, "Overhead is too large");
		return -ERANGE;
	}

	err = pasw_tc_shaper_tick(sw, type, rate, burst, &tick, &tokens);
	if (err) {
		NL_SET_ERR_MSG_FMT_MOD(extack, "Unable to represent a rate of %llu B/s",
				       rate);
		return err;
	}

	err = pasw_row_set(&rr, &t->tokens, tokens);
	err = err ? : pasw_row_set(&rr, &t->tick, tick);
	err = err ? : pasw_row_set(&rr, &t->ifg_correction, ifg);
	err = err ? : pasw_row_set(&rr, &t->avb, type == PASW_TC_SHAPER_CBS);
	err = err ? : pasw_row_store(&rr, sw);
	if (!err && burst != sw->tc->burst)
		err = pasw_var_set(sw, &sw->abi->var.shaper_capacity,
				   PASW_TC_SHAPER_THRESHOLD + burst);
	err = err ? : pasw_tc_shaper_enable(sw, rr.idx, true);
	if (err)
		return err;

	if (*shaper == PASW_TC_SHAPER_NONE)
		sw->tc->nr_shapers++;

	*shaper = type;
	sw->tc->burst = burst;
	return 0;
}

static int pasw_tc_shaper_clr(struct pasw *sw, int port, u8 txq)
{
	enum pasw_tc_shaper_type *shaper = pasw_tc_shaper_find(sw, port, txq);
	int err;

	if (!shaper || *shaper == PASW_TC_SHAPER_NONE)
		return 0;

	err = pasw_tc_shaper_enable(sw, shaper - sw->tc->shaper, false);
	if (err)
		return err;

	*shaper = PASW_TC_SHAPER_NONE;
	sw->tc->nr_shapers--;
	return 0;
}

/* AVB mode implements cbs, with idleslope as the rate and hicredit as
 * the capacity. locredit and sendslope follow from the port rate.
 */
static int pasw_tc_cbs(struct pasw *sw, int port, struct tc_cbs_qopt_offload *cbs)
{
	struct netlink_ext_ack extack = {};
	int err;

	if (cbs->queue < 0)
		return -EINVAL;

	if (!cbs->enable)
		return pasw_tc_shaper_clr(sw, port, cbs->queue);

	if (cbs->idleslope <= 0 || cbs->hicredit < 0) {
		NL_SET_ERR_MSG_MOD(&extack, "idleslope must be positive, and hicredit non-negative");
		err = -EINVAL;
	} else {
		err = pasw_tc_shaper_set(sw, port, cbs->queue, PASW_TC_SHAPER_CBS,
					 (u64)cbs->idleslope * 1000 / 8, cbs->hicredit,
					 PASW_TC_SHAPER_IFG_DEFAULT, &extack);
	}

	pasw_tc_report(sw, port, "cbs", &extack, err);
	return err;
}

/* tbf only accounts for the frame itself, plus the configured overhead */
static int pasw_tc_tbf_replace(struct pasw *sw, int port, u8 txq,
			       const struct tc_tbf_qopt_offload_replace_params *p)
{
	struct netlink_ext_ack extack = {};
	int err;

	if (p->rate.overhead < 0 || p->rate.mpu ||
	    p->rate.linklayer != TC_LINKLAYER_ETHERNET) {
		NL_SET_ERR_MSG_MOD(&extack, "Only overhead adjustments are supported");
		err = -EOPNOTSUPP;
	} else {
		err = pasw_tc_shaper_set(sw, port, txq, PASW_TC_SHAPER_TBF,
					 p->rate.rate_bytes_ps, p->max_size,
					 p->rate.overhead, &extack);
	}

	pasw_tc_report(sw, port, "tbf", &extack, err);
	return err;
}

static int pasw_tc_tbf(struct pasw *sw, int port, struct tc_tbf_qopt_offload *tbf)
{
	enum pasw_tc_shaper_type *shaper;
	u8 txq;

	if (tbf->parent == TC_H_ROOT || !TC_H_MIN(tbf->parent))
		return -EOPNOTSUPP;

	txq = TC_H_MIN(tbf->parent) - 1;

	switch (tbf->command) {
	case TC_TBF_REPLACE:
		return pasw_tc_tbf_replace(sw, port, txq, &tbf->replace_params);
	case TC_TBF_DESTROY:
		return pasw_tc_shaper_clr(sw, port, txq);
	case TC_TBF_STATS:
		/* No stats to report, but this marks the qdisc as
		 * offloaded.
		 */
		shaper = pasw_tc_shaper_find(sw, port, txq);
		if (shaper && *shaper == PASW_TC_SHAPER_TBF)
			return 0;
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static u32 pasw_tc_gcl_size(struct pasw *sw)
{
	return pasw_table(sw, egress_gate_list)->len;
}

static int pasw_tc_gcl_alloc(struct pasw *sw, u16 len, struct pasw_tc_gcl *gcl)
{
	u32 size = pasw_tc_gcl_size(sw);
	unsigned long start;

	start = bitmap_find_next_zero_area(sw->tc->gcl_used, size, 0, len, 0);
	if (start >= size)
		return -ENOSPC;

	bitmap_set(sw->tc->gcl_used, start, len);
	*gcl = (struct pasw_tc_gcl) { .start = start, .len = len };
	return 0;
}

static void pasw_tc_gcl_free(struct pasw *sw, struct pasw_tc_gcl *gcl)
{
	bitmap_clear(sw->tc->gcl_used, gcl->start, gcl->len);
	*gcl = (struct pasw_tc_gcl) {};
}

static int pasw_tc_gate_enable(struct pasw *sw, int port, bool enable)
{
	struct pasw_row en = { .table = pasw_table(sw, egress_gate_en), .idx = port };
	int err;

	err = pasw_row_set(&en, &pasw_t(sw, egress_gate_en)->enabled, enable);
	return err ? : pasw_row_store(&en, sw);
}

static int pasw_tc_gate_pending(struct pasw *sw, int port)
{
	u32 pending;
	int err;

	err = pasw_var_get(sw, &sw->abi->var.egress_gate_pending, &pending);
	return err ? : !!(pending & BIT(port));
}

static int pasw_tc_row_set_time(struct pasw_row *row, const struct pasw_field *sec,
				const struct pasw_field *ns, u64 time)
{
	u32 rem;
	u64 s;
	int err;

	s = div_u64_rem(time, NSEC_PER_SEC, &rem);

	err = pasw_row_set(row, sec, s);
	return err ? : pasw_row_set(row, ns, rem);
}

/* Push the base time forward by an integer number of cycles if it
 * would otherwise expire before the schedule has been handed to the
 * hardware. Software is responsible for this, as the hardware does
 * not implement SetConfigChangeTime() from 802.1Q.
 */
static int pasw_tc_taprio_base_time(struct pasw *sw, ktime_t base_time, u64 cycle,
				    u64 *out)
{
	u64 base = max_t(s64, base_time, 0);
	struct timespec64 now;
	u64 min;
	int err;

	err = sw->gettime(sw, &now);
	if (err)
		return err;

	min = timespec64_to_ns(&now) + PASW_TC_GATE_MARGIN_NS;
	if (base < min)
		base += DIV64_U64_ROUND_UP(min - base, cycle) * cycle;

	*out = base;
	return 0;
}

static u32 pasw_tc_taprio_closed(struct pasw *sw, u32 gate_mask)
{
	u32 closed = 0;
	u8 q;

	for (q = 0; q < sw->abi->attr.nr_of_queues; q++) {
		if (!(gate_mask & BIT(pasw_prio_from_queue(sw, q))))
			closed |= BIT(q);
	}

	return closed;
}

static int pasw_tc_taprio_validate(struct pasw *sw, const struct tc_taprio_qopt_offload *qopt)
{
	struct netlink_ext_ack *extack = qopt->extack;
	size_t i;
	int err;

	err = pasw_tc_mqprio_validate(sw, &qopt->mqprio, extack);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(qopt->max_sdu); i++) {
		if (qopt->max_sdu[i]) {
			NL_SET_ERR_MSG_MOD(extack, "queueMaxSDU is not supported");
			return -EOPNOTSUPP;
		}
	}

	if (!qopt->cycle_time) {
		NL_SET_ERR_MSG_MOD(extack, "Cycle time must be non-zero");
		return -EINVAL;
	}

	if (!qopt->num_entries || qopt->num_entries > pasw_tc_gcl_size(sw) ||
	    !pasw_field_fits(&pasw_t(sw, egress_gate_cfg)->list_len, qopt->num_entries)) {
		NL_SET_ERR_MSG_FMT_MOD(extack, "Schedules must contain 1-%u entries",
				       pasw_tc_gcl_size(sw));
		return -EOPNOTSUPP;
	}

	for (i = 0; i < qopt->num_entries; i++) {
		if (qopt->entries[i].command != TC_TAPRIO_CMD_SET_GATES) {
			NL_SET_ERR_MSG_MOD(extack, "Only SetGateStates operations are supported");
			return -EOPNOTSUPP;
		}

		if (qopt->entries[i].interval <= PASW_TC_GATE_MIN_INTERVAL_NS) {
			NL_SET_ERR_MSG_FMT_MOD(extack, "Intervals must be longer than %u ns",
					       PASW_TC_GATE_MIN_INTERVAL_NS);
			return -EOPNOTSUPP;
		}
	}

	return 0;
}

static int pasw_tc_taprio_write(struct pasw *sw, int port,
				const struct tc_taprio_qopt_offload *qopt,
				const struct pasw_tc_gcl *gcl, u64 base)
{
	const struct pasw_t_egress_gate_list *tl = pasw_t(sw, egress_gate_list);
	const struct pasw_t_egress_gate_cfg *tc = pasw_t(sw, egress_gate_cfg);
	struct pasw_row cfg = { .table = &tc->table, .idx = port };
	struct pasw_row upd = { .table = pasw_table(sw, egress_gate_update), .idx = port };
	struct pasw_row ent = { .table = &tl->table };
	size_t i;
	int err;

	/* Sequence according to the datasheet: enable, configure,
	 * write the list, and finally request the update.
	 */
	err = pasw_tc_gate_enable(sw, port, true);
	if (err)
		return err;

	err = pasw_tc_row_set_time(&cfg, &tc->base_time_sec, &tc->base_time_ns, base);
	err = err ? : pasw_tc_row_set_time(&cfg, &tc->cycle_time_sec, &tc->cycle_time_ns,
					   qopt->cycle_time);
	err = err ? : pasw_tc_row_set_time(&cfg, &tc->cycle_time_ext_sec, &tc->cycle_time_ext_ns,
					   qopt->cycle_time_extension);
	err = err ? : pasw_row_set(&cfg, &tc->list_len, gcl->len);
	err = err ? : pasw_row_set(&cfg, &tc->start_addr, gcl->start);
	err = err ? : pasw_row_store(&cfg, sw);
	if (err)
		return err;

	for (i = 0; i < gcl->len; i++) {
		const struct tc_taprio_sched_entry *e = &qopt->entries[i];

		ent.idx = gcl->start + i;
		memset(ent.word, 0, sizeof(ent.word));
		err = pasw_row_set(&ent, &tl->disable_queue_mask,
				   pasw_tc_taprio_closed(sw, e->gate_mask));
		err = err ? : pasw_tc_row_set_time(&ent, &tl->interval_sec, &tl->interval_ns,
						   e->interval);
		err = err ? : pasw_row_store(&ent, sw);
		if (err)
			return err;
	}

	err = pasw_row_set(&upd, &pasw_t(sw, egress_gate_update)->start, 1);
	return err ? : pasw_row_store(&upd, sw);
}

static int pasw_tc_taprio_replace(struct pasw *sw, int port,
				  struct tc_taprio_qopt_offload *qopt)
{
	struct pasw_tc_sched *sched = &sw->tc->sched[port];
	struct netlink_ext_ack *extack = qopt->extack;
	struct pasw_tc_gcl gcl;
	u64 base;
	int err;

	if (!sw->tc->sched) {
		NL_SET_ERR_MSG_MOD(extack, "Core does not support transmission gates");
		return -EOPNOTSUPP;
	}

	if (!sw->gettime) {
		NL_SET_ERR_MSG_MOD(extack, "No time source available");
		return -EOPNOTSUPP;
	}

	err = pasw_tc_taprio_validate(sw, qopt);
	if (err)
		return err;

	err = pasw_tc_gate_pending(sw, port);
	if (err < 0)
		return err;
	if (err) {
		NL_SET_ERR_MSG_MOD(extack,
				   "The previous schedule has not yet been activated, "
				   "retry after its base time has passed");
		return -EBUSY;
	}

	pasw_tc_gcl_free(sw, &sched->prev);

	err = pasw_tc_gcl_alloc(sw, qopt->num_entries, &gcl);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Not enough free gate control list entries");
		return err;
	}

	err = pasw_tc_taprio_base_time(sw, qopt->base_time, qopt->cycle_time, &base);
	err = err ? : pasw_tc_taprio_write(sw, port, qopt, &gcl, base);
	if (err) {
		pasw_tc_gcl_free(sw, &gcl);
		return err;
	}

	sched->prev = sched->cur;
	sched->cur = gcl;
	return 0;
}

static int pasw_tc_taprio_destroy(struct pasw *sw, int port)
{
	struct pasw_tc_sched *sched = &sw->tc->sched[port];
	int err;

	if (!sw->tc->sched)
		return 0;

	/* With the gate disabled, all queues are open. */
	err = pasw_tc_gate_enable(sw, port, false);
	if (err)
		return err;

	pasw_tc_gcl_free(sw, &sched->cur);
	pasw_tc_gcl_free(sw, &sched->prev);
	return 0;
}

static int pasw_tc_taprio(struct pasw *sw, int port, struct tc_taprio_qopt_offload *qopt)
{
	switch (qopt->cmd) {
	case TAPRIO_CMD_REPLACE:
		return pasw_tc_taprio_replace(sw, port, qopt);
	case TAPRIO_CMD_DESTROY:
		return pasw_tc_taprio_destroy(sw, port);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int pasw_tc_query_caps(struct pasw *sw, struct tc_query_caps_base *base)
{
	struct tc_mqprio_caps *caps = base->caps;

	if (base->type != TC_SETUP_QDISC_MQPRIO)
		return -EOPNOTSUPP;

	caps->validate_queue_counts = true;
	return 0;
}

int pasw_dso_port_setup_tc(struct dsa_switch *ds, int port,
			   enum tc_setup_type type, void *type_data)
{
	struct pasw *sw = ds->priv;

	switch (type) {
	case TC_QUERY_CAPS:
		return pasw_tc_query_caps(sw, type_data);
	case TC_SETUP_QDISC_MQPRIO:
		return pasw_tc_mqprio(sw, port, type_data);
	case TC_SETUP_QDISC_CBS:
		return pasw_tc_cbs(sw, port, type_data);
	case TC_SETUP_QDISC_TBF:
		return pasw_tc_tbf(sw, port, type_data);
	case TC_SETUP_QDISC_TAPRIO:
		return pasw_tc_taprio(sw, port, type_data);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int pasw_tc_setup_shapers(struct pasw *sw)
{
	struct pasw_tick *tick = pasw_tick_get(sw, PASW_PTP_TICK, 0);
	struct pasw_row en = { .table = pasw_table(sw, shaper_en) };
	int err;

	if (!tick ||
	    !pasw_var_ok(&sw->abi->var.shaper_capacity) ||
	    !pasw_var_ok(&sw->abi->var.shaper_threshold) ||
	    !pasw_table_ok(pasw_table(sw, shaper_en)) ||
	    !pasw_table_ok(pasw_table(sw, shaper_rate)))
		return 0;

	err = pasw_tick_rate_set(tick, PASW_TC_SHAPER_TICK_HZ);
	if (err) {
		dev_err(sw->dev, "Unable to set shaper tick rate: %pe\n", ERR_PTR(err));
		return err;
	}

	err = pasw_var_set(sw, &sw->abi->var.shaper_threshold, PASW_TC_SHAPER_THRESHOLD);
	err = err ? : pasw_var_set(sw, &sw->abi->var.shaper_capacity, PASW_TC_SHAPER_THRESHOLD);
	err = err ? : pasw_row_store(&en, sw);
	if (err)
		return err;

	sw->tc->shaper = devm_kcalloc(sw->dev, sw->ds.num_ports * sw->abi->attr.nr_of_queues,
				      sizeof(*sw->tc->shaper), GFP_KERNEL);
	return sw->tc->shaper ? 0 : -ENOMEM;
}

static int pasw_tc_setup_gates(struct pasw *sw)
{
	int err, port;

	if (!pasw_var_ok(&sw->abi->var.egress_gate_pending) ||
	    !pasw_table_ok(pasw_table(sw, egress_gate_cfg)) ||
	    !pasw_table_ok(pasw_table(sw, egress_gate_en)) ||
	    !pasw_table_ok(pasw_table(sw, egress_gate_list)) ||
	    !pasw_table_ok(pasw_table(sw, egress_gate_update)))
		return 0;

	for (port = 0; port < sw->ds.num_ports; port++) {
		err = pasw_tc_gate_enable(sw, port, false);
		if (err)
			return err;
	}

	sw->tc->gcl_used = devm_bitmap_zalloc(sw->dev, pasw_tc_gcl_size(sw), GFP_KERNEL);
	if (!sw->tc->gcl_used)
		return -ENOMEM;

	sw->tc->sched = devm_kcalloc(sw->dev, sw->ds.num_ports, sizeof(*sw->tc->sched),
				     GFP_KERNEL);
	return sw->tc->sched ? 0 : -ENOMEM;
}

int pasw_tc_setup(struct pasw *sw)
{
	int err;

	sw->tc = devm_kzalloc(sw->dev, sizeof(*sw->tc), GFP_KERNEL);
	if (!sw->tc)
		return -ENOMEM;

	err = pasw_tc_setup_shapers(sw);
	return err ? : pasw_tc_setup_gates(sw);
}

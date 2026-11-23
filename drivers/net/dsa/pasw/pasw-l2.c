#include <linux/etherdevice.h>
#include <linux/if_bridge.h>

#include "pasw.h"
#include "pasw-abi.h"
#include "pasw-clk.h"
#include "pasw-l2.h"

void pasw_l2_iter_init(struct pasw *sw, struct pasw_l2_iter *iter,
		       const struct pasw_l2_match *match)
{
	*iter = (struct pasw_l2_iter) {
		.next_idx = 0,

		.hash = {
			.aging      = { .table = pasw_table(sw, l2_aging)      },
			.aging_stat = { .table = pasw_table(sw, l2_aging_stat) },
			.addr       = { .table = pasw_table(sw, l2_da_hash)    },
		},
		.coll = {
			.aging      = { .table = pasw_table(sw, l2_coll_aging)      },
			.aging_stat = { .table = pasw_table(sw, l2_coll_aging_stat) },
			.addr       = { .table = pasw_table(sw, l2_coll)            },
		},

		.entry = &iter->hash,

		.dst   = { .table = pasw_table(sw, l2_dst) },

		.match = match,
	};
}

static bool pasw_l2_iter_next_idx(struct pasw *sw, struct pasw_l2_iter *iter)
{
	u32 idx = iter->next_idx++;

	iter->dst.idx = idx;

	if (idx < pasw_table(sw, l2_da_hash)->len) {
		iter->hash.aging.idx = iter->hash.aging_stat.idx = iter->hash.addr.idx = idx;
		iter->entry = &iter->hash;
		return true;
	}

	idx -= pasw_table(sw, l2_da_hash)->len;
	if (idx < pasw_table(sw, l2_coll)->len) {
		iter->coll.aging.idx = iter->coll.aging_stat.idx = iter->coll.addr.idx = idx;
		iter->entry = &iter->coll;
		return true;
	}

	return false;
}

int pasw_l2_iter_next(struct pasw *sw, struct pasw_l2_iter *iter)
{
	int err, status;
	u32 hash, way;

	while (pasw_l2_iter_next_idx(sw, iter)) {
		if (iter->entry == &iter->hash) {
			pasw_table_hashway(pasw_table(sw, l2_da_hash),
					   iter->dst.idx, &hash, &way);

			if (iter->match->hash.match && hash != iter->match->hash.val)
				continue;
			if (iter->match->way.match && way != iter->match->way.val)
				continue;
		}

		err = pasw_row_load(&iter->entry->aging, sw);
		if (err)
			return err;

		status = pasw_row_geti(&iter->entry->aging, &pasw_t(sw, l2_aging)->status);
		if (!(BIT(status) & iter->match->status))
			continue;

		err = pasw_row_load(&iter->dst, sw);
		if (err)
			return err;

		/* TODO: Lookup portmask in multicast table when !uc */
		if (iter->match->port.match &&
		    (!pasw_row_geti(&iter->dst, &pasw_t(sw, l2_dst)->uc) ||
		     (pasw_row_geti(&iter->dst, &pasw_t(sw, l2_dst)->dst)
		      != iter->match->port.val)))
			continue;

		err = pasw_row_load(&iter->entry->addr, sw);
		err = err ? : pasw_row_load(&iter->entry->aging_stat, sw);
		if (err)
			return err;

		return 1;
	}

	return 0;
}

static int pasw_l2_iter_store_invalidate(struct pasw *sw, struct pasw_l2_iter *iter)
{
	int err;

	err = pasw_row_set(&iter->entry->aging, &pasw_t(sw, l2_aging)->status, PASW_L2_INVALID);
	err = err ? : pasw_row_set(&iter->entry->aging_stat, &pasw_t(sw, l2_aging_stat)->valid, 0);

	err = err ? : pasw_row_store(&iter->entry->aging, sw);
	err = err ? : pasw_row_store(&iter->entry->aging_stat, sw);
	return err;
}

static int pasw_l2_iter_store(struct pasw *sw, struct pasw_l2_iter *iter)
{
	int err;

	err = pasw_row_store(&iter->dst, sw);
	err = err ? : pasw_row_store(&iter->entry->addr, sw);
	err = err ? : pasw_row_store(&iter->entry->aging, sw);
	err = err ? : pasw_row_store(&iter->entry->aging_stat, sw);
	return err;
}

static int pasw_l2_set_hw_learn_age(struct pasw *sw, bool enable)
{
	int err;

	err = pasw_var_set(sw, &sw->abi->var.l2_aging, enable ? 1 : 0);
	err = err ? : pasw_var_set(sw, &sw->abi->var.l2_learning, enable ? 1 : 0);
	err = err ? : pasw_var_set(sw, &sw->abi->var.l2_da_hit, enable ? 1 : 0);
	return err;
}

static int pasw_l2_flush(struct pasw *sw, const struct pasw_l2_match *match)
{
	struct pasw_l2_iter iter;
	int err;

	err = pasw_l2_set_hw_learn_age(sw, false);
	if (err)
		return err;

	pasw_l2_iter_init(sw, &iter, match);
	while ((err = pasw_l2_iter_next(sw, &iter)) == 1) {
		err = pasw_l2_iter_store_invalidate(sw, &iter);
		if (err)
			break;
	}

	if (err)
		goto err_reenable;

	return pasw_l2_set_hw_learn_age(sw, true);

err_reenable:
	pasw_l2_set_hw_learn_age(sw, true);
	return err;
}

int pasw_l2_flush_port(struct pasw *sw, int port, bool inc_static)
{
	struct pasw_l2_match match = {
		.status = PASW_L2_VALID_BITS | (inc_static ? PASW_L2_STATIC_BIT : 0),
		.port = {
			.match = true,
			.val = port,
		}
	};

	return pasw_l2_flush(sw, &match);
}

static int pasw_l2_flush_all(struct pasw *sw)
{
	struct pasw_l2_match match = {
		.status = PASW_L2_VALID_BITS | PASW_L2_STATIC_BIT,
	};

	return pasw_l2_flush(sw, &match);
}

static int __pasw_l2_fdb_del(struct pasw *sw, struct pasw_l2_match *match,
			     u16 gid, u64 addr)
{
	struct pasw_l2_iter iter;
	int cgid, err;
	u64 caddr;

	match->hash.match = true;
	match->hash.val = pasw_table_hash(pasw_table(sw, l2_da_hash),
					  addr | ((u64)gid << 48), 49),

	pasw_l2_iter_init(sw, &iter, match);
	while ((err = pasw_l2_iter_next(sw, &iter)) == 1) {
		cgid = pasw_row_geti(&iter.entry->addr, &pasw_t(sw, l2_da_hash)->gid);
		if (cgid != gid)
			continue;

		pasw_row_get(&iter.entry->addr,
			     &pasw_t(sw, l2_da_hash)->mac_addr, &caddr);
		if (caddr != addr)
			continue;

		return pasw_l2_iter_store_invalidate(sw, &iter);
	}

	return err ? : -ENOENT;
}

static int __pasw_l2_fdb_del_learned(struct pasw *sw, u16 gid, u64 addr)
{
	struct pasw_l2_match match = {
		.status = PASW_L2_VALID_BITS,
	};
	int err;

	err = __pasw_l2_fdb_del(sw, &match, gid, addr);
	if (err == -ENOENT)
		err = 0;

	return err;
}

int pasw_l2_fdb_add_static(struct pasw *sw, int port, u16 gid, const u8 *addr)
{
	u64 addr64 = ether_addr_to_u64(addr);
	struct pasw_l2_match match = {
		.status = PASW_L2_INVALID_BIT,
		.hash = {
			.match = true,
			.val = pasw_table_hash(pasw_table(sw, l2_da_hash),
					       addr64 | ((u64)gid << 48), 49),
		},
	};
	struct pasw_l2_iter iter;
	int err;

	err = pasw_l2_set_hw_learn_age(sw, false);
	err = err ? : __pasw_l2_fdb_del_learned(sw, gid, addr64);
	if (err)
		return err;

	pasw_l2_iter_init(sw, &iter, &match);
	while ((err = pasw_l2_iter_next(sw, &iter)) == 1) {
		err = pasw_row_set(&iter.entry->addr, &pasw_t(sw, l2_da_hash)->gid, gid);
		err = err ? : pasw_row_set(&iter.entry->addr,
					   &pasw_t(sw, l2_da_hash)->mac_addr, addr64);

		err = err ? : pasw_row_set(&iter.dst, &pasw_t(sw, l2_dst)->uc, 1);
		err = err ? : pasw_row_set(&iter.dst, &pasw_t(sw, l2_dst)->dst, port);

		err = err ? : pasw_row_set(&iter.entry->aging,
					   &pasw_t(sw, l2_aging)->status,
					   PASW_L2_STATIC);
		err = err ? : pasw_row_set(&iter.entry->aging_stat,
					   &pasw_t(sw, l2_aging_stat)->valid, 1);
		err = err ? : pasw_l2_iter_store(sw, &iter);
		err = err ? : pasw_l2_set_hw_learn_age(sw, true);
		goto out_enable;
	}

	err = -ENOSPC;

out_enable:
	if (err)
		pasw_l2_set_hw_learn_age(sw, true);
	else
		err = pasw_l2_set_hw_learn_age(sw, true);

	return err;
}

int pasw_l2_fdb_del_static(struct pasw *sw, int port, u16 gid, const u8 *addr)
{
	struct pasw_l2_match match = {
		.status = PASW_L2_STATIC_BIT,
		.port = {
			.match = true,
			.val = port,
		},
	};
	int err;

	err = pasw_l2_set_hw_learn_age(sw, false);
	if (err)
		return err;

	err = __pasw_l2_fdb_del(sw, &match, gid, ether_addr_to_u64(addr));
	if (err)
		pasw_l2_set_hw_learn_age(sw, true);
	else
		err = pasw_l2_set_hw_learn_age(sw, true);

	return err;
}

int pasw_l2_set_aging_time(struct pasw *sw, unsigned int msecs)
{
#define PASW_L2_AGE_CNT 20
	struct pasw_row age_time = { .table = pasw_table(sw, l2_age_time) };
	struct pasw_tick *tick = pasw_tick_get(sw, PASW_TICK, 0);
	unsigned long ticks;
	int err;

	err = pasw_row_load(&age_time, sw);
	err = err ? : pasw_row_set(&age_time, &pasw_t(sw, l2_age_time)->age_cnt,
				   PASW_L2_AGE_CNT);
	if (err)
		return err;

	/* HW ages out entries in age_cnt*tick_cnt ticks */
	msecs /= PASW_L2_AGE_CNT;

	ticks = pasw_tick_ticks(tick, msecs * NSEC_PER_MSEC);
	if (!ticks || !pasw_field_fits(&pasw_t(sw, l2_age_time)->tick_cnt, ticks))
		return -ERANGE;

	err = pasw_row_set(&age_time, &pasw_t(sw, l2_age_time)->tick_cnt, ticks);
	return err ? :pasw_row_store(&age_time, sw);
}

int pasw_l2_setup(struct pasw *sw)
{
	unsigned int aging_time = jiffies_to_msecs(BR_DEFAULT_AGEING_TIME);
	int err;

	err = pasw_l2_flush_all(sw);
	err = err ? : pasw_l2_set_aging_time(sw, aging_time);
	return err;
}

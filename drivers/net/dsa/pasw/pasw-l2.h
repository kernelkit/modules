#ifndef _PASW_L2_H
#define _PASW_L2_H

#include "pasw-abi.h"

enum pasw_l2_status {
	PASW_L2_INVALID  =  0,
	PASW_L2_RESERVED = 30,
	PASW_L2_STATIC   = 31,

	PASW_L2_INVALID_BIT  = BIT(PASW_L2_INVALID),
	PASW_L2_VALID_BITS   = GENMASK(29, 1),
	PASW_L2_RESERVED_BIT = BIT(PASW_L2_RESERVED),
	PASW_L2_STATIC_BIT   = BIT(PASW_L2_STATIC),
};

struct pasw_l2_match {
	u32 status;

	struct {
		bool match;
		int val;
	} port;
	struct {
		bool match;
		u32 val;
	} hash;
	struct {
		bool match;
		u32 val;
	} way;
};

struct pasw_l2_entry {
	struct pasw_row aging;
	struct pasw_row aging_stat;
	struct pasw_row addr;
};

struct pasw_l2_iter {
	u32 next_idx;

	struct pasw_l2_entry hash;
	struct pasw_l2_entry coll;
	struct pasw_l2_entry *entry;

	struct pasw_row dst;

	const struct pasw_l2_match *match;
};

void pasw_l2_iter_init(struct pasw *sw, struct pasw_l2_iter *iter,
		       const struct pasw_l2_match *match);
int pasw_l2_iter_next(struct pasw *sw, struct pasw_l2_iter *iter);

int pasw_l2_flush_port(struct pasw *sw, int port, bool inc_static);

int pasw_l2_fdb_add_static(struct pasw *sw, int port, u16 gid, const u8 *addr);
int pasw_l2_fdb_del_static(struct pasw *sw, int port, u16 gid, const u8 *addr);

int pasw_l2_set_aging_time(struct pasw *sw, unsigned int msecs);
int pasw_l2_setup(struct pasw *sw);

#endif	/* _PASW_L2_H */

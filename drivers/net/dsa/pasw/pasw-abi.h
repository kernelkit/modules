#ifndef _PASW_ABI_H
#define _PASW_ABI_H

u64 pasw_hash(u64 key, u8 key_bits, u8 hash_bits);

struct pasw;

struct pasw_field {
	u32 offs;
	u32 mask, mask_hi;
};

static inline bool pasw_field_ok(const struct pasw_field *f)
{
	return f && f->mask;
}

bool pasw_field_fits(const struct pasw_field *f, u64 val);

struct pasw_var {
	u32 reg;
	struct pasw_field field;
};

static inline bool pasw_var_ok(const struct pasw_var *v)
{
	return v && pasw_field_ok(&v->field);
}

int pasw_var_get(struct pasw *sw, const struct pasw_var *v, u32 *val);
int pasw_var_set(struct pasw *sw, const struct pasw_var *v, u32 val);
int pasw_var_mod(struct pasw *sw, const struct pasw_var *v, u32 clr, u32 set);


struct pasw_table {
	u32 reg;
	u32 len:20;
	u32 stride:6;
	u32 ways:4;

	/* Some tables are replicated to allow for parallel
	 * lookups. For such tables, this holds the base
	 * register. NOTE: 0 is safe to use as a sentinel value since
	 * that register always holds the core version.
	 */
	u32 replica_reg;
};

#define pasw_t(_sw, _t) ((_sw)->abi->table._t)
#define pasw_table(_sw, _t) (&((_sw)->abi->table._t->table))

static inline bool pasw_table_ok(const struct pasw_table *t)
{
	return t && t->len;
}

u64 pasw_table_hash(const struct pasw_table *t, u64 key, u8 key_bits);
void pasw_table_hashway(const struct pasw_table *t, u32 idx, u32 *hash, u32 *way);

struct pasw_row {
	const struct pasw_table *table;
	u32 idx;

#define __PASW_ROW_WORDS 8
	u32 word[__PASW_ROW_WORDS];
};

int pasw_row_load(struct pasw_row *row, struct pasw *sw);
int pasw_row_store(const struct pasw_row *row, struct pasw *sw);

int pasw_row_get(const struct pasw_row *row, const struct pasw_field *f, u64 *val);
int pasw_row_geti(const struct pasw_row *row, const struct pasw_field *field);
int pasw_row_set(struct pasw_row *row, const struct pasw_field *f, u64 val);
int pasw_row_mod(struct pasw_row *row, const struct pasw_field *f, u64 clr, u64 set);

struct pasw_stat_field {
	struct pasw_field field;
	const char *name;
};

struct pasw_stat_table {
	struct pasw_table table;
	const char *name;

	size_t nr_fields;
	const struct pasw_stat_field *field;
};

enum pasw_tag_format {
	PASW_TAG_UNSUPPORTED,
	PASW_TAG_2B_PORT_QUEUE,
};

/* struct pasw_abi is generated from pasw-abi.yml */
#define _INJECT_PASW_ABI_SPEC
#include "pasw-abi-spec.h"
#undef _INJECT_PASW_ABI_SPEC

#endif	/* _PASW_ABI_H */

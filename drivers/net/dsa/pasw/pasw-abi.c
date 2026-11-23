#include <linux/bitfield.h>

/* Provide compat definitions of field_{prep,get} for older
 * kernels (< 6.19)
 */
#ifndef field_prep
#define field_prep(mask, val)						\
	({								\
		__auto_type __mask = (mask);				\
		typeof(__mask) __val = (val);				\
		unsigned int __shift = BITS_PER_TYPE(__mask) <= 32 ?	\
				       __ffs(__mask) : __ffs64(__mask);	\
		(__val << __shift) & __mask;				\
	})
#endif	/* field_prep */

#ifndef field_get
#define field_get(mask, reg)						\
	({								\
		__auto_type __mask = (mask);				\
		typeof(__mask) __reg =  (reg);				\
		unsigned int __shift = BITS_PER_TYPE(__mask) <= 32 ?	\
				       __ffs(__mask) : __ffs64(__mask);	\
		(__reg & __mask) >> __shift;				\
	})

#endif	/* field_get */

#include <linux/bug.h>
#include <linux/regmap.h>

#include "pasw.h"
#include "pasw-abi.h"

u64 pasw_hash(u64 key, u8 key_bits, u8 hash_bits)
{
	u64 hash = 0;
	u8 i = 0;

	for (i = 0; i < key_bits; i += hash_bits)
		hash ^= field_get(GENMASK(i + hash_bits, i), key);

	return hash & ((1 << hash_bits) - 1);
}

static u64 pasw_field_get(const u32 *word, const struct pasw_field *f)
{
	unsigned int shift_hi;

	BUG_ON(!pasw_field_ok(f));

	if (likely(!f->mask_hi))
		return field_get(f->mask, word[f->offs]);

	/* When a field crosses a word boundary, we know that the
	 * lower part goes all the way up to the MSB
	 */
	shift_hi = 32 - __ffs(f->mask);

	return ((u64)field_get(f->mask_hi, word[f->offs + 1]) << shift_hi) |
		(u64)field_get(f->mask, word[f->offs]);
}

static void pasw_field_set(u32 *word, const struct pasw_field *f, u64 val)
{
	u32 lo, hi;

	BUG_ON(!pasw_field_ok(f));

	lo = field_prep(f->mask, (u32)val);
	word[f->offs] = (word[f->offs] & ~f->mask) | lo;

	if (likely(!f->mask_hi))
		return;

	val >>= 32 - __ffs(f->mask);
	hi = field_prep(f->mask_hi, (u32)val);
	word[f->offs+1] = (word[f->offs+1] & ~f->mask_hi) | hi;
}

static void pasw_field_mod(u32 *word, const struct pasw_field *f, u64 clr, u64 set)
{
	pasw_field_set(word, f, (pasw_field_get(word, f) & ~clr) | set);
}

bool pasw_field_fits(const struct pasw_field *f, u64 val)
{
	u32 word[2] = { 0, 0 };

	pasw_field_set(word, f, val);
	return pasw_field_get(word, f) == val;
}

int pasw_var_get(struct pasw *sw, const struct pasw_var *v, u32 *val)
{
	u32 word;
	int err;

	if (!pasw_var_ok(v))
		return -EOPNOTSUPP;


	err = regmap_read(sw->regs, v->reg, &word);
	if (err)
		return err;

	*val = (u32)pasw_field_get(&word, &v->field);
	return 0;
}

int pasw_var_set(struct pasw *sw, const struct pasw_var *v, u32 val)
{
	u32 word;
	int err;

	if (!pasw_var_ok(v))
		return -EOPNOTSUPP;

	err = regmap_read(sw->regs, v->reg, &word);
	if (err)
		return err;

	pasw_field_set(&word, &v->field, val);

	err = regmap_write(sw->regs, v->reg, word);
	if (err)
		return err;

	return 0;
}

int pasw_var_mod(struct pasw *sw, const struct pasw_var *v, u32 clr, u32 set)
{
	u32 word;
	int err;

	if (!pasw_var_ok(v))
		return -EOPNOTSUPP;

	err = regmap_read(sw->regs, v->reg, &word);
	if (err)
		return err;

	pasw_field_mod(&word, &v->field, clr, set);

	err = regmap_write(sw->regs, v->reg, word);
	if (err)
		return err;

	return 0;
}

u64 pasw_table_hash(const struct pasw_table *t, u64 key, u8 key_bits)
{
	u32 hash_bits;

	BUG_ON(t->ways < 2);

	hash_bits = __fls((t->len / t->ways) - 1) + 1;
	return pasw_hash(key, key_bits, hash_bits);
}

void pasw_table_hashway(const struct pasw_table *t, u32 idx, u32 *hash, u32 *way)
{
	u32 hash_mask, way_mask;

	BUG_ON(t->ways < 2);

	hash_mask = (t->len / t->ways) - 1;
	way_mask = (t->ways - 1) << (__fls(hash_mask) + 1);

	*hash = field_get(hash_mask, idx);
	*way = field_get(way_mask, idx);
}

int pasw_row_load(struct pasw_row *row, struct pasw *sw)
{
	const struct pasw_table *t = row->table;

	if (!pasw_table_ok(t))
		return -EOPNOTSUPP;

	return regmap_bulk_read(sw->regs, t->reg + row->idx * t->stride,
				row->word, t->stride);
}

int pasw_row_store(const struct pasw_row *row, struct pasw *sw)
{
	const struct pasw_table *t = row->table;
	int err;

	if (!pasw_table_ok(t))
		return -EOPNOTSUPP;

	err = regmap_bulk_write(sw->regs, t->reg + row->idx * t->stride,
				row->word, t->stride);
	if (err)
		return err;

	if (t->replica_reg)
		err = regmap_bulk_write(sw->regs, t->replica_reg + row->idx * t->stride,
					row->word, t->stride);

	return err;
}

int pasw_row_get(const struct pasw_row *row, const struct pasw_field *field, u64 *val)
{
	const struct pasw_table *t = row->table;

	if (!pasw_table_ok(t) || !pasw_field_ok(field))
		return -EOPNOTSUPP;

	*val = pasw_field_get(row->word, field);
	return 0;
}

int pasw_row_geti(const struct pasw_row *row, const struct pasw_field *field)
{
	u64 val;
	int err;

	err = pasw_row_get(row, field, &val);
	return err ? : (int)(val & 0x7fffffff);
}

int pasw_row_set(struct pasw_row *row, const struct pasw_field *field, u64 val)
{
	const struct pasw_table *t = row->table;

	if (!pasw_table_ok(t) || !pasw_field_ok(field))
		return -EOPNOTSUPP;

	pasw_field_set(row->word, field, val);
	return 0;
}

int pasw_row_mod(struct pasw_row *row, const struct pasw_field *field, u64 clr, u64 set)
{
	const struct pasw_table *t = row->table;

	if (!pasw_table_ok(t) || !pasw_field_ok(field))
		return -EOPNOTSUPP;

	pasw_field_mod(row->word, field, clr, set);
	return 0;
}


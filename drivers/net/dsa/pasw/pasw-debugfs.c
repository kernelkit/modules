#include <linux/debugfs.h>

#include "pasw.h"
#include "pasw-abi.h"

static struct dentry *pasw_debugfsdir;

static int pasw_debugfs_stats_show_table_row(struct seq_file *s,
					     const struct pasw_stat_table *stab,
					     const struct pasw_row *row)
{
	const struct pasw_stat_field *sf;
	char name[0x80];
	int err = 0;
	size_t i;
	u64 val;

	for (i = 0, sf = stab->field; i < stab->nr_fields; i++, sf++) {
		err = pasw_row_get(row, &sf->field, &val);
		if (err)
			break;

		if (stab->table.len > 1)
			snprintf(name, sizeof(name), "%s[%02u].%s",
				 stab->name, row->idx, sf->name);
		else
			snprintf(name, sizeof(name), "%s.%s",
				 stab->name, sf->name);

		seq_printf(s, "%-60s %10llu\n", name, val);
	}

	return err;
}

static int pasw_debugfs_stats_show_table(struct seq_file *s,
					 const struct pasw_stat_table *stab)
{
	struct pasw_row row = { .table = &stab->table };
	int err = 0;
	u32 i;

	for (i = 0; i < stab->table.len; i++) {
		row.idx = i;
		err = pasw_row_load(&row, s->private);
		if (err)
			break;

		err = pasw_debugfs_stats_show_table_row(s, stab, &row);
		if (err)
			break;
	}

	return err;
}

static int pasw_debugfs_stats_show(struct seq_file *s, void *offset)
{
	struct pasw *sw = (struct pasw *)s->private;
	int err = 0, i;

	for (i = 0; i < sw->abi->stats.nr_stats; i++) {
		err = pasw_debugfs_stats_show_table(s, &sw->abi->stats.table[i]);
		if (err)
			break;
	}

	return err;
}

DEFINE_SHOW_ATTRIBUTE(pasw_debugfs_stats);

int pasw_debugfs_setup(struct pasw *sw)
{
	sw->debugfs = debugfs_create_dir(dev_name(sw->dev), pasw_debugfsdir);
	debugfs_create_file("stats", 0444, sw->debugfs, sw, &pasw_debugfs_stats_fops);
	return 0;
}

int __init pasw_module_init_debugfs(void)
{
	pasw_debugfsdir = debugfs_create_dir("pasw", NULL);
	return 0;
}

void __exit pasw_module_exit_debugfs(void)
{
	debugfs_remove_recursive(pasw_debugfsdir);
}

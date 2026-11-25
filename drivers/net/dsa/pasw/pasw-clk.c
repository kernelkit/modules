// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/math64.h>

#include "pasw.h"
#include "pasw-abi.h"
#include "pasw-clk.h"

#define PASW_PTP_TICK_SEL_CORE 1

struct pasw_tick {
	struct clk_hw clkhw;
	struct pasw *sw;
	const struct pasw_var *div;
	u32 divshift;
	bool ro;
};

struct pasw_clks {
	struct clk *core;

	struct pasw_tick *tick;
	struct pasw_tick *ptp_tick;
};

/* The master tick's divider has divshift fractional bits. Since
 * rate = parent / div and div = parent / rate, this converts both ways.
 */
static u64 pasw_tick_div(struct pasw_tick *tick, unsigned long parent_rate, u64 x)
{
	return DIV64_U64_ROUND_CLOSEST((u64)parent_rate << tick->divshift, x);
}

static unsigned long pasw_tick_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct pasw_tick *tick = container_of(hw, struct pasw_tick, clkhw);
	u32 div;
	int err;

	err = pasw_var_get(tick->sw, tick->div, &div);
	if (err || !div)
		return 0;

	return pasw_tick_div(tick, parent_rate, div);
}

static long pasw_tick_round_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long *parent_rate)
{
	struct pasw_tick *tick = container_of(hw, struct pasw_tick, clkhw);
	u64 div;

	if (!rate)
		return -ERANGE;

	div = pasw_tick_div(tick, *parent_rate, rate);
	if (!div || !pasw_field_fits(&tick->div->field, div))
		return -ERANGE;

	return pasw_tick_div(tick, *parent_rate, div);
}

static int pasw_tick_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent_rate)
{
	struct pasw_tick *tick = container_of(hw, struct pasw_tick, clkhw);
	u64 div;

	if (tick->ro)
		return -EPERM;

	if (!rate)
		return -ERANGE;

	div = pasw_tick_div(tick, parent_rate, rate);
	if (!div || !pasw_field_fits(&tick->div->field, div))
		return -ERANGE;

	return pasw_var_set(tick->sw, tick->div, div);
}

struct clk_ops pasw_tick_ops = {
	.recalc_rate = pasw_tick_recalc_rate,
	.round_rate = pasw_tick_round_rate,
	.set_rate = pasw_tick_set_rate,
};

struct pasw_tick *pasw_tick_get(struct pasw *sw, enum pasw_tick_type type, u8 idx)
{
	switch (type) {
	case PASW_TICK:
		if (idx >= sw->abi->attr.nr_of_ticks)
			return NULL;
		return &sw->clks->tick[idx];
	case PASW_PTP_TICK:
		if (idx >= sw->abi->attr.nr_of_ptp_ticks)
			return NULL;
		return &sw->clks->ptp_tick[idx];
	}

	return NULL;
}

unsigned long pasw_tick_rate(struct pasw_tick *tick)
{
	return clk_get_rate(tick->clkhw.clk);
}

int pasw_tick_rate_set(struct pasw_tick *tick, unsigned long rate)
{
	return clk_set_rate(tick->clkhw.clk, rate);
}

unsigned long pasw_tick_ticks(struct pasw_tick *tick, unsigned long ns)
{
	unsigned long rate = pasw_tick_rate(tick);
	u32 mult = clocksource_hz2mult(rate, 0);

	return ns / mult;
}

static struct pasw_tick *pasw_clk_setup_tick(struct pasw *sw, const char *name,
					     const struct pasw_var *div,
					     const struct pasw_var *step,
					     u8 nr_ticks)
{
	struct pasw_tick *tick;
	int err;
	u8 i;

	tick = devm_kcalloc(sw->dev, nr_ticks, sizeof(*tick), GFP_KERNEL);
	if (!tick)
		return ERR_PTR(-ENOMEM);

	tick[0] = (struct pasw_tick) {
		.clkhw =  {
			.init = CLK_HW_INIT(devm_kasprintf(sw->dev, GFP_KERNEL, "%s-%s0",
							      dev_name(sw->dev), name),
					    __clk_get_name(sw->clks->core), &pasw_tick_ops, 0),
		},
		.sw = sw,
		.div = div,
		.divshift = 8,
	};

	err = devm_clk_hw_register(sw->dev, &tick[0].clkhw);
	if (err) {
		dev_err(sw->dev, "Unable to register %s: %pe\n",
			tick[0].clkhw.init->name, ERR_PTR(err));
		return ERR_PTR(err);
	}

	for (i = 1; i < nr_ticks; i++) {
		tick[i] = (struct pasw_tick) {
			.clkhw =  {
				.init = CLK_HW_INIT_HW(devm_kasprintf(sw->dev, GFP_KERNEL, "%s-%s%u",
								      dev_name(sw->dev), name, i),
						       &tick[i - 1].clkhw, &pasw_tick_ops, 0),
			},
			.sw = sw,
			.div = step,
			.ro = i > 1,
		};

		err = devm_clk_hw_register(sw->dev, &tick[i].clkhw);
		if (err) {
			dev_err(sw->dev, "Unable to register %s: %pe\n",
				tick[i].clkhw.init->name, ERR_PTR(err));
			return ERR_PTR(err);
		}
	}

	return tick;
}

int pasw_clk_setup(struct pasw *sw)
{
	sw->clks = devm_kzalloc(sw->dev, sizeof(*sw->clks), GFP_KERNEL);
	if (!sw->clks)
		return -ENOMEM;

	sw->clks->core = devm_clk_get_enabled(sw->dev, "core");
	if (IS_ERR(sw->clks->core)) {
		dev_err(sw->dev, "Unable to locate core clock: %pe\n", sw->clks->core);
		return PTR_ERR(sw->clks->core);
	}

	sw->clks->tick = pasw_clk_setup_tick(sw, "tick",
					     &sw->abi->var.tick_div,
					     &sw->abi->var.tick_step,
					     sw->abi->attr.nr_of_ticks);
	if (IS_ERR(sw->clks->tick))
		return PTR_ERR(sw->clks->tick);

	if (sw->abi->attr.nr_of_ptp_ticks) {
		sw->clks->ptp_tick = pasw_clk_setup_tick(sw, "ptp-tick",
							 &sw->abi->var.ptp_tick_div,
							 &sw->abi->var.ptp_tick_step,
							 sw->abi->attr.nr_of_ptp_ticks);
		if (IS_ERR(sw->clks->ptp_tick))
			return PTR_ERR(sw->clks->ptp_tick);

		if (pasw_var_ok(&sw->abi->var.ptp_tick_sel))
			return pasw_var_set(sw, &sw->abi->var.ptp_tick_sel,
					    PASW_PTP_TICK_SEL_CORE);
	}

	return 0;
}

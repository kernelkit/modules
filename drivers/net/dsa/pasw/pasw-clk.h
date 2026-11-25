/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _PASW_CLK_H
#define _PASW_CLK_H

enum pasw_tick_type {
	PASW_TICK,
	PASW_PTP_TICK,
};

struct pasw_tick;
struct pasw_tick *pasw_tick_get(struct pasw *sw, enum pasw_tick_type type, u8 idx);
unsigned long pasw_tick_rate(struct pasw_tick *tick);
int pasw_tick_rate_set(struct pasw_tick *tick, unsigned long rate);
unsigned long pasw_tick_ticks(struct pasw_tick *tick, unsigned long ns);

int pasw_clk_setup(struct pasw *sw);

#endif	/* _PASW_CLK_H */

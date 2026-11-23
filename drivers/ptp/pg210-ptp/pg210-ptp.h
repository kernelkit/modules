/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _PG210_PTP_H
#define _PG210_PTP_H

#include <linux/time64.h>

struct device;

int pg210_ptp_gettime(struct device *dev, struct timespec64 *ts);

#endif	/* _PG210_PTP_H */

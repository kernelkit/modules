/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __TAG_PASW_H
#define __TAG_PASW_H

#include <linux/types.h>

/* MODULE_ALIAS_DSA_TAG_DRIVER relies on ${PROTO} + ${PROTO}_VALUE
 * both being defined. */
#define DSA_TAG_PROTO_PASW_PQ_VALUE 9999
#define DSA_TAG_PROTO_PASW_PQ DSA_TAG_PROTO_PASW_PQ_VALUE

#define ETH_P_PASW_PQ_FROM_CPU 0x9998
#define ETH_P_PASW_PQ_TO_CPU   0x9999

#define PASW_TAG_PQ_FROM_CPU_LEN  5
#define PASW_TAG_PQ_TO_CPU_LEN   15

#endif	/* __TAG_PASW_H */

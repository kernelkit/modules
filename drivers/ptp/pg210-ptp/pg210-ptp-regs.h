/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _PG210_PTP_REGS_H
#define _PG210_PTP_REGS_H

#include <linux/bitops.h>

#define PG210_PTP_TOD_CONFIG                  0x000
#define  PG210_PTP_TOD_CONFIG_EN_SYS_TIMER      BIT(0)
#define  PG210_PTP_TOD_CONFIG_EN_EXT_TOD        BIT(1)
#define  PG210_PTP_TOD_CONFIG_OVERWRITE_MASK    GENMASK(3, 2)
#define   PG210_PTP_TOD_OVERWRITE_NONE            0x0
#define   PG210_PTP_TOD_OVERWRITE_EXT_SEC         0x1
#define   PG210_PTP_TOD_OVERWRITE_SW_SEC          0x2
#define  PG210_PTP_TOD_CONFIG_AUTO_REFRESH      BIT(4)
#define  PG210_PTP_TOD_CONFIG_EN_EXT_SNAP       BIT(5)
#define  PG210_PTP_TOD_CONFIG_EN_PORT(_n)        BIT(16 + (_n))
#define  PG210_PTP_TOD_CONFIG_PORT_MASK         GENMASK(31, 16)

#define PG210_PTP_TOD_SNAPSHOT                0x004
#define  PG210_PTP_TOD_SNAPSHOT_ALL             BIT(0)

#define PG210_PTP_TOD_INTR_ENABLE             0x008
#define PG210_PTP_TOD_INTR_STATUS             0x00c
#define  PG210_PTP_TOD_INTR_1PPS_MASTER         BIT(0)
#define  PG210_PTP_TOD_INTR_1PPS_EXT            BIT(16)

#define PG210_PTP_TOD_SW_SEC_LO               0x010
#define PG210_PTP_TOD_SW_SEC_HI               0x014
#define PG210_PTP_TOD_SW_NS                   0x018

#define PG210_PTP_TOD_SW_LOAD                 0x01c
#define  PG210_PTP_TOD_SW_LOAD_TOD              BIT(0)
#define  PG210_PTP_TOD_SW_LOAD_OFFSET           BIT(1)

#define PG210_PTP_TOD_SW_CTIME_LO             0x020
#define PG210_PTP_TOD_SW_CTIME_HI             0x024

#define PG210_PTP_TOD_SEC_SYS_OFFSET_LO       0x028
#define PG210_PTP_TOD_SEC_SYS_OFFSET_HI       0x02c
#define PG210_PTP_TOD_NS_SYS_OFFSET_LO        0x030
#define  PG210_PTP_TOD_SEC_OFFSET_SUBTRACT      BIT(47)

#define PG210_PTP_TOD_SYS_PERIOD_LO           0x034
#define PG210_PTP_TOD_SYS_PERIOD_HI           0x038
#define  PG210_PTP_TOD_SYS_PERIOD_HI_MASK       GENMASK(23, 0)

#define PG210_PTP_TOD_SYS_SEC_LO              0x100
#define PG210_PTP_TOD_SYS_SEC_HI              0x104
#define PG210_PTP_TOD_SYS_NS                  0x108
#define PG210_PTP_TOD_SYS_OFFSET              0x10c
#define PG210_PTP_TOD_SYS_CTIME_LO            0x110
#define PG210_PTP_TOD_SYS_CTIME_HI            0x114

#define PG210_PTP_TODBUS_SEC_LO               0x120
#define PG210_PTP_TODBUS_SEC_HI               0x124
#define PG210_PTP_TODBUS_SYS_DIFF             0x12c

#define  PG210_PTP_TOD_SEC_HI_MASK              GENMASK(15, 0)
#define  PG210_PTP_TOD_NS_MASK                  GENMASK(29, 0)
#define  PG210_PTP_TOD_CTIME_HI_MASK            GENMASK(30, 0)

#define PG210_PTP_PERIOD_FRAC_BITS            48
#define PG210_PTP_OFFSET_FRAC_BITS            16


#define PG210_PTP_PORT_BASE                   0x200
#define PG210_PTP_PORT_STRIDE                 0x080
#define PG210_PTP_PORT_MAX                    16

#define PG210_PTP_PORT_REG(_p, _reg) \
        (PG210_PTP_PORT_BASE + (_p) * PG210_PTP_PORT_STRIDE + (_reg))

#define PG210_PTP_TX_CTIME_LO                 0x00
#define PG210_PTP_TX_CTIME_HI                 0x04
#define PG210_PTP_TX_PERIOD_LO                0x08
#define PG210_PTP_TX_PERIOD_HI                0x0c
#define PG210_PTP_TX_SYS_OFFSET               0x10
#define PG210_PTP_TX_NS_SNAP                  0x14
#define PG210_PTP_TX_SEC_SNAP_LO              0x18
#define PG210_PTP_TX_SEC_SNAP_HI              0x1c

#define PG210_PTP_RX_CTIME_LO                 0x20
#define PG210_PTP_RX_CTIME_HI                 0x24
#define PG210_PTP_RX_PERIOD_LO                0x28
#define PG210_PTP_RX_PERIOD_HI                0x2c
#define PG210_PTP_RX_SYS_OFFSET               0x30
#define PG210_PTP_RX_NS_SNAP                  0x34
#define PG210_PTP_RX_SEC_SNAP_LO              0x38
#define PG210_PTP_RX_SEC_SNAP_HI              0x3c

#define PG210_PTP_CORE_TX_PERIOD_LO           0x40
#define PG210_PTP_CORE_TX_PERIOD_HI           0x44
#define PG210_PTP_CORE_RX_PERIOD_LO           0x48
#define PG210_PTP_CORE_RX_PERIOD_HI           0x4c

#define PG210_PTP_PORT_SEC_OFFSET_LO          0x50
#define PG210_PTP_PORT_SEC_OFFSET_HI          0x54
#define PG210_PTP_PORT_NS_OFFSET              0x58

#define PG210_PTP_REGMAP_MAX \
        (PG210_PTP_PORT_REG(PG210_PTP_PORT_MAX - 1, PG210_PTP_PORT_NS_OFFSET))

#endif /* _PG210_PTP_REGS_H */

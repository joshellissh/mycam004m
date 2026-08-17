/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MYIR MY-CAM004M quad-AHD to 4-lane MIPI CSI-2 decoder
 *
 * Shared definitions between mycam004m.c and mycam004m-regs.h, kept in
 * their own header so mycam004m-regs.h's register tables don't need to
 * touch this file when they change.
 */

#ifndef __MYCAM004M_H__
#define __MYCAM004M_H__

#include <linux/types.h>

/* One AHD input == one media-controller sink pad. */
#define MYCAM004M_NUM_CAMS	4

/* Single physical 4-lane MIPI CSI-2 output, carrying up to
 * MYCAM004M_NUM_CAMS multiplexed streams (one per virtual channel).
 *
 * MYCAM004M_NUM_LANES is deliberately a separate constant from
 * MYCAM004M_NUM_CAMS even though both are 4 -- they mean different
 * things (physical D-PHY lanes vs. camera inputs/virtual channels)
 * and there's no hardware reason they'd have to stay equal on a
 * different MY-CAM variant.
 */
#define MYCAM004M_PAD_SOURCE	MYCAM004M_NUM_CAMS
#define MYCAM004M_NUM_PADS	(MYCAM004M_NUM_CAMS + 1)
#define MYCAM004M_NUM_LANES	4

/* Fixed target mode -- see the task brief. Deliberately not negotiable
 * at runtime (mycam004m_set_fmt() always forces these values).
 *
 * MYCAM004M_FPS assumes a 30fps-capable AHD camera head. N4's own
 * power-on-reset default is 1080p25, not 1080p30 -- mycam004m_init_regs[]
 * in mycam004m-regs.h explicitly overrides that default to match this
 * constant. If your actual camera heads are 25fps-only (see that
 * table's comment for a specific real-world example), change both
 * MYCAM004M_FPS here and the AHD_MD values there together.
 */
#define MYCAM004M_WIDTH		1920
#define MYCAM004M_HEIGHT		1080
#define MYCAM004M_FPS			30

/*
 * N4 (confirmed via Nextchip's own datasheet -- see mycam004m-regs.h) uses
 * a bank-switched 8-bit register map: writing this register selects which
 * bank the following reg/val writes land in. Confirmed both by N4's own
 * datasheet chapter numbering (BANK0, BANK1, BANK2~3, BANK4, BANK20,
 * BANK21) and, directly, by two independent register-compatible
 * sibling-chip (NVP6324) drivers that both select banks by writing this
 * exact register -- see mycam004m-regs.h for the full citation trail.
 */
#define MYCAM004M_REG_BANK_SEL	0xff

/**
 * struct mycam004m_reg - one write in an init/config register sequence.
 * @bank: bank to select (via MYCAM004M_REG_BANK_SEL) before writing @reg.
 * @reg: register address within that bank.
 * @val: value to write.
 *
 * 8-bit bank, 8-bit register address, 8-bit value -- confirmed against
 * N4's own I2C protocol description (single base-register-index byte
 * per write) and against the register-compatible sibling driver's actual
 * i2c_master_send() calls (2-byte payload: reg, val). @reg is u16 rather
 * than u8 purely so a hypothetical future 16-bit-addressed variant
 * wouldn't need a type change here; N4 itself is confirmed 8-bit.
 */
struct mycam004m_reg {
	u8 bank;
	u16 reg;
	u8 val;
};

#endif /* __MYCAM004M_H__ */

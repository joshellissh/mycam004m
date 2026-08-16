/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MYIR MY-CAM004M quad-AHD to 4-lane MIPI CSI-2 decoder
 *
 * Shared definitions between mycam004m.c and mycam004m-regs.h, kept in
 * their own header so the (currently empty) register tables in
 * mycam004m-regs.h can be dropped in later without touching this file.
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

/* Fixed target mode -- see the task brief. There is no register map yet
 * to support anything else, so this is deliberately not negotiable at
 * runtime (mycam004m_set_fmt() always forces these values).
 */
#define MYCAM004M_WIDTH		1920
#define MYCAM004M_HEIGHT		1080
#define MYCAM004M_FPS			30

/**
 * struct mycam004m_reg - one write in an init/config register sequence.
 * @reg: register address.
 * @val: value to write.
 *
 * @reg is u16 (rather than u8) so a variant of the table using 16-bit
 * register addressing doesn't need a type change here -- only the
 * regmap_config.reg_bits in mycam004m.c and the values in
 * mycam004m-regs.h would need updating. The actual address width is
 * unconfirmed -- see the TODO in mycam004m.c's regmap_config.
 */
struct mycam004m_reg {
	u16 reg;
	u8 val;
};

#endif /* __MYCAM004M_H__ */

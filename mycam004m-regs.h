/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MYIR MY-CAM004M / N4 register initialization tables.
 *
 * *** Every table below is a deliberately empty placeholder. ***
 *
 * This is the single file to edit once the MYIR MY-CAM004M / N4
 * programming guide is available. mycam004m.c only ever walks these
 * tables through mycam004m_write_regs() -- it does not need to change
 * when real register data is added here.
 *
 * Do not guess register addresses or values. An incorrect table would
 * silently misconfigure the decoder (or brick a debugging session);
 * an empty table just no-ops, which is safe and obviously incomplete
 * (every caller of these tables logs at dev_dbg when the table it was
 * about to apply has zero entries -- see mycam004m.c).
 *
 * Expected shape once the datasheet is in hand:
 *
 *  - mycam004m_init_regs[]:
 *      One-time chip bring-up sequence, applied once after RESET_N is
 *      released and before any per-input or CSI-2 output configuration
 *      (see mycam004m_apply_init_sequence() in mycam004m.c).
 *
 *  - mycam004m_csi_output_regs[]:
 *      4-lane MIPI CSI-2 TX configuration: lane count/mapping, output
 *      pixel format, and virtual-channel assignment per AHD input.
 *      (see mycam004m_configure_csi_output() in mycam004m.c).
 *
 *      Two things elsewhere in the driver are load-bearing on this
 *      table matching reality and MUST be kept in sync with whatever
 *      gets programmed here:
 *
 *        1. mycam004m_vc_for_cam() in mycam004m.c assumes AHD input N
 *           is transmitted on virtual channel N. If the real chip
 *           assigns VCs differently (fixed to something else, or
 *           programmable to a non-identity mapping), update that
 *           function to match whatever this table actually programs.
 *
 *        2. MIPI_CSI2_DT_YUV422_8B is hardcoded as the CSI-2 data type
 *           in mycam004m_get_frame_desc() in mycam004m.c. That's the
 *           standard MIPI CSI-2 wire code for 8-bit YUV422 (not
 *           MYIR-specific), so it should be correct as long as
 *           whatever gets written here actually configures the chip to
 *           emit that data type. If MY-CAM004M has a non-standard or
 *           configurable output data type, confirm this table produces
 *           0x1e on the wire, or update mycam004m_get_frame_desc() to
 *           match.
 *
 *  - Per-AHD-input configuration (video standard detect/force,
 *    per-channel enable/disable) is intentionally NOT modeled as a
 *    table here yet, because its register shape (single global
 *    register with per-channel bits? four independent per-channel
 *    register blocks? something else?) isn't known. See
 *    mycam004m_enable_camera_input() / mycam004m_disable_camera_input()
 *    in mycam004m.c -- fill those in directly once the shape is known,
 *    adding a table type here if a tabular approach ends up fitting.
 *
 *  - Confirm against the datasheet whether register writes need any
 *    inter-write delay (some decoder/bridge chips require a settle
 *    time after specific writes, e.g. PLL lock). mycam004m_write_regs()
 *    currently writes the table back-to-back with no delay.
 *
 * Unconfirmed lead, worth checking before assuming a flat register map:
 * MYIR's own product page for MY-CAM004M states it "employs the N4
 * chip" but publishes no vendor/part number. The I2C address-strap
 * naming in the task this driver was written against ("SA0/SA1") is a
 * distinctive match for Nextchip's AHD decoder pinout naming (verified
 * directly against Nextchip's own public NVP6134C datasheet), and
 * Nextchip's NVP63xx family (e.g. NVP6334, per nextchip.com) is
 * marketed as exactly this device class: 4-channel AHD in, direct
 * 4-lane MIPI CSI-2 out. This is a plausible chip family match, NOT a
 * confirmed identification -- MY-CAM004M's actual chip marking (a
 * photo of the physical IC, if the board is in hand) would confirm or
 * rule this out in seconds, which is a much better use of five minutes
 * than continuing to search for it online.
 *
 * If it does turn out to be Nextchip-family: NVP6134C's public
 * datasheet (a different, non-MIPI chip in the same family, but the
 * closest real register-map reference available) uses a BANK-SWITCHED
 * 8-bit register map (multiple 0x00-0xFF banks selected via a bank
 * register, not one flat 8-bit address space) -- if the real MY-CAM004M
 * table ends up needing that, mycam004m_write_regs() in mycam004m.c
 * will need a bank-select write before applying entries whose bank
 * differs from the currently-selected one, not just a straight
 * regmap_write() loop like it does now.
 */

#ifndef __MYCAM004M_REGS_H__
#define __MYCAM004M_REGS_H__

#include "mycam004m.h" /* struct mycam004m_reg */

static const struct mycam004m_reg mycam004m_init_regs[] = {
	/* TODO: populate from the MYIR MY-CAM004M / N4 programming guide. */
};

static const struct mycam004m_reg mycam004m_csi_output_regs[] = {
	/* TODO: populate from the MYIR MY-CAM004M / N4 programming guide. */
};

#endif /* __MYCAM004M_REGS_H__ */

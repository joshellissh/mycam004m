/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MYIR MY-CAM004M / Nextchip N4 register initialization tables.
 *
 * MY-CAM004M's decoder chip is confirmed to be Nextchip's "N4" (4-CH
 * Automotive RX with MIPI-CSI2 Interface, 121-BGA-8x8) -- confirmed
 * directly from Nextchip's own datasheet (Rev 0.0, 2017-11-13) supplied
 * alongside the board schematic, product manual, and pinout sheet.
 * Everything below is sourced from that datasheet plus two independent,
 * real, register-compatible sibling-chip (Nextchip NVP6324) drivers --
 * N4 and NVP6324 are described in multiple independent sources (Nextchip
 * FAE/distributor literature, a TI E2E integration thread) as the same
 * underlying design, differing only in grade/package (N4 = industrial,
 * BGA; NVP6324 = automotive, QFP). Citations are inline below; see also
 * the project README's "How this was verified" section.
 *
 * *** NOT tested against real MY-CAM004M hardware. *** Everything here
 * is the best evidence available without a populated board in hand:
 * primary-sourced where the N4 datasheet documents it directly, and
 * clearly flagged where it instead relies on the sibling-chip drivers
 * (N4's own datasheet is a preliminary Rev 0.0 with its AC/timing tables
 * left as "TBD", so the MIPI PLL configuration in particular is NOT in
 * N4's own datasheet -- see the block comment on the PLL table below).
 *
 * Register map shape: bank-switched, not flat. Write
 * MYCAM004M_REG_BANK_SEL (0xff, see mycam004m.h) to select a bank, then
 * write within it. Confirmed via N4's own datasheet chapter structure
 * (BANK0, BANK1, BANK2~3, BANK4, BANK20, BANK21) lining up exactly with
 * two independent NVP6324 drivers' actual bank-select writes:
 *   - Allwinner's nvp6324/jaguar1_i2c.c + mipi_dev_nvp6324.c (in this
 *     project's MY-CAM004M/MYD-LT527/bsp reference tree):
 *     nvp6324_i2c_write(addr, 0xFF, bank) -- literal bank-select writes,
 *     including 0xFF=0x21 before every MIPI TX (N4's "BANK21") register.
 *   - NXP's i.MX8QM NVP6324 MIPI driver (0001-add-nvp6324-driver.patch,
 *     obtained from NXP Community KB article "Nvp6324 driver for
 *     imx8qm."): struct reg_base entries pairing a bank byte with each
 *     register address, same bank numbering.
 * mycam004m_write_regs() in mycam004m.c implements this: it tracks the
 * last-selected bank and emits a MYCAM004M_REG_BANK_SEL write whenever
 * an entry's bank differs from it.
 *
 * Confirmed register width: 8-bit register address, 8-bit value, single
 * write per register (not 16-bit / burst) -- matches both N4's I2C
 * protocol chapter (single base-register-index byte per write) and the
 * sibling driver's actual i2c_master_send(client, {reg, val}, 2) calls.
 *
 * Confirmed inter-write delay: the sibling I2C driver (jaguar1_i2c.c)
 * does udelay(300) after every single register write. mycam004m.c does
 * not currently replicate this delay in mycam004m_write_regs() -- add it
 * there if real hardware needs it; not yet verified either way since
 * there's no board to test against.
 */

#ifndef __MYCAM004M_REGS_H__
#define __MYCAM004M_REGS_H__

#include "mycam004m.h" /* struct mycam004m_reg, MYCAM004M_REG_BANK_SEL */

/* -----------------------------------------------------------------------
 * Bank numbers
 *
 * The value written to MYCAM004M_REG_BANK_SEL is the same number the N4
 * datasheet uses to label the bank (e.g. "BANK21" <-> write 0x21) --
 * confirmed directly against the sibling driver's 0xFF=0x21 write before
 * every register N4's own datasheet documents under "BANK21".
 * -----------------------------------------------------------------------
 */
#define MYCAM004M_BANK_VIDEO_FORMAT	0x00 /* N4 datasheet "BANK0": video decode config, per-channel power-down, AHD mode select, chip/rev ID */
#define MYCAM004M_BANK_MIPI_TX		0x21 /* N4 datasheet "BANK21": MIPI CSI-2 TX PHY/PLL, lane count, VC assignment, data type */

/* -----------------------------------------------------------------------
 * BANK0 (video format) registers used by this driver
 * -----------------------------------------------------------------------
 */

/* PD_VCH_1..4 (0x00-0x03): per-channel AFE power-down, bit0, active
 * high. Bit4 is a fixed reserved "-1-" per the datasheet (default value
 * 0x10 = bit4 set, bit0 clear = normal/not-powered-down out of reset).
 * Confirmed directly from the N4 datasheet register table.
 */
#define MYCAM004M_REG_PD_VCH(cam_idx)	(0x00 + (cam_idx))
#define MYCAM004M_PD_VCH_NORMAL		0x10 /* bit4 reserved-1 | bit0=0: channel enabled */
#define MYCAM004M_PD_VCH_POWERDOWN	0x11 /* bit4 reserved-1 | bit0=1: channel powered down */

/* AHD_MD_1..4 (0x08-0x0B): per-channel AHD input resolution/frame-rate
 * select, confirmed directly from the N4 datasheet register table:
 *   0x00 SD  0x02 1080p30  0x03 1080p25 (POR default)  0x04 720p60
 *   0x05 720p50  0x0C 720p30  0x0D 720p25
 * The chip's own power-on-reset default is 1080p25, NOT 1080p30 -- this
 * driver's fixed target (mycam004m.h MYCAM004M_FPS = 30) requires
 * explicitly writing 0x02.
 */
#define MYCAM004M_REG_AHD_MD(cam_idx)	(0x08 + (cam_idx))
#define MYCAM004M_AHD_MD_1080P30	0x02
#define MYCAM004M_AHD_MD_1080P25	0x03 /* POR default; NOT this driver's target */

/* DEV_ID / REV_ID (0xF4/0xF5): read-only chip identification. Confirmed
 * directly from the N4 datasheet: "DEV_ID: It shows Device ID (N4 =
 * 0xB0)". REV_ID default 0x00 for this datasheet's Rev 0.0 silicon.
 */
#define MYCAM004M_REG_DEV_ID		0xf4
#define MYCAM004M_REG_REV_ID		0xf5
#define MYCAM004M_DEV_ID_VAL		0xb0

/* -----------------------------------------------------------------------
 * BANK21 (MIPI TX) registers used by this driver
 * -----------------------------------------------------------------------
 */

/* Register 0x07: PHY power-down / lane count / TX enable, all in one
 * byte. Confirmed directly from the N4 datasheet bit layout:
 *   bit7 MIPI_TX_PHY_PWR_DOWN (0=normal, 1=power down)
 *   bits6:4 MIPI_TX_PHY_REF_SEL (trim, leave 0)
 *   bits3:2 MIPI_TX_LANES_ACTIVE (0=1-lane, 1=2-lane, 2 or 3=4-lane)
 *   bit1 MIPI_TX_SYS_CORE_RDY
 *   bit0 MIPI_TX_SERIAL_IF_EN
 * 0x0f (= 4-lane | SYS_CORE_RDY=1 | SERIAL_IF_EN=1) is not a guess: it's
 * the literal value the sibling Allwinner driver writes to this exact
 * register (jaguar1_mipi.c mipi_tx_init(): gpio_i2c_write(addr, 0x07,
 * 0x0F)) after selecting bank 0x21, decoded bit-for-bit against N4's own
 * datasheet layout for that register.
 */
#define MYCAM004M_REG_MIPI_TX_CTRL	0x07
#define MYCAM004M_MIPI_TX_CTRL_START	0x0f
#define MYCAM004M_MIPI_TX_CTRL_STOP	0x00 /* SERIAL_IF_EN=0 drops the PHY to its lowest power state per the datasheet -- good enough for "stopped" without a separate PHY_PWR_DOWN write */

/* Register 0x2E: virtual-channel-ID insertion mode (0=auto, i.e. VC N
 * assigned to AHD input N automatically; 1=manual via a single global
 * channel-number register, not useful for 4 independent channels).
 * Confirmed directly from the N4 datasheet; 0x00 is also the documented
 * power-on-reset default, matching mycam004m_vc_for_cam()'s identity
 * mapping. Written explicitly anyway: a real-world TI E2E thread about
 * this exact chip on a TI SoC ("SK-AM62P-LP: Enabling 4 AHD Cameras")
 * reported a total no-video failure that TI support traced back to
 * virtual-channel configuration -- explicit and verified-in-testing
 * beats relying on an assumed reset state.
 */
#define MYCAM004M_REG_MIPI_CH_ID_TYPE	0x2e
#define MYCAM004M_MIPI_CH_ID_AUTO	0x00

/* Registers 0x38-0x3B: MIPI CSI-2 data type per virtual channel (one
 * register per AHD input). 0x1e = YUV422 8-bit, the standard MIPI CSI-2
 * wire code MIPI_CSI2_DT_YUV422_8B already hardcoded in
 * mycam004m_get_frame_desc() -- confirmed as both N4's documented POR
 * default for these registers and the value both sibling drivers write
 * explicitly.
 */
#define MYCAM004M_REG_MIPI_TX_DATA_TYPE(cam_idx)	(0x38 + (cam_idx))
#define MYCAM004M_MIPI_TX_DATA_TYPE_YUV422_8B		0x1e

/* Register 0x0F bit0: MIPI_TX_FRAME_CNT_EN. Confirmed directly from the
 * N4 datasheet field description; both sibling drivers write 0x01 here.
 */
#define MYCAM004M_REG_MIPI_TX_FRAME_CNT	0x0f
#define MYCAM004M_MIPI_TX_FRAME_CNT_EN		0x01

/* -----------------------------------------------------------------------
 * mycam004m_init_regs[] -- one-time bring-up, applied once after
 * RESET_N is released and before any per-input or CSI-2 output
 * configuration (see mycam004m_apply_init_sequence() in mycam004m.c).
 *
 * Per-channel enable/disable (PD_VCH) deliberately is NOT in this table
 * -- it's toggled dynamically per stream by mycam004m_enable_camera_input()
 * / mycam004m_disable_camera_input() in mycam004m.c, since that's a
 * runtime operation (stream on/off), not one-time bring-up. This table
 * only sets the resolution/frame-rate mode, which is fixed for the
 * lifetime of this driver (see mycam004m.h).
 * -----------------------------------------------------------------------
 */
static const struct mycam004m_reg mycam004m_init_regs[] = {
	/*
	 * *** If your actual AHD camera heads are 25fps-only (e.g. MYIR's
	 * own QJD6048-2053 panoramic module in the MY-CAM004M docs bundle,
	 * whose spec sheet states a fixed 25fps output with no 30fps
	 * option -- unlike the also-bundled Sony 225 module, which is
	 * PAL/NTSC switchable), forcing AHD_MD=0x02 (30P) here will NOT
	 * make a 25fps-only camera output 30fps -- the decoder needs to be
	 * told what the camera is actually sending, not the other way
	 * around. Confirm which camera head ships on your board; if it's
	 * 25fps-only, change these four to MYCAM004M_AHD_MD_1080P25 (the
	 * chip's own POR default -- you could also just delete these four
	 * lines) AND change MYCAM004M_FPS to 25 in mycam004m.h. ***
	 */
	{ MYCAM004M_BANK_VIDEO_FORMAT, MYCAM004M_REG_AHD_MD(0), MYCAM004M_AHD_MD_1080P30 },
	{ MYCAM004M_BANK_VIDEO_FORMAT, MYCAM004M_REG_AHD_MD(1), MYCAM004M_AHD_MD_1080P30 },
	{ MYCAM004M_BANK_VIDEO_FORMAT, MYCAM004M_REG_AHD_MD(2), MYCAM004M_AHD_MD_1080P30 },
	{ MYCAM004M_BANK_VIDEO_FORMAT, MYCAM004M_REG_AHD_MD(3), MYCAM004M_AHD_MD_1080P30 },
};

/* -----------------------------------------------------------------------
 * mycam004m_csi_output_regs[] -- 4-lane MIPI CSI-2 TX configuration:
 * PLL/PHY clock, virtual-channel assignment, output data type. Applied
 * once, before mycam004m_start_csi_tx() actually enables the link (see
 * mycam004m_configure_csi_output() in mycam004m.c).
 *
 * mycam004m_vc_for_cam() in mycam004m.c and MIPI_CSI2_DT_YUV422_8B in
 * mycam004m_get_frame_desc() both assume this table's effects -- see the
 * per-register comments above for why both are believed correct as-is.
 * -----------------------------------------------------------------------
 */
static const struct mycam004m_reg mycam004m_csi_output_regs[] = {
	/*
	 * MIPI TX PHY/byte-clock PLL configuration for N4's/NVP6324's
	 * fastest of four discrete PHY clock steps (378 / 594 / 756 /
	 * 1242 MHz -- confirmed as an enum of exactly these four values in
	 * NXP's independent i.MX8QM NVP6324 driver). ~1242 Mbps/lane is
	 * comfortably above this board's ~249 Mbps/lane minimum (4x
	 * 1920x1080@30 YUV422 8-bit over 4 lanes) and matches the DT
	 * overlay's link-frequencies value.
	 *
	 * *** NOT in N4's own datasheet *** -- N4's datasheet (Rev 0.0,
	 * preliminary) leaves its entire AC/timing chapter as "TBD" and
	 * does not document registers 0x40-0x43 in BANK21 at all (its
	 * table jumps from 0x3F straight to 0x44). Sourced instead from
	 * two independently-written, real, register-compatible
	 * sibling-chip (NVP6324) drivers, which agree almost exactly:
	 *   - Allwinner jaguar1_mipi.c mipi_tx_init(), "SET_MIPI_1242MHZ
	 *     1080P" branch (in this project's MY-CAM004M/MYD-LT527/bsp
	 *     reference tree) -- the values actually used below.
	 *   - NXP's i.MX8QM NVP6324 driver (0001-add-nvp6324-driver.patch,
	 *     from NXP Community KB "Nvp6324 driver for imx8qm."),
	 *     val_mclk_1242[] / nvp6324_regs_base_mclk[].
	 * 13 of these 17 register values are byte-for-byte identical
	 * between those two independent sources (regs 0x42, 0x43, and all
	 * 13 of 0x10-0x1C). Only 0x40/0x41 (the PLL multiplier/divider
	 * pair) differ: Allwinner's driver (used below, since it's the
	 * one confirmed to be an active/shipped code path, not merely a
	 * table entry the driver never actually selects -- see the
	 * project README) writes 0xB4/0x00; NXP's writes 0xDC/0x10
	 * instead. If the link doesn't lock on real hardware, that byte
	 * pair is the first thing to try swapping.
	 */
	{ MYCAM004M_BANK_MIPI_TX, 0x40, 0xb4 },
	{ MYCAM004M_BANK_MIPI_TX, 0x41, 0x00 },
	{ MYCAM004M_BANK_MIPI_TX, 0x42, 0x03 },
	{ MYCAM004M_BANK_MIPI_TX, 0x43, 0x43 },
	{ MYCAM004M_BANK_MIPI_TX, 0x10, 0x13 },
	{ MYCAM004M_BANK_MIPI_TX, 0x11, 0x08 },
	{ MYCAM004M_BANK_MIPI_TX, 0x12, 0x0b },
	{ MYCAM004M_BANK_MIPI_TX, 0x13, 0x12 },
	{ MYCAM004M_BANK_MIPI_TX, 0x14, 0x2d },
	{ MYCAM004M_BANK_MIPI_TX, 0x15, 0x07 },
	{ MYCAM004M_BANK_MIPI_TX, 0x16, 0x0b },
	{ MYCAM004M_BANK_MIPI_TX, 0x17, 0x02 },
	{ MYCAM004M_BANK_MIPI_TX, 0x18, 0x12 },
	{ MYCAM004M_BANK_MIPI_TX, 0x19, 0x09 },
	{ MYCAM004M_BANK_MIPI_TX, 0x1a, 0x15 },
	{ MYCAM004M_BANK_MIPI_TX, 0x1b, 0x11 },
	{ MYCAM004M_BANK_MIPI_TX, 0x1c, 0x0e },

	/* Frame counter enable -- both sibling drivers write this after
	 * the PLL block above.
	 */
	{ MYCAM004M_BANK_MIPI_TX, MYCAM004M_REG_MIPI_TX_FRAME_CNT, MYCAM004M_MIPI_TX_FRAME_CNT_EN },

	/* Reserved-bit reassertion at 0x2D -- both sibling drivers write
	 * 0x01 here after the PLL block. N4's datasheet documents 0x2D as
	 * a fixed "-1-" reserved bit with POR default 0x01, so this is a
	 * no-op against the reset default, kept only for parity with two
	 * independently-confirmed working init sequences.
	 */
	{ MYCAM004M_BANK_MIPI_TX, 0x2d, 0x01 },

	/* Auto VC-ID insertion (see the register's comment above). */
	{ MYCAM004M_BANK_MIPI_TX, MYCAM004M_REG_MIPI_CH_ID_TYPE, MYCAM004M_MIPI_CH_ID_AUTO },

	/* YUV422 8-bit output data type, one write per AHD input / VC. */
	{ MYCAM004M_BANK_MIPI_TX, MYCAM004M_REG_MIPI_TX_DATA_TYPE(0), MYCAM004M_MIPI_TX_DATA_TYPE_YUV422_8B },
	{ MYCAM004M_BANK_MIPI_TX, MYCAM004M_REG_MIPI_TX_DATA_TYPE(1), MYCAM004M_MIPI_TX_DATA_TYPE_YUV422_8B },
	{ MYCAM004M_BANK_MIPI_TX, MYCAM004M_REG_MIPI_TX_DATA_TYPE(2), MYCAM004M_MIPI_TX_DATA_TYPE_YUV422_8B },
	{ MYCAM004M_BANK_MIPI_TX, MYCAM004M_REG_MIPI_TX_DATA_TYPE(3), MYCAM004M_MIPI_TX_DATA_TYPE_YUV422_8B },
};

#endif /* __MYCAM004M_REGS_H__ */

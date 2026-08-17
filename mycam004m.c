// SPDX-License-Identifier: GPL-2.0-only
/*
 * MYIR MY-CAM004M V4L2 subdevice driver
 *
 * MY-CAM004M is a quad-AHD-camera-input decoder board that outputs a
 * single 4-lane MIPI CSI-2 link, multiplexing all four inputs onto it
 * as four separate virtual channels. It has its own oscillator (no
 * external MCLK needed) and is controlled over I2C plus three GPIO
 * lines (RESET_N, PWREN, PWRDN).
 *
 * This driver only represents/configures the external decoder chip
 * itself, per TI's documented requirement that CSI-2 sources be
 * separate V4L2 subdevices from the AM625 CSI2RX receiver. It is used
 * on BeaglePlay (AM625) via J17, alongside TI's j721e-csi2rx /
 * cdns-csi2rx drivers, which own the D-PHY, CSI-2 bridge, and DMA.
 *
 * *** IMPORTANT ***
 * MY-CAM004M's decoder is confirmed to be Nextchip's "N4"; its register
 * map is populated in mycam004m-regs.h from N4's own datasheet plus two
 * independent register-compatible sibling-chip (NVP6324) drivers -- see
 * the citations there. All of that is *** NOT tested against real
 * MY-CAM004M hardware ***; a few specific values remain flagged
 * lower-confidence where sourced from the sibling drivers rather than
 * N4's own datasheet (which is a preliminary Rev 0.0 missing its AC/
 * timing tables). Search mycam004m-regs.h and this file for remaining
 * TODOs. The V4L2-level plumbing -- media graph, pad/stream topology,
 * formats, virtual-channel frame descriptors -- was already implemented
 * for real before the register map was found and didn't need to change.
 *
 * Media topology:
 *
 *   [pad 0] AHD input 0 (sink, no fwnode link -- analog, not MC)  --\
 *   [pad 1] AHD input 1 (sink, no fwnode link)                     \
 *   [pad 2] AHD input 2 (sink, no fwnode link)                      >--[pad 4] CSI-2 TX (source) --> cdns_csi2rx0
 *   [pad 3] AHD input 3 (sink, no fwnode link)                     /
 *                                                                 -/
 *
 * Each sink pad carries exactly one stream (stream 0) and is routed,
 * by default, 1:1 onto source-pad stream N (N = the sink pad index),
 * matching the modern V4L2 streams/routing model used by TI's own
 * ds90ub960 driver in this tree -- see mycam004m_init_state().
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "mycam004m.h"
#include "mycam004m-regs.h"

struct mycam004m {
	struct i2c_client *client;
	struct regmap *regmap;

	struct v4l2_subdev sd;
	struct media_pad pads[MYCAM004M_NUM_PADS];
	struct v4l2_ctrl_handler ctrl_handler;

	/* RESET_N, PWREN, PWRDN -- see the DT overlay for how these map to
	 * BeaglePlay J17 on this board. All three are requested as
	 * optional so the driver doesn't hard-fail on carrier boards that
	 * don't wire all of them (BeaglePlay itself only wires two -- see
	 * the overlay).
	 */
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwren_gpio;
	struct gpio_desc *pwrdn_gpio;

	/* Parsed from the single (source/TX) endpoint in DT. */
	s64 link_freq[1];

	/* Bitmask (indexed by source-pad stream number, i.e. by virtual
	 * channel) of currently-enabled streams.
	 */
	u64 stream_enable_mask;
	bool streaming;
};

static inline struct mycam004m *to_mycam004m(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mycam004m, sd);
}

/*
 * Confirmed via N4's own datasheet: MIPI_CH_ID_TYPE (bank 0x21, reg
 * 0x2E, written in mycam004m_csi_output_regs[] -- see mycam004m-regs.h)
 * set to auto-insertion mode assigns VC N to AHD input N automatically,
 * matching this identity mapping. NOT tested against real hardware --
 * a real-world TI E2E thread about this exact chip reported a total
 * no-video failure traced back to virtual-channel configuration, so
 * verify this carefully once a board is available rather than trusting
 * it by inspection alone.
 */
static u8 mycam004m_vc_for_cam(unsigned int cam_idx)
{
	return cam_idx;
}

/* -----------------------------------------------------------------------
 * Register access
 * -----------------------------------------------------------------------
 */

static int mycam004m_write_regs(struct mycam004m *cam,
				  const struct mycam004m_reg *regs,
				  unsigned int n_regs)
{
	struct device *dev = &cam->client->dev;
	/* No entry's .bank (a u8) can ever equal -1, so the first entry
	 * always selects its bank rather than assuming one's already
	 * selected.
	 */
	int cur_bank = -1;
	unsigned int i;
	int ret;

	for (i = 0; i < n_regs; i++) {
		if (regs[i].bank != cur_bank) {
			ret = regmap_write(cam->regmap, MYCAM004M_REG_BANK_SEL,
					    regs[i].bank);
			if (ret) {
				dev_err(dev, "failed selecting bank 0x%02x: %d\n",
					regs[i].bank, ret);
				return ret;
			}
			cur_bank = regs[i].bank;
		}

		ret = regmap_write(cam->regmap, regs[i].reg, regs[i].val);
		if (ret) {
			dev_err(dev, "failed writing bank 0x%02x reg 0x%04x = 0x%02x: %d\n",
				regs[i].bank, regs[i].reg, regs[i].val, ret);
			return ret;
		}
	}

	return 0;
}

/* One-time chip bring-up, applied right after RESET_N release. */
static int mycam004m_apply_init_sequence(struct mycam004m *cam)
{
	return mycam004m_write_regs(cam, mycam004m_init_regs,
				     ARRAY_SIZE(mycam004m_init_regs));
}

/*
 * 4-lane MIPI CSI-2 TX configuration: lane count/mapping, output pixel
 * format, per-input virtual-channel assignment. See the long comment in
 * mycam004m-regs.h -- mycam004m_vc_for_cam() and the CSI2 data type in
 * mycam004m_get_frame_desc() both need to stay in sync with whatever
 * this ends up actually programming.
 */
static int mycam004m_configure_csi_output(struct mycam004m *cam)
{
	return mycam004m_write_regs(cam, mycam004m_csi_output_regs,
				     ARRAY_SIZE(mycam004m_csi_output_regs));
}

/*
 * Per-AHD-input enable/disable: toggles PD_VCH_<cam_idx> (bank 0, reg
 * 0x00+cam_idx, bit0, active high) -- see mycam004m-regs.h. Single
 * register per channel, confirmed directly from the N4 datasheet; no
 * table needed for something this small.
 */
static int mycam004m_enable_camera_input(struct mycam004m *cam,
					   unsigned int cam_idx)
{
	const struct mycam004m_reg reg = {
		MYCAM004M_BANK_VIDEO_FORMAT,
		MYCAM004M_REG_PD_VCH(cam_idx),
		MYCAM004M_PD_VCH_NORMAL,
	};

	return mycam004m_write_regs(cam, &reg, 1);
}

static int mycam004m_disable_camera_input(struct mycam004m *cam,
					    unsigned int cam_idx)
{
	const struct mycam004m_reg reg = {
		MYCAM004M_BANK_VIDEO_FORMAT,
		MYCAM004M_REG_PD_VCH(cam_idx),
		MYCAM004M_PD_VCH_POWERDOWN,
	};

	return mycam004m_write_regs(cam, &reg, 1);
}

/* Starts/stops the physical CSI-2 transmitter (PLL, D-PHY lanes) via
 * bank 0x21 reg 0x07 -- see its comment in mycam004m-regs.h. Kept
 * separate from mycam004m_configure_csi_output() so that "configure the
 * link" and "turn the link on" can be sequenced independently, matching
 * how ds90ub960.c (this tree's reference multiplexed-stream decoder
 * driver) structures TX port enable.
 */
static int mycam004m_start_csi_tx(struct mycam004m *cam)
{
	const struct mycam004m_reg reg = {
		MYCAM004M_BANK_MIPI_TX,
		MYCAM004M_REG_MIPI_TX_CTRL,
		MYCAM004M_MIPI_TX_CTRL_START,
	};

	return mycam004m_write_regs(cam, &reg, 1);
}

static int mycam004m_stop_csi_tx(struct mycam004m *cam)
{
	const struct mycam004m_reg reg = {
		MYCAM004M_BANK_MIPI_TX,
		MYCAM004M_REG_MIPI_TX_CTRL,
		MYCAM004M_MIPI_TX_CTRL_STOP,
	};

	return mycam004m_write_regs(cam, &reg, 1);
}

/* -----------------------------------------------------------------------
 * Power / reset sequencing
 * -----------------------------------------------------------------------
 */

static void mycam004m_power_on(struct mycam004m *cam)
{
	struct device *dev = &cam->client->dev;

	/* MY-CAM004M has its own oscillator -- no MCLK to enable here. */

	gpiod_set_value_cansleep(cam->pwren_gpio, 1);

	/*
	 * Confirmed: PWREN gates the board's SGM2028-ADJ LDO (its EN pin,
	 * wired directly, no inversion -- confirmed from the MY-CAM004M
	 * schematic) that supplies N4's 3.3V rail. SGM2028's own datasheet
	 * "Shutdown Exit Delay" spec is 30us typical (VOUT to 90% of
	 * final value). 5-10ms here is a comfortable margin above that,
	 * not a guess.
	 */
	usleep_range(5000, 10000);

	/* PWRDN is unmanaged on this board (unwired in the overlay -- see
	 * its DT comment); this is a no-op there. Kept for boards/revisions
	 * that do wire it.
	 */
	gpiod_set_value_cansleep(cam->pwrdn_gpio, 0);

	gpiod_set_value_cansleep(cam->reset_gpio, 0); /* release RESET_N */

	/*
	 * Confirmed: N4's datasheet AC characteristics give RSTB release
	 * time (low-to-high) as >=10us minimum before the chip is ready.
	 * 10-20ms here is a large margin above that minimum, not a guess
	 * -- kept this conservative since there's no board to time it
	 * against yet.
	 */
	usleep_range(10000, 20000);

	dev_dbg(dev, "power-on sequence complete\n");
}

static void mycam004m_power_off(struct mycam004m *cam)
{
	gpiod_set_value_cansleep(cam->reset_gpio, 1); /* assert RESET_N */
	gpiod_set_value_cansleep(cam->pwrdn_gpio, 1);
	gpiod_set_value_cansleep(cam->pwren_gpio, 0);
}

/* -----------------------------------------------------------------------
 * V4L2 subdev pad ops: routing, format, frame descriptors
 * -----------------------------------------------------------------------
 */

static int mycam004m_set_routing(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_krouting *routing)
{
	static const struct v4l2_mbus_framefmt format = {
		.width = MYCAM004M_WIDTH,
		.height = MYCAM004M_HEIGHT,
		.code = MEDIA_BUS_FMT_YUYV8_1X16,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.ycbcr_enc = V4L2_YCBCR_ENC_601,
		.quantization = V4L2_QUANTIZATION_LIM_RANGE,
		.xfer_func = V4L2_XFER_FUNC_SRGB,
	};
	unsigned int i;
	int ret;

	if (routing->num_routes > MYCAM004M_NUM_CAMS)
		return -E2BIG;

	ret = v4l2_subdev_routing_validate(sd, routing,
					    V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
					    V4L2_SUBDEV_ROUTING_NO_SINK_STREAM_MIX);
	if (ret)
		return ret;

	/*
	 * MY-CAM004M's internal AHD-input -> virtual-channel mapping is
	 * fixed identity (input N -> VC N) via the auto-VC-insertion mode
	 * programmed in mycam004m_configure_csi_output() -- see
	 * MYCAM004M_REG_MIPI_CH_ID_TYPE in mycam004m-regs.h. N4 also has a
	 * manual VC-insertion mode, but it's a single global channel-number
	 * register, not independently reprogrammable per AHD input, so
	 * there's no non-identity routing this driver could actually
	 * program the hardware to match. Only accept the identity routing
	 * this driver installs by default and reject anything else.
	 */
	for (i = 0; i < routing->num_routes; i++) {
		const struct v4l2_subdev_route *route = &routing->routes[i];

		if (route->source_pad != MYCAM004M_PAD_SOURCE ||
		    route->sink_stream != 0 ||
		    route->source_stream != route->sink_pad)
			return -EINVAL;
	}

	return v4l2_subdev_set_routing_with_fmt(sd, state, routing, &format);
}

static int mycam004m_pad_set_routing(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       enum v4l2_subdev_format_whence which,
				       struct v4l2_subdev_krouting *routing)
{
	struct mycam004m *cam = to_mycam004m(sd);

	if (which == V4L2_SUBDEV_FORMAT_ACTIVE && cam->streaming)
		return -EBUSY;

	return mycam004m_set_routing(sd, state, routing);
}

static int mycam004m_init_state(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[MYCAM004M_NUM_CAMS];
	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};
	unsigned int i;

	for (i = 0; i < MYCAM004M_NUM_CAMS; i++) {
		routes[i] = (struct v4l2_subdev_route){
			.sink_pad = i,
			.sink_stream = 0,
			.source_pad = MYCAM004M_PAD_SOURCE,
			.source_stream = i,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		};
	}

	return mycam004m_set_routing(sd, state, &routing);
}

static int mycam004m_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *format)
{
	struct mycam004m *cam = to_mycam004m(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE && cam->streaming)
		return -EBUSY;

	/*
	 * Fixed-function decoder with no register map to negotiate
	 * anything else yet -- always force the one target mode from the
	 * task brief, regardless of what was requested, same as how many
	 * fixed-mode bridge/decoder chips behave. See MYCAM004M_WIDTH /
	 * _HEIGHT in mycam004m.h.
	 */
	format->format.width = MYCAM004M_WIDTH;
	format->format.height = MYCAM004M_HEIGHT;
	format->format.code = MEDIA_BUS_FMT_YUYV8_1X16;
	format->format.field = V4L2_FIELD_NONE;
	format->format.colorspace = V4L2_COLORSPACE_SRGB;
	format->format.ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->format.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	format->format.xfer_func = V4L2_XFER_FUNC_SRGB;

	fmt = v4l2_subdev_state_get_format(state, format->pad, format->stream);
	if (!fmt)
		return -EINVAL;
	*fmt = format->format;

	/* Keep the paired stream on the other side of the route in sync,
	 * same as ds90ub960_set_fmt() in this tree.
	 */
	fmt = v4l2_subdev_state_get_opposite_stream_format(state, format->pad,
							     format->stream);
	if (fmt)
		*fmt = format->format;

	return 0;
}

static int mycam004m_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				      struct v4l2_mbus_frame_desc *fd)
{
	struct v4l2_subdev_state *state;
	struct v4l2_subdev_route *route;
	int ret = 0;

	if (pad != MYCAM004M_PAD_SOURCE)
		return -EINVAL;

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	for_each_active_route(&state->routing, route) {
		struct v4l2_mbus_frame_desc_entry *entry;

		if (route->source_pad != pad)
			continue;

		if (fd->num_entries >= V4L2_FRAME_DESC_ENTRY_MAX) {
			ret = -E2BIG;
			break;
		}

		entry = &fd->entry[fd->num_entries];
		entry->stream = route->source_stream;
		entry->flags = 0;
		entry->length = 0;
		entry->pixelcode = MEDIA_BUS_FMT_YUYV8_1X16;

		/*
		 * VC assignment: see mycam004m_vc_for_cam(). This is the
		 * value TI's j721e-csi2rx driver reads (via get_frame_desc)
		 * to program its own per-context VC filter -- getting it
		 * wrong here means the wrong camera's data lands on the
		 * wrong /dev/videoN, not just a cosmetic mismatch.
		 *
		 * 0x1e is the standard MIPI CSI-2 wire code for 8-bit YUV422
		 * -- not MYIR-specific -- and is confirmed to be what
		 * mycam004m_configure_csi_output() actually programs the
		 * chip to emit (MYCAM004M_MIPI_TX_DATA_TYPE_YUV422_8B in
		 * mycam004m-regs.h).
		 */
		entry->bus.csi2.vc = mycam004m_vc_for_cam(route->sink_pad);
		entry->bus.csi2.dt = MIPI_CSI2_DT_YUV422_8B;

		fd->num_entries++;
	}

	v4l2_subdev_unlock_state(state);

	return ret;
}

/*
 * TODO / deliberate omission: no .get_frame_interval /
 * .set_frame_interval. The target brief specifies 30fps, but that's
 * hardware-determined (no register map to negotiate anything else, same
 * situation as MYCAM004M_WIDTH/_HEIGHT above) and ds90ub960.c in this
 * tree doesn't implement these ops either for the same reason. If a
 * downstream consumer ever queries this, add
 * .get_frame_interval returning a fixed { .numerator = 1, .denominator
 * = MYCAM004M_FPS } -- not done now because nothing in the capture path
 * requires it (frame timing comes from real DMA, not this).
 */

/* -----------------------------------------------------------------------
 * V4L2 subdev pad ops: stream enable/disable
 * -----------------------------------------------------------------------
 */

static int mycam004m_enable_streams(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      u32 pad, u64 streams_mask)
{
	struct mycam004m *cam = to_mycam004m(sd);
	struct device *dev = &cam->client->dev;
	struct v4l2_subdev_route *route;
	u64 newly_enabled = streams_mask & ~cam->stream_enable_mask;
	int ret;

	if (pad != MYCAM004M_PAD_SOURCE)
		return -EINVAL;

	if (!cam->stream_enable_mask) {
		dev_dbg(dev, "first stream: applying init sequence + CSI output config\n");

		ret = mycam004m_apply_init_sequence(cam);
		if (ret) {
			dev_err(dev, "init sequence failed: %d\n", ret);
			return ret;
		}

		ret = mycam004m_configure_csi_output(cam);
		if (ret) {
			dev_err(dev, "CSI-2 output configuration failed: %d\n", ret);
			return ret;
		}
	}

	for_each_active_route(&state->routing, route) {
		if (route->source_pad != pad)
			continue;
		if (!(newly_enabled & BIT_ULL(route->source_stream)))
			continue;

		dev_info(dev, "enabling AHD input %u -> stream %u (VC %u)\n",
			 route->sink_pad, route->source_stream,
			 mycam004m_vc_for_cam(route->sink_pad));

		ret = mycam004m_enable_camera_input(cam, route->sink_pad);
		if (ret) {
			dev_err(dev, "failed enabling AHD input %u: %d\n",
				route->sink_pad, ret);
			goto err_disable_newly_enabled;
		}
	}

	if (!cam->stream_enable_mask) {
		ret = mycam004m_start_csi_tx(cam);
		if (ret) {
			dev_err(dev, "failed starting CSI-2 TX: %d\n", ret);
			goto err_disable_newly_enabled;
		}
	}

	cam->stream_enable_mask |= streams_mask;
	cam->streaming = true;

	dev_dbg(dev, "streams 0x%llx enabled (mask now 0x%llx)\n",
		streams_mask, cam->stream_enable_mask);

	return 0;

err_disable_newly_enabled:
	for_each_active_route(&state->routing, route) {
		if (route->source_pad != pad)
			continue;
		if (!(newly_enabled & BIT_ULL(route->source_stream)))
			continue;
		mycam004m_disable_camera_input(cam, route->sink_pad);
	}

	return ret;
}

static int mycam004m_disable_streams(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       u32 pad, u64 streams_mask)
{
	struct mycam004m *cam = to_mycam004m(sd);
	struct device *dev = &cam->client->dev;
	struct v4l2_subdev_route *route;

	if (pad != MYCAM004M_PAD_SOURCE)
		return -EINVAL;

	for_each_active_route(&state->routing, route) {
		if (route->source_pad != pad)
			continue;
		if (!(streams_mask & BIT_ULL(route->source_stream)))
			continue;

		dev_info(dev, "disabling AHD input %u -> stream %u (VC %u)\n",
			 route->sink_pad, route->source_stream,
			 mycam004m_vc_for_cam(route->sink_pad));

		mycam004m_disable_camera_input(cam, route->sink_pad);
	}

	cam->stream_enable_mask &= ~streams_mask;

	if (!cam->stream_enable_mask) {
		mycam004m_stop_csi_tx(cam);
		cam->streaming = false;
	}

	dev_dbg(dev, "streams 0x%llx disabled (mask now 0x%llx)\n",
		streams_mask, cam->stream_enable_mask);

	return 0;
}

/* -----------------------------------------------------------------------
 * Subdev ops tables
 * -----------------------------------------------------------------------
 */

static const struct v4l2_subdev_internal_ops mycam004m_internal_ops = {
	.init_state = mycam004m_init_state,
};

static const struct v4l2_subdev_pad_ops mycam004m_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mycam004m_set_fmt,
	.set_routing = mycam004m_pad_set_routing,
	.get_frame_desc = mycam004m_get_frame_desc,
	.enable_streams = mycam004m_enable_streams,
	.disable_streams = mycam004m_disable_streams,
};

static const struct v4l2_subdev_ops mycam004m_subdev_ops = {
	.pad = &mycam004m_pad_ops,
};

/* -----------------------------------------------------------------------
 * Probe / remove
 * -----------------------------------------------------------------------
 */

/*
 * Confirmed 8-bit register address / 8-bit value against N4's own I2C
 * protocol chapter and the sibling driver's actual i2c_master_send()
 * calls -- see mycam004m-regs.h. MYCAM004M_REG_BANK_SEL (written by
 * mycam004m_write_regs()) extends the effective address space via
 * banking rather than needing a wider reg_bits here.
 */
static const struct regmap_config mycam004m_regmap_config = {
	.name = "mycam004m",
	.reg_bits = 8,
	.val_bits = 8,
};

static int mycam004m_parse_dt(struct mycam004m *cam)
{
	struct device *dev = &cam->client->dev;
	struct fwnode_handle *ep_fwnode;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	int ret;

	ep_fwnode = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!ep_fwnode)
		return dev_err_probe(dev, -ENODEV, "no endpoint found in DT\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep_fwnode, &vep);
	fwnode_handle_put(ep_fwnode);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (vep.bus.mipi_csi2.num_data_lanes != MYCAM004M_NUM_LANES) {
		dev_err(dev,
			"unsupported data-lanes count %u (MY-CAM004M is a %u-lane device)\n",
			vep.bus.mipi_csi2.num_data_lanes, MYCAM004M_NUM_LANES);
		ret = -EINVAL;
		goto out_free_vep;
	}

	if (vep.nr_of_link_frequencies != 1) {
		dev_err(dev,
			"expected exactly one 'link-frequencies' entry in DT, got %u\n",
			vep.nr_of_link_frequencies);
		ret = -EINVAL;
		goto out_free_vep;
	}

	/*
	 * ~1242 Mbps/lane, sourced from register-compatible sibling-chip
	 * drivers rather than N4's own datasheet (whose AC/timing tables
	 * are left "TBD") -- see the link-frequencies comment in the DT
	 * overlay for the full citation. Whatever's in DT is taken as
	 * authoritative here; there is no hardcoded fallback, so revising
	 * this remains a DT-only change if real hardware disagrees.
	 */
	cam->link_freq[0] = vep.link_frequencies[0];

	ret = 0;

out_free_vep:
	v4l2_fwnode_endpoint_free(&vep);
	return ret;
}

/* Chip-ID readback: confirms the I2C link works and that whatever's at
 * this address is actually an N4 before going any further. Bank 0 reg
 * 0xF4 (DEV_ID) and 0xF5 (REV_ID) are read-only -- confirmed directly
 * from N4's own datasheet ("It shows Device ID (N4 = 0xB0)").
 */
static int mycam004m_check_chip_id(struct mycam004m *cam)
{
	struct device *dev = &cam->client->dev;
	unsigned int dev_id, rev_id;
	int ret;

	ret = regmap_write(cam->regmap, MYCAM004M_REG_BANK_SEL,
			    MYCAM004M_BANK_VIDEO_FORMAT);
	if (ret)
		return dev_err_probe(dev, ret,
				      "failed selecting bank 0x%02x for chip-ID readback\n",
				      MYCAM004M_BANK_VIDEO_FORMAT);

	ret = regmap_read(cam->regmap, MYCAM004M_REG_DEV_ID, &dev_id);
	if (ret)
		return dev_err_probe(dev, ret, "failed reading DEV_ID\n");

	if (dev_id != MYCAM004M_DEV_ID_VAL)
		return dev_err_probe(dev, -ENODEV,
				      "unexpected DEV_ID 0x%02x (expected 0x%02x for N4) -- wrong I2C address, dead chip, or not an N4?\n",
				      dev_id, MYCAM004M_DEV_ID_VAL);

	ret = regmap_read(cam->regmap, MYCAM004M_REG_REV_ID, &rev_id);
	if (ret)
		return dev_err_probe(dev, ret, "failed reading REV_ID\n");

	dev_info(dev, "found N4 decoder: DEV_ID 0x%02x, REV_ID 0x%02x\n",
		 dev_id, rev_id);

	return 0;
}

static int mycam004m_init_controls(struct mycam004m *cam)
{
	struct v4l2_ctrl *ctrl;

	v4l2_ctrl_handler_init(&cam->ctrl_handler, 1);

	/*
	 * cdns-csi2rx.c (this tree's Cadence CSI-2 RX bridge, underneath
	 * TI's j721e-csi2rx shim) calls v4l2_get_link_freq() against this
	 * subdev's control handler to configure the D-PHY -- this control
	 * is required, not cosmetic.
	 */
	ctrl = v4l2_ctrl_new_int_menu(&cam->ctrl_handler, NULL,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(cam->link_freq) - 1, 0,
				       cam->link_freq);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (cam->ctrl_handler.error) {
		int ret = cam->ctrl_handler.error;

		v4l2_ctrl_handler_free(&cam->ctrl_handler);
		return ret;
	}

	cam->sd.ctrl_handler = &cam->ctrl_handler;

	return 0;
}

static int mycam004m_create_subdev(struct mycam004m *cam)
{
	struct device *dev = &cam->client->dev;
	unsigned int i;
	int ret;

	v4l2_i2c_subdev_init(&cam->sd, cam->client, &mycam004m_subdev_ops);
	cam->sd.internal_ops = &mycam004m_internal_ops;

	ret = mycam004m_init_controls(cam);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init controls\n");

	cam->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			  V4L2_SUBDEV_FL_HAS_EVENTS |
			  V4L2_SUBDEV_FL_STREAMS;
	cam->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	for (i = 0; i < MYCAM004M_NUM_CAMS; i++)
		cam->pads[i].flags = MEDIA_PAD_FL_SINK;
	cam->pads[MYCAM004M_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&cam->sd.entity, MYCAM004M_NUM_PADS,
				      cam->pads);
	if (ret)
		goto err_free_ctrl;

	/* Tie the subdev active-state lock to the control handler's lock,
	 * same as ds90ub960.c in this tree -- avoids a second private
	 * mutex for what would otherwise be redundant locking between
	 * controls and streaming state.
	 */
	cam->sd.state_lock = cam->ctrl_handler.lock;

	ret = v4l2_subdev_init_finalize(&cam->sd);
	if (ret)
		goto err_entity_cleanup;

	ret = v4l2_async_register_subdev(&cam->sd);
	if (ret) {
		dev_err(dev, "failed to register subdev: %d\n", ret);
		goto err_subdev_cleanup;
	}

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(&cam->sd);
err_entity_cleanup:
	media_entity_cleanup(&cam->sd.entity);
err_free_ctrl:
	v4l2_ctrl_handler_free(&cam->ctrl_handler);

	return ret;
}

static void mycam004m_destroy_subdev(struct mycam004m *cam)
{
	v4l2_async_unregister_subdev(&cam->sd);
	v4l2_subdev_cleanup(&cam->sd);
	media_entity_cleanup(&cam->sd.entity);
	v4l2_ctrl_handler_free(&cam->ctrl_handler);
}

static int mycam004m_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mycam004m *cam;
	int ret;

	cam = devm_kzalloc(dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->client = client;
	i2c_set_clientdata(client, cam);

	cam->regmap = devm_regmap_init_i2c(client, &mycam004m_regmap_config);
	if (IS_ERR(cam->regmap))
		return dev_err_probe(dev, PTR_ERR(cam->regmap),
				      "failed to init regmap\n");

	/*
	 * Confirmed against the MY-CAM004M schematic: RESET_N wires
	 * straight through (no inverter) to N4's RSTB pin, documented
	 * "System Reset (Active Low)" -- GPIO_ACTIVE_LOW in the DT overlay
	 * is correct. PWREN wires straight through (0R, no inversion) to
	 * the EN pin of the board's SGM2028-ADJ LDO, a standard
	 * active-high enable -- GPIO_ACTIVE_HIGH in the DT overlay is also
	 * correct. This driver only ever deals in logical
	 * asserted(1)/deasserted(0) values either way, so a polarity fix
	 * (if this ever turns out wrong against a real board) would still
	 * only be a DT-only change.
	 *
	 * All three are optional: BeaglePlay's J17 only wires two GPIOs to
	 * the camera header (see the DT overlay), and other boards may tie
	 * some of these lines active in hardware instead of exposing a
	 * GPIO at all. gpiod_set_value_cansleep() on a NULL descriptor is
	 * a safe no-op.
	 */
	cam->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(cam->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(cam->reset_gpio),
				      "failed to get reset GPIO\n");

	cam->pwren_gpio = devm_gpiod_get_optional(dev, "pwren", GPIOD_OUT_LOW);
	if (IS_ERR(cam->pwren_gpio))
		return dev_err_probe(dev, PTR_ERR(cam->pwren_gpio),
				      "failed to get pwren GPIO\n");

	cam->pwrdn_gpio = devm_gpiod_get_optional(dev, "pwrdn", GPIOD_OUT_LOW);
	if (IS_ERR(cam->pwrdn_gpio))
		return dev_err_probe(dev, PTR_ERR(cam->pwrdn_gpio),
				      "failed to get pwrdn GPIO\n");

	ret = mycam004m_parse_dt(cam);
	if (ret)
		return ret;

	mycam004m_power_on(cam);

	ret = mycam004m_check_chip_id(cam);
	if (ret)
		goto err_power_off;

	ret = mycam004m_create_subdev(cam);
	if (ret)
		goto err_power_off;

	dev_info(dev, "probed at I2C address 0x%02x, %u data lanes, link-freq %lld Hz\n",
		 client->addr, MYCAM004M_NUM_LANES, cam->link_freq[0]);

	return 0;

err_power_off:
	mycam004m_power_off(cam);
	return ret;
}

static void mycam004m_remove(struct i2c_client *client)
{
	struct mycam004m *cam = i2c_get_clientdata(client);

	mycam004m_destroy_subdev(cam);
	mycam004m_power_off(cam);
}

static const struct i2c_device_id mycam004m_id[] = {
	{ "mycam004m", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, mycam004m_id);

static const struct of_device_id mycam004m_of_ids[] = {
	/*
	 * TODO: "myir" is not (yet) a registered vendor prefix in
	 * Documentation/devicetree/bindings/vendor-prefixes.yaml. Harmless
	 * for out-of-tree/module use (of_device_id matching doesn't care),
	 * but would need registering before any upstream submission.
	 */
	{ .compatible = "myir,mycam004m" },
	{}
};
MODULE_DEVICE_TABLE(of, mycam004m_of_ids);

static struct i2c_driver mycam004m_driver = {
	.probe = mycam004m_probe,
	.remove = mycam004m_remove,
	.id_table = mycam004m_id,
	.driver = {
		.name = "mycam004m",
		.of_match_table = mycam004m_of_ids,
	},
};
module_i2c_driver(mycam004m_driver);

MODULE_DESCRIPTION("MYIR MY-CAM004M quad-AHD to 4-lane MIPI CSI-2 decoder driver");
MODULE_AUTHOR("Josh Ellis");
MODULE_LICENSE("GPL");

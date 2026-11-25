// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung AMS641RW01 1080x2340 60Hz AMOLED DSI command-mode panel driver,
 * as found in the OPPO R17 (PBEM00).
 *
 * The panel is driven with the vendor command sequences for power-on,
 * power-off, AOD (LP1/NOLP), HBM and the SEED colour modes.  The ESD
 * protection reads the Get Power Mode register over the HS link and
 * power-cycles the panel in place when the answer is not 0x9f.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define AMS641RW01_SEED_SRGB		0
#define AMS641RW01_SEED_SEED1		1
#define AMS641RW01_SEED_DCIP3		2
#define AMS641RW01_SEED_OFF		3

/* ESD polling cadence and the Get Power Mode answer of a healthy,
 *  fully-on panel (booster on, display on, normal mode).
 */
#define AMS641RW01_ESD_INTERVAL_MS	5000
#define AMS641RW01_POWER_MODE_OK	0x9f

/* Longest packet in the sequences is the 22 byte CRC LUT write; keep one
 *  slot of headroom so the CMD() zero-fill initializer never aliases the
 *  payload.
 */
#define AMS641RW01_CMD_DATA_MAX		24

struct ams641rw01_cmd {
	u8 delay_ms;
	u8 len;
	u8 data[AMS641RW01_CMD_DATA_MAX];
};

#define CMD(delay, ...) \
	{ (delay), sizeof((u8[]){ __VA_ARGS__ }), { __VA_ARGS__ } }

struct ams641rw01 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct backlight_device *backlight;

	struct mutex lock;
	bool prepared;
	bool enabled;
	bool displayed;
	bool hbm;
	bool aod;
	u8 seed;

	u32 peak_brightness;
	u32 color_primaries[8];
	bool has_hdr;

	struct delayed_work esd_work;
};

static const struct regulator_bulk_data ams641rw01_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vdda" },
};

/*
 * Unlock/lock the panel test-key levels.  Level 1 is register 0x9f, level
 * 2 is 0xf0 and level 3 is 0xfc; several sequences below mix them, so keep
 * the raw packets instead of macros.
 */

static const struct ams641rw01_cmd ams641rw01_on_seq[] = {
	/* Unlock level 1, sleep out, lock level 1 */
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(5,   0x11),
	CMD(0,   0x9f, 0x5a, 0x5a),
	/* FD setting */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x01),
	CMD(0,   0xcd, 0x01),
	CMD(0,   0xf0, 0xa5, 0xa5),
	/* Power sequence setting */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x70),
	CMD(0,   0xb9, 0xd7),
	CMD(0,   0xb0, 0x07),
	CMD(0,   0xef, 0x45, 0x11, 0x31, 0x11),
	CMD(0,   0xb0, 0x6e),
	CMD(0,   0xb9, 0x81),
	CMD(0,   0xb0, 0x09),
	CMD(0,   0xef, 0x31),
	CMD(20,  0xf0, 0xa5, 0xa5),
	/* Tearing effect line on */
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(0,   0x35, 0x00),
	CMD(0,   0x9f, 0x5a, 0x5a),
	/* MIC setting */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xeb, 0x17, 0x41, 0x92, 0x0e, 0x10, 0x82, 0x5a),
	CMD(0,   0xf0, 0xa5, 0xa5),
	/* Column and page address setting */
	CMD(0,   0x2a, 0x00, 0x00, 0x04, 0x37),
	CMD(0,   0x2b, 0x00, 0x00, 0x09, 0x23),
	/* ESD setting */
	CMD(0,   0xfc, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x01),
	CMD(0,   0xe3, 0x88),
	CMD(0,   0xb0, 0x07),
	CMD(0,   0xed, 0x67),
	CMD(60,  0xfc, 0xa5, 0xa5),
	/* Backlight dimming setting */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x07),
	CMD(0,   0xb7, 0x01),
	CMD(0,   0xb0, 0x08),
	CMD(0,   0xb7, 0x12),
	CMD(0,   0xf0, 0xa5, 0xa5),
	CMD(0,   0x53, 0x28),
	/* ACL off */
	CMD(0,   0x55, 0x00),
	/* CRC mode enable */
	CMD(0,   0x81, 0x90),
	/* CRC LUT */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x02),
	CMD(0,   0xb1, 0xe0, 0x00, 0x08, 0x10, 0xff, 0x00, 0x00, 0x00, 0xff,
		 0x15, 0xfd, 0xf0, 0xf0, 0x00, 0xf0, 0xec, 0xf0, 0x00, 0xff,
		 0xff, 0xfa),
	CMD(0,   0xb1, 0x00, 0x00),
	CMD(0,   0xf0, 0xa5, 0xa5),
	/* SEED TCS on */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x23),
	CMD(0,   0xb3, 0x91),
	CMD(0,   0x83, 0x80),
	CMD(0,   0xb3, 0x00, 0xc0),
	CMD(0,   0xf0, 0xa5, 0xa5),
};

/* Sent after the DCS brightness has been programmed: display on. */
static const struct ams641rw01_cmd ams641rw01_post_on_seq[] = {
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(0,   0x29),
	CMD(0,   0x9f, 0x5a, 0x5a),
};

static const struct ams641rw01_cmd ams641rw01_off_seq[] = {
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(17,  0x28),
	CMD(0,   0x9f, 0x5a, 0x5a),
	/* LTPS off setting */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x50),
	CMD(0,   0xb9, 0x82),
	CMD(17,  0xf0, 0xa5, 0xa5),
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(0,   0x10),
	CMD(0,   0x9f, 0x5a, 0x5a),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x05),
	CMD(0,   0xf4, 0x01),
	CMD(0,   0xb0, 0x01),
	CMD(0,   0xcd, 0x02),
	CMD(150, 0xf0, 0xa5, 0xa5),
};

/* Always-on-display entry: display off, LTPS off, then a low-power mode
 *  re-init that leaves the panel self-refreshing the AOD clock.
 */
static const struct ams641rw01_cmd ams641rw01_lp1_seq[] = {
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(17,  0x28),
	CMD(0,   0x9f, 0x5a, 0x5a),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x50),
	CMD(0,   0xb9, 0x82),
	CMD(17,  0xf0, 0xa5, 0xa5),
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(0,   0x10),
	CMD(0,   0x9f, 0x5a, 0x5a),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x05),
	CMD(0,   0xf4, 0x01),
	CMD(120, 0xf0, 0xa5, 0xa5),
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(5,   0x11),
	CMD(0,   0x9f, 0x5a, 0x5a),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x01),
	CMD(0,   0xcd, 0x02),
	CMD(0,   0xf0, 0xa5, 0xa5),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x70),
	CMD(0,   0xb9, 0xd7),
	CMD(0,   0xb0, 0x07),
	CMD(0,   0xef, 0x45, 0x11, 0x31, 0x11),
	CMD(0,   0xb0, 0x6e),
	CMD(0,   0xb9, 0x81),
	CMD(0,   0xb0, 0x09),
	CMD(0,   0xef, 0x31),
	CMD(115, 0xf0, 0xa5, 0xa5),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0x53, 0x22),
	CMD(0,   0xb0, 0xa5),
	CMD(0,   0xc7, 0x00),
	CMD(0,   0xf0, 0xa5, 0xa5),
	CMD(0,   0x81, 0x00),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb1, 0x00, 0x01),
	CMD(0,   0xb3, 0x00, 0xc1),
	CMD(0,   0xf0, 0xa5, 0xa5),
};

static const struct ams641rw01_cmd ams641rw01_nolp_seq[] = {
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x01),
	CMD(0,   0xcd, 0x01),
	CMD(0,   0xf0, 0xa5, 0xa5),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x07),
	CMD(0,   0xb7, 0x01),
	CMD(0,   0xb0, 0x08),
	CMD(0,   0xb7, 0x12),
	CMD(0,   0xf0, 0xa5, 0xa5),
	CMD(0,   0x53, 0x20),
	/* CRC mode enable */
	CMD(0,   0x81, 0x90),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x02),
	CMD(0,   0xb1, 0xe0, 0x00, 0x08, 0x10, 0xff, 0x00, 0x00, 0x00, 0xff,
		 0x15, 0xfd, 0xf0, 0xf0, 0x00, 0xf0, 0xec, 0xf0, 0x00, 0xff,
		 0xff, 0xfa),
	CMD(0,   0xb1, 0x00, 0x00),
	CMD(0,   0xf0, 0xa5, 0xa5),
	/* SEED TCS on */
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x23),
	CMD(0,   0xb3, 0x91),
	CMD(0,   0x83, 0x80),
	CMD(0,   0xb3, 0x00, 0xc0),
	CMD(0,   0xf0, 0xa5, 0xa5),
	CMD(0,   0x9f, 0xa5, 0xa5),
	CMD(0,   0x29),
	CMD(0,   0x9f, 0x5a, 0x5a),
};

/* HBM is a fixed maximum-brightness write plus a high-brightness-mode
 *  control display flag; in AOD the same pair exists with a shorter settle.
 */
static const struct ams641rw01_cmd ams641rw01_hbm_on_seq[] = {
	CMD(0,   0x51, 0x03, 0xff),
	CMD(34,  0x53, 0xe0),
};

static const struct ams641rw01_cmd ams641rw01_hbm_off_seq[] = {
	CMD(0,   0x53, 0x20),
};

static const struct ams641rw01_cmd ams641rw01_aod_hbm_on_seq[] = {
	CMD(0,   0x51, 0x03, 0xff),
	CMD(0,   0x53, 0xe0),
};

static const struct ams641rw01_cmd ams641rw01_aod_hbm_off_seq[] = {
	CMD(0,   0x51, 0x00, 0x01),
	CMD(0,   0x53, 0x22),
};

/* All three colour modes (natural / vivid / DCI-P3) use the same
 *  panel-side CRC + SEED TCS sequence; seed-off ends the colour mode.
 */
static const struct ams641rw01_cmd ams641rw01_seed_on_seq[] = {
	CMD(0,   0x81, 0x90),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x02),
	CMD(0,   0xb1, 0xe0, 0x00, 0x08, 0x10, 0xff, 0x00, 0x00, 0x00, 0xff,
		 0x15, 0xfd, 0xf0, 0xf0, 0x00, 0xf0, 0xec, 0xf0, 0x00, 0xff,
		 0xff, 0xfa),
	CMD(0,   0xb1, 0x00, 0x00),
	CMD(0,   0xf0, 0xa5, 0xa5),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb0, 0x23),
	CMD(0,   0xb3, 0x91),
	CMD(0,   0x83, 0x80),
	CMD(0,   0xb3, 0x00, 0xc0),
	CMD(0,   0xf0, 0xa5, 0xa5),
};

static const struct ams641rw01_cmd ams641rw01_seed_off_seq[] = {
	CMD(0,   0x81, 0x00),
	CMD(0,   0xf0, 0x5a, 0x5a),
	CMD(0,   0xb1, 0x00, 0x01),
	CMD(0,   0xb3, 0x00, 0xc1),
	CMD(0,   0xf0, 0xa5, 0xa5),
};

static const struct drm_display_mode ams641rw01_mode = {
	.clock		= (1080 + 48 + 48 + 24) * (2340 + 16 + 20 + 4) * 60 / 1000,
	.hdisplay	= 1080,
	.hsync_start	= 1080 + 48,
	.hsync_end	= 1080 + 48 + 24,
	.htotal		= 1080 + 48 + 48 + 24,
	.vdisplay	= 2340,
	.vsync_start	= 2340 + 20,
	.vsync_end	= 2340 + 20 + 4,
	.vtotal		= 2340 + 16 + 20 + 4,
	.width_mm	= 69,
	.height_mm	= 148,
	.type		= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	.flags		= DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static inline struct ams641rw01 *to_ams641rw01(struct drm_panel *panel)
{
	return container_of(panel, struct ams641rw01, panel);
}

static int ams641rw01_send_seq(struct mipi_dsi_device *dsi,
			       const struct ams641rw01_cmd *seq,
			       size_t count)
{
	int ret;

	for (; count; count--, seq++) {
		ret = mipi_dsi_dcs_write_buffer(dsi, seq->data, seq->len);
		if (ret < 0)
			return ret;

		if (seq->delay_ms)
			usleep_range(seq->delay_ms * 1000,
				     seq->delay_ms * 1000 + 1000);
	}

	return 0;
}

static int ams641rw01_set_brightness(struct ams641rw01 *ctx, u16 brightness)
{
	/* DCS command transfers stay on the HS link. */
	return mipi_dsi_dcs_set_display_brightness_large(ctx->dsi, brightness);
}

static void ams641rw01_reset(struct ams641rw01 *ctx)
{
	/* reset-gpios is GPIO_ACTIVE_LOW: value 1 pulses the panel into
	 *  reset, value 0 releases it.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static void ams641rw01_esd_work(struct work_struct *work)
{
	struct ams641rw01 *ctx = container_of(to_delayed_work(work),
					      struct ams641rw01, esd_work);
	struct mipi_dsi_device *dsi = ctx->dsi;
	u8 power_mode;
	int ret;

	mutex_lock(&ctx->lock);

	/* The power-mode answer differs while the AOD clock is up and while
	 *  the panel is off, so only sample a fully-on panel.
	 */
	if (!ctx->enabled || ctx->aod)
		goto out;

	ret = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_POWER_MODE, &power_mode, 1);
	if (ret >= 1 && power_mode == AMS641RW01_POWER_MODE_OK)
		goto reschedule;

	dev_err(&dsi->dev,
		"ESD detected (read ret=%d power_mode=0x%02x), recovering\n",
		ret, ret >= 1 ? power_mode : 0);

	/* In-place power cycle: off sequence, hardware reset, full init and
	 * display-on again.  Supplies and the DSI link stay up; the DPU keeps
	 * TE-gating updates and resumes pushing frames once the panel TE
	 *  line comes back.
	 */
	if (ams641rw01_send_seq(dsi, ams641rw01_off_seq,
				ARRAY_SIZE(ams641rw01_off_seq)))
		dev_err(&dsi->dev, "ESD recovery: off sequence failed\n");

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);

	if (ams641rw01_send_seq(dsi, ams641rw01_on_seq,
				ARRAY_SIZE(ams641rw01_on_seq))) {
		dev_err(&dsi->dev, "ESD recovery: on sequence failed\n");
		ctx->enabled = false;
		ctx->displayed = false;
		goto out;
	}

	ctx->hbm = false;

	ret = ams641rw01_set_brightness(ctx,
					backlight_get_brightness(ctx->backlight));
	if (!ret)
		ret = ams641rw01_send_seq(dsi, ams641rw01_post_on_seq,
				ARRAY_SIZE(ams641rw01_post_on_seq));
	if (ret)
		dev_err(&dsi->dev, "ESD recovery: display on failed\n");
	else
		ctx->displayed = true;

reschedule:
	schedule_delayed_work(&ctx->esd_work,
			      msecs_to_jiffies(AMS641RW01_ESD_INTERVAL_MS));
out:
	mutex_unlock(&ctx->lock);
}

static int ams641rw01_prepare(struct drm_panel *panel)
{
	struct ams641rw01 *ctx = to_ams641rw01(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ams641rw01_supplies),
				    ctx->supplies);
	if (ret < 0)
		return ret;

	ams641rw01_reset(ctx);

	mutex_lock(&ctx->lock);
	ret = ams641rw01_send_seq(ctx->dsi, ams641rw01_on_seq,
				  ARRAY_SIZE(ams641rw01_on_seq));
	if (!ret)
		ctx->prepared = true;
	mutex_unlock(&ctx->lock);

	if (ret < 0) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ams641rw01_supplies),
				       ctx->supplies);
	}

	return ret;
}

static int ams641rw01_unprepare(struct drm_panel *panel)
{
	struct ams641rw01 *ctx = to_ams641rw01(panel);

	mutex_lock(&ctx->lock);
	ctx->prepared = false;
	ctx->displayed = false;
	mutex_unlock(&ctx->lock);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ams641rw01_supplies), ctx->supplies);

	return 0;
}

static int ams641rw01_enable(struct drm_panel *panel)
{
	struct ams641rw01 *ctx = to_ams641rw01(panel);
	int ret;

	/* Bring-up must not race with the ESD worker. */
	cancel_delayed_work_sync(&ctx->esd_work);

	mutex_lock(&ctx->lock);
	if (!ctx->prepared) {
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}

	/* Panel bring-up order is: init sequence, DCS brightness, display
	 * on.  Deliver the whole chain here so a panel enable always ends
	 *  with a lit display even before userspace touches the backlight.
	 */
	ret = ams641rw01_set_brightness(ctx,
					backlight_get_brightness(ctx->backlight));
	if (!ret)
		ret = ams641rw01_send_seq(ctx->dsi, ams641rw01_post_on_seq,
				ARRAY_SIZE(ams641rw01_post_on_seq));
	if (!ret) {
		ctx->enabled = true;
		ctx->displayed = true;
	}
	mutex_unlock(&ctx->lock);

	if (ret)
		return ret;

	schedule_delayed_work(&ctx->esd_work,
			      msecs_to_jiffies(AMS641RW01_ESD_INTERVAL_MS));

	return 0;
}

static int ams641rw01_disable(struct drm_panel *panel)
{
	struct ams641rw01 *ctx = to_ams641rw01(panel);

	cancel_delayed_work_sync(&ctx->esd_work);

	mutex_lock(&ctx->lock);
	if (ctx->enabled) {
		/* Errors are not fatal here: the panel is going away. */
		if (ams641rw01_send_seq(ctx->dsi, ams641rw01_off_seq,
					ARRAY_SIZE(ams641rw01_off_seq)))
			dev_err(&ctx->dsi->dev,
				"failed to send display-off sequence\n");
	}
	ctx->enabled = false;
	ctx->displayed = false;
	ctx->hbm = false;
	ctx->aod = false;
	mutex_unlock(&ctx->lock);

	return 0;
}

static int ams641rw01_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector,
						    &ams641rw01_mode);
}

static const struct drm_panel_funcs ams641rw01_panel_funcs = {
	.prepare = ams641rw01_prepare,
	.unprepare = ams641rw01_unprepare,
	.enable = ams641rw01_enable,
	.disable = ams641rw01_disable,
	.get_modes = ams641rw01_get_modes,
};

static int ams641rw01_bl_update_status(struct backlight_device *bl)
{
	struct ams641rw01 *ctx = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret = 0;

	mutex_lock(&ctx->lock);

	/* While HBM is active the panel sits at a fixed maximum level; the
	 *  normal ramp resumes when HBM is turned off.
	 */
	if (ctx->enabled && ctx->displayed && !ctx->hbm)
		ret = ams641rw01_set_brightness(ctx, brightness);

	mutex_unlock(&ctx->lock);

	return ret;
}

static const struct backlight_ops ams641rw01_bl_ops = {
	.update_status = ams641rw01_bl_update_status,
};

static struct backlight_device *
ams641rw01_create_backlight(struct ams641rw01 *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.brightness = 512,
		.max_brightness = 1023,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &ams641rw01_bl_ops, &props);
}

static ssize_t ams641rw01_hbm_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ams641rw01 *ctx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", ctx->hbm);
}

static ssize_t ams641rw01_hbm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct ams641rw01 *ctx = dev_get_drvdata(dev);
	const struct ams641rw01_cmd *seq;
	unsigned int value;
	bool enable;
	size_t count_seq;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret)
		return ret;
	if (value > 1)
		return -EINVAL;

	enable = value;

	mutex_lock(&ctx->lock);
	if (!ctx->enabled) {
		mutex_unlock(&ctx->lock);
		return -EPERM;
	}

	if (enable == ctx->hbm) {
		mutex_unlock(&ctx->lock);
		return count;
	}

	/* The AOD clock has its own HBM pair with a different settle. */
	if (ctx->aod) {
		seq = enable ? ams641rw01_aod_hbm_on_seq :
			       ams641rw01_aod_hbm_off_seq;
		count_seq = enable ? ARRAY_SIZE(ams641rw01_aod_hbm_on_seq) :
				     ARRAY_SIZE(ams641rw01_aod_hbm_off_seq);
	} else {
		seq = enable ? ams641rw01_hbm_on_seq : ams641rw01_hbm_off_seq;
		count_seq = enable ? ARRAY_SIZE(ams641rw01_hbm_on_seq) :
				     ARRAY_SIZE(ams641rw01_hbm_off_seq);
	}

	ret = ams641rw01_send_seq(ctx->dsi, seq, count_seq);
	if (ret) {
		mutex_unlock(&ctx->lock);
		return ret;
	}
	ctx->hbm = enable;
	mutex_unlock(&ctx->lock);

	return count;
}
static struct device_attribute dev_attr_hbm =
	__ATTR(hbm, 0644, ams641rw01_hbm_show, ams641rw01_hbm_store);

static ssize_t ams641rw01_aod_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ams641rw01 *ctx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", ctx->aod);
}

static ssize_t ams641rw01_aod_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct ams641rw01 *ctx = dev_get_drvdata(dev);
	const struct ams641rw01_cmd *seq;
	unsigned int value;
	bool enable;
	size_t count_seq;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret)
		return ret;
	if (value > 1)
		return -EINVAL;

	enable = value;

	mutex_lock(&ctx->lock);
	if (!ctx->enabled) {
		mutex_unlock(&ctx->lock);
		return -EPERM;
	}

	if (enable == ctx->aod) {
		mutex_unlock(&ctx->lock);
		return count;
	}

	seq = enable ? ams641rw01_lp1_seq : ams641rw01_nolp_seq;
	count_seq = enable ? ARRAY_SIZE(ams641rw01_lp1_seq) :
			     ARRAY_SIZE(ams641rw01_nolp_seq);

	ret = ams641rw01_send_seq(ctx->dsi, seq, count_seq);
	if (!ret && !enable) {
		/* The AOD pair may have left a fixed HBM level in the
		 *  brightness register; restore the userspace ramp.
		 */
		ret = ams641rw01_set_brightness(ctx,
					backlight_get_brightness(ctx->backlight));
	}
	if (ret) {
		mutex_unlock(&ctx->lock);
		return ret;
	}

	ctx->aod = enable;
	ctx->hbm = false;
	mutex_unlock(&ctx->lock);

	/* The power-mode answer is not comparable while the AOD clock is
	 *  displayed; stop sampling and pick up again on the way out.
	 */
	if (enable)
		cancel_delayed_work_sync(&ctx->esd_work);
	else
		schedule_delayed_work(&ctx->esd_work,
				msecs_to_jiffies(AMS641RW01_ESD_INTERVAL_MS));

	return count;
}
static struct device_attribute dev_attr_aod =
	__ATTR(aod, 0644, ams641rw01_aod_show, ams641rw01_aod_store);

static ssize_t ams641rw01_seed_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ams641rw01 *ctx = dev_get_drvdata(dev);

	/* The panel sequence does not distinguish the three colour modes,
	 *  only on (0/1/2) versus off (3); report the last written value.
	 */
	return sysfs_emit(buf, "%u\n", ctx->seed);
}

static ssize_t ams641rw01_seed_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct ams641rw01 *ctx = dev_get_drvdata(dev);
	const struct ams641rw01_cmd *seq;
	unsigned int value;
	size_t count_seq;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret)
		return ret;
	if (value > AMS641RW01_SEED_OFF)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	if (!ctx->enabled || ctx->aod) {
		mutex_unlock(&ctx->lock);
		return -EPERM;
	}

	if (value == AMS641RW01_SEED_OFF)
		seq = ams641rw01_seed_off_seq;
	else
		seq = ams641rw01_seed_on_seq;
	count_seq = (value == AMS641RW01_SEED_OFF) ?
		    ARRAY_SIZE(ams641rw01_seed_off_seq) :
		    ARRAY_SIZE(ams641rw01_seed_on_seq);

	ret = ams641rw01_send_seq(ctx->dsi, seq, count_seq);
	if (!ret)
		ctx->seed = value;
	mutex_unlock(&ctx->lock);

	return ret ? ret : count;
}
static struct device_attribute dev_attr_seed =
	__ATTR(seed, 0644, ams641rw01_seed_show, ams641rw01_seed_store);

static ssize_t ams641rw01_hdr_static_metadata_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	struct ams641rw01 *ctx = dev_get_drvdata(dev);

	if (!ctx->has_hdr)
		return -ENODEV;

	return sysfs_emit(buf,
			  "peak_brightness=%u color_primaries=%u %u %u %u %u %u %u %u\n",
			  ctx->peak_brightness,
			  ctx->color_primaries[0], ctx->color_primaries[1],
			  ctx->color_primaries[2], ctx->color_primaries[3],
			  ctx->color_primaries[4], ctx->color_primaries[5],
			  ctx->color_primaries[6], ctx->color_primaries[7]);
}
static struct device_attribute dev_attr_hdr_static_metadata =
	__ATTR(hdr_static_metadata, 0444, ams641rw01_hdr_static_metadata_show, NULL);

static struct attribute *ams641rw01_attrs[] = {
	&dev_attr_hbm.attr,
	&dev_attr_aod.attr,
	&dev_attr_seed.attr,
	&dev_attr_hdr_static_metadata.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ams641rw01);

static void ams641rw01_sysfs_remove(void *data)
{
	struct device *dev = data;

	sysfs_remove_groups(&dev->kobj, ams641rw01_groups);
}

static int ams641rw01_parse_hdr(struct ams641rw01 *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = of_property_read_u32(dev->of_node, "peak-brightness",
				   &ctx->peak_brightness);
	if (ret)
		return 0;

	ret = of_property_read_u32_array(dev->of_node, "color-primaries",
					 ctx->color_primaries,
					 ARRAY_SIZE(ctx->color_primaries));
	if (ret)
		return dev_err_probe(dev, ret,
				     "invalid color-primaries\n");

	ctx->has_hdr = true;

	return 0;
}

static int ams641rw01_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ams641rw01 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ams641rw01, panel,
				   &ams641rw01_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(ams641rw01_supplies),
					    ams641rw01_supplies,
					    &ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset-gpios\n");

	mutex_init(&ctx->lock);
	INIT_DELAYED_WORK(&ctx->esd_work, ams641rw01_esd_work);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ret = ams641rw01_parse_hdr(ctx);
	if (ret)
		return ret;

	/* Command mode panel: refresh is TE-gated by the DPU and the link
	 *  runs DCS transfers over HS, so no video-mode flags.
	 */
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = 0;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = ams641rw01_create_backlight(ctx);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "failed to create backlight\n");
	ctx->backlight = ctx->panel.backlight;

	ret = sysfs_create_groups(&dsi->dev.kobj, ams641rw01_groups);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to create sysfs attributes\n");
	ret = devm_add_action_or_reset(dev, ams641rw01_sysfs_remove, &dsi->dev);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void ams641rw01_remove(struct mipi_dsi_device *dsi)
{
	struct ams641rw01 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	cancel_delayed_work_sync(&ctx->esd_work);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ams641rw01_of_match[] = {
	{ .compatible = "samsung,ams641rw01" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ams641rw01_of_match);

static struct mipi_dsi_driver ams641rw01_driver = {
	.probe = ams641rw01_probe,
	.remove = ams641rw01_remove,
	.driver = {
		.name = "panel-samsung-ams641rw01",
		.of_match_table = ams641rw01_of_match,
	},
};
module_mipi_dsi_driver(ams641rw01_driver);

MODULE_AUTHOR("omajili-manbu");
MODULE_DESCRIPTION("Samsung AMS641RW01 AMOLED DSI command-mode panel driver");
MODULE_LICENSE("GPL");

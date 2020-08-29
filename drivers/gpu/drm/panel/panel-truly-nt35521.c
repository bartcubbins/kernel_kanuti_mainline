// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020, Pavel Dubrova <pashadubrova@gmail.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#define MCS_CMD_READ_ID			0xDA

#define NT35521_BACKLIGHT_DEFAULT	240
#define NT35521_BACKLIGHT_MAX		255

struct nt35521 {
	struct device *dev;
	struct drm_panel panel;

	struct backlight_device *bl_dev;

	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;
};

static inline struct nt35521 *panel_to_nt35521(struct drm_panel *panel)
{
	return container_of(panel, struct nt35521, panel);
}

static void nt35521_dcs_write_buf(struct nt35521 *ctx, const void *data,
				   size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	/* Data will be sent in LPM mode */
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	if (mipi_dsi_dcs_write_buffer(dsi, data, len) < 0)
		DRM_WARN("mipi dsi dcs write buffer failed.\n");
}

#define nt35521_dcs_write_seq(ctx, seq...)		\
({							\
	static const u8 d[] = { seq };			\
	nt35521_dcs_write_buf(ctx, d, ARRAY_SIZE(d));	\
})

static int nt35521_read_id(struct nt35521 *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	u8 id;
	int ret;

	ret = mipi_dsi_dcs_read(dsi, MCS_CMD_READ_ID, &id, 1);
	if (ret < 0) {
		DRM_DEV_ERROR(ctx->dev, "Could not read panel manufacturer ID\n");
		return ret;
	}

	DRM_DEV_INFO(ctx->dev, "Panel manufacturer ID: %02x\n", id);

	return 0;
}

static void nt35521_init_sequence(struct nt35521 *ctx)
{
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	nt35521_dcs_write_seq(ctx, 0xFF, 0xAA, 0x55, 0xA5, 0x80);
	nt35521_dcs_write_seq(ctx, 0x6F, 0x11, 0x00);
	nt35521_dcs_write_seq(ctx, 0xF7, 0x20, 0x00);
	nt35521_dcs_write_seq(ctx, 0x6F, 0x01);
	nt35521_dcs_write_seq(ctx, 0xB1, 0x21);
	nt35521_dcs_write_seq(ctx, 0xBD, 0x01, 0xA0, 0x10, 0x08, 0x01);
	nt35521_dcs_write_seq(ctx, 0xB8, 0x01, 0x02, 0x0C, 0x02);
	nt35521_dcs_write_seq(ctx, 0xBB, 0x11, 0x11);
	nt35521_dcs_write_seq(ctx, 0xBC, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB6, 0x02);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	nt35521_dcs_write_seq(ctx, 0xB0, 0x09, 0x09);
	nt35521_dcs_write_seq(ctx, 0xB1, 0x09, 0x09);
	nt35521_dcs_write_seq(ctx, 0xBC, 0x8C, 0x00);
	nt35521_dcs_write_seq(ctx, 0xBD, 0x8C, 0x00);
	nt35521_dcs_write_seq(ctx, 0xCA, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC0, 0x04);
	nt35521_dcs_write_seq(ctx, 0xBE, 0xB5);
	nt35521_dcs_write_seq(ctx, 0xB3, 0x35, 0x35);
	nt35521_dcs_write_seq(ctx, 0xB4, 0x25, 0x25);
	nt35521_dcs_write_seq(ctx, 0xB9, 0x43, 0x43);
	nt35521_dcs_write_seq(ctx, 0xBA, 0x24, 0x24);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x02);
	nt35521_dcs_write_seq(ctx, 0xEE, 0x03);
	nt35521_dcs_write_seq(ctx, 0xB0, 0x00, 0xB2, 0x00, 0xB3, 0x00, 0xB6, 0x00, 0xC3, 0x00, 0xCE, 0x00, 0xE1, 0x00, 0xF3, 0x01, 0x11);
	nt35521_dcs_write_seq(ctx, 0xB1, 0x01, 0x2E, 0x01, 0x5C, 0x01, 0x82, 0x01, 0xC3, 0x01, 0xFE, 0x02, 0x00, 0x02, 0x37, 0x02, 0x77);
	nt35521_dcs_write_seq(ctx, 0xB2, 0x02, 0xA1, 0x02, 0xD7, 0x02, 0xFE, 0x03, 0x2C, 0x03, 0x4B, 0x03, 0x63, 0x03, 0x8F, 0x03, 0x90);
	nt35521_dcs_write_seq(ctx, 0xB3, 0x03, 0x96, 0x03, 0x98);
	nt35521_dcs_write_seq(ctx, 0xB4, 0x00, 0x81, 0x00, 0x8B, 0x00, 0x9C, 0x00, 0xA9, 0x00, 0xB5, 0x00, 0xCB, 0x00, 0xDF, 0x01, 0x02);
	nt35521_dcs_write_seq(ctx, 0xB5, 0x01, 0x1F, 0x01, 0x51, 0x01, 0x7A, 0x01, 0xBF, 0x01, 0xFA, 0x01, 0xFC, 0x02, 0x34, 0x02, 0x76);
	nt35521_dcs_write_seq(ctx, 0xB6, 0x02, 0x9F, 0x02, 0xD7, 0x02, 0xFC, 0x03, 0x2C, 0x03, 0x4A, 0x03, 0x63, 0x03, 0x8F, 0x03, 0xA2);
	nt35521_dcs_write_seq(ctx, 0xB7, 0x03, 0xB8, 0x03, 0xBA);
	nt35521_dcs_write_seq(ctx, 0xB8, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0E, 0x00, 0x2A, 0x00, 0x41, 0x00, 0x67, 0x00, 0x87, 0x00, 0xB9);
	nt35521_dcs_write_seq(ctx, 0xB9, 0x00, 0xE2, 0x01, 0x22, 0x01, 0x54, 0x01, 0xA3, 0x01, 0xE6, 0x01, 0xE7, 0x02, 0x24, 0x02, 0x67);
	nt35521_dcs_write_seq(ctx, 0xBA, 0x02, 0x93, 0x02, 0xCD, 0x02, 0xF6, 0x03, 0x31, 0x03, 0x6C, 0x03, 0xE9, 0x03, 0xEF, 0x03, 0xF4);
	nt35521_dcs_write_seq(ctx, 0xBB, 0x03, 0xF6, 0x03, 0xF7);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x03);
	nt35521_dcs_write_seq(ctx, 0xB0, 0x22, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB1, 0x22, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB2, 0x05, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB3, 0x05, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB4, 0x05, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB5, 0x05, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xBA, 0x53, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xBB, 0x53, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xBC, 0x53, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xBD, 0x53, 0x00, 0x60, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC0, 0x00, 0x34, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC1, 0x00, 0x00, 0x34, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC2, 0x00, 0x00, 0x34, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC3, 0x00, 0x00, 0x34, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC4, 0x60);
	nt35521_dcs_write_seq(ctx, 0xC5, 0xC0);
	nt35521_dcs_write_seq(ctx, 0xC6, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC7, 0x00);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x05);
	nt35521_dcs_write_seq(ctx, 0xB0, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB1, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB2, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB3, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB4, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB5, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB6, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB7, 0x17, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB8, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB9, 0x00, 0x03);
	nt35521_dcs_write_seq(ctx, 0xBA, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xBB, 0x02, 0x03);
	nt35521_dcs_write_seq(ctx, 0xBC, 0x02, 0x03);
	nt35521_dcs_write_seq(ctx, 0xBD, 0x03, 0x03, 0x00, 0x03, 0x03);
	nt35521_dcs_write_seq(ctx, 0xC0, 0x0B);
	nt35521_dcs_write_seq(ctx, 0xC1, 0x09);
	nt35521_dcs_write_seq(ctx, 0xC2, 0xA6);
	nt35521_dcs_write_seq(ctx, 0xC3, 0x05);
	nt35521_dcs_write_seq(ctx, 0xC4, 0x00);
	nt35521_dcs_write_seq(ctx, 0xC5, 0x02);
	nt35521_dcs_write_seq(ctx, 0xC6, 0x22);
	nt35521_dcs_write_seq(ctx, 0xC7, 0x03);
	nt35521_dcs_write_seq(ctx, 0xC8, 0x07, 0x20);
	nt35521_dcs_write_seq(ctx, 0xC9, 0x03, 0x20);
	nt35521_dcs_write_seq(ctx, 0xCA, 0x01, 0x60);
	nt35521_dcs_write_seq(ctx, 0xCB, 0x01, 0x60);
	nt35521_dcs_write_seq(ctx, 0xCC, 0x00, 0x00, 0x02);
	nt35521_dcs_write_seq(ctx, 0xCD, 0x00, 0x00, 0x02);
	nt35521_dcs_write_seq(ctx, 0xCE, 0x00, 0x00, 0x02);
	nt35521_dcs_write_seq(ctx, 0xCF, 0x00, 0x00, 0x02);
	nt35521_dcs_write_seq(ctx, 0xD1, 0x00, 0x05, 0x01, 0x07, 0x10);
	nt35521_dcs_write_seq(ctx, 0xD2, 0x10, 0x05, 0x05, 0x03, 0x10);
	nt35521_dcs_write_seq(ctx, 0xD3, 0x20, 0x00, 0x43, 0x07, 0x10);
	nt35521_dcs_write_seq(ctx, 0xD4, 0x30, 0x00, 0x43, 0x07, 0x10);
	nt35521_dcs_write_seq(ctx, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xD5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xD6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xD7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xE5, 0x06);
	nt35521_dcs_write_seq(ctx, 0xE6, 0x06);
	nt35521_dcs_write_seq(ctx, 0xE7, 0x00);
	nt35521_dcs_write_seq(ctx, 0xE8, 0x06);
	nt35521_dcs_write_seq(ctx, 0xE9, 0x06);
	nt35521_dcs_write_seq(ctx, 0xEA, 0x06);
	nt35521_dcs_write_seq(ctx, 0xEB, 0x00);
	nt35521_dcs_write_seq(ctx, 0xEC, 0x00);
	nt35521_dcs_write_seq(ctx, 0xED, 0x30);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x06);
	nt35521_dcs_write_seq(ctx, 0xB0, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xB1, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xB2, 0x2D, 0x2E);
	nt35521_dcs_write_seq(ctx, 0xB3, 0x31, 0x34);
	nt35521_dcs_write_seq(ctx, 0xB4, 0x29, 0x2A);
	nt35521_dcs_write_seq(ctx, 0xB5, 0x12, 0x10);
	nt35521_dcs_write_seq(ctx, 0xB6, 0x18, 0x16);
	nt35521_dcs_write_seq(ctx, 0xB7, 0x00, 0x02);
	nt35521_dcs_write_seq(ctx, 0xB8, 0x08, 0x31);
	nt35521_dcs_write_seq(ctx, 0xB9, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xBA, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xBB, 0x31, 0x08);
	nt35521_dcs_write_seq(ctx, 0xBC, 0x03, 0x01);
	nt35521_dcs_write_seq(ctx, 0xBD, 0x17, 0x19);
	nt35521_dcs_write_seq(ctx, 0xBE, 0x11, 0x13);
	nt35521_dcs_write_seq(ctx, 0xBF, 0x2A, 0x29);
	nt35521_dcs_write_seq(ctx, 0xC0, 0x34, 0x31);
	nt35521_dcs_write_seq(ctx, 0xC1, 0x2E, 0x2D);
	nt35521_dcs_write_seq(ctx, 0xC2, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xC3, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xC4, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xC5, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xC6, 0x2E, 0x2D);
	nt35521_dcs_write_seq(ctx, 0xC7, 0x31, 0x34);
	nt35521_dcs_write_seq(ctx, 0xC8, 0x29, 0x2A);
	nt35521_dcs_write_seq(ctx, 0xC9, 0x17, 0x19);
	nt35521_dcs_write_seq(ctx, 0xCA, 0x11, 0x13);
	nt35521_dcs_write_seq(ctx, 0xCB, 0x03, 0x01);
	nt35521_dcs_write_seq(ctx, 0xCC, 0x08, 0x31);
	nt35521_dcs_write_seq(ctx, 0xCD, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xCE, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xCF, 0x31, 0x08);
	nt35521_dcs_write_seq(ctx, 0xD0, 0x00, 0x02);
	nt35521_dcs_write_seq(ctx, 0xD1, 0x12, 0x10);
	nt35521_dcs_write_seq(ctx, 0xD2, 0x18, 0x16);
	nt35521_dcs_write_seq(ctx, 0xD3, 0x2A, 0x29);
	nt35521_dcs_write_seq(ctx, 0xD4, 0x34, 0x31);
	nt35521_dcs_write_seq(ctx, 0xD5, 0x2D, 0x2E);
	nt35521_dcs_write_seq(ctx, 0xD6, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xD7, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xE5, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xE6, 0x31, 0x31);
	nt35521_dcs_write_seq(ctx, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xD9, 0x00, 0x00, 0x00, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xE7, 0x00);
	nt35521_dcs_write_seq(ctx, 0x6F, 0x02);
	nt35521_dcs_write_seq(ctx, 0xF7, 0x47);
	nt35521_dcs_write_seq(ctx, 0x6F, 0x0A);
	nt35521_dcs_write_seq(ctx, 0xF7, 0x02);
	nt35521_dcs_write_seq(ctx, 0x6F, 0x17);
	nt35521_dcs_write_seq(ctx, 0xF4, 0x60);
	nt35521_dcs_write_seq(ctx, 0x6F, 0x01);
	nt35521_dcs_write_seq(ctx, 0xF9, 0x46);
	nt35521_dcs_write_seq(ctx, 0x6F, 0x11);
	nt35521_dcs_write_seq(ctx, 0xF3, 0x01);
	nt35521_dcs_write_seq(ctx, 0x35, 0x00);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	nt35521_dcs_write_seq(ctx, 0xD9, 0x02, 0x03, 0x00);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	nt35521_dcs_write_seq(ctx, 0xB1, 0x6C, 0x21);
	nt35521_dcs_write_seq(ctx, 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00);
	nt35521_dcs_write_seq(ctx, 0x35, 0x00);
	nt35521_dcs_write_seq(ctx, 0x11, 0x00);
	msleep(78);
	nt35521_dcs_write_seq(ctx, 0x29, 0x00);
	msleep(1);
	nt35521_dcs_write_seq(ctx, 0x53, 0x24);
}

static int nt35521_prepare(struct drm_panel *panel)
{
	struct nt35521 *ctx = panel_to_nt35521(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		usleep_range(20000, 20000);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		usleep_range(1000, 1000);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		usleep_range(20000, 20000);
	}

	ret = nt35521_read_id(ctx);
	if (ret)
		return ret;

	nt35521_init_sequence(ctx);

	ctx->prepared = true;

	return 0;
}

static int nt35521_enable(struct drm_panel *panel)
{
	struct nt35521 *ctx = panel_to_nt35521(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->enabled)
		return 0;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret)
		return ret;

	msleep(78);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret)
		return ret;

	msleep(1);

	backlight_enable(ctx->bl_dev);

	ctx->enabled = true;

	return 0;
}

static int nt35521_disable(struct drm_panel *panel)
{
	struct nt35521 *ctx = panel_to_nt35521(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (!ctx->enabled)
		return 0;

	backlight_disable(ctx->bl_dev);

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret)
		return ret;

	ctx->enabled = false;

	return 0;
}

static int nt35521_unprepare(struct drm_panel *panel)
{
	struct nt35521 *ctx = panel_to_nt35521(panel);

	if (!ctx->prepared)
		return 0;

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
	}

	ctx->prepared = false;

	return 0;
}

static const struct drm_display_mode nt35521_modes = {
	.clock = (720 + 632 + 40 + 295) * (1280 + 18 + 1 + 18) * 60 / 1000,
	.hdisplay = 720,
	.hsync_start = 720 + 632,
	.hsync_end = 720 + 632 + 40,
	.htotal = 720 + 632 + 40 + 295,
	.vdisplay = 1280,
	.vsync_start = 1280 + 18,
	.vsync_end = 1280 + 18 + 1,
	.vtotal = 1280 + 18 + 1 + 18,
};

static int nt35521_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &nt35521_modes);
	if (!mode) {
		DRM_ERROR("Failed to add mode %ux%ux@%u\n",
			  nt35521_modes.hdisplay, nt35521_modes.vdisplay,
			  drm_mode_vrefresh(&nt35521_modes));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 65;
	connector->display_info.height_mm = 116;

	return 1;
}

static const struct drm_panel_funcs nt35521_drm_funcs = {
	.prepare   = nt35521_prepare,
	.enable    = nt35521_enable,
	.disable   = nt35521_disable,
	.unprepare = nt35521_unprepare,
	.get_modes = nt35521_get_modes,
};

static int nt35521_backlight_update_status(struct backlight_device *bd)
{
	struct nt35521 *ctx = bl_get_data(bd);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	u8 brightness[] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS, bd->props.brightness };

	if (!ctx->prepared) {
		DRM_DEBUG("Panel not ready for setting its backlight!\n");
		return -ENXIO;
	}

	/* DCS will be sent in DSI HS mode */
	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_buffer(dsi, brightness, ARRAY_SIZE(brightness));

	/* Restore DSI LPM mode */
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static const struct backlight_ops nt35521_backlight_ops = {
	.update_status = nt35521_backlight_update_status,
};

static int nt35521_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nt35521 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		DRM_DEV_ERROR(dev, "Cannot get reset-gpio\n");
		ctx->reset_gpio = NULL;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &nt35521_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->bl_dev = devm_backlight_device_register(dev, dev_name(dev), dsi->host->dev, ctx,
						&nt35521_backlight_ops, NULL);
	if (IS_ERR(ctx->bl_dev)) {
		ret = PTR_ERR(ctx->bl_dev);
		DRM_DEV_ERROR(dev, "Failed to register backlight: %d\n", ret);
		return ret;
	}

	ctx->bl_dev->props.max_brightness = NT35521_BACKLIGHT_MAX;
	ctx->bl_dev->props.brightness = NT35521_BACKLIGHT_DEFAULT;
	ctx->bl_dev->props.power = FB_BLANK_POWERDOWN;
	ctx->bl_dev->props.type = BACKLIGHT_RAW;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		DRM_DEV_ERROR(dev, "mipi_dsi_attach failed.\n");
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static int nt35521_remove(struct mipi_dsi_device *dsi)
{
	struct nt35521 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id truly_nt35521_of_match[] = {
	{ .compatible = "truly,nt35521" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, truly_nt35521_of_match);

static struct mipi_dsi_driver truly_nt35521_driver = {
	.probe  = nt35521_probe,
	.remove = nt35521_remove,
	.driver = {
		.name = "panel-truly-nt35521",
		.of_match_table = truly_nt35521_of_match,
	},
};
module_mipi_dsi_driver(truly_nt35521_driver);

MODULE_AUTHOR("Pavel Dubrova <pashadubrova@gmail.com>");
MODULE_DESCRIPTION("Truly NT35521 panel driver");
MODULE_LICENSE("GPL v2"); 

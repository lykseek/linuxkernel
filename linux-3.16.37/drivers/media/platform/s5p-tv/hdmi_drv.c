/*
 * Samsung HDMI interface driver
 *
 * Copyright (c) 2010-2011 Samsung Electronics Co., Ltd.
 *
 * Tomasz Stanislawski, <t.stanislaws@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundiation. either version 2 of the License,
 * or (at your option) any later version
 */

#define pr_fmt(fmt) "s5p-tv (hdmi_drv): " fmt

#ifdef CONFIG_VIDEO_SAMSUNG_S5P_HDMI_DEBUG
#define DEBUG
#endif

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <media/v4l2-subdev.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/bug.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/v4l2-dv-timings.h>

#include <media/s5p_hdmi.h>
#include <media/v4l2-common.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dv-timings.h>

#include "regs-hdmi.h"

MODULE_AUTHOR("Tomasz Stanislawski, <t.stanislaws@samsung.com>");
MODULE_DESCRIPTION("Samsung HDMI");
MODULE_LICENSE("GPL");

struct hdmi_pulse {
	u32 beg;
	u32 end;
};

struct hdmi_timings {
	struct hdmi_pulse hact;
	u32 hsyn_pol; /* 0 - high, 1 - low */
	struct hdmi_pulse hsyn;
	u32 interlaced;
	struct hdmi_pulse vact[2];
	u32 vsyn_pol; /* 0 - high, 1 - low */
	u32 vsyn_off;
	struct hdmi_pulse vsyn[2];
};

struct hdmi_resources {
	struct clk *hdmi;
	struct clk *sclk_hdmi;
	struct clk *sclk_pixel;
	struct clk *sclk_hdmiphy;
	struct clk *hdmiphy;
	struct regulator_bulk_data *regul_bulk;
	int regul_count;
};

struct hdmi_device {
	/** base address of HDMI registers */
	void __iomem *regs;
	/** HDMI interrupt */
	unsigned int irq;
	/** pointer to device parent */
	struct device *dev;
	/** subdev generated by HDMI device */
	struct v4l2_subdev sd;
	/** V4L2 device structure */
	struct v4l2_device v4l2_dev;
	/** subdev of HDMIPHY interface */
	struct v4l2_subdev *phy_sd;
	/** subdev of MHL interface */
	struct v4l2_subdev *mhl_sd;
	/** configuration of current graphic mode */
	const struct hdmi_timings *cur_conf;
	/** flag indicating that timings are dirty */
	int cur_conf_dirty;
	/** current timings */
	struct v4l2_dv_timings cur_timings;
	/** other resources */
	struct hdmi_resources res;
};

static struct platform_device_id hdmi_driver_types[] = {
	{
		.name		= "s5pv210-hdmi",
	}, {
		.name		= "exynos4-hdmi",
	}, {
		/* end node */
	}
};

static const struct v4l2_subdev_ops hdmi_sd_ops;

static struct hdmi_device *sd_to_hdmi_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct hdmi_device, sd);
}

static inline
void hdmi_write(struct hdmi_device *hdev, u32 reg_id, u32 value)
{
	writel(value, hdev->regs + reg_id);
}

static inline
void hdmi_write_mask(struct hdmi_device *hdev, u32 reg_id, u32 value, u32 mask)
{
	u32 old = readl(hdev->regs + reg_id);
	value = (value & mask) | (old & ~mask);
	writel(value, hdev->regs + reg_id);
}

static inline
void hdmi_writeb(struct hdmi_device *hdev, u32 reg_id, u8 value)
{
	writeb(value, hdev->regs + reg_id);
}

static inline
void hdmi_writebn(struct hdmi_device *hdev, u32 reg_id, int n, u32 value)
{
	switch (n) {
	default:
		writeb(value >> 24, hdev->regs + reg_id + 12);
	case 3:
		writeb(value >> 16, hdev->regs + reg_id + 8);
	case 2:
		writeb(value >>  8, hdev->regs + reg_id + 4);
	case 1:
		writeb(value >>  0, hdev->regs + reg_id + 0);
	}
}

static inline u32 hdmi_read(struct hdmi_device *hdev, u32 reg_id)
{
	return readl(hdev->regs + reg_id);
}

static irqreturn_t hdmi_irq_handler(int irq, void *dev_data)
{
	struct hdmi_device *hdev = dev_data;
	u32 intc_flag;

	(void)irq;
	intc_flag = hdmi_read(hdev, HDMI_INTC_FLAG);
	/* clearing flags for HPD plug/unplug */
	if (intc_flag & HDMI_INTC_FLAG_HPD_UNPLUG) {
		pr_info("unplugged\n");
		hdmi_write_mask(hdev, HDMI_INTC_FLAG, ~0,
			HDMI_INTC_FLAG_HPD_UNPLUG);
	}
	if (intc_flag & HDMI_INTC_FLAG_HPD_PLUG) {
		pr_info("plugged\n");
		hdmi_write_mask(hdev, HDMI_INTC_FLAG, ~0,
			HDMI_INTC_FLAG_HPD_PLUG);
	}

	return IRQ_HANDLED;
}

static void hdmi_reg_init(struct hdmi_device *hdev)
{
	/* enable HPD interrupts */
	hdmi_write_mask(hdev, HDMI_INTC_CON, ~0, HDMI_INTC_EN_GLOBAL |
		HDMI_INTC_EN_HPD_PLUG | HDMI_INTC_EN_HPD_UNPLUG);
	/* choose DVI mode */
	hdmi_write_mask(hdev, HDMI_MODE_SEL,
		HDMI_MODE_DVI_EN, HDMI_MODE_MASK);
	hdmi_write_mask(hdev, HDMI_CON_2, ~0,
		HDMI_DVI_PERAMBLE_EN | HDMI_DVI_BAND_EN);
	/* disable bluescreen */
	hdmi_write_mask(hdev, HDMI_CON_0, 0, HDMI_BLUE_SCR_EN);
	/* choose bluescreen (fecal) color */
	hdmi_writeb(hdev, HDMI_BLUE_SCREEN_0, 0x12);
	hdmi_writeb(hdev, HDMI_BLUE_SCREEN_1, 0x34);
	hdmi_writeb(hdev, HDMI_BLUE_SCREEN_2, 0x56);
}

static void hdmi_timing_apply(struct hdmi_device *hdev,
	const struct hdmi_timings *t)
{
	/* setting core registers */
	hdmi_writebn(hdev, HDMI_H_BLANK_0, 2, t->hact.beg);
	hdmi_writebn(hdev, HDMI_H_SYNC_GEN_0, 3,
		(t->hsyn_pol << 20) | (t->hsyn.end << 10) | t->hsyn.beg);
	hdmi_writeb(hdev, HDMI_VSYNC_POL, t->vsyn_pol);
	hdmi_writebn(hdev, HDMI_V_BLANK_0, 3,
		(t->vact[0].beg << 11) | t->vact[0].end);
	hdmi_writebn(hdev, HDMI_V_SYNC_GEN_1_0, 3,
		(t->vsyn[0].beg << 12) | t->vsyn[0].end);
	if (t->interlaced) {
		u32 vsyn_trans = t->hsyn.beg + t->vsyn_off;

		hdmi_writeb(hdev, HDMI_INT_PRO_MODE, 1);
		hdmi_writebn(hdev, HDMI_H_V_LINE_0, 3,
			(t->hact.end << 12) | t->vact[1].end);
		hdmi_writebn(hdev, HDMI_V_BLANK_F_0, 3,
			(t->vact[1].end << 11) | t->vact[1].beg);
		hdmi_writebn(hdev, HDMI_V_SYNC_GEN_2_0, 3,
			(t->vsyn[1].beg << 12) | t->vsyn[1].end);
		hdmi_writebn(hdev, HDMI_V_SYNC_GEN_3_0, 3,
			(vsyn_trans << 12) | vsyn_trans);
	} else {
		hdmi_writeb(hdev, HDMI_INT_PRO_MODE, 0);
		hdmi_writebn(hdev, HDMI_H_V_LINE_0, 3,
			(t->hact.end << 12) | t->vact[0].end);
	}

	/* Timing generator registers */
	hdmi_writebn(hdev, HDMI_TG_H_FSZ_L, 2, t->hact.end);
	hdmi_writebn(hdev, HDMI_TG_HACT_ST_L, 2, t->hact.beg);
	hdmi_writebn(hdev, HDMI_TG_HACT_SZ_L, 2, t->hact.end - t->hact.beg);
	hdmi_writebn(hdev, HDMI_TG_VSYNC_L, 2, t->vsyn[0].beg);
	hdmi_writebn(hdev, HDMI_TG_VACT_ST_L, 2, t->vact[0].beg);
	hdmi_writebn(hdev, HDMI_TG_VACT_SZ_L, 2,
		t->vact[0].end - t->vact[0].beg);
	hdmi_writebn(hdev, HDMI_TG_VSYNC_TOP_HDMI_L, 2, t->vsyn[0].beg);
	hdmi_writebn(hdev, HDMI_TG_FIELD_TOP_HDMI_L, 2, t->vsyn[0].beg);
	if (t->interlaced) {
		hdmi_write_mask(hdev, HDMI_TG_CMD, ~0, HDMI_TG_FIELD_EN);
		hdmi_writebn(hdev, HDMI_TG_V_FSZ_L, 2, t->vact[1].end);
		hdmi_writebn(hdev, HDMI_TG_VSYNC2_L, 2, t->vsyn[1].beg);
		hdmi_writebn(hdev, HDMI_TG_FIELD_CHG_L, 2, t->vact[0].end);
		hdmi_writebn(hdev, HDMI_TG_VACT_ST2_L, 2, t->vact[1].beg);
		hdmi_writebn(hdev, HDMI_TG_VSYNC_BOT_HDMI_L, 2, t->vsyn[1].beg);
		hdmi_writebn(hdev, HDMI_TG_FIELD_BOT_HDMI_L, 2, t->vsyn[1].beg);
	} else {
		hdmi_write_mask(hdev, HDMI_TG_CMD, 0, HDMI_TG_FIELD_EN);
		hdmi_writebn(hdev, HDMI_TG_V_FSZ_L, 2, t->vact[0].end);
	}
}

static int hdmi_conf_apply(struct hdmi_device *hdmi_dev)
{
	struct device *dev = hdmi_dev->dev;
	const struct hdmi_timings *conf = hdmi_dev->cur_conf;
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	/* skip if conf is already synchronized with HW */
	if (!hdmi_dev->cur_conf_dirty)
		return 0;

	/* reset hdmiphy */
	hdmi_write_mask(hdmi_dev, HDMI_PHY_RSTOUT, ~0, HDMI_PHY_SW_RSTOUT);
	mdelay(10);
	hdmi_write_mask(hdmi_dev, HDMI_PHY_RSTOUT,  0, HDMI_PHY_SW_RSTOUT);
	mdelay(10);

	/* configure timings */
	ret = v4l2_subdev_call(hdmi_dev->phy_sd, video, s_dv_timings,
				&hdmi_dev->cur_timings);
	if (ret) {
		dev_err(dev, "failed to set timings\n");
		return ret;
	}

	/* resetting HDMI core */
	hdmi_write_mask(hdmi_dev, HDMI_CORE_RSTOUT,  0, HDMI_CORE_SW_RSTOUT);
	mdelay(10);
	hdmi_write_mask(hdmi_dev, HDMI_CORE_RSTOUT, ~0, HDMI_CORE_SW_RSTOUT);
	mdelay(10);

	hdmi_reg_init(hdmi_dev);

	/* setting core registers */
	hdmi_timing_apply(hdmi_dev, conf);

	hdmi_dev->cur_conf_dirty = 0;

	return 0;
}

static void hdmi_dumpregs(struct hdmi_device *hdev, char *prefix)
{
#define DUMPREG(reg_id) \
	dev_dbg(hdev->dev, "%s:" #reg_id " = %08x\n", prefix, \
		readl(hdev->regs + reg_id))

	dev_dbg(hdev->dev, "%s: ---- CONTROL REGISTERS ----\n", prefix);
	DUMPREG(HDMI_INTC_FLAG);
	DUMPREG(HDMI_INTC_CON);
	DUMPREG(HDMI_HPD_STATUS);
	DUMPREG(HDMI_PHY_RSTOUT);
	DUMPREG(HDMI_PHY_VPLL);
	DUMPREG(HDMI_PHY_CMU);
	DUMPREG(HDMI_CORE_RSTOUT);

	dev_dbg(hdev->dev, "%s: ---- CORE REGISTERS ----\n", prefix);
	DUMPREG(HDMI_CON_0);
	DUMPREG(HDMI_CON_1);
	DUMPREG(HDMI_CON_2);
	DUMPREG(HDMI_SYS_STATUS);
	DUMPREG(HDMI_PHY_STATUS);
	DUMPREG(HDMI_STATUS_EN);
	DUMPREG(HDMI_HPD);
	DUMPREG(HDMI_MODE_SEL);
	DUMPREG(HDMI_HPD_GEN);
	DUMPREG(HDMI_DC_CONTROL);
	DUMPREG(HDMI_VIDEO_PATTERN_GEN);

	dev_dbg(hdev->dev, "%s: ---- CORE SYNC REGISTERS ----\n", prefix);
	DUMPREG(HDMI_H_BLANK_0);
	DUMPREG(HDMI_H_BLANK_1);
	DUMPREG(HDMI_V_BLANK_0);
	DUMPREG(HDMI_V_BLANK_1);
	DUMPREG(HDMI_V_BLANK_2);
	DUMPREG(HDMI_H_V_LINE_0);
	DUMPREG(HDMI_H_V_LINE_1);
	DUMPREG(HDMI_H_V_LINE_2);
	DUMPREG(HDMI_VSYNC_POL);
	DUMPREG(HDMI_INT_PRO_MODE);
	DUMPREG(HDMI_V_BLANK_F_0);
	DUMPREG(HDMI_V_BLANK_F_1);
	DUMPREG(HDMI_V_BLANK_F_2);
	DUMPREG(HDMI_H_SYNC_GEN_0);
	DUMPREG(HDMI_H_SYNC_GEN_1);
	DUMPREG(HDMI_H_SYNC_GEN_2);
	DUMPREG(HDMI_V_SYNC_GEN_1_0);
	DUMPREG(HDMI_V_SYNC_GEN_1_1);
	DUMPREG(HDMI_V_SYNC_GEN_1_2);
	DUMPREG(HDMI_V_SYNC_GEN_2_0);
	DUMPREG(HDMI_V_SYNC_GEN_2_1);
	DUMPREG(HDMI_V_SYNC_GEN_2_2);
	DUMPREG(HDMI_V_SYNC_GEN_3_0);
	DUMPREG(HDMI_V_SYNC_GEN_3_1);
	DUMPREG(HDMI_V_SYNC_GEN_3_2);

	dev_dbg(hdev->dev, "%s: ---- TG REGISTERS ----\n", prefix);
	DUMPREG(HDMI_TG_CMD);
	DUMPREG(HDMI_TG_H_FSZ_L);
	DUMPREG(HDMI_TG_H_FSZ_H);
	DUMPREG(HDMI_TG_HACT_ST_L);
	DUMPREG(HDMI_TG_HACT_ST_H);
	DUMPREG(HDMI_TG_HACT_SZ_L);
	DUMPREG(HDMI_TG_HACT_SZ_H);
	DUMPREG(HDMI_TG_V_FSZ_L);
	DUMPREG(HDMI_TG_V_FSZ_H);
	DUMPREG(HDMI_TG_VSYNC_L);
	DUMPREG(HDMI_TG_VSYNC_H);
	DUMPREG(HDMI_TG_VSYNC2_L);
	DUMPREG(HDMI_TG_VSYNC2_H);
	DUMPREG(HDMI_TG_VACT_ST_L);
	DUMPREG(HDMI_TG_VACT_ST_H);
	DUMPREG(HDMI_TG_VACT_SZ_L);
	DUMPREG(HDMI_TG_VACT_SZ_H);
	DUMPREG(HDMI_TG_FIELD_CHG_L);
	DUMPREG(HDMI_TG_FIELD_CHG_H);
	DUMPREG(HDMI_TG_VACT_ST2_L);
	DUMPREG(HDMI_TG_VACT_ST2_H);
	DUMPREG(HDMI_TG_VSYNC_TOP_HDMI_L);
	DUMPREG(HDMI_TG_VSYNC_TOP_HDMI_H);
	DUMPREG(HDMI_TG_VSYNC_BOT_HDMI_L);
	DUMPREG(HDMI_TG_VSYNC_BOT_HDMI_H);
	DUMPREG(HDMI_TG_FIELD_TOP_HDMI_L);
	DUMPREG(HDMI_TG_FIELD_TOP_HDMI_H);
	DUMPREG(HDMI_TG_FIELD_BOT_HDMI_L);
	DUMPREG(HDMI_TG_FIELD_BOT_HDMI_H);
#undef DUMPREG
}

static const struct hdmi_timings hdmi_timings_480p = {
	.hact = { .beg = 138, .end = 858 },
	.hsyn_pol = 1,
	.hsyn = { .beg = 16, .end = 16 + 62 },
	.interlaced = 0,
	.vact[0] = { .beg = 42 + 3, .end = 522 + 3 },
	.vsyn_pol = 1,
	.vsyn[0] = { .beg = 6 + 3, .end = 12 + 3},
};

static const struct hdmi_timings hdmi_timings_576p50 = {
	.hact = { .beg = 144, .end = 864 },
	.hsyn_pol = 1,
	.hsyn = { .beg = 12, .end = 12 + 64 },
	.interlaced = 0,
	.vact[0] = { .beg = 44 + 5, .end = 620 + 5 },
	.vsyn_pol = 1,
	.vsyn[0] = { .beg = 0 + 5, .end = 5 + 5},
};

static const struct hdmi_timings hdmi_timings_720p60 = {
	.hact = { .beg = 370, .end = 1650 },
	.hsyn_pol = 0,
	.hsyn = { .beg = 110, .end = 110 + 40 },
	.interlaced = 0,
	.vact[0] = { .beg = 25 + 5, .end = 745 + 5 },
	.vsyn_pol = 0,
	.vsyn[0] = { .beg = 0 + 5, .end = 5 + 5},
};

static const struct hdmi_timings hdmi_timings_720p50 = {
	.hact = { .beg = 700, .end = 1980 },
	.hsyn_pol = 0,
	.hsyn = { .beg = 440, .end = 440 + 40 },
	.interlaced = 0,
	.vact[0] = { .beg = 25 + 5, .end = 745 + 5 },
	.vsyn_pol = 0,
	.vsyn[0] = { .beg = 0 + 5, .end = 5 + 5},
};

static const struct hdmi_timings hdmi_timings_1080p24 = {
	.hact = { .beg = 830, .end = 2750 },
	.hsyn_pol = 0,
	.hsyn = { .beg = 638, .end = 638 + 44 },
	.interlaced = 0,
	.vact[0] = { .beg = 41 + 4, .end = 1121 + 4 },
	.vsyn_pol = 0,
	.vsyn[0] = { .beg = 0 + 4, .end = 5 + 4},
};

static const struct hdmi_timings hdmi_timings_1080p60 = {
	.hact = { .beg = 280, .end = 2200 },
	.hsyn_pol = 0,
	.hsyn = { .beg = 88, .end = 88 + 44 },
	.interlaced = 0,
	.vact[0] = { .beg = 41 + 4, .end = 1121 + 4 },
	.vsyn_pol = 0,
	.vsyn[0] = { .beg = 0 + 4, .end = 5 + 4},
};

static const struct hdmi_timings hdmi_timings_1080i60 = {
	.hact = { .beg = 280, .end = 2200 },
	.hsyn_pol = 0,
	.hsyn = { .beg = 88, .end = 88 + 44 },
	.interlaced = 1,
	.vact[0] = { .beg = 20 + 2, .end = 560 + 2 },
	.vact[1] = { .beg = 583 + 2, .end = 1123 + 2 },
	.vsyn_pol = 0,
	.vsyn_off = 1100,
	.vsyn[0] = { .beg = 0 + 2, .end = 5 + 2},
	.vsyn[1] = { .beg = 562 + 2, .end = 567 + 2},
};

static const struct hdmi_timings hdmi_timings_1080i50 = {
	.hact = { .beg = 720, .end = 2640 },
	.hsyn_pol = 0,
	.hsyn = { .beg = 528, .end = 528 + 44 },
	.interlaced = 1,
	.vact[0] = { .beg = 20 + 2, .end = 560 + 2 },
	.vact[1] = { .beg = 583 + 2, .end = 1123 + 2 },
	.vsyn_pol = 0,
	.vsyn_off = 1320,
	.vsyn[0] = { .beg = 0 + 2, .end = 5 + 2},
	.vsyn[1] = { .beg = 562 + 2, .end = 567 + 2},
};

static const struct hdmi_timings hdmi_timings_1080p50 = {
	.hact = { .beg = 720, .end = 2640 },
	.hsyn_pol = 0,
	.hsyn = { .beg = 528, .end = 528 + 44 },
	.interlaced = 0,
	.vact[0] = { .beg = 41 + 4, .end = 1121 + 4 },
	.vsyn_pol = 0,
	.vsyn[0] = { .beg = 0 + 4, .end = 5 + 4},
};

/* default hdmi_timings index of the timings configured on probe */
#define HDMI_DEFAULT_TIMINGS_IDX (0)

static const struct {
	bool reduced_fps;
	const struct v4l2_dv_timings dv_timings;
	const struct hdmi_timings *hdmi_timings;
} hdmi_timings[] = {
	{ false, V4L2_DV_BT_CEA_720X480P59_94, &hdmi_timings_480p    },
	{ false, V4L2_DV_BT_CEA_720X576P50,    &hdmi_timings_576p50  },
	{ false, V4L2_DV_BT_CEA_1280X720P50,   &hdmi_timings_720p50  },
	{ true,  V4L2_DV_BT_CEA_1280X720P60,   &hdmi_timings_720p60  },
	{ false, V4L2_DV_BT_CEA_1920X1080P24,  &hdmi_timings_1080p24 },
	{ false, V4L2_DV_BT_CEA_1920X1080P30,  &hdmi_timings_1080p60 },
	{ false, V4L2_DV_BT_CEA_1920X1080P50,  &hdmi_timings_1080p50 },
	{ false, V4L2_DV_BT_CEA_1920X1080I50,  &hdmi_timings_1080i50 },
	{ false, V4L2_DV_BT_CEA_1920X1080I60,  &hdmi_timings_1080i60 },
	{ false, V4L2_DV_BT_CEA_1920X1080P60,  &hdmi_timings_1080p60 },
};

static int hdmi_streamon(struct hdmi_device *hdev)
{
	struct device *dev = hdev->dev;
	struct hdmi_resources *res = &hdev->res;
	int ret, tries;

	dev_dbg(dev, "%s\n", __func__);

	ret = hdmi_conf_apply(hdev);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(hdev->phy_sd, video, s_stream, 1);
	if (ret)
		return ret;

	/* waiting for HDMIPHY's PLL to get to steady state */
	for (tries = 100; tries; --tries) {
		u32 val = hdmi_read(hdev, HDMI_PHY_STATUS);
		if (val & HDMI_PHY_STATUS_READY)
			break;
		mdelay(1);
	}
	/* steady state not achieved */
	if (tries == 0) {
		dev_err(dev, "hdmiphy's pll could not reach steady state.\n");
		v4l2_subdev_call(hdev->phy_sd, video, s_stream, 0);
		hdmi_dumpregs(hdev, "hdmiphy - s_stream");
		return -EIO;
	}

	/* starting MHL */
	ret = v4l2_subdev_call(hdev->mhl_sd, video, s_stream, 1);
	if (hdev->mhl_sd && ret) {
		v4l2_subdev_call(hdev->phy_sd, video, s_stream, 0);
		hdmi_dumpregs(hdev, "mhl - s_stream");
		return -EIO;
	}

	/* hdmiphy clock is used for HDMI in streaming mode */
	clk_disable(res->sclk_hdmi);
	clk_set_parent(res->sclk_hdmi, res->sclk_hdmiphy);
	clk_enable(res->sclk_hdmi);

	/* enable HDMI and timing generator */
	hdmi_write_mask(hdev, HDMI_CON_0, ~0, HDMI_EN);
	hdmi_write_mask(hdev, HDMI_TG_CMD, ~0, HDMI_TG_EN);
	hdmi_dumpregs(hdev, "streamon");
	return 0;
}

static int hdmi_streamoff(struct hdmi_device *hdev)
{
	struct device *dev = hdev->dev;
	struct hdmi_resources *res = &hdev->res;

	dev_dbg(dev, "%s\n", __func__);

	hdmi_write_mask(hdev, HDMI_CON_0, 0, HDMI_EN);
	hdmi_write_mask(hdev, HDMI_TG_CMD, 0, HDMI_TG_EN);

	/* pixel(vpll) clock is used for HDMI in config mode */
	clk_disable(res->sclk_hdmi);
	clk_set_parent(res->sclk_hdmi, res->sclk_pixel);
	clk_enable(res->sclk_hdmi);

	v4l2_subdev_call(hdev->mhl_sd, video, s_stream, 0);
	v4l2_subdev_call(hdev->phy_sd, video, s_stream, 0);

	hdmi_dumpregs(hdev, "streamoff");
	return 0;
}

static int hdmi_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct hdmi_device *hdev = sd_to_hdmi_dev(sd);
	struct device *dev = hdev->dev;

	dev_dbg(dev, "%s(%d)\n", __func__, enable);
	if (enable)
		return hdmi_streamon(hdev);
	return hdmi_streamoff(hdev);
}

static int hdmi_resource_poweron(struct hdmi_resources *res)
{
	int ret;

	/* turn HDMI power on */
	ret = regulator_bulk_enable(res->regul_count, res->regul_bulk);
	if (ret < 0)
		return ret;
	/* power-on hdmi physical interface */
	clk_enable(res->hdmiphy);
	/* use VPP as parent clock; HDMIPHY is not working yet */
	clk_set_parent(res->sclk_hdmi, res->sclk_pixel);
	/* turn clocks on */
	clk_enable(res->sclk_hdmi);

	return 0;
}

static void hdmi_resource_poweroff(struct hdmi_resources *res)
{
	/* turn clocks off */
	clk_disable(res->sclk_hdmi);
	/* power-off hdmiphy */
	clk_disable(res->hdmiphy);
	/* turn HDMI power off */
	regulator_bulk_disable(res->regul_count, res->regul_bulk);
}

static int hdmi_s_power(struct v4l2_subdev *sd, int on)
{
	struct hdmi_device *hdev = sd_to_hdmi_dev(sd);
	int ret;

	if (on)
		ret = pm_runtime_get_sync(hdev->dev);
	else
		ret = pm_runtime_put_sync(hdev->dev);
	/* only values < 0 indicate errors */
	return IS_ERR_VALUE(ret) ? ret : 0;
}

static int hdmi_s_dv_timings(struct v4l2_subdev *sd,
	struct v4l2_dv_timings *timings)
{
	struct hdmi_device *hdev = sd_to_hdmi_dev(sd);
	struct device *dev = hdev->dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(hdmi_timings); i++)
		if (v4l2_match_dv_timings(&hdmi_timings[i].dv_timings,
					timings, 0))
			break;
	if (i == ARRAY_SIZE(hdmi_timings)) {
		dev_err(dev, "timings not supported\n");
		return -EINVAL;
	}
	hdev->cur_conf = hdmi_timings[i].hdmi_timings;
	hdev->cur_conf_dirty = 1;
	hdev->cur_timings = *timings;
	if (!hdmi_timings[i].reduced_fps)
		hdev->cur_timings.bt.flags &= ~V4L2_DV_FL_CAN_REDUCE_FPS;
	return 0;
}

static int hdmi_g_dv_timings(struct v4l2_subdev *sd,
	struct v4l2_dv_timings *timings)
{
	*timings = sd_to_hdmi_dev(sd)->cur_timings;
	return 0;
}

static int hdmi_g_mbus_fmt(struct v4l2_subdev *sd,
	  struct v4l2_mbus_framefmt *fmt)
{
	struct hdmi_device *hdev = sd_to_hdmi_dev(sd);
	const struct hdmi_timings *t = hdev->cur_conf;

	dev_dbg(hdev->dev, "%s\n", __func__);
	if (!hdev->cur_conf)
		return -EINVAL;
	memset(fmt, 0, sizeof(*fmt));
	fmt->width = t->hact.end - t->hact.beg;
	fmt->height = t->vact[0].end - t->vact[0].beg;
	fmt->code = V4L2_MBUS_FMT_FIXED; /* means RGB888 */
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	if (t->interlaced) {
		fmt->field = V4L2_FIELD_INTERLACED;
		fmt->height *= 2;
	} else {
		fmt->field = V4L2_FIELD_NONE;
	}
	return 0;
}

static int hdmi_enum_dv_timings(struct v4l2_subdev *sd,
	struct v4l2_enum_dv_timings *timings)
{
	if (timings->pad != 0)
		return -EINVAL;
	if (timings->index >= ARRAY_SIZE(hdmi_timings))
		return -EINVAL;
	timings->timings = hdmi_timings[timings->index].dv_timings;
	if (!hdmi_timings[timings->index].reduced_fps)
		timings->timings.bt.flags &= ~V4L2_DV_FL_CAN_REDUCE_FPS;
	return 0;
}

static int hdmi_dv_timings_cap(struct v4l2_subdev *sd,
	struct v4l2_dv_timings_cap *cap)
{
	struct hdmi_device *hdev = sd_to_hdmi_dev(sd);

	if (cap->pad != 0)
		return -EINVAL;

	/* Let the phy fill in the pixelclock range */
	v4l2_subdev_call(hdev->phy_sd, pad, dv_timings_cap, cap);
	cap->type = V4L2_DV_BT_656_1120;
	cap->bt.min_width = 720;
	cap->bt.max_width = 1920;
	cap->bt.min_height = 480;
	cap->bt.max_height = 1080;
	cap->bt.standards = V4L2_DV_BT_STD_CEA861;
	cap->bt.capabilities = V4L2_DV_BT_CAP_INTERLACED |
			       V4L2_DV_BT_CAP_PROGRESSIVE;
	return 0;
}

static const struct v4l2_subdev_core_ops hdmi_sd_core_ops = {
	.s_power = hdmi_s_power,
};

static const struct v4l2_subdev_video_ops hdmi_sd_video_ops = {
	.s_dv_timings = hdmi_s_dv_timings,
	.g_dv_timings = hdmi_g_dv_timings,
	.g_mbus_fmt = hdmi_g_mbus_fmt,
	.s_stream = hdmi_s_stream,
};

static const struct v4l2_subdev_pad_ops hdmi_sd_pad_ops = {
	.enum_dv_timings = hdmi_enum_dv_timings,
	.dv_timings_cap = hdmi_dv_timings_cap,
};

static const struct v4l2_subdev_ops hdmi_sd_ops = {
	.core = &hdmi_sd_core_ops,
	.video = &hdmi_sd_video_ops,
};

static int hdmi_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hdmi_device *hdev = sd_to_hdmi_dev(sd);

	dev_dbg(dev, "%s\n", __func__);
	v4l2_subdev_call(hdev->mhl_sd, core, s_power, 0);
	hdmi_resource_poweroff(&hdev->res);
	/* flag that device context is lost */
	hdev->cur_conf_dirty = 1;
	return 0;
}

static int hdmi_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hdmi_device *hdev = sd_to_hdmi_dev(sd);
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	ret = hdmi_resource_poweron(&hdev->res);
	if (ret < 0)
		return ret;

	/* starting MHL */
	ret = v4l2_subdev_call(hdev->mhl_sd, core, s_power, 1);
	if (hdev->mhl_sd && ret)
		goto fail;

	dev_dbg(dev, "poweron succeed\n");

	return 0;

fail:
	hdmi_resource_poweroff(&hdev->res);
	dev_err(dev, "poweron failed\n");

	return ret;
}

static const struct dev_pm_ops hdmi_pm_ops = {
	.runtime_suspend = hdmi_runtime_suspend,
	.runtime_resume	 = hdmi_runtime_resume,
};

static void hdmi_resource_clear_clocks(struct hdmi_resources *res)
{
	res->hdmi	 = ERR_PTR(-EINVAL);
	res->sclk_hdmi	 = ERR_PTR(-EINVAL);
	res->sclk_pixel	 = ERR_PTR(-EINVAL);
	res->sclk_hdmiphy = ERR_PTR(-EINVAL);
	res->hdmiphy	 = ERR_PTR(-EINVAL);
}

static void hdmi_resources_cleanup(struct hdmi_device *hdev)
{
	struct hdmi_resources *res = &hdev->res;

	dev_dbg(hdev->dev, "HDMI resource cleanup\n");
	/* put clocks, power */
	if (res->regul_count)
		regulator_bulk_free(res->regul_count, res->regul_bulk);
	/* kfree is NULL-safe */
	kfree(res->regul_bulk);
	if (!IS_ERR(res->hdmiphy))
		clk_put(res->hdmiphy);
	if (!IS_ERR(res->sclk_hdmiphy))
		clk_put(res->sclk_hdmiphy);
	if (!IS_ERR(res->sclk_pixel))
		clk_put(res->sclk_pixel);
	if (!IS_ERR(res->sclk_hdmi))
		clk_put(res->sclk_hdmi);
	if (!IS_ERR(res->hdmi))
		clk_put(res->hdmi);
	memset(res, 0, sizeof(*res));
	hdmi_resource_clear_clocks(res);
}

static int hdmi_resources_init(struct hdmi_device *hdev)
{
	struct device *dev = hdev->dev;
	struct hdmi_resources *res = &hdev->res;
	static char *supply[] = {
		"hdmi-en",
		"vdd",
		"vdd_osc",
		"vdd_pll",
	};
	int i, ret;

	dev_dbg(dev, "HDMI resource init\n");

	memset(res, 0, sizeof(*res));
	hdmi_resource_clear_clocks(res);

	/* get clocks, power */
	res->hdmi = clk_get(dev, "hdmi");
	if (IS_ERR(res->hdmi)) {
		dev_err(dev, "failed to get clock 'hdmi'\n");
		goto fail;
	}
	res->sclk_hdmi = clk_get(dev, "sclk_hdmi");
	if (IS_ERR(res->sclk_hdmi)) {
		dev_err(dev, "failed to get clock 'sclk_hdmi'\n");
		goto fail;
	}
	res->sclk_pixel = clk_get(dev, "sclk_pixel");
	if (IS_ERR(res->sclk_pixel)) {
		dev_err(dev, "failed to get clock 'sclk_pixel'\n");
		goto fail;
	}
	res->sclk_hdmiphy = clk_get(dev, "sclk_hdmiphy");
	if (IS_ERR(res->sclk_hdmiphy)) {
		dev_err(dev, "failed to get clock 'sclk_hdmiphy'\n");
		goto fail;
	}
	res->hdmiphy = clk_get(dev, "hdmiphy");
	if (IS_ERR(res->hdmiphy)) {
		dev_err(dev, "failed to get clock 'hdmiphy'\n");
		goto fail;
	}
	res->regul_bulk = kcalloc(ARRAY_SIZE(supply),
				  sizeof(res->regul_bulk[0]), GFP_KERNEL);
	if (!res->regul_bulk) {
		dev_err(dev, "failed to get memory for regulators\n");
		goto fail;
	}
	for (i = 0; i < ARRAY_SIZE(supply); ++i) {
		res->regul_bulk[i].supply = supply[i];
		res->regul_bulk[i].consumer = NULL;
	}

	ret = regulator_bulk_get(dev, ARRAY_SIZE(supply), res->regul_bulk);
	if (ret) {
		dev_err(dev, "failed to get regulators\n");
		goto fail;
	}
	res->regul_count = ARRAY_SIZE(supply);

	return 0;
fail:
	dev_err(dev, "HDMI resource init - failed\n");
	hdmi_resources_cleanup(hdev);
	return -ENODEV;
}

static int hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct i2c_adapter *adapter;
	struct v4l2_subdev *sd;
	struct hdmi_device *hdmi_dev = NULL;
	struct s5p_hdmi_platform_data *pdata = dev->platform_data;
	int ret;

	dev_dbg(dev, "probe start\n");

	if (!pdata) {
		dev_err(dev, "platform data is missing\n");
		ret = -ENODEV;
		goto fail;
	}

	hdmi_dev = devm_kzalloc(&pdev->dev, sizeof(*hdmi_dev), GFP_KERNEL);
	if (!hdmi_dev) {
		dev_err(dev, "out of memory\n");
		ret = -ENOMEM;
		goto fail;
	}

	hdmi_dev->dev = dev;

	ret = hdmi_resources_init(hdmi_dev);
	if (ret)
		goto fail;

	/* mapping HDMI registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		dev_err(dev, "get memory resource failed.\n");
		ret = -ENXIO;
		goto fail_init;
	}

	hdmi_dev->regs = devm_ioremap(&pdev->dev, res->start,
				      resource_size(res));
	if (hdmi_dev->regs == NULL) {
		dev_err(dev, "register mapping failed.\n");
		ret = -ENXIO;
		goto fail_init;
	}

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		dev_err(dev, "get interrupt resource failed.\n");
		ret = -ENXIO;
		goto fail_init;
	}

	ret = devm_request_irq(&pdev->dev, res->start, hdmi_irq_handler, 0,
			       "hdmi", hdmi_dev);
	if (ret) {
		dev_err(dev, "request interrupt failed.\n");
		goto fail_init;
	}
	hdmi_dev->irq = res->start;

	/* setting v4l2 name to prevent WARN_ON in v4l2_device_register */
	strlcpy(hdmi_dev->v4l2_dev.name, dev_name(dev),
		sizeof(hdmi_dev->v4l2_dev.name));
	/* passing NULL owner prevents driver from erasing drvdata */
	ret = v4l2_device_register(NULL, &hdmi_dev->v4l2_dev);
	if (ret) {
		dev_err(dev, "could not register v4l2 device.\n");
		goto fail_init;
	}

	/* testing if hdmiphy info is present */
	if (!pdata->hdmiphy_info) {
		dev_err(dev, "hdmiphy info is missing in platform data\n");
		ret = -ENXIO;
		goto fail_vdev;
	}

	adapter = i2c_get_adapter(pdata->hdmiphy_bus);
	if (adapter == NULL) {
		dev_err(dev, "hdmiphy adapter request failed\n");
		ret = -ENXIO;
		goto fail_vdev;
	}

	hdmi_dev->phy_sd = v4l2_i2c_new_subdev_board(&hdmi_dev->v4l2_dev,
		adapter, pdata->hdmiphy_info, NULL);
	/* on failure or not adapter is no longer useful */
	i2c_put_adapter(adapter);
	if (hdmi_dev->phy_sd == NULL) {
		dev_err(dev, "missing subdev for hdmiphy\n");
		ret = -ENODEV;
		goto fail_vdev;
	}

	/* initialization of MHL interface if present */
	if (pdata->mhl_info) {
		adapter = i2c_get_adapter(pdata->mhl_bus);
		if (adapter == NULL) {
			dev_err(dev, "MHL adapter request failed\n");
			ret = -ENXIO;
			goto fail_vdev;
		}

		hdmi_dev->mhl_sd = v4l2_i2c_new_subdev_board(
			&hdmi_dev->v4l2_dev, adapter,
			pdata->mhl_info, NULL);
		/* on failure or not adapter is no longer useful */
		i2c_put_adapter(adapter);
		if (hdmi_dev->mhl_sd == NULL) {
			dev_err(dev, "missing subdev for MHL\n");
			ret = -ENODEV;
			goto fail_vdev;
		}
	}

	clk_enable(hdmi_dev->res.hdmi);

	pm_runtime_enable(dev);

	sd = &hdmi_dev->sd;
	v4l2_subdev_init(sd, &hdmi_sd_ops);
	sd->owner = THIS_MODULE;

	strlcpy(sd->name, "s5p-hdmi", sizeof(sd->name));
	hdmi_dev->cur_timings =
		hdmi_timings[HDMI_DEFAULT_TIMINGS_IDX].dv_timings;
	/* FIXME: missing fail timings is not supported */
	hdmi_dev->cur_conf =
		hdmi_timings[HDMI_DEFAULT_TIMINGS_IDX].hdmi_timings;
	hdmi_dev->cur_conf_dirty = 1;

	/* storing subdev for call that have only access to struct device */
	dev_set_drvdata(dev, sd);

	dev_info(dev, "probe successful\n");

	return 0;

fail_vdev:
	v4l2_device_unregister(&hdmi_dev->v4l2_dev);

fail_init:
	hdmi_resources_cleanup(hdmi_dev);

fail:
	dev_err(dev, "probe failed\n");
	return ret;
}

static int hdmi_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct hdmi_device *hdmi_dev = sd_to_hdmi_dev(sd);

	pm_runtime_disable(dev);
	clk_disable(hdmi_dev->res.hdmi);
	v4l2_device_unregister(&hdmi_dev->v4l2_dev);
	disable_irq(hdmi_dev->irq);
	hdmi_resources_cleanup(hdmi_dev);
	dev_info(dev, "remove successful\n");

	return 0;
}

static struct platform_driver hdmi_driver __refdata = {
	.probe = hdmi_probe,
	.remove = hdmi_remove,
	.id_table = hdmi_driver_types,
	.driver = {
		.name = "s5p-hdmi",
		.owner = THIS_MODULE,
		.pm = &hdmi_pm_ops,
	}
};

module_platform_driver(hdmi_driver);

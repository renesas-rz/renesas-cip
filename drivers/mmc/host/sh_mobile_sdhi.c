/*
 * SuperH Mobile SDHI
 *
 * Copyright (C) 2009 Magnus Damm
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Based on "Compaq ASIC3 support":
 *
 * Copyright 2001 Compaq Computer Corporation.
 * Copyright 2004-2005 Phil Blundell
 * Copyright 2007-2008 OpenedHand Ltd.
 *
 * Authors: Phil Blundell <pb@handhelds.org>,
 *	    Samuel Ortiz <sameo@openedhand.com>
 *
 */

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sh_mobile_sdhi.h>
#include <linux/mfd/tmio.h>
#include <linux/sh_dma.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>

#include "tmio_mmc.h"

#define HOST_MODE           0xe4

#define host_to_priv(host) container_of((host)->pdata, struct sh_mobile_sdhi, mmc_data)

struct sh_mobile_sdhi_scc {
	unsigned long clk;	/* clock for SDR104 */
	u32 tap;		/* sampling clock position for SDR104 */
};

struct sh_mobile_sdhi_of_data {
	unsigned long tmio_flags;
	unsigned long capabilities;
	unsigned long capabilities2;
	enum dma_slave_buswidth dma_buswidth;
	dma_addr_t dma_rx_offset;
	unsigned int max_blk_count;
	unsigned short max_segs;
	bool sdbuf_64bit;
	int scc_offset;
	struct sh_mobile_sdhi_scc *taps;
	int taps_num;
};

static const struct sh_mobile_sdhi_of_data sh_mobile_sdhi_of_cfg[] = {
	{
		.tmio_flags = TMIO_MMC_HAS_IDLE_WAIT,
	},
};

static const struct sh_mobile_sdhi_of_data of_rcar_gen1_compatible = {
	.tmio_flags	= TMIO_MMC_HAS_IDLE_WAIT | TMIO_MMC_WRPROTECT_DISABLE |
			  TMIO_MMC_CLK_ACTUAL,
	.capabilities	= MMC_CAP_SD_HIGHSPEED | MMC_CAP_SDIO_IRQ,
};

/* Definitions for sampling clocks */
static struct sh_mobile_sdhi_scc rcar_gen2_scc_taps[] = {
	{
		.clk = 156000000,
		.tap = 0x00000703,
	},
	{
		.clk = 0,
		.tap = 0x00000300,
	},
};

static const struct sh_mobile_sdhi_of_data of_rcar_gen2_compatible = {
	.tmio_flags	= TMIO_MMC_HAS_IDLE_WAIT | TMIO_MMC_WRPROTECT_DISABLE |
			  TMIO_MMC_CLK_ACTUAL,
	.capabilities	= MMC_CAP_SD_HIGHSPEED | MMC_CAP_SDIO_IRQ,
	.dma_buswidth	= DMA_SLAVE_BUSWIDTH_4_BYTES,
	.dma_rx_offset	= 0x2000,
	.scc_offset = 0x0300,
	.taps = rcar_gen2_scc_taps,
	.taps_num = ARRAY_SIZE(rcar_gen2_scc_taps),
};

static const struct of_device_id sh_mobile_sdhi_of_match[] = {
	{ .compatible = "renesas,sdhi-shmobile" },
	{ .compatible = "renesas,sdhi-sh7372" },
	{ .compatible = "renesas,sdhi-sh73a0", .data = &sh_mobile_sdhi_of_cfg[0], },
	{ .compatible = "renesas,sdhi-r8a73a4", .data = &sh_mobile_sdhi_of_cfg[0], },
	{ .compatible = "renesas,sdhi-r8a7740", .data = &sh_mobile_sdhi_of_cfg[0], },
	{ .compatible = "renesas,sdhi-r8a7778", .data = &of_rcar_gen1_compatible, },
	{ .compatible = "renesas,sdhi-r8a7779", .data = &of_rcar_gen1_compatible, },
	{ .compatible = "renesas,sdhi-r8a7743", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7744", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7745", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7742", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7790", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7791", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7792", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7793", .data = &of_rcar_gen2_compatible, },
	{ .compatible = "renesas,sdhi-r8a7794", .data = &of_rcar_gen2_compatible, },
    { .compatible = "renesas,sdhi-r8a7745", .data = &of_rcar_gen2_compatible, },
    { .compatible = "renesas,sdhi-r8a77470", .data = &of_rcar_gen2_compatible, },
	{},
};
MODULE_DEVICE_TABLE(of, sh_mobile_sdhi_of_match);

struct sh_mobile_sdhi {
	struct clk *clk;
	struct tmio_mmc_data mmc_data;
	struct tmio_mmc_dma dma_priv;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default, *pins_uhs;
};

static void sh_mobile_sdhi_sdbuf_width(struct tmio_mmc_host *host, int width)
{
	u32 val;

	/*
	 * see also
	 *	sh_mobile_sdhi_of_data :: dma_buswidth
	 */
	switch (sd_ctrl_read16(host, CTL_VERSION)) {
	case 0x490C:
		val = (width == 32) ? 0x0001 : 0x0000;
		break;
	case 0xCB0D:
		val = (width == 32) ? 0x0000 : 0x0001;
		break;
	case 0xCC10:
	case 0xCD10:
		if (width == 64)
			val = 0x0000;
		else if (width == 32)
			val = 0x0101;
		else	/* width = 16 */
			val = 0x0001;
		break;
	default:
		/* nothing to do */
		return;
	}

	sd_ctrl_write16(host, HOST_MODE, val);
}

static int sh_mobile_sdhi_clk_enable(struct platform_device *pdev, unsigned int *f)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct sh_mobile_sdhi *priv = host_to_priv(host);
	int ret = clk_prepare_enable(priv->clk);
	if (ret < 0)
		return ret;

	*f = clk_get_rate(priv->clk);

	/* enable 16bit data access on SDBUF as default */
	sh_mobile_sdhi_sdbuf_width(host, 16);

	return 0;
}

static void sh_mobile_sdhi_clk_disable(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct tmio_mmc_host *host = mmc_priv(mmc);
	struct sh_mobile_sdhi *priv = host_to_priv(host);
	clk_disable_unprepare(priv->clk);
}

static void sh_mobile_sdhi_set_clk_div(struct platform_device *pdev,
				       int state)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct tmio_mmc_host *host = mmc_priv(mmc);

	if (state) {
		sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~0x0100 &
				sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
		sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, 0x00ff);
	}
}

#define SH_MOBILE_SDHI_DAT0	0x0080
static int sh_mobile_sdhi_card_busy(struct tmio_mmc_host *host)
{
	u16 dat0;

	/* check to see DAT[3:0] */
	dat0 = sd_ctrl_read16(host, CTL_STATUS2);
	return !(dat0 & SH_MOBILE_SDHI_DAT0);
}

#define SH_MOBILE_SDHI_SIGNAL_180V	0
#define SH_MOBILE_SDHI_SIGNAL_330V	1

static int sh_mobile_sdhi_set_ioctrl(struct tmio_mmc_host *host, int state)
{
	struct platform_device *pdev = host->pdev;
	struct sh_mobile_sdhi *priv =
		container_of(host->pdata, struct sh_mobile_sdhi, mmc_data);
	struct pinctrl_state *pstate;
	int ret;

	if (state == SH_MOBILE_SDHI_SIGNAL_330V) {
		pstate = priv->pins_default;
	} else if (state == SH_MOBILE_SDHI_SIGNAL_180V) {
		pstate = priv->pins_uhs;
	} else {
		dev_err(&pdev->dev, "update_ioctrl: unknown state\n");
		ret = -EINVAL;
		goto err;
	}

	if (!pstate) {
		ret = -EIO;
		goto err;
	}

	ret = pinctrl_select_state(priv->pinctrl, pstate);
	if (ret)
		goto err;

	return 0;

err:
	return ret;
}

static int sh_mobile_sdhi_start_signal_voltage_switch(
	struct tmio_mmc_host *host, unsigned char signal_voltage)
{
	struct mmc_host *mmc = host->mmc;
	int ret;

	if (signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
		/* Enable 3.3V Signal */
		if (!IS_ERR(mmc->supply.vqmmc)) {
			/* ioctrl */
			ret = sh_mobile_sdhi_set_ioctrl(host,
						SH_MOBILE_SDHI_SIGNAL_330V);
			if (ret) {
				dev_err(&host->pdev->dev,
					"3.3V pin function control failed\n");
				return -EIO;
			}

			ret = regulator_set_voltage(mmc->supply.vqmmc,
						    3300000, 3300000);
			if (ret) {
				dev_warn(&host->pdev->dev,
					 "3.3V signalling voltage failed\n");
				return -EIO;
			}
		} else {
			return -EIO;
		}
		usleep_range(5000, 10000);
	} else if (signal_voltage == MMC_SIGNAL_VOLTAGE_180) {
		/* Enable 1.8V Signal */
		if (!IS_ERR(mmc->supply.vqmmc)) {
			ret = regulator_set_voltage(mmc->supply.vqmmc,
						    1800000, 1800000);
			if (ret) {
				dev_warn(&host->pdev->dev,
					 "1.8V signalling voltage failed\n");
				return -EIO;
			}
			/* ioctrl */
			ret = sh_mobile_sdhi_set_ioctrl(host,
						SH_MOBILE_SDHI_SIGNAL_180V);
			if (ret) {
				dev_err(&host->pdev->dev,
					"1.8V pin function control failed\n");
				return -EIO;
			}
		} else {
			return -EIO;
		}
		/* Wait for 5ms */
		usleep_range(5000, 10000);
	} else {
		/* No signal voltage switch required */
	}

	return 0;
}

/* SCC registers */
#define SH_MOBILE_SDHI_SCC_DTCNTL	0x000
#define SH_MOBILE_SDHI_SCC_TAPSET	0x002
#define SH_MOBILE_SDHI_SCC_DT2FF	0x004
#define SH_MOBILE_SDHI_SCC_CKSEL	0x006
#define SH_MOBILE_SDHI_SCC_RVSCNTL	0x008
#define SH_MOBILE_SDHI_SCC_RVSREQ	0x00A

/* Definitions for values the SH_MOBILE_SDHI_SCC_DTCNTL register */
#define SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN		(1 << 0)
/* Definitions for values the SH_MOBILE_SDHI_SCC_CKSEL register */
#define SH_MOBILE_SDHI_SCC_CKSEL_DTSEL		(1 << 0)
/* Definitions for values the SH_MOBILE_SDHI_SCC_RVSCNTL register */
#define SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN	(1 << 0)
/* Definitions for values the SH_MOBILE_SDHI_SCC_RVSREQ register */
#define SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR	(1 << 2)

static inline u32 sd_scc_read32(struct tmio_mmc_host *host, int addr)
{
	struct platform_device *pdev = host->pdev;
	const struct of_device_id *of_id =
		of_match_device(sh_mobile_sdhi_of_match, &pdev->dev);
	const struct sh_mobile_sdhi_of_data *of_data = of_id->data;

	return readl(host->ctl + of_data->scc_offset +
		     (addr << host->bus_shift));
}

static inline void sd_scc_write32(struct tmio_mmc_host *host, int addr,
				  u32 val)
{
	struct platform_device *pdev = host->pdev;
	const struct of_device_id *of_id =
		of_match_device(sh_mobile_sdhi_of_match, &pdev->dev);
	const struct sh_mobile_sdhi_of_data *of_data = of_id->data;

	writel(val, host->ctl + of_data->scc_offset +
	       (addr << host->bus_shift));
}

static bool sh_mobile_sdhi_inquiry_tuning(struct tmio_mmc_host *host)
{
	/* SDHI should be tuning only SDR104 and HS200 */
	if (host->mmc->ios.timing == MMC_TIMING_UHS_SDR104 ||
	    host->mmc->ios.timing == MMC_TIMING_MMC_HS200)
		return true;

	return false;
}

static void sh_mobile_sdhi_init_tuning(struct tmio_mmc_host *host,
							unsigned long *num)
{
	/* set sampling clock selection range */
	if (host->scc_tapnum)
		sd_scc_write32(host, SH_MOBILE_SDHI_SCC_DTCNTL,
				host->scc_tapnum << 16);

	/* Initialize SCC */
	sd_ctrl_write32(host, CTL_STATUS, 0x00000000);

	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_DTCNTL,
		SH_MOBILE_SDHI_SCC_DTCNTL_TAPEN |
		sd_scc_read32(host, SH_MOBILE_SDHI_SCC_DTCNTL));

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~0x0100 &
		sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_CKSEL,
		SH_MOBILE_SDHI_SCC_CKSEL_DTSEL |
		sd_scc_read32(host, SH_MOBILE_SDHI_SCC_CKSEL));

	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, 0x0100 |
		sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_RVSCNTL,
		~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
		sd_scc_read32(host, SH_MOBILE_SDHI_SCC_RVSCNTL));

	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_DT2FF, host->scc_tappos);

	/* Read TAPNUM */
	*num = (sd_scc_read32(host, SH_MOBILE_SDHI_SCC_DTCNTL) >> 16) & 0xff;
}

static int sh_mobile_sdhi_prepare_tuning(struct tmio_mmc_host *host,
							unsigned long tap)
{
	/* Set sampling clock position */
	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_TAPSET, tap);

	return 0;
}

#define SH_MOBILE_SDHI_MAX_TAP	3
static int sh_mobile_sdhi_select_tuning(struct tmio_mmc_host *host,
							unsigned long *tap)
{
	unsigned long tap_num;	/* total number of taps */
	unsigned long tap_cnt;	/* counter of tuning success */
	unsigned long tap_set;	/* tap position */
	unsigned long tap_start;	/* start position of tuning success */
	unsigned long tap_end;	/* end position of tuning success */
	unsigned long ntap;	/* temporary counter of tuning success */
	unsigned long i;

	/* Clear SCC_RVSREQ */
	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_RVSREQ, 0);

	/* Select SCC */
	tap_num = (sd_scc_read32(host,
				 SH_MOBILE_SDHI_SCC_DTCNTL) >> 16) & 0xff;

	tap_cnt = 0;
	ntap = 0;
	tap_start = 0;
	tap_end = 0;
	for (i = 0; i < tap_num * 2; i++) {
		if (tap[i] == 0)
			ntap++;
		else {
			if (ntap > tap_cnt) {
				tap_start = i - ntap;
				tap_end = i - 1;
				tap_cnt = ntap;
			}
			ntap = 0;
		}
	}

	if (ntap > tap_cnt) {
		tap_start = i - ntap;
		tap_end = i - 1;
		tap_cnt = ntap;
	}

	if (tap_cnt >= SH_MOBILE_SDHI_MAX_TAP)
		tap_set = (tap_start + tap_end) / 2 % tap_num;
	else
		return -EIO;

	/* Set SCC */
	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_TAPSET, tap_set);

	/* Enable auto re-tuning */
	sd_scc_write32(host, SH_MOBILE_SDHI_SCC_RVSCNTL,
		SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN |
		sd_scc_read32(host, SH_MOBILE_SDHI_SCC_RVSCNTL));

	return 0;
}

static bool sh_mobile_sdhi_retuning(struct tmio_mmc_host *host)
{
	/* Check SCC error */
	if (sd_scc_read32(host, SH_MOBILE_SDHI_SCC_RVSCNTL) &
	    SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &&
	    sd_scc_read32(host, SH_MOBILE_SDHI_SCC_RVSREQ) &
	    SH_MOBILE_SDHI_SCC_RVSREQ_RVSERR) {
		/* Clear SCC error */
		sd_scc_write32(host, SH_MOBILE_SDHI_SCC_RVSREQ, 0);
		return true;
	}
	return false;
}

static void sh_mobile_sdhi_hw_reset(struct tmio_mmc_host *host)
{
	struct tmio_mmc_data *pdata = host->pdata;

	if (pdata->flags & TMIO_MMC_HAS_UHS_SCC) {
		/* Reset SCC */
		sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, ~0x0100 &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

		sd_scc_write32(host, SH_MOBILE_SDHI_SCC_CKSEL,
			~SH_MOBILE_SDHI_SCC_CKSEL_DTSEL &
			sd_scc_read32(host, SH_MOBILE_SDHI_SCC_CKSEL));

		sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, 0x0100 |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));

		sd_scc_write32(host, SH_MOBILE_SDHI_SCC_RVSCNTL,
			~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
			sd_scc_read32(host, SH_MOBILE_SDHI_SCC_RVSCNTL));

		sd_scc_write32(host, SH_MOBILE_SDHI_SCC_RVSCNTL,
			~SH_MOBILE_SDHI_SCC_RVSCNTL_RVSEN &
			sd_scc_read32(host, SH_MOBILE_SDHI_SCC_RVSCNTL));
	}
}

static int sh_mobile_sdhi_wait_idle(struct tmio_mmc_host *host)
{
	int timeout = 1000;

	while (--timeout && !(sd_ctrl_read16(host, CTL_STATUS2) & (1 << 13)))
		udelay(1);

	if (!timeout) {
		dev_warn(&host->pdev->dev, "timeout waiting for SD bus idle\n");
		return -EBUSY;
	}

	return 0;
}

static int sh_mobile_sdhi_write16_hook(struct tmio_mmc_host *host, int addr)
{
	switch (addr)
	{
	case CTL_SD_CMD:
	case CTL_STOP_INTERNAL_ACTION:
	case CTL_XFER_BLK_COUNT:
	case CTL_SD_CARD_CLK_CTL:
	case CTL_SD_XFER_LEN:
	case CTL_SD_MEM_CARD_OPT:
	case CTL_TRANSACTION_CTL:
	case CTL_DMA_ENABLE:
	case HOST_MODE:
		return sh_mobile_sdhi_wait_idle(host);
	}

	return 0;
}

static int sh_mobile_sdhi_multi_io_quirk(struct mmc_card *card,
					 unsigned int direction, int blk_size)
{
	/*
	 * In Renesas controllers, when performing a
	 * multiple block read of one or two blocks,
	 * depending on the timing with which the
	 * response register is read, the response
	 * value may not be read properly.
	 * Use single block read for this HW bug
	 */
	if ((direction == MMC_DATA_READ) &&
	    blk_size == 2)
		return 1;

	return blk_size;
}

static void sh_mobile_sdhi_enable_dma(struct tmio_mmc_host *host, bool enable)
{
	int dma_width = host->dma->sdbuf_64bit ? 64 : 32;

	sd_ctrl_write16(host, CTL_DMA_ENABLE, enable ? 2 : 0);

	/* enable 32bit access if DMA mode if possibile */
	sh_mobile_sdhi_sdbuf_width(host, enable ? dma_width : 16);
}

static int sh_mobile_sdhi_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id =
		of_match_device(sh_mobile_sdhi_of_match, &pdev->dev);
	struct sh_mobile_sdhi *priv;
	struct tmio_mmc_data *mmc_data;
	struct tmio_mmc_data *mmd = pdev->dev.platform_data;
	struct tmio_mmc_host *host;
	struct resource *res;
	struct device_node *np = pdev->dev.of_node;
	int irq, ret, i;
	bool multiplexed_isr = true;
	struct tmio_mmc_dma *dma_priv;
	int clk_rate;
	u32 num, tapnum = 0, tappos;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct sh_mobile_sdhi), GFP_KERNEL);
	if (priv == NULL) {
		dev_err(&pdev->dev, "kzalloc failed\n");
		return -ENOMEM;
	}

	mmc_data = &priv->mmc_data;
	dma_priv = &priv->dma_priv;

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		ret = PTR_ERR(priv->clk);
		dev_err(&pdev->dev, "cannot get clock: %d\n", ret);
		goto eprobe;
	}

	if (np && !of_property_read_u32(np, "renesas,clk-rate", &clk_rate)) {
		if (clk_rate) {
			clk_prepare_enable(priv->clk);
			ret = clk_set_rate(priv->clk, clk_rate);
			if (ret < 0)
				dev_err(&pdev->dev,
					"cannot set clock rate: %d\n", ret);

			clk_disable_unprepare(priv->clk);
		}
	}

	if (np && !of_property_read_u32(np, "renesas,mmc-scc-tapnum", &num))
		tapnum = num;

	priv->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (!IS_ERR(priv->pinctrl)) {
		const char *p;
		struct pinctrl_state *pstate;

		num = of_property_count_strings(np, "pinctrl-names");
		if (num < 1) {
			dev_err(&pdev->dev,
				"not find pinctrl for voltage switch\n");
			ret =  -ENODEV;
			goto eprobe;
		}

		for (i = 0; i < num; i++) {
			ret = of_property_read_string_index(np, "pinctrl-names",
							    i, &p);
			if (ret)
				continue;

			pstate = pinctrl_lookup_state(priv->pinctrl, p);
			if (IS_ERR(pstate))
				continue;

			if (!strcmp(p, "default"))
				priv->pins_default = pstate;
			else if (!strcmp(p, "uhs"))
				priv->pins_uhs = pstate;
		}

		if (!priv->pins_default && !priv->pins_uhs) {
			dev_err(&pdev->dev,
				"not find pinctrl state for voltage switch\n");
			ret =  -ENODEV;
			goto eprobe;
		}
	}

	host = tmio_mmc_host_alloc(pdev);
	if (!host) {
		ret = -ENOMEM;
		goto eprobe;
	}

	host->dma		= dma_priv;
	host->write16_hook	= sh_mobile_sdhi_write16_hook;
	host->clk_enable	= sh_mobile_sdhi_clk_enable;
	host->clk_disable	= sh_mobile_sdhi_clk_disable;
	host->card_busy		= sh_mobile_sdhi_card_busy;
	host->multi_io_quirk	= sh_mobile_sdhi_multi_io_quirk;
	host->set_clk_div	= sh_mobile_sdhi_set_clk_div;
	host->start_signal_voltage_switch =
			sh_mobile_sdhi_start_signal_voltage_switch;
	host->inquiry_tuning = sh_mobile_sdhi_inquiry_tuning;
	host->init_tuning = sh_mobile_sdhi_init_tuning;
	host->prepare_tuning = sh_mobile_sdhi_prepare_tuning;
	host->select_tuning = sh_mobile_sdhi_select_tuning;
	host->retuning = sh_mobile_sdhi_retuning;
	host->hw_reset = sh_mobile_sdhi_hw_reset;
	host->scc_tapnum = tapnum;
	/* SD control register space size */
	if (resource_size(res) > 0x400) /* 0x400 for bus_shift=2 */
		host->bus_shift = 2;
	else if (resource_size(res) > 0x100) /* 0x100, 0x200 for bus_shift=1 */
		host->bus_shift = 1;
	else
		host->bus_shift = 0;

	if (mmd)
		*mmc_data = *mmd;

	dma_priv->filter = shdma_chan_filter;
	dma_priv->enable = sh_mobile_sdhi_enable_dma;

	mmc_data->alignment_shift = 1; /* 2-byte alignment */
	mmc_data->capabilities |= MMC_CAP_MMC_HIGHSPEED;

	/*
	 * All SDHI blocks support 2-byte and larger block sizes in 4-bit
	 * bus width mode.
	 */
	mmc_data->flags |= TMIO_MMC_BLKSZ_2BYTES;

	/*
	 * All SDHI blocks support SDIO IRQ signalling.
	 */
	mmc_data->flags |= TMIO_MMC_SDIO_IRQ;

	/*
	 * All SDHI have CMD12 controll bit
	 */
	mmc_data->flags |= TMIO_MMC_HAVE_CMD12_CTRL;

	/*
	 * All SDHI need SDIO_INFO1 reserved bit
	 */
	mmc_data->flags |= TMIO_MMC_SDIO_STATUS_QUIRK;

	if (of_id && of_id->data) {
		const struct sh_mobile_sdhi_of_data *of_data = of_id->data;
		const struct sh_mobile_sdhi_scc *taps = of_data->taps;
		mmc_data->flags |= of_data->tmio_flags;
		mmc_data->capabilities |= of_data->capabilities;
		mmc_data->capabilities2 |= of_data->capabilities2;
		mmc_data->dma_rx_offset = of_data->dma_rx_offset;
		mmc_data->max_blk_count	= of_data->max_blk_count;
		mmc_data->max_segs = of_data->max_segs;
		dma_priv->dma_buswidth = of_data->dma_buswidth;
		dma_priv->sdbuf_64bit = of_data->sdbuf_64bit;
		if (np && !of_property_read_u32(np, "renesas,mmc-scc-tappos",
						&tappos)) {
			host->scc_tappos = tappos;
		} else {
			for (i = 0, taps = of_data->taps;
			     i < of_data->taps_num; i++, taps++) {
				if (taps->clk == 0 || taps->clk == clk_rate) {
					host->scc_tappos = taps->tap;
					break;
				}
			}
			if (taps->clk != 0 && taps->clk != clk_rate)
				dev_warn(&host->pdev->dev, "Unknown clock rate for SDR104 and HS200\n");
		}
	}

	if (of_find_property(np, "sd-uhs-sdr50", NULL))
		mmc_data->capabilities |= MMC_CAP_UHS_SDR50;
	if (of_find_property(np, "sd-uhs-sdr104", NULL))
		mmc_data->capabilities |= MMC_CAP_UHS_SDR104;

	if (mmc_data->capabilities & MMC_CAP_UHS_SDR104) {
		mmc_data->capabilities |= MMC_CAP_HW_RESET;
		mmc_data->flags |= TMIO_MMC_HAS_UHS_SCC;
	}

	ret = tmio_mmc_host_probe(host, mmc_data);
	if (ret < 0)
		goto efree;

	/*
	 * Allow one or more specific (named) ISRs or
	 * one or more multiplexed (un-named) ISRs.
	 */

	irq = platform_get_irq_byname(pdev, SH_MOBILE_SDHI_IRQ_CARD_DETECT);
	if (irq >= 0) {
		multiplexed_isr = false;
		ret = devm_request_irq(&pdev->dev, irq, tmio_mmc_card_detect_irq, 0,
				  dev_name(&pdev->dev), host);
		if (ret)
			goto eirq;
	}

	irq = platform_get_irq_byname(pdev, SH_MOBILE_SDHI_IRQ_SDIO);
	if (irq >= 0) {
		multiplexed_isr = false;
		ret = devm_request_irq(&pdev->dev, irq, tmio_mmc_sdio_irq, 0,
				  dev_name(&pdev->dev), host);
		if (ret)
			goto eirq;
	}

	irq = platform_get_irq_byname(pdev, SH_MOBILE_SDHI_IRQ_SDCARD);
	if (irq >= 0) {
		multiplexed_isr = false;
		ret = devm_request_irq(&pdev->dev, irq, tmio_mmc_sdcard_irq, 0,
				  dev_name(&pdev->dev), host);
		if (ret)
			goto eirq;
	} else if (!multiplexed_isr) {
		dev_err(&pdev->dev,
			"Principal SD-card IRQ is missing among named interrupts\n");
		ret = irq;
		goto eirq;
	}

	if (multiplexed_isr) {
		i = 0;
		while (1) {
			irq = platform_get_irq(pdev, i);
			if (irq < 0)
				break;
			i++;
			ret = devm_request_irq(&pdev->dev, irq, tmio_mmc_irq, 0,
					  dev_name(&pdev->dev), host);
			if (ret)
				goto eirq;
		}

		/* There must be at least one IRQ source */
		if (!i) {
			ret = irq;
			goto eirq;
		}
	}

	dev_info(&pdev->dev, "%s base at 0x%08lx clock rate %u MHz\n",
		 mmc_hostname(host->mmc), (unsigned long)
		 (platform_get_resource(pdev, IORESOURCE_MEM, 0)->start),
		 host->mmc->f_max / 1000000);

	return ret;

eirq:
	tmio_mmc_host_remove(host);
efree:
	tmio_mmc_host_free(host);
eprobe:
	return ret;
}

static int sh_mobile_sdhi_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct tmio_mmc_host *host = mmc_priv(mmc);

	tmio_mmc_host_remove(host);

	return 0;
}

static const struct dev_pm_ops tmio_mmc_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(tmio_mmc_host_suspend,
			tmio_mmc_host_resume)
	SET_RUNTIME_PM_OPS(tmio_mmc_host_runtime_suspend,
			tmio_mmc_host_runtime_resume,
			NULL)
};

static struct platform_driver sh_mobile_sdhi_driver = {
	.driver		= {
		.name	= "sh_mobile_sdhi",
		.pm	= &tmio_mmc_dev_pm_ops,
		.of_match_table = sh_mobile_sdhi_of_match,
	},
	.probe		= sh_mobile_sdhi_probe,
	.remove		= sh_mobile_sdhi_remove,
};

module_platform_driver(sh_mobile_sdhi_driver);

MODULE_DESCRIPTION("SuperH Mobile SDHI driver");
MODULE_AUTHOR("Magnus Damm");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:sh_mobile_sdhi");

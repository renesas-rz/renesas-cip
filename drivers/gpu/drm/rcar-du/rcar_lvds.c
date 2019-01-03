// SPDX-License-Identifier: GPL-2.0
/*
 * rcar_lvds.c  --  R-Car LVDS Encoder
 *
 * Copyright (C) 2013-2018 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_panel.h>

#include "rcar_lvds.h"
#include "rcar_lvds_regs.h"

struct rcar_lvds;
static struct rcar_lvds *g_lvds[RCAR_LVDS_MAX_NUM];

/* Keep in sync with the LVDCR0.LVMD hardware register values. */
enum rcar_lvds_mode {
	RCAR_LVDS_MODE_JEIDA = 0,
	RCAR_LVDS_MODE_MIRROR = 1,
	RCAR_LVDS_MODE_VESA = 4,
};

enum rcar_lvds_link_mode {
	RCAR_LVDS_SINGLE = 0,
	RCAR_LVDS_DUAL,
};

/* LVDS lanes 1 and 3 inverted */
#define RCAR_LVDS_QUIRK_LANES		BIT(0)
/* LVEN bit needs to be set on R8A77970/R8A7799x */
#define RCAR_LVDS_QUIRK_GEN3_LVEN	BIT(1)
/* PWD bit available (all of Gen3 but E3) */
#define RCAR_LVDS_QUIRK_PWD		BIT(2)
/* Has extended PLL */
#define RCAR_LVDS_QUIRK_EXT_PLL		BIT(3)
/* Supports dual-link operation */
#define RCAR_LVDS_QUIRK_DUAL_LINK	BIT(4)

struct rcar_lvds_device_info {
	unsigned int gen;
	unsigned int quirks;
	void (*pll_setup)(struct rcar_lvds *lvds, unsigned int freq);
};

struct rcar_lvds {
	struct device *dev;
	const struct rcar_lvds_device_info *info;
	struct reset_control *rstc;

	struct drm_bridge bridge;

	struct drm_bridge *next_bridge;
	struct drm_connector connector;
	struct drm_panel *panel;

	void __iomem *mmio;
	struct {
		struct clk *mod;		/* CPG module clock */
		struct clk *extal;		/* External clock */
		struct clk *dotclkin[2];	/* External DU clocks */
	} clocks;
	bool enabled;

	struct drm_display_mode display_mode;
	enum rcar_lvds_mode mode;
	enum rcar_lvds_link_mode link_mode;
	u32 id;
};

#define bridge_to_rcar_lvds(bridge) \
	container_of(bridge, struct rcar_lvds, bridge)

#define connector_to_rcar_lvds(connector) \
	container_of(connector, struct rcar_lvds, connector)

static void rcar_lvds_write(struct rcar_lvds *lvds, u32 reg, u32 data)
{
	iowrite32(data, lvds->mmio + reg);
}

static u32 rcar_lvds_read(struct rcar_lvds *lvds, u32 reg)
{
	return ioread32(lvds->mmio + reg);
}

/* -----------------------------------------------------------------------------
 * Connector & Panel
 */

static int rcar_lvds_connector_get_modes(struct drm_connector *connector)
{
	struct rcar_lvds *lvds = connector_to_rcar_lvds(connector);

	return drm_panel_get_modes(lvds->panel);
}

static int rcar_lvds_connector_atomic_check(struct drm_connector *connector,
					    struct drm_connector_state *state)
{
	struct rcar_lvds *lvds = connector_to_rcar_lvds(connector);
	const struct drm_display_mode *panel_mode;
	struct drm_crtc_state *crtc_state;

	if (!state->crtc)
		return 0;

	if (list_empty(&connector->modes)) {
		dev_dbg(lvds->dev, "connector: empty modes list\n");
		return -EINVAL;
	}

	panel_mode = list_first_entry(&connector->modes,
				      struct drm_display_mode, head);

	/* We're not allowed to modify the resolution. */
	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	if (crtc_state->mode.hdisplay != panel_mode->hdisplay ||
	    crtc_state->mode.vdisplay != panel_mode->vdisplay)
		return -EINVAL;

	/* The flat panel mode is fixed, just copy it to the adjusted mode. */
	drm_mode_copy(&crtc_state->adjusted_mode, panel_mode);

	return 0;
}

static const struct drm_connector_helper_funcs rcar_lvds_conn_helper_funcs = {
	.get_modes = rcar_lvds_connector_get_modes,
	.atomic_check = rcar_lvds_connector_atomic_check,
};

static const struct drm_connector_funcs rcar_lvds_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* -----------------------------------------------------------------------------
 * PLL Setup
 */

static void rcar_lvds_pll_setup_gen2(struct rcar_lvds *lvds, unsigned int freq)
{
	u32 val;

	if (freq < 39000000)
		val = LVDPLLCR_CEEN | LVDPLLCR_COSEL | LVDPLLCR_PLLDLYCNT_38M;
	else if (freq < 61000000)
		val = LVDPLLCR_CEEN | LVDPLLCR_COSEL | LVDPLLCR_PLLDLYCNT_60M;
	else if (freq < 121000000)
		val = LVDPLLCR_CEEN | LVDPLLCR_COSEL | LVDPLLCR_PLLDLYCNT_121M;
	else
		val = LVDPLLCR_PLLDLYCNT_150M;

	rcar_lvds_write(lvds, LVDPLLCR, val);
}

static void rcar_lvds_pll_setup_gen3(struct rcar_lvds *lvds, unsigned int freq)
{
	u32 val;

	if (freq < 42000000)
		val = LVDPLLCR_PLLDIVCNT_42M;
	else if (freq < 85000000)
		val = LVDPLLCR_PLLDIVCNT_85M;
	else if (freq < 128000000)
		val = LVDPLLCR_PLLDIVCNT_128M;
	else
		val = LVDPLLCR_PLLDIVCNT_148M;

	rcar_lvds_write(lvds, LVDPLLCR, val);
}

struct pll_info {
	struct clk *clk;
	unsigned long diff;
	unsigned int pll_m;
	unsigned int pll_n;
	unsigned int pll_e;
	unsigned int div;
};

static void rcar_lvds_d3_e3_pll_calc(struct rcar_lvds *lvds, struct clk *clk,
				     unsigned long target, struct pll_info *pll)
{
	unsigned long fin;
	unsigned int m_min;
	unsigned int m_max;
	unsigned int m;

	if (!clk)
		return;

	/*
	 * The LVDS PLL is made of a pre-divider and a multiplier (strangerly
	 * enough called M and N respectively), followed by a post-divider E.
	 *
	 *         ,-----.         ,-----.     ,-----.         ,-----.
	 * Fin --> | 1/M | -Fpdf-> | PFD | --> | VCO | -Fvco-> | 1/E | --> Fout
	 *         `-----'     ,-> |     |     `-----'   |     `-----'
	 *                     |   `-----'               |
	 *                     |         ,-----.         |
	 *                     `-------- | 1/N | <-------'
	 *                               `-----'
	 *
	 * The clock output by the PLL is then further divided by a programmable
	 * divider DIV to achieve the desired target frequency. Finally, an
	 * optional fixed /7 divider is used to convert the bit clock to a pixel
	 * clock (as LVDS transmits 7 bits per lane per clock sample).
	 *
	 *          ,-------.     ,-----.     |\
	 * Fout --> | 1/DIV | --> | 1/7 | --> | |
	 *          `-------'  |  `-----'     | | --> dot clock
	 *                     `------------> | |
	 *                                    |/
	 *
	 * The /7 divider is optional when the LVDS PLL is used to generate a
	 * dot clock for the DU RGB output, without using the LVDS encoder. We
	 * don't support this configuration yet.
	 *
	 * The PLL allowed input frequency range is 12 MHz to 192 MHz.
	 */

	fin = clk_get_rate(clk);
	if (fin < 12000000 || fin > 192000000)
		return;

	/*
	 * The comparison frequency range is 12 MHz to 24 MHz, which limits the
	 * allowed values for the pre-divider M (normal range 1-8).
	 *
	 * Fpfd = Fin / M
	 */
	m_min = max_t(unsigned int, 1, DIV_ROUND_UP(fin, 24000000));
	m_max = min_t(unsigned int, 8, fin / 12000000);

	for (m = m_min; m <= m_max; ++m) {
		unsigned long fpfd;
		unsigned int n_min;
		unsigned int n_max;
		unsigned int n;

		/*
		 * The VCO operating range is 900 Mhz to 1800 MHz, which limits
		 * the allowed values for the multiplier N (normal range
		 * 60-120).
		 *
		 * Fvco = Fin * N / M
		 */
		fpfd = fin / m;
		n_min = max_t(unsigned int, 60, DIV_ROUND_UP(900000000, fpfd));
		n_max = min_t(unsigned int, 120, 1800000000 / fpfd);

		for (n = n_min; n < n_max; ++n) {
			unsigned long fvco;
			unsigned int e_min;
			unsigned int e;

			/*
			 * The output frequency is limited to 1039.5 MHz,
			 * limiting again the allowed values for the
			 * post-divider E (normal value 1, 2 or 4).
			 *
			 * Fout = Fvco / E
			 */
			fvco = fpfd * n;
			e_min = fvco > 1039500000 ? 1 : 0;

			for (e = e_min; e < 3; ++e) {
				unsigned long fout;
				unsigned long diff;
				unsigned int div;

				/*
				 * Finally we have a programable divider after
				 * the PLL, followed by a an optional fixed /7
				 * divider.
				 */
				fout = fvco / (1 << e) / 7;
				div = DIV_ROUND_CLOSEST(fout, target);
				diff = abs(fout / div - target);

				if (diff < pll->diff) {
					pll->clk = clk;
					pll->diff = diff;
					pll->pll_m = m;
					pll->pll_n = n;
					pll->pll_e = e;
					pll->div = div;

					if (diff == 0)
						goto done;
				}
			}
		}
	}

done:
#if defined(CONFIG_DEBUG) || defined(CONFIG_DYNAMIC_DEBUG)
	{
		unsigned long output = fin * pll->pll_n / pll->pll_m
				     / (1 << pll->pll_e) / 7 / pll->div;
		int error = (long)(output - target) * 10000 / (long)target;

		dev_dbg(lvds->dev,
			"%pC %lu Hz -> Fout %lu Hz (target %lu Hz, error %d.%02u%%), PLL M/N/E/DIV %u/%u/%u/%u\n",
			clk, fin, output, target, error / 100,
			error < 0 ? -error % 100 : error % 100,
			pll->pll_m, pll->pll_n, pll->pll_e, pll->div);
	}
#endif

	return;
}

static void rcar_lvds_pll_setup_d3_e3(struct rcar_lvds *lvds, unsigned int freq)
{
	struct drm_crtc *crtc = lvds->bridge.encoder->crtc;
	struct pll_info pll = { .diff = (unsigned long)-1 };
	u32 lvdpllcr;

	if (lvds->clocks.dotclkin[0] || lvds->clocks.dotclkin[1]) {
		rcar_lvds_d3_e3_pll_calc(lvds, lvds->clocks.dotclkin[0],
					 freq, &pll);
		rcar_lvds_d3_e3_pll_calc(lvds, lvds->clocks.dotclkin[1],
					 freq, &pll);
	} else if (lvds->clocks.extal) {
		rcar_lvds_d3_e3_pll_calc(lvds, lvds->clocks.extal,
					 freq, &pll);
	}

	lvdpllcr = LVDPLLCR_PLLON | LVDPLLCR_CLKOUT
		 | LVDPLLCR_PLLN(pll.pll_n - 1) | LVDPLLCR_PLLM(pll.pll_m - 1);

	if (pll.clk == lvds->clocks.extal)
		lvdpllcr |= LVDPLLCR_CKSEL_EXTAL;
	else
		lvdpllcr |= LVDPLLCR_CKSEL_DU_DOTCLKIN(drm_crtc_index(crtc));

	if (pll.pll_e > 0)
		lvdpllcr |= LVDPLLCR_STP_CLKOUTE | LVDPLLCR_OUTCLKSEL
			 |  LVDPLLCR_PLLE(pll.pll_e - 1);

	/* Wait 200us until pll-lock */
	usleep_range(200, 250);

	rcar_lvds_write(lvds, LVDPLLCR, lvdpllcr);

	if (pll.div > 1)
		rcar_lvds_write(lvds, LVDDIV, LVDDIV_DIVSEL |
				LVDDIV_DIVRESET | LVDDIV_DIV(pll.div - 1));
	else
		rcar_lvds_write(lvds, LVDDIV, 0);
}

static void rcar_lvds_dual_mode(struct rcar_lvds *lvds0,
				struct rcar_lvds *lvds1)
{
	u32 lvdcr0 = 0, lvdcr1 = 0, lvdhcr;
	u32 lvdcr0_lvres, lvdcr1_lvres;

	lvdcr0_lvres = rcar_lvds_read(lvds0, LVDCR0) & LVDCR0_LVRES;
	lvdcr1_lvres = rcar_lvds_read(lvds1, LVDCR0) & LVDCR0_LVRES;

	if (lvdcr0_lvres && lvdcr1_lvres)
		return;

	lvdhcr = LVDCHCR_CHSEL_CH(0, 0) | LVDCHCR_CHSEL_CH(1, 1) |
		 LVDCHCR_CHSEL_CH(2, 2) | LVDCHCR_CHSEL_CH(3, 3);

	rcar_lvds_write(lvds0, LVDCTRCR, LVDCTRCR_CTR3SEL_ZERO |
			LVDCTRCR_CTR2SEL_DISP | LVDCTRCR_CTR1SEL_VSYNC |
			LVDCTRCR_CTR0SEL_HSYNC);
	rcar_lvds_write(lvds0, LVDCHCR, lvdhcr);
	rcar_lvds_write(lvds0, LVDSTRIPE, LVDSTRIPE_ST_ON);

	rcar_lvds_write(lvds1, LVDCTRCR, LVDCTRCR_CTR3SEL_ZERO |
			LVDCTRCR_CTR2SEL_DISP | LVDCTRCR_CTR1SEL_VSYNC |
			LVDCTRCR_CTR0SEL_HSYNC);
	rcar_lvds_write(lvds1, LVDCHCR, lvdhcr);
	rcar_lvds_write(lvds1, LVDSTRIPE, LVDSTRIPE_ST_ON);

	/* Turn all the channels on. */
	rcar_lvds_write(lvds0, LVDCR1,
			LVDCR1_CHSTBY(3) | LVDCR1_CHSTBY(2) |
			LVDCR1_CHSTBY(1) | LVDCR1_CHSTBY(0) | LVDCR1_CLKSTBY);
	rcar_lvds_write(lvds1, LVDCR1,
			LVDCR1_CHSTBY(3) | LVDCR1_CHSTBY(2) |
			LVDCR1_CHSTBY(1) | LVDCR1_CHSTBY(0) | LVDCR1_CLKSTBY);

	/*
	 * Turn the PLL on, set it to LVDS normal mode, wait for the startup
	 * delay and turn the output on.
	 */
	if ((lvds0->info->quirks & RCAR_LVDS_QUIRK_PWD) ||
	    (lvds1->info->quirks & RCAR_LVDS_QUIRK_PWD)) {
		lvdcr0 |= LVDCR0_PWD;
		rcar_lvds_write(lvds0, LVDCR0, lvdcr0);

		lvdcr1 |= LVDCR0_PWD;
		rcar_lvds_write(lvds1, LVDCR0, lvdcr1);

		lvdcr1 |= LVDCR0_LVEN | LVDCR0_LVRES;
		rcar_lvds_write(lvds1, LVDCR0, lvdcr1);

		lvdcr0 |= LVDCR0_LVEN | LVDCR0_LVRES;
		rcar_lvds_write(lvds0, LVDCR0, lvdcr0);

		return;
	}

	lvdcr0 |= LVDCR0_LVEN;
	rcar_lvds_write(lvds0, LVDCR0, lvdcr0);

	lvdcr1 |= LVDCR0_LVEN;
	rcar_lvds_write(lvds1, LVDCR0, lvdcr1);

	lvdcr1 |= LVDCR0_LVRES;
	rcar_lvds_write(lvds1, LVDCR0, lvdcr1);

	lvdcr0 |= LVDCR0_LVRES;
	rcar_lvds_write(lvds0, LVDCR0, lvdcr0);
}

/* -----------------------------------------------------------------------------
 * Bridge
 */

static void rcar_lvds_enable(struct drm_bridge *bridge)
{
	struct rcar_lvds *lvds = bridge_to_rcar_lvds(bridge);
	const struct drm_display_mode *mode = &lvds->display_mode;
	/*
	 * FIXME: We should really retrieve the CRTC through the state, but how
	 * do we get a state pointer?
	 */
	struct drm_crtc *crtc = lvds->bridge.encoder->crtc;
	u32 lvdhcr;
	u32 lvdcr0;
	int ret;

	WARN_ON(lvds->enabled);

	if ((lvds->info->quirks & RCAR_LVDS_QUIRK_DUAL_LINK) &&
	    lvds->link_mode == RCAR_LVDS_DUAL) {
		struct rcar_lvds *lvds0;
		struct rcar_lvds *lvds1;

		if (!g_lvds[0] || !g_lvds[1])
			return;

		lvds0 = g_lvds[0];
		lvds1 = g_lvds[1];

		rcar_lvds_dual_mode(lvds0, lvds1);

		goto dual_link;
	}

	if (!(lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL)) {
		reset_control_deassert(lvds->rstc);
		ret = clk_prepare_enable(lvds->clocks.mod);
		if (ret < 0)
			return;
	}

	/*
	 * Hardcode the channels and control signals routing for now.
	 *
	 * HSYNC -> CTRL0
	 * VSYNC -> CTRL1
	 * DISP  -> CTRL2
	 * 0     -> CTRL3
	 */
	rcar_lvds_write(lvds, LVDCTRCR, LVDCTRCR_CTR3SEL_ZERO |
			LVDCTRCR_CTR2SEL_DISP | LVDCTRCR_CTR1SEL_VSYNC |
			LVDCTRCR_CTR0SEL_HSYNC);

	if (lvds->info->quirks & RCAR_LVDS_QUIRK_LANES)
		lvdhcr = LVDCHCR_CHSEL_CH(0, 0) | LVDCHCR_CHSEL_CH(1, 3)
		       | LVDCHCR_CHSEL_CH(2, 2) | LVDCHCR_CHSEL_CH(3, 1);
	else
		lvdhcr = LVDCHCR_CHSEL_CH(0, 0) | LVDCHCR_CHSEL_CH(1, 1)
		       | LVDCHCR_CHSEL_CH(2, 2) | LVDCHCR_CHSEL_CH(3, 3);

	rcar_lvds_write(lvds, LVDCHCR, lvdhcr);

	/* PLL clock configuration. */
	if (lvds->info->pll_setup)
		lvds->info->pll_setup(lvds, mode->clock * 1000);

	/* Set the LVDS mode and select the input. */
	lvdcr0 = lvds->mode << LVDCR0_LVMD_SHIFT;
	if (drm_crtc_index(crtc) == 2)
		lvdcr0 |= LVDCR0_DUSEL;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

	/* Turn all the channels on. */
	rcar_lvds_write(lvds, LVDCR1,
			LVDCR1_CHSTBY(3) | LVDCR1_CHSTBY(2) |
			LVDCR1_CHSTBY(1) | LVDCR1_CHSTBY(0) | LVDCR1_CLKSTBY);

	if (lvds->info->gen < 3) {
		/* Enable LVDS operation and turn the bias circuitry on. */
		lvdcr0 |= LVDCR0_BEN | LVDCR0_LVEN;
		rcar_lvds_write(lvds, LVDCR0, lvdcr0);
	}

	if (!(lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL)) {
		/*
		 * Turn the PLL on (simple PLL only, extended PLL is fully
		 * controlled through LVDPLLCR).
		 */
		lvdcr0 |= LVDCR0_PLLON;
		rcar_lvds_write(lvds, LVDCR0, lvdcr0);
	}

	if (lvds->info->quirks & RCAR_LVDS_QUIRK_PWD) {
		/* Set LVDS normal mode. */
		lvdcr0 |= LVDCR0_PWD;
		rcar_lvds_write(lvds, LVDCR0, lvdcr0);
	}

	if (lvds->info->quirks & RCAR_LVDS_QUIRK_GEN3_LVEN) {
		/* Turn on the LVDS PHY. */
		lvdcr0 |= LVDCR0_LVEN;
		rcar_lvds_write(lvds, LVDCR0, lvdcr0);
	}

	if (!(lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL)) {
		/* Wait for the PLL startup delay (simple PLL only). */
		usleep_range(100, 150);
	}

	/* Turn the output on. */
	lvdcr0 |= LVDCR0_LVRES;
	rcar_lvds_write(lvds, LVDCR0, lvdcr0);

dual_link:
	if (lvds->panel) {
		drm_panel_prepare(lvds->panel);
		drm_panel_enable(lvds->panel);
	}

	lvds->enabled = true;
}

static void __rcar_lvds_disable(struct drm_bridge *bridge)
{
	struct rcar_lvds *lvds = bridge_to_rcar_lvds(bridge);
	u32 lvdcr0 = 0;

	WARN_ON(!lvds->enabled);

	if (lvds->panel) {
		drm_panel_disable(lvds->panel);
		drm_panel_unprepare(lvds->panel);
	}

	if (lvds->info->quirks & RCAR_LVDS_QUIRK_DUAL_LINK &&
	    lvds->link_mode == RCAR_LVDS_DUAL) {
		struct rcar_lvds *lvds_pair;
		struct rcar_lvds *lvds0;
		struct rcar_lvds *lvds1;
		u32 id;

		if (!g_lvds[0] || !g_lvds[1])
			return;

		lvds0 = g_lvds[0];
		lvds1 = g_lvds[1];

		id = lvds->id == 0 ? 1 : 0;
		lvds_pair = g_lvds[id];

		if (!lvds_pair->enabled) {
			u32 lvdcr0 = 0, lvdcr1 = 0;

			lvdcr0 = rcar_lvds_read(lvds0, LVDCR0) & ~LVDCR0_LVRES;
			rcar_lvds_write(lvds0, LVDCR0, lvdcr0);
			lvdcr1 = rcar_lvds_read(lvds1, LVDCR0) & ~LVDCR0_LVRES;
			rcar_lvds_write(lvds1, LVDCR0, lvdcr1);

			lvdcr0 = rcar_lvds_read(lvds0, LVDCR0) & ~LVDCR0_LVEN;
			rcar_lvds_write(lvds0, LVDCR0, lvdcr0);
			lvdcr1 = rcar_lvds_read(lvds1, LVDCR0) & ~LVDCR0_LVEN;
			rcar_lvds_write(lvds1, LVDCR0, lvdcr1);

			if (lvds->info->quirks & RCAR_LVDS_QUIRK_PWD) {
				lvdcr0 = rcar_lvds_read(lvds0, LVDCR0)
							& ~LVDCR0_PWD;
				rcar_lvds_write(lvds0, LVDCR0, lvdcr0);
				lvdcr1 = rcar_lvds_read(lvds1, LVDCR0)
							& ~LVDCR0_PWD;
				rcar_lvds_write(lvds1, LVDCR0, lvdcr1);
			}
			rcar_lvds_write(lvds0, LVDCR1, 0);
			rcar_lvds_write(lvds1, LVDCR1, 0);
			rcar_lvds_write(lvds0, LVDPLLCR, 0);
			rcar_lvds_write(lvds1, LVDPLLCR, 0);

			clk_disable_unprepare(lvds0->clocks.mod);
			clk_disable_unprepare(lvds1->clocks.mod);
			reset_control_assert(lvds0->rstc);
			reset_control_assert(lvds1->rstc);
		}
	} else {
		lvdcr0 = rcar_lvds_read(lvds, LVDCR0) & ~LVDCR0_LVRES;
		rcar_lvds_write(lvds, LVDCR0, lvdcr0);

		if (lvds->info->quirks & RCAR_LVDS_QUIRK_GEN3_LVEN) {
			lvdcr0 = rcar_lvds_read(lvds, LVDCR0) & ~LVDCR0_LVEN;
			rcar_lvds_write(lvds, LVDCR0, lvdcr0);
		}

		if (lvds->info->quirks & RCAR_LVDS_QUIRK_PWD) {
			lvdcr0 = rcar_lvds_read(lvds, LVDCR0) & ~LVDCR0_PWD;
			rcar_lvds_write(lvds, LVDCR0, lvdcr0);
		}

		if (!(lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL)) {
			lvdcr0 = rcar_lvds_read(lvds, LVDCR0) & ~LVDCR0_PLLON;
			rcar_lvds_write(lvds, LVDCR0, lvdcr0);
		}

		rcar_lvds_write(lvds, LVDCR1, 0);
		rcar_lvds_write(lvds, LVDPLLCR, 0);

		clk_disable_unprepare(lvds->clocks.mod);
		reset_control_assert(lvds->rstc);
	}

	lvds->enabled = false;
}

static void rcar_lvds_disable(struct drm_bridge *bridge)
{
	struct rcar_lvds *lvds = bridge_to_rcar_lvds(bridge);

	if (lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL)
		return;

	__rcar_lvds_disable(bridge);
}

static bool rcar_lvds_mode_fixup(struct drm_bridge *bridge,
				 const struct drm_display_mode *mode,
				 struct drm_display_mode *adjusted_mode)
{
	struct rcar_lvds *lvds = bridge_to_rcar_lvds(bridge);

	/*
	 * The internal LVDS encoder has a restricted clock frequency operating
	 * range (31MHz to 148.5MHz). In case of r8a77990/r8a77995, frequency
	 * operating range (5MHz to 148.5MHz). Clamp the clock accordingly.
	 */
	if (lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL)
		adjusted_mode->clock = clamp(adjusted_mode->clock,
					     5000, 148500);
	else
		adjusted_mode->clock = clamp(adjusted_mode->clock,
					     31000, 148500);

	return true;
}

static void rcar_lvds_get_lvds_mode(struct rcar_lvds *lvds)
{
	struct drm_display_info *info = &lvds->connector.display_info;
	enum rcar_lvds_mode mode;

	/*
	 * There is no API yet to retrieve LVDS mode from a bridge, only panels
	 * are supported.
	 */
	if (!lvds->panel)
		return;

	if (!info->num_bus_formats || !info->bus_formats) {
		dev_err(lvds->dev, "no LVDS bus format reported\n");
		return;
	}

	switch (info->bus_formats[0]) {
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		mode = RCAR_LVDS_MODE_JEIDA;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
		mode = RCAR_LVDS_MODE_VESA;
		break;
	default:
		dev_err(lvds->dev, "unsupported LVDS bus format 0x%04x\n",
			info->bus_formats[0]);
		return;
	}

	if (info->bus_flags & DRM_BUS_FLAG_DATA_LSB_TO_MSB)
		mode |= RCAR_LVDS_MODE_MIRROR;

	lvds->mode = mode;
}

static void rcar_lvds_mode_set(struct drm_bridge *bridge,
			       struct drm_display_mode *mode,
			       struct drm_display_mode *adjusted_mode)
{
	struct rcar_lvds *lvds = bridge_to_rcar_lvds(bridge);

	WARN_ON(lvds->enabled);

	lvds->display_mode = *adjusted_mode;

	rcar_lvds_get_lvds_mode(lvds);
}

static int rcar_lvds_attach(struct drm_bridge *bridge)
{
	struct rcar_lvds *lvds = bridge_to_rcar_lvds(bridge);
	struct drm_connector *connector = &lvds->connector;
	struct drm_encoder *encoder = bridge->encoder;
	int ret;

	/* If we have a next bridge just attach it. */
	if (lvds->next_bridge)
		return drm_bridge_attach(bridge->encoder, lvds->next_bridge,
					 bridge);

	/* Otherwise we have a panel, create a connector. */
	ret = drm_connector_init(bridge->dev, connector, &rcar_lvds_conn_funcs,
				 DRM_MODE_CONNECTOR_LVDS);
	if (ret < 0)
		return ret;

	drm_connector_helper_add(connector, &rcar_lvds_conn_helper_funcs);

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret < 0)
		return ret;

	return drm_panel_attach(lvds->panel, connector);
}

static void rcar_lvds_detach(struct drm_bridge *bridge)
{
	struct rcar_lvds *lvds = bridge_to_rcar_lvds(bridge);

	if (lvds->panel)
		drm_panel_detach(lvds->panel);
}

static const struct drm_bridge_funcs rcar_lvds_bridge_ops = {
	.attach = rcar_lvds_attach,
	.detach = rcar_lvds_detach,
	.enable = rcar_lvds_enable,
	.disable = rcar_lvds_disable,
	.mode_fixup = rcar_lvds_mode_fixup,
	.mode_set = rcar_lvds_mode_set,
};

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static int rcar_lvds_parse_dt(struct rcar_lvds *lvds)
{
	struct device_node *local_output = NULL;
	struct device_node *remote_input = NULL;
	struct device_node *remote = NULL;
	struct device_node *node;
	bool is_bridge = false;
	int ret = 0;
	u32 id;
	const char *str;

	local_output = of_graph_get_endpoint_by_regs(lvds->dev->of_node, 1, 0);
	if (!local_output) {
		dev_dbg(lvds->dev, "unconnected port@1\n");
		return -ENODEV;
	}

	/*
	 * Locate the connected entity and infer its type from the number of
	 * endpoints.
	 */
	remote = of_graph_get_remote_port_parent(local_output);
	if (!remote) {
		dev_dbg(lvds->dev, "unconnected endpoint %pOF\n", local_output);
		ret = -ENODEV;
		goto done;
	}

	if (!of_device_is_available(remote)) {
		dev_dbg(lvds->dev, "connected entity %pOF is disabled\n",
			remote);
		ret = -ENODEV;
		goto done;
	}

	remote_input = of_graph_get_remote_endpoint(local_output);

	for_each_endpoint_of_node(remote, node) {
		if (node != remote_input) {
			/*
			 * We've found one endpoint other than the input, this
			 * must be a bridge.
			 */
			is_bridge = true;
			of_node_put(node);
			break;
		}
	}

	if (is_bridge) {
		lvds->next_bridge = of_drm_find_bridge(remote);
		if (!lvds->next_bridge)
			ret = -EPROBE_DEFER;
	} else {
		lvds->panel = of_drm_find_panel(remote);
		if (!lvds->panel)
			ret = -EPROBE_DEFER;
	}

	/* Make sure LVDS id is present and sane */
	if (!of_property_read_u32(lvds->dev->of_node, "renesas,id", &id))
		lvds->id = id;
	else
		lvds->id = 0;

	if (!of_property_read_string(lvds->dev->of_node, "mode", &str)) {
		if (!strcmp(str, "dual-link"))
			lvds->link_mode = RCAR_LVDS_DUAL;
		else
			lvds->link_mode = RCAR_LVDS_SINGLE;
	} else {
		lvds->link_mode = RCAR_LVDS_SINGLE;
	}

done:
	of_node_put(local_output);
	of_node_put(remote_input);
	of_node_put(remote);

	return ret;
}

int rcar_lvds_pll_round_rate(u32 index, unsigned long rate)
{
	struct rcar_lvds *lvds;
	int ret;

	if (index >= RCAR_LVDS_MAX_NUM)
		return 0;

	lvds = g_lvds[index];

	if (!(lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL))
		return 0;

	if (rate == 0) {
		__rcar_lvds_disable(&lvds->bridge);
	} else {
		if ((lvds->info->quirks & RCAR_LVDS_QUIRK_DUAL_LINK) &&
		    lvds->link_mode == RCAR_LVDS_DUAL) {
			bool enable;
			struct rcar_lvds *lvds0;
			struct rcar_lvds *lvds1;

			if (!g_lvds[0] || !g_lvds[1])
				return 0;

			lvds0 = g_lvds[0];
			lvds1 = g_lvds[1];

			enable = __clk_is_enabled(lvds->clocks.mod);
			if (enable)
				goto skip;

			reset_control_deassert(lvds0->rstc);
			reset_control_deassert(lvds1->rstc);

			ret = clk_prepare_enable(lvds0->clocks.mod);
			if (ret < 0)
				return ret;

			ret = clk_prepare_enable(lvds1->clocks.mod);
			if (ret < 0)
				return ret;
skip:
			rcar_lvds_pll_setup_d3_e3(lvds, rate);
		} else {
			reset_control_deassert(lvds->rstc);
			ret = clk_prepare_enable(lvds->clocks.mod);
			if (ret < 0)
				return ret;
			rcar_lvds_pll_setup_d3_e3(lvds, rate);
		}
	}

	return 0;
}
EXPORT_SYMBOL(rcar_lvds_pll_round_rate);

static struct clk *rcar_lvds_get_clock(struct rcar_lvds *lvds, const char *name,
				       bool optional)
{
	struct clk *clk;

	clk = devm_clk_get(lvds->dev, name);
	if (!IS_ERR(clk))
		return clk;

	if (PTR_ERR(clk) == -ENOENT && optional)
		return NULL;

	if (PTR_ERR(clk) != -EPROBE_DEFER)
		dev_err(lvds->dev, "failed to get %s clock\n",
			name ? name : "module");

	return clk;
}

static int rcar_lvds_get_clocks(struct rcar_lvds *lvds)
{
	lvds->clocks.mod = rcar_lvds_get_clock(lvds, NULL, false);
	if (IS_ERR(lvds->clocks.mod))
		return PTR_ERR(lvds->clocks.mod);

	/*
	 * LVDS encoders without an extended PLL have no external clock inputs.
	 */
	if (!(lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL))
		return 0;

	lvds->clocks.extal = rcar_lvds_get_clock(lvds, "extal", true);
	if (IS_ERR(lvds->clocks.extal))
		return PTR_ERR(lvds->clocks.extal);

	lvds->clocks.dotclkin[0] = rcar_lvds_get_clock(lvds, "dclkin.0", true);
	if (IS_ERR(lvds->clocks.dotclkin[0]))
		return PTR_ERR(lvds->clocks.dotclkin[0]);

	lvds->clocks.dotclkin[1] = rcar_lvds_get_clock(lvds, "dclkin.1", true);
	if (IS_ERR(lvds->clocks.dotclkin[1]))
		return PTR_ERR(lvds->clocks.dotclkin[1]);

	/* At least one input to the PLL must be available. */
	if (!lvds->clocks.extal && !lvds->clocks.dotclkin[0] &&
	    !lvds->clocks.dotclkin[1]) {
		dev_err(lvds->dev,
			"no input clock (extal, dclkin.0 or dclkin.1)\n");
		return -EINVAL;
	}

	return 0;
}

static int rcar_lvds_probe(struct platform_device *pdev)
{
	struct rcar_lvds *lvds;
	struct resource *mem;
	int ret;

	lvds = devm_kzalloc(&pdev->dev, sizeof(*lvds), GFP_KERNEL);
	if (lvds == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, lvds);

	lvds->dev = &pdev->dev;
	lvds->info = of_device_get_match_data(&pdev->dev);
	lvds->enabled = false;

	ret = rcar_lvds_parse_dt(lvds);
	if (ret < 0)
		return ret;

	lvds->bridge.driver_private = lvds;
	lvds->bridge.funcs = &rcar_lvds_bridge_ops;
	lvds->bridge.of_node = pdev->dev.of_node;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	lvds->mmio = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(lvds->mmio))
		return PTR_ERR(lvds->mmio);

	ret = rcar_lvds_get_clocks(lvds);
	if (ret < 0)
		return ret;

	lvds->rstc = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(lvds->rstc)) {
		dev_err(&pdev->dev, "failed to get cpg reset\n");
		return PTR_ERR(lvds->rstc);
	}

	drm_bridge_add(&lvds->bridge);

	if (!(lvds->info->quirks & RCAR_LVDS_QUIRK_EXT_PLL))
		return 0;

	g_lvds[lvds->id] = lvds;

	return 0;
}

static int rcar_lvds_remove(struct platform_device *pdev)
{
	struct rcar_lvds *lvds = platform_get_drvdata(pdev);

	drm_bridge_remove(&lvds->bridge);

	return 0;
}

static const struct rcar_lvds_device_info rcar_lvds_gen2_info = {
	.gen = 2,
	.pll_setup = rcar_lvds_pll_setup_gen2,
};

static const struct rcar_lvds_device_info rcar_lvds_r8a7790_info = {
	.gen = 2,
	.quirks = RCAR_LVDS_QUIRK_LANES,
	.pll_setup = rcar_lvds_pll_setup_gen2,
};

static const struct rcar_lvds_device_info rcar_lvds_gen3_info = {
	.gen = 3,
	.quirks = RCAR_LVDS_QUIRK_PWD,
	.pll_setup = rcar_lvds_pll_setup_gen3,
};

static const struct rcar_lvds_device_info rcar_lvds_r8a77970_info = {
	.gen = 3,
	.quirks = RCAR_LVDS_QUIRK_PWD | RCAR_LVDS_QUIRK_GEN3_LVEN,
	.pll_setup = rcar_lvds_pll_setup_gen2,
};

static const struct rcar_lvds_device_info rcar_lvds_r8a77990_info = {
	.gen = 3,
	.quirks = RCAR_LVDS_QUIRK_GEN3_LVEN | RCAR_LVDS_QUIRK_EXT_PLL
		| RCAR_LVDS_QUIRK_DUAL_LINK,
};

static const struct rcar_lvds_device_info rcar_lvds_r8a77995_info = {
	.gen = 3,
	.quirks = RCAR_LVDS_QUIRK_GEN3_LVEN | RCAR_LVDS_QUIRK_PWD
		| RCAR_LVDS_QUIRK_EXT_PLL | RCAR_LVDS_QUIRK_DUAL_LINK,
};

static const struct of_device_id rcar_lvds_of_table[] = {
	{ .compatible = "renesas,r8a7743-lvds", .data = &rcar_lvds_gen2_info },
	{ .compatible = "renesas,r8a7790-lvds", .data = &rcar_lvds_r8a7790_info },
	{ .compatible = "renesas,r8a7791-lvds", .data = &rcar_lvds_gen2_info },
	{ .compatible = "renesas,r8a7793-lvds", .data = &rcar_lvds_gen2_info },
	{ .compatible = "renesas,r8a7795-lvds", .data = &rcar_lvds_gen3_info },
	{ .compatible = "renesas,r8a7796-lvds", .data = &rcar_lvds_gen3_info },
	{ .compatible = "renesas,r8a77970-lvds", .data = &rcar_lvds_r8a77970_info },
	{ .compatible = "renesas,r8a77990-lvds",
		.data = &rcar_lvds_r8a77990_info },
	{ .compatible = "renesas,r8a77995-lvds",
		.data = &rcar_lvds_r8a77995_info },
	{ }
};

MODULE_DEVICE_TABLE(of, rcar_lvds_of_table);

static struct platform_driver rcar_lvds_platform_driver = {
	.probe		= rcar_lvds_probe,
	.remove		= rcar_lvds_remove,
	.driver		= {
		.name	= "rcar-lvds",
		.of_match_table = rcar_lvds_of_table,
	},
};

module_platform_driver(rcar_lvds_platform_driver);

MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("Renesas R-Car LVDS Encoder Driver");
MODULE_LICENSE("GPL");

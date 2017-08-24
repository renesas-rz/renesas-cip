/*
 * ov7725 Camera Driver
 *
 * Copyright (C) 2008 Renesas Solutions Corp.
 * Kuninori Morimoto <morimoto.kuninori@renesas.com>
 *
 * Based on ov7670 and soc_camera_platform driver,
 *
 * Copyright 2006-7 Jonathan Corbet <corbet@lwn.net>
 * Copyright (C) 2008 Magnus Damm
 * Copyright (C) 2008, Guennadi Liakhovetski <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/module.h>
#include <linux/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>
#include <media/soc_camera.h>
#include <media/soc_mediabus.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <media/ov772x.h>

/*
 * register offset
 */
#define GAIN		 0x00 /* AGC - Gain control gain setting */
#define BLUE		 0x01 /* AWB - Blue channel gain setting */
#define RED		 0x02 /* AWB - Red	 channel gain setting */
#define GREEN		 0x03 /* AWB - Green channel gain setting */
#define COM1		 0x04 /* Common control 1 */
#define BAVG		 0x05 /* U/B Average Level */
#define GAVG		 0x06 /* Y/Gb Average Level */
#define RAVG		 0x07 /* V/R Average Level */
#define AECH		 0x08 /* Exposure Value - AEC MSBs */
#define COM2		 0x09 /* Common control 2 */
#define PID		 0x0A /* Product ID Number MSB */
#define VER		 0x0B /* Product ID Number LSB */
#define COM3		 0x0C /* Common control 3 */
#define COM4		 0x0D /* Common control 4 */
#define COM5		 0x0E /* Common control 5 */
#define COM6		 0x0F /* Common control 6 */
#define AEC		 0x10 /* Exposure Value */
#define CLKRC		 0x11 /* Internal clock */
#define COM7		 0x12 /* Common control 7 */
#define COM8		 0x13 /* Common control 8 */
#define COM9		 0x14 /* Common control 9 */
#define COM10		 0x15 /* Common control 10 */
#define REG16		 0x16 /* Register 16 */
#define HSTART		 0x17 /* Horizontal sensor size */
#define HSIZE		 0x18 /* Horizontal frame (HREF column) end high 8-bit */
#define VSTART		 0x19 /* Vertical frame (row) start high 8-bit */
#define VSIZE		 0x1A /* Vertical sensor size */
#define PSHFT		 0x1B /* Data format - pixel delay select */
#define MIDH		 0x1C /* Manufacturer ID byte - high */
#define MIDL		 0x1D /* Manufacturer ID byte - low  */
#define LAEC		 0x1F /* Fine AEC value */
#define COM11		 0x20 /* Common control 11 */
#define BDBASE		 0x22 /* Banding filter Minimum AEC value */
#define DBSTEP		 0x23 /* Banding filter Maximum Setp */
#define AEW		 0x24 /* AGC/AEC - Stable operating region (upper limit) */
#define AEB		 0x25 /* AGC/AEC - Stable operating region (lower limit) */
#define VPT		 0x26 /* AGC/AEC Fast mode operating region */
#define REG28		 0x28 /* Register 28 */
#define HOUTSIZE	 0x29 /* Horizontal data output size MSBs */
#define EXHCH		 0x2A /* Dummy pixel insert MSB */
#define EXHCL		 0x2B /* Dummy pixel insert LSB */
#define VOUTSIZE	 0x2C /* Vertical data output size MSBs */
#define ADVFL		 0x2D /* LSB of insert dummy lines in Vertical direction */
#define ADVFH		 0x2E /* MSG of insert dummy lines in Vertical direction */
#define YAVE		 0x2F /* Y/G Channel Average value */
#define LUMHTH		 0x30 /* Histogram AEC/AGC Luminance high level threshold */
#define LUMLTH		 0x31 /* Histogram AEC/AGC Luminance low  level threshold */
#define HREF		 0x32 /* Image start and size control */
#define DM_LNL		 0x33 /* Dummy line low  8 bits */
#define DM_LNH		 0x34 /* Dummy line high 8 bits */
#define ADOFF_B	 	 0x35 /* AD offset compensation value for B  channel */
#define ADOFF_R		 0x36 /* AD offset compensation value for R  channel */
#define ADOFF_GB	 0x37 /* AD offset compensation value for Gb channel */
#define ADOFF_GR	 0x38 /* AD offset compensation value for Gr channel */
#define OFF_B		 0x39 /* Analog process B  channel offset value */
#define OFF_R		 0x3A /* Analog process R  channel offset value */
#define OFF_GB		 0x3B /* Analog process Gb channel offset value */
#define OFF_GR		 0x3C /* Analog process Gr channel offset value */
#define COM12		 0x3D /* Common control 12 */
#define COM13		 0x3E /* Common control 13 */
#define COM14		 0x3F /* Common control 14 */
#define COM15		 0x40 /* Common control 15*/
#define COM16		 0x41 /* Common control 16 */
#define TGT_B		 0x42 /* BLC blue channel target value */
#define TGT_R		 0x43 /* BLC red  channel target value */
#define TGT_GB		 0x44 /* BLC Gb   channel target value */
#define TGT_GR		 0x45 /* BLC Gr   channel target value */
/* for ov7720 */
#define LCC0		 0x46 /* Lens correction control 0 */
#define LCC1		 0x47 /* Lens correction option 1 - X coordinate */
#define LCC2		 0x48 /* Lens correction option 2 - Y coordinate */
#define LCC3		 0x49 /* Lens correction option 3 */
#define LCC4		 0x4A /* Lens correction option 4 - radius of the circular */
#define LCC5		 0x4B /* Lens correction option 5 */
#define LCC6		 0x4C /* Lens correction option 6 */
/* for ov7725 */
#define LC_CTR		 0x46 /* Lens correction control */
#define LC_XC		 0x47 /* X coordinate of lens correction center relative */
#define LC_YC		 0x48 /* Y coordinate of lens correction center relative */
#define LC_COEF	 	 0x49 /* Lens correction coefficient */
#define LC_RADI	 	 0x4A /* Lens correction radius */
#define LC_COEFB	 0x4B /* Lens B channel compensation coefficient */
#define LC_COEFR	 0x4C /* Lens R channel compensation coefficient */

#define FIXGAIN	 	 0x4D /* Analog fix gain amplifer */
#define AREF0		 0x4E /* Sensor reference control */
#define AREF1		 0x4F /* Sensor reference current control */
#define AREF2		 0x50 /* Analog reference control */
#define AREF3		 0x51 /* ADC	reference control */
#define AREF4		 0x52 /* ADC	reference control */
#define AREF5		 0x53 /* ADC	reference control */
#define AREF6		 0x54 /* Analog reference control */
#define AREF7		 0x55 /* Analog reference control */
#define UFIX		 0x60 /* U channel fixed value output */
#define VFIX		 0x61 /* V channel fixed value output */
#define AWBB_BLK	 0x62 /* AWB option for advanced AWB */
#define AWB_CTRL0	 0x63 /* AWB control byte 0 */
#define DSP_CTRL1	 0x64 /* DSP control byte 1 */
#define DSP_CTRL2	 0x65 /* DSP control byte 2 */
#define DSP_CTRL3	 0x66 /* DSP control byte 3 */
#define DSP_CTRL4	 0x67 /* DSP control byte 4 */
#define AWB_BIAS	 0x68 /* AWB BLC level clip */
#define AWB_CTRL1	 0x69 /* AWB control  1 */
#define AWB_CTRL2	 0x6A /* AWB control  2 */
#define AWB_CTRL3	 0x6B /* AWB control  3 */
#define AWB_CTRL4	 0x6C /* AWB control  4 */
#define AWB_CTRL5	 0x6D /* AWB control  5 */
#define AWB_CTRL6	 0x6E /* AWB control  6 */
#define AWB_CTRL7	 0x6F /* AWB control  7 */
#define AWB_CTRL8	 0x70 /* AWB control  8 */
#define AWB_CTRL9	 0x71 /* AWB control  9 */
#define AWB_CTRL10  	 0x72 /* AWB control 10 */
#define AWB_CTRL11  	 0x73 /* AWB control 11 */
#define AWB_CTRL12  	 0x74 /* AWB control 12 */
#define AWB_CTRL13  	 0x75 /* AWB control 13 */
#define AWB_CTRL14  	 0x76 /* AWB control 14 */
#define AWB_CTRL15  	 0x77 /* AWB control 15 */
#define AWB_CTRL16  	 0x78 /* AWB control 16 */
#define AWB_CTRL17  	 0x79 /* AWB control 17 */
#define AWB_CTRL18  	 0x7A /* AWB control 18 */
#define AWB_CTRL19  	 0x7B /* AWB control 19 */
#define AWB_CTRL20  	 0x7C /* AWB control 20 */
#define AWB_CTRL21  	 0x7D /* AWB control 21 */
#define GAM1		 0x7E /* Gamma Curve  1st segment input end point */
#define GAM2		 0x7F /* Gamma Curve  2nd segment input end point */
#define GAM3		 0x80 /* Gamma Curve  3rd segment input end point */
#define GAM4		 0x81 /* Gamma Curve  4th segment input end point */
#define GAM5		 0x82 /* Gamma Curve  5th segment input end point */
#define GAM6		 0x83 /* Gamma Curve  6th segment input end point */
#define GAM7		 0x84 /* Gamma Curve  7th segment input end point */
#define GAM8		 0x85 /* Gamma Curve  8th segment input end point */
#define GAM9		 0x86 /* Gamma Curve  9th segment input end point */
#define GAM10		 0x87 /* Gamma Curve 10th segment input end point */
#define GAM11		 0x88 /* Gamma Curve 11th segment input end point */
#define GAM12		 0x89 /* Gamma Curve 12th segment input end point */
#define GAM13		 0x8A /* Gamma Curve 13th segment input end point */
#define GAM14		 0x8B /* Gamma Curve 14th segment input end point */
#define GAM15		 0x8C /* Gamma Curve 15th segment input end point */
#define SLOP		 0x8D /* Gamma curve highest segment slope */
#define DNSTH		 0x8E /* De-noise threshold */
#define EDGE_STRNGT 	 0x8F /* Edge strength	control when manual mode */
#define EDGE_TRSHLD 	 0x90 /* Edge threshold control when manual mode */
#define DNSOFF		 0x91 /* Auto De-noise threshold control */
#define EDGE_UPPER  	 0x92 /* Edge strength upper limit when Auto mode */
#define EDGE_LOWER  	 0x93 /* Edge strength lower limit when Auto mode */
#define MTX1		 0x94 /* Matrix coefficient 1 */
#define MTX2		 0x95 /* Matrix coefficient 2 */
#define MTX3		 0x96 /* Matrix coefficient 3 */
#define MTX4		 0x97 /* Matrix coefficient 4 */
#define MTX5		 0x98 /* Matrix coefficient 5 */
#define MTX6		 0x99 /* Matrix coefficient 6 */
#define MTX_CTRL	 0x9A /* Matrix control */
#define BRIGHT		 0x9B /* Brightness control */
#define CNTRST		 0x9C /* Contrast contrast */
#define CNTRST_CTRL 	 0x9D /* Contrast contrast center */
#define UVAD_J0	 	 0x9E /* Auto UV adjust contrast 0 */
#define UVAD_J1	 	 0x9F /* Auto UV adjust contrast 1 */
#define SCAL0		 0xA0 /* Scaling control 0 */
#define SCAL1		 0xA1 /* Scaling control 1 */
#define SCAL2		 0xA2 /* Scaling control 2 */
#define FIFODLYM	 0xA3 /* FIFO manual mode delay control */
#define FIFODLYA	 0xA4 /* FIFO auto	 mode delay control */
#define SDE		 0xA6 /* Special digital effect control */
#define USAT		 0xA7 /* U component saturation control */
#define VSAT		 0xA8 /* V component saturation control */
/* for ov7720 */
#define HUE0		 0xA9 /* Hue control 0 */
#define HUE1		 0xAA /* Hue control 1 */
/* for ov7725 */
#define HUECOS		 0xA9 /* Cosine value */
#define HUESIN		 0xAA /* Sine value */

#define SIGN		 0xAB /* Sign bit for Hue and contrast */
#define DSPAUTO	 0xAC /* DSP auto function ON/OFF control */

/*
 * register detail
 */

/* COM2 */
#define SOFT_SLEEP_MODE 0x10	/* Soft sleep mode */
				/* Output drive capability */
#define OCAP_1x		 0x00	/* 1x */
#define OCAP_2x		 0x01	/* 2x */
#define OCAP_3x		 0x02	/* 3x */
#define OCAP_4x		 0x03	/* 4x */

/* COM3 */
#define SWAP_MASK		 (SWAP_RGB | SWAP_YUV | SWAP_ML)
#define IMG_MASK		 (VFLIP_IMG | HFLIP_IMG)

#define VFLIP_IMG	 0x80	/* Vertical flip image ON/OFF selection */
#define HFLIP_IMG	 0x40	/* Horizontal mirror image ON/OFF selection */
#define SWAP_RGB	 0x20	/* Swap B/R  output sequence in RGB mode */
#define SWAP_YUV	 0x10	/* Swap Y/UV output sequence in YUV mode */
#define SWAP_ML		 0x08	/* Swap output MSB/LSB */
				/* Tri-state option for output clock */
#define NOTRI_CLOCK	 0x04	/*	 0: Tri-state	 at this period */
				/*	 1: No tri-state at this period */
				/* Tri-state option for output data */
#define NOTRI_DATA	 0x02	/*	 0: Tri-state	 at this period */
				/*	 1: No tri-state at this period */
#define SCOLOR_TEST	 0x01	/* Sensor color bar test pattern */

/* COM4 */
				/* PLL frequency control */
#define PLL_BYPASS	 0x00	/*	00: Bypass PLL */
#define PLL_4x		 0x40	/*	01: PLL 4x */
#define PLL_6x		 0x80	/*	10: PLL 6x */
#define PLL_8x		 0xc0	/*	11: PLL 8x */
				/* AEC evaluate window */
#define AEC_FULL	 0x00	/*	00: Full window */
#define AEC_1p2		 0x10	/*	01: 1/2  window */
#define AEC_1p4		 0x20	/*	10: 1/4  window */
#define AEC_2p3		 0x30	/*	11: Low 2/3 window */

/* COM5 */
#define AFR_ON_OFF	 0x80	/* Auto frame rate control ON/OFF selection */
#define AFR_SPPED	 0x40	/* Auto frame rate control speed selection */
				/* Auto frame rate max rate control */
#define AFR_NO_RATE	 0x00	/*	   No  reduction of frame rate */
#define AFR_1p2		 0x10	/*	   Max reduction to 1/2 frame rate */
#define AFR_1p4		 0x20	/*	   Max reduction to 1/4 frame rate */
#define AFR_1p8		 0x30	/* Max reduction to 1/8 frame rate */
				/* Auto frame rate active point control */
#define AF_2x		 0x00	/*	   Add frame when AGC reaches  2x gain */
#define AF_4x		 0x04	/*	   Add frame when AGC reaches  4x gain */
#define AF_8x		 0x08	/*	   Add frame when AGC reaches  8x gain */
#define AF_16x		 0x0c	/* Add frame when AGC reaches 16x gain */
				/* AEC max step control */
#define AEC_NO_LIMIT	 0x01	/*	 0 : AEC incease step has limit */
				/*	 1 : No limit to AEC increase step */

/* COM7 */
#define SCCB_RESET	 0x80	/*	 0 : No change */
				/*	 1 : Resets all registers to default */
				/* Resolution selection */
#define SLCT_MASK	 0x60	/*	 Mask of VGA or QVGA + itu656 */
#define SLCT_VGA	 0x00	/*	 0 : VGA */
#define SLCT_QVGA	 0x40	/*	 1 : QVGA */
#define ITU656_ON_OFF	 0x20	/* ITU656 protocol ON/OFF selection */
				/* RGB output format control */
#define FMT_MASK	 0x0c	/*		Mask of color format */
#define FMT_GBR422	 0x00	/*		00 : GBR 4:2:2 */
#define FMT_RGB565	 0x04	/*		01 : RGB 565 */
#define FMT_RGB555	 0x08	/*		10 : RGB 555 */
#define FMT_RGB444	 0x0c	/* 11 : RGB 444 */
				/* Output format control */
#define OFMT_MASK	 0x03	 /*		 Mask of output format */
#define OFMT_YUV	 0x00	/*		00 : YUV */
#define OFMT_P_BRAW	 0x01	/*		01 : Processed Bayer RAW */
#define OFMT_RGB	 0x02	/*		10 : RGB */
#define OFMT_BRAW	 0x03	/* 11 : Bayer RAW */

/* COM8 */
#define FAST_ALGO	 0x80	/* Enable fast AGC/AEC algorithm */
				/* AEC Setp size limit */
#define UNLMT_STEP	 0x40	/*	 0 : Step size is limited */
				/*	 1 : Unlimited step size */
#define BNDF_ON_OFF	 0x20	/* Banding filter ON/OFF */
#define AEC_BND		 0x10	/* Enable AEC below banding value */
#define AEC_ON_OFF	 0x08	/* Fine AEC ON/OFF control */
#define AGC_ON		 0x04	/* AGC Enable */
#define AWB_ON		 0x02	/* AWB Enable */
#define AEC_ON		 0x01	/* AEC Enable */

/* COM9 */
#define BASE_AECAGC	 0x80	/* Histogram or average based AEC/AGC */
				/* Automatic gain ceiling - maximum AGC value */
#define GAIN_2x		 0x00	/*	  000 :   2x */
#define GAIN_4x		 0x10	/*	  001 :   4x */
#define GAIN_8x		 0x20	/*	  010 :   8x */
#define GAIN_16x	 0x30	/*	  011 :  16x */
#define GAIN_32x	 0x40	/*	  100 :  32x */
#define GAIN_64x	 0x50	/* 101 :  64x */
#define GAIN_128x	 0x60	/* 110 : 128x */
#define DROP_VSYNC	 0x04	/* Drop VSYNC output of corrupt frame */
#define DROP_HREF	 0x02	/* Drop HREF  output of corrupt frame */

/* COM11 */
#define SGLF_ON_OFF	 0x02	/* Single frame ON/OFF selection */
#define SGLF_TRIG	 0x01	/* Single frame transfer trigger */

/* EXHCH */
#define VSIZE_LSB	 0x04	/* Vertical data output size LSB */

/* DSP_CTRL1 */
#define FIFO_ON		 0x80	/* FIFO enable/disable selection */
#define UV_ON_OFF	 0x40	/* UV adjust function ON/OFF selection */
#define YUV444_2_422	 0x20	/* YUV444 to 422 UV channel option selection */
#define CLR_MTRX_ON_OFF  0x10	/* Color matrix ON/OFF selection */
#define INTPLT_ON_OFF	 0x08	/* Interpolation ON/OFF selection */
#define GMM_ON_OFF	 0x04	/* Gamma function ON/OFF selection */
#define AUTO_BLK_ON_OFF	 0x02	/* Black defect auto correction ON/OFF */
#define AUTO_WHT_ON_OFF  0x01	/* White define auto correction ON/OFF */

/* DSP_CTRL3 */
#define UV_MASK		 0x80	/* UV output sequence option */
#define UV_ON		 0x80	/*	 ON */
#define UV_OFF		 0x00	/*	 OFF */
#define CBAR_MASK	 0x20	/* DSP Color bar mask */
#define CBAR_ON		 0x20	/*	 ON */
#define CBAR_OFF	 0x00	/*	 OFF */

/* HSTART */
#define HST_VGA		 0x23
#define HST_QVGA	 0x3F

/* HSIZE */
#define HSZ_VGA		 0xA0
#define HSZ_QVGA	 0x50

/* VSTART */
#define VST_VGA		 0x07
#define VST_QVGA	 0x03

/* VSIZE */
#define VSZ_VGA		 0xF0
#define VSZ_QVGA	 0x78

/* HOUTSIZE */
#define HOSZ_VGA	 0xA0
#define HOSZ_QVGA	 0x50

/* VOUTSIZE */
#define VOSZ_VGA	 0xF0
#define VOSZ_QVGA	 0x78

/* DSPAUTO (DSP Auto Function ON/OFF Control) */
#define AWB_ACTRL	 0x80 /* AWB auto threshold control */
#define DENOISE_ACTRL	 0x40 /* De-noise auto threshold control */
#define EDGE_ACTRL	 0x20 /* Edge enhancement auto strength control */
#define UV_ACTRL	 0x10 /* UV adjust auto slope control */
#define SCAL0_ACTRL	 0x08 /* Auto scaling factor control */
#define SCAL1_2_ACTRL	 0x04 /* Auto scaling factor control */

/*
 * ID
 */
#define OV7720  0x7720
#define OV7725  0x7721
#define VERSION(pid, ver) ((pid<<8)|(ver&0xFF))

static int debug;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0-2)");

struct regval_list {
	unsigned char reg_num;
	unsigned char value;
};

struct ov7725_color_format {
	u32 code;
	enum v4l2_colorspace colorspace;
	u8 dsp3;
	u8 com3;
	u8 com7;
};

struct ov7725_win_size {
	char					 *name;
	struct v4l2_rect	  rect;
	unsigned char			  com7_bit;
	const struct regval_list *regs;
};

struct ov7725 {
	struct v4l2_subdev				  sd;
	struct v4l2_ctrl_handler	hdl;
	struct ov772x_camera_info		 *info;
	const struct ov7725_color_format *cfmt;
	const struct ov7725_win_size	 *win;
	int								  model;
	unsigned short					  vflip:1;
	unsigned short					  hflip:1;
	/* band_filter = COM8[5] ? 256 - BDBASE : 0 */
	unsigned short					  band_filter;
};

#define ENDMARKER { 0xff, 0xff }

/*
 * register setting for window size
 */
static const struct regval_list ov7725_qvga_regs[] = {
	{ HSTART,	HST_QVGA },
	{ HSIZE,	HSZ_QVGA },
	{ VSTART,	VST_QVGA },
	{ VSIZE,	VSZ_QVGA  },
	{ HOUTSIZE, HOSZ_QVGA },
	{ VOUTSIZE, VOSZ_QVGA },
	ENDMARKER,
};

static const struct regval_list ov7725_vga_regs[] = {
		 {0x12, 0x80},
		 {0x12, 0x24},
		 {0x03, 0x80},
		 {0x3d, 0x03},
		 {0x17, 0x22},
		 {0x18, 0xa4},
		 {0x19, 0x07},
		 {0x1a, 0xf0},
		 {0x32, 0x00},
		 {0x29, 0xa0},
		 {0x2c, 0xf0},
		 {0x2a, 0x00},
		 {0x11, 0x03},
		 {0x42, 0x7f},
		 {0x4d, 0x09},
		 {0x63, 0xe0},
		 {0x64, 0xff},
		 {0x65, 0x20},
		 {0x66, 0x00},
		 {0x67, 0x48},
		 {0x13, 0xf0},
		 {0x0d, 0x41},
		 {0x0f, 0xc5},
		 {0x14, 0x11},
		 {0x22, 0x3f},
		 {0x23, 0x07},
		 {0x24, 0x40},
		 {0x25, 0x30},
		 {0x26, 0xa1},
		 {0x2b, 0x00},
		 {0x6b, 0xaa},
		 {0x13, 0xff},
		 {0x90, 0x05},
		 {0x91, 0x01},
		 {0x92, 0x03},
		 {0x93, 0x00},
		 {0x94, 0xb0},
		 {0x95, 0x9d},
		 {0x96, 0x13},
		 {0x97, 0x16},
		 {0x98, 0x7b},
		 {0x99, 0x91},
		 {0x9a, 0x1e},
		 {0x9b, 0x08},
		 {0x9c, 0x20},
		 {0x9e, 0x81},
		 {0xa6, 0x04},
		 {0x7e, 0x0c},
		 {0x7f, 0x16},
		 {0x80, 0x2a},
		 {0x81, 0x4e},
		 {0x82, 0x61},
		 {0x83, 0x6f},
		 {0x84, 0x7b},
		 {0x85, 0x86},
		 {0x86, 0x8e},
		 {0x87, 0x97},
		 {0x88, 0xa4},
		 {0x89, 0xaf},
		 {0x8a, 0xc5},
		 {0x8b, 0xd7},
		 {0x8c, 0xe8},
		 {0x8d, 0x20},
		 {0x11, 0x01},
		 {0x22, 0x9f}, /*0x99*/
		 {0x23, 0x03},
		 {0xc, 0x00},
	ENDMARKER,
};

/*
 * supported color format list
 */
static const struct ov7725_color_format ov7725_cfmts[] = {
	{
		.code		= MEDIA_BUS_FMT_YUYV8_2X8,
		.colorspace	= V4L2_COLORSPACE_JPEG,
		.dsp3		= 0x0,
		.com3		= SWAP_YUV,
		.com7		= OFMT_YUV | ITU656_ON_OFF,
	},
	{
		.code		= MEDIA_BUS_FMT_YVYU8_2X8,
		.colorspace	= V4L2_COLORSPACE_JPEG,
		.dsp3		= UV_ON,
		.com3		= SWAP_YUV,
		.com7		= OFMT_YUV | ITU656_ON_OFF,
	},
	{
		.code		= MEDIA_BUS_FMT_UYVY8_2X8,
		.colorspace	= V4L2_COLORSPACE_JPEG,
		.dsp3		= 0x0,
		.com3		= 0x0,
		.com7		= OFMT_YUV | ITU656_ON_OFF,
	},
	{
		.code		= MEDIA_BUS_FMT_RGB555_2X8_PADHI_LE,
		.colorspace	= V4L2_COLORSPACE_SRGB,
		.dsp3		= 0x0,
		.com3		= SWAP_RGB,
		.com7		= FMT_RGB555 | OFMT_RGB,
	},
	{
		.code		= MEDIA_BUS_FMT_RGB555_2X8_PADHI_BE,
		.colorspace	= V4L2_COLORSPACE_SRGB,
		.dsp3		= 0x0,
		.com3		= 0x0,
		.com7		= FMT_RGB555 | OFMT_RGB,
	},
	{
		.code		= MEDIA_BUS_FMT_RGB565_2X8_LE,
		.colorspace	= V4L2_COLORSPACE_SRGB,
		.dsp3		= 0x0,
		.com3		= SWAP_RGB,
		.com7		= FMT_RGB565 | OFMT_RGB,
	},
	{
		.code		= MEDIA_BUS_FMT_RGB565_2X8_BE,
		.colorspace	= V4L2_COLORSPACE_SRGB,
		.dsp3		= 0x0,
		.com3		= 0x0,
		.com7		= FMT_RGB565 | OFMT_RGB,
	},
};

/*
 * window size list
 */
#define VGA_WIDTH	 640
#define VGA_HEIGHT  480
#define QVGA_WIDTH  320
#define QVGA_HEIGHT 240
#define MAX_WIDTH	 VGA_WIDTH
#define MAX_HEIGHT  VGA_HEIGHT


static const struct ov7725_win_size ov7725_win_sizes[] = {
	{
		.name	  = "VGA",
		.com7_bit = SLCT_VGA,
		.rect = {
			.left = 140,
			.top = 14,
			.width = VGA_WIDTH,
			.height = VGA_HEIGHT,
		},
		.regs	  = ov7725_vga_regs,
	}, {
		.name	  = "QVGA",
		.com7_bit = SLCT_QVGA,
		.rect = {
			.left = 252,
			.top = 6,
			.width = QVGA_WIDTH,
			.height = QVGA_HEIGHT,
		},
		.regs	  = ov7725_qvga_regs,
	},
};

static const struct v4l2_queryctrl ov7725_qctrl[] = {
	{
		.id		= V4L2_CID_VFLIP,
		.type		= V4L2_CTRL_TYPE_BOOLEAN,
		.name		= "Flip Vertically",
		.minimum	= 0,
		.maximum	= 1,
		.step		= 1,
		.default_value	= 0,
	},
	{
		.id		= V4L2_CID_HFLIP,
		.type		= V4L2_CTRL_TYPE_BOOLEAN,
		.name		= "Flip Horizontally",
		.minimum	= 0,
		.maximum	= 1,
		.step		= 1,
		.default_value	= 0,
	},
	{
		.id		= V4L2_CID_BAND_STOP_FILTER,
		.type		= V4L2_CTRL_TYPE_INTEGER,
		.name		= "Band-stop filter",
		.minimum	= 0,
		.maximum	= 256,
		.step		= 1,
		.default_value	= 0,
	},
};

/*
 * general function
 */

static struct ov7725 *to_ov7725(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov7725, sd);
}

static int ov7725_read(struct v4l2_subdev *sd, unsigned char addr)
{
	 struct i2c_client *c = v4l2_get_subdevdata(sd);
	 unsigned char buffer[1];
	 int rc;

	 buffer[0] = addr;
	 rc = i2c_master_send(c, buffer, 1);
	 if (1 != rc)
		 v4l2_dbg(0, debug, sd, "i2c i/o error: rc == %d (should be 1)\n", rc);

	 msleep(10);

	 rc = i2c_master_recv(c, buffer, 1);
	 if (1 != rc)
		 v4l2_dbg(0, debug, sd, "i2c i/o error: rc == %d (should be 1)\n", rc);

	 return buffer[0];
}

static inline int ov7725_write(struct v4l2_subdev *sd, unsigned char addr,
								  unsigned char value)
{
	 struct i2c_client *c = v4l2_get_subdevdata(sd);
	 unsigned char buffer[2];
	 int rc;

	 buffer[0] = addr;
	 buffer[1] = value;
	 rc = i2c_master_send(c, buffer, 2);
	 if (2 != rc) {
		v4l2_dbg(0, debug, sd, "i2c i/o error: rc == %d (should be 2)\n", rc);
		return -1;
	}
	return 0;
}

static inline int ov7725_mask_set(struct v4l2_subdev *sd, unsigned char addr,
					  u8  mask, u8	set)
{
	u8 val = ov7725_read (sd, addr);

	val &= ~mask;
	val |= set & mask;

	return ov7725_write (sd, addr, val);
}

static int ov7725_write_array(struct v4l2_subdev *sd,
				  const struct regval_list *vals)
{
	while (vals->reg_num != 0xff) {
		int ret = ov7725_write(sd, vals->reg_num,
						vals->value);
		if (ret < 0)
			return ret;
		vals++;
	}
	return 0;
}

static int ov7725_reset(struct v4l2_subdev *sd, u32 val)
{
	int ret = ov7725_write (sd, COM7, SCCB_RESET);
	msleep(1);
	return ret;
}

/*
 * soc_camera_ops function
 */

static int ov7725_s_stream(struct v4l2_subdev *sd, int enable)
{
	if (!enable) {
		ov7725_mask_set(sd, COM2, SOFT_SLEEP_MODE, SOFT_SLEEP_MODE);
		return 0;
	}

	ov7725_mask_set(sd, COM2, SOFT_SLEEP_MODE, 0);
	return 0;
}

static int ov7725_queryctrl(struct v4l2_subdev *sd, struct v4l2_queryctrl *qc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ov7725_qctrl); i++)
		 if (qc->id && qc->id == ov7725_qctrl[i].id) {
			 memcpy(qc, &(ov7725_qctrl[i]),
				sizeof(*qc));
			 return 0;
		 }

	return -EINVAL;
}

static int ov7725_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct ov7725 *core = to_ov7725(sd);

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		ctrl->value = core->vflip;
		return 0;
	case V4L2_CID_HFLIP:
		ctrl->value = core->hflip;
		return 0;
	case V4L2_CID_BAND_STOP_FILTER:
		ctrl->value = core->band_filter;
		return 0;
	}
	return -EINVAL;
}

static int ov7725_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct ov7725 *core = to_ov7725(sd);
	int ret = 0;
	u8 val;
	u8 i, n;
	n = ARRAY_SIZE(ov7725_qctrl);

	for (i = 0; i < n; i++) {
		 if (ctrl->id != ov7725_qctrl[i].id)
			 continue;
		 if (ctrl->value < ov7725_qctrl[i].minimum ||
				 ctrl->value > ov7725_qctrl[i].maximum)
					 return -ERANGE;
		 v4l2_dbg(1, debug, sd, "s_ctrl: id=%d, value=%d\n",
						 ctrl->id, ctrl->value);
		 break;
		 }

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		val = ctrl->value ? VFLIP_IMG : 0x00;
		core->vflip = ctrl->value;
		return ov7725_mask_set(sd, COM3, VFLIP_IMG, val);
	case V4L2_CID_HFLIP:
		val = ctrl->value ? HFLIP_IMG : 0x00;
		core->hflip = ctrl->value;
		return ov7725_mask_set(sd, COM3, HFLIP_IMG, val);
	case V4L2_CID_BAND_STOP_FILTER:
		if ((unsigned)ctrl->value > 256)
			ctrl->value = 256;
		if (ctrl->value == core->band_filter)
			return 0;
		if (!ctrl->value) {
			/* Switch the filter off, it is on now */
			ret = ov7725_mask_set(sd, BDBASE, 0xff, 0xff);
			if (!ret)
				ret = ov7725_mask_set(sd, COM8, BNDF_ON_OFF, 0);
		} else {
			/* Switch the filter on, set AEC low limit */
			val = 256 - ctrl->value;
			ret = ov7725_mask_set(sd, COM8, BNDF_ON_OFF, BNDF_ON_OFF);
			if (!ret)
				ret = ov7725_mask_set(sd, BDBASE, 0xff, val);
		}
		if (!ret)
			core->band_filter = ctrl->value;
		return ret;
	}

	return -EINVAL;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int ov7725_g_register(struct v4l2_subdev *sd,
				 struct v4l2_dbg_register *reg)
{
	int ret;

	reg->size = 1;
	if (reg->reg > 0xff)
		return -EINVAL;

	ret = ov7725_read(sd, reg->reg);
	if (ret < 0)
		return ret;

	reg->val = (__u64)ret;

	return 0;
}

static int ov7725_s_register(struct v4l2_subdev *sd,
				 struct v4l2_dbg_register *reg)
{

	if (reg->reg > 0xff ||
		reg->val > 0xff)
		return -EINVAL;

	ov7725_write (sd, reg->reg, reg->val);

	return 0;
}
#endif

static int ov7725_set_params(struct v4l2_subdev *sd, u32 *width, u32 *height,
				 u32 code)
{
	struct ov7725 *core = to_ov7725(sd);
	int ret = -EINVAL;
	u8	val;
	int i;

	/*
	 * select format
	 */
	core->cfmt = NULL;
	for (i = 0; i < ARRAY_SIZE(ov7725_cfmts); i++) {
		if (code == ov7725_cfmts[i].code) {
			core->cfmt = ov7725_cfmts + i;
			break;
		}
	}
	if (!core->cfmt)
		goto ov7725_set_fmt_error;

	/*
	 * select VGA window
	 */
	core->win = &ov7725_win_sizes[0];
	/*
	 * reset hardware
	 */
	ov7725_reset(sd, 0);

	/*
	 * set size format
	 */
	ret = ov7725_write_array(sd, core->win->regs);
	if (ret < 0)
		goto ov7725_set_fmt_error;

	/*
	 * set COM7
	 */
	val = core->win->com7_bit | core->cfmt->com7;
	ret = ov7725_mask_set(sd,
				  COM7, SLCT_MASK | FMT_MASK | OFMT_MASK,
				  val);
	if (ret < 0)
		goto ov7725_set_fmt_error;

	*width = core->win->rect.width;
	*height = core->win->rect.height;

	return ret;

ov7725_set_fmt_error:
	ov7725_reset(sd, 0);
	core->win = NULL;
	core->cfmt = NULL;

	return ret;
}

static int ov7725_g_crop(struct v4l2_subdev *sd, struct v4l2_crop *a)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov7725 *core = to_ov7725(sd);

	a->c.width = core->win->rect.width;
	a->c.height = core->win->rect.height;
	a->type		= V4L2_BUF_TYPE_VIDEO_CAPTURE;

	dev_dbg(&client->dev, "ov7725_g_crop: w %d, h %d\n",
		a->c.width, a->c.height);
	return 0;
}

static int ov7725_cropcap(struct v4l2_subdev *sd, struct v4l2_cropcap *a)
{
	struct ov7725 *core = to_ov7725(sd);

	a->bounds.left			= 0;
	a->bounds.top			= 0;
	a->bounds.width	= core->win->rect.width;
	a->bounds.height = core->win->rect.height;
	a->defrect			= a->bounds;
	a->type				= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	a->pixelaspect.numerator	= 1;
	a->pixelaspect.denominator	= 1;

	return 0;
}

static int ov7725_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	struct ov7725 *core = to_ov7725(sd);

	if (!core->win || !core->cfmt) {
		u32 width = VGA_WIDTH, height = VGA_HEIGHT;
		int ret = ov7725_set_params(sd, &width, &height,
						MEDIA_BUS_FMT_YUYV8_2X8);
		if (ret < 0)
			return ret;
	}

	mf->width	= core->win->rect.width;
	mf->height	= core->win->rect.height;
	mf->code	= core->cfmt->code;
	mf->colorspace	= core->cfmt->colorspace;

	return 0;
}

static int ov7725_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *mf = &format->format;
	const struct ov7725_win_size *win;
	const struct ov7725_color_format *cfmt;

	cfmt = &ov7725_cfmts[0];
	/* Select a window size. */
	win = &ov7725_win_sizes[0];

	if (!format || format->pad)
		return -EINVAL;

	mf->width = win->rect.width;
	mf->height = win->rect.height;
	mf->field = V4L2_FIELD_NONE;
	mf->code = MEDIA_BUS_FMT_YUYV8_2X8;
	mf->colorspace = cfmt->colorspace;

	ov7725_set_params(sd, &mf->width, &mf->height,
					mf->code);
	return 0;
}

static int ov7725_video_probe(struct v4l2_subdev *sd)
{
	u8 pid, ver;
	const char		   *devname;
		 struct i2c_client *client = v4l2_get_subdevdata(sd);

	/*
	 * check and show product ID and manufacturer ID
	 */
	pid = ov7725_read(sd, PID);
	ver = ov7725_read(sd, VER);
	switch (VERSION(pid, ver)) {
	case OV7725:
		devname		= "ov7725";
		break;
	default:
		dev_err(&client->dev, "Product ID error %x:%x\n", pid, ver);
		return -ENODEV;
	}

	dev_info(&client->dev,
		 "%s: Product ID %0x:%0x Manufacturer ID %x:%x\n",
		 devname,
		 pid,
		 ver,
		 ov7725_read(sd, MIDH),
		 ov7725_read(sd, MIDL));
	return 0;

}

static int ov7725_g_mbus_config(struct v4l2_subdev *sd,
				struct v4l2_mbus_config *cfg)
{
	cfg->flags = V4L2_MBUS_PCLK_SAMPLE_RISING | V4L2_MBUS_MASTER |
		V4L2_MBUS_VSYNC_ACTIVE_HIGH | V4L2_MBUS_HSYNC_ACTIVE_HIGH |
		V4L2_MBUS_DATA_ACTIVE_HIGH;
	cfg->type = V4L2_MBUS_PARALLEL;

	return 0;
}

static int ov7725_enum_mbus_code(struct v4l2_subdev *sd,
		struct v4l2_subdev_pad_config *cfg,
		struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index >= ARRAY_SIZE(ov7725_cfmts))
		return -EINVAL;

	code->code = ov7725_cfmts[code->index].code;
	return 0;
}

static struct v4l2_subdev_core_ops ov7725_subdev_core_ops = {
	.reset		= ov7725_reset,
	.queryctrl		= ov7725_queryctrl,
	.g_ctrl		= ov7725_g_ctrl,
	.s_ctrl		= ov7725_s_ctrl,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register	= ov7725_g_register,
	.s_register	= ov7725_s_register,
#endif
};

static struct v4l2_subdev_video_ops ov7725_subdev_video_ops = {
	.s_stream	= ov7725_s_stream,
	.cropcap	= ov7725_cropcap,
	.g_crop		= ov7725_g_crop,
	.g_mbus_config	= ov7725_g_mbus_config,
};

static const struct v4l2_subdev_pad_ops ov7725_subdev_pad_ops = {
	.enum_mbus_code = ov7725_enum_mbus_code,
	.get_fmt	= ov7725_get_fmt,
	.set_fmt	= ov7725_set_fmt,
};


static struct v4l2_subdev_ops ov7725_subdev_ops = {
	.core	= &ov7725_subdev_core_ops,
	.video	= &ov7725_subdev_video_ops,
	.pad	= &ov7725_subdev_pad_ops,
};

/*
 * i2c_driver function
 */
static int ov7725_probe(struct i2c_client *client,
			const struct i2c_device_id *did)
{
		 struct ov7725 *core;
		 struct v4l2_subdev *sd;
	int ret;
	u8 chip_id_high, chip_id_low;

		 /* Check if the adapter supports the needed features */
	if (!i2c_check_functionality(client->adapter,
		  I2C_FUNC_SMBUS_READ_BYTE | I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		 return -EIO;

	core = kzalloc(sizeof(struct ov7725), GFP_KERNEL);
	if (!core) {
		 return -ENOMEM;
	}
	core->info = kzalloc(sizeof(struct ov772x_camera_info), GFP_KERNEL);
	sd = &core->sd;
	v4l2_i2c_subdev_init(sd, client, &ov7725_subdev_ops);

	chip_id_high = ov7725_read(sd, PID);
	if (chip_id_high != 0x77) {
		pr_warning("camera ov7725 is not found\n");
		return -ENODEV;
	}
	chip_id_low = ov7725_read(sd, VER);
	if (chip_id_low != 0x21) {
		pr_warning("camera ov7725 is not found\n");
				 return -ENODEV;
	}

	v4l_info(client, "chip found @ 0x%02x (%s)\n",
				  client->addr, client->adapter->name);
	ret = ov7725_video_probe(sd);
	if (ret) {
		v4l2_info(sd, "camera ov7725 is not found.\n");
		kfree(core);
		return -EINVAL;
	}

	v4l2_ctrl_handler_init(&core->hdl, 5);
	sd->ctrl_handler = &core->hdl;
	if (core->hdl.error) {
		ret = core->hdl.error;
		goto err;
	}
	v4l2_ctrl_handler_setup(&core->hdl);

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0) {
		goto err;
	}

err:
	v4l2_ctrl_handler_free(&core->hdl);

	return ret;
}

static int ov7725_remove(struct i2c_client *client)
{
		 struct v4l2_subdev *sd = i2c_get_clientdata(client);

		 v4l2_dbg(1, debug, sd,
				 "ov7725.c: removing ov7725 adapter on address 0x%x\n",
				 client->addr << 1);

		 v4l2_device_unregister_subdev(sd);
		 kfree(to_ov7725(sd));
		 return 0;
}

static const struct i2c_device_id ov7725_id[] = {
	{ "ov772x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ov7725_id);

static struct i2c_driver ov7725_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name = "ov7725",
	},
	.probe	  = ov7725_probe,
	.remove   = ov7725_remove,
	.id_table = ov7725_id,
};

module_i2c_driver(ov7725_i2c_driver);

MODULE_DESCRIPTION("Camera driver for ov7725");
MODULE_LICENSE("GPL v2");

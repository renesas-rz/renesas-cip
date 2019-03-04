/*
 * Copyright (c) 2016 iWave Systems Technologies Pvt. Ltd.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>


#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder_slave.h>
#include <drm/drm_edid.h>

struct sii902x {
	struct i2c_client *client;

	enum drm_connector_status status;
	int dpms_mode;

	unsigned int current_edid_segment;
	uint8_t edid_buf[256];

	wait_queue_head_t wq;
	struct drm_encoder *encoder;

	struct edid *edid;
	int gpio_pd;

	struct workqueue_struct *work_queue;
	struct delayed_work edid_handler;
	struct delayed_work hpd_handler;
	unsigned int edid_read_retries;
	bool connector_detect_disconnect;
};

static struct sii902x *encoder_to_sii902x(struct drm_encoder *encoder)
{
	return to_encoder_slave(encoder)->slave_priv;
}

static int sii902x_write(struct i2c_client *client, uint8_t addr, uint8_t val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, addr, val);
	if (ret)
		dev_info(&client->dev, "%s failed with %d\n", __func__, ret);

	return ret;
}

static uint8_t sii902x_read(struct i2c_client *client, uint8_t addr)
{
	int dat;

	dat = i2c_smbus_read_byte_data(client, addr);

	return dat;
}

static int hdmi_cap;

static void sii902x_poweron(struct i2c_client *client)
{
	/* Turn on DVI or HDMI */
	if (hdmi_cap)
		sii902x_write(client, 0x1A, 0x01 | 4);
	else
		sii902x_write(client, 0x1A, 0x00);
}

static void sii902x_poweroff(struct i2c_client *client)
{
	/* disable tmds before changing resolution */
	if (hdmi_cap)
		sii902x_write(client, 0x1A, 0x11);
	else
		sii902x_write(client, 0x1A, 0x10);
}

static int sii902x_get_modes(struct drm_encoder *encoder,
	struct drm_connector *connector)
{
	struct i2c_client *client = encoder_to_sii902x(encoder)->client;
	struct i2c_adapter *adap = client->adapter;

	struct edid *edid;
	int ret;
	int old, dat, cnt = 100;

	old = sii902x_read(client, 0x1A);

	sii902x_write(client, 0x1A, old | 0x4);
	do {
		cnt--;
		msleep(10);
		dat = sii902x_read(client, 0x1A);
	} while ((!(dat & 0x2)) && cnt);

	if (!cnt)
		return -ETIMEDOUT;

	sii902x_write(client, 0x1A, old | 0x06);

	edid = drm_get_edid(connector, adap);
	if (edid) {
		drm_mode_connector_update_edid_property(connector, edid);
		ret = drm_add_edid_modes(connector, edid);
		kfree(edid);
	}

	sii902x_read(client, 0x1A);

	cnt = 100;
	do {
		cnt--;
		/*Once the host has finished reading EDID, it must write TPI 0x1A= 00 to clear the request*/
		sii902x_write(client, 0x1A, 0x0);
		msleep(10);
		dat = sii902x_read(client, 0x1A);
	} while ((dat & 0x6) && cnt);

	if (!cnt)
		ret = -1;

	sii902x_write(client, 0x1A, old);

	return 0;
}

static irqreturn_t sii902x_detect_handler(int irq, void *data)
{
	struct sii902x *sii902x = data;
	struct i2c_client *client = sii902x->client;
	int dat;

	dat = sii902x_read(client, 0x3D);

	sii902x_write(client, 0x3D, dat);

	return IRQ_HANDLED;
}


static int sii902x_mode_valid(struct drm_encoder *encoder,
			struct drm_display_mode *mode)
{
	return MODE_OK;
}

static void sii902x_mode_set(struct drm_encoder *encoder,
		struct drm_display_mode *mode,
		struct drm_display_mode *adjusted_mode)
{
	struct i2c_client *client = encoder_to_sii902x(encoder)->client;

	u16 data[4];
	u32 refresh;
	u8 *tmp;
	int i;

	/* Power up */
	sii902x_write(client, 0x1E, 0x00);

	dev_dbg(&client->dev, "%s: %dx%d, pixclk %d\n", __func__,
			mode->hdisplay, mode->vdisplay,
			mode->clock * 1000);

	/* set TPI video mode */
	data[0] = mode->clock / 10;
	data[2] = mode->htotal;
	data[3] = mode->vtotal;
	refresh = data[2] * data[3];
	refresh = (mode->clock * 1000) / refresh;
	data[1] = refresh * 100;
	tmp = (u8 *)data;
	for (i = 0; i < 8; i++)
		sii902x_write(client, i, tmp[i]);

	/* input bus/pixel: full pixel wide (24bit), rising edge */
	sii902x_write(client, 0x08, 0x70);
	/* Set input format to RGB */
	sii902x_write(client, 0x09, 0x00);
	/* set output format to RGB */
	sii902x_write(client, 0x0A, 0x00);
	/* audio setup */
	sii902x_write(client, 0x25, 0x00);
	sii902x_write(client, 0x26, 0x40);
	sii902x_write(client, 0x27, 0x00);
}

static void sii902x_dpms(struct drm_encoder *encoder, int mode)
{
	struct i2c_client *client = encoder_to_sii902x(encoder)->client;

	if (mode)
		sii902x_poweroff(client);
	else
		sii902x_poweron(client);
}

static enum
drm_connector_status sii902x_encoder_detect(struct drm_encoder *encoder,
	struct drm_connector *connector)
{
	return 1;
}

struct drm_encoder_slave_funcs sii902x_encoder_funcs = {
	.dpms = sii902x_dpms,
	.mode_set = sii902x_mode_set,
	.get_modes = sii902x_get_modes,
	.mode_valid = sii902x_mode_valid,
	.detect = sii902x_encoder_detect,
};

/* I2C driver functions */

static int
sii902x_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int dat, ret, int_gpio;
	struct sii902x *sii902x;
	struct device_node *np;

	sii902x = kzalloc(sizeof(*sii902x), GFP_KERNEL);
	if (!sii902x)
		return -ENOMEM;

	sii902x->client = client;

	/* Set 902x in hardware TPI mode on and jump out of D3 state */
	if (sii902x_write(client, 0xc7, 0x00) < 0) {
		dev_err(&client->dev, "SII902x: cound not find device\n");
		return -ENODEV;
	}

	/* read device ID */
	dat = sii902x_read(client, 0x1b);
	if (dat != 0xb0) {
		dev_err(&client->dev, "not found. id is 0x%02x instead of 0xb0\n",
				dat);
		return -ENODEV;
	}

	np = of_find_compatible_node(NULL, NULL, "iwave,g23m-sbc-hdmi-int");
	int_gpio = of_get_named_gpio(np, "int-gpios", 0);
	if (gpio_is_valid(int_gpio) && !gpio_request_one(int_gpio,
		GPIOF_DIR_IN | GPIOF_EXPORT_DIR_FIXED, "hdmi-int"))
		client->irq = gpio_to_irq(int_gpio);

	if (client->irq) {
		ret = request_threaded_irq(client->irq, NULL,
				sii902x_detect_handler,
				IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
				"SII902x_det", sii902x);
		sii902x_write(client, 0x3c, 0x01);
	}

	i2c_set_clientdata(client, sii902x);
	/* Power up */
	sii902x_write(client, 0x1E, 0x00);
	sii902x_write(client, 0x1A, 0x00);

	dev_info(&client->dev, "initialized\n");

	return 0;
}

static int sii902x_remove(struct i2c_client *client)
{
	return 0;
}

static int sii902x_encoder_init(struct i2c_client *i2c,
	struct drm_device *dev, struct drm_encoder_slave *encoder)
{

	struct sii902x *sii902x = i2c_get_clientdata(i2c);

	encoder->slave_priv = sii902x;
	encoder->slave_funcs = &sii902x_encoder_funcs;

	sii902x->encoder = &encoder->base;

	return 0;
}

static struct i2c_device_id sii902x_ids[] = {
	{ "sii9022", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, sii902x_ids);

static const struct of_device_id sii902x_of_ids[] = {
	{ .compatible = "sil,sii9022", },
	{ }
};
MODULE_DEVICE_TABLE(of, sii902x_of_ids);
static struct drm_i2c_encoder_driver sii902x_driver = {
	.i2c_driver = {
		.probe = sii902x_probe,
		.remove = sii902x_remove,
		.driver = {
			.name = "sii902x",
			 .of_match_table = sii902x_of_ids,
		},
		.id_table = sii902x_ids,
	},

	.encoder_init = sii902x_encoder_init,
};

static int __init sii902x_init(void)
{
	return drm_i2c_encoder_register(THIS_MODULE, &sii902x_driver);
}

static void __exit sii902x_exit(void)
{
	drm_i2c_encoder_unregister(&sii902x_driver);
}

MODULE_AUTHOR("Sascha Hauer <s.hauer at pengutronix.de>");
MODULE_DESCRIPTION("Silicon Image sii902x HDMI transmitter driver");
MODULE_LICENSE("GPL");

module_init(sii902x_init);
module_exit(sii902x_exit);

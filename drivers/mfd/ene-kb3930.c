// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-or-later
/*
 * ENE KB3930 Embedded Controller Driver
 *
 * Copyright (C) 2020 Lubomir Rintel
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

/* I2C registers that are multiplexing access to the EC RAM. */
enum {
	EC_DATA_IN	= 0x00,
	EC_RAM_OUT	= 0x80,
	EC_RAM_IN	= 0x81,
};

/* EC RAM registers. */
enum {
	EC_MODEL	= 0x30,
	EC_VERSION_MAJ	= 0x31,
	EC_VERSION_MIN	= 0x32,
};

struct kb3930 {
	struct i2c_client *client;
	struct regmap *ram_regmap;
	struct gpio_descs *off_gpios;
	struct power_handler power_handler;
};

#define EC_GPIO_WAVE		0
#define EC_GPIO_OFF_MODE	1

#define EC_OFF_MODE_REBOOT	0
#define EC_OFF_MODE_POWER	1

static void kb3930_off(struct kb3930 *ddata, int off_mode)
{
	gpiod_direction_output(ddata->off_gpios->desc[EC_GPIO_OFF_MODE],
			       off_mode);

	/*
	 * This creates a 10 Hz wave on EC_GPIO_WAVE that signals a
	 * shutdown request to the EC. Once the EC detects it, it will
	 * proceed to turn the power off or reset the board depending on
	 * the value of EC_GPIO_OFF_MODE.
	 */
	while (1) {
		mdelay(50);
		gpiod_direction_output(ddata->off_gpios->desc[EC_GPIO_WAVE], 0);
		mdelay(50);
		gpiod_direction_output(ddata->off_gpios->desc[EC_GPIO_WAVE], 1);
	}
}

static void kb3930_restart(struct restart_data *data)
{
	struct kb3930 *ddata = data->cb_data;

	kb3930_off(ddata, EC_OFF_MODE_REBOOT);
}

static void kb3930_power_off(struct power_off_data *data)
{
	struct kb3930 *ddata = data->cb_data;

	kb3930_off(ddata, EC_OFF_MODE_POWER);
}

static const struct mfd_cell ariel_ec_cells[] = {
	{ .name = "dell-wyse-ariel-led", },
	{ .name = "dell-wyse-ariel-power", },
};

static int kb3930_ec_ram_reg_write(void *context, unsigned int reg,
				   unsigned int val)
{
	struct kb3930 *ddata = context;

	return i2c_smbus_write_word_data(ddata->client, EC_RAM_OUT,
					 (val << 8) | reg);
}

static int kb3930_ec_ram_reg_read(void *context, unsigned int reg,
				  unsigned int *val)
{
	struct kb3930 *ddata = context;
	int ret;

	ret = i2c_smbus_write_word_data(ddata->client, EC_RAM_IN, reg);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_read_word_data(ddata->client, EC_DATA_IN);
	if (ret < 0)
		return ret;

	*val = ret >> 8;
	return 0;
}

static const struct regmap_config kb3930_ram_regmap_config = {
	.name = "ec_ram",
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = 0xff,
	.reg_write = kb3930_ec_ram_reg_write,
	.reg_read = kb3930_ec_ram_reg_read,
	.fast_io = false,
};

static int kb3930_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct kb3930 *ddata;
	unsigned int model;
	int ret;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->client = client;
	i2c_set_clientdata(client, ddata);

	ddata->ram_regmap = devm_regmap_init(dev, NULL, ddata,
					     &kb3930_ram_regmap_config);
	if (IS_ERR(ddata->ram_regmap))
		return PTR_ERR(ddata->ram_regmap);

	ret = regmap_read(ddata->ram_regmap, EC_MODEL, &model);
	if (ret < 0)
		return ret;

	/* Currently we only support the cells present on Dell Ariel model. */
	if (model != 'J') {
		dev_err(dev, "unknown board model: %02x\n", model);
		return -ENODEV;
	}

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				   ariel_ec_cells,
				   ARRAY_SIZE(ariel_ec_cells),
				   NULL, 0, NULL);
	if (ret)
		return ret;

	if (of_property_read_bool(np, "system-power-controller")) {
		ddata->off_gpios =
			devm_gpiod_get_array_optional(dev, "off", GPIOD_IN);
		if (IS_ERR(ddata->off_gpios))
			return PTR_ERR(ddata->off_gpios);
		if (ddata->off_gpios->ndescs < 2) {
			dev_err(dev, "invalid off-gpios property\n");
			return -EINVAL;
		}
	}

	if (ddata->off_gpios) {
		ddata->power_handler.cb_data = ddata;
		ddata->power_handler.restart_cb = kb3930_restart;
		ddata->power_handler.power_off_cb = kb3930_power_off;

		ret = devm_register_power_handler(dev, &ddata->power_handler);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct of_device_id kb3930_dt_ids[] = {
	{ .compatible = "ene,kb3930" },
	{ }
};
MODULE_DEVICE_TABLE(of, kb3930_dt_ids);

static struct i2c_driver kb3930_driver = {
	.probe_new = kb3930_probe,
	.driver = {
		.name = "ene-kb3930",
		.of_match_table = kb3930_dt_ids,
	},
};
module_i2c_driver(kb3930_driver);

MODULE_AUTHOR("Lubomir Rintel <lkundrak@v3.sk>");
MODULE_DESCRIPTION("ENE KB3930 Embedded Controller Driver");
MODULE_LICENSE("Dual BSD/GPL");

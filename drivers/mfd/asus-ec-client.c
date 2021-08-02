// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS EC client driver
 *
 * Written by: Svyatoslav Ryhel <clamor95@gmail.com>
 *
 * Copyright (C) 2021 Svyatoslav Ryhel
 *
 */

#include <linux/delay.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mfd/asus-ec.h>

struct asusec_client_data {
	struct asusec_info info;
	struct i2c_client *dockram;
	struct platform_device *pdev;
	u8 ec_data[DOCKRAM_ENTRY_BUFSIZE];
};

struct asusec_client_initdata {
	const char *model;
};

static struct asusec_platform_data asusec_pdata = {
	.battery_addr = 0x24,
	.charger_addr = 0x23,
};

static const struct mfd_cell asusec_client_cells[] = {
	{
		.name = "asusec-battery",
		.platform_data = &asusec_pdata,
		.pdata_size = sizeof(asusec_pdata),
	},
	{
		.name = "asusec-charger",
		.platform_data = &asusec_pdata,
		.pdata_size = sizeof(asusec_pdata),
	},
};

static const struct asusec_client_initdata asusec_model_info[] = {
	{	/* Asus T30 Windows Mobile Dock  */
		.model		= "ASUS-TF600T-DOCK",
	},
	{	/* Asus T114 Mobile Dock */
		.model		= "ASUS-TF701T-DOCK",
	},
};

static int asusec_client_read(struct i2c_client *client, int reg, char *buf)
{
	int ret, i;
	u8 command[] = { 0x05, 0x0b, 0x00, 0x36, (u8)reg, (u8)24 };

	ret = asus_dockram_write(client, 0x11, command);
	if (ret < 0)
		return ret;

	msleep(20);

	ret = asus_dockram_read(client, 0x11, buf);
	if (ret < 0)
	        return ret;

	/* A strange loop to read status data */
	for (i = 9; i < 32; i++)
		buf[i-9] = buf[i];

	return 0;
}

static int asusec_client_log_info(struct asusec_client_data *priv, unsigned int reg,
			    const char *name, char **out)
{
	char *buf = priv->ec_data;
	int ret;

	ret = asusec_client_read(priv->dockram, reg, buf);
	if (ret < 0)
		return ret;

	pr_info("asus-ec-client: %-14s: %.*s\n", name, buf[0], buf);

	if (out)
		*out = kstrndup(buf, buf[0], GFP_KERNEL);

	return 0;
}

static int asusec_client_detect(struct asusec_client_data *priv)
{
	char *model = NULL;
	int ret, i;

	ret = asusec_client_log_info(priv, 0x01, "model", &model);
	if (ret)
		goto err_exit;

	ret = asusec_client_log_info(priv, 0x02, "FW version", NULL);
	if (ret)
		goto err_exit;

	ret = asusec_client_log_info(priv, 0x03, "Config format", NULL);
	if (ret)
		goto err_exit;

	ret = asusec_client_log_info(priv, 0x04, "HW version", NULL);
	if (ret)
		goto err_exit;

	for (i = 0; i < ARRAY_SIZE(asusec_model_info); i++) {
		ret = strcmp(model, asusec_model_info[i].model);
		if (!ret)
			break;
	}

	kfree(model);

	return ret;

err_exit:
	if (ret)
		dev_err(&priv->pdev->dev, "failed to access EC: %d\n", ret);
	kfree(model);
	return ret;
}

static int asusec_client_probe(struct platform_device *pdev)
{
	const struct asusec_info *ec = asusec_cell_to_ec(pdev);
	struct asusec_client_data *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->info.name = ec->name;
	priv->info.dockram = ec->dockram;

	priv->dockram = ec->dockram;
	priv->pdev = pdev;

	ret = asusec_client_detect(priv);
	if (ret) {
		dev_err(&pdev->dev, "EC model not recognized\n");
		return -ENODEV;
	}

	ret = mfd_add_devices(&priv->pdev->dev, PLATFORM_DEVID_AUTO,
			      asusec_client_cells,
			      ARRAY_SIZE(asusec_client_cells),
			      NULL, 0, NULL);
	if (ret) {
		dev_err(&pdev->dev, "failed to add sub-devices: %d\n", ret);
		return ret;
	}

	return 0;
}

static int asusec_client_remove(struct platform_device *pdev)
{
	struct asusec_client_data *priv = dev_get_drvdata(&pdev->dev);

	mfd_remove_devices(&priv->pdev->dev);

	return 0;
}

static const struct of_device_id asusec_client_match[] = {
	{ .compatible = "asus,dock-ec" },
	{ },
};
MODULE_DEVICE_TABLE(of, asusec_client_match);

static struct platform_driver asusec_client_driver = {
	.driver = {
		.name = "asusec-client",
		.of_match_table	= asusec_client_match,
	},
	.probe = asusec_client_probe,
	.remove = asusec_client_remove,
};
module_platform_driver(asusec_client_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("ASUS Transformer client EC driver");
MODULE_LICENSE("GPL");

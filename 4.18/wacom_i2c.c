// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Wacom Penabled Driver for I2C
 *
 * Copyright (c) 2011 - 2023 Tatsunosuke Tobita, Wacom.
 * <tatsunosuke.tobita@wacom.com>
 */

#include "../config.h"

#include <linux/module.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#ifdef WACOM_LINUX_UNALIGNED
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,19,0)
#include <linux/bits.h>

/* Bitmasks (for data[3]) */
#define WACOM_TIP_SWITCH	BIT(0)
#define WACOM_BARREL_SWITCH	BIT(1)
#define WACOM_ERASER		BIT(2)
#define WACOM_INVERT		BIT(3)
#define WACOM_BARREL_SWITCH_2	BIT(4)
#define WACOM_IN_PROXIMITY	BIT(5)

#else
/* Bitmasks (for data[3]) */
#define WACOM_TIP_SWITCH	0x01
#define WACOM_BARREL_SWITCH	0x02
#define WACOM_ERASER		0x04
#define WACOM_INVERT		0x08
#define WACOM_BARREL_SWITCH_2	0x10
#define WACOM_IN_PROXIMITY	0x20
#endif

/* Registers */
#define WACOM_COMMAND_LSB	0x04
#define WACOM_COMMAND_MSB	0x00

#define WACOM_DATA_LSB		0x05
#define WACOM_DATA_MSB		0x00

/* Report types */
#define REPORT_FEATURE		0x30

/* Requests / operations */
#define OPCODE_GET_REPORT	0x02

#define WACOM_QUERY_REPORT	3
#define WACOM_QUERY_SIZE	22

/* Resolutions */
#define XY_RESOLUTION		100	/* Distance : SI Linear Unit with exponent -3 */
#define DIST_RESOLUTION		10	/* Distance : SI Linear Unit with exponent -2.
					 * This covers 'Z' resolution too */
#define TILT_RESOLUTION		5730	/* Degrees : English Rotation with exponent -2 */

/* Generation selction */
#define WACOM_BG9		0	/* G9 or earlier neither height nor tilt is supported */
#define WACOM_AG12		1	/* After G12 the IC supports "height"
					 * which is "ABS_DISTANCE" event */
#define MAX_LEN_BG9		10	/* Packet length for G9 or eralier */
#define MAX_LEN_G12		15	/* Length for G12 */
#define MAX_LEN_AG14		17	/* Length for G14 or later */

#define DISTANCE_MAX		255

struct feature_support {
	bool distance;
	bool tilt;
};

struct wacom_features {
	struct feature_support support;
	int x_max;
	int y_max;
	int pressure_max;
	int distance_max;
	int tilt_x_max;
	int tilt_y_max;	char fw_version;
	unsigned char generation;
};

struct wacom_i2c {
	struct i2c_client *client;
	struct input_dev *input;
	struct wacom_features features;
	u8 data[WACOM_QUERY_SIZE];
	bool prox;
	int tool;
};

static int wacom_query_device(struct i2c_client *client,
			      struct wacom_features *features)
{
	u8 get_query_data_cmd[] = {
		WACOM_COMMAND_LSB,
		WACOM_COMMAND_MSB,
		REPORT_FEATURE | WACOM_QUERY_REPORT,
		OPCODE_GET_REPORT,
		WACOM_DATA_LSB,
		WACOM_DATA_MSB,
	};
	u8 data[WACOM_QUERY_SIZE];
	int ret;

	struct i2c_msg msgs[] = {
		/* Request reading of feature ReportID: 3 (Pen Query Data) */
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(get_query_data_cmd),
			.buf = get_query_data_cmd,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(data),
			.buf = data,
		},
	};

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	features->x_max = get_unaligned_le16(&data[3]);
	features->y_max = get_unaligned_le16(&data[5]);
	features->distance_max = data[16];
	features->pressure_max = get_unaligned_le16(&data[11]);
	features->fw_version = get_unaligned_le16(&data[13]);
	features->tilt_x_max = get_unaligned_le16(&data[17]);
	features->tilt_y_max = get_unaligned_le16(&data[19]);
	features->distance_max = data[16];

	if (features->distance_max)
		features->support.distance = true;

	if ((features->tilt_x_max && features->tilt_y_max))
		features->support.tilt = true;

	dev_dbg(&client->dev,
		"x_max: %d, y_max: %d, pressure: %d, fw: %d, distance: %d,"
		" tilt_x_max: %d, tilt_y_max: %d \n",
		features->x_max, features->y_max,
		features->pressure_max, features->fw_version,
		features->distance_max,
		features->tilt_x_max, features->tilt_y_max);

	if (!features->support.distance && !features->support.tilt)
		features->generation = WACOM_BG9;
	else if (features->distance_max == DISTANCE_MAX)
		features->generation = WACOM_AG12;
	return 0;
}

static irqreturn_t wacom_i2c_irq(int irq, void *dev_id)
{
	struct wacom_i2c *wac_i2c = dev_id;
	struct input_dev *input = wac_i2c->input;
	struct wacom_features *features = &wac_i2c->features;
	u8 *data = wac_i2c->data;
	unsigned int x, y, pressure;
	unsigned char tsw, f1, f2, ers;
	short tilt_x, tilt_y;
	short distance = 0;
	int error;

	error = i2c_master_recv(wac_i2c->client,
				wac_i2c->data, sizeof(wac_i2c->data));
	if (error < 0)
		goto out;

	tsw = data[3] & WACOM_TIP_SWITCH;
	ers = data[3] & WACOM_ERASER;
	f1 = data[3] & WACOM_BARREL_SWITCH;
	f2 = data[3] & WACOM_BARREL_SWITCH_2;
	x = le16_to_cpup((__le16 *)&data[4]);
	y = le16_to_cpup((__le16 *)&data[6]);
	pressure = le16_to_cpup((__le16 *)&data[8]);

	if (!wac_i2c->prox)
		wac_i2c->tool = (data[3] & (WACOM_ERASER | WACOM_INVERT)) ?
			BTN_TOOL_RUBBER : BTN_TOOL_PEN;

	wac_i2c->prox = data[3] & WACOM_IN_PROXIMITY;

	if (features->generation) {
		/* Tilt (signed) */
		tilt_x = le16_to_cpup((__le16 *)&data[11]);
		tilt_y = le16_to_cpup((__le16 *)&data[13]);
		input_report_abs(input, ABS_TILT_X, tilt_x);
		input_report_abs(input, ABS_TILT_Y, tilt_y);

		/* Hover height */
		if (data[0] == MAX_LEN_G12) {
			distance = data[10];
		} else if (data[0] >= MAX_LEN_AG14) {
			distance = le16_to_cpup((__le16 *)&data[15]);
			distance = -distance; /* The output is negative. Make it positive */
		}
		input_report_abs(input, ABS_DISTANCE, distance);
	}

	input_report_key(input, BTN_TOUCH, tsw || ers);
	input_report_key(input, wac_i2c->tool, wac_i2c->prox);
	input_report_key(input, BTN_STYLUS, f1);
	input_report_key(input, BTN_STYLUS2, f2);
	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_report_abs(input, ABS_PRESSURE, pressure);
	input_sync(input);

out:
	return IRQ_HANDLED;
}

static int wacom_i2c_open(struct input_dev *dev)
{
	struct wacom_i2c *wac_i2c = input_get_drvdata(dev);
	struct i2c_client *client = wac_i2c->client;

	enable_irq(client->irq);

	return 0;
}

static void wacom_i2c_close(struct input_dev *dev)
{
	struct wacom_i2c *wac_i2c = input_get_drvdata(dev);
	struct i2c_client *client = wac_i2c->client;

	disable_irq(client->irq);
}

#ifdef WACOM_PROBE_LEGACY
static int wacom_i2c_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
#else
static int wacom_i2c_probe(struct i2c_client *client)
#endif
{
	struct device *dev = &client->dev;
	struct wacom_i2c *wac_i2c;
	struct input_dev *input;
	struct wacom_features *features;
	int error;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "i2c_check_functionality error\n");
		return -EIO;
	}

	wac_i2c = devm_kzalloc(dev, sizeof(*wac_i2c), GFP_KERNEL);
	if (!wac_i2c)
		return -ENOMEM;

	features = &wac_i2c->features;
	error = wacom_query_device(client, features);
	if (error)
		return error;

	wac_i2c->client = client;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	wac_i2c->input = input;

	input->name = "Wacom I2C Digitizer";
	input->id.bustype = BUS_I2C;
	input->id.vendor = 0x56a;
	input->id.version = features->fw_version;
	input->dev.parent = &client->dev;
	input->open = wacom_i2c_open;
	input->close = wacom_i2c_close;

	input->evbit[0] |= BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	__set_bit(BTN_TOOL_PEN, input->keybit);
	__set_bit(BTN_TOOL_RUBBER, input->keybit);
	__set_bit(BTN_STYLUS, input->keybit);
	__set_bit(BTN_STYLUS2, input->keybit);
	__set_bit(BTN_TOUCH, input->keybit);

	input_set_abs_params(input, ABS_X, 0, features->x_max, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, features->y_max, 0, 0);
	input_set_abs_params(input, ABS_PRESSURE,
			     0, features->pressure_max, 0, 0);
	input_abs_set_res(input, ABS_X, XY_RESOLUTION);
	input_abs_set_res(input, ABS_Y, XY_RESOLUTION);

	if (features->generation & 0xff) { /* G12/G14 */
		/* Tilt X & Y property setting */
		input_set_abs_params(input, ABS_TILT_X, -features->tilt_x_max,
				     features->tilt_x_max, 0, 0);
		input_set_abs_params(input, ABS_TILT_Y, -features->tilt_y_max,
				     features->tilt_y_max, 0, 0);
		input_abs_set_res(input, ABS_TILT_X, TILT_RESOLUTION);
		input_abs_set_res(input, ABS_TILT_Y, TILT_RESOLUTION);

		/* Distance property setting follows Linux Input subsystem event */
		input_set_abs_params(input, ABS_DISTANCE, 0, features->distance_max, 0, 0);
		input_abs_set_res(input, ABS_DISTANCE, DIST_RESOLUTION);
	}

	input_set_drvdata(input, wac_i2c);

	error = devm_request_threaded_irq(dev, client->irq, NULL, wacom_i2c_irq,
					  IRQF_ONESHOT, "wacom_i2c", wac_i2c);
	if (error) {
		dev_err(dev, "Failed to request IRQ: %d\n", error);
		return error;
	}

	/* Disable the IRQ, we'll enable it in wac_i2c_open() */
	disable_irq(client->irq);

	error = input_register_device(wac_i2c->input);
	if (error) {
		dev_err(dev, "Failed to register input device: %d\n", error);
		return error;
	}

	return 0;
}

static int __maybe_unused wacom_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	disable_irq(client->irq);

	return 0;
}

static int __maybe_unused wacom_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	enable_irq(client->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(wacom_i2c_pm, wacom_i2c_suspend, wacom_i2c_resume);

static const struct i2c_device_id wacom_i2c_id[] = {
	{ "WAC_I2C_EMR", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, wacom_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id wacom_i2c_of_match_table[] = {
	 { .compatible = "emr,wacom_i2c" },
	 {}
};
MODULE_DEVICE_TABLE(of, wacom_i2c_of_match_table);
#endif

static struct i2c_driver wacom_i2c_driver = {
	.driver	= {
		.name	= "wacom_i2c",
		.pm	= &wacom_i2c_pm,

#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(wacom_i2c_of_match_table),
#endif

	},

	.probe		= wacom_i2c_probe,
	.id_table	= wacom_i2c_id,
};
module_i2c_driver(wacom_i2c_driver);

MODULE_AUTHOR("Tatsunosuke Tobita <tobita.tatsunosuke@wacom.co.jp>");
MODULE_DESCRIPTION("WACOM EMR I2C Driver");
MODULE_LICENSE("GPL");

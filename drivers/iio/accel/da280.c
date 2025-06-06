// SPDX-License-Identifier: GPL-2.0-only
/*
 * IIO driver for the MiraMEMS DA217 and DA280 3-axis accelerometer and
 * IIO driver for the MiraMEMS DA226 2-axis accelerometer
 *
 * Copyright (c) 2016 Hans de Goede <hdegoede@redhat.com>
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/byteorder/generic.h>

#define DA280_REG_CHIP_ID		0x01
#define DA280_REG_ACC_X_LSB		0x02
#define DA280_REG_ACC_Y_LSB		0x04
#define DA280_REG_ACC_Z_LSB		0x06
#define DA280_REG_MODE_BW		0x11

#define DA280_CHIP_ID			0x13
#define DA280_MODE_ENABLE		0x1e
#define DA280_MODE_DISABLE		0x9e

/*
 * a value of + or -4096 corresponds to + or - 1G
 * scale = 9.81 / 4096 = 0.002395019
 */

static const int da280_nscale = 2395019;

#define DA280_CHANNEL(reg, axis) {	\
	.type = IIO_ACCEL,	\
	.address = reg,	\
	.modified = 1,	\
	.channel2 = IIO_MOD_##axis,	\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
}

static const struct iio_chan_spec da280_channels[] = {
	DA280_CHANNEL(DA280_REG_ACC_X_LSB, X),
	DA280_CHANNEL(DA280_REG_ACC_Y_LSB, Y),
	DA280_CHANNEL(DA280_REG_ACC_Z_LSB, Z),
};

struct da280_match_data {
	const char *name;
	int num_channels;
};

struct da280_data {
	struct i2c_client *client;
};

static int da280_enable(struct i2c_client *client, bool enable)
{
	u8 data = enable ? DA280_MODE_ENABLE : DA280_MODE_DISABLE;

	return i2c_smbus_write_byte_data(client, DA280_REG_MODE_BW, data);
}

static int da280_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct da280_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = i2c_smbus_read_word_data(data->client, chan->address);
		if (ret < 0)
			return ret;
		/*
		 * Values are 14 bits, stored as 16 bits with the 2
		 * least significant bits always 0.
		 */
		*val = (short)ret >> 2;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = da280_nscale;
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info da280_info = {
	.read_raw	= da280_read_raw,
};

static void da280_disable(void *client)
{
	da280_enable(client, false);
}

static int da280_probe(struct i2c_client *client)
{
	const struct da280_match_data *match_data;
	struct iio_dev *indio_dev;
	struct da280_data *data;
	int ret;

	ret = i2c_smbus_read_byte_data(client, DA280_REG_CHIP_ID);
	if (ret != DA280_CHIP_ID)
		return (ret < 0) ? ret : -ENODEV;

	match_data = i2c_get_match_data(client);
	if (!match_data) {
		dev_err(&client->dev, "Error match-data not set\n");
		return -EINVAL;
	}

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;

	indio_dev->info = &da280_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = da280_channels;
	indio_dev->num_channels = match_data->num_channels;
	indio_dev->name = match_data->name;

	ret = da280_enable(client, true);
	if (ret < 0)
		return ret;

	ret = devm_add_action_or_reset(&client->dev, da280_disable, client);
	if (ret)
		return ret;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int da280_suspend(struct device *dev)
{
	return da280_enable(to_i2c_client(dev), false);
}

static int da280_resume(struct device *dev)
{
	return da280_enable(to_i2c_client(dev), true);
}

static DEFINE_SIMPLE_DEV_PM_OPS(da280_pm_ops, da280_suspend, da280_resume);

static const struct da280_match_data da217_match_data = { "da217", 3 };
static const struct da280_match_data da226_match_data = { "da226", 2 };
static const struct da280_match_data da280_match_data = { "da280", 3 };

static const struct acpi_device_id da280_acpi_match[] = {
	{ "NSA2513", (kernel_ulong_t)&da217_match_data },
	{ "MIRAACC", (kernel_ulong_t)&da280_match_data },
	{ }
};
MODULE_DEVICE_TABLE(acpi, da280_acpi_match);

static const struct i2c_device_id da280_i2c_id[] = {
	{ "da217", (kernel_ulong_t)&da217_match_data },
	{ "da226", (kernel_ulong_t)&da226_match_data },
	{ "da280", (kernel_ulong_t)&da280_match_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, da280_i2c_id);

static struct i2c_driver da280_driver = {
	.driver = {
		.name = "da280",
		.acpi_match_table = da280_acpi_match,
		.pm = pm_sleep_ptr(&da280_pm_ops),
	},
	.probe		= da280_probe,
	.id_table	= da280_i2c_id,
};

module_i2c_driver(da280_driver);

MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_DESCRIPTION("MiraMEMS DA280 3-Axis Accelerometer driver");
MODULE_LICENSE("GPL v2");

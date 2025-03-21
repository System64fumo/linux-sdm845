// SPDX-License-Identifier: GPL-2.0
/*
 *  MAX77705 voltage and current hwmon driver.
 *
 *  Copyright (C) 2025 Dzmitry Sankouski <dsankouski@gmail.com>
 */

#include <linux/err.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mfd/max77705-private.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct channel_desc {
	u8 reg;
	u8 avg_reg;
	const char *const label;
	// register resolution. nano Volts for voltage, nano Amperes for current
	u64 resolution;
};

static const struct channel_desc current_channel_desc[] = {
	{
		.reg = IIN_REG,
		.label = "IIN_REG",
		.resolution = 125000
	},
	{
		.reg = ISYS_REG,
		.avg_reg = AVGISYS_REG,
		.label = "ISYS_REG",
		.resolution = 312500
	}
};

static const struct channel_desc voltage_channel_desc[] = {
	{
		.reg = VBYP_REG,
		.label = "VBYP_REG",
		.resolution = 427246
	},
	{
		.reg = VSYS_REG,
		.label = "VSYS_REG",
		.resolution = 156250
	}
};

static const struct regmap_range max77705_hwmon_readable_ranges[] = {
	regmap_reg_range(AVGISYS_REG,	AVGISYS_REG + 1),
	regmap_reg_range(IIN_REG,	IIN_REG + 1),
	regmap_reg_range(ISYS_REG,	ISYS_REG + 1),
	regmap_reg_range(VBYP_REG,	VBYP_REG + 1),
	regmap_reg_range(VSYS_REG,	VSYS_REG + 1),
};

static const struct regmap_access_table max77705_hwmon_readable_table = {
	.yes_ranges = max77705_hwmon_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(max77705_hwmon_readable_ranges),
};

static const struct regmap_config max77705_hwmon_regmap_config = {
	.name = "max77705_hwmon",
	.reg_bits = 8,
	.val_bits = 16,
	.rd_table = &max77705_hwmon_readable_table,
	.max_register = MAX77705_FG_END,
	.val_format_endian = REGMAP_ENDIAN_LITTLE
};

static int max77705_read_and_convert(struct regmap *regmap, u8 reg, u64 res, long *val)
{
	int ret;
	u32 regval;

	ret = regmap_read(regmap, reg, &regval);
	if (ret < 0)
		return ret;
	*val = mult_frac((long)regval, res, 1000000);

	return 0;
}

static umode_t max77705_is_visible(const void *data,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	switch (type) {
	case hwmon_in:
		if (channel >= ARRAY_SIZE(voltage_channel_desc))
			return 0;

		switch (attr) {
		case hwmon_in_input:
		case hwmon_in_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_curr:
		if (channel >= ARRAY_SIZE(current_channel_desc))
			return 0;

		switch (attr) {
		case hwmon_curr_input:
		case hwmon_in_label:
			return 0444;
		case hwmon_curr_average:
			if (current_channel_desc[channel].avg_reg)
				return 0444;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

static int max77705_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				int channel, const char **buf)
{
	switch (type) {
	case hwmon_curr:
		switch (attr) {
		case hwmon_in_label:
			*buf = current_channel_desc[channel].label;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_label:
			*buf = voltage_channel_desc[channel].label;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int max77705_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct regmap *regmap = dev_get_drvdata(dev);
	u8 reg;
	u64 res;

	switch (type) {
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			reg = current_channel_desc[channel].reg;
			res = current_channel_desc[channel].resolution;

			return max77705_read_and_convert(regmap, reg, res, val);
		case hwmon_curr_average:
			reg = current_channel_desc[channel].avg_reg;
			res = current_channel_desc[channel].resolution;

			return max77705_read_and_convert(regmap, reg, res, val);
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			reg = voltage_channel_desc[channel].reg;
			res = voltage_channel_desc[channel].resolution;

			return max77705_read_and_convert(regmap, reg, res, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_ops max77705_hwmon_ops = {
	.is_visible = max77705_is_visible,
	.read = max77705_read,
	.read_string = max77705_read_string,
};

static const struct hwmon_channel_info *max77705_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL
			),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_AVERAGE | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL
			),
	NULL
};

static const struct hwmon_chip_info max77705_chip_info = {
	.ops = &max77705_hwmon_ops,
	.info = max77705_info,
};

static int max77705_hwmon_probe(struct platform_device *pdev)
{
	struct i2c_client *i2c;
	struct device *hwmon_dev;
	struct regmap *regmap;

	i2c = to_i2c_client(pdev->dev.parent);
	regmap = devm_regmap_init_i2c(i2c, &max77705_hwmon_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(regmap),
				"Failed to register max77705 hwmon regmap\n");

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, "max77705", regmap,
							 &max77705_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return dev_err_probe(&pdev->dev, PTR_ERR(hwmon_dev),
				"Unable to register hwmon device\n");

	return 0;
};

static struct platform_driver max77705_hwmon_driver = {
	.driver = {
		.name = "max77705-hwmon",
	},
	.probe = max77705_hwmon_probe,
};

module_platform_driver(max77705_hwmon_driver);

MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_DESCRIPTION("MAX77705 monitor driver");
MODULE_LICENSE("GPL");


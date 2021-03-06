/*
 * STMicroelectronics st_sensor_hub magnetometer driver
 *
 * Copyright 2014 STMicroelectronics Inc.
 * Copyright (C) 2016 XiaoMi, Inc.
 *
 * Denis Ciocca <denis.ciocca@st.com>
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <asm/unaligned.h>

#include "st_sensor_hub.h"

#define ST_HUB_MAGN_NUM_DATA_CH		4

static const struct iio_chan_spec st_hub_magn_ch[] = {
	ST_HUB_DEVICE_CHANNEL(IIO_MAGN, 0, 1, IIO_MOD_X, IIO_LE, 16, 16,
		BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE), 0, 0, 's'),
	ST_HUB_DEVICE_CHANNEL(IIO_MAGN, 1, 1, IIO_MOD_Y, IIO_LE, 16, 16,
		BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE), 0, 0, 's'),
	ST_HUB_DEVICE_CHANNEL(IIO_MAGN, 2, 1, IIO_MOD_Z, IIO_LE, 16, 16,
		BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE), 0, 0, 's'),
	ST_HUB_DEVICE_CHANNEL(IIO_MAGN, 3, 1, IIO_MOD_ACCURACY, IIO_LE, 8, 8,
		BIT(IIO_CHAN_INFO_RAW), 0, 0, 'u'),
	IIO_CHAN_SOFT_TIMESTAMP(4)
};

static ST_HUB_DEV_ATTR_SAMP_FREQ_AVAIL();
static ST_HUB_DEV_ATTR_SAMP_FREQ();
static ST_HUB_BATCH_MAX_EVENT_COUNT();
static ST_HUB_BATCH_BUFFER_LENGTH();
static ST_HUB_START_SELFTEST();
static ST_HUB_BATCH_TIMEOUT();
static ST_HUB_BATCH_AVAIL();
static ST_HUB_BATCH();

static ssize_t st_hub_force_magn_calibration(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int err;
	u8 command[2];
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct st_hub_pdata_info *info = indio_dev->dev.parent->platform_data;

	command[0] = ST_HUB_SINGLE_FORCE_CALIB;
	command[1] = ST_MAGN_INDEX;

	err = st_hub_send(info->hdata, command, ARRAY_SIZE(command), true);
	return err < 0 ? err : size;
}

static IIO_DEVICE_ATTR(force_magn_calibration, S_IWUSR,
					NULL, st_hub_force_magn_calibration, 0);

static void st_hub_magn_push_data(struct platform_device *pdev,
						u8 *data, int64_t timestamp)
{
	int i;
	u8 *sensor_data = data;
	size_t byte_for_channel;
	unsigned int init_copy = 0;
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct st_hub_sensor_data *mdata = iio_priv(indio_dev);

	for (i = 0; i < ST_HUB_MAGN_NUM_DATA_CH; i++) {
		byte_for_channel =
			indio_dev->channels[i].scan_type.storagebits >> 3;
		if (test_bit(i, indio_dev->active_scan_mask)) {
			memcpy(&mdata->buffer[init_copy],
						sensor_data, byte_for_channel);
			init_copy += byte_for_channel;
		}
		sensor_data += byte_for_channel;
	}

	iio_push_to_buffers_with_timestamp(indio_dev, mdata->buffer, timestamp);
}

static int st_hub_read_axis_data(struct iio_dev *indio_dev,
						unsigned int index, int *data)
{
	int err;
	u8 *outdata;
	struct st_hub_sensor_data *mdata = iio_priv(indio_dev);
	struct st_hub_pdata_info *info = indio_dev->dev.parent->platform_data;
	unsigned int byte_for_channel =
			indio_dev->channels[0].scan_type.storagebits >> 3;

	outdata = kmalloc(mdata->cdata->payload_byte, GFP_KERNEL);
	if (!outdata)
		return -EINVAL;

	err = st_hub_set_enable(info->hdata, info->index, true, true, 0, true);
	if (err < 0)
		goto st_hub_read_axis_free_memory;

	err = st_hub_read_axis_data_asincronous(info->hdata, info->index,
					outdata, mdata->cdata->payload_byte);
	if (err < 0)
		goto st_hub_read_axis_free_memory;

	err = st_hub_set_enable(info->hdata, info->index, false, true, 0, true);
	if (err < 0)
		goto st_hub_read_axis_free_memory;

	if (index == (ST_HUB_MAGN_NUM_DATA_CH - 1))
		*data = outdata[byte_for_channel * index];
	else
		*data = (s16)get_unaligned_le16(
					&outdata[byte_for_channel * index]);

st_hub_read_axis_free_memory:
	kfree(outdata);
	return err;
}

static int st_hub_magn_read_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *ch, int *val, int *val2, long mask)
{
	int err;
	struct st_hub_sensor_data *mdata = iio_priv(indio_dev);

	*val = 0;
	*val2 = 0;
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (indio_dev->currentmode == INDIO_BUFFER_TRIGGERED)
			return -EBUSY;

		err = st_hub_read_axis_data(indio_dev, ch->scan_index, val);
		if (err < 0)
			return err;

		*val = *val >> ch->scan_type.shift;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val2 = mdata->cdata->gain;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		err = -EINVAL;
		break;
	}

	return err;
}

static struct attribute *st_hub_magn_attributes[] = {
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_dev_attr_force_magn_calibration.dev_attr.attr,
	&iio_dev_attr_batch_mode_max_event_count.dev_attr.attr,
	&iio_dev_attr_batch_mode_buffer_length.dev_attr.attr,
	&iio_dev_attr_batch_mode_timeout.dev_attr.attr,
	&iio_dev_attr_batch_mode_available.dev_attr.attr,
	&iio_dev_attr_batch_mode.dev_attr.attr,
	&iio_dev_attr_selftest.dev_attr.attr,
	NULL,
};

static const struct attribute_group st_hub_magn_attribute_group = {
	.attrs = st_hub_magn_attributes,
};

static const struct iio_info st_hub_magn_info = {
	.driver_module = THIS_MODULE,
	.attrs = &st_hub_magn_attribute_group,
	.read_raw = &st_hub_magn_read_raw,
};

static const struct iio_buffer_setup_ops st_hub_buffer_setup_ops = {
	.preenable = &st_hub_buffer_preenable,
	.postenable = &st_hub_buffer_postenable,
	.predisable = &st_hub_buffer_predisable,
};

static int st_hub_magn_probe(struct platform_device *pdev)
{
	int err;
	struct iio_dev *indio_dev;
	struct st_hub_pdata_info *info;
	struct st_hub_sensor_data *mdata;
	struct st_sensor_hub_callbacks callback;

	indio_dev = iio_device_alloc(sizeof(*mdata));
	if (!indio_dev)
		return -ENOMEM;

	platform_set_drvdata(pdev, indio_dev);

	indio_dev->channels = st_hub_magn_ch;
	indio_dev->num_channels = ARRAY_SIZE(st_hub_magn_ch);
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->info = &st_hub_magn_info;
	indio_dev->name = pdev->name;
	indio_dev->modes = INDIO_DIRECT_MODE;

	mdata = iio_priv(indio_dev);
	info = pdev->dev.platform_data;
	st_hub_get_common_data(info->hdata, info->index, &mdata->cdata);

	err = st_hub_set_default_values(mdata, info, indio_dev);
	if (err < 0)
		goto st_hub_deallocate_device;

	err = iio_triggered_buffer_setup(indio_dev, NULL,
						NULL, &st_hub_buffer_setup_ops);
	if (err)
		goto st_hub_deallocate_device;

	err = st_hub_setup_trigger_sensor(indio_dev, mdata);
	if (err < 0)
		goto st_hub_clear_buffer;

	err = iio_device_register(indio_dev);
	if (err)
		goto st_hub_remove_trigger;

	callback.pdev = pdev;
	callback.push_data = &st_hub_magn_push_data;
	st_hub_register_callback(info->hdata, &callback, info->index);

	return 0;

st_hub_remove_trigger:
	st_hub_remove_trigger(mdata);
st_hub_clear_buffer:
	iio_triggered_buffer_cleanup(indio_dev);
st_hub_deallocate_device:
	iio_device_free(indio_dev);
	return err;
}

static int st_hub_magn_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct st_hub_sensor_data *mdata = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	st_hub_remove_trigger(mdata);
	iio_triggered_buffer_cleanup(indio_dev);
	iio_device_free(indio_dev);

	return 0;
}

static struct platform_device_id st_hub_magn_ids[] = {
	{ CONCATENATE_STRING(LIS331EB_DEV_NAME, "magn") },
	{ CONCATENATE_STRING(LSM6DB0_DEV_NAME, "magn") },
#ifdef CONFIG_IIO_ST_HUB_ENABLE_WAKE_LOCK_SENSORS
	{ CONCATENATE_STRING(LIS331EB_DEV_NAME, "magn_wk") },
	{ CONCATENATE_STRING(LSM6DB0_DEV_NAME, "magn_wk") },
#endif
	{},
};
MODULE_DEVICE_TABLE(platform, st_hub_magn_ids);

static struct platform_driver st_hub_magn_platform_driver = {
	.id_table = st_hub_magn_ids,
	.driver = {
		.name	= KBUILD_MODNAME,
		.owner	= THIS_MODULE,
	},
	.probe		= st_hub_magn_probe,
	.remove		= st_hub_magn_remove,
};
module_platform_driver(st_hub_magn_platform_driver);

MODULE_AUTHOR("Denis Ciocca <denis.ciocca@st.com>");
MODULE_DESCRIPTION("STMicroelectronics sensor-hub magnetometers driver");
MODULE_LICENSE("GPL v2");

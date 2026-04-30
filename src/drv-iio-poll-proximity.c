/*
 * Copyright (c) 2019 Purism SPC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"
#include "iio-buffer-utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define PROXIMITY_NEAR_LEVEL "PROXIMITY_NEAR_LEVEL"

#define PROXIMITY_WATER_MARK_LOW  0.9
#define PROXIMITY_WATER_MARK_HIGH 1.1

typedef struct DrvData {
	char               *input_attr;
	guint               timeout_id;
	GUdevDevice        *dev;
	gint                near_level;
	gint                last_level;
} DrvData;

static gboolean
poll_proximity (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	ProximityReadings readings;
	gint prox;
	gdouble near_level = drv_data->near_level;

	/* g_udev_device_get_sysfs_attr_as_int does not update when there's no event */
	prox = g_udev_device_get_sysfs_attr_as_int_uncached (drv_data->dev, drv_data->input_attr);
	/* Use a margin so we don't trigger too often */
	near_level *=  (drv_data->last_level > near_level) ? PROXIMITY_WATER_MARK_LOW : PROXIMITY_WATER_MARK_HIGH;
	readings.is_near = (prox > near_level) ? PROXIMITY_NEAR_TRUE : PROXIMITY_NEAR_FALSE;
	g_debug ("Proximity read from IIO on '%s': %d/%f, near: %d", sensor_device->name, prox, near_level, readings.is_near);
	drv_data->last_level = prox;

	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return G_SOURCE_CONTINUE;
}

static gboolean
iio_poll_proximity_discover (GUdevDevice *device)
{
	return drv_check_udev_sensor_type (device, "iio-poll-proximity", "IIO poll proximity sensor");
}

static void
iio_poll_proximity_set_polling (SensorDevice *sensor_device,
				gboolean state)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	if (drv_data->timeout_id > 0 && state)
		return;
	if (drv_data->timeout_id == 0 && !state)
		return;

	g_clear_handle_id (&drv_data->timeout_id, g_source_remove);
	if (state) {
		drv_data->timeout_id = g_timeout_add (700, poll_proximity, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[iio_poll_proximity_set_polling] poll_proximity");

		poll_proximity (sensor_device);
	}
}

static char *
get_proximity_channel_attr (GUdevDevice *device,
			      const char *suffix)
{
	const char *channels[] = {
		"in_proximity",
		"in_proximity0"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS (channels); i++) {
		g_autofree char *path = NULL, *attr = NULL;
		gboolean exists = FALSE;

		attr = g_strdup_printf ("%s_%s", channels[i], suffix);
		path = g_strdup_printf ("%s/%s", g_udev_device_get_sysfs_path (device), attr);
		exists = g_file_test (path, G_FILE_TEST_EXISTS);
		if (exists)
			return g_steal_pointer (&attr);
	}
	return NULL;
}

static gint
get_near_level (GUdevDevice *device)
{
	gint near_level;

	near_level = g_udev_device_get_property_as_int (device, PROXIMITY_NEAR_LEVEL);
	if (!near_level)
		near_level = g_udev_device_get_sysfs_attr_as_int (device, "in_proximity_nearlevel");

	if (!near_level) {
		g_warning ("Found proximity sensor but no " PROXIMITY_NEAR_LEVEL " udev property");
		g_warning ("See https://gitlab.freedesktop.org/hadess/iio-sensor-proxy/blob/master/README.md");
		return 0;
	}

	g_debug ("Near level: %d", near_level);
	return near_level;
}


static SensorDevice *
iio_poll_proximity_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;
	int near_level;

	iio_fixup_sampling_frequency (device);
	near_level = get_near_level (device);
	if (!near_level)
		return FALSE;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup (g_udev_device_get_property (device, "NAME"));
	if (!sensor_device->name)
		sensor_device->name = g_strdup (g_udev_device_get_name (device));
	sensor_device->priv = g_new0 (DrvData, 1);
	drv_data = (DrvData *) sensor_device->priv;
	drv_data->dev = g_object_ref (device);
	drv_data->near_level = near_level;
	drv_data->input_attr = get_proximity_channel_attr (device, "raw");

	if(!drv_data->input_attr)
		return NULL;

	return sensor_device;
}

static void
iio_poll_proximity_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_pointer (&drv_data->input_attr, g_free);
	g_clear_object (&drv_data->dev);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver iio_poll_proximity = {
	.driver_name = "IIO Poll proximity sensor",
	.type = DRIVER_TYPE_PROXIMITY,

	.discover = iio_poll_proximity_discover,
	.open = iio_poll_proximity_open,
	.set_polling = iio_poll_proximity_set_polling,
	.close = iio_poll_proximity_close,
};

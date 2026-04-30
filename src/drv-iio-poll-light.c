/*
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
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

#define DEFAULT_POLL_TIME 0.8

typedef struct DrvData {
	char               *input_path;
	guint               interval;
	guint               timeout_id;

	double              scale;
} DrvData;

static gboolean
iio_poll_light_discover (GUdevDevice *device)
{
	return drv_check_udev_sensor_type (device, "iio-poll-als", "IIO poll als");
}

static gboolean
light_changed (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	LightReadings readings;
	gdouble level;
	char *contents;
	g_autoptr(GError) error = NULL;

	if (g_file_get_contents (drv_data->input_path, &contents, NULL, &error)) {
		level = g_ascii_strtod (contents, NULL);
		g_free (contents);
	} else {
		g_warning ("Failed to read input level from %s at %s: %s",
			   sensor_device->name, drv_data->input_path, error->message);
		return G_SOURCE_CONTINUE;
	}
	readings.level = level * drv_data->scale;
	g_debug ("Light read from %s: %lf, (scale %lf)", sensor_device->name,
		 level, drv_data->scale);

	/* Even though the IIO kernel API declares in_intensity* values as unitless,
	 * we use Microsoft's hid-sensors-usages.docx which mentions that Windows 8
	 * compatible sensor proxies will be using Lux as the unit, and most sensors
	 * will be Windows 8 compatible */
	readings.uses_lux = TRUE;

	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return G_SOURCE_CONTINUE;
}

static char *
get_illuminance_channel_path (GUdevDevice *device,
			      const char *suffix)
{
	const char *channels[] = {
		"in_illuminance",
		"in_illuminance0",
		"in_illuminance_clear",
		"in_intensity_clear"
	};
	char *path = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS (channels); i++) {
		path = g_strdup_printf ("%s/%s_%s",
					g_udev_device_get_sysfs_path (device),
					channels[i],
					suffix);
		if (g_file_test (path, G_FILE_TEST_EXISTS))
			return path;
		g_clear_pointer (&path, g_free);
	}
	return NULL;
}

static guint
get_interval (GUdevDevice *device)
{
	gdouble time = DEFAULT_POLL_TIME;
	char *path, *contents;

	path = get_illuminance_channel_path (device, "integration_time");
	if (!path)
		goto out;
	if (g_file_get_contents (path, &contents, NULL, NULL)) {
		time = g_ascii_strtod (contents, NULL);
		g_free (contents);
	}
	g_free (path);

out:
	return (time * 1000);
}

static void
iio_poll_light_set_polling (SensorDevice *sensor_device,
			    gboolean state)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	if (drv_data->timeout_id > 0 && state)
		return;
	if (drv_data->timeout_id == 0 && !state)
		return;

	if (drv_data->timeout_id) {
		g_source_remove (drv_data->timeout_id);
		drv_data->timeout_id = 0;
	}

	if (state) {
		drv_data->timeout_id = g_timeout_add (drv_data->interval,
						      (GSourceFunc) light_changed,
						      sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[iio_poll_light_set_polling] light_changed");

		light_changed (sensor_device);
	}
}

static SensorDevice *
iio_poll_light_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;
	g_autofree char *input_path = NULL;

	iio_fixup_sampling_frequency (device);

	input_path = get_illuminance_channel_path (device, "input");
	if (!input_path)
		input_path = get_illuminance_channel_path (device, "raw");
	if (!input_path)
		return NULL;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup (g_udev_device_get_property (device, "NAME"));
	if (!sensor_device->name)
		sensor_device->name = g_strdup (g_udev_device_get_name (device));
	sensor_device->priv = g_new0 (DrvData, 1);
	drv_data = (DrvData *) sensor_device->priv;

	drv_data->interval = get_interval (device);

	if (g_str_has_prefix (input_path, "in_illuminance0")) {
		drv_data->scale = g_udev_device_get_sysfs_attr_as_double (device,
									  "in_illuminance0_scale");
	} else {
		drv_data->scale = g_udev_device_get_sysfs_attr_as_double (device,
									  "in_illuminance_scale");
	}
	if (drv_data->scale == 0.0)
		drv_data->scale = 1.0;

	drv_data->input_path = g_steal_pointer (&input_path);

	return sensor_device;
}

static void
iio_poll_light_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_pointer (&drv_data->input_path, g_free);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver iio_poll_light = {
	.driver_name = "IIO Polling Light sensor",
	.type = DRIVER_TYPE_LIGHT,

	.discover = iio_poll_light_discover,
	.open = iio_poll_light_open,
	.set_polling = iio_poll_light_set_polling,
	.close = iio_poll_light_close,
};

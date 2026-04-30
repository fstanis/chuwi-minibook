/*
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include "utils.h"

#define DEFAULT_POLL_TIME (IS_TEST ? 500 : 8000)
#define MAX_LIGHT_LEVEL   255

typedef struct DrvData {
	GUdevDevice        *device;
	guint               timeout_id;
} DrvData;

static gboolean
hwmon_light_discover (GUdevDevice *device)
{
	return drv_check_udev_sensor_type (device, "hwmon-als", "HWMon light");
}

static gboolean
light_changed (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	LightReadings readings;
	gdouble level;
	const char *contents;
	int light1, light2;

	contents = g_udev_device_get_sysfs_attr_uncached (drv_data->device, "light");
	if (!contents)
		return G_SOURCE_CONTINUE;

	if (sscanf (contents, "(%d,%d)", &light1, &light2) != 2) {
		g_warning ("Failed to parse light level: %s", contents);
		return G_SOURCE_CONTINUE;
	}
	level = (double) (((float) MAX(light1, light2)) / (float) MAX_LIGHT_LEVEL * 100.0f);

	readings.level = level;
	readings.uses_lux = FALSE;
	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return G_SOURCE_CONTINUE;
}

static SensorDevice *
hwmon_light_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup (g_udev_device_get_name (device));
	sensor_device->priv = g_new0 (DrvData, 1);
	drv_data = (DrvData *) sensor_device->priv;

	drv_data->device = g_object_ref (device);

	return sensor_device;
}

static void
hwmon_light_set_polling (SensorDevice *sensor_device,
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
		drv_data->timeout_id = g_timeout_add (DEFAULT_POLL_TIME, (GSourceFunc) light_changed, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[hwmon_light_set_polling] light_changed");

		/* And send a reading straight away */
		light_changed (sensor_device);
	}
}

static void
hwmon_light_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_object (&drv_data->device);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver hwmon_light = {
	.driver_name = "Platform HWMon Light",
	.type = DRIVER_TYPE_LIGHT,

	.discover = hwmon_light_discover,
	.open = hwmon_light_open,
	.set_polling = hwmon_light_set_polling,
	.close = hwmon_light_close,
};

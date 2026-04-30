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

#include <linux/input.h>

typedef struct DrvData {
	guint              timeout_id;
} DrvData;

static gboolean
fake_light_discover (GUdevDevice *device)
{
	if (g_getenv ("FAKE_LIGHT_SENSOR") == NULL)
		return FALSE;

	/* We need a udev device to associate with our fake light sensor,
	 * and the power button is as good as any, and should be available
	 * on most devices we want to run this on. */
	if (g_strcmp0 (g_udev_device_get_subsystem (device), "input") != 0 ||
	    g_strcmp0 (g_udev_device_get_property (device, "NAME"), "\"Power Button\"") != 0)
		return FALSE;

	g_debug ("Found fake light at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

static gboolean
light_changed (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	static gdouble level = -1.0;
	LightReadings readings;

	/* XXX:
	 * Might need to do something better here, like
	 * replicate real readings from a device */
	level += 1.0;
	readings.level = level;
	readings.uses_lux = TRUE;
	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return G_SOURCE_CONTINUE;
}

static SensorDevice *
fake_light_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->priv = g_new0 (DrvData, 1);
	sensor_device->name = g_strdup ("Fake Light Sensor");

	return sensor_device;
}

static void
fake_light_set_polling (SensorDevice *sensor_device,
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
		drv_data->timeout_id = g_timeout_add_seconds (1, (GSourceFunc) light_changed, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[fake_light_set_polling] light_changed");

		light_changed (sensor_device);
	}
}

static void
fake_light_close (SensorDevice *sensor_device)
{
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver fake_light = {
	.driver_name = "Fake light",
	.type = DRIVER_TYPE_LIGHT,

	.discover = fake_light_discover,
	.open = fake_light_open,
	.set_polling = fake_light_set_polling,
	.close = fake_light_close,
};

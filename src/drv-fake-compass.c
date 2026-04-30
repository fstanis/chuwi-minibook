/*
 * Copyright (c) 2015 Bastien Nocera <hadess@hadess.net>
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
fake_compass_discover (GUdevDevice *device)
{
	if (g_getenv ("FAKE_COMPASS") == NULL)
		return FALSE;

	if (g_strcmp0 (g_udev_device_get_subsystem (device), "input") != 0)
		return FALSE;

	/* "Power Button" is a random input device to latch onto */
	if (g_strcmp0 (g_udev_device_get_property (device, "NAME"), "\"Power Button\"") != 0)
		return FALSE;

	g_debug ("Found fake compass at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

static gboolean
compass_changed (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	static gdouble heading = 0;
	CompassReadings readings;

	heading += 10;
	if (heading >= 360)
		heading = 0;
	g_debug ("Changed heading to %f", heading);
	readings.heading = heading;

	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return G_SOURCE_CONTINUE;
}

static SensorDevice *
fake_compass_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->priv = g_new0 (DrvData, 1);
	sensor_device->name = g_strdup ("Fake Compass");

	return sensor_device;
}

static void
fake_compass_set_polling (SensorDevice *sensor_device,
			  gboolean      state)
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
		drv_data->timeout_id = g_timeout_add_seconds (1, (GSourceFunc) compass_changed, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[fake_compass_set_polling] compass_changed");

		compass_changed (sensor_device);
	}
}

static void
fake_compass_close (SensorDevice *sensor_device)
{
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver fake_compass = {
	.driver_name = "Fake compass",
	.type = DRIVER_TYPE_COMPASS,

	.discover = fake_compass_discover,
	.open = fake_compass_open,
	.set_polling = fake_compass_set_polling,
	.close = fake_compass_close,
};

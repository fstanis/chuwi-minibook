/*
 * Copyright (c) 2023-2025 Dylan Van Assche
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
#include <stdlib.h>
#include <gio/gio.h>
#include <libssc-sensor.h>
#include <libssc-sensor-compass.h>

typedef struct DrvData {
	SSCSensorCompass *sensor;
	gulong measurement_id;
} DrvData;

static gboolean
ssc_compass_discover (GUdevDevice *device)
{
	g_autoptr(SSCSensorCompass) sensor = NULL;

	/* Verify presence of FastRPC device */
	if (!drv_check_udev_sensor_type (device, "ssc-compass", NULL))
		return FALSE;

	/* Open and close SSC compass for discovering */
	sensor = ssc_sensor_compass_new_sync (NULL, NULL);
	if (!sensor)
		return FALSE;

	if (!ssc_sensor_compass_close_sync (sensor, NULL, NULL))
		return FALSE;

	g_debug ("Found SSC compass at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

static void
measurement_cb (SSCSensorCompass *sensor, gfloat azimuth, gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	CompassReadings readings;

	readings.heading = azimuth;

	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);
}

static SensorDevice *
ssc_compass_open (GUdevDevice *device)
{
	g_autoptr(GError) error = NULL;
	SensorDevice *sensor_device;
	DrvData *drv_data;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->priv = g_new0 (DrvData, 1);

	drv_data = (DrvData *) sensor_device->priv;
	drv_data->sensor = ssc_sensor_compass_new_sync (NULL, &error);
	if (!drv_data->sensor)
		g_warning ("Creating SSC compass sensor failed: %s", error->message);
	else {
	  g_object_get (drv_data->sensor,
			SSC_SENSOR_NAME, &sensor_device->name,
			NULL);
	}

	return sensor_device;
}

static void
ssc_compass_set_polling (SensorDevice *sensor_device, gboolean state)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	g_autoptr(GError) error = NULL;

	if (state) {
		if (drv_data->measurement_id)
			return;
		/* Start listening for measurements */
		drv_data->measurement_id = g_signal_connect (drv_data->sensor,
					                     "measurement",
							     G_CALLBACK (measurement_cb),
							     sensor_device);

		/* Enable sensor */
		if (!ssc_sensor_compass_open_sync (drv_data->sensor, NULL, &error))
			g_warning ("Opening SSC compass sensor failed: %s", error->message);
	} else {
		if (!drv_data->measurement_id)
			return;

		/* Stop listening for measurements */
		g_clear_signal_handler (&drv_data->measurement_id, drv_data->sensor);

		/* Disable sensor */
		if (!ssc_sensor_compass_close_sync (drv_data->sensor, NULL, &error))
			g_warning ("Closing SSC compass sensor failed: %s", error->message);
	}
}

static void
ssc_compass_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_object (&drv_data->sensor);
	g_clear_pointer (&sensor_device->name, g_free);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver ssc_compass = {
	.driver_name = "SSC compass sensor",
	.type = DRIVER_TYPE_COMPASS,

	.discover = ssc_compass_discover,
	.open = ssc_compass_open,
	.set_polling = ssc_compass_set_polling,
	.close = ssc_compass_close,
};

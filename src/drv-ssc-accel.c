/*
 * Copyright (c) 2023-2025 Dylan Van Assche
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"
#include "accel-mount-matrix.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <libssc-sensor.h>
#include <libssc-sensor-accelerometer.h>

typedef struct DrvData {
	SSCSensorAccelerometer *sensor;
	gulong measurement_id;
	AccelVec3 *mount_matrix;
	AccelLocation location;
	AccelScale scale;
} DrvData;

static gboolean
ssc_accelerometer_discover (GUdevDevice *device)
{
	g_autoptr(SSCSensorAccelerometer) sensor = NULL;

	/* Verify presence of FastRPC device */
	if (!drv_check_udev_sensor_type (device, "ssc-accel", NULL))
		return FALSE;

	/* Open and close SSC accelerometer for discovering */
	sensor = ssc_sensor_accelerometer_new_sync (NULL, NULL);
	if (!sensor)
		return FALSE;

	if (!ssc_sensor_accelerometer_close_sync (sensor, NULL, NULL))
		return FALSE;

	g_debug ("Found SSC accelerometer at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

static void
measurement_cb (SSCSensorAccelerometer *sensor, gfloat accel_x, gfloat accel_y, gfloat accel_z, gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	AccelReadings readings;
	AccelVec3 tmp;

	tmp.x = accel_x;
	tmp.y = accel_y;
	tmp.z = accel_z;

	if (!apply_mount_matrix (drv_data->mount_matrix, &tmp))
		g_warning ("Could not apply mount matrix");

	readings.accel_x = tmp.x;
	readings.accel_y = tmp.y;
	readings.accel_z = tmp.z;
	copy_accel_scale (&readings.scale, drv_data->scale);

	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);
}

static SensorDevice *
ssc_accelerometer_open (GUdevDevice *device)
{
	g_autoptr(GError) error = NULL;
	SensorDevice *sensor_device;
	DrvData *drv_data;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->priv = g_new0 (DrvData, 1);

	drv_data = (DrvData *) sensor_device->priv;
	drv_data->sensor = ssc_sensor_accelerometer_new_sync (NULL, &error);
	if (!drv_data->sensor)
		g_warning ("Creating SSC accelerometer sensor failed: %s", error->message);
	else
		g_object_get (drv_data->sensor,
		              SSC_SENSOR_NAME, &sensor_device->name,
		              NULL);

	/* Setup accel attributes */
	drv_data->mount_matrix = setup_mount_matrix (device);
	drv_data->location = setup_accel_location (device);
	set_accel_scale (&drv_data->scale, 1.0);

	return sensor_device;
}

static void
ssc_accelerometer_set_polling (SensorDevice *sensor_device, gboolean state)
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
		if (!ssc_sensor_accelerometer_open_sync (drv_data->sensor, NULL, &error)) {
			g_warning ("Opening SSC accelerometer sensor failed: %s", error->message);
			return;
		}
	} else {
		if (!drv_data->measurement_id)
			return;

		/* Stop listening for measurements */
		g_clear_signal_handler (&drv_data->measurement_id, drv_data->sensor);

		/* Disable sensor */
		if (!ssc_sensor_accelerometer_close_sync (drv_data->sensor, NULL, &error))
			g_warning ("Closing SSC accelerometer sensor failed: %s", error->message);
	}
}

static void
ssc_accelerometer_close (SensorDevice *sensor_device)
{
	g_autoptr(GError) error = NULL;
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_object (&drv_data->sensor);
	g_clear_pointer (&drv_data->mount_matrix, g_free);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver ssc_accel = {
	.driver_name = "SSC accelerometer sensor",
	.type = DRIVER_TYPE_ACCEL,

	.discover = ssc_accelerometer_discover,
	.open = ssc_accelerometer_open,
	.set_polling = ssc_accelerometer_set_polling,
	.close = ssc_accelerometer_close,
};

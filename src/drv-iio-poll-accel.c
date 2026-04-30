/*
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"
#include "iio-buffer-utils.h"
#include "accel-mount-matrix.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct DrvData {
	guint               timeout_id;
	GUdevDevice        *dev;
	AccelVec3          *mount_matrix;
	AccelLocation       location;
	AccelScale          scale;
} DrvData;

static gboolean
poll_orientation (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	int accel_x, accel_y, accel_z;
	AccelReadings readings;
	AccelVec3 tmp;

	accel_x = g_udev_device_get_sysfs_attr_as_int_uncached (drv_data->dev, "in_accel_x_raw");
	accel_y = g_udev_device_get_sysfs_attr_as_int_uncached (drv_data->dev, "in_accel_y_raw");
	accel_z = g_udev_device_get_sysfs_attr_as_int_uncached (drv_data->dev, "in_accel_z_raw");
	copy_accel_scale (&readings.scale, drv_data->scale);

	g_debug ("Accel read from IIO on '%s': %d, %d, %d (scale %lf,%lf,%lf)", sensor_device->name,
		 accel_x, accel_y, accel_z,
		 drv_data->scale.x, drv_data->scale.y, drv_data->scale.z);

	tmp.x = accel_x;
	tmp.y = accel_y;
	tmp.z = accel_z;

	if (!apply_mount_matrix (drv_data->mount_matrix, &tmp))
		g_warning ("Could not apply mount matrix");

	//FIXME report errors
	readings.accel_x = tmp.x;
	readings.accel_y = tmp.y;
	readings.accel_z = tmp.z;

	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return G_SOURCE_CONTINUE;
}

static gboolean
iio_poll_accel_discover (GUdevDevice *device)
{
	if (!drv_check_udev_sensor_type (device, "iio-poll-accel", NULL))
		return FALSE;

	g_debug ("Found IIO poll accelerometer at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

static void
iio_poll_accel_set_polling (SensorDevice *sensor_device,
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
		drv_data->timeout_id = g_timeout_add (700, poll_orientation, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[iio_poll_accel_set_polling] poll_orientation");

		poll_orientation (sensor_device);
	}
}

static SensorDevice *
iio_poll_accel_open (GUdevDevice *device)
{

	SensorDevice *sensor_device;
	DrvData *drv_data;

	iio_fixup_sampling_frequency (device);

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup (g_udev_device_get_property (device, "NAME"));
	if (!sensor_device->name)
		sensor_device->name = g_strdup (g_udev_device_get_name (device));
	sensor_device->priv = g_new0 (DrvData, 1);
	drv_data = (DrvData *) sensor_device->priv;
	drv_data->dev = g_object_ref (device);
	drv_data->mount_matrix = setup_mount_matrix (device);
	drv_data->location = setup_accel_location (device);
	if (!get_accel_scale (device, &drv_data->scale))
		reset_accel_scale (&drv_data->scale);

	return sensor_device;
}

static void
iio_poll_accel_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_object (&drv_data->dev);
	g_clear_pointer (&drv_data->mount_matrix, g_free);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver iio_poll_accel = {
	.driver_name = "IIO Poll accelerometer",
	.type = DRIVER_TYPE_ACCEL,

	.discover = iio_poll_accel_discover,
	.open = iio_poll_accel_open,
	.set_polling = iio_poll_accel_set_polling,
	.close = iio_poll_accel_close,
};

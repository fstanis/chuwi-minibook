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
#include "utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

typedef struct {
	guint              timeout_id;

	GUdevDevice *dev;
	char *dev_path;
	AccelVec3 *mount_matrix;
	AccelLocation location;
	int device_id;
	BufferDrvData *buffer_data;
} DrvData;

static int
process_scan (IIOSensorData data, SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	int i;
	int accel_x, accel_y, accel_z;
	gboolean present_x, present_y, present_z;
	AccelReadings readings;
	AccelVec3 tmp;
	AccelScale scale;

	if (data.read_size < 0) {
		g_warning ("Couldn't read from device '%s': %s", sensor_device->name, g_strerror (errno));
		return 0;
	}

	/* Rather than read everything:
	 * for (i = 0; i < data.read_size / drv_data->scan_size; i++)...
	 * Just read the last one */
	i = (data.read_size / drv_data->buffer_data->scan_size) - 1;
	if (i < 0) {
		g_debug ("Not enough data to read from '%s' (read_size: %d scan_size: %d)", sensor_device->name,
			 (int) data.read_size, drv_data->buffer_data->scan_size);
		return 0;
	}

	process_scan_1(data.data + drv_data->buffer_data->scan_size*i, drv_data->buffer_data, "in_accel_x", &accel_x, &scale.x, &present_x);
	process_scan_1(data.data + drv_data->buffer_data->scan_size*i, drv_data->buffer_data, "in_accel_y", &accel_y, &scale.y, &present_y);
	process_scan_1(data.data + drv_data->buffer_data->scan_size*i, drv_data->buffer_data, "in_accel_z", &accel_z, &scale.z, &present_z);

	g_debug ("Accel read from IIO on '%s': %d, %d, %d (scale %lf,%lf,%lf)", sensor_device->name,
		 accel_x, accel_y, accel_z,
		 scale.x, scale.y, scale.z);

	tmp.x = accel_x;
	tmp.y = accel_y;
	tmp.z = accel_z;

	if (!apply_mount_matrix (drv_data->mount_matrix, &tmp))
		g_warning ("Could not apply mount matrix");

	//FIXME report errors
	readings.accel_x = tmp.x;
	readings.accel_y = tmp.y;
	readings.accel_z = tmp.z;
	copy_accel_scale (&readings.scale, scale);
	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return 1;
}

static void
prepare_output (SensorDevice *sensor_device,
		const char   *dev_dir_name,
		const char   *trigger_name)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	g_autoptr(IIOSensorData) data = NULL;
	g_auto(IioFd) fp = -1;

	int buf_len = 127;

	data = iio_sensor_data_new (drv_data->buffer_data->scan_size * buf_len);

	/* Attempt to open non blocking to access dev */
	fp = open (drv_data->dev_path, O_RDONLY | O_NONBLOCK);
	if (fp == -1) { /* If it isn't there make the node */
		if (!IS_TEST)
			g_warning ("Failed to open '%s' at %s: %s", sensor_device->name, drv_data->dev_path, g_strerror (errno));
		return;
	}

	/* Actually read the data */
	data->read_size = read (fp, data->data, buf_len * drv_data->buffer_data->scan_size);
	if (data->read_size == -1 && errno == EAGAIN) {
		g_debug ("No new data available on '%s'", sensor_device->name);
		return;
	}

	process_scan (*data, sensor_device);
}

static gboolean
read_orientation (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	prepare_output (sensor_device, drv_data->buffer_data->dev_dir_name, drv_data->buffer_data->trigger_name);

	return G_SOURCE_CONTINUE;
}

static void
iio_buffer_accel_set_polling (SensorDevice *sensor_device,
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
		disable_ring_buffer (drv_data->buffer_data);
	}

	if (state) {
		enable_ring_buffer (drv_data->buffer_data);
		drv_data->timeout_id = g_timeout_add (700, read_orientation, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[iio_buffer_accel_set_polling] read_orientation");

		read_orientation (sensor_device);
	}
}

static SensorDevice *
iio_buffer_accel_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;
	BufferDrvData *buffer_data;

	buffer_data = buffer_drv_data_new (device);
	if (!buffer_data)
		return NULL;

	/* Disable the sensor buffer until claimed */
	disable_ring_buffer (buffer_data);

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup (g_udev_device_get_property (device, "NAME"));
	if (!sensor_device->name)
		sensor_device->name = g_strdup (g_udev_device_get_name (device));
	sensor_device->priv = g_new0 (DrvData, 1);
	drv_data = (DrvData *) sensor_device->priv;
	drv_data->buffer_data = buffer_data;
	drv_data->mount_matrix = setup_mount_matrix (device);
	drv_data->location = setup_accel_location (device);
	drv_data->dev = g_object_ref (device);
	drv_data->dev_path = get_device_file (device);

	return sensor_device;
}

static void
iio_buffer_accel_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_pointer (&drv_data->buffer_data, buffer_drv_data_free);
	g_clear_object (&drv_data->dev);
	g_clear_pointer (&drv_data->mount_matrix, g_free);
	g_clear_pointer (&drv_data->dev_path, g_free);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

static gboolean
iio_buffer_accel_discover (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;
	gboolean buffer_usable = FALSE;

	if (!drv_check_udev_sensor_type (device, "iio-buffer-accel", NULL))
		return FALSE;

	sensor_device = iio_buffer_accel_open (device);
	if (!sensor_device)
		return FALSE;

	/* Attempt to read from the sensor */
	drv_data = (DrvData *) sensor_device->priv;
	enable_ring_buffer (drv_data->buffer_data);

	buffer_usable = is_buffer_usable(drv_data->dev_path);

	/* Close the sensor until it has been claimed */
	disable_ring_buffer (drv_data->buffer_data);
	iio_buffer_accel_close (sensor_device);

	if (!buffer_usable)
		 return FALSE;

	g_debug ("Found IIO buffer accelerometer at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

SensorDriver iio_buffer_accel = {
	.driver_name = "IIO Buffer accelerometer",
	.type = DRIVER_TYPE_ACCEL,

	.discover = iio_buffer_accel_discover,
	.open = iio_buffer_accel_open,
	.set_polling = iio_buffer_accel_set_polling,
	.close = iio_buffer_accel_close,
};

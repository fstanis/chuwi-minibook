/*
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
 * Copyright (c) 2015 Elad Alfassa <elad@fedoraproject.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"
#include "iio-buffer-utils.h"
#include "utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

typedef struct {
	guint              timeout_id;

	GUdevDevice *dev;
	char *dev_path;
	int device_id;
	BufferDrvData *buffer_data;
} DrvData;

static int
process_scan (IIOSensorData data, SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	int i;
	int level = 0;
	gdouble scale;
	gboolean present_level;
	LightReadings readings;

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

	process_scan_1(data.data + drv_data->buffer_data->scan_size*i, drv_data->buffer_data, "in_intensity_both", &level, &scale, &present_level);

	g_debug ("Light read from IIO on '%s': %d (scale %lf) = %lf", sensor_device->name, level, scale, level * scale);
	readings.level = level * scale;

	/* Even though the IIO kernel API declares in_intensity* values as unitless,
	 * we use Microsoft's hid-sensors-usages.docx which mentions that Windows 8
	 * compatible sensor proxies will be using Lux as the unit, and most sensors
	 * will be Windows 8 compatible */
	readings.uses_lux = TRUE;

	//FIXME report errors
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
	g_auto(IioFd) fp = 1;

	int buf_len = 127;

	data = iio_sensor_data_new (drv_data->buffer_data->scan_size * buf_len);

	/* Attempt to open non blocking to access dev */
	fp = open (drv_data->dev_path, O_RDONLY | O_NONBLOCK);
	if (fp == -1) { /* If it isn't there make the node */
		g_warning ("Failed to open '%s' at %s : %s", sensor_device->name, drv_data->dev_path, g_strerror (errno));
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
read_light (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	prepare_output (sensor_device, drv_data->buffer_data->dev_dir_name, drv_data->buffer_data->trigger_name);

	return G_SOURCE_CONTINUE;
}

static void
iio_buffer_light_set_polling (SensorDevice *sensor_device,
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
		drv_data->timeout_id = g_timeout_add (700, read_light, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[iio_buffer_light_set_polling] read_light");

		read_light (sensor_device);
	}
}

static SensorDevice *
iio_buffer_light_open (GUdevDevice *device)
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
	drv_data->dev = g_object_ref (device);
	drv_data->buffer_data = buffer_data;
	drv_data->dev_path = get_device_file (device);

	return sensor_device;
}

static void
iio_buffer_light_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_pointer (&drv_data->buffer_data, buffer_drv_data_free);
	g_clear_object (&drv_data->dev);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

static gboolean
iio_buffer_light_discover (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;
	gboolean buffer_usable = FALSE;

	if (!drv_check_udev_sensor_type (device, "iio-buffer-als", NULL))
		return FALSE;

	sensor_device = iio_buffer_light_open (device);
	if (!sensor_device)
		return FALSE;

	/* Attempt to read from the sensor */
	drv_data = (DrvData *) sensor_device->priv;
	enable_ring_buffer (drv_data->buffer_data);

	buffer_usable = is_buffer_usable(drv_data->dev_path);

	/* Close the sensor until it has been claimed */
	disable_ring_buffer (drv_data->buffer_data);
	iio_buffer_light_close (sensor_device);

	if (!buffer_usable)
		return FALSE;

	g_debug ("Found IIO buffer ALS at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

SensorDriver iio_buffer_light = {
	.driver_name = "IIO Buffer Light sensor",
	.type = DRIVER_TYPE_LIGHT,

	.discover = iio_buffer_light_discover,
	.open = iio_buffer_light_open,
	.set_polling = iio_buffer_light_set_polling,
	.close = iio_buffer_light_close,
};

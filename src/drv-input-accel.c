/*
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"
#include "accel-mount-matrix.h"
#include "utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <linux/input.h>

typedef struct DrvData {
	guint              timeout_id;

	GUdevClient *client;
	GUdevDevice *dev, *parent;
	const char *dev_path;
	AccelVec3 *mount_matrix;
	AccelLocation location;
	gboolean sends_kevent;
} DrvData;

static void input_accel_set_polling (SensorDevice *sensor_device,
				     gboolean state);

/* From src/linux/up-device-supply.c in UPower */
static GUdevDevice *
get_sibling_with_subsystem (GUdevDevice *device,
			    const char *subsystem)
{
	GUdevDevice *parent;
	GUdevClient *client;
	GUdevDevice *sibling;
	const char * class[] = { NULL, NULL };
	const char *parent_path;
	GList *devices, *l;

	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (subsystem != NULL, NULL);

	parent = g_udev_device_get_parent (device);
	if (!parent)
		return NULL;
	parent_path = g_udev_device_get_sysfs_path (parent);

	sibling = NULL;
	class[0] = subsystem;
	client = g_udev_client_new (class);
	devices = g_udev_client_query_by_subsystem (client, subsystem);
	for (l = devices; l != NULL && sibling == NULL; l = l->next) {
		GUdevDevice *d = l->data;
		GUdevDevice *p;
		const char *p_path;

		p = g_udev_device_get_parent (d);
		if (!p)
			continue;
		p_path = g_udev_device_get_sysfs_path (p);
		if (g_strcmp0 (p_path, parent_path) == 0)
			sibling = g_object_ref (d);

		g_object_unref (p);
	}

	g_list_free_full (devices, (GDestroyNotify) g_object_unref);
	g_object_unref (client);
	g_object_unref (parent);

	return sibling;
}

static gboolean
is_part_of_joypad (GUdevDevice *device)
{
	g_autoptr(GUdevDevice) sibling;

	sibling = get_sibling_with_subsystem (device, "input");
	if (!sibling)
		return FALSE;
	return g_udev_device_get_property_as_boolean (sibling, "ID_INPUT_JOYSTICK");
}

static gboolean
input_accel_discover (GUdevDevice *device)
{
	const char *path;
	g_autoptr(GUdevDevice) parent = NULL;

	if (!drv_check_udev_sensor_type (device, "input-accel", NULL))
		return FALSE;

	path = g_udev_device_get_device_file (device);
	if (!path)
		return FALSE;
	if (strstr (path, "/event") == NULL)
		return FALSE;

	parent = g_udev_device_get_parent (device);
	if (parent && is_part_of_joypad (parent))
		return FALSE;

	g_debug ("Found input accel at %s", g_udev_device_get_sysfs_path (device));
	return TRUE;
}

#define READ_AXIS(axis, var) { memzero(&abs_info, sizeof(abs_info)); r = ioctl(fd, EVIOCGABS(axis), &abs_info); if (r < 0) return; var = abs_info.value; }
#define memzero(x,l) (memset((x), 0, (l)))

static void
accelerometer_changed (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	struct input_absinfo abs_info;
	int accel_x = 0, accel_y = 0, accel_z = 0;
	g_auto(IioFd) fd = -1;
	int r;
	AccelReadings readings;
	AccelVec3 tmp;

	fd = open (drv_data->dev_path, O_RDONLY|O_CLOEXEC);
	if (fd < 0) {
		g_warning ("Could not open input accel '%s': %s",
			   drv_data->dev_path, g_strerror (errno));
		return;
	}

	READ_AXIS(ABS_X, accel_x);
	READ_AXIS(ABS_Y, accel_y);
	READ_AXIS(ABS_Z, accel_z);

	/* Scale from 1G ~= 256 to a value in m/s² */
	set_accel_scale (&readings.scale, 1.0 / 256 * 9.81);

	g_debug ("Accel read from input on '%s': %d, %d, %d (scale %lf,%lf,%lf)", sensor_device->name,
		 accel_x, accel_y, accel_z,
		 readings.scale.x, readings.scale.y, readings.scale.z);

	tmp.x = accel_x;
	tmp.y = accel_y;
	tmp.z = accel_z;

	if (!apply_mount_matrix (drv_data->mount_matrix, &tmp))
		g_warning ("Could not apply mount matrix");

	readings.accel_x = tmp.x;
	readings.accel_y = tmp.y;
	readings.accel_z = tmp.z;

	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);
}

static void
uevent_received (GUdevClient *client,
		 gchar       *action,
		 GUdevDevice *device,
		 gpointer     user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	if (g_strcmp0 (action, "change") != 0)
		return;

	if (g_strcmp0 (g_udev_device_get_sysfs_path (device), g_udev_device_get_sysfs_path (drv_data->parent)) != 0)
		return;

	if (!drv_data->sends_kevent) {
		drv_data->sends_kevent = TRUE;
		g_debug ("Received kevent, let's stop polling for accelerometer data on %s", drv_data->dev_path);
		input_accel_set_polling (sensor_device, FALSE);
	}

	accelerometer_changed (sensor_device);
}

static SensorDevice *
input_accel_open (GUdevDevice *device)
{
	const gchar * const subsystems[] = { "input", NULL };
	SensorDevice *sensor_device;
	DrvData *drv_data;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup (g_udev_device_get_property (device, "NAME"));
	if (!sensor_device->name)
		sensor_device->name = g_strdup (g_udev_device_get_name (device));
	if (!sensor_device->name)
		sensor_device->name = g_strdup (g_udev_device_get_property (device, "ID_MODEL"));
	if (!sensor_device->name) {
		g_autoptr(GUdevDevice) parent = NULL;

		parent = g_udev_device_get_parent (device);
		sensor_device->name = g_strdup (g_udev_device_get_property (parent, "NAME"));
	}

	sensor_device->priv = g_new0 (DrvData, 1);
	drv_data = (DrvData *) sensor_device->priv;
	drv_data->dev = g_object_ref (device);
	drv_data->parent = g_udev_device_get_parent (drv_data->dev);
	drv_data->dev_path = g_udev_device_get_device_file (device);
	drv_data->client = g_udev_client_new (subsystems);
	drv_data->mount_matrix = setup_mount_matrix (device);
	drv_data->location = setup_accel_location (device);

	g_signal_connect (drv_data->client, "uevent",
			  G_CALLBACK (uevent_received), sensor_device);

	return sensor_device;
}

static gboolean
read_accel_poll (gpointer user_data)
{
	accelerometer_changed (user_data);
	return G_SOURCE_CONTINUE;
}

static void
input_accel_set_polling (SensorDevice *sensor_device,
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

	if (state && !drv_data->sends_kevent) {
		drv_data->timeout_id = g_timeout_add (700, read_accel_poll, sensor_device);
		g_source_set_name_by_id (drv_data->timeout_id, "[input_accel_set_polling] read_accel_poll");

		read_accel_poll (sensor_device);
	}
}

static void
input_accel_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_object (&drv_data->client);
	g_clear_object (&drv_data->dev);
	g_clear_object (&drv_data->parent);
	g_clear_pointer (&drv_data->mount_matrix, g_free);

	g_clear_pointer (&sensor_device->priv, g_free);
}

SensorDriver input_accel = {
	.driver_name = "Input accelerometer",
	.type = DRIVER_TYPE_ACCEL,

	.discover = input_accel_discover,
	.open = input_accel_open,
	.set_polling = input_accel_set_polling,
	.close = input_accel_close,
};

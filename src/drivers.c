/*
 * Copyright (c) 2020 Purism SPC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"
#include <gudev/gudev.h>

gboolean drv_check_udev_sensor_type (GUdevDevice *device,
				     const gchar *match,
				     const gchar *name)
{
	g_auto (GStrv) types = NULL;
	const gchar *attr = g_udev_device_get_property (device, "IIO_SENSOR_PROXY_TYPE");

	if (attr == NULL)
		return FALSE;

	types = g_strsplit (attr, " ", 0);
	if (!g_strv_contains ((const gchar * const *)types, match))
		return FALSE;

	if (name)
		g_debug ("Found %s at %s", name, g_udev_device_get_sysfs_path (device));
	return TRUE;
}

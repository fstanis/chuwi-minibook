/*
 * Copyright (c) 2019 Luís Ferreira <luis@aurorafoss.org>
 * Copyright (c) 2019 Daniel Stuart <daniel.stuart@pucpr.edu.br>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include "accel-attributes.h"

AccelLocation
setup_accel_location (GUdevDevice *device)
{
	AccelLocation ret;
	const char *location;

	location = g_udev_device_get_property (device, "ACCEL_LOCATION");
	if (location) {
		if (parse_accel_location (location, &ret))
			return ret;

		g_warning ("Failed to parse ACCEL_LOCATION ('%s') from udev",
			   location);
	}
	location = g_udev_device_get_sysfs_attr (device, "label");
	if (location) {
		if (parse_accel_label (location, &ret))
			return ret;
	}
	location = g_udev_device_get_sysfs_attr (device, "location");
	if (location) {
		if (parse_accel_location (location, &ret))
			return ret;

		g_warning ("Failed to parse location ('%s') from sysfs",
			   location);
	}
	g_debug ("No auto-detected location, falling back to display location");

	ret = ACCEL_LOCATION_DISPLAY;
	return ret;
}

gboolean
parse_accel_label (const char *location, AccelLocation *value)
{
	if (location == NULL ||
	    *location == '\0')
		return FALSE;
	if (g_str_equal (location, "accel-base")) {
		*value = ACCEL_LOCATION_BASE;
		return TRUE;
	} else if (g_str_equal (location, "accel-display")) {
		*value = ACCEL_LOCATION_DISPLAY;
		return TRUE;
	}
	g_debug ("Failed to parse label '%s' as a location", location);
	return FALSE;
}

gboolean
parse_accel_location (const char *location, AccelLocation *value)
{
	/* Empty string means we use the display location */
	if (location == NULL ||
	    *location == '\0' ||
	    g_str_equal (location, "display") ||
	    g_str_equal (location, "lid")) {
		*value = ACCEL_LOCATION_DISPLAY;
		return TRUE;
	} else if (g_str_equal (location, "base")) {
		*value = ACCEL_LOCATION_BASE;
		return TRUE;
	} else {
		g_warning ("Failed to parse '%s' as a location", location);
		return FALSE;
	}
}

gboolean
get_accel_scale (GUdevDevice *device,
		 AccelScale  *scale_vec)
{
	gdouble scale;

	g_return_val_if_fail (scale_vec != NULL, FALSE);

	scale = g_udev_device_get_sysfs_attr_as_double (device, "in_accel_x_scale");
	if (scale != 0.0) {
		scale_vec->x = scale;
		scale_vec->y = g_udev_device_get_sysfs_attr_as_double (device, "in_accel_y_scale");
		scale_vec->z = g_udev_device_get_sysfs_attr_as_double (device, "in_accel_z_scale");
		if (scale_vec->y != 0.0 &&
		    scale_vec->z != 0.0) {
			g_debug ("Attribute in_accel_{x,y,z}_scale (%f,%f,%f) found in sysfs",
				 scale_vec->x, scale_vec->y, scale_vec->z);
			return TRUE;
		}
		g_warning ("Could not read in_accel_{x,y,z}_scale attributes, kernel bug");
	}
	scale = g_udev_device_get_sysfs_attr_as_double (device, "in_accel_scale");
	if (scale != 0.0) {
		g_debug ("Attribute in_accel_scale ('%f') found on sysfs", scale);
		set_accel_scale (scale_vec, scale);
		return TRUE;
	}
	scale = g_udev_device_get_sysfs_attr_as_double (device, "scale");
	if (scale != 0.0) {
		g_debug ("Attribute scale ('%f') found on sysfs", scale);
		set_accel_scale (scale_vec, scale);
		return TRUE;
	}

	g_debug ("Failed to auto-detect scale, falling back to 1.0");
	reset_accel_scale (scale_vec);
	return TRUE;
}

const char *
accel_location_to_string (AccelLocation location)
{
	switch (location) {
	case ACCEL_LOCATION_DISPLAY:
		return "display";
	case ACCEL_LOCATION_BASE:
		return "base";
	default:
		g_assert_not_reached ();
	}
}

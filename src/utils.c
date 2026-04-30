/*
 * Copyright (c) 2021 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include "utils.h"

char *
get_device_file (GUdevDevice *device)
{
	if (!IS_TEST)
		return g_strdup (g_udev_device_get_device_file (device));

	return g_build_filename (g_getenv ("UMOCKDEV_DIR"),
				 "iio-dev-data.bin",
				 NULL);
}

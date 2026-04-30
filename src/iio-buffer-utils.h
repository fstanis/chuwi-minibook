/*
 * Modified from industrialio buffer test code, and Lenovo Yoga (2 Pro) orientation helper
 * Copyright (c) 2008 Jonathan Cameron
 * Copyright (c) 2014 Peter F. Patel-Schneider
 * Copyright (c) 2014 Bastien Nocera <hadess@hadess.net>
 * Copyright (c) 2015 Elad Alfassa <elad@fedoraproject.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include <glib.h>
#include <gudev/gudev.h>

typedef struct iio_channel_info iio_channel_info;

typedef struct {
	GUdevDevice       *device;
	char              *trigger_name;
	const char        *dev_dir_name;
	int                channels_count;
	iio_channel_info **channels;
	int                scan_size;
} BufferDrvData;

typedef struct {
	ssize_t  read_size;
	char    *data;
} IIOSensorData;

IIOSensorData *iio_sensor_data_new (gsize size);
void iio_sensor_data_free (IIOSensorData *data);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(IIOSensorData, iio_sensor_data_free)

void process_scan_1                    (char              *data,
				        BufferDrvData     *buffer_data,
				        const char        *ch_name,
				        int               *ch_val,
				        gdouble           *ch_scale,
				        gboolean          *ch_present);
gboolean iio_fixup_sampling_frequency  (GUdevDevice *dev);
char *get_trigger_name                 (GUdevDevice *device);

void           buffer_drv_data_free    (BufferDrvData *buffer_data);
BufferDrvData *buffer_drv_data_new     (GUdevDevice *device);
void     disable_ring_buffer           (BufferDrvData *data);
gboolean enable_ring_buffer            (BufferDrvData *data);
gboolean is_buffer_usable              (char *path);

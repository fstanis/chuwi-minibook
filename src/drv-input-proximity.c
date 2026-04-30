 /*
 * Copyright (C) 2009 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2023 Sicelo A. Mhlongo <absicsz@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 */

#include "drivers.h"
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <glib/gstdio.h>

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

typedef struct DrvData {
	GUdevDevice        *dev;
	gchar              *native_path;
	int                last_switch_state;
	GIOChannel         *channel;
	struct input_event event;
	gsize              offset;
	guint              watcher_id;
} DrvData;

/**
 * input_str_to_bitmask:
 * @s: string representation of a hexadecimal value
 * @bitmask: destination array to store the binary representation of the
 *   input string
 * @max_size: the size of the allocated bitmask array
 *
 * Convert a hexadecimal value represented as a string into its binary
 * equivalent. The binary value is stored in the bitmask array
 *
 * Returns: the number of bits that are set in the resulting bitmask
 */
static gint
input_str_to_bitmask (const gchar *s, glong *bitmask, size_t max_size)
{
	gint i, j;
	g_auto(GStrv) v = NULL;
	gint num_bits_set = 0;

	memset (bitmask, 0, max_size);
	v = g_strsplit (s, " ", max_size);
	for (i = g_strv_length (v) - 1, j = 0; i >= 0; i--, j++) {
		gulong val;

		val = strtoul (v[i], NULL, 16);
		bitmask[j] = val;

		while (val != 0) {
			num_bits_set++;
			val &= (val - 1);
		}
	}

	return num_bits_set;
}

static gboolean
input_device_is_switch (GUdevDevice *device, glong *bitmask)
{
	gboolean ret = FALSE;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *native_path = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	gint num_bits;

	/* only process devices on the input subsystem */
	if (g_strcmp0 (g_udev_device_get_subsystem (device), "input") != 0)
		return FALSE;

	/* ignore devices which do not report any switch capabilities */
	native_path = g_strdup (g_udev_device_get_sysfs_path (device));
	path = g_build_filename (native_path, "../capabilities/sw", NULL);
	if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
		return FALSE;
	}

	ret = g_file_get_contents (path, &contents, NULL, &error);
	/* for completeness, report read failures */
	if (!ret) {
		g_warning ("Failed to read contents of [%s]: %s",
			path, error->message);
		return FALSE;
	}

	/* confirm that the device has a usable switch */
	num_bits = input_str_to_bitmask (contents, bitmask, sizeof (bitmask));
	if (num_bits == 0) {
		g_debug ("%s is not a valid switch", native_path);
        } else if (num_bits >= SW_CNT) {
		g_debug ("%s reports an invalid number of switches. Was iio-sensor-proxy compiled with kernel headers not matching the running kernel?",
			native_path);
		return FALSE;
	}

	return TRUE;
}

static gboolean
proximity_changed (GIOChannel *channel, GIOCondition condition, gpointer userdata) {
	g_autoptr(GError) error = NULL;
	gsize read_bytes;
	SensorDevice *sensor_device = userdata;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	glong bitmask[NBITS(SW_MAX)];
	ProximityReadings readings;

	while (g_io_channel_read_chars (channel,
		((gchar*)&drv_data->event) + drv_data->offset,
		sizeof(struct input_event) - drv_data->offset,
		&read_bytes, &error) == G_IO_STATUS_NORMAL) {

		/* check if the event data was completely read */
		if (drv_data->offset + read_bytes < sizeof (struct input_event)) {
			drv_data->offset = drv_data->offset + read_bytes;
			g_warning ("Incomplete data was read");
			return TRUE;
		}
		drv_data->offset = 0;

		if (drv_data->event.type != EV_SW) {
			continue;
		}

		if (drv_data->event.code != SW_FRONT_PROXIMITY) {
			continue;
		}

		/* check proximity sensor/switch state */
		if (ioctl (g_io_channel_unix_get_fd(channel), EVIOCGSW(sizeof (bitmask)), bitmask) < 0) {
			g_warning ("Could not determine proximity sensor state, ioctl EVIOCGSW failed");
			continue;
		}

		drv_data->last_switch_state = test_bit (drv_data->event.code, bitmask);
		readings.is_near = drv_data->last_switch_state ? PROXIMITY_NEAR_TRUE : PROXIMITY_NEAR_FALSE;
		sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);
	}
	return TRUE;
}

static gboolean
watch_input_proximity (gpointer user_data)
{
	SensorDevice *sensor_device = user_data;
	DrvData *drv_data = (DrvData *) sensor_device->priv;
	int eventfd;
	const gchar *device_file;
	g_autoptr(GError) error = NULL;
	GIOStatus status;
	glong bitmask[NBITS(SW_MAX)];
	ProximityReadings readings;

	device_file = g_udev_device_get_device_file (drv_data->dev);
	if (device_file == NULL || device_file[0] == '\0') {
		return FALSE;
	}

	eventfd = open (device_file, O_RDONLY | O_NONBLOCK);
	if (eventfd < 0) {
		return FALSE;
	}

	/* get initial state of sensor */
	if (ioctl (eventfd, EVIOCGSW(sizeof (bitmask)), bitmask) < 0) {
		g_warning ("Could not determine sensor state, ioctl EVIOCGSW on %s failed", drv_data->native_path);
		close (eventfd);
		return FALSE;
	}

	/* configure I/O channel and watch it for events */
	g_debug ("Watching %s (%i)", device_file, eventfd);
	drv_data->channel = g_io_channel_unix_new (eventfd);
	g_io_channel_set_close_on_unref (drv_data->channel, TRUE);

	status = g_io_channel_set_encoding (drv_data->channel, NULL, &error);
	if (status != G_IO_STATUS_NORMAL) {
		g_warning ("Failed to set encoding for binary data: %s", error->message);
		return FALSE;
	}

	drv_data->watcher_id = g_io_add_watch (drv_data->channel, G_IO_IN, proximity_changed, sensor_device);
	drv_data->last_switch_state = test_bit (SW_FRONT_PROXIMITY, bitmask);

	/* Notify the initial reading to the manager */
	readings.is_near = drv_data->last_switch_state ? PROXIMITY_NEAR_TRUE : PROXIMITY_NEAR_FALSE;
	sensor_device->callback_func (sensor_device, (gpointer) &readings, sensor_device->user_data);

	return TRUE;
}

static gboolean
input_proximity_discover (GUdevDevice *device)
{
	glong bitmask[NBITS(SW_MAX)];

	if (!input_device_is_switch (device, bitmask))
		return FALSE;

	/* is this SW_FRONT_PROXIMITY? */
	if (!test_bit (SW_FRONT_PROXIMITY, bitmask))
		return FALSE;

	/* Input proximity sensor found */
	g_debug ("Found input proximity sensor at %s",
		g_udev_device_get_sysfs_path (device));
	return TRUE;
}

static void
input_proximity_set_polling (SensorDevice *sensor_device, gboolean state)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	if (drv_data->watcher_id > 0 && state)
		return;

	if (drv_data->watcher_id == 0 && !state)
		return;

	g_clear_handle_id (&drv_data->watcher_id, g_source_remove);

	if (state) {
		/* start watching for proximity events */
		watch_input_proximity (sensor_device);
	}
}

static SensorDevice *
input_proximity_open (GUdevDevice *device)
{
	SensorDevice *sensor_device;
	DrvData *drv_data;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup (g_udev_device_get_name (device));
	sensor_device->priv = g_new0 (DrvData, 1);
	drv_data = (DrvData *) sensor_device->priv;

	drv_data->dev = g_object_ref (device);
	drv_data->native_path = g_strdup (g_udev_device_get_sysfs_path (device));

	return sensor_device;
}

static void
input_proximity_close (SensorDevice *sensor_device)
{
	DrvData *drv_data = (DrvData *) sensor_device->priv;

	g_clear_object (&drv_data->dev);
	g_clear_pointer (&sensor_device->priv, g_free);
	g_free (sensor_device);
}

SensorDriver input_proximity = {
	.driver_name = "Input proximity",
	.type = DRIVER_TYPE_PROXIMITY,
	.discover = input_proximity_discover,
	.open = input_proximity_open,
	.close = input_proximity_close,
	.set_polling = input_proximity_set_polling,
};

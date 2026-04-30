/*
 * Copyright (c) 2015 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include <locale.h>
#include <gio/gio.h>

static GMainLoop *loop;
static guint watch_id;
static GDBusProxy *iio_proxy, *iio_proxy_compass;

static gboolean watch_accel = FALSE;
static gboolean watch_prox = FALSE;
static gboolean watch_compass = FALSE;
static gboolean watch_light = FALSE;

static void
properties_changed (GDBusProxy *proxy,
		    GVariant   *changed_properties,
		    GStrv       invalidated_properties,
		    gpointer    user_data)
{
	GVariant *v;
	GVariantDict dict;

	g_variant_dict_init (&dict, changed_properties);

	if (g_variant_dict_contains (&dict, "HasAccelerometer")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "HasAccelerometer");
		if (g_variant_get_boolean (v))
			g_print ("+++ Accelerometer appeared\n");
		else
			g_print ("--- Accelerometer disappeared\n");
		g_variant_unref (v);
	}
	if (g_variant_dict_contains (&dict, "AccelerometerOrientation")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "AccelerometerOrientation");
		g_print ("    Accelerometer orientation changed: %s\n", g_variant_get_string (v, NULL));
		g_variant_unref (v);
	}
	if (g_variant_dict_contains (&dict, "AccelerometerTilt")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "AccelerometerTilt");
		g_print ("    Tilt changed: %s\n", g_variant_get_string (v, NULL));
		g_variant_unref (v);
	}
	if (g_variant_dict_contains (&dict, "HasAmbientLight")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "HasAmbientLight");
		if (g_variant_get_boolean (v))
			g_print ("+++ Light sensor appeared\n");
		else
			g_print ("--- Light sensor disappeared\n");
		g_variant_unref (v);
	}
	if (g_variant_dict_contains (&dict, "LightLevel")) {
		GVariant *unit;

		v = g_dbus_proxy_get_cached_property (iio_proxy, "LightLevel");
		unit = g_dbus_proxy_get_cached_property (iio_proxy, "LightLevelUnit");
		g_print ("    Light changed: %lf (%s)\n", g_variant_get_double (v), g_variant_get_string (unit, NULL));
		g_variant_unref (v);
		g_variant_unref (unit);
	}
	if (g_variant_dict_contains (&dict, "HasProximity")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "HasProximity");
		if (g_variant_get_boolean (v))
			g_print ("+++ Proximity sensor appeared\n");
		else
			g_print ("--- Proximity sensor disappeared\n");
		g_variant_unref (v);
	}
	if (g_variant_dict_contains (&dict, "ProximityNear")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "ProximityNear");
		g_print ("    Proximity value changed: %d\n", g_variant_get_boolean (v));
		g_variant_unref (v);
	}
	if (g_variant_dict_contains (&dict, "HasCompass")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy_compass, "HasCompass");
		if (g_variant_get_boolean (v))
			g_print ("+++ Compass appeared\n");
		else
			g_print ("--- Compass disappeared\n");
		g_variant_unref (v);
	}
	if (g_variant_dict_contains (&dict, "CompassHeading")) {
		v = g_dbus_proxy_get_cached_property (iio_proxy_compass, "CompassHeading");
		g_print ("    Compass heading changed: %lf\n", g_variant_get_double (v));
		g_variant_unref (v);
	}

	g_variant_dict_clear (&dict);
}

static void
print_initial_values (void)
{
	GVariant *v;

	if (watch_accel) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "HasAccelerometer");
		if (g_variant_get_boolean (v)) {
			g_variant_unref (v);
			GVariant *y;
			v = g_dbus_proxy_get_cached_property (iio_proxy, "AccelerometerOrientation");
			y = g_dbus_proxy_get_cached_property (iio_proxy, "AccelerometerTilt");
			g_print ("=== Has accelerometer (orientation: %s, tilt: %s)\n",
				 g_variant_get_string (v, NULL), g_variant_get_string (y, NULL));
			g_clear_pointer (&y, g_variant_unref);
		} else {
			g_print ("=== No accelerometer\n");
		}
		g_variant_unref (v);
	}

	if (watch_light) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "HasAmbientLight");
		if (g_variant_get_boolean (v)) {
			GVariant *unit;

			g_variant_unref (v);
			v = g_dbus_proxy_get_cached_property (iio_proxy, "LightLevel");
			unit = g_dbus_proxy_get_cached_property (iio_proxy, "LightLevelUnit");
			g_print ("=== Has ambient light sensor (value: %lf, unit: %s)\n",
				 g_variant_get_double (v),
				 g_variant_get_string (unit, NULL));
			g_variant_unref (unit);
		} else {
			g_print ("=== No ambient light sensor\n");
		}
		g_variant_unref (v);
	}

	if (watch_prox) {
		v = g_dbus_proxy_get_cached_property (iio_proxy, "HasProximity");
		if (g_variant_get_boolean (v)) {
			g_variant_unref (v);
			v = g_dbus_proxy_get_cached_property (iio_proxy, "ProximityNear");
			g_print ("=== Has proximity sensor (near: %d)\n",
				 g_variant_get_boolean (v));
		} else {
			g_print ("=== No proximity sensor\n");
		}
		g_variant_unref (v);
	}

	if (!iio_proxy_compass)
		return;

	v = g_dbus_proxy_get_cached_property (iio_proxy_compass, "HasCompass");
	if (g_variant_get_boolean (v)) {
		g_variant_unref (v);
		v = g_dbus_proxy_get_cached_property (iio_proxy, "CompassHeading");
		if (v) {
			g_print ("=== Has compass (heading: %lf)\n",
				 g_variant_get_double (v));
			g_variant_unref (v);
		} else {
			g_print ("=== Has compass (heading: unset)\n");
		}
	} else {
		g_print ("=== No compass\n");
	}
	g_clear_pointer (&v, g_variant_unref);
}

static void
appeared_cb (GDBusConnection *connection,
	     const gchar     *name,
	     const gchar     *name_owner,
	     gpointer         user_data)
{
	GError *error = NULL;
	GVariant *ret = NULL;

	g_print ("+++ iio-sensor-proxy appeared\n");

	iio_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
						   G_DBUS_PROXY_FLAGS_NONE,
						   NULL,
						   "net.hadess.SensorProxy",
						   "/net/hadess/SensorProxy",
						   "net.hadess.SensorProxy",
						   NULL, NULL);

	g_signal_connect (G_OBJECT (iio_proxy), "g-properties-changed",
			  G_CALLBACK (properties_changed), NULL);

	if (watch_compass) {
		iio_proxy_compass = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
								   G_DBUS_PROXY_FLAGS_NONE,
								   NULL,
								   "net.hadess.SensorProxy",
								   "/net/hadess/SensorProxy/Compass",
								   "net.hadess.SensorProxy.Compass",
								   NULL, NULL);

		g_signal_connect (G_OBJECT (iio_proxy_compass), "g-properties-changed",
				  G_CALLBACK (properties_changed), NULL);
	}

	/* Accelerometer */
	if (watch_accel) {
		ret = g_dbus_proxy_call_sync (iio_proxy,
					      "ClaimAccelerometer",
					      NULL,
					      G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      NULL, &error);
		if (!ret) {
			if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_warning ("Failed to claim accelerometer: %s", error->message);
			g_main_loop_quit (loop);
			return;
		}
		g_clear_pointer (&ret, g_variant_unref);
	}

	/* ALS */
	if (watch_light) {
		ret = g_dbus_proxy_call_sync (iio_proxy,
					      "ClaimLight",
					      NULL,
					      G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      NULL, &error);
		if (!ret) {
			if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_warning ("Failed to claim light sensor: %s", error->message);
			g_main_loop_quit (loop);
			return;
		}
		g_clear_pointer (&ret, g_variant_unref);
	}

	/* Proximity sensor */
	if (watch_prox) {
		ret = g_dbus_proxy_call_sync (iio_proxy,
					      "ClaimProximity",
					      NULL,
					      G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      NULL, &error);
		if (!ret) {
			if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_warning ("Failed to claim proximity sensor: %s", error->message);
			g_main_loop_quit (loop);
			return;
		}
		g_clear_pointer (&ret, g_variant_unref);
	}

	/* Compass */
	if (watch_compass) {
		ret = g_dbus_proxy_call_sync (iio_proxy_compass,
					     "ClaimCompass",
					     NULL,
					     G_DBUS_CALL_FLAGS_NONE,
					     -1,
					     NULL, &error);
		if (!ret) {
			if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_warning ("Failed to claim compass: %s", error->message);
			g_main_loop_quit (loop);
			return;
		}
		g_clear_pointer (&ret, g_variant_unref);
	}

	print_initial_values ();
}

static void
vanished_cb (GDBusConnection *connection,
	     const gchar *name,
	     gpointer user_data)
{
	if (iio_proxy || iio_proxy_compass) {
		g_clear_object (&iio_proxy);
		g_clear_object (&iio_proxy_compass);
		g_print ("--- iio-sensor-proxy vanished, waiting for it to appear\n");
	}
}

int main (int argc, char **argv)
{
	g_autoptr(GOptionContext) option_context = NULL;
	g_autoptr(GError) error = NULL;
	gboolean opt_watch_accel = FALSE;
	gboolean opt_watch_prox = FALSE;
	gboolean opt_watch_compass = FALSE;
	gboolean opt_watch_light = FALSE;
	gboolean opt_all = FALSE;
	const GOptionEntry options[] = {
		{ "all", 'a', 0, G_OPTION_ARG_NONE, &opt_all, "Monitor all the sensor changes", NULL },
		{ "accel", 0, 0, G_OPTION_ARG_NONE, &opt_watch_accel, "Monitor accelerometer changes", NULL },
		{ "proximity", 0, 0, G_OPTION_ARG_NONE, &opt_watch_prox, "Monitor proximity sensor changes", NULL },
		{ "compass", 0, 0, G_OPTION_ARG_NONE, &opt_watch_compass, "Monitor compass changes", NULL },
		{ "light", 0, 0, G_OPTION_ARG_NONE, &opt_watch_light, "Monitor light changes changes", NULL },
		{ NULL}
	};
	int ret = 0;

	setlocale (LC_ALL, "");
	option_context = g_option_context_new ("");
	g_option_context_add_main_entries (option_context, options, NULL);

	ret = g_option_context_parse (option_context, &argc, &argv, &error);
	if (!ret) {
		g_print ("Failed to parse arguments: %s\n", error->message);
		return EXIT_FAILURE;
	}

	if ((!opt_watch_accel &&
	     !opt_watch_prox &&
	     !opt_watch_compass &&
	     !opt_watch_light) ||
	    opt_all) {
		opt_watch_accel = opt_watch_prox = opt_watch_light = opt_watch_compass = TRUE;
	}

	watch_accel = opt_watch_accel;
	watch_prox = opt_watch_prox;
	watch_compass = opt_watch_compass;
	watch_light = opt_watch_light;

	watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
				     "net.hadess.SensorProxy",
				     G_BUS_NAME_WATCHER_FLAGS_NONE,
				     appeared_cb,
				     vanished_cb,
				     NULL, NULL);

	g_print ("    Waiting for iio-sensor-proxy to appear\n");
	loop = g_main_loop_new (NULL, TRUE);
	g_main_loop_run (loop);

	return 0;
}

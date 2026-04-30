/*
 * Copyright (c) 2014-2016 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include "config.h"

#include <locale.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib-unix.h>
#include <gio/gio.h>
#include <gudev/gudev.h>
#include <polkit/polkit.h>
#include "drivers.h"
#include "orientation.h"

#include "iio-sensor-proxy-resources.h"

#define SENSOR_PROXY_DBUS_NAME          "net.hadess.SensorProxy"
#define SENSOR_PROXY_DBUS_PATH          "/net/hadess/SensorProxy"
#define SENSOR_PROXY_COMPASS_DBUS_PATH  "/net/hadess/SensorProxy/Compass"
#define SENSOR_PROXY_IFACE_NAME         SENSOR_PROXY_DBUS_NAME
#define SENSOR_PROXY_COMPASS_IFACE_NAME SENSOR_PROXY_DBUS_NAME ".Compass"

#define NUM_SENSOR_TYPES DRIVER_TYPE_PROXIMITY + 1

typedef struct {
	GMainLoop *loop;
	GUdevClient *client;
	GDBusNodeInfo *introspection_data;
	GDBusConnection *connection;
	guint name_id;
	int ret;

	PolkitAuthority *auth;

	SensorDriver      *drivers[NUM_SENSOR_TYPES];
	SensorDevice      *devices[NUM_SENSOR_TYPES];
	GUdevDevice       *udev_devices[NUM_SENSOR_TYPES];
	GHashTable        *clients[NUM_SENSOR_TYPES]; /* key = D-Bus name, value = watch ID */

	GPtrArray         *sensor_startup_dbus_invocations_delayed[NUM_SENSOR_TYPES];
	gboolean           sensor_startup_dbus_event_delayed[NUM_SENSOR_TYPES];

	/* Accelerometer */
	OrientationUp previous_orientation;
	Tilt          previous_tilt;

	/* Light */
	gdouble previous_level;
	gboolean uses_lux;

	/* Compass */
	gdouble previous_heading;

	/* Proximity */
	gboolean previous_prox_near;
} SensorData;

static const SensorDriver * const drivers[] = {
	&iio_buffer_accel,
	&iio_poll_accel,
	&input_accel,
	&iio_buffer_light,
	&iio_poll_light,
	&hwmon_light,
	&fake_compass,
	&fake_light,
	&iio_buffer_compass,
	&iio_poll_proximity,
	&input_proximity,
#ifdef HAS_LIBSSC
	&ssc_proximity,
	&ssc_light,
	&ssc_accel,
	&ssc_compass,
#endif
};

static ReadingsUpdateFunc driver_type_to_callback_func (DriverType type);

static const char *
driver_type_to_str (DriverType type)
{
	switch (type) {
	case DRIVER_TYPE_ACCEL:
		return "accelerometer";
	case DRIVER_TYPE_LIGHT:
		return "ambient light sensor";
	case DRIVER_TYPE_COMPASS:
		return "compass";
	case DRIVER_TYPE_PROXIMITY:
		return "proximity";
	default:
		g_assert_not_reached ();
	}
}

#define DRIVER_FOR_TYPE(driver_type) data->drivers[driver_type]
#define DEVICE_FOR_TYPE(driver_type) data->devices[driver_type]
#define UDEV_DEVICE_FOR_TYPE(driver_type) data->udev_devices[driver_type]

static void sensor_changes (GUdevClient *client,
			    gchar       *action,
			    GUdevDevice *device,
			    SensorData  *data);

static gboolean
driver_type_exists (SensorData *data,
		    DriverType  driver_type)
{
	return (DRIVER_FOR_TYPE(driver_type) != NULL);
}

static gboolean
find_sensors (GUdevClient *client,
	      SensorData  *data)
{
	GList *devices, *input, *platform, *l;
	gboolean found = FALSE;

	devices = g_udev_client_query_by_subsystem (client, "iio");
	input = g_udev_client_query_by_subsystem (client, "input");
	platform = g_udev_client_query_by_subsystem (client, "platform");
	devices = g_list_concat (devices, input);
	devices = g_list_concat (devices, platform);
#ifdef HAS_LIBSSC
	GList *fastrpc;
	fastrpc = g_udev_client_query_by_subsystem (client, "misc");
	devices = g_list_concat (devices, fastrpc);
#endif

	/* Find the devices */
	for (l = devices; l != NULL; l = l->next) {
		GUdevDevice *dev = l->data;
		guint i;

		for (i = 0; i < G_N_ELEMENTS(drivers); i++) {
			SensorDriver *driver = (SensorDriver *) drivers[i];
			if (!driver_type_exists (data, driver->type) &&
			    driver_discover (driver, dev)) {
				g_debug ("Found device %s of type %s at %s",
					 g_udev_device_get_sysfs_path (dev),
					 driver_type_to_str (driver->type),
					 driver->driver_name);
				UDEV_DEVICE_FOR_TYPE(driver->type) = g_object_ref (dev);
				DRIVER_FOR_TYPE(driver->type) = (SensorDriver *) driver;

				found = TRUE;
			}
		}

		if (driver_type_exists (data, DRIVER_TYPE_ACCEL) &&
		    driver_type_exists (data, DRIVER_TYPE_LIGHT) &&
		    driver_type_exists (data, DRIVER_TYPE_PROXIMITY) &&
		    driver_type_exists (data, DRIVER_TYPE_COMPASS))
			break;
	}

	g_list_free_full (devices, g_object_unref);
	return found;
}

static void
free_client_watch (gpointer data)
{
	guint watch_id = GPOINTER_TO_UINT (data);

	if (watch_id == 0)
		return;
	g_bus_unwatch_name (watch_id);
}

static GHashTable *
create_clients_hash_table (void)
{
	return g_hash_table_new_full (g_str_hash, g_str_equal,
				      g_free, free_client_watch);
}

typedef enum {
	PROP_HAS_ACCELEROMETER		= 1 << 0,
	PROP_ACCELEROMETER_ORIENTATION  = 1 << 1,
	PROP_HAS_AMBIENT_LIGHT		= 1 << 2,
	PROP_LIGHT_LEVEL		= 1 << 3,
	PROP_HAS_COMPASS                = 1 << 4,
	PROP_COMPASS_HEADING            = 1 << 5,
	PROP_HAS_PROXIMITY              = 1 << 6,
	PROP_PROXIMITY_NEAR             = 1 << 7,
	PROP_ACCELEROMETER_TILT         = 1 << 8,
} PropertiesMask;

#define PROP_ALL (PROP_HAS_ACCELEROMETER | \
                  PROP_ACCELEROMETER_ORIENTATION | \
                  PROP_HAS_AMBIENT_LIGHT | \
                  PROP_LIGHT_LEVEL | \
                  PROP_HAS_PROXIMITY | \
		  PROP_PROXIMITY_NEAR | \
                  PROP_ACCELEROMETER_TILT)
#define PROP_ALL_COMPASS (PROP_HAS_COMPASS | \
			  PROP_COMPASS_HEADING)

static PropertiesMask
mask_for_sensor_type (DriverType sensor_type)
{
	switch (sensor_type) {
	case DRIVER_TYPE_ACCEL:
		return PROP_HAS_ACCELEROMETER |
			PROP_ACCELEROMETER_ORIENTATION |
			PROP_ACCELEROMETER_TILT;
	case DRIVER_TYPE_LIGHT:
		return PROP_HAS_AMBIENT_LIGHT |
			PROP_LIGHT_LEVEL;
	case DRIVER_TYPE_COMPASS:
		return PROP_HAS_COMPASS |
			PROP_COMPASS_HEADING;
	case DRIVER_TYPE_PROXIMITY:
		return PROP_HAS_PROXIMITY |
			PROP_PROXIMITY_NEAR;
	default:
		g_assert_not_reached ();
	}
}

static void
send_dbus_event_for_client (SensorData     *data,
			    const char     *destination_bus_name,
			    PropertiesMask  mask)
{
	GVariantBuilder props_builder;
	GVariant *props_changed = NULL;

	g_return_if_fail (destination_bus_name != NULL);

	g_variant_builder_init (&props_builder, G_VARIANT_TYPE ("a{sv}"));

	if (mask & PROP_HAS_ACCELEROMETER) {
		gboolean has_accel;

		has_accel = driver_type_exists (data, DRIVER_TYPE_ACCEL);
		g_variant_builder_add (&props_builder, "{sv}", "HasAccelerometer",
				       g_variant_new_boolean (has_accel));

		/* Send the orientation when the device appears */
		if (has_accel) {
			mask |= PROP_ACCELEROMETER_ORIENTATION;
			mask |= PROP_ACCELEROMETER_TILT;
                } else {
			data->previous_orientation = ORIENTATION_UNDEFINED;
			data->previous_tilt = TILT_UNDEFINED;
		}
	}

	if (mask & PROP_ACCELEROMETER_ORIENTATION) {
		g_variant_builder_add (&props_builder, "{sv}", "AccelerometerOrientation",
				       g_variant_new_string (orientation_to_string (data->previous_orientation)));
	}

	if (mask & PROP_ACCELEROMETER_TILT) {
		g_variant_builder_add (&props_builder, "{sv}", "AccelerometerTilt",
				       g_variant_new_string (tilt_to_string (data->previous_tilt)));
	}

	if (mask & PROP_HAS_AMBIENT_LIGHT) {
		gboolean has_als;

		has_als = driver_type_exists (data, DRIVER_TYPE_LIGHT);
		g_variant_builder_add (&props_builder, "{sv}", "HasAmbientLight",
				       g_variant_new_boolean (has_als));

		/* Send the light level when the device appears */
		if (has_als)
			mask |= PROP_LIGHT_LEVEL;
	}

	if (mask & PROP_LIGHT_LEVEL) {
		g_variant_builder_add (&props_builder, "{sv}", "LightLevelUnit",
				       g_variant_new_string (data->uses_lux ? "lux" : "vendor"));
		g_variant_builder_add (&props_builder, "{sv}", "LightLevel",
				       g_variant_new_double (data->previous_level));
	}

	if (mask & PROP_HAS_COMPASS) {
		gboolean has_compass;

		has_compass = driver_type_exists (data, DRIVER_TYPE_COMPASS);
		g_variant_builder_add (&props_builder, "{sv}", "HasCompass",
				       g_variant_new_boolean (has_compass));

		/* Send the heading when the device appears */
		if (has_compass)
			mask |= PROP_COMPASS_HEADING;
	}

	if (mask & PROP_COMPASS_HEADING) {
		g_variant_builder_add (&props_builder, "{sv}", "CompassHeading",
				       g_variant_new_double (data->previous_heading));
	}

	if (mask & PROP_HAS_PROXIMITY) {
		gboolean has_proximity;

		has_proximity = driver_type_exists (data, DRIVER_TYPE_PROXIMITY);
		g_variant_builder_add (&props_builder, "{sv}", "HasProximity",
				       g_variant_new_boolean (has_proximity));

		/* Send proximity information when the device appears */
		if (has_proximity)
			mask |= PROP_PROXIMITY_NEAR;
	}

	if (mask & PROP_PROXIMITY_NEAR) {
		g_variant_builder_add (&props_builder, "{sv}", "ProximityNear",
				       g_variant_new_boolean (data->previous_prox_near));
	}

	props_changed = g_variant_new ("(s@a{sv}@as)", (mask & PROP_ALL) ? SENSOR_PROXY_IFACE_NAME : SENSOR_PROXY_COMPASS_IFACE_NAME,
				       g_variant_builder_end (&props_builder),
				       g_variant_new_strv (NULL, 0));

	g_dbus_connection_emit_signal (data->connection,
				       destination_bus_name,
				       (mask & PROP_ALL) ? SENSOR_PROXY_DBUS_PATH : SENSOR_PROXY_COMPASS_DBUS_PATH,
				       "org.freedesktop.DBus.Properties",
				       "PropertiesChanged",
				       props_changed, NULL);
}

static void
send_dbus_event (SensorData     *data,
		 PropertiesMask  mask)
{
	GHashTable *ht;
	guint i;
	GHashTableIter iter;
	gpointer key, value;

	g_assert (mask != 0);
	g_assert (data->connection);
	g_assert ((mask & PROP_ALL) == 0 || (mask & PROP_ALL_COMPASS) == 0);

	/* Make a list of the events each client for each sensor
	 * is interested in */
	ht = g_hash_table_new (g_str_hash, g_str_equal);
	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		GList *clients, *l;

		clients = g_hash_table_get_keys (data->clients[i]);
		for (l = clients; l != NULL; l = l->next) {
			PropertiesMask m, new_mask;

			/* Already have a mask? */
			m = GPOINTER_TO_UINT (g_hash_table_lookup (ht, l->data));
			new_mask = mask & mask_for_sensor_type (i);
			m |= new_mask;
			g_hash_table_insert (ht, l->data, GUINT_TO_POINTER (m));
		}
	}

	g_hash_table_iter_init (&iter, ht);
	while (g_hash_table_iter_next (&iter, &key, &value))
		send_dbus_event_for_client (data, (const char *) key, GPOINTER_TO_UINT (value));
	g_hash_table_destroy (ht);
}

static void
send_driver_changed_dbus_event (SensorData   *data,
				DriverType    driver_type)
{
	if (driver_type == DRIVER_TYPE_ACCEL)
		send_dbus_event (data, PROP_HAS_ACCELEROMETER);
	else if (driver_type == DRIVER_TYPE_LIGHT)
		send_dbus_event (data, PROP_HAS_AMBIENT_LIGHT);
	else if (driver_type == DRIVER_TYPE_PROXIMITY)
		send_dbus_event (data, PROP_HAS_PROXIMITY);
	else if (driver_type == DRIVER_TYPE_COMPASS)
		send_dbus_event (data, PROP_HAS_COMPASS);
	else
		g_assert_not_reached ();
}

static gboolean
any_sensors_left (SensorData *data)
{
	guint i;
	gboolean exists = FALSE;

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		if (driver_type_exists (data, i)) {
			exists = TRUE;
			break;
		}
	}

	return exists;
}

static void
client_release (SensorData            *data,
		const char            *sender,
		DriverType             driver_type)
{
	GHashTable *ht;
	guint watch_id;

	ht = data->clients[driver_type];

	watch_id = GPOINTER_TO_UINT (g_hash_table_lookup (ht, sender));
	if (watch_id == 0) {
		g_debug ("Sender '%s' already released device, no-op", sender);
		return;
	}

	g_hash_table_remove (ht, sender);

	if (driver_type_exists (data, driver_type) &&
	    g_hash_table_size (ht) == 0) {
		SensorDevice *sensor_device = DEVICE_FOR_TYPE(driver_type);
		driver_set_polling (sensor_device, FALSE);
	}
}

static void
client_vanished_cb (GDBusConnection *connection,
		    const gchar     *name,
		    gpointer         user_data)
{
	SensorData *data = user_data;
	guint i;
	char *sender;

	if (name == NULL)
		return;

	sender = g_strdup (name);

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		GHashTable *ht;
		guint watch_id;

		ht = data->clients[i];
		g_assert (ht);

		watch_id = GPOINTER_TO_UINT (g_hash_table_lookup (ht, sender));
		if (watch_id > 0)
			client_release (data, sender, i);
	}

	g_free (sender);
}

static gboolean
check_claim_permission (SensorData   *data,
			const char   *sender,
			GError      **error)
{
	g_autoptr(GError) local_error = NULL;
	g_autoptr(PolkitAuthorizationResult) result = NULL;
	g_autoptr(PolkitSubject) subject = NULL;

	subject = polkit_system_bus_name_new (sender);
	result = polkit_authority_check_authorization_sync (data->auth,
							    subject,
							    "net.hadess.SensorProxy.claim-sensor",
							    NULL,
							    POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE,
							    NULL, &local_error);
	if (result == NULL ||
	    !polkit_authorization_result_get_is_authorized (result))
	{
		g_set_error (error, G_DBUS_ERROR,
			     G_DBUS_ERROR_ACCESS_DENIED,
			     "Not Authorized: %s", local_error ? local_error->message : "Sensor claim not allowed");
		return FALSE;
	}

	return TRUE;
}

static void
handle_generic_method_call (SensorData            *data,
			    const gchar           *sender,
			    const gchar           *object_path,
			    const gchar           *interface_name,
			    const gchar           *method_name,
			    GVariant              *parameters,
			    GDBusMethodInvocation *invocation,
			    DriverType             driver_type)
{
	GHashTable *ht;
	guint watch_id;

	g_debug ("Handling driver refcounting method '%s' for %s device",
		 method_name, driver_type_to_str (driver_type));

	ht = data->clients[driver_type];

	if (g_str_has_prefix (method_name, "Claim")) {
		watch_id = GPOINTER_TO_UINT (g_hash_table_lookup (ht, sender));
		if (watch_id > 0) {
			g_debug ("Sender '%s' already claimed device, no-op", sender);
			g_dbus_method_invocation_return_value (invocation, NULL);
			return;
		}

		watch_id = g_bus_watch_name_on_connection (data->connection,
							   sender,
							   G_BUS_NAME_WATCHER_FLAGS_NONE,
							   NULL,
							   client_vanished_cb,
							   data,
							   NULL);
		g_hash_table_insert (ht, g_strdup (sender), GUINT_TO_POINTER (watch_id));

		if (driver_type_exists (data, driver_type)) {
			SensorDevice *sensor_device = DEVICE_FOR_TYPE(driver_type);

			/* No other clients for this sensor? Start it */
			if (g_hash_table_size (ht) == 1) {
				/* Return the dbus invocation only after we receive the first
				 * reading from the sensor, so that the client has a valid
				 * measurement to work with right after calling Claim(). */
				g_ptr_array_add (data->sensor_startup_dbus_invocations_delayed[driver_type],
						 invocation);

				driver_set_polling (sensor_device, TRUE);
			} else if (data->sensor_startup_dbus_invocations_delayed[driver_type]->len > 0 ||
				   data->sensor_startup_dbus_event_delayed[driver_type]) {
				/* Client added while the sensor is still starting up, add it to
				 * the delayed invocation list. */
				g_ptr_array_add (data->sensor_startup_dbus_invocations_delayed[driver_type],
						 invocation);
			} else {
				/* Client added while sensor is active already, can return the
				 * invocation immediately. */
				g_dbus_method_invocation_return_value (invocation, NULL);
			}
		} else {
			g_dbus_method_invocation_return_value (invocation, NULL);
		}
	} else if (g_str_has_prefix (method_name, "Release")) {
		client_release (data, sender, driver_type);
		g_dbus_method_invocation_return_value (invocation, NULL);
	}
}

static void
handle_method_call (GDBusConnection       *connection,
		    const gchar           *sender,
		    const gchar           *object_path,
		    const gchar           *interface_name,
		    const gchar           *method_name,
		    GVariant              *parameters,
		    GDBusMethodInvocation *invocation,
		    gpointer               user_data)
{
	SensorData *data = user_data;
	DriverType driver_type;
	g_autoptr(GError) error = NULL;

	if (g_strcmp0 (method_name, "ClaimAccelerometer") == 0 ||
	    g_strcmp0 (method_name, "ReleaseAccelerometer") == 0)
		driver_type = DRIVER_TYPE_ACCEL;
	else if (g_strcmp0 (method_name, "ClaimLight") == 0 ||
		 g_strcmp0 (method_name, "ReleaseLight") == 0)
		driver_type = DRIVER_TYPE_LIGHT;
	else if (g_strcmp0 (method_name, "ClaimProximity") == 0 ||
		 g_strcmp0 (method_name, "ReleaseProximity") == 0)
	        driver_type = DRIVER_TYPE_PROXIMITY;
	else {
		g_dbus_method_invocation_return_error (invocation,
						       G_DBUS_ERROR,
						       G_DBUS_ERROR_UNKNOWN_METHOD,
						       "Method '%s' does not exist on object %s",
						       method_name, object_path);
		return;
	}

	if (!check_claim_permission (data, sender, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return;
	}

	handle_generic_method_call (data, sender, object_path,
				    interface_name, method_name,
				    parameters, invocation, driver_type);
}

static GVariant *
handle_get_property (GDBusConnection *connection,
		     const gchar     *sender,
		     const gchar     *object_path,
		     const gchar     *interface_name,
		     const gchar     *property_name,
		     GError         **error,
		     gpointer         user_data)
{
	SensorData *data = user_data;

	g_assert (data->connection);

	if (g_strcmp0 (property_name, "HasAccelerometer") == 0)
		return g_variant_new_boolean (driver_type_exists (data, DRIVER_TYPE_ACCEL));
	if (g_strcmp0 (property_name, "AccelerometerOrientation") == 0)
		return g_variant_new_string (orientation_to_string (data->previous_orientation));
	if (g_strcmp0 (property_name, "AccelerometerTilt") == 0)
		return g_variant_new_string (tilt_to_string (data->previous_tilt));
	if (g_strcmp0 (property_name, "HasAmbientLight") == 0)
		return g_variant_new_boolean (driver_type_exists (data, DRIVER_TYPE_LIGHT));
	if (g_strcmp0 (property_name, "LightLevelUnit") == 0)
		return g_variant_new_string (data->uses_lux ? "lux" : "vendor");
	if (g_strcmp0 (property_name, "LightLevel") == 0)
		return g_variant_new_double (data->previous_level);
	if (g_strcmp0 (property_name, "HasProximity") == 0)
		return g_variant_new_boolean (driver_type_exists (data, DRIVER_TYPE_PROXIMITY));
	if (g_strcmp0 (property_name, "ProximityNear") == 0)
		return g_variant_new_boolean (data->previous_prox_near);

	return NULL;
}

static const GDBusInterfaceVTable interface_vtable =
{
	handle_method_call,
	handle_get_property,
	NULL
};

static void
handle_compass_method_call (GDBusConnection       *connection,
			    const gchar           *sender,
			    const gchar           *object_path,
			    const gchar           *interface_name,
			    const gchar           *method_name,
			    GVariant              *parameters,
			    GDBusMethodInvocation *invocation,
			    gpointer               user_data)
{
	SensorData *data = user_data;
	g_autoptr(GError) error = NULL;

	if (g_strcmp0 (method_name, "ClaimCompass") != 0 &&
	    g_strcmp0 (method_name, "ReleaseCompass") != 0) {
		g_dbus_method_invocation_return_error (invocation,
						       G_DBUS_ERROR,
						       G_DBUS_ERROR_UNKNOWN_METHOD,
						       "Method '%s' does not exist on object %s",
						       method_name, object_path);
		return;
	}

	if (!check_claim_permission (data, sender, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return;
	}

	handle_generic_method_call (data, sender, object_path,
				    interface_name, method_name,
				    parameters, invocation, DRIVER_TYPE_COMPASS);
}

static GVariant *
handle_compass_get_property (GDBusConnection *connection,
			     const gchar     *sender,
			     const gchar     *object_path,
			     const gchar     *interface_name,
			     const gchar     *property_name,
			     GError         **error,
			     gpointer         user_data)
{
	SensorData *data = user_data;

	g_assert (data->connection);

	if (g_strcmp0 (property_name, "HasCompass") == 0)
		return g_variant_new_boolean (data->drivers[DRIVER_TYPE_COMPASS] != NULL);
	if (g_strcmp0 (property_name, "CompassHeading") == 0)
		return g_variant_new_double (data->previous_heading);

	return NULL;
}

static const GDBusInterfaceVTable compass_interface_vtable =
{
	handle_compass_method_call,
	handle_compass_get_property,
	NULL
};

static void
name_lost_handler (GDBusConnection *connection,
		   const gchar     *name,
		   gpointer         user_data)
{
	g_debug ("iio-sensor-proxy is already running, or it cannot own its D-Bus name. Verify installation.");
	exit (0);
}

static void
bus_acquired_handler (GDBusConnection *connection,
		      const gchar     *name,
		      gpointer         user_data)
{
	SensorData *data = user_data;
	const gchar * const subsystems[] = {
		"iio",
		"input",
		"platform",
#ifdef HAS_LIBSSC
		"misc",
#endif
		NULL
    };
	guint i;

	g_dbus_connection_register_object (connection,
					   SENSOR_PROXY_DBUS_PATH,
					   data->introspection_data->interfaces[0],
					   &interface_vtable,
					   data,
					   NULL,
					   NULL);

	g_dbus_connection_register_object (connection,
					   SENSOR_PROXY_COMPASS_DBUS_PATH,
					   data->introspection_data->interfaces[1],
					   &compass_interface_vtable,
					   data,
					   NULL,
					   NULL);

	/* Initialize DBus clients hash table before DBus is fully exposed */
	data->client = g_udev_client_new (subsystems);

	for (guint i = 0; i < NUM_SENSOR_TYPES; i++)
		data->clients[i] = create_clients_hash_table ();

	data->connection = g_object_ref (connection);
}

static void
name_acquired_handler (GDBusConnection *connection,
		       const gchar     *name,
		       gpointer         user_data)
{
	SensorData *data = user_data;
	guint i;

	if (!find_sensors (data->client, data))
		goto bail;

	g_signal_connect (G_OBJECT (data->client), "uevent",
			  G_CALLBACK (sensor_changes), data);

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		SensorDevice *sensor_device;

		data->sensor_startup_dbus_invocations_delayed[i] = g_ptr_array_new ();

		if (!driver_type_exists (data, i))
			continue;

		sensor_device = driver_open (DRIVER_FOR_TYPE(i), UDEV_DEVICE_FOR_TYPE(i),
					     driver_type_to_callback_func (data->drivers[i]->type), data);
		if (!sensor_device) {
			DRIVER_FOR_TYPE(i) = NULL;
			g_clear_object (&UDEV_DEVICE_FOR_TYPE(i));
			continue;
		}

		DEVICE_FOR_TYPE(i) = sensor_device;
	}

	if (!any_sensors_left (data))
		goto bail;

	send_dbus_event (data, PROP_ALL);
	send_dbus_event (data, PROP_ALL_COMPASS);
	return;

bail:
	data->ret = 0;
	g_message ("No sensors or missing kernel drivers for the sensors. Exiting");
	g_main_loop_quit (data->loop);
}

static gboolean
setup_dbus (SensorData *data,
	    gboolean    replace)
{
	GBytes *bytes;
	GBusNameOwnerFlags flags;

	bytes = g_resources_lookup_data ("/net/hadess/SensorProxy/net.hadess.SensorProxy.xml",
					 G_RESOURCE_LOOKUP_FLAGS_NONE,
					 NULL);
	data->introspection_data = g_dbus_node_info_new_for_xml (g_bytes_get_data (bytes, NULL), NULL);
	g_bytes_unref (bytes);
	g_assert (data->introspection_data != NULL);

	flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
	if (replace)
		flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

	data->name_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
					SENSOR_PROXY_DBUS_NAME,
					flags,
					bus_acquired_handler,
					name_acquired_handler,
					name_lost_handler,
					data,
					NULL);

	return TRUE;
}

static void
maybe_notify_sensor_startup_finished (SensorData *data,
				      DriverType  type)
{
	if (data->sensor_startup_dbus_invocations_delayed[type]->len > 0) {
		g_autofree GDBusMethodInvocation **invocations = NULL;
		size_t n_invocations, i;

		invocations = (GDBusMethodInvocation**) g_ptr_array_steal (data->sensor_startup_dbus_invocations_delayed[type],
									   &n_invocations);
		for (i = 0; i < n_invocations; i++)
			g_dbus_method_invocation_return_value (invocations[i], NULL);
	}

	if (data->sensor_startup_dbus_event_delayed[type]) {
		send_driver_changed_dbus_event (data, type);
		data->sensor_startup_dbus_event_delayed[type] = FALSE;
	}
}

static void
accel_changed_func (SensorDevice *sensor_device,
		    gpointer      readings_data,
		    gpointer      user_data)
{
	SensorData *data = user_data;
	AccelReadings *readings = (AccelReadings *) readings_data;
	OrientationUp orientation;
	Tilt tilt;

	//FIXME handle errors
	g_debug ("Accel sent by driver (quirk applied): %d, %d, %d (scale: %lf,%lf,%lf)",
		 readings->accel_x, readings->accel_y, readings->accel_z,
		 readings->scale.x, readings->scale.y, readings->scale.z);

	orientation = orientation_calc (data->previous_orientation,
					readings->accel_x, readings->accel_y, readings->accel_z,
					readings->scale);

	if (data->previous_orientation != orientation) {
		OrientationUp tmp;

		tmp = data->previous_orientation;
		data->previous_orientation = orientation;
		send_dbus_event (data, PROP_ACCELEROMETER_ORIENTATION);
		g_debug ("Emitted orientation changed: from %s to %s",
			 orientation_to_string (tmp),
			 orientation_to_string (data->previous_orientation));
	}

	tilt = tilt_calc (data->previous_tilt,
			  readings->accel_x, readings->accel_y, readings->accel_z,
			  readings->scale);

	if (data->previous_tilt != tilt) {
		Tilt tmp;

		tmp = data->previous_tilt;
		data->previous_tilt = tilt;
		send_dbus_event (data, PROP_ACCELEROMETER_TILT);
		g_debug ("Emitted tilt changed: from %s to %s",
			 tilt_to_string (tmp),
			 tilt_to_string (data->previous_tilt));
	}

	maybe_notify_sensor_startup_finished (data, DRIVER_TYPE_ACCEL);
}

static void
light_changed_func (SensorDevice *sensor_device,
		    gpointer      readings_data,
		    gpointer      user_data)
{
	SensorData *data = user_data;
	LightReadings *readings = (LightReadings *) readings_data;

	//FIXME handle errors
	g_debug ("Light level sent by driver (quirk applied): %lf (unit: %s)",
		 readings->level, data->uses_lux ? "lux" : "vendor");

	if (data->previous_level != readings->level ||
	    data->uses_lux != readings->uses_lux) {
		gdouble tmp;

		tmp = data->previous_level;
		data->previous_level = readings->level;

		data->uses_lux = readings->uses_lux;

		send_dbus_event (data, PROP_LIGHT_LEVEL);
		g_debug ("Emitted light changed: from %lf to %lf",
			 tmp, data->previous_level);
	}

	maybe_notify_sensor_startup_finished (data, DRIVER_TYPE_LIGHT);
}

static void
compass_changed_func (SensorDevice *sensor_device,
                      gpointer      readings_data,
                      gpointer      user_data)
{
	SensorData *data = user_data;
	CompassReadings *readings = (CompassReadings *) readings_data;

	//FIXME handle errors
	g_debug ("Heading sent by driver (quirk applied): %lf degrees",
	         readings->heading);

	if (data->previous_heading != readings->heading) {
		gdouble tmp;

		tmp = data->previous_heading;
		data->previous_heading = readings->heading;

		send_dbus_event (data, PROP_COMPASS_HEADING);
		g_debug ("Emitted heading changed: from %lf to %lf",
			 tmp, data->previous_heading);
	}

	maybe_notify_sensor_startup_finished (data, DRIVER_TYPE_COMPASS);
}

static void
proximity_changed_func (SensorDevice *sensor_device,
			gpointer      readings_data,
			gpointer      user_data)
{
	SensorData *data = user_data;
	ProximityReadings *readings = (ProximityReadings *) readings_data;
	gboolean near;

	//FIXME handle errors
	g_debug ("Proximity sent by driver: %d",
	         readings->is_near);

	near = readings->is_near > 0;
	if (data->previous_prox_near != near) {
		ProximityNear tmp;

		tmp = data->previous_prox_near;
		data->previous_prox_near = near;

		send_dbus_event (data, PROP_PROXIMITY_NEAR);
		g_debug ("Emitted proximity changed: from %d to %d",
			 tmp, near);
	}

	maybe_notify_sensor_startup_finished (data, DRIVER_TYPE_PROXIMITY);
}

static ReadingsUpdateFunc
driver_type_to_callback_func (DriverType type)
{
	switch (type) {
	case DRIVER_TYPE_ACCEL:
		return accel_changed_func;
	case DRIVER_TYPE_LIGHT:
		return light_changed_func;
	case DRIVER_TYPE_COMPASS:
		return compass_changed_func;
	case DRIVER_TYPE_PROXIMITY:
		return proximity_changed_func;
	default:
		g_assert_not_reached ();
	}
}

static void
free_sensor_data (SensorData *data)
{
	guint i;

	if (data == NULL)
		return;

	if (data->name_id != 0) {
		g_bus_unown_name (data->name_id);
		data->name_id = 0;
	}

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		if (driver_type_exists (data, i))
			driver_close (DEVICE_FOR_TYPE(i));
		g_clear_object (&UDEV_DEVICE_FOR_TYPE(i));
		g_clear_pointer (&data->clients[i], g_hash_table_unref);
		g_clear_pointer (&data->sensor_startup_dbus_invocations_delayed[i], g_ptr_array_unref);
	}

	g_clear_object (&data->auth);
	g_clear_pointer (&data->introspection_data, g_dbus_node_info_unref);
	g_clear_object (&data->connection);
	g_clear_object (&data->client);
	g_clear_pointer (&data->loop, g_main_loop_unref);
	g_free (data);
}

static void
sensor_changes (GUdevClient *client,
		gchar       *action,
		GUdevDevice *device,
		SensorData  *data)
{
	guint i;

	g_debug ("Sensor changes: action = %s, device = %s",
		 action, g_udev_device_get_sysfs_path (device));

	if (g_strcmp0 (action, "remove") == 0) {
		for (i = 0; i < NUM_SENSOR_TYPES; i++) {
			GUdevDevice *dev = UDEV_DEVICE_FOR_TYPE(i);

			if (!dev)
				continue;

			if (g_strcmp0 (g_udev_device_get_sysfs_path (device), g_udev_device_get_sysfs_path (dev)) == 0) {
				g_debug ("Sensor type %s got removed (%s)",
					 driver_type_to_str (i),
					 g_udev_device_get_sysfs_path (dev));

				g_clear_object (&UDEV_DEVICE_FOR_TYPE(i));
				driver_close (DEVICE_FOR_TYPE(i));
				DEVICE_FOR_TYPE(i) = NULL;
				DRIVER_FOR_TYPE(i) = NULL;

				g_clear_pointer (&data->clients[i], g_hash_table_unref);
				data->clients[i] = create_clients_hash_table ();

				/* If the driver is still starting up and never sent us a
				 * reading, we might still be waiting for that. */
				if (data->sensor_startup_dbus_event_delayed[i])
					data->sensor_startup_dbus_event_delayed[i] = FALSE;

				if (data->sensor_startup_dbus_invocations_delayed[i]->len > 0) {
					g_autofree GDBusMethodInvocation **invocations = NULL;
					size_t n_invocations, j;

					invocations = (GDBusMethodInvocation**) g_ptr_array_steal (data->sensor_startup_dbus_invocations_delayed[i],
												   &n_invocations);
					for (j = 0; j < n_invocations; j++) {
						g_dbus_method_invocation_return_error (invocations[j],
										       G_DBUS_ERROR,
										       G_DBUS_ERROR_FAILED,
										       "Sensor removed while waiting for it to start up");
					}
				}

				send_driver_changed_dbus_event (data, i);
			}
		}

		if (!any_sensors_left (data))
			g_main_loop_quit (data->loop);
	} else if (g_strcmp0 (action, "add") == 0) {
		for (i = 0; i < G_N_ELEMENTS(drivers); i++) {
			SensorDriver *driver = (SensorDriver *) drivers[i];
			if (!driver_type_exists (data, driver->type) &&
			    driver_discover (driver, device)) {
				SensorDevice *sensor_device = NULL;

				g_debug ("Found hotplugged device %s of type %s at %s",
					 g_udev_device_get_sysfs_path (device),
					 driver_type_to_str (driver->type),
					 driver->driver_name);

				sensor_device = driver_open (driver, device,
					driver_type_to_callback_func (driver->type), data);

				if (sensor_device) {
					GHashTable *ht;

					UDEV_DEVICE_FOR_TYPE(driver->type) = g_object_ref (device);
					DEVICE_FOR_TYPE(driver->type) = sensor_device;
					DRIVER_FOR_TYPE(driver->type) = (SensorDriver *) driver;

					g_assert (data->sensor_startup_dbus_event_delayed[driver->type] == FALSE);

					ht = data->clients[driver->type];

					if (g_hash_table_size (ht) > 0) {
						SensorDevice *sensor_device = DEVICE_FOR_TYPE(driver->type);

						/* Sensor just appeared but it's already claimed by some clients. Wait
						 * with calling send_driver_changed_dbus_event() until the first measurement
						 * appears, so that the clients have a valid measurement to work with. */
						data->sensor_startup_dbus_event_delayed[driver->type] = TRUE;

						driver_set_polling (sensor_device, TRUE);
					} else {
						send_driver_changed_dbus_event (data, driver->type);
					}
				}
				break;
			}
		}
	}
}

gboolean
termination_signal_handler (gpointer user_data)
{
	SensorData *data = user_data;

	g_debug ("Shutting down");
	g_main_loop_quit (data->loop);

	return G_SOURCE_REMOVE;
}

int main (int argc, char **argv)
{
	SensorData *data;
	g_autoptr(GOptionContext) option_context = NULL;
	g_autoptr(GError) error = NULL;
	gboolean verbose = FALSE;
	gboolean replace = FALSE;
	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Show extra debugging information", NULL },
		{ "replace", 'r', 0, G_OPTION_ARG_NONE, &replace, "Replace the running instance of iio-sensor-proxy", NULL },
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

	if (verbose) {
		if (!g_setenv ("G_MESSAGES_DEBUG", "all", TRUE)) {
			g_warning ("Failed to enable debug");
			return EXIT_FAILURE;
		}
		g_debug ("Starting iio-sensor-proxy version "VERSION);
	}

	data = g_new0 (SensorData, 1);
	data->previous_orientation = ORIENTATION_UNDEFINED;
	data->previous_tilt = TILT_UNDEFINED;
	data->uses_lux = TRUE;

	/* Set up D-Bus */
	setup_dbus (data, replace);

	data->auth = polkit_authority_get_sync (NULL, NULL);
	data->loop = g_main_loop_new (NULL, TRUE);

	g_unix_signal_add (SIGINT, (GSourceFunc) termination_signal_handler, data);
	g_unix_signal_add (SIGTERM, (GSourceFunc) termination_signal_handler, data);

	g_main_loop_run (data->loop);
	ret = data->ret;
	free_sensor_data (data);

	return ret;
}

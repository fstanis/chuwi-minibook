/*
 * Test MXC6655 repeated ClaimAccelerometer behavior.
 *
 * MXC6655 delays ClaimAccelerometer replies until the driver callback
 * provides a sample.  This test verifies that repeated set_polling(TRUE)
 * with want_polling already TRUE still invokes the callback.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "drv-mxc6655-accel.c"

typedef struct {
	int            count;
	AccelReadings  readings;
} CallbackCapture;

static void
capture_callback (SensorDevice *sensor_device,
		  gpointer      readings,
		  gpointer      user_data)
{
	CallbackCapture *cap = (CallbackCapture *) user_data;

	(void) sensor_device;

	cap->count++;
	if (readings)
		cap->readings = *(AccelReadings *) readings;
}

static SensorDevice *
make_test_device (gint             cur_orient,
		  CallbackCapture *cap)
{
	SensorDevice *sensor_device;
	DrvData      *drv_data;

	drv_data = g_new0 (DrvData, 1);
	drv_data->i2c_fds[0] = -1;
	drv_data->i2c_fds[1] = -1;
	drv_data->uinput_fd = -1;
	drv_data->lid_fd = -1;
	drv_data->want_polling = TRUE;
	drv_data->cur_orient = cur_orient;
	drv_data->pending_orient = -1;

	sensor_device = g_new0 (SensorDevice, 1);
	sensor_device->name = g_strdup ("test-device");
	sensor_device->priv = drv_data;
	sensor_device->drv = &mxc6655_accel;
	sensor_device->callback_func = capture_callback;
	sensor_device->user_data = cap;

	return sensor_device;
}

static void
test_repeated_claim_known_orientation (void)
{
	SensorDevice   *sd;
	AccelReadings   expected;
	CallbackCapture cap = { 0 };

	/* LEFT -> x=256, y=0, z=0 */
	build_synthetic_readings (MXC_ORIENT_LEFT, &expected);

	sd = make_test_device (MXC_ORIENT_LEFT, &cap);
	mxc6655_set_polling (sd, TRUE);

	g_assert_cmpint (cap.count, ==, 1);
	g_assert_cmpint (cap.readings.accel_x, ==, expected.accel_x);
	g_assert_cmpint (cap.readings.accel_y, ==, expected.accel_y);
	g_assert_cmpint (cap.readings.accel_z, ==, expected.accel_z);

	driver_close (sd);
}

static void
test_repeated_claim_unknown_orientation (void)
{
	SensorDevice   *sd;
	AccelReadings   expected;
	CallbackCapture cap = { 0 };

	/* Unknown cur_orient falls back to MXC_ORIENT_RIGHT -> x=-256, y=0, z=0 */
	build_synthetic_readings (MXC_ORIENT_RIGHT, &expected);

	sd = make_test_device (-1, &cap);
	mxc6655_set_polling (sd, TRUE);

	g_assert_cmpint (cap.count, ==, 1);
	g_assert_cmpint (cap.readings.accel_x, ==, expected.accel_x);
	g_assert_cmpint (cap.readings.accel_y, ==, expected.accel_y);
	g_assert_cmpint (cap.readings.accel_z, ==, expected.accel_z);

	driver_close (sd);
}

int
main (int    argc,
      char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/mxc6655/repeated-claim-known-orientation",
			 test_repeated_claim_known_orientation);
	g_test_add_func ("/mxc6655/repeated-claim-unknown-orientation",
			 test_repeated_claim_unknown_orientation);

	return g_test_run ();
}

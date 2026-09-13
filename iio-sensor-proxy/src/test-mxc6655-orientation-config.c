/*
 * Test MXC6655 orientation configuration overrides.
 *
 * MINIBOOK_PANEL_ORIENTATION overrides the DRM-detected static panel
 * orientation, MINIBOOK_LAPTOP_ORIENTATION the orientation reported
 * outside tablet mode.
 *
 * Copyright (c) 2025
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "drv-mxc6655-accel.c"

static void
test_panel_orientation_unset (void)
{
	g_unsetenv (ENV_PANEL_ORIENTATION);

	g_assert_cmpint (configured_panel_orientation (), ==,
			 PANEL_ORIENT_UNKNOWN);
}

/* The names the kernel takes in video=<connector>:panel_orientation= */
static void
test_panel_orientation_values (void)
{
	g_setenv (ENV_PANEL_ORIENTATION, "normal", TRUE);
	g_assert_cmpint (configured_panel_orientation (), ==,
			 PANEL_ORIENT_NORMAL);

	g_setenv (ENV_PANEL_ORIENTATION, "upside_down", TRUE);
	g_assert_cmpint (configured_panel_orientation (), ==,
			 PANEL_ORIENT_BOTTOM_UP);

	g_setenv (ENV_PANEL_ORIENTATION, "left_side_up", TRUE);
	g_assert_cmpint (configured_panel_orientation (), ==,
			 PANEL_ORIENT_LEFT_UP);

	g_setenv (ENV_PANEL_ORIENTATION, "right_side_up", TRUE);
	g_assert_cmpint (configured_panel_orientation (), ==,
			 PANEL_ORIENT_RIGHT_UP);

	g_setenv (ENV_PANEL_ORIENTATION, "auto", TRUE);
	g_assert_cmpint (configured_panel_orientation (), ==,
			 PANEL_ORIENT_UNKNOWN);

	g_unsetenv (ENV_PANEL_ORIENTATION);
}

static void
test_panel_orientation_invalid (void)
{
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
			       "*ignoring invalid*");

	g_setenv (ENV_PANEL_ORIENTATION, "270", TRUE);
	g_assert_cmpint (configured_panel_orientation (), ==,
			 PANEL_ORIENT_UNKNOWN);
	g_unsetenv (ENV_PANEL_ORIENTATION);

	g_test_assert_expected_messages ();
}

/* Each panel orientation cancels the rotation a compositor applies for it. */
static void
test_panel_orientation_compensation (void)
{
	g_setenv (ENV_PANEL_ORIENTATION, "right_side_up", TRUE);
	g_assert_cmpint (detect_panel_rotation (), ==, 270);

	g_setenv (ENV_PANEL_ORIENTATION, "left_side_up", TRUE);
	g_assert_cmpint (detect_panel_rotation (), ==, 90);

	g_setenv (ENV_PANEL_ORIENTATION, "upside_down", TRUE);
	g_assert_cmpint (detect_panel_rotation (), ==, 180);

	g_setenv (ENV_PANEL_ORIENTATION, "normal", TRUE);
	g_assert_cmpint (detect_panel_rotation (), ==, 0);

	g_unsetenv (ENV_PANEL_ORIENTATION);
}

/* right_side_up cancels the default laptop orientation out to normal. */
static void
test_panel_orientation_laptop_result (void)
{
	g_setenv (ENV_PANEL_ORIENTATION, "right_side_up", TRUE);

	g_assert_cmpint (compose_orientation (configured_laptop_orientation (),
					      detect_panel_rotation ()),
			 ==, MXC_ORIENT_NORMAL);

	g_unsetenv (ENV_PANEL_ORIENTATION);
}

static void
test_laptop_orientation_unset (void)
{
	g_unsetenv (ENV_LAPTOP_ORIENTATION);

	g_assert_cmpint (configured_laptop_orientation (), ==, MXC_ORIENT_RIGHT);
}

static void
test_laptop_orientation_values (void)
{
	g_setenv (ENV_LAPTOP_ORIENTATION, "normal", TRUE);
	g_assert_cmpint (configured_laptop_orientation (), ==, MXC_ORIENT_NORMAL);

	g_setenv (ENV_LAPTOP_ORIENTATION, "left-up", TRUE);
	g_assert_cmpint (configured_laptop_orientation (), ==, MXC_ORIENT_LEFT);

	g_setenv (ENV_LAPTOP_ORIENTATION, "bottom-up", TRUE);
	g_assert_cmpint (configured_laptop_orientation (), ==, MXC_ORIENT_INVERTED);

	g_setenv (ENV_LAPTOP_ORIENTATION, "right-up", TRUE);
	g_assert_cmpint (configured_laptop_orientation (), ==, MXC_ORIENT_RIGHT);

	g_unsetenv (ENV_LAPTOP_ORIENTATION);
}

static void
test_laptop_orientation_invalid (void)
{
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
			       "*ignoring invalid*");

	g_setenv (ENV_LAPTOP_ORIENTATION, "sideways", TRUE);
	g_assert_cmpint (configured_laptop_orientation (), ==, MXC_ORIENT_RIGHT);
	g_unsetenv (ENV_LAPTOP_ORIENTATION);

	g_test_assert_expected_messages ();
}

/* The override is composed with the panel rotation, not applied raw. */
static void
test_laptop_orientation_composed_with_panel (void)
{
	g_assert_cmpint (compose_orientation (MXC_ORIENT_RIGHT, 0), ==,
			 MXC_ORIENT_RIGHT);
	g_assert_cmpint (compose_orientation (MXC_ORIENT_RIGHT, 180), ==,
			 MXC_ORIENT_LEFT);
	g_assert_cmpint (compose_orientation (MXC_ORIENT_NORMAL, 90), ==,
			 MXC_ORIENT_RIGHT);
}

int
main (int    argc,
      char **argv)
{
	g_test_init (&argc, &argv, NULL);

	g_test_add_func ("/mxc6655/panel-orientation-unset",
			 test_panel_orientation_unset);
	g_test_add_func ("/mxc6655/panel-orientation-values",
			 test_panel_orientation_values);
	g_test_add_func ("/mxc6655/panel-orientation-invalid",
			 test_panel_orientation_invalid);
	g_test_add_func ("/mxc6655/panel-orientation-compensation",
			 test_panel_orientation_compensation);
	g_test_add_func ("/mxc6655/panel-orientation-laptop-result",
			 test_panel_orientation_laptop_result);
	g_test_add_func ("/mxc6655/laptop-orientation-unset",
			 test_laptop_orientation_unset);
	g_test_add_func ("/mxc6655/laptop-orientation-values",
			 test_laptop_orientation_values);
	g_test_add_func ("/mxc6655/laptop-orientation-invalid",
			 test_laptop_orientation_invalid);
	g_test_add_func ("/mxc6655/laptop-orientation-composed-with-panel",
			 test_laptop_orientation_composed_with_panel);

	return g_test_run ();
}

/*
 * Copyright (c) 2020 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include <glib.h>

#include "accel-scale.h"

void
reset_accel_scale (AccelScale *scale)
{
	set_accel_scale (scale, 1.0);
}

void
set_accel_scale (AccelScale *scale,
		 double     value)
{
	g_return_if_fail (scale != NULL);
	g_return_if_fail (value != 0.0);

	scale->x = scale->y = scale->z = value;
}

void
copy_accel_scale (AccelScale *target,
		  AccelScale  source)
{
	g_return_if_fail (target != NULL);

	target->x = source.x;
	target->y = source.y;
	target->z = source.z;
}

/*
 * Copyright (c) 2020 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#pragma once

typedef struct {
	double x;
	double y;
	double z;
} AccelScale;

void reset_accel_scale (AccelScale *scale);
void set_accel_scale (AccelScale *scale, double value);
void copy_accel_scale (AccelScale *target, AccelScale source);

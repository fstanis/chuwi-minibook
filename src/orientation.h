/*
 * Copyright (c) 2011, 2014 Bastien Nocera <hadess@hadess.net>
 *
 * orientation_calc() from the sensorfw package
 * Copyright (C) 2009-2010 Nokia Corporation
 * Authors:
 *   Üstün Ergenoglu <ext-ustun.ergenoglu@nokia.com>
 *   Timo Rongas <ext-timo.2.rongas@nokia.com>
 *   Lihan Guo <lihan.guo@digia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#include "accel-scale.h"

typedef enum {
        ORIENTATION_UNDEFINED,
        ORIENTATION_NORMAL,
        ORIENTATION_BOTTOM_UP,
        ORIENTATION_LEFT_UP,
        ORIENTATION_RIGHT_UP
} OrientationUp;

typedef enum {
        TILT_UNDEFINED,
        TILT_VERTICAL,
        TILT_UP,
        TILT_DOWN,
        FACE_UP,
        FACE_DOWN
} Tilt;

#define ORIENTATION_UP_UP ORIENTATION_NORMAL

const char    *orientation_to_string (OrientationUp o);
OrientationUp  string_to_orientation (const char *orientation);

const char    *tilt_to_string (Tilt t);
Tilt           string_to_tilt (const char *tilt);

OrientationUp  orientation_calc      (OrientationUp prev,
				      int           x,
				      int           y,
				      int           z,
				      AccelScale    scale);

Tilt  tilt_calc (Tilt   prev,
	         int        in_x,
	         int        in_y,
	         int        in_z,
	         AccelScale scale);

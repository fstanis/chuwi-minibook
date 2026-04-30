/*
 * Copyright (c) 2021 Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#pragma once

#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include <gudev/gudev.h>

#define IS_TEST (g_getenv ("UMOCKDEV_DIR") != NULL)

typedef int IioFd;
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(IioFd, close, -1)

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FILE, fclose)

char *get_device_file (GUdevDevice *device);

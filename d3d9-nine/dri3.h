/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine X11DRV DRI3 interface
 *
 * Copyright 2014 Axel Davy
 */

#ifndef __NINE_DRI3_H
#define __NINE_DRI3_H

#include <X11/Xlib.h>

BOOL DRI3CheckExtension(Display *dpy, int major, int minor);

BOOL DRI3Open(Display *dpy, int screen, int *device_fd);

BOOL DRI3PixmapFromDmaBuf(Display *dpy, int screen, int fd, int width, int height,
        int stride, int depth, int bpp, Pixmap *pixmap);

#endif /* __NINE_DRI3_H */

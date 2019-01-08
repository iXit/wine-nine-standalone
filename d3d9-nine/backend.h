/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine D3D9 DRI backend interface
 *
 * Copyright 2019 Patrick Rudolph
 */

#ifndef __NINE_BACKEND_H
#define __NINE_BACKEND_H

#include <X11/Xlib.h>

struct DRIBackend;

BOOL DRIBackendOpen(Display *dpy, int screen, struct DRIBackend **dri_backend);

void DRIBackendClose(struct DRIBackend *dri_backend);

int DRIBackendFd(struct DRIBackend *dri_backend);

BOOL DRIBackendCheckExtension(Display *dpy);

#endif /* __NINE_BACKEND_H */

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine D3D9-NINE DRI2 interface
 *
 * Copyright 2014 Axel Davy
 * Copyright 2019 Patrick Rudolph
 */

#ifndef __NINE_DRI2_H
#define __NINE_DRI2_H

#ifdef D3D9NINE_DRI2

#include <X11/Xlib.h>

struct DRI2PixmapPriv;
struct DRI2priv;

BOOL DRI2FallbackOpen(Display *dpy, int screen, int *device_fd);

BOOL DRI2FallbackInit(Display *dpy, struct DRI2priv **priv);

void DRI2FallbackDestroy(struct DRI2priv *priv);

BOOL DRI2FallbackCheckSupport(Display *dpy);

BOOL DRI2PresentPixmap(struct DRI2priv *dri2_priv, struct DRI2PixmapPriv *dri2_pixmap_priv);

BOOL DRI2FallbackPRESENTPixmap(struct DRI2priv *dri2_priv,
        int fd, int width, int height, int stride, int depth,
        int bpp, struct DRI2PixmapPriv **dri2_pixmap_priv,
        Pixmap *pixmap);

void DRI2DestroyPixmap(struct DRI2priv *dri2_priv, struct DRI2PixmapPriv *dri2_pixmap_priv);

#endif

#endif /* __NINE_DRI2_H */

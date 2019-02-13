/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine D3D9 DRI backend interface
 *
 * Copyright 2019 Patrick Rudolph
 */

#ifndef __NINE_BACKEND_H
#define __NINE_BACKEND_H

#include <X11/Xlib.h>

struct dri_backend;
struct PRESENTpriv;
struct PRESENTPixmapPriv;
struct DRIpriv;
struct DRIPixmapPriv;
struct DRI2PixmapPriv;
typedef struct PRESENTPriv PRESENTpriv;
typedef struct PRESENTPixmapPriv PRESENTPixmapPriv;

struct D3DWindowBuffer
{
    PRESENTPixmapPriv *present_pixmap_priv;

    struct DRIPixmapPriv *dri_pixmap_priv;
};

struct dri_backend *backend_create(Display *dpy, int screen);

void DRIBackendClose(struct dri_backend *dri_backend);

int DRIBackendFd(struct dri_backend *dri_backend);

BOOL DRIBackendCheckExtension(Display *dpy);

BOOL DRIBackendD3DWindowBufferFromDmaBuf(struct dri_backend *dri_backend,
        PRESENTpriv *present_priv, struct DRIpriv *dri_priv,
        int dmaBufFd, int width, int height, int stride, int depth,
        int bpp, struct D3DWindowBuffer **out);

BOOL DRIBackendHelperCopyFront(struct dri_backend *dri_backend, PRESENTPixmapPriv *present_pixmap_priv);

BOOL DRIBackendInit(struct dri_backend *dri_backend, struct DRIpriv **dri_priv);

void DRIBackendDestroy(struct dri_backend *dri_backend, struct DRIpriv *dri_priv);

void DRIBackendPresentPixmap(struct dri_backend *dri_backend, struct DRIpriv *dri_priv,
        struct DRIPixmapPriv *dri_pixmap_priv);

void DRIBackendDestroyPixmap(struct dri_backend *dri_backend, struct DRIpriv *dri_priv,
        struct DRIPixmapPriv *dri_pixmap_priv);

#endif /* __NINE_BACKEND_H */

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

BOOL DRIBackendOpen(Display *dpy, int screen, struct DRIBackend **dri_backend);

void DRIBackendClose(struct DRIBackend *dri_backend);

int DRIBackendFd(struct DRIBackend *dri_backend);

BOOL DRIBackendCheckExtension(Display *dpy);

BOOL DRIBackendD3DWindowBufferFromDmaBuf(struct DRIBackend *dri_backend,
        PRESENTpriv *present_priv, struct DRIpriv *dri_priv,
        int dmaBufFd, int width, int height, int stride, int depth,
        int bpp, struct D3DWindowBuffer **out);

BOOL DRIBackendHelperCopyFront(struct DRIBackend *dri_backend, PRESENTPixmapPriv *present_pixmap_priv);

BOOL DRIBackendInit(struct DRIBackend *dri_backend, struct DRIpriv **dri_priv);

void DRIBackendDestroy(struct DRIBackend *dri_backend, struct DRIpriv *dri_priv);

void DRIBackendPresentPixmap(struct DRIBackend *dri_backend, struct DRIpriv *dri_priv,
        struct DRIPixmapPriv *dri_pixmap_priv);

void DRIBackendDestroyPixmap(struct DRIBackend *dri_backend, struct DRIpriv *dri_priv,
        struct DRIPixmapPriv *dri_pixmap_priv);

#endif /* __NINE_BACKEND_H */

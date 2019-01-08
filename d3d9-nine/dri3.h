/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine X11DRV DRI3 interface
 *
 * Copyright 2014 Axel Davy
 */

#ifndef __NINE_DRI3_H
#define __NINE_DRI3_H

#include <wingdi.h>
#include <X11/Xlib.h>

BOOL DRI3CheckExtension(Display *dpy, int major, int minor);

#ifdef D3D9NINE_DRI2
struct DRI2priv;

BOOL DRI2FallbackInit(Display *dpy, struct DRI2priv **priv);

void DRI2FallbackDestroy(struct DRI2priv *priv);

BOOL DRI2FallbackCheckSupport(Display *dpy);
#endif
LONG PRESENTGetNewSerial(void);

BOOL PRESENTCheckExtension(Display *dpy, int major, int minor);

BOOL DRI3Open(Display *dpy, int screen, int *device_fd);

#ifdef D3D9NINE_DRI2
BOOL DRI2FallbackOpen(Display *dpy, int screen, int *device_fd);
#endif

BOOL DRI3PixmapFromDmaBuf(Display *dpy, int screen, int fd, int width, int height,
        int stride, int depth, int bpp, Pixmap *pixmap);

BOOL DRI3DmaBufFromPixmap(Display *dpy, Pixmap pixmap, int *fd, int *width, int *height,
        int *stride, int *depth, int *bpp);

typedef struct PRESENTPriv PRESENTpriv;
typedef struct PRESENTPixmapPriv PRESENTPixmapPriv;

BOOL PRESENTInit(Display *dpy, PRESENTpriv **present_priv);

/* will clean properly and free all PRESENTPixmapPriv associated to PRESENTpriv.
 * PRESENTPixmapPriv should not be freed by something else.
 * If never a PRESENTPixmapPriv has to be destroyed,
 * please destroy the current PRESENTpriv and create a new one.
 * This will take care than all pixmaps are released */
void PRESENTDestroy(Display *dpy, PRESENTpriv *present_priv);

BOOL PRESENTPixmapInit(PRESENTpriv *present_priv, Pixmap pixmap, PRESENTPixmapPriv **present_pixmap_priv);

#ifdef D3D9NINE_DRI2
BOOL DRI2PresentPixmap(struct DRI2priv *dri2_priv, PRESENTPixmapPriv *present_pixmap_priv);

BOOL DRI2FallbackPRESENTPixmap(PRESENTpriv *present_priv, struct DRI2priv *priv,
        int fd, int width, int height, int stride, int depth,
        int bpp, PRESENTPixmapPriv **present_pixmap_priv);
#endif

BOOL PRESENTTryFreePixmap(Display *dpy, PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTHelperCopyFront(Display *dpy, PRESENTPixmapPriv *present_pixmap_priv);

struct DRIBackend;

BOOL PRESENTPixmapPrepare(XID window, PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTPixmap(XID window, PRESENTPixmapPriv *present_pixmap_priv,
        const UINT PresentationInterval, const BOOL PresentAsync, const BOOL SwapEffectCopy,
        const RECT *pSourceRect, const RECT *pDestRect, const RGNDATA *pDirtyRegion);

BOOL PRESENTWaitPixmapReleased(PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTIsPixmapReleased(PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTWaitReleaseEvent(PRESENTpriv *present_priv);

#endif /* __NINE_DRI3_H */

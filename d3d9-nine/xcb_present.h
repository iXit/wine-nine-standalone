/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine XCB PRESENT interface
 *
 * Copyright 2014 Axel Davy
 * Copyright 2019 Patrick Rudolph
 */

#ifndef __NINE_XCB_PRESENT_H
#define __NINE_XCB_PRESENT_H

#include <wingdi.h>
#include <X11/Xlib.h>

LONG PRESENTGetNewSerial(void);

BOOL PRESENTCheckExtension(Display *dpy, int major, int minor);

typedef struct PRESENTPriv PRESENTpriv;
typedef struct PRESENTPixmapPriv PRESENTPixmapPriv;

BOOL PRESENTInit(Display *dpy, PRESENTpriv **present_priv);

/* will clean properly and free all PRESENTPixmapPriv associated to PRESENTpriv.
 * PRESENTPixmapPriv should not be freed by something else.
 * If never a PRESENTPixmapPriv has to be destroyed,
 * please destroy the current PRESENTpriv and create a new one.
 * This will take care than all pixmaps are released */
void PRESENTDestroy(PRESENTpriv *present_priv);

BOOL PRESENTGetGeom(PRESENTpriv *present_priv, XID window, int *width, int *height, int *depth);
BOOL PRESENTGeomUpdated(PRESENTpriv *present_priv);

BOOL PRESENTPixmapCreate(PRESENTpriv *present_priv, int screen,
        Pixmap *pixmap, int width, int height, int stride, int depth,
        int bpp);

BOOL PRESENTPixmapInit(PRESENTpriv *present_priv, Pixmap pixmap, PRESENTPixmapPriv **present_pixmap_priv);

BOOL PRESENTTryFreePixmap(PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTHelperCopyFront(PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTPixmapPrepare(XID window, PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTPixmap(XID window, PRESENTPixmapPriv *present_pixmap_priv,
        const UINT PresentationInterval, const BOOL PresentAsync, const BOOL SwapEffectCopy,
        const RECT *pSourceRect, const RECT *pDestRect, const RGNDATA *pDirtyRegion);

BOOL PRESENTWaitPixmapReleased(PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTIsPixmapReleased(PRESENTPixmapPriv *present_pixmap_priv);

BOOL PRESENTWaitReleaseEvent(PRESENTpriv *present_priv);

#endif /* __NINE_XCB_PRESENT_H */

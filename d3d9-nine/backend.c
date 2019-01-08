/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine D3D9 DRI backend interface
 *
 * Copyright 2019 Patrick Rudolph
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015-2019 Patrick Rudolph
 */

#include <windows.h>
#include <wine/debug.h>
#include <X11/Xlib-xcb.h>
#include <stdlib.h>

#include "backend.h"
#include "dri3.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

struct DRIBackend {
    Display *dpy;
    int fd;
    int screen;
};

#ifdef D3D9NINE_DRI2
int is_dri2_fallback;
#endif

BOOL DRIBackendOpen(Display *dpy, int screen, struct DRIBackend **dri_backend)
{
    WINE_TRACE("dpy=%p screen=%d dri_backend=%p\n", dpy, screen, dri_backend);

    if (!dri_backend)
        return FALSE;

    *dri_backend = HeapAlloc(GetProcessHeap(), 0, sizeof(struct DRIBackend));
    if (!*dri_backend)
        return FALSE;

    (*dri_backend)->dpy = dpy;
    (*dri_backend)->screen = screen;

    if (DRI3Open(dpy, screen, &((*dri_backend)->fd)))
        return TRUE;

    WINE_ERR("DRI3Open failed (fd=%d)\n", (*dri_backend)->fd);

#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback && DRI2FallbackOpen(dpy, screen, &((*dri_backend)->fd)))
        return TRUE;
    WINE_ERR("DRI2Open failed (fd=%d)\n", (*dri_backend)->fd);
#endif

    HeapFree(GetProcessHeap(), 0, dri_backend);
    return FALSE;
}

int DRIBackendFd(struct DRIBackend *dri_backend)
{
    WINE_TRACE("dri_backend=%p\n", dri_backend);

    return dri_backend ? dri_backend->fd : -1;
}

void DRIBackendClose(struct DRIBackend *dri_backend)
{
    WINE_TRACE("dri_backend=%p\n", dri_backend);

    if (dri_backend)
    {
        HeapFree(GetProcessHeap(), 0, dri_backend);
    }
}

BOOL DRIBackendCheckExtension(Display *dpy)
{
    WINE_TRACE("dpy=%p\n", dpy);

    if (!dpy)
        return FALSE;

    if (!DRI3CheckExtension(dpy, 1, 0))
    {
#ifndef D3D9NINE_DRI2
        WINE_ERR("Unable to query DRI3.\n");
        return FALSE;
#else
        WINE_ERR("Unable to query DRI3. Trying DRI2 fallback (slower performance).\n");
        is_dri2_fallback = 1;
        if (!DRI2FallbackCheckSupport(dpy))
        {
            WINE_ERR("DRI2 fallback unsupported\n");
            return FALSE;
        }
#endif
    }
    return TRUE;
}

BOOL DRIBackendD3DWindowBufferFromDmaBuf(struct DRIBackend *dri_backend,
        PRESENTpriv *present_priv, struct DRI2priv *dri2_priv,
        int dmaBufFd, int width, int height, int stride, int depth,
        int bpp, struct D3DWindowBuffer **out)
{
    Pixmap pixmap;

    WINE_TRACE("dri_backend=%p present_priv=%p dmaBufFd=%d\n",
            dri_backend, present_priv, dmaBufFd);

    if (!out)
        return FALSE;

    *out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct D3DWindowBuffer));
    if (!*out)
        return FALSE;

#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback)
    {
        if (!DRI2FallbackPRESENTPixmap(present_priv, dri2_priv,
                dmaBufFd, width, height, stride, depth,
                bpp, &((*out)->present_pixmap_priv)))
        {
            WINE_ERR("DRI2FallbackPRESENTPixmap failed\n");
            HeapFree(GetProcessHeap(), 0, *out);
            return FALSE;
        }
        return TRUE;
    }
#endif

    if (!DRI3PixmapFromDmaBuf(dri_backend->dpy, dri_backend->screen,
            dmaBufFd, width, height, stride, depth, bpp, &pixmap))
    {
        WINE_ERR("DRI3PixmapFromDmaBuf failed\n");
        HeapFree(GetProcessHeap(), 0, *out);
        return FALSE;
    }

    if (!PRESENTPixmapInit(present_priv, pixmap, &((*out)->present_pixmap_priv)))
    {
        WINE_ERR("PRESENTPixmapInit failed\n");
        HeapFree(GetProcessHeap(), 0, *out);
        return FALSE;
    }

    return TRUE;
}

BOOL DRIBackendHelperCopyFront(Display *dpy, PRESENTPixmapPriv *present_pixmap_priv)
{
#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback)
        return FALSE;
#endif
    if (PRESENTHelperCopyFront(dpy, present_pixmap_priv))
        return TRUE;
    else
        return FALSE;
}

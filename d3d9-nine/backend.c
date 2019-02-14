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
#include "dri2.h"
#include "dri3.h"
#include "xcb_present.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

struct dri_backend *backend_create(Display *dpy, int screen)
{
    struct dri_backend *dri_backend;

    WINE_TRACE("dpy=%p screen=%d\n", dpy, screen);

    dri_backend = HeapAlloc(GetProcessHeap(), 0, sizeof(struct dri_backend));
    if (!dri_backend)
        return NULL;

    dri_backend->dpy = dpy;
    dri_backend->fd = -1;
    dri_backend->screen = screen;
    dri_backend->type = TYPE_INVALID;
    dri_backend->funcs = NULL;
    dri_backend->priv = NULL;

    if (DRI3Open(dri_backend->dpy, dri_backend->screen, &dri_backend->fd))
    {
        dri_backend->type = TYPE_DRI3;
        return dri_backend;
    }
    WINE_ERR("DRI3Open failed (fd=%d)\n", dri_backend->fd);

#ifdef D3D9NINE_DRI2
    if (DRI2FallbackOpen(dri_backend->dpy, dri_backend->screen, &dri_backend->fd))
    {
        dri_backend->type = TYPE_DRI2;
        return dri_backend;
    }
    WINE_ERR("DRI2Open failed (fd=%d)\n", dri_backend->fd);
#endif

    HeapFree(GetProcessHeap(), 0, dri_backend);
    return NULL;
}

void backend_destroy(struct dri_backend *dri_backend)
{
    WINE_TRACE("dri_backend=%p\n", dri_backend);

    if (!dri_backend)
        return;

#ifdef D3D9NINE_DRI2
    if (dri_backend->priv && dri_backend->type == TYPE_DRI2)
        DRI2FallbackDestroy(dri_backend->priv);
#endif

    HeapFree(GetProcessHeap(), 0, dri_backend);
}

int backend_get_fd(const struct dri_backend *dri_backend)
{
    WINE_TRACE("dri_backend=%p\n", dri_backend);

    return dri_backend ? dri_backend->fd : -1;
}

BOOL DRIBackendCheckExtension(Display *dpy)
{
    WINE_TRACE("dpy=%p\n", dpy);

    if (!dpy)
        return FALSE;

    if (!DRI3CheckExtension(dpy))
    {
#ifndef D3D9NINE_DRI2
        WINE_ERR("Unable to query DRI3.\n");
        return FALSE;
#else
        WINE_ERR("Unable to query DRI3. Trying DRI2 fallback (slower performance).\n");

        if (!DRI2FallbackCheckSupport(dpy))
        {
            WINE_ERR("DRI2 fallback unsupported\n");
            return FALSE;
        }
#endif
    }
    return TRUE;
}

BOOL DRIBackendD3DWindowBufferFromDmaBuf(struct dri_backend *dri_backend,
        PRESENTpriv *present_priv,
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
    if (dri_backend->type == TYPE_DRI2)
    {
        if (!PRESENTPixmapCreate(present_priv, dri_backend->screen, &pixmap,
                width, height, stride, depth, bpp))
        {
            HeapFree(GetProcessHeap(), 0, *out);
            WINE_ERR("Failed to create pixmap\n");
            return FALSE;
        }

        if (!DRI2FallbackPRESENTPixmap(dri_backend->priv,
                dmaBufFd, width, height, stride, depth, bpp,
                &(*out)->priv, &pixmap))
        {
            WINE_ERR("DRI2FallbackPRESENTPixmap failed\n");
            HeapFree(GetProcessHeap(), 0, *out);
            return FALSE;
        }
    }
    else
#endif
    if (dri_backend->type == TYPE_DRI3)
    {
        if (!DRI3PixmapFromDmaBuf(dri_backend->dpy, dri_backend->screen,
                dmaBufFd, width, height, stride, depth, bpp, &pixmap))
        {
            WINE_ERR("DRI3PixmapFromDmaBuf failed\n");
            HeapFree(GetProcessHeap(), 0, *out);
            return FALSE;
        }
    }

    if (!PRESENTPixmapInit(present_priv, pixmap, &((*out)->present_pixmap_priv)))
    {
        WINE_ERR("PRESENTPixmapInit failed\n");
        HeapFree(GetProcessHeap(), 0, *out);
        return FALSE;
    }

    return TRUE;
}

BOOL DRIBackendHelperCopyFront(struct dri_backend *dri_backend, PRESENTPixmapPriv *present_pixmap_priv)
{
#ifdef D3D9NINE_DRI2
    if (dri_backend->type == TYPE_DRI2)
        return FALSE;
#endif
    if (PRESENTHelperCopyFront(present_pixmap_priv))
        return TRUE;
    else
        return FALSE;
}

BOOL DRIBackendInit(struct dri_backend *dri_backend)
{
    WINE_TRACE("dri_backend=%p\n", dri_backend);

#ifdef D3D9NINE_DRI2
    if (dri_backend->type == TYPE_DRI2 &&
           !DRI2FallbackInit(dri_backend->dpy, &dri_backend->priv))
        return FALSE;
#endif
    return TRUE;
}

void DRIBackendPresentPixmap(struct dri_backend *dri_backend, struct buffer_priv *buffer_priv)
{
    WINE_TRACE("dri_backend=%p priv=%p\n", dri_backend, buffer_priv);

#ifdef D3D9NINE_DRI2
    if (dri_backend->type == TYPE_DRI2)
        DRI2PresentPixmap(dri_backend->priv, buffer_priv);
#endif
}

void DRIBackendDestroyPixmap(struct dri_backend *dri_backend, struct buffer_priv *buffer_priv)
{
    WINE_TRACE("dri_backend=%p priv=%p\n", dri_backend, buffer_priv);

#ifdef D3D9NINE_DRI2
    if (dri_backend->type == TYPE_DRI2)
        DRI2DestroyPixmap(dri_backend->priv, buffer_priv);
#endif
}

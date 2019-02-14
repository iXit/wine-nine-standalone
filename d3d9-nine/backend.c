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
#include "xcb_present.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

extern const struct dri_backend_funcs dri3_funcs;
#ifdef D3D9NINE_DRI2
extern const struct dri_backend_funcs dri2_funcs;
#endif

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
    dri_backend->funcs = NULL;
    dri_backend->priv = NULL;

    if (dri3_funcs.create(dri_backend->dpy, dri_backend->screen, &dri_backend->fd))
    {
        dri_backend->funcs = &dri3_funcs;
        return dri_backend;
    }

    WINE_ERR("DRI3Open failed (fd=%d)\n", dri_backend->fd);

#ifdef D3D9NINE_DRI2
    if (dri2_funcs.create(dri_backend->dpy, dri_backend->screen, &dri_backend->fd))
    {
        dri_backend->funcs = &dri2_funcs;
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

    if (dri_backend->priv)
        dri_backend->funcs->destroy(dri_backend->priv);

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

    if (!dri3_funcs.probe(dpy))
    {
#ifndef D3D9NINE_DRI2
        WINE_ERR("Unable to query DRI3.\n");
        return FALSE;
#else
        WINE_ERR("Unable to query DRI3. Trying DRI2 fallback (slower performance).\n");

        if (!dri2_funcs.probe(dpy))
        {
            WINE_ERR("DRI2 fallback unsupported\n");
            return FALSE;
        }
#endif
    }
    return TRUE;
}

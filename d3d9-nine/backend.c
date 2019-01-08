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
extern int is_dri2_fallback;
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

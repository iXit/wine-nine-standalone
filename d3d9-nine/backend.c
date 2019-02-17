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

static const struct dri_backend_funcs *backends[] = {
    &dri3_funcs,
#ifdef D3D9NINE_DRI2
    &dri2_funcs,
#endif
};

static const int backends_count = sizeof(backends) / sizeof(*backends);

static const char *backend_getenv()
{
    const char *env = getenv("D3D_BACKEND");
    static BOOL first = TRUE;

    if (env && first)
    {
        first = FALSE;
        WINE_WARN("Backend overwritten by D3D_BACKEND: %s\n", env);
    }

    return env;
}

BOOL backend_probe(Display *dpy)
{
    int i;
    const char *env;

    WINE_TRACE("dpy=%p\n", dpy);

    if (!dpy)
        return FALSE;

    env = backend_getenv();

    for (i = 0; i < backends_count; ++i)
    {
        if (env && strcmp(env, backends[i]->name))
            continue;

        if (backends[i]->probe(dpy))
        {
            if (i != 0)
                WINE_ERR("DRI3 backend not active (slower performance).\n");

            return TRUE;
        }

        WINE_ERR("Error probing backend %s\n", backends[i]->name);
    }

    return FALSE;
}

struct dri_backend *backend_create(Display *dpy, int screen)
{
    struct dri_backend *dri_backend;
    int i;
    const char *env;

    WINE_TRACE("dpy=%p screen=%d\n", dpy, screen);

    dri_backend = HeapAlloc(GetProcessHeap(), 0, sizeof(struct dri_backend));
    if (!dri_backend)
        return NULL;

    dri_backend->dpy = dpy;
    dri_backend->fd = -1;
    dri_backend->screen = screen;
    dri_backend->funcs = NULL;
    dri_backend->priv = NULL;

    env = backend_getenv();

    for (i = 0; i < backends_count; ++i)
    {
        if (env && strcmp(env, backends[i]->name))
            continue;

        if (backends[i]->create(dri_backend->dpy, dri_backend->screen, &dri_backend->fd, &dri_backend->priv))
        {
            WINE_TRACE("Active backend: %s\n", backends[i]->name);
            dri_backend->funcs = backends[i];
            return dri_backend;
        }

        WINE_ERR("Error creating backend %s (fd=%d)\n", backends[i]->name, dri_backend->fd);
    }

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

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine D3D9 DRI backend interface
 *
 * Copyright 2019 Patrick Rudolph
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015-2019 Patrick Rudolph
 */

#include <windows.h>
#include <X11/Xlib-xcb.h>
#include <stdlib.h>

#include "../common/debug.h"
#include "../common/registry.h"
#include "backend.h"
#include "xcb_present.h"

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
        WARN("Backend overwritten by D3D_BACKEND: %s\n", env);
    }

    return env;
}

BOOL backend_probe(Display *dpy)
{
    int i;
    const char *env;
    struct dri_backend_priv *p;

    TRACE("dpy=%p\n", dpy);

    if (!dpy)
        return FALSE;

    env = backend_getenv();

    for (i = 0; i < backends_count; ++i)
    {
        if (env && strcmp(env, backends[i]->name))
            continue;

        if (!backends[i]->probe(dpy))
        {
            TRACE("Error probing backend %s\n", backends[i]->name);
            continue;
        }

        if (!backends[i]->create(dpy, DefaultScreen(dpy), &p))
        {
            TRACE("Error creating backend %s\n", backends[i]->name);
            continue;
        }

        if (!backends[i]->init(p))
        {
            TRACE("Error initializing backend %s\n", backends[i]->name);
            backends[i]->destroy(p);
            continue;
        }

        backends[i]->destroy(p);

        if (i != 0)
            fprintf(stderr, "\033[1;31mDRI3 backend not active (slower performance)\033[0m\n");

        return TRUE;
    }

    return FALSE;
}

struct dri_backend *backend_create(Display *dpy, int screen)
{
    struct dri_backend *dri_backend;
    int i;
    const char *env;

    TRACE("dpy=%p screen=%d\n", dpy, screen);

    dri_backend = HeapAlloc(GetProcessHeap(), 0, sizeof(struct dri_backend));
    if (!dri_backend)
        return NULL;

    dri_backend->funcs = NULL;
    dri_backend->priv = NULL;

    env = backend_getenv();

    for (i = 0; i < backends_count; ++i)
    {
        if (env && strcmp(env, backends[i]->name))
            continue;

        if (!backends[i]->probe(dpy))
            continue;

        if (backends[i]->create(dpy, screen, &dri_backend->priv))
        {
            TRACE("Active backend: %s\n", backends[i]->name);

            dri_backend->funcs = backends[i];
            return dri_backend;
        }

        ERR("Error creating backend %s\n", backends[i]->name);
    }

    HeapFree(GetProcessHeap(), 0, dri_backend);
    return NULL;
}

void backend_destroy(struct dri_backend *dri_backend)
{
    TRACE("dri_backend=%p\n", dri_backend);

    if (!dri_backend)
        return;

    if (dri_backend->priv)
        dri_backend->funcs->destroy(dri_backend->priv);

    HeapFree(GetProcessHeap(), 0, dri_backend);
}

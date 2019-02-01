/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <d3dadapter/drm.h>
#include <wine/debug.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

#include "library.h"
#include "registry.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

#define D3DADAPTER9 "d3dadapter9.so.1"

static void *open_d3dadapter(char *paths, char **res, char **err)
{
    char *next, *end, *p, *lasterr = NULL;
    void *handle = NULL;
    char path[MAX_PATH];
    int len;

    end = paths + strlen(paths);
    for (p = paths; p < end; p = next + 1)
    {
        next = strchr(p, ':');
        if (!next)
            next = end;

        len = next - p;
        snprintf(path, sizeof(path), "%.*s", len, p);

        WINE_TRACE("Trying to load '%s'\n", path);
        handle = dlopen(path, RTLD_GLOBAL | RTLD_NOW);

        if (handle) {
            if (res)
              *res = strdup(path);

            break;
        }

        free(lasterr);
        lasterr = strdup(dlerror());

        WINE_TRACE("Failed to load '%s': %s\n", path, lasterr);
    }

    if (handle || !err)
    {
        free(lasterr);
        lasterr = NULL;
    }

    if (handle)
        WINE_TRACE("Loaded '%s'\n", path);

    if (err)
        *err = lasterr;

    return handle;
}

void *common_load_d3dadapter(char **path, char **err)
{
    static void *handle = NULL;
    char *env, *reg;

    env = getenv("D3D_MODULE_PATH");
    if (env)
    {
        handle = open_d3dadapter(env, path, err);

        if (!handle)
            WINE_ERR("Failed to load " D3DADAPTER9 " set by D3D_MODULE_PATH (%s)\n", env);

        return handle;
    }

    if (common_get_registry_string(reg_path_nine, reg_key_module_path, &reg))
    {
        handle = open_d3dadapter(reg, path, err);

        if (!handle)
            WINE_ERR("Failed to load " D3DADAPTER9 " set by ModulePath (%s)\n", reg);

        HeapFree(GetProcessHeap(), 0, reg);

        return handle;
    }

#if defined(D3D9NINE_MODULEPATH)
    handle = open_d3dadapter(D3D9NINE_MODULEPATH, path, err);

    if (!handle)
        WINE_ERR("Failed to load " D3DADAPTER9 " set by builtin default '%s'\n",
                 D3D9NINE_MODULEPATH);

    return handle;
#else
    WINE_ERR("d3d9-nine.dll was built without default module path.\n"
             "Setting the envvar D3D_MODULE_PATH or "
             "regkey Software\\Wine\\Direct3DNine\\ModulePath is required\n");

    return NULL;
#endif
}

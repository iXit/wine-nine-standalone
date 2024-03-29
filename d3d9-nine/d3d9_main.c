/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Direct3D 9
 *
 * Copyright 2002-2003 Jason Edmeades
 * Copyright 2002-2003 Raphael Junqueira
 * Copyright 2005 Oliver Stieber
 * Copyright 2015 Patrick Rudolph
 */

#include <d3d9.h>
#include <fcntl.h>

#include "../common/debug.h"
#include "d3dadapter9.h"
#include "wndproc.h"
#include "shader_validator.h"

static int D3DPERF_event_level = 0;
static Display *gdi_display;

void WINAPI DebugSetMute(void)
{
    /* nothing to do */
}

IDirect3D9 * WINAPI DECLSPEC_HOTPATCH Direct3DCreate9(UINT sdk_version)
{
    IDirect3D9 *native;
    TRACE("sdk_version %#x.\n", sdk_version);

    if (SUCCEEDED(d3dadapter9_new(gdi_display, FALSE, (IDirect3D9Ex **)&native)))
        return native;

    return NULL;
}

HRESULT WINAPI DECLSPEC_HOTPATCH Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex **d3d9ex)
{
    TRACE("sdk_version %#x, d3d9ex %p.\n", sdk_version, d3d9ex);

    return d3dadapter9_new(gdi_display, TRUE, d3d9ex);
}

/*******************************************************************
 *       Direct3DShaderValidatorCreate9 (D3D9.@)
 *
 * No documentation available for this function.
 * SDK only says it is internal and shouldn't be used.
 */

void* WINAPI Direct3DShaderValidatorCreate9(void)
{
    IDirect3DShaderValidator9Impl* object =
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    sizeof(IDirect3DShaderValidator9Impl));

    object->lpVtbl = &IDirect3DShaderValidator9Vtbl;
    object->ref = 1;

    TRACE("Returning interface %p\n", object);
    return (void*) object;
}

/*******************************************************************
 *       DllMain
 */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            if (!(gdi_display = XOpenDisplay( NULL )))
            {
                ERR("Failed to open display\n");
                return FALSE;
            }

            fcntl( ConnectionNumber(gdi_display), F_SETFD, 1 ); /* set close on exec flag */

            nine_dll_init(inst);
            break;
        case DLL_PROCESS_DETACH:
            if (!reserved)
                return nine_dll_destroy(inst);
            break;
    }

    return TRUE;
}

/***********************************************************************
 *              D3DPERF_BeginEvent (D3D9.@)
 */
int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, const WCHAR *name)
{
    TRACE("color 0x%08x, name %s.\n", (UINT)color, nine_dbgstr_w(name));

    return D3DPERF_event_level++;
}

/***********************************************************************
 *              D3DPERF_EndEvent (D3D9.@)
 */
int WINAPI D3DPERF_EndEvent(void)
{
    TRACE("(void) : stub\n");

    return --D3DPERF_event_level;
}

/***********************************************************************
 *              D3DPERF_GetStatus (D3D9.@)
 */
DWORD WINAPI D3DPERF_GetStatus(void)
{
    FIXME("(void) : stub\n");

    return 0;
}

/***********************************************************************
 *              D3DPERF_SetOptions (D3D9.@)
 *
 */
void WINAPI D3DPERF_SetOptions(DWORD options)
{
  FIXME("(%#x) : stub\n", (UINT)options);
}

/***********************************************************************
 *              D3DPERF_QueryRepeatFrame (D3D9.@)
 */
BOOL WINAPI D3DPERF_QueryRepeatFrame(void)
{
    FIXME("(void) : stub\n");

    return FALSE;
}

/***********************************************************************
 *              D3DPERF_SetMarker (D3D9.@)
 */
void WINAPI D3DPERF_SetMarker(D3DCOLOR color, const WCHAR *name)
{
    FIXME("color 0x%08x, name %s stub!\n", (UINT)color, nine_dbgstr_w(name));
}

/***********************************************************************
 *              D3DPERF_SetRegion (D3D9.@)
 */
void WINAPI D3DPERF_SetRegion(D3DCOLOR color, const WCHAR *name)
{
    FIXME("color 0x%08x, name %s stub!\n", (UINT)color, nine_dbgstr_w(name));
}

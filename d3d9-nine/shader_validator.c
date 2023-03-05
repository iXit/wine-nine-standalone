/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Direct3D 9 ShaderValidator
 *
 * Copyright 2016 Patrick Rudolph
 */

#include <windows.h>

#include "../common/debug.h"
#include "shader_validator.h"

static HRESULT WINAPI IDirect3DShaderValidator9Impl_QueryInterface(IDirect3DShaderValidator9Impl *This,
        REFIID riid, LPVOID* ppobj)
{
    /* TODO: AddRef(iface). */
    *ppobj = This;
    TRACE("This=%p, riid=%s, object=%p.\n", This, nine_dbgstr_guid(riid), ppobj);

    return S_OK;
}

static ULONG WINAPI IDirect3DShaderValidator9Impl_AddRef(IDirect3DShaderValidator9Impl *This)
{
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("This=%p increasing refcount to %u.\n", This, (UINT)ref);

    return ref;
}

static ULONG WINAPI IDirect3DShaderValidator9Impl_Release(IDirect3DShaderValidator9Impl *This)
{
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("This=%p decreasing refcount to %u.\n", This, (UINT)ref);

    if (ref == 0)
        HeapFree(GetProcessHeap(), 0, This);

    return ref;
}

static LONG WINAPI IDirect3DShaderValidator9Impl_Begin(IDirect3DShaderValidator9Impl *This,
        void* callback, void* unknown1, ULONG unknown2)
{
    TRACE("This=%p, callback=%p, unknown1=%p, unknown2=%u\n",
          This, callback, unknown1, (UINT)unknown2);
    return 1;
}

static LONG WINAPI IDirect3DShaderValidator9Impl_Instruction(IDirect3DShaderValidator9Impl *This,
        const char* unknown1, unsigned int unknown2, const unsigned long* unknown3, unsigned int unknown4)
{
    TRACE("This=%p, unknown1=%p, unknown2=%u, unknown3=%p, unknown4=%u\n",
          This, unknown1, unknown2, unknown3, unknown4);
    return 1;
}

static LONG WINAPI IDirect3DShaderValidator9Impl_End(IDirect3DShaderValidator9Impl *This)
{
    TRACE("This=%p\n", This);
    return 1;
}

const void *IDirect3DShaderValidator9Vtbl[] =
{
    /* IUnknown */
    IDirect3DShaderValidator9Impl_QueryInterface,
    IDirect3DShaderValidator9Impl_AddRef,
    IDirect3DShaderValidator9Impl_Release,
    /* IDirect3DShaderValidator9 */
    IDirect3DShaderValidator9Impl_Begin,
    IDirect3DShaderValidator9Impl_Instruction,
    IDirect3DShaderValidator9Impl_End
};


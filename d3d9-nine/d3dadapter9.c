/*
 * Wine IDirect3D9 interface using ID3DAdapter9
 *
 * Copyright 2013 Joakim Sindholt
 *                Christoph Bumiller
 * Copyright 2014 David Heidelberger
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015 Nick Sarnie
 *                Patrick Rudolph
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

#include <d3dadapter/d3dadapter9.h>
#include "present.h"
#include "device_wrap.h"

/* this represents a snapshot taken at the moment of creation */
struct output
{
    D3DDISPLAYROTATION rotation; /* current rotation */
    D3DDISPLAYMODEEX *modes;
    unsigned nmodes;
    unsigned nmodesalloc;
    unsigned current; /* current mode num */

    HMONITOR monitor;
};

struct adapter_group
{
    struct output *outputs;
    unsigned noutputs;
    unsigned noutputsalloc;

    /* override driver provided DeviceName with this to homogenize device names
     * with wine */
    WCHAR devname[32];

    /* driver stuff */
    ID3DAdapter9 *adapter;
};

struct adapter_map
{
    unsigned group;
    unsigned master;
};

struct d3dadapter9
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    /* adapter groups and mappings */
    struct adapter_group *groups;
    struct adapter_map *map;
    unsigned nadapters;
    unsigned ngroups;
    unsigned ngroupsalloc;

    /* true if it implements IDirect3D9Ex */
    boolean ex;
    Display *gdi_display;
};

/* convenience wrapper for calls into ID3D9Adapter */
#define ADAPTER_GROUP \
    This->groups[This->map[Adapter].group]

#define ADAPTER_PROC(name, ...) \
    ID3DAdapter9_##name(ADAPTER_GROUP.adapter, ## __VA_ARGS__)

#define ADAPTER_OUTPUT \
    ADAPTER_GROUP.outputs[Adapter-This->map[Adapter].master]

static HRESULT WINAPI d3dadapter9_CheckDeviceFormat(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
        DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat);

static ULONG WINAPI d3dadapter9_AddRef(struct d3dadapter9 *This)
{
    ULONG refs = InterlockedIncrement(&This->refs);
    TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI d3dadapter9_Release(struct d3dadapter9 *This)
{
    ULONG refs = InterlockedDecrement(&This->refs);
    TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0)
    {
        /* dtor */
        if (This->map)
        {
            HeapFree(GetProcessHeap(), 0, This->map);
        }

        if (This->groups)
        {
            int i, j;
            for (i = 0; i < This->ngroups; ++i)
            {
                if (This->groups[i].outputs)
                {
                    for (j = 0; j < This->groups[i].noutputs; ++j)
                    {
                        if (This->groups[i].outputs[j].modes)
                        {
                            HeapFree(GetProcessHeap(), 0,
                                     This->groups[i].outputs[j].modes);
                        }
                    }
                    HeapFree(GetProcessHeap(), 0, This->groups[i].outputs);
                }

                if (This->groups[i].adapter)
                    ID3DAdapter9_Release(This->groups[i].adapter);
            }
            HeapFree(GetProcessHeap(), 0, This->groups);
        }

        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI d3dadapter9_QueryInterface(struct d3dadapter9 *This,
        REFIID riid, void **ppvObject)
{
    if (!ppvObject)
        return E_POINTER;

    if ((IsEqualGUID(&IID_IDirect3D9Ex, riid) && This->ex) ||
            IsEqualGUID(&IID_IDirect3D9, riid) ||
            IsEqualGUID(&IID_IUnknown, riid))
    {
        *ppvObject = This;
        d3dadapter9_AddRef(This);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static HRESULT WINAPI d3dadapter9_RegisterSoftwareDevice(struct d3dadapter9 *This,
        void *pInitializeFunction)
{
    FIXME("(%p, %p), stub!\n", This, pInitializeFunction);
    return D3DERR_INVALIDCALL;
}

static UINT WINAPI d3dadapter9_GetAdapterCount(struct d3dadapter9 *This)
{
    return This->nadapters;
}

static HRESULT WINAPI d3dadapter9_GetAdapterIdentifier(struct d3dadapter9 *This,
        UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier)
{
    HRESULT hr;
    HKEY regkey;

    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    hr = ADAPTER_PROC(GetAdapterIdentifier, Flags, pIdentifier);
    if (SUCCEEDED(hr))
    {
        /* Override the driver provided DeviceName with what Wine provided */
        ZeroMemory(pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName));
        if (!WideCharToMultiByte(CP_ACP, 0, ADAPTER_GROUP.devname, -1,
                pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName), NULL, NULL))
            return D3DERR_INVALIDCALL;

        TRACE("DeviceName overriden: %s\n", pIdentifier->DeviceName);

        /* Override PCI IDs when wined3d registry keys are set */
        if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\Direct3DNine", &regkey))
        {
            DWORD type, data;
            DWORD size = sizeof(DWORD);

            if (!RegQueryValueExA(regkey, "VideoPciDeviceID", 0, &type, (BYTE *)&data, &size) &&
                    (type == REG_DWORD) && (size == sizeof(DWORD)))
                pIdentifier->DeviceId = data;
            if (size != sizeof(DWORD))
            {
                ERR("VideoPciDeviceID is not a DWORD\n");
                size = sizeof(DWORD);
            }
            if (!RegQueryValueExA(regkey, "VideoPciVendorID", 0, &type, (BYTE *)&data, &size) &&
                    (type == REG_DWORD) && (size == sizeof(DWORD)))
                pIdentifier->VendorId = data;
            if (size != sizeof(DWORD))
                ERR("VideoPciVendorID is not a DWORD\n");
            RegCloseKey(regkey);

            TRACE("DeviceId:VendorId overridden: %04X:%04X\n", pIdentifier->DeviceId, pIdentifier->VendorId);
        }
    }
    return hr;
}

static UINT WINAPI d3dadapter9_GetAdapterModeCount(struct d3dadapter9 *This,
        UINT Adapter, D3DFORMAT Format)
{
    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    if (FAILED(d3dadapter9_CheckDeviceFormat(This, Adapter, D3DDEVTYPE_HAL,
            Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, Format)))
    {
        WARN("DeviceFormat not available.\n");
        return 0;
    }

    TRACE("%u modes.\n", ADAPTER_OUTPUT.nmodes);
    return ADAPTER_OUTPUT.nmodes;
}

static HRESULT WINAPI d3dadapter9_EnumAdapterModes(struct d3dadapter9 *This,
        UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode)
{
    HRESULT hr;

    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    hr = d3dadapter9_CheckDeviceFormat(This, Adapter, D3DDEVTYPE_HAL,
            Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, Format);

    if (FAILED(hr))
    {
        TRACE("DeviceFormat not available.\n");
        return hr;
    }

    if (Mode >= ADAPTER_OUTPUT.nmodes)
    {
        WARN("Mode %u does not exist.\n", Mode);
        return D3DERR_INVALIDCALL;
    }

    pMode->Width = ADAPTER_OUTPUT.modes[Mode].Width;
    pMode->Height = ADAPTER_OUTPUT.modes[Mode].Height;
    pMode->RefreshRate = ADAPTER_OUTPUT.modes[Mode].RefreshRate;
    pMode->Format = Format;

    return D3D_OK;
}

static HRESULT WINAPI d3dadapter9_GetAdapterDisplayMode(struct d3dadapter9 *This,
        UINT Adapter, D3DDISPLAYMODE *pMode)
{
    UINT Mode;

    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    Mode = ADAPTER_OUTPUT.current;
    pMode->Width = ADAPTER_OUTPUT.modes[Mode].Width;
    pMode->Height = ADAPTER_OUTPUT.modes[Mode].Height;
    pMode->RefreshRate = ADAPTER_OUTPUT.modes[Mode].RefreshRate;
    pMode->Format = ADAPTER_OUTPUT.modes[Mode].Format;

    return D3D_OK;
}

static HRESULT WINAPI d3dadapter9_CheckDeviceType(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat,
        D3DFORMAT BackBufferFormat, BOOL bWindowed)
{
    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    return ADAPTER_PROC(CheckDeviceType,
            DevType, AdapterFormat, BackBufferFormat, bWindowed);
}

static HRESULT WINAPI d3dadapter9_CheckDeviceFormat(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
        DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{
    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    return ADAPTER_PROC(CheckDeviceFormat,
             DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}

static HRESULT WINAPI d3dadapter9_CheckDeviceMultiSampleType(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
        BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels)
{
    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    return ADAPTER_PROC(CheckDeviceMultiSampleType, DeviceType, SurfaceFormat,
            Windowed, MultiSampleType, pQualityLevels);
}

static HRESULT WINAPI d3dadapter9_CheckDepthStencilMatch(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
        D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
{
    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    return ADAPTER_PROC(CheckDepthStencilMatch, DeviceType, AdapterFormat,
            RenderTargetFormat, DepthStencilFormat);
}

static HRESULT WINAPI d3dadapter9_CheckDeviceFormatConversion(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
{
    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    return ADAPTER_PROC(CheckDeviceFormatConversion,
            DeviceType, SourceFormat, TargetFormat);
}

static HRESULT WINAPI d3dadapter9_GetDeviceCaps(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps)
{
    HRESULT hr;

    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    hr = ADAPTER_PROC(GetDeviceCaps, DeviceType, pCaps);
    if (FAILED(hr))
        return hr;

    pCaps->MasterAdapterOrdinal = This->map[Adapter].master;
    pCaps->AdapterOrdinalInGroup = Adapter-This->map[Adapter].master;
    pCaps->NumberOfAdaptersInGroup = ADAPTER_GROUP.noutputs;

    return hr;
}

static HMONITOR WINAPI d3dadapter9_GetAdapterMonitor(struct d3dadapter9 *This,
        UINT Adapter)
{
    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return (HMONITOR)0;

    return (HMONITOR)ADAPTER_OUTPUT.monitor;
}

static HRESULT WINAPI DECLSPEC_HOTPATCH d3dadapter9_CreateDeviceEx(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS *pPresentationParameters,
        D3DDISPLAYMODEEX *pFullscreenDisplayMode,
        IDirect3DDevice9Ex **ppReturnedDeviceInterface);

static HRESULT WINAPI DECLSPEC_HOTPATCH d3dadapter9_CreateDevice(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS *pPresentationParameters,
        IDirect3DDevice9 **ppReturnedDeviceInterface)
{
    HRESULT hr;
    hr = d3dadapter9_CreateDeviceEx(This, Adapter, DeviceType, hFocusWindow,
            BehaviorFlags, pPresentationParameters, NULL,
            (IDirect3DDevice9Ex **)ppReturnedDeviceInterface);
    if (FAILED(hr))
        return hr;

    return D3D_OK;
}

static UINT WINAPI d3dadapter9_GetAdapterModeCountEx(struct d3dadapter9 *This,
        UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter)
{
    FIXME("(%p, %u, %p), half stub!\n", This, Adapter, pFilter);
    return d3dadapter9_GetAdapterModeCount(This, Adapter, pFilter->Format);
}

static HRESULT WINAPI d3dadapter9_EnumAdapterModesEx(struct d3dadapter9 *This,
        UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode,
        D3DDISPLAYMODEEX *pMode)
{
    HRESULT hr;

    FIXME("(%p, %u, %p, %u, %p), half stub!\n", This, Adapter, pFilter, Mode, pMode);

    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    hr = d3dadapter9_CheckDeviceFormat(This, Adapter, D3DDEVTYPE_HAL,
            pFilter->Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, pFilter->Format);

    if (FAILED(hr))
    {
        TRACE("DeviceFormat not available.\n");
        return hr;
    }

    if (Mode >= ADAPTER_OUTPUT.nmodes)
    {
        WARN("Mode %u does not exist.\n", Mode);
        return D3DERR_INVALIDCALL;
    }

    pMode->Size = ADAPTER_OUTPUT.modes[Mode].Size;
    pMode->Width = ADAPTER_OUTPUT.modes[Mode].Width;
    pMode->Height = ADAPTER_OUTPUT.modes[Mode].Height;
    pMode->RefreshRate = ADAPTER_OUTPUT.modes[Mode].RefreshRate;
    pMode->Format = ADAPTER_OUTPUT.modes[Mode].Format;
    pMode->ScanLineOrdering = ADAPTER_OUTPUT.modes[Mode].ScanLineOrdering;

    return D3D_OK;
}

static HRESULT WINAPI d3dadapter9_GetAdapterDisplayModeEx(struct d3dadapter9 *This,
        UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation)
{
    UINT Mode;

    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    if (pMode)
    {
        Mode = ADAPTER_OUTPUT.current;
        pMode->Size = sizeof(D3DDISPLAYMODEEX);
        pMode->Width = ADAPTER_OUTPUT.modes[Mode].Width;
        pMode->Height = ADAPTER_OUTPUT.modes[Mode].Height;
        pMode->RefreshRate = ADAPTER_OUTPUT.modes[Mode].RefreshRate;
        pMode->Format = ADAPTER_OUTPUT.modes[Mode].Format;
        pMode->ScanLineOrdering = ADAPTER_OUTPUT.modes[Mode].ScanLineOrdering;
    }
    if (pRotation)
        *pRotation = ADAPTER_OUTPUT.rotation;

    return D3D_OK;
}

static HRESULT WINAPI DECLSPEC_HOTPATCH d3dadapter9_CreateDeviceEx(struct d3dadapter9 *This,
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS *pPresentationParameters,
        D3DDISPLAYMODEEX *pFullscreenDisplayMode,
        IDirect3DDevice9Ex **ppReturnedDeviceInterface)
{
    ID3DPresentGroup *present;
    HRESULT hr;

    if (Adapter >= d3dadapter9_GetAdapterCount(This))
        return D3DERR_INVALIDCALL;

    {
        struct adapter_group *group = &ADAPTER_GROUP;
        unsigned nparams, ordinal;

        if (BehaviorFlags & D3DCREATE_ADAPTERGROUP_DEVICE)
        {
            nparams = group->noutputs;
            ordinal = 0;
        }
        else
        {
            nparams = 1;
            ordinal = Adapter - This->map[Adapter].master;
        }
        hr = present_create_present_group(This->gdi_display, group->devname, ordinal,
                hFocusWindow, pPresentationParameters, nparams, &present, This->ex,
                BehaviorFlags);
    }

    if (FAILED(hr))
    {
        WARN("Failed to create PresentGroup.\n");
        return hr;
    }

    if (This->ex)
    {
        hr = ADAPTER_PROC(CreateDeviceEx, Adapter, DeviceType, hFocusWindow,
                BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode,
                (IDirect3D9Ex *)This, present, ppReturnedDeviceInterface);
    }
    else
    {
        /* CreateDevice on non-ex */
        hr = ADAPTER_PROC(CreateDevice, Adapter, DeviceType, hFocusWindow,
                BehaviorFlags, pPresentationParameters, (IDirect3D9 *)This, present,
                (IDirect3DDevice9 **)ppReturnedDeviceInterface);
    }
    if (FAILED(hr))
    {
        WARN("ADAPTER_PROC failed.\n");
        ID3DPresentGroup_Release(present);
        return hr;
    }

    /* Nine returns different vtables for Ex, non Ex and
     * if you use the multithread flag or not. This prevents
     * things like Steam overlay to work, in addition to the problem
     * that functions nine side are not recognized by wine as
     * hotpatch-able. If possible, we use our vtable wrapper,
     * which solves the problem described above. */
    if (enable_device_vtable_wrapper())
        (*ppReturnedDeviceInterface)->lpVtbl = get_device_vtable();
    return hr;
}

static HRESULT WINAPI d3dadapter9_GetAdapterLUID(struct d3dadapter9 *This,
        UINT Adapter, LUID *pLUID)
{
    FIXME("(%p, %u, %p), stub!\n", This, Adapter, pLUID);
    return D3DERR_INVALIDCALL;
}

static struct adapter_group *add_group(struct d3dadapter9 *This)
{
    if (This->ngroups >= This->ngroupsalloc)
    {
        void *r;

        if (This->ngroupsalloc == 0)
        {
            This->ngroupsalloc = 2;
            r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    This->ngroupsalloc*sizeof(struct adapter_group));
        }
        else
        {
            This->ngroupsalloc <<= 1;
            r = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, This->groups,
                    This->ngroupsalloc*sizeof(struct adapter_group));
        }

        if (!r)
            return NULL;
        This->groups = r;
    }

    return &This->groups[This->ngroups++];
}

static void remove_group(struct d3dadapter9 *This)
{
    struct adapter_group *group = &This->groups[This->ngroups-1];
    int i;

    for (i = 0; i < group->noutputs; ++i)
    {
        HeapFree(GetProcessHeap(), 0, group->outputs[i].modes);
    }
    HeapFree(GetProcessHeap(), 0, group->outputs);

    ZeroMemory(group, sizeof(struct adapter_group));
    This->ngroups--;
}

static struct output *add_output(struct d3dadapter9 *This)
{
    struct adapter_group *group = &This->groups[This->ngroups-1];

    if (group->noutputs >= group->noutputsalloc)
    {
        void *r;

        if (group->noutputsalloc == 0)
        {
            group->noutputsalloc = 2;
            r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    group->noutputsalloc*sizeof(struct output));
        }
        else
        {
            group->noutputsalloc <<= 1;
            r = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, group->outputs,
                    group->noutputsalloc*sizeof(struct output));
        }

        if (!r)
            return NULL;
        group->outputs = r;
    }

    return &group->outputs[group->noutputs++];
}

static void remove_output(struct d3dadapter9 *This)
{
    struct adapter_group *group = &This->groups[This->ngroups-1];
    struct output *out = &group->outputs[group->noutputs-1];

    HeapFree(GetProcessHeap(), 0, out->modes);

    ZeroMemory(out, sizeof(struct output));
    group->noutputs--;
}

static D3DDISPLAYMODEEX *add_mode(struct d3dadapter9 *This)
{
    struct adapter_group *group = &This->groups[This->ngroups-1];
    struct output *out = &group->outputs[group->noutputs-1];

    if (out->nmodes >= out->nmodesalloc)
    {
        void *r;

        if (out->nmodesalloc == 0)
        {
            out->nmodesalloc = 8;
            r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    out->nmodesalloc*sizeof(struct D3DDISPLAYMODEEX));
        }
        else
        {
            out->nmodesalloc <<= 1;
            r = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, out->modes,
                    out->nmodesalloc*sizeof(struct D3DDISPLAYMODEEX));
        }

        if (!r)
            return NULL;
        out->modes = r;
    }

    return &out->modes[out->nmodes++];
}

static void remove_mode(struct d3dadapter9 *This)
{
    struct adapter_group *group = &This->groups[This->ngroups-1];
    struct output *out = &group->outputs[group->noutputs-1];
    out->nmodes--;
}

static HRESULT fill_groups(struct d3dadapter9 *This)
{
    DISPLAY_DEVICEW dd;
    DEVMODEW dm;
    POINT pt;
    HDC hdc;
    HRESULT hr;
    int i, j, k;

    WCHAR wdisp[] = {'D','I','S','P','L','A','Y',0};

    ZeroMemory(&dd, sizeof(dd));
    ZeroMemory(&dm, sizeof(dm));
    dd.cb = sizeof(dd);
    dm.dmSize = sizeof(dm);

    for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); ++i)
    {
        struct adapter_group *group = add_group(This);
        if (!group)
        {
            ERR("Out of memory.\n");
            return E_OUTOFMEMORY;
        }

        hdc = CreateDCW(wdisp, dd.DeviceName, NULL, NULL);
        if (!hdc)
        {
            remove_group(This);
            WARN("Unable to create DC for display %d.\n", i);
            goto end_group;
        }

        hr = present_create_adapter9(This->gdi_display, hdc, &group->adapter);
        DeleteDC(hdc);
        if (FAILED(hr))
        {
            remove_group(This);
            goto end_group;
        }

        CopyMemory(group->devname, dd.DeviceName, sizeof(group->devname));
        for (j = 0; EnumDisplayDevicesW(group->devname, j, &dd, 0); ++j)
        {
            struct output *out = add_output(This);
            boolean orient = FALSE, monit = FALSE;
            if (!out)
            {
                ERR("Out of memory.\n");
                return E_OUTOFMEMORY;
            }

            for (k = 0; EnumDisplaySettingsExW(dd.DeviceName, k, &dm, 0); ++k)
            {
                D3DDISPLAYMODEEX *mode = add_mode(This);
                if (!out)
                {
                    ERR("Out of memory.\n");
                    return E_OUTOFMEMORY;
                }

                mode->Size = sizeof(D3DDISPLAYMODEEX);
                mode->Width = dm.dmPelsWidth;
                mode->Height = dm.dmPelsHeight;
                mode->RefreshRate = dm.dmDisplayFrequency;
                mode->ScanLineOrdering =
                        (dm.dmDisplayFlags & DM_INTERLACED) ?
                        D3DSCANLINEORDERING_INTERLACED :
                        D3DSCANLINEORDERING_PROGRESSIVE;

                switch (dm.dmBitsPerPel)
                {
                    case 32: mode->Format = D3DFMT_X8R8G8B8; break;
                    case 24: mode->Format = D3DFMT_R8G8B8; break;
                    case 16: mode->Format = D3DFMT_R5G6B5; break;
                    case 8:
                        remove_mode(This);
                        goto end_mode;

                    default:
                        remove_mode(This);
                        WARN("Unknown format (%u bpp) in display %d, monitor "
                                "%d, mode %d.\n", dm.dmBitsPerPel, i, j, k);
                        goto end_mode;
                }

                if (!orient)
                {
                    switch (dm.dmDisplayOrientation)
                    {
                        case DMDO_DEFAULT:
                            out->rotation = D3DDISPLAYROTATION_IDENTITY;
                            break;

                        case DMDO_90:
                            out->rotation = D3DDISPLAYROTATION_90;
                            break;

                        case DMDO_180:
                            out->rotation = D3DDISPLAYROTATION_180;
                            break;

                        case DMDO_270:
                            out->rotation = D3DDISPLAYROTATION_270;
                            break;

                        default:
                            remove_output(This);
                            WARN("Unknown display rotation in display %d, "
                                    "monitor %d\n", i, j);
                            goto end_output;
                    }
                    orient = TRUE;
                }

                if (!monit)
                {
                    pt.x = dm.dmPosition.x;
                    pt.y = dm.dmPosition.y;
                    out->monitor = MonitorFromPoint(pt, 0);
                    if (!out->monitor)
                    {
                        remove_output(This);
                        WARN("Unable to get monitor handle for display %d, "
                                "monitor %d.\n", i, j);
                        goto end_output;
                    }
                    monit = TRUE;
                }

end_mode:
                ZeroMemory(&dm, sizeof(dm));
                dm.dmSize = sizeof(dm);
            }

end_output:
            ZeroMemory(&dd, sizeof(dd));
            dd.cb = sizeof(dd);
        }

end_group:
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
    }

    return D3D_OK;
}

static IDirect3D9ExVtbl d3dadapter9_vtable = {
    (void *)d3dadapter9_QueryInterface,
    (void *)d3dadapter9_AddRef,
    (void *)d3dadapter9_Release,
    (void *)d3dadapter9_RegisterSoftwareDevice,
    (void *)d3dadapter9_GetAdapterCount,
    (void *)d3dadapter9_GetAdapterIdentifier,
    (void *)d3dadapter9_GetAdapterModeCount,
    (void *)d3dadapter9_EnumAdapterModes,
    (void *)d3dadapter9_GetAdapterDisplayMode,
    (void *)d3dadapter9_CheckDeviceType,
    (void *)d3dadapter9_CheckDeviceFormat,
    (void *)d3dadapter9_CheckDeviceMultiSampleType,
    (void *)d3dadapter9_CheckDepthStencilMatch,
    (void *)d3dadapter9_CheckDeviceFormatConversion,
    (void *)d3dadapter9_GetDeviceCaps,
    (void *)d3dadapter9_GetAdapterMonitor,
    (void *)d3dadapter9_CreateDevice,
    (void *)d3dadapter9_GetAdapterModeCountEx,
    (void *)d3dadapter9_EnumAdapterModesEx,
    (void *)d3dadapter9_GetAdapterDisplayModeEx,
    (void *)d3dadapter9_CreateDeviceEx,
    (void *)d3dadapter9_GetAdapterLUID
};

HRESULT d3dadapter9_new(Display *gdi_display, boolean ex, IDirect3D9Ex **ppOut)
{
    struct d3dadapter9 *This;
    HRESULT hr;
    unsigned i, j, k;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct d3dadapter9));
    if (!This)
    {
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->vtable = &d3dadapter9_vtable;
    This->refs = 1;
    This->ex = ex;
    This->gdi_display = gdi_display;

    if (!present_has_d3dadapter(gdi_display))
    {
        ERR("Your display driver doesn't support native D3D9 adapters.\n");
        d3dadapter9_Release(This);
        return D3DERR_NOTAVAILABLE;
    }

    if (FAILED(hr = fill_groups(This)))
    {
        d3dadapter9_Release(This);
        return hr;
    }

    /* map absolute adapter IDs with internal adapters */
    for (i = 0; i < This->ngroups; ++i)
    {
        for (j = 0; j < This->groups[i].noutputs; ++j)
        {
            This->nadapters++;
        }
    }
    if (This->nadapters == 0)
    {
        ERR("No available native adapters in system.\n");
        d3dadapter9_Release(This);
        return D3DERR_NOTAVAILABLE;
    }

    This->map = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            This->nadapters * sizeof(struct adapter_map));

    if (!This->map)
    {
        d3dadapter9_Release(This);
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }
    for (i = k = 0; i < This->ngroups; ++i)
    {
        for (j = 0; j < This->groups[i].noutputs; ++j, ++k)
        {
            This->map[k].master = k-j;
            This->map[k].group = i;
        }
    }

    *ppOut = (IDirect3D9Ex *)This;
    FIXME("\033[1;32m\nNative Direct3D 9 is active."
            "\nFor more information visit https://wiki.ixit.cz/d3d9\033[0m\n");
    return D3D_OK;
}

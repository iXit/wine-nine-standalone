/*
 * Wine ID3DAdapter9 support functions
 *
 * Copyright 2013 Joakim Sindholt
 *                Christoph Bumiller
 * Copyright 2014 Tiziano Bacocco
 *                David Heidelberger
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015 Patrick Rudolph
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
#include "wine/port.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

#include <d3dadapter/drm.h>
#include <X11/Xutil.h>

#include "dri3.h"
#include "wndproc.h"

#include "wine/library.h" // for wine_dl*
#include "wine/unicode.h" // for strcpyW

#ifndef D3DPRESENT_DONOTWAIT
#define D3DPRESENT_DONOTWAIT      0x00000001
#endif

#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MAJOR 1
#if defined (ID3DPresent_SetPresentParameters2)
#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR 3
#elif defined (ID3DPresent_ResolutionMismatch)
#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR 2
#elif defined (ID3DPresent_GetWindowOccluded)
#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR 1
#else
#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR 0
#endif

static const struct D3DAdapter9DRM *d3d9_drm = NULL;
#ifdef D3D9NINE_DRI2
static int is_dri2_fallback = 0;
#endif

/* Start section of x11drv.h */
#define X11DRV_ESCAPE 6789
enum x11drv_escape_codes
{
    X11DRV_SET_DRAWABLE,     /* set current drawable for a DC */
    X11DRV_GET_DRAWABLE,     /* get current drawable for a DC */
    X11DRV_START_EXPOSURES,  /* start graphics exposures */
    X11DRV_END_EXPOSURES,    /* end graphics exposures */
    X11DRV_FLUSH_GL_DRAWABLE /* flush changes made to the gl drawable */
};

struct x11drv_escape_get_drawable
{
    enum x11drv_escape_codes code;         /* escape code (X11DRV_GET_DRAWABLE) */
    Drawable                 drawable;     /* X drawable */
    Drawable                 gl_drawable;  /* GL drawable */
    int                      pixel_format; /* internal GL pixel format */
};
/* End section x11drv.h */

static XContext d3d_hwnd_context;
static CRITICAL_SECTION context_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &context_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": context_section") }
};
static CRITICAL_SECTION context_section = { &critsect_debug, -1, 0, 0, 0, 0 };

const GUID IID_ID3DPresent = { 0x77D60E80, 0xF1E6, 0x11DF, { 0x9E, 0x39, 0x95, 0x0C, 0xDF, 0xD7, 0x20, 0x85 } };
const GUID IID_ID3DPresentGroup = { 0xB9C3016E, 0xF32A, 0x11DF, { 0x9C, 0x18, 0x92, 0xEA, 0xDE, 0xD7, 0x20, 0x85 } };

struct d3d_drawable
{
    Drawable drawable; /* X11 drawable */
    HDC hdc;
    HWND wnd; /* HWND (for convenience) */
};

struct DRI3Present
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    D3DPRESENT_PARAMETERS params;
    HWND focus_wnd;
    PRESENTpriv *present_priv;
#ifdef D3D9NINE_DRI2
    struct DRI2priv *dri2_priv;
#endif

    WCHAR devname[32];
    HCURSOR hCursor;

    DEVMODEW initial_mode;

    DWORD style;
    DWORD style_ex;

    BOOL reapply_mode;
    BOOL ex;
    BOOL resolution_mismatch;
    BOOL occluded;
    BOOL drop_wnd_messages;
    BOOL no_window_changes;
    Display *gdi_display;

    UINT present_interval;
    BOOL present_async;
    BOOL present_swapeffectcopy;
    BOOL allow_discard_delayed_release;
    BOOL tear_free_discard;
    struct d3d_drawable *d3d;
};

struct D3DWindowBuffer
{
    PRESENTPixmapPriv *present_pixmap_priv;
};

static void free_d3dadapter_drawable(struct d3d_drawable *d3d)
{
    ReleaseDC(d3d->wnd, d3d->hdc);
    HeapFree(GetProcessHeap(), 0, d3d);
}

static void destroy_d3dadapter_drawable(Display *gdi_display, HWND hwnd)
{
    struct d3d_drawable *d3d;

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd,
            d3d_hwnd_context, (char **)&d3d))
    {
        XDeleteContext(gdi_display, (XID)hwnd, d3d_hwnd_context);
        free_d3dadapter_drawable(d3d);
    }
    LeaveCriticalSection(&context_section);
}

static RECT DRI3Present_GetClientRecWindowRelative(HWND hwnd)
{
    RECT rect;
    RECT wnd;

    /* Get client space dimensions */
    GetClientRect(hwnd, &rect);

    /* Get window in screen space */
    GetWindowRect(hwnd, &wnd);

    /* Transform to offset */
    MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT) &wnd, 2);
    wnd.top *= -1;
    wnd.left *= -1;
    wnd.bottom = wnd.top + rect.bottom;
    wnd.right = wnd.left + rect.right;

    return wnd;
}

static struct d3d_drawable *create_d3dadapter_drawable(HWND hwnd)
{
    struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };
    struct d3d_drawable *d3d;

    d3d = HeapAlloc(GetProcessHeap(), 0, sizeof(*d3d));
    if (!d3d)
    {
        ERR("Couldn't allocate d3d_drawable.\n");
        return NULL;
    }

    d3d->hdc = GetDCEx(hwnd, 0, DCX_CACHE | DCX_CLIPSIBLINGS);
    if (ExtEscape(d3d->hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
            sizeof(extesc), (LPSTR)&extesc) <= 0)
    {
        ERR("Unexpected error in X Drawable lookup (hwnd=%p, hdc=%p)\n", hwnd, d3d->hdc);
        ReleaseDC(hwnd, d3d->hdc);
        HeapFree(GetProcessHeap(), 0, d3d);
        return NULL;
    }

    d3d->drawable = extesc.drawable;
    d3d->wnd = hwnd;

    return d3d;
}

static struct d3d_drawable *get_d3d_drawable(Display *gdi_display, HWND hwnd)
{
    struct d3d_drawable *d3d, *race;

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd, d3d_hwnd_context, (char **)&d3d))
    {
        return d3d;
    }
    LeaveCriticalSection(&context_section);

    TRACE("No d3d_drawable attached to hwnd %p, creating one.\n", hwnd);

    d3d = create_d3dadapter_drawable(hwnd);
    if (!d3d)
        return NULL;

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd,
            d3d_hwnd_context, (char **)&race))
    {
        /* apparently someone beat us to creating this d3d drawable. Let's not
           waste more time with X11 calls and just use theirs instead. */
        free_d3dadapter_drawable(d3d);
        return race;
    }
    XSaveContext(gdi_display, (XID)hwnd, d3d_hwnd_context, (char *)d3d);
    return d3d;
}

static void release_d3d_drawable(struct d3d_drawable *d3d)
{
    if (!d3d)
        ERR("Driver internal error: d3d_drawable is NULL\n");
    LeaveCriticalSection(&context_section);
}

static ULONG WINAPI DRI3Present_AddRef(struct DRI3Present *This)
{
    ULONG refs = InterlockedIncrement(&This->refs);
    TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI DRI3Present_Release(struct DRI3Present *This)
{
    ULONG refs = InterlockedDecrement(&This->refs);
    TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0)
    {
        /* dtor */
        (void) nine_unregister_window(This->focus_wnd);
        if (This->d3d)
            destroy_d3dadapter_drawable(This->gdi_display, This->d3d->wnd);
        ChangeDisplaySettingsExW(This->devname, &(This->initial_mode), 0, CDS_FULLSCREEN, NULL);
        PRESENTDestroy(This->gdi_display, This->present_priv);
#ifdef D3D9NINE_DRI2
        if (is_dri2_fallback)
            DRI2FallbackDestroy(This->dri2_priv);
#endif
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI DRI3Present_QueryInterface(struct DRI3Present *This,
        REFIID riid, void **ppvObject)
{
    if (!ppvObject)
        return E_POINTER;

    if (IsEqualGUID(&IID_ID3DPresent, riid) ||
            IsEqualGUID(&IID_IUnknown, riid))
    {
        *ppvObject = This;
        DRI3Present_AddRef(This);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static HRESULT DRI3Present_ChangePresentParameters(struct DRI3Present *This,
        D3DPRESENT_PARAMETERS *params);

static HRESULT WINAPI DRI3Present_SetPresentParameters(struct DRI3Present *This,
        D3DPRESENT_PARAMETERS *pPresentationParameters,
        D3DDISPLAYMODEEX *pFullscreenDisplayMode)
{
    if (pFullscreenDisplayMode)
        FIXME("Ignoring pFullscreenDisplayMode\n");
    return DRI3Present_ChangePresentParameters(This, pPresentationParameters);
}

static HRESULT WINAPI DRI3Present_D3DWindowBufferFromDmaBuf(struct DRI3Present *This,
        int dmaBufFd, int width, int height, int stride, int depth,
        int bpp, struct D3DWindowBuffer **out)
{
    Pixmap pixmap;

#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback)
    {
        *out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                sizeof(struct D3DWindowBuffer));
        if (!DRI2FallbackPRESENTPixmap(This->present_priv, This->dri2_priv,
                dmaBufFd, width, height, stride, depth,
                bpp, &((*out)->present_pixmap_priv)))
        {
            ERR("DRI2FallbackPRESENTPixmap failed\n");
            HeapFree(GetProcessHeap(), 0, *out);
            return D3DERR_DRIVERINTERNALERROR;
        }
        return D3D_OK;
    }
#endif
    if (!DRI3PixmapFromDmaBuf(This->gdi_display, DefaultScreen(This->gdi_display),
            dmaBufFd, width, height, stride, depth, bpp, &pixmap))
    {
        ERR("DRI3PixmapFromDmaBuf failed\n");
        return D3DERR_DRIVERINTERNALERROR;
    }

    *out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct D3DWindowBuffer));
    if (!PRESENTPixmapInit(This->present_priv, pixmap, &((*out)->present_pixmap_priv)))
    {
        ERR("PRESENTPixmapInit failed\n");
        HeapFree(GetProcessHeap(), 0, *out);
        return D3DERR_DRIVERINTERNALERROR;
    }
    return D3D_OK;
}

static HRESULT WINAPI DRI3Present_DestroyD3DWindowBuffer(struct DRI3Present *This,
        struct D3DWindowBuffer *buffer)
{
    /* the pixmap is managed by the PRESENT backend.
     * But if it can delete it right away, we may have
     * better performance */
    PRESENTTryFreePixmap(This->gdi_display, buffer->present_pixmap_priv);
    HeapFree(GetProcessHeap(), 0, buffer);
    return D3D_OK;
}

static HRESULT WINAPI DRI3Present_WaitBufferReleased(struct DRI3Present *This,
        struct D3DWindowBuffer *buffer)
{
    if(!PRESENTWaitPixmapReleased(buffer->present_pixmap_priv))
    {
        ERR("PRESENTWaitPixmapReleased failed\n");
        return D3DERR_DRIVERINTERNALERROR;
    }
    return D3D_OK;
}

static HRESULT WINAPI DRI3Present_FrontBufferCopy(struct DRI3Present *This,
        struct D3DWindowBuffer *buffer)
{
#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback)
        return D3DERR_DRIVERINTERNALERROR;
#endif
    if (PRESENTHelperCopyFront(This->gdi_display, buffer->present_pixmap_priv))
        return D3D_OK;
    else
        return D3DERR_DRIVERINTERNALERROR;
}

/* Try to detect client side window decorations by walking the X Drawable up.
 * In case there's an intermediate Drawable, server side window decorations are used.
 * TODO: Find a X11 function to query for window decorations.
 */
static BOOL DRI3Present_HasClientSideWindowDecorations(struct DRI3Present *This,
        HWND hwnd)
{
    struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };
    Window Wroot;
    Window Wparent;
    Window *Wchildren;
    unsigned int numchildren;
    HWND parent;
    HDC hdc;
    BOOL ret = TRUE;

    parent = GetParent(hwnd);
    if (!parent)
        parent = GetDesktopWindow();
    if (!parent)
    {
        ERR("Unexpected error getting the parent hwnd (hwnd=%p)\n", hwnd);
        return FALSE;
    }

    hdc = GetDCEx(hwnd, 0, DCX_CACHE | DCX_CLIPSIBLINGS);
    if (!hdc)
        return FALSE;
    if (ExtEscape(hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
            sizeof(extesc), (LPSTR)&extesc) <= 0)
    {
        ERR("Unexpected error in X Drawable lookup (hwnd=%p, hdc=%p)\n", hwnd, hdc);
        ReleaseDC(hwnd, hdc);
        return FALSE;
    }
    ReleaseDC(hwnd, hdc);

    if (XQueryTree(This->gdi_display, extesc.drawable, &Wroot, &Wparent, &Wchildren, &numchildren))
    {
        hdc = GetDCEx(parent, 0, DCX_CACHE | DCX_CLIPSIBLINGS);
        if (!hdc)
            return FALSE;

        if (ExtEscape(hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
                sizeof(extesc), (LPSTR)&extesc) <= 0)
        {
            ERR("Unexpected error in X Drawable lookup (hwnd=%p, hdc=%p)\n", parent, hdc);
            ReleaseDC(parent, hdc);
            return FALSE;
        }
        ReleaseDC(parent, hdc);

        if (Wparent != extesc.drawable)
        {
            /* Found at least one intermediate window */
            ret = FALSE;
        }
        if (Wchildren)
            free(Wchildren);
    }

    return ret;
}

static HRESULT WINAPI DRI3Present_PresentBuffer( struct DRI3Present *This,
        struct D3DWindowBuffer *buffer, HWND hWndOverride, const RECT *pSourceRect,
        const RECT *pDestRect, const RGNDATA *pDirtyRegion, DWORD Flags )
{
    struct d3d_drawable *d3d;
    RECT dest_translate;
    RECT offset;
    HWND hwnd;

    if (hWndOverride)
        hwnd = hWndOverride;
    else if (This->params.hDeviceWindow)
        hwnd = This->params.hDeviceWindow;
    else
        hwnd = This->focus_wnd;

    d3d = get_d3d_drawable(This->gdi_display, hwnd);

    if (!d3d)
        return D3DERR_DRIVERINTERNALERROR;

    /* TODO: should we use a list here instead ? */
    if (This->d3d && (This->d3d->wnd != d3d->wnd))
        destroy_d3dadapter_drawable(This->gdi_display, This->d3d->wnd);

    This->d3d = d3d;

    /* In case of client side window decorations we need to add an offset within
     * the X drawable.
     * FIXME: Call once on window style / size change */
    if (DRI3Present_HasClientSideWindowDecorations(This, hwnd))
    {
        offset = DRI3Present_GetClientRecWindowRelative(hwnd);

        if ((offset.top != 0) || (offset.left != 0))
        {
            if (!pDestRect)
                pDestRect = (const RECT *) &offset;
            else
            {
                dest_translate.top = pDestRect->top + offset.top;
                dest_translate.left = pDestRect->left + offset.left;
                dest_translate.bottom = pDestRect->bottom + offset.bottom;
                dest_translate.right = pDestRect->right + offset.right;
                pDestRect = (const RECT *) &dest_translate;
            }
        }
    }

    if (!PRESENTPixmap(This->gdi_display, d3d->drawable, buffer->present_pixmap_priv,
            This->present_interval, This->present_async, This->present_swapeffectcopy,
            pSourceRect, pDestRect, pDirtyRegion))
    {
        release_d3d_drawable(d3d);
        return D3DERR_DRIVERINTERNALERROR;
    }
    release_d3d_drawable(d3d);

    return D3D_OK;
}

/* Based on wine's wined3d_get_adapter_raster_status. */
static HRESULT WINAPI DRI3Present_GetRasterStatus( struct DRI3Present *This,
        D3DRASTER_STATUS *pRasterStatus )
{
    LONGLONG freq_per_frame, freq_per_line;
    LARGE_INTEGER counter, freq_per_sec;
    unsigned refresh_rate, height;

    TRACE("This=%p, pRasterStatus=%p\n", This, pRasterStatus);

    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&freq_per_sec))
        return D3DERR_INVALIDCALL;

    if (This->params.Windowed)
    {
        refresh_rate = This->initial_mode.dmDisplayFrequency;
        height = This->initial_mode.dmPelsHeight;
    }
    else
    {
        refresh_rate = This->params.FullScreen_RefreshRateInHz;
        height = This->params.BackBufferHeight;
    }

    if (refresh_rate == 0)
        refresh_rate = 60;

    TRACE("refresh_rate=%u, height=%u\n", refresh_rate, height);

    freq_per_frame = freq_per_sec.QuadPart / refresh_rate;
    /* Assume 20 scan lines in the vertical blank. */
    freq_per_line = freq_per_frame / (height + 20);
    pRasterStatus->ScanLine = (counter.QuadPart % freq_per_frame) / freq_per_line;
    if (pRasterStatus->ScanLine < height)
        pRasterStatus->InVBlank = FALSE;
    else
    {
        pRasterStatus->ScanLine = 0;
        pRasterStatus->InVBlank = TRUE;
    }

    TRACE("Returning fake value, InVBlank %u, ScanLine %u.\n",
            pRasterStatus->InVBlank, pRasterStatus->ScanLine);

    return D3D_OK;
}

static HRESULT WINAPI DRI3Present_GetDisplayMode( struct DRI3Present *This,
        D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation )
{
    DEVMODEW dm;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    EnumDisplaySettingsExW(This->devname, ENUM_CURRENT_SETTINGS, &dm, 0);
    pMode->Width = dm.dmPelsWidth;
    pMode->Height = dm.dmPelsHeight;
    pMode->RefreshRate = dm.dmDisplayFrequency;
    pMode->ScanLineOrdering = (dm.dmDisplayFlags & DM_INTERLACED) ?
            D3DSCANLINEORDERING_INTERLACED : D3DSCANLINEORDERING_PROGRESSIVE;

    /* XXX This is called "guessing" */
    switch (dm.dmBitsPerPel)
    {
        case 32: pMode->Format = D3DFMT_X8R8G8B8; break;
        case 24: pMode->Format = D3DFMT_R8G8B8; break;
        case 16: pMode->Format = D3DFMT_R5G6B5; break;
        default:
            WARN("Unknown display format with %u bpp.\n", dm.dmBitsPerPel);
            pMode->Format = D3DFMT_UNKNOWN;
    }

    switch (dm.dmDisplayOrientation)
    {
        case DMDO_DEFAULT: *pRotation = D3DDISPLAYROTATION_IDENTITY; break;
        case DMDO_90:      *pRotation = D3DDISPLAYROTATION_90; break;
        case DMDO_180:     *pRotation = D3DDISPLAYROTATION_180; break;
        case DMDO_270:     *pRotation = D3DDISPLAYROTATION_270; break;
        default:
            WARN("Unknown display rotation %u.\n", dm.dmDisplayOrientation);
            *pRotation = D3DDISPLAYROTATION_IDENTITY;
    }

    return D3D_OK;
}

static HRESULT WINAPI DRI3Present_GetPresentStats( struct DRI3Present *This, D3DPRESENTSTATS *pStats )
{
    FIXME("(%p, %p), stub!\n", This, pStats);
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI DRI3Present_GetCursorPos( struct DRI3Present *This, POINT *pPoint )
{
    BOOL ok;
    HWND draw_window;

    if (!pPoint)
        return D3DERR_INVALIDCALL;

    draw_window = This->params.hDeviceWindow ?
            This->params.hDeviceWindow : This->focus_wnd;

    ok = GetCursorPos(pPoint);
    ok = ok && ScreenToClient(draw_window, pPoint);
    return ok ? S_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI DRI3Present_SetCursorPos( struct DRI3Present *This, POINT *pPoint )
{
    BOOL ok;
    POINT real_pos;

    if (!pPoint)
        return D3DERR_INVALIDCALL;

    ok = SetCursorPos(pPoint->x, pPoint->y);
    if (!ok)
        goto error;

    ok = GetCursorPos(&real_pos);
    if (!ok || real_pos.x != pPoint->x || real_pos.y != pPoint->y)
        goto error;

    return D3D_OK;

error:
    SetCursor(NULL); /* Hide cursor rather than put wrong pos */
    return D3DERR_DRIVERINTERNALERROR;
}

/* Note: assuming 32x32 cursor */
static HRESULT WINAPI DRI3Present_SetCursor( struct DRI3Present *This, void *pBitmap,
        POINT *pHotspot, BOOL bShow )
{
   if (pBitmap)
   {
      ICONINFO info;
      HCURSOR cursor;

      DWORD mask[32];
      memset(mask, ~0, sizeof(mask));

      if (!pHotspot)
         return D3DERR_INVALIDCALL;
      info.fIcon = FALSE;
      info.xHotspot = pHotspot->x;
      info.yHotspot = pHotspot->y;
      info.hbmMask = CreateBitmap(32, 32, 1, 1, mask);
      info.hbmColor = CreateBitmap(32, 32, 1, 32, pBitmap);

      cursor = CreateIconIndirect(&info);
      if (info.hbmMask) DeleteObject(info.hbmMask);
      if (info.hbmColor) DeleteObject(info.hbmColor);
      if (cursor)
         DestroyCursor(This->hCursor);
      This->hCursor = cursor;
   }
   SetCursor(bShow ? This->hCursor : NULL);

   return D3D_OK;
}

static HRESULT WINAPI DRI3Present_SetGammaRamp( struct DRI3Present *This,
        const D3DGAMMARAMP *pRamp, HWND hWndOverride )
{
    HWND hWnd = hWndOverride ? hWndOverride : This->focus_wnd;
    HDC hdc;
    BOOL ok;
    if (!pRamp)
        return D3DERR_INVALIDCALL;

    hdc = GetDC(hWnd);
    ok = SetDeviceGammaRamp(hdc, (void *)pRamp);
    ReleaseDC(hWnd, hdc);
    return ok ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI DRI3Present_GetWindowInfo( struct DRI3Present *This,
        HWND hWnd, int *width, int *height, int *depth )
{
    HRESULT hr;
    RECT pRect;

    if (!hWnd)
        hWnd = This->focus_wnd;
    hr = GetClientRect(hWnd, &pRect);
    if (!hr)
        return D3DERR_INVALIDCALL;
    *width = pRect.right - pRect.left;
    *height = pRect.bottom - pRect.top;
    *depth = 24; //TODO
    return D3D_OK;
}

#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 1
static BOOL WINAPI DRI3Present_GetWindowOccluded(struct DRI3Present *This)
{
    return This->occluded;
}
#endif

#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 2
static BOOL WINAPI DRI3Present_ResolutionMismatch(struct DRI3Present *This)
{
    /* The resolution might change due to a third party app.
     * Poll this function to get the device's resolution match.
     * A device reset is required to restore the requested resolution.
     */
    return This->resolution_mismatch;
}

static HANDLE WINAPI DRI3Present_CreateThread( struct DRI3Present *This,
        void *pThreadfunc, void *pParam )
{
    LPTHREAD_START_ROUTINE lpStartAddress =
            (LPTHREAD_START_ROUTINE) pThreadfunc;

    return CreateThread(NULL, 0, lpStartAddress, pParam, 0, NULL);
}

static BOOL WINAPI DRI3Present_WaitForThread( struct DRI3Present *This, HANDLE thread )
{
    DWORD ExitCode = 0;
    while (GetExitCodeThread(thread, &ExitCode) && ExitCode == STILL_ACTIVE)
        Sleep(10);

    return TRUE;
}
#endif

#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 3
static HRESULT WINAPI DRI3Present_SetPresentParameters2( struct DRI3Present *This, D3DPRESENT_PARAMETERS2 *pParams )
{
    This->allow_discard_delayed_release = pParams->AllowDISCARDDelayedRelease;
    This->tear_free_discard = pParams->AllowDISCARDDelayedRelease && pParams->TearFreeDISCARD;
    return D3D_OK;
}

static BOOL WINAPI DRI3Present_IsBufferReleased( struct DRI3Present *This, struct D3DWindowBuffer *buffer )
{
    return PRESENTIsPixmapReleased(buffer->present_pixmap_priv);
}

static HRESULT WINAPI DRI3Present_WaitBufferReleaseEvent( struct DRI3Present *This )
{
    PRESENTWaitReleaseEvent(This->present_priv);
    return D3D_OK;
}
#endif

/*----------*/

static ID3DPresentVtbl DRI3Present_vtable = {
    (void *)DRI3Present_QueryInterface,
    (void *)DRI3Present_AddRef,
    (void *)DRI3Present_Release,
    (void *)DRI3Present_SetPresentParameters,
    (void *)DRI3Present_D3DWindowBufferFromDmaBuf,
    (void *)DRI3Present_DestroyD3DWindowBuffer,
    (void *)DRI3Present_WaitBufferReleased,
    (void *)DRI3Present_FrontBufferCopy,
    (void *)DRI3Present_PresentBuffer,
    (void *)DRI3Present_GetRasterStatus,
    (void *)DRI3Present_GetDisplayMode,
    (void *)DRI3Present_GetPresentStats,
    (void *)DRI3Present_GetCursorPos,
    (void *)DRI3Present_SetCursorPos,
    (void *)DRI3Present_SetCursor,
    (void *)DRI3Present_SetGammaRamp,
    (void *)DRI3Present_GetWindowInfo,
#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 1
    (void *)DRI3Present_GetWindowOccluded,
#endif
#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 2
    (void *)DRI3Present_ResolutionMismatch,
    (void *)DRI3Present_CreateThread,
    (void *)DRI3Present_WaitForThread,
#endif
#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 3
    (void *)DRI3Present_SetPresentParameters2,
    (void *)DRI3Present_IsBufferReleased,
    (void *)DRI3Present_WaitBufferReleaseEvent,
#endif
};

/* The following code is based on WINE's wined3d/device.c and
 * wined3d/swapchain.c and WINE's d3d9 files. */

static LONG fullscreen_style(LONG style)
{
    /* Make sure the window is managed, otherwise we won't get keyboard input. */
    style |= WS_POPUP | WS_SYSMENU;
    style &= ~(WS_CAPTION | WS_THICKFRAME);

    return style;
}

static LONG fullscreen_exstyle(LONG exstyle)
{
    /* Filter out window decorations. */
    exstyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);

    return exstyle;
}

static HRESULT DRI3Present_ChangeDisplaySettingsIfNeccessary(struct DRI3Present *This, DEVMODEW *new_mode)
{
    DEVMODEW current_mode;
    LONG hr;

    /* Filter invalid resolution */
    if (!new_mode->dmPelsWidth || !new_mode->dmPelsHeight)
        return D3DERR_INVALIDCALL;

    /* Ignore invalid frequency requested */
    if (new_mode->dmDisplayFrequency > 1000)
        new_mode->dmDisplayFrequency = 0;

    ZeroMemory(&current_mode, sizeof(DEVMODEW));
    current_mode.dmSize = sizeof(DEVMODEW);
    /* Only change the mode if necessary. */
    if (!EnumDisplaySettingsW(This->devname, ENUM_CURRENT_SETTINGS, &current_mode))
       ERR("Failed to get current display mode.\n");
    else if (current_mode.dmPelsWidth != new_mode->dmPelsWidth
           || current_mode.dmPelsHeight != new_mode->dmPelsHeight
           || (current_mode.dmDisplayFrequency != new_mode->dmDisplayFrequency
           && (new_mode->dmFields & DM_DISPLAYFREQUENCY)))
    {
        hr = ChangeDisplaySettingsExW(This->devname, new_mode, 0, CDS_FULLSCREEN, NULL);
        if (hr != DISP_CHANGE_SUCCESSFUL)
        {
            /* try again without display RefreshRate */
            if (new_mode->dmFields & DM_DISPLAYFREQUENCY)
            {
                new_mode->dmFields &= ~DM_DISPLAYFREQUENCY;
                new_mode->dmDisplayFrequency = 0;
                hr = ChangeDisplaySettingsExW(This->devname, new_mode, 0, CDS_FULLSCREEN, NULL);
                if (hr != DISP_CHANGE_SUCCESSFUL)
                {
                    ERR("ChangeDisplaySettingsExW failed with 0x%08X\n", hr);
                    return D3DERR_INVALIDCALL;
                }
            }
            else
            {
                ERR("ChangeDisplaySettingsExW failed with 0x%08X\n", hr);
                return D3DERR_INVALIDCALL;
            }
        }
    }
    return D3D_OK;
}

LRESULT device_process_message(struct DRI3Present *present, HWND window, BOOL unicode,
        UINT message, WPARAM wparam, LPARAM lparam, WNDPROC proc)
{
    boolean drop_wnd_messages;
    DEVMODEW current_mode;
    DEVMODEW new_mode;

    TRACE("Got message: window %p, message %#x, wparam %#lx, lparam %#lx.\n",
                    window, message, wparam, lparam);

    if (present->drop_wnd_messages && message != WM_DISPLAYCHANGE)
    {
        TRACE("Filtering message: window %p, message %#x, wparam %#lx, lparam %#lx.\n",
                window, message, wparam, lparam);
        if (unicode)
            return DefWindowProcW(window, message, wparam, lparam);
        else
            return DefWindowProcA(window, message, wparam, lparam);
    }

    if (message == WM_DESTROY)
    {
        TRACE("unregister window %p.\n", window);
        (void) nine_unregister_window(window);
    }
    else if (message == WM_DISPLAYCHANGE)
    {
        /* Ex restores display mode, while non Ex requires the
         * user to call Device::Reset() */
        ZeroMemory(&current_mode, sizeof(DEVMODEW));
        current_mode.dmSize = sizeof(current_mode);
        if (!present->ex &&
            !present->params.Windowed &&
            present->params.hDeviceWindow &&
            EnumDisplaySettingsW(present->devname, ENUM_CURRENT_SETTINGS, &current_mode) &&
            (current_mode.dmPelsWidth != present->params.BackBufferWidth ||
             current_mode.dmPelsHeight != present->params.BackBufferHeight))
        {
            present->resolution_mismatch = TRUE;
        }
        else
        {
            present->resolution_mismatch = FALSE;
        }
    }
    else if (message == WM_ACTIVATEAPP)
    {
        drop_wnd_messages = present->drop_wnd_messages;
        present->drop_wnd_messages = TRUE;

        if (wparam == WA_INACTIVE)
        {
            present->occluded = TRUE;
            present->reapply_mode = TRUE;

            ZeroMemory(&new_mode, sizeof(DEVMODEW));
            new_mode.dmSize = sizeof(new_mode);
            if (EnumDisplaySettingsW(present->devname, ENUM_REGISTRY_SETTINGS, &new_mode))
                DRI3Present_ChangeDisplaySettingsIfNeccessary(present, &new_mode);

            if (!present->no_window_changes &&
                    IsWindowVisible(present->params.hDeviceWindow))
                ShowWindow(present->params.hDeviceWindow, SW_MINIMIZE);
        }
        else
        {
            present->occluded = FALSE;

            if (!present->no_window_changes)
            {
                /* restore window */
                SetWindowPos(present->params.hDeviceWindow, NULL, 0, 0,
                             present->params.BackBufferWidth, present->params.BackBufferHeight,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }

            if (present->ex)
            {
                ZeroMemory(&new_mode, sizeof(DEVMODEW));
                new_mode.dmSize = sizeof(new_mode);
                new_mode.dmPelsWidth = present->params.BackBufferWidth;
                new_mode.dmPelsHeight = present->params.BackBufferHeight;
                new_mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
                if (present->params.FullScreen_RefreshRateInHz)
                {
                    new_mode.dmFields |= DM_DISPLAYFREQUENCY;
                    new_mode.dmDisplayFrequency = present->params.FullScreen_RefreshRateInHz;
                }
                DRI3Present_ChangeDisplaySettingsIfNeccessary(present, &new_mode);
            }
        }
        present->drop_wnd_messages = drop_wnd_messages;
    }
    else if (message == WM_SYSCOMMAND)
    {
        if (wparam == SC_RESTORE)
        {
            if (unicode)
                DefWindowProcW(window, message, wparam, lparam);
            else
                DefWindowProcA(window, message, wparam, lparam);
        }
    }

    if (unicode)
        return CallWindowProcW(proc, window, message, wparam, lparam);
    else
        return CallWindowProcA(proc, window, message, wparam, lparam);
}

static void setup_fullscreen_window(struct DRI3Present *This,
        HWND hwnd, int w, int h)
{
    boolean drop_wnd_messages;
    LONG style, style_ex;

    This->style = GetWindowLongW(hwnd, GWL_STYLE);
    This->style_ex = GetWindowLongW(hwnd, GWL_EXSTYLE);

    style = fullscreen_style(This->style);
    style_ex = fullscreen_exstyle(This->style_ex);

    drop_wnd_messages = This->drop_wnd_messages;
    This->drop_wnd_messages = TRUE;

    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, style_ex);

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, w, h,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);

    This->drop_wnd_messages = drop_wnd_messages;
}

static void move_fullscreen_window(struct DRI3Present *This,
        HWND hwnd, int w, int h)
{
    boolean drop_wnd_messages;
    LONG style, style_ex;

    /* move draw window back to place */

    style = GetWindowLongW(hwnd, GWL_STYLE);
    style_ex = GetWindowLongW(hwnd, GWL_EXSTYLE);

    style = fullscreen_style(style);
    style_ex = fullscreen_exstyle(style_ex);

    drop_wnd_messages = This->drop_wnd_messages;
    This->drop_wnd_messages = TRUE;
    SetWindowLongW(hwnd, GWL_STYLE, style);
    SetWindowLongW(hwnd, GWL_EXSTYLE, style_ex);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, w, h,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    This->drop_wnd_messages = drop_wnd_messages;
}

static void restore_fullscreen_window(struct DRI3Present *This,
        HWND hwnd)
{
    boolean drop_wnd_messages;
    LONG style, style_ex;

    /* switch from fullscreen to window */
    style = GetWindowLongW(hwnd, GWL_STYLE);
    style_ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    /* These flags are set by us, not the
     * application, and we want to ignore them in the test below, since it's
     * not the application's fault that they changed. Additionally, we want to
     * preserve the current status of these flags (i.e. don't restore them) to
     * more closely emulate the behavior of Direct3D, which leaves these flags
     * alone when returning to windowed mode. */
    This->style ^= (This->style ^ style) & WS_VISIBLE;
    This->style_ex ^= (This->style_ex ^ style_ex) & WS_EX_TOPMOST;

    /* Only restore the style if the application didn't modify it during the
     * fullscreen phase. Some applications change it before calling Reset()
     * when switching between windowed and fullscreen modes (HL2), some
     * depend on the original style (Eve Online). */
    drop_wnd_messages = This->drop_wnd_messages;
    This->drop_wnd_messages = TRUE;
    if (style == fullscreen_style(This->style) &&
            style_ex == fullscreen_exstyle(This->style_ex))
    {
        SetWindowLongW(hwnd, GWL_STYLE, This->style);
        SetWindowLongW(hwnd, GWL_EXSTYLE, This->style_ex);
    }
    SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED |
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE);
    This->drop_wnd_messages = drop_wnd_messages;

    This->style = 0;
    This->style_ex = 0;
}

static void DRI3Present_UpdatePresentationInterval(struct DRI3Present *This)
{
    switch(This->params.PresentationInterval)
    {
        case D3DPRESENT_INTERVAL_DEFAULT:
        case D3DPRESENT_INTERVAL_ONE:
            This->present_interval = 1;
            This->present_async = FALSE;
            break;
        case D3DPRESENT_INTERVAL_TWO:
            This->present_interval = 2;
            This->present_async = FALSE;
            break;
        case D3DPRESENT_INTERVAL_THREE:
            This->present_interval = 3;
            This->present_async = FALSE;
            break;
        case D3DPRESENT_INTERVAL_FOUR:
            This->present_interval = 4;
            This->present_async = FALSE;
            break;
        case D3DPRESENT_INTERVAL_IMMEDIATE:
        default:
            This->present_interval = 0;
            This->present_async =
                !(This->params.SwapEffect == D3DSWAPEFFECT_DISCARD &&
                  This->tear_free_discard);
            break;
    }

    /* D3DSWAPEFFECT_COPY: Force Copy.
     * This->present_interval == 0: Force Copy to have buffers
     * release as soon as possible (the display server/compositor
     * won't hold any buffer), unless DISCARD and
     * allow_discard_delayed_release */
    This->present_swapeffectcopy =
        This->params.SwapEffect == D3DSWAPEFFECT_COPY ||
        (This->present_interval == 0 &&
        !(This->params.SwapEffect == D3DSWAPEFFECT_DISCARD &&
          This->allow_discard_delayed_release));
}

static HRESULT DRI3Present_ChangePresentParameters(struct DRI3Present *This,
        D3DPRESENT_PARAMETERS *params)
{
    HWND focus_window = This->focus_wnd ? This->focus_wnd : params->hDeviceWindow;
    RECT rect;
    DEVMODEW new_mode;
    HRESULT hr;
    boolean drop_wnd_messages;

    TRACE("This=%p, params=%p, focus_window=%p, params->hDeviceWindow=%p\n",
            This, params, focus_window, params->hDeviceWindow);

    This->params.SwapEffect = params->SwapEffect;
    This->params.AutoDepthStencilFormat = params->AutoDepthStencilFormat;
    This->params.Flags = params->Flags;
    This->params.FullScreen_RefreshRateInHz = params->FullScreen_RefreshRateInHz;
    This->params.PresentationInterval = params->PresentationInterval;
    This->params.EnableAutoDepthStencil = params->EnableAutoDepthStencil;
    if (!params->hDeviceWindow)
        params->hDeviceWindow = This->params.hDeviceWindow;
    else
        This->params.hDeviceWindow = params->hDeviceWindow;

    if ((This->params.BackBufferWidth != params->BackBufferWidth) ||
            (This->params.BackBufferHeight != params->BackBufferHeight) ||
            (This->params.Windowed != params->Windowed) ||
            This->reapply_mode)
    {
        This->reapply_mode = FALSE;

        if (!params->Windowed)
        {
            TRACE("Setting fullscreen mode: %dx%d@%d\n", params->BackBufferWidth,
                     params->BackBufferHeight, params->FullScreen_RefreshRateInHz);

            /* switch display mode */
            ZeroMemory(&new_mode, sizeof(DEVMODEW));
            new_mode.dmPelsWidth = params->BackBufferWidth;
            new_mode.dmPelsHeight = params->BackBufferHeight;
            new_mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
            if (params->FullScreen_RefreshRateInHz)
            {
                new_mode.dmFields |= DM_DISPLAYFREQUENCY;
                new_mode.dmDisplayFrequency = params->FullScreen_RefreshRateInHz;
            }
            new_mode.dmSize = sizeof(DEVMODEW);
            hr = DRI3Present_ChangeDisplaySettingsIfNeccessary(This, &new_mode);
            if (FAILED(hr))
                return hr;

            /* Dirty as BackBufferWidth and BackBufferHeight hasn't been set yet */
            This->resolution_mismatch = FALSE;
        }
        else if(!This->params.Windowed && params->Windowed)
        {
            TRACE("Setting fullscreen mode: %dx%d@%d\n", This->initial_mode.dmPelsWidth,
                    This->initial_mode.dmPelsHeight, This->initial_mode.dmDisplayFrequency);

            hr = DRI3Present_ChangeDisplaySettingsIfNeccessary(This, &This->initial_mode);
            if (FAILED(hr))
                return hr;

            /* Dirty as BackBufferWidth and BackBufferHeight hasn't been set yet */
            This->resolution_mismatch = FALSE;
        }

        if (This->params.Windowed)
        {
            if (!params->Windowed)
            {
                /* switch from window to fullscreen */
                if (!nine_register_window(focus_window, This))
                    return D3DERR_INVALIDCALL;

                SetWindowPos(focus_window, 0, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);

                setup_fullscreen_window(This, params->hDeviceWindow,
                        params->BackBufferWidth, params->BackBufferHeight);
            }
        }
        else
        {
            if (!params->Windowed)
            {
                /* switch from fullscreen to fullscreen */
                drop_wnd_messages = This->drop_wnd_messages;
                This->drop_wnd_messages = TRUE;
                MoveWindow(params->hDeviceWindow, 0, 0,
                        params->BackBufferWidth,
                        params->BackBufferHeight,
                        TRUE);
                This->drop_wnd_messages = drop_wnd_messages;
            }
            else if (This->style || This->style_ex)
            {
                restore_fullscreen_window(This, params->hDeviceWindow);
            }

            if (params->Windowed && !nine_unregister_window(focus_window))
                ERR("Window %p is not registered with nine.\n", focus_window);
        }
        This->params.Windowed = params->Windowed;
    }
    else if (!params->Windowed)
    {
        move_fullscreen_window(This, params->hDeviceWindow, params->BackBufferWidth, params->BackBufferHeight);
    }
    else
    {
        TRACE("Nothing changed.\n");
    }
    if (!params->BackBufferWidth || !params->BackBufferHeight) {
        if (!params->Windowed)
            return D3DERR_INVALIDCALL;

        if (!GetClientRect(params->hDeviceWindow, &rect))
            return D3DERR_INVALIDCALL;

        if (params->BackBufferWidth == 0)
            params->BackBufferWidth = rect.right - rect.left;

        if (params->BackBufferHeight == 0)
            params->BackBufferHeight = rect.bottom - rect.top;
    }

    /* Set as last in case of failed reset those aren't updated */
    This->params.BackBufferWidth = params->BackBufferWidth;
    This->params.BackBufferHeight = params->BackBufferHeight;
    This->params.BackBufferFormat = params->BackBufferFormat;
    This->params.BackBufferCount = params->BackBufferCount;
    This->params.MultiSampleType = params->MultiSampleType;
    This->params.MultiSampleQuality = params->MultiSampleQuality;

    DRI3Present_UpdatePresentationInterval(This);

    return D3D_OK;
}

/* The following code isn't based on WINE's wined3d or d3d9. */

static HRESULT DRI3Present_new(Display *gdi_display, const WCHAR *devname,
        D3DPRESENT_PARAMETERS *params, HWND focus_wnd, struct DRI3Present **out,
        boolean ex, boolean no_window_changes)
{
    struct DRI3Present *This;
    HWND focus_window;
    DEVMODEW new_mode;
    HRESULT hr;
    RECT rect;

    if (!focus_wnd && !params->hDeviceWindow)
    {
        ERR("No focus HWND specified for presentation backend.\n");
        return D3DERR_INVALIDCALL;
    }

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                     sizeof(struct DRI3Present));
    if (!This)
    {
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->gdi_display = gdi_display;
    This->vtable = &DRI3Present_vtable;
    This->refs = 1;
    This->focus_wnd = focus_wnd;
    This->ex = ex;
    This->no_window_changes = no_window_changes;

    /* store current resolution */
    ZeroMemory(&(This->initial_mode), sizeof(This->initial_mode));
    This->initial_mode.dmSize = sizeof(This->initial_mode);
    EnumDisplaySettingsExW(This->devname, ENUM_CURRENT_SETTINGS, &(This->initial_mode), 0);

    if (!params->hDeviceWindow)
        params->hDeviceWindow = This->focus_wnd;

    if (!params->Windowed) {
        focus_window = This->focus_wnd ? This->focus_wnd : params->hDeviceWindow;

        if (!nine_register_window(focus_window, This))
            return D3DERR_INVALIDCALL;

        SetWindowPos(focus_window, 0, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);

        /* switch display mode */
        ZeroMemory(&new_mode, sizeof(DEVMODEW));
        new_mode.dmSize = sizeof(DEVMODEW);
        new_mode.dmPelsWidth = params->BackBufferWidth;
        new_mode.dmPelsHeight = params->BackBufferHeight;
        new_mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

        if (params->FullScreen_RefreshRateInHz)
        {
            new_mode.dmFields |= DM_DISPLAYFREQUENCY;
            new_mode.dmDisplayFrequency = params->FullScreen_RefreshRateInHz;
        }

        hr = DRI3Present_ChangeDisplaySettingsIfNeccessary(This, &new_mode);
        if (FAILED(hr))
        {
            nine_unregister_window(focus_window);
            HeapFree(GetProcessHeap(), 0, This);
            return hr;
        }

        /* Dirty as BackBufferWidth and BackBufferHeight hasn't been set yet */
        This->resolution_mismatch = FALSE;

        setup_fullscreen_window(This, params->hDeviceWindow,
                params->BackBufferWidth, params->BackBufferHeight);
    } else {
        GetClientRect(params->hDeviceWindow, &rect);
        if (!params->BackBufferWidth || !params->BackBufferHeight) {

            if (params->BackBufferWidth == 0)
                params->BackBufferWidth = rect.right - rect.left;

            if (params->BackBufferHeight == 0)
                params->BackBufferHeight = rect.bottom - rect.top;
        }
    }

    This->params = *params;

    DRI3Present_UpdatePresentationInterval(This);

    strcpyW(This->devname, devname);

    PRESENTInit(gdi_display, &(This->present_priv));
#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback && !DRI2FallbackInit(gdi_display, &(This->dri2_priv)))
        return D3DERR_INVALIDCALL;
#endif
    *out = This;

    return D3D_OK;
}

struct DRI3PresentGroup
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    boolean ex;
    struct DRI3Present **present_backends;
    unsigned npresent_backends;
    Display *gdi_display;
    boolean no_window_changes;
};

static ULONG WINAPI DRI3PresentGroup_AddRef(struct DRI3PresentGroup *This)
{
    ULONG refs = InterlockedIncrement(&This->refs);
    TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI DRI3PresentGroup_Release(struct DRI3PresentGroup *This)
{
    ULONG refs = InterlockedDecrement(&This->refs);
    TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0)
    {
        unsigned i;
        if (This->present_backends)
        {
            for (i = 0; i < This->npresent_backends; ++i)
            {
                if (This->present_backends[i])
                    DRI3Present_Release(This->present_backends[i]);
            }
            HeapFree(GetProcessHeap(), 0, This->present_backends);
        }
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI DRI3PresentGroup_QueryInterface(struct DRI3PresentGroup *This,
        REFIID riid, void **ppvObject )
{
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualGUID(&IID_ID3DPresentGroup, riid) ||
            IsEqualGUID(&IID_IUnknown, riid))
    {
        *ppvObject = This;
        DRI3PresentGroup_AddRef(This);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static UINT WINAPI DRI3PresentGroup_GetMultiheadCount(struct DRI3PresentGroup *This)
{
    FIXME("(%p), stub!\n", This);
    return 1;
}

static HRESULT WINAPI DRI3PresentGroup_GetPresent(struct DRI3PresentGroup *This,
        UINT Index, ID3DPresent **ppPresent)
{
    if (Index >= DRI3PresentGroup_GetMultiheadCount(This))
    {
        ERR("Index >= MultiHeadCount\n");
        return D3DERR_INVALIDCALL;
    }
    DRI3Present_AddRef(This->present_backends[Index]);
    *ppPresent = (ID3DPresent *)This->present_backends[Index];

    return D3D_OK;
}

static HRESULT WINAPI DRI3PresentGroup_CreateAdditionalPresent(struct DRI3PresentGroup *This,
        D3DPRESENT_PARAMETERS *pPresentationParameters, ID3DPresent **ppPresent)
{
    HRESULT hr;
    hr = DRI3Present_new(This->gdi_display, This->present_backends[0]->devname,
            pPresentationParameters, 0, (struct DRI3Present **)ppPresent,
            This->ex, This->no_window_changes);

    return hr;
}

static void WINAPI DRI3PresentGroup_GetVersion(struct DRI3PresentGroup *This,
        int *major, int *minor)
{
    *major = WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MAJOR;
    *minor = WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR;
}

static ID3DPresentGroupVtbl DRI3PresentGroup_vtable = {
    (void *)DRI3PresentGroup_QueryInterface,
    (void *)DRI3PresentGroup_AddRef,
    (void *)DRI3PresentGroup_Release,
    (void *)DRI3PresentGroup_GetMultiheadCount,
    (void *)DRI3PresentGroup_GetPresent,
    (void *)DRI3PresentGroup_CreateAdditionalPresent,
    (void *)DRI3PresentGroup_GetVersion
};

HRESULT present_create_present_group(Display *gdi_display, const WCHAR *device_name,
        UINT adapter, HWND focus_wnd, D3DPRESENT_PARAMETERS *params,
        unsigned nparams, ID3DPresentGroup **group, boolean ex, DWORD BehaviorFlags)
{
    struct DRI3PresentGroup *This;
    DISPLAY_DEVICEW dd;
    HRESULT hr;
    unsigned i;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct DRI3PresentGroup));
    if (!This)
    {
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->gdi_display = gdi_display;
    This->vtable = &DRI3PresentGroup_vtable;
    This->refs = 1;
    This->ex = ex;
    This->npresent_backends = nparams;
    This->no_window_changes = !!(BehaviorFlags & D3DCREATE_NOWINDOWCHANGES);
    This->present_backends = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            This->npresent_backends * sizeof(struct DRI3Present *));
    if (!This->present_backends)
    {
        DRI3PresentGroup_Release(This);
        ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    if (nparams != 1)
        adapter = 0;

    for (i = 0; i < This->npresent_backends; ++i)
    {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        /* find final device name */
        if (!EnumDisplayDevicesW(device_name, adapter + i, &dd, 0))
        {
            WARN("Couldn't find subdevice %d from `%s'\n",
                    i, debugstr_w(device_name));
        }

        /* create an ID3DPresent for it */
        hr = DRI3Present_new(gdi_display, dd.DeviceName, &params[i],
                focus_wnd, &This->present_backends[i], ex, This->no_window_changes);
        if (FAILED(hr))
        {
            DRI3PresentGroup_Release(This);
            return hr;
        }
    }

    *group = (ID3DPresentGroup *)This;
    TRACE("Returning %p\n", *group);

    return D3D_OK;
}

HRESULT present_create_adapter9(Display *gdi_display, HDC hdc, ID3DAdapter9 **out)
{
    struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };
    HRESULT hr;
    int fd;

    if (!d3d9_drm)
    {
        ERR("DRM drivers are not supported on your system.\n");
        return D3DERR_DRIVERINTERNALERROR;
    }

    if (ExtEscape(hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
            sizeof(extesc), (LPSTR)&extesc) <= 0)
        ERR("X11 drawable lookup failed (hdc=%p)\n", hdc);

#ifdef D3D9NINE_DRI2
    if (!is_dri2_fallback && !DRI3Open(gdi_display, DefaultScreen(gdi_display), &fd))
#else
    if (!DRI3Open(gdi_display, DefaultScreen(gdi_display), &fd))
#endif
    {
        ERR("DRI3Open failed (fd=%d)\n", fd);
        return D3DERR_DRIVERINTERNALERROR;
    }
#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback && !DRI2FallbackOpen(gdi_display, DefaultScreen(gdi_display), &fd))
    {
        ERR("DRI2Open failed (fd=%d)\n", fd);
        return D3DERR_DRIVERINTERNALERROR;
    }
#endif
    hr = d3d9_drm->create_adapter(fd, out);
    if (FAILED(hr))
    {
        ERR("Unable to create ID3DAdapter9 (fd=%d)\n", fd);
        return hr;
    }

    TRACE("Created ID3DAdapter9 with fd %d\n", fd);

    return D3D_OK;
}

BOOL present_has_d3dadapter(Display *gdi_display)
{
    static const void * WINAPI (*pD3DAdapter9GetProc)(const char *);
    static void *handle = NULL;
    static int done = 0;
    HKEY regkey;
    LSTATUS rc;
    char *path = NULL;

    char errbuf[256];
    char pathbuf[MAX_PATH];

    /* like in opengl.c (single threaded assumption OK?) */
    if (done)
        return handle != NULL;
    done = 1;

    if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\Direct3DNine", &regkey))
    {
        DWORD type;
        DWORD size = 0;

        rc = RegQueryValueExA(regkey, "ModulePath", 0, &type, NULL, &size);
        if (rc == ERROR_FILE_NOT_FOUND)
            goto use_default_path;

        TRACE("Reading registry key for module path\n");
        if (rc != ERROR_SUCCESS  || type != REG_SZ)
        {
            ERR("Failed to read Direct3DNine ModulePath registry key: Invalid content\n");
            goto cleanup;
        }

        path = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 1);
        if (!path)
        {
            ERR("Out of memory\n");
            return FALSE;
        }
        rc = RegQueryValueExA(regkey, "ModulePath", 0, &type, (LPBYTE)path, &size);
        if (rc != ERROR_SUCCESS)
        {
            ERR("Failed to read Direct3DNine registry\n");
            goto cleanup;
        }
        /* Split colon separated path for multi-arch support */
        if (strstr(path, ":"))
        {
            char *tmp_path = strstr(path, ":");

            /* Replace colon by string terminate */
            *tmp_path = 0;
            tmp_path ++;
            handle = wine_dlopen(path,
                    RTLD_GLOBAL | RTLD_NOW, errbuf, sizeof(errbuf));
            if (!handle)
            {
                TRACE("Failed to load '%s': %s\n", path, errbuf);

                handle = wine_dlopen(tmp_path,
                        RTLD_GLOBAL | RTLD_NOW, errbuf, sizeof(errbuf));
                if (!handle)
                {
                    TRACE("Failed to load '%s': %s\n", tmp_path, errbuf);
                    ERR("Failed to load '%s' and '%s' set by ModulePath.\n",
                            path, tmp_path);
                    goto cleanup;
                }
            }
        }
        else
        {
            handle = wine_dlopen(path,
                    RTLD_GLOBAL | RTLD_NOW, errbuf, sizeof(errbuf));
            if (!handle)
            {
                TRACE("Failed to load %s: %s\n", path, errbuf);
                ERR("Failed to load '%s' set by ModulePath.\n", path);
                goto cleanup;
            }
        }
        memcpy(pathbuf, path, size >= sizeof(pathbuf) ? (sizeof(pathbuf)-1) : size);
        pathbuf[sizeof(pathbuf)-1] = 0;

        HeapFree(GetProcessHeap(), 0, path);
    }

use_default_path:
#if !defined(D3D9NINE_MODULEPATH)
    if (!handle)
    {
        ERR("d3d9-nine.dll was built without default module path.\n"
                "Setting Software\\Wine\\Direct3DNine ModulePath is required\n");
        goto cleanup;
    }
#else
    if (!handle)
    {
        handle = wine_dlopen(D3D9NINE_MODULEPATH,
                RTLD_GLOBAL | RTLD_NOW, errbuf, sizeof(errbuf));
        if (!handle)
        {
            ERR("Failed to load '%s': %s\n", D3D9NINE_MODULEPATH, errbuf);
            goto cleanup;
        }
        memcpy(pathbuf, D3D9NINE_MODULEPATH,
               sizeof(D3D9NINE_MODULEPATH) >= sizeof(pathbuf) ?
                   (sizeof(pathbuf)-1) : sizeof(D3D9NINE_MODULEPATH));
        pathbuf[sizeof(pathbuf)-1] = 0;
    }
#endif
    /* find our entry point in d3dadapter9 */
    pD3DAdapter9GetProc = wine_dlsym(handle, "D3DAdapter9GetProc",
            errbuf, sizeof(errbuf));
    if (!pD3DAdapter9GetProc)
    {
        ERR("Failed to get the entry point from %s: %s", pathbuf, errbuf);
        goto cleanup;
    }

    /* get a handle to the drm backend struct */
    d3d9_drm = pD3DAdapter9GetProc("drm");
    if (!d3d9_drm)
    {
        ERR("%s doesn't support the drm backend.\n", pathbuf);
        goto cleanup;
    }

    /* verify that we're binary compatible */
    if (d3d9_drm->major_version != 0)
    {
        ERR("Version mismatch. %s has %d.%d, was expecting 0.x\n",
                pathbuf, d3d9_drm->major_version, d3d9_drm->minor_version);
        goto cleanup;
    }

    /* this will be used to store d3d_drawables */
    d3d_hwnd_context = XUniqueContext();

    if (!PRESENTCheckExtension(gdi_display, 1, 0))
    {
        ERR("Unable to query PRESENT.\n");
        goto cleanup;
    }

    if (!DRI3CheckExtension(gdi_display, 1, 0))
    {
#ifndef D3D9NINE_DRI2
        ERR("Unable to query DRI3.\n");
        goto cleanup;
#else
        ERR("Unable to query DRI3. Trying DRI2 fallback (slower performance).\n");
        is_dri2_fallback = 1;
        if (!DRI2FallbackCheckSupport(gdi_display))
        {
            ERR("DRI2 fallback unsupported\n");
            goto cleanup;
        }
#endif
    }

    return TRUE;

cleanup:
    ERR("\033[1;31m\nNative Direct3D 9 will be unavailable."
            "\nFor more information visit https://wiki.ixit.cz/d3d9\033[0m\n");
    if (handle)
    {
        wine_dlclose(handle, NULL, 0);
        handle = NULL;
    }

    if (path)
        HeapFree(GetProcessHeap(), 0, path);

    return FALSE;
}

BOOL enable_device_vtable_wrapper(void)
{
    if (!d3d9_drm)
    {
        ERR("enable_device_vtable_wrapper call before init.\n");
        return FALSE;
    }
    /* Since minor version 1, we can assume a copy of the internal vtable is stored in second pos.
     * For now always enable if possible the wrapper (enables Steam overlay for example),
     * we might in the future let user choose. */
    return d3d9_drm->minor_version >= 1;
}

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine ID3DAdapter9 support functions
 *
 * Copyright 2013 Joakim Sindholt
 *                Christoph Bumiller
 * Copyright 2014 Tiziano Bacocco
 *                David Heidelberger
 * Copyright 2014-2015 Axel Davy
 * Copyright 2015 Patrick Rudolph
 */

#include <d3dadapter/drm.h>
#include <wine/debug.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

#include "../common/library.h"
#include "backend.h"
#include "wndproc.h"
#include "xcb_present.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d9nine);

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
      0, 0, { /*(DWORD_PTR)(__FILE__ ": context_section")*/ }
};
static CRITICAL_SECTION context_section = { &critsect_debug, -1, 0, 0, 0, 0 };

const GUID IID_ID3DPresent = { 0x77D60E80, 0xF1E6, 0x11DF, { 0x9E, 0x39, 0x95, 0x0C, 0xDF, 0xD7, 0x20, 0x85 } };
const GUID IID_ID3DPresentGroup = { 0xB9C3016E, 0xF32A, 0x11DF, { 0x9C, 0x18, 0x92, 0xEA, 0xDE, 0xD7, 0x20, 0x85 } };

struct d3d_drawable
{
    Drawable drawable; /* X11 drawable */
    HDC hdc;
    HWND wnd; /* HWND (for convenience) */
    RECT windowRect;
    POINT offset; /* offset of the client area compared to the X11 drawable */
    unsigned int width;
    unsigned int height;
    unsigned int depth;
};

struct DRIPresent
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    D3DPRESENT_PARAMETERS params;
    HWND focus_wnd;
    PRESENTpriv *present_priv;

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

    struct dri_backend *dri_backend;
};

static void free_d3dadapter_drawable(struct d3d_drawable *d3d)
{
    ReleaseDC(d3d->wnd, d3d->hdc);
    HeapFree(GetProcessHeap(), 0, d3d);
}

static void destroy_d3dadapter_drawable(Display *gdi_display, HWND hwnd)
{
    struct d3d_drawable *d3d;
    //WINE_TRACE("This=%p hwnd=%p\n", gdi_display, hwnd);

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd,
            d3d_hwnd_context, (char **)&d3d))
    {
        XDeleteContext(gdi_display, (XID)hwnd, d3d_hwnd_context);
        free_d3dadapter_drawable(d3d);
    }
    LeaveCriticalSection(&context_section);
}

/* Compute the position of a drawable compared to a parent */
static void get_relative_position(Display *display, Drawable drawable, Drawable parent, POINT *offset)
{
    Window Wroot, Wparent, *Wchildren;
    int resx = 0, resy = 0, dx, dy;
    unsigned int width, height, border_width, depth, children;

    while (1) {
        if (!XGetGeometry(display, drawable, &Wroot, &dx, &dy, &width, &height, &border_width, &depth))
            break;

        WINE_TRACE("Next geometry: %d %d\n", dx, dy);

        /* Should we really add border_width ? */
        resx += dx + border_width;
        resy += dy + border_width;

        if (!XQueryTree(display, drawable, &Wroot, &Wparent, &Wchildren, &children))
            break;

        if (Wchildren)
            XFree(Wchildren);

        if (Wparent == Wroot || Wparent == parent)
        {
            WINE_TRACE("Successfully determined drawable pos (debug: %ld, %ld, %ld)\n", drawable, Wroot, parent);
            break;
        }

        drawable = Wparent;
    }

    offset->x = resx;
    offset->y = resy;
}

static BOOL CALLBACK edm_callback(HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM lp)
{
    RECT *r = (RECT *)lp;

    UnionRect(r, r, rect);
    return TRUE;
}

/* see wine's get_virtual_screen_rect() */
static void offset_by_virtual_screen(POINT *pt)
{
    RECT r;

    SetRectEmpty(&r);
    EnumDisplayMonitors(0, NULL, edm_callback, (LPARAM)&r);

    WINE_TRACE("Virtual screen size: %s\n", wine_dbgstr_rect(&r));

    pt->x -= r.left;
    pt->y -= r.top;
}

static BOOL get_wine_drawable_from_dc(HDC hdc, Drawable *drawable)
{
    struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };

    if (ExtEscape(hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
                  sizeof(extesc), (LPSTR)&extesc) <= 0)
    {
        WINE_ERR("Unexpected error in X Drawable lookup (hdc=%p)\n", hdc);
        return FALSE;
    }

    if (drawable)
        *drawable = extesc.drawable;

    return TRUE;
}

static BOOL get_wine_drawable_from_wnd(HWND hwnd, Drawable *drawable, HDC *hdc)
{
    HDC h;

    h = GetDCEx(hwnd, 0, DCX_CACHE | DCX_CLIPSIBLINGS);
    if (!h)
        return FALSE;

    if (!get_wine_drawable_from_dc(h, drawable))
    {
        ReleaseDC(hwnd, h);
        return FALSE;
    }

    if (hdc)
        *hdc = h;
    else
        ReleaseDC(hwnd, h);

    return TRUE;
}

static void DRIPresent_FillOffset(Display *gdi_display, struct d3d_drawable *d3d)
{
    Drawable wineRoot;
    POINT pt;
    Window Wroot;
    int x, y;
    unsigned int border_width;

    //WINE_TRACE("hwnd=%p\n", d3d->wnd);

    /* Finding the offset is hard because a drawable
     * doesn't always start a the top left of a hwnd window,
     * for example if the windows window decoration is replaced
     * by the window managed.
     * In the case of non-virtual desktop, wine root is
     * the X root.
     * In the case of virtual desktop, We assume the root drawable
     * begins at pos (0, 0) */

    d3d->offset.x = d3d->offset.y = 0;

    if (!get_wine_drawable_from_wnd(GetDesktopWindow(), &wineRoot, NULL))
        return;

    if (!XGetGeometry(gdi_display, d3d->drawable, &Wroot, &x, &y, &d3d->width, &d3d->height, &border_width, &d3d->depth))
    {
        d3d->width = 0;
        d3d->height = 0;
        d3d->depth= 0;
    }

    /* The position of the top left client area compared to wine root window */
    pt.x = pt.y = 0;
    if (!ClientToScreen(d3d->wnd, &pt))
    {
        WINE_ERR("ClientToScreen failed: 0x%x\n", GetLastError());
        return;
    }
    WINE_TRACE("Relative coord client area: %d %d\n", pt.x, pt.y);
    offset_by_virtual_screen(&pt);
    WINE_TRACE("Coord client area: %d %d\n", pt.x, pt.y);
    d3d->offset.x += pt.x;
    d3d->offset.y += pt.y;

    get_relative_position(gdi_display, d3d->drawable, wineRoot, &pt);
    WINE_TRACE("Coord drawable: %d %d\n", pt.x, pt.y);
    d3d->offset.x -= pt.x;
    d3d->offset.y -= pt.y;

    WINE_TRACE("Offset: %d %d\n", d3d->offset.x, d3d->offset.y);
}

static struct d3d_drawable *create_d3dadapter_drawable(Display *gdi_display, HWND hwnd)
{
    struct d3d_drawable *d3d;

    //WINE_TRACE("hwnd=%p\n", hwnd);

    d3d = HeapAlloc(GetProcessHeap(), 0, sizeof(*d3d));
    if (!d3d)
    {
        WINE_ERR("Couldn't allocate d3d_drawable.\n");
        return NULL;
    }

    if (!get_wine_drawable_from_wnd(hwnd, &d3d->drawable, &d3d->hdc))
    {
        ReleaseDC(hwnd, d3d->hdc);
        HeapFree(GetProcessHeap(), 0, d3d);
        return NULL;
    }

    WINE_TRACE("hwnd drawable: %ld\n", d3d->drawable);
    d3d->wnd = hwnd;
    GetWindowRect(hwnd, &d3d->windowRect);
    DRIPresent_FillOffset(gdi_display, d3d);

    return d3d;
}

static struct d3d_drawable *get_d3d_drawable(Display *gdi_display, HWND hwnd)
{
    struct d3d_drawable *d3d, *race;

    //WINE_TRACE("hwnd=%p\n", hwnd);

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd, d3d_hwnd_context, (char **)&d3d))
    {
        return d3d;
    }
    LeaveCriticalSection(&context_section);

    WINE_TRACE("No d3d_drawable attached to hwnd %p, creating one.\n", hwnd);

    d3d = create_d3dadapter_drawable(gdi_display, hwnd);
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
        WINE_ERR("Driver internal error: d3d_drawable is NULL\n");
    LeaveCriticalSection(&context_section);
}

static ULONG WINAPI DRIPresent_AddRef(struct DRIPresent *This)
{
    ULONG refs = InterlockedIncrement(&This->refs);
    WINE_TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI DRIPresent_Release(struct DRIPresent *This)
{
    ULONG refs = InterlockedDecrement(&This->refs);
    WINE_TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0)
    {
        /* dtor */
        (void) nine_unregister_window(This->focus_wnd);
        if (This->d3d)
            destroy_d3dadapter_drawable(This->gdi_display, This->d3d->wnd);
        ChangeDisplaySettingsExW(This->devname, &(This->initial_mode), 0, CDS_FULLSCREEN, NULL);
        PRESENTDestroy(This->present_priv);
        This->dri_backend->funcs->deinit(This->dri_backend->priv);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI DRIPresent_QueryInterface(struct DRIPresent *This,
        REFIID riid, void **ppvObject)
{
    if (!ppvObject)
        return E_POINTER;

    if (IsEqualGUID(&IID_ID3DPresent, riid) ||
            IsEqualGUID(&IID_IUnknown, riid))
    {
        *ppvObject = This;
        DRIPresent_AddRef(This);
        return S_OK;
    }

    WINE_WARN("%s not implemented, returning E_NOINTERFACE.\n", wine_dbgstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static HRESULT DRIPresent_ChangePresentParameters(struct DRIPresent *This,
        D3DPRESENT_PARAMETERS *params);

static HRESULT WINAPI DRIPresent_SetPresentParameters(struct DRIPresent *This,
        D3DPRESENT_PARAMETERS *pPresentationParameters,
        D3DDISPLAYMODEEX *pFullscreenDisplayMode)
{
    if (pFullscreenDisplayMode)
        WINE_FIXME("Ignoring pFullscreenDisplayMode\n");
    return DRIPresent_ChangePresentParameters(This, pPresentationParameters);
}

static HRESULT WINAPI DRIPresent_D3DWindowBufferFromDmaBuf(struct DRIPresent *This,
        int dmaBufFd, int width, int height, int stride, int depth,
        int bpp, struct D3DWindowBuffer **out)
{
    const struct dri_backend *dri_backend = This->dri_backend;

    if (!dri_backend->funcs->window_buffer_from_dmabuf(dri_backend->priv,
            This->present_priv, dmaBufFd, width, height, stride, depth, bpp, out))
    {
        WINE_ERR("window_buffer_from_dmabuf failed\n");
        return D3DERR_DRIVERINTERNALERROR;
    }

    //WINE_TRACE("This=%p buffer=%p\n", This, *out);
    return D3D_OK;
}

static HRESULT WINAPI DRIPresent_DestroyD3DWindowBuffer(struct DRIPresent *This,
        struct D3DWindowBuffer *buffer)
{
    const struct dri_backend *dri_backend = This->dri_backend;

    /* the pixmap is managed by the PRESENT backend.
     * But if it can delete it right away, we may have
     * better performance */
    //WINE_TRACE("This=%p buffer=%p of priv %p\n", This, buffer, buffer->present_pixmap_priv);
    PRESENTTryFreePixmap(buffer->present_pixmap_priv);
    dri_backend->funcs->destroy_pixmap(dri_backend->priv, buffer->priv);
    HeapFree(GetProcessHeap(), 0, buffer);
    return D3D_OK;
}

static HRESULT WINAPI DRIPresent_WaitBufferReleased(struct DRIPresent *This,
        struct D3DWindowBuffer *buffer)
{
    //WINE_TRACE("This=%p buffer=%p\n", This, buffer);
    if(!PRESENTWaitPixmapReleased(buffer->present_pixmap_priv))
    {
        WINE_ERR("PRESENTWaitPixmapReleased failed\n");
        return D3DERR_DRIVERINTERNALERROR;
    }
    return D3D_OK;
}

static HRESULT WINAPI DRIPresent_FrontBufferCopy(struct DRIPresent *This,
        struct D3DWindowBuffer *buffer)
{
    const struct dri_backend *dri_backend = This->dri_backend;

    if (!dri_backend->funcs->copy_front(buffer->present_pixmap_priv))
        return D3DERR_DRIVERINTERNALERROR;

    return D3D_OK;
}

static HRESULT WINAPI DRIPresent_PresentBuffer( struct DRIPresent *This,
        struct D3DWindowBuffer *buffer, HWND hWndOverride, const RECT *pSourceRect,
        const RECT *pDestRect, const RGNDATA *pDirtyRegion, DWORD Flags )
{
    const struct dri_backend *dri_backend = This->dri_backend;
    struct d3d_drawable *d3d;
    RECT dest_translate;
    RECT windowRect;
    RECT offset;
    HWND hwnd;

    if (hWndOverride)
        hwnd = hWndOverride;
    else if (This->params.hDeviceWindow)
        hwnd = This->params.hDeviceWindow;
    else
        hwnd = This->focus_wnd;

    //WINE_TRACE("This=%p hwnd=%p\n", This, hwnd);

    d3d = get_d3d_drawable(This->gdi_display, hwnd);

    if (!d3d)
        return D3DERR_DRIVERINTERNALERROR;

    /* TODO: should we use a list here instead ? */
    if (This->d3d && (This->d3d->wnd != d3d->wnd))
        destroy_d3dadapter_drawable(This->gdi_display, This->d3d->wnd);

    This->d3d = d3d;

    GetWindowRect(d3d->wnd, &windowRect);
    /* The "correct" way to detect offset changes
     * would be to catch any window related change with a
     * listener. But it is complicated and this heuristic
     * is fast and should work well. */
    if (windowRect.top != d3d->windowRect.top ||
        windowRect.left != d3d->windowRect.left ||
        windowRect.bottom != d3d->windowRect.bottom ||
        windowRect.right != d3d->windowRect.right)
    {
        d3d->windowRect = windowRect;
        DRIPresent_FillOffset(This->gdi_display, d3d);
    }

    GetClientRect(d3d->wnd, &offset);
    offset.left += d3d->offset.x;
    offset.top += d3d->offset.y;
    offset.right += d3d->offset.x;
    offset.bottom += d3d->offset.y;

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

    if (!PRESENTPixmapPrepare(d3d->drawable, buffer->present_pixmap_priv))
    {
        release_d3d_drawable(d3d);
        WINE_ERR("PresentPrepare call failed\n");
        return D3DERR_DRIVERINTERNALERROR;
    }

    /* FIMXE: Do we need to aquire present mutex here? */
    dri_backend->funcs->present_pixmap(dri_backend->priv, buffer->priv);

    if (!PRESENTPixmap(d3d->drawable, buffer->present_pixmap_priv,
            This->present_interval, This->present_async, This->present_swapeffectcopy,
            pSourceRect, pDestRect, pDirtyRegion))
    {
        release_d3d_drawable(d3d);
        WINE_TRACE("Present call failed\n");
        return D3DERR_DRIVERINTERNALERROR;
    }
    release_d3d_drawable(d3d);

    return D3D_OK;
}

/* Based on wine's wined3d_get_adapter_raster_status. */
static HRESULT WINAPI DRIPresent_GetRasterStatus( struct DRIPresent *This,
        D3DRASTER_STATUS *pRasterStatus )
{
    LONGLONG freq_per_frame, freq_per_line;
    LARGE_INTEGER counter, freq_per_sec;
    unsigned refresh_rate, height;

    WINE_TRACE("This=%p, pRasterStatus=%p\n", This, pRasterStatus);

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

    WINE_TRACE("refresh_rate=%u, height=%u\n", refresh_rate, height);

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

    WINE_TRACE("Returning fake value, InVBlank %u, ScanLine %u.\n",
            pRasterStatus->InVBlank, pRasterStatus->ScanLine);

    return D3D_OK;
}

static HRESULT WINAPI DRIPresent_GetDisplayMode( struct DRIPresent *This,
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
            WINE_WARN("Unknown display format with %u bpp.\n", dm.dmBitsPerPel);
            pMode->Format = D3DFMT_UNKNOWN;
    }

    switch (dm.dmDisplayOrientation)
    {
        case DMDO_DEFAULT: *pRotation = D3DDISPLAYROTATION_IDENTITY; break;
        case DMDO_90:      *pRotation = D3DDISPLAYROTATION_90; break;
        case DMDO_180:     *pRotation = D3DDISPLAYROTATION_180; break;
        case DMDO_270:     *pRotation = D3DDISPLAYROTATION_270; break;
        default:
            WINE_WARN("Unknown display rotation %u.\n", dm.dmDisplayOrientation);
            *pRotation = D3DDISPLAYROTATION_IDENTITY;
    }

    return D3D_OK;
}

static HRESULT WINAPI DRIPresent_GetPresentStats( struct DRIPresent *This, D3DPRESENTSTATS *pStats )
{
    WINE_FIXME("(%p, %p), stub!\n", This, pStats);
    return D3DERR_INVALIDCALL;
}

static HRESULT WINAPI DRIPresent_GetCursorPos( struct DRIPresent *This, POINT *pPoint )
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

static HRESULT WINAPI DRIPresent_SetCursorPos( struct DRIPresent *This, POINT *pPoint )
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
static HRESULT WINAPI DRIPresent_SetCursor( struct DRIPresent *This, void *pBitmap,
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

static HRESULT WINAPI DRIPresent_SetGammaRamp( struct DRIPresent *This,
        const D3DGAMMARAMP *pRamp, HWND hWndOverride )
{
    HWND draw_window = This->params.hDeviceWindow ?
        This->params.hDeviceWindow : This->focus_wnd;
    HWND hWnd = hWndOverride ? hWndOverride : draw_window;
    HDC hdc;
    BOOL ok;
    if (!pRamp)
        return D3DERR_INVALIDCALL;

    hdc = GetDC(hWnd);
    ok = SetDeviceGammaRamp(hdc, (void *)pRamp);
    ReleaseDC(hWnd, hdc);
    return ok ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI DRIPresent_GetWindowInfo( struct DRIPresent *This,
        HWND hWnd, int *width, int *height, int *depth )
{
    HWND draw_window = This->params.hDeviceWindow ?
        This->params.hDeviceWindow : This->focus_wnd;
    HRESULT hr;
    RECT pRect;

    //WINE_TRACE("This=%p hwnd=%p\n", This, hWnd);

    /* For fullscreen modes, use the dimensions of the X11 window instead of
     * the game window. This is for compability with Valve's "fullscreen hack",
     * which won't switch to the game's resolution anymore, but instead scales
     * the game window to the root window. Only then can page flipping be used.
     */
    if (!This->params.Windowed && This->d3d)
        if (This->d3d->width > 0 && This->d3d->height > 0 && This->d3d->depth > 0)
        {
            *width = This->d3d->width;
            *height = This->d3d->height;
            *depth = This->d3d->depth;
            return D3D_OK;
        }

    if (!hWnd)
        hWnd = draw_window;
    hr = GetClientRect(hWnd, &pRect);
    if (!hr)
        return D3DERR_INVALIDCALL;
    //WINE_TRACE("pRect: %d %d %d %d\n", pRect.left, pRect.top, pRect.right, pRect.bottom);
    *width = pRect.right - pRect.left;
    *height = pRect.bottom - pRect.top;
    *depth = 24; //TODO
    return D3D_OK;
}

#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 1
static BOOL WINAPI DRIPresent_GetWindowOccluded(struct DRIPresent *This)
{
    return This->occluded;
}
#endif

#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 2
static BOOL WINAPI DRIPresent_ResolutionMismatch(struct DRIPresent *This)
{
    /* The resolution might change due to a third party app.
     * Poll this function to get the device's resolution match.
     * A device reset is required to restore the requested resolution.
     */
    return This->resolution_mismatch;
}

static HANDLE WINAPI DRIPresent_CreateThread( struct DRIPresent *This,
        void *pThreadfunc, void *pParam )
{
    LPTHREAD_START_ROUTINE lpStartAddress =
            (LPTHREAD_START_ROUTINE) pThreadfunc;

    return CreateThread(NULL, 0, lpStartAddress, pParam, 0, NULL);
}

static BOOL WINAPI DRIPresent_WaitForThread( struct DRIPresent *This, HANDLE thread )
{
    DWORD ExitCode = 0;
    while (GetExitCodeThread(thread, &ExitCode) && ExitCode == STILL_ACTIVE)
        Sleep(10);

    return TRUE;
}
#endif

#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 3
static HRESULT WINAPI DRIPresent_SetPresentParameters2( struct DRIPresent *This, D3DPRESENT_PARAMETERS2 *pParams )
{
    This->allow_discard_delayed_release = pParams->AllowDISCARDDelayedRelease;
    This->tear_free_discard = pParams->AllowDISCARDDelayedRelease && pParams->TearFreeDISCARD;
    return D3D_OK;
}

static BOOL WINAPI DRIPresent_IsBufferReleased( struct DRIPresent *This, struct D3DWindowBuffer *buffer )
{
    //WINE_TRACE("This=%p buffer=%p\n", This, buffer);
    return PRESENTIsPixmapReleased(buffer->present_pixmap_priv);
}

static HRESULT WINAPI DRIPresent_WaitBufferReleaseEvent( struct DRIPresent *This )
{
    PRESENTWaitReleaseEvent(This->present_priv);
    return D3D_OK;
}
#endif

/*----------*/

static ID3DPresentVtbl DRIPresent_vtable = {
    (void *)DRIPresent_QueryInterface,
    (void *)DRIPresent_AddRef,
    (void *)DRIPresent_Release,
    (void *)DRIPresent_SetPresentParameters,
    (void *)DRIPresent_D3DWindowBufferFromDmaBuf,
    (void *)DRIPresent_DestroyD3DWindowBuffer,
    (void *)DRIPresent_WaitBufferReleased,
    (void *)DRIPresent_FrontBufferCopy,
    (void *)DRIPresent_PresentBuffer,
    (void *)DRIPresent_GetRasterStatus,
    (void *)DRIPresent_GetDisplayMode,
    (void *)DRIPresent_GetPresentStats,
    (void *)DRIPresent_GetCursorPos,
    (void *)DRIPresent_SetCursorPos,
    (void *)DRIPresent_SetCursor,
    (void *)DRIPresent_SetGammaRamp,
    (void *)DRIPresent_GetWindowInfo,
#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 1
    (void *)DRIPresent_GetWindowOccluded,
#endif
#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 2
    (void *)DRIPresent_ResolutionMismatch,
    (void *)DRIPresent_CreateThread,
    (void *)DRIPresent_WaitForThread,
#endif
#if WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR >= 3
    (void *)DRIPresent_SetPresentParameters2,
    (void *)DRIPresent_IsBufferReleased,
    (void *)DRIPresent_WaitBufferReleaseEvent,
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

static HRESULT DRIPresent_ChangeDisplaySettingsIfNeccessary(struct DRIPresent *This, DEVMODEW *new_mode)
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
       WINE_ERR("Failed to get current display mode.\n");
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
                    WINE_ERR("ChangeDisplaySettingsExW failed with 0x%08X\n", hr);
                    return D3DERR_INVALIDCALL;
                }
            }
            else
            {
                WINE_ERR("ChangeDisplaySettingsExW failed with 0x%08X\n", hr);
                return D3DERR_INVALIDCALL;
            }
        }
    }
    return D3D_OK;
}

LRESULT device_process_message(struct DRIPresent *present, HWND window, BOOL unicode,
        UINT message, WPARAM wparam, LPARAM lparam, WNDPROC proc)
{
    boolean drop_wnd_messages;
    DEVMODEW current_mode;
    DEVMODEW new_mode;

    WINE_TRACE("Got message: window %p, message %#x, wparam %#lx, lparam %#lx.\n",
                    window, message, wparam, lparam);

    if (present->drop_wnd_messages && message != WM_DISPLAYCHANGE)
    {
        WINE_TRACE("Filtering message: window %p, message %#x, wparam %#lx, lparam %#lx.\n",
                window, message, wparam, lparam);
        if (unicode)
            return DefWindowProcW(window, message, wparam, lparam);
        else
            return DefWindowProcA(window, message, wparam, lparam);
    }

    if (message == WM_DESTROY)
    {
        WINE_TRACE("unregister window %p.\n", window);
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
                DRIPresent_ChangeDisplaySettingsIfNeccessary(present, &new_mode);

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
                DRIPresent_ChangeDisplaySettingsIfNeccessary(present, &new_mode);
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

static void setup_fullscreen_window(struct DRIPresent *This,
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

static void move_fullscreen_window(struct DRIPresent *This,
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

static void restore_fullscreen_window(struct DRIPresent *This,
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

static void DRIPresent_UpdatePresentationInterval(struct DRIPresent *This)
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

static HRESULT DRIPresent_ChangePresentParameters(struct DRIPresent *This,
        D3DPRESENT_PARAMETERS *params)
{
    HWND focus_window = This->focus_wnd ? This->focus_wnd : params->hDeviceWindow;
    RECT rect;
    DEVMODEW new_mode;
    HRESULT hr;
    boolean drop_wnd_messages;

    WINE_TRACE("This=%p, params=%p, focus_window=%p, params->hDeviceWindow=%p\n",
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
            WINE_TRACE("Setting fullscreen mode: %dx%d@%d\n", params->BackBufferWidth,
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
            hr = DRIPresent_ChangeDisplaySettingsIfNeccessary(This, &new_mode);
            if (FAILED(hr))
                return hr;

            /* Dirty as BackBufferWidth and BackBufferHeight hasn't been set yet */
            This->resolution_mismatch = FALSE;
        }
        else if(!This->params.Windowed && params->Windowed)
        {
            WINE_TRACE("Setting fullscreen mode: %dx%d@%d\n", This->initial_mode.dmPelsWidth,
                    This->initial_mode.dmPelsHeight, This->initial_mode.dmDisplayFrequency);

            hr = DRIPresent_ChangeDisplaySettingsIfNeccessary(This, &This->initial_mode);
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
                WINE_ERR("Window %p is not registered with nine.\n", focus_window);
        }
        This->params.Windowed = params->Windowed;
    }
    else if (!params->Windowed)
    {
        move_fullscreen_window(This, params->hDeviceWindow, params->BackBufferWidth, params->BackBufferHeight);
    }
    else
    {
        WINE_TRACE("Nothing changed.\n");
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

    DRIPresent_UpdatePresentationInterval(This);

    if (!params->Windowed) {
        struct d3d_drawable *d3d = get_d3d_drawable(This->gdi_display, focus_window);
        Atom _NET_WM_BYPASS_COMPOSITOR = XInternAtom(This->gdi_display,
                                                     "_NET_WM_BYPASS_COMPOSITOR",
                                                     False);

        Atom _VARIABLE_REFRESH = XInternAtom(This->gdi_display,
                                             "_VARIABLE_REFRESH",
                                             False);
        if (!d3d)
            return D3D_OK;

        /* Disable compositing for fullscreen windows */
        int bypass_value = 1;
        XChangeProperty(This->gdi_display, d3d->drawable,
                        _NET_WM_BYPASS_COMPOSITOR, XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)&bypass_value, 1);

        /* Enable variable sync */
        int vrr_value = 1;
        XChangeProperty(This->gdi_display, d3d->drawable,
                        _VARIABLE_REFRESH, XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)&vrr_value, 1);

        release_d3d_drawable(d3d);
    }

    return D3D_OK;
}

/* The following code isn't based on WINE's wined3d or d3d9. */

static HRESULT DRIPresent_new(Display *gdi_display, const WCHAR *devname,
        D3DPRESENT_PARAMETERS *params, HWND focus_wnd, struct DRIPresent **out,
        boolean ex, boolean no_window_changes, struct dri_backend *dri_backend)
{
    struct DRIPresent *This;
    HWND focus_window;
    DEVMODEW new_mode;
    HRESULT hr;
    RECT rect;

    if (!focus_wnd && !params->hDeviceWindow)
    {
        WINE_ERR("No focus HWND specified for presentation backend.\n");
        return D3DERR_INVALIDCALL;
    }

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                     sizeof(struct DRIPresent));
    if (!This)
    {
        WINE_ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->gdi_display = gdi_display;
    This->vtable = &DRIPresent_vtable;
    This->refs = 1;
    This->focus_wnd = focus_wnd;
    This->ex = ex;
    This->no_window_changes = no_window_changes;
    This->dri_backend = dri_backend;

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

        hr = DRIPresent_ChangeDisplaySettingsIfNeccessary(This, &new_mode);
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

    DRIPresent_UpdatePresentationInterval(This);

    lstrcpyW(This->devname, devname);

    if (!PRESENTInit(gdi_display, &(This->present_priv)))
    {
        WINE_ERR("Failed to init Present backend\n");
        return D3DERR_DRIVERINTERNALERROR;
    }

    if (!dri_backend->funcs->init(dri_backend->priv))
    {
        HeapFree(GetProcessHeap(), 0, This);
        return D3DERR_DRIVERINTERNALERROR;
    }

    *out = This;

    return D3D_OK;
}

struct DRIPresentGroup
{
    /* COM vtable */
    void *vtable;
    /* IUnknown reference count */
    LONG refs;

    boolean ex;
    struct DRIPresent **present_backends;
    unsigned npresent_backends;
    Display *gdi_display;
    boolean no_window_changes;
    struct dri_backend *dri_backend;
};

static ULONG WINAPI DRIPresentGroup_AddRef(struct DRIPresentGroup *This)
{
    ULONG refs = InterlockedIncrement(&This->refs);
    WINE_TRACE("%p increasing refcount to %u.\n", This, refs);
    return refs;
}

static ULONG WINAPI DRIPresentGroup_Release(struct DRIPresentGroup *This)
{
    ULONG refs = InterlockedDecrement(&This->refs);
    WINE_TRACE("%p decreasing refcount to %u.\n", This, refs);
    if (refs == 0)
    {
        unsigned i;
        if (This->present_backends)
        {
            for (i = 0; i < This->npresent_backends; ++i)
            {
                if (This->present_backends[i])
                    DRIPresent_Release(This->present_backends[i]);
            }
            HeapFree(GetProcessHeap(), 0, This->present_backends);
        }
        HeapFree(GetProcessHeap(), 0, This);
    }
    return refs;
}

static HRESULT WINAPI DRIPresentGroup_QueryInterface(struct DRIPresentGroup *This,
        REFIID riid, void **ppvObject )
{
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualGUID(&IID_ID3DPresentGroup, riid) ||
            IsEqualGUID(&IID_IUnknown, riid))
    {
        *ppvObject = This;
        DRIPresentGroup_AddRef(This);
        return S_OK;
    }

    WINE_WARN("%s not implemented, returning E_NOINTERFACE.\n", wine_dbgstr_guid(riid));
    *ppvObject = NULL;

    return E_NOINTERFACE;
}

static UINT WINAPI DRIPresentGroup_GetMultiheadCount(struct DRIPresentGroup *This)
{
    WINE_FIXME("(%p), stub!\n", This);
    return 1;
}

static HRESULT WINAPI DRIPresentGroup_GetPresent(struct DRIPresentGroup *This,
        UINT Index, ID3DPresent **ppPresent)
{
    if (Index >= DRIPresentGroup_GetMultiheadCount(This))
    {
        WINE_ERR("Index >= MultiHeadCount\n");
        return D3DERR_INVALIDCALL;
    }
    DRIPresent_AddRef(This->present_backends[Index]);
    *ppPresent = (ID3DPresent *)This->present_backends[Index];

    return D3D_OK;
}

static HRESULT WINAPI DRIPresentGroup_CreateAdditionalPresent(struct DRIPresentGroup *This,
        D3DPRESENT_PARAMETERS *pPresentationParameters, ID3DPresent **ppPresent)
{
    HRESULT hr;
    hr = DRIPresent_new(This->gdi_display, This->present_backends[0]->devname,
            pPresentationParameters, 0, (struct DRIPresent **)ppPresent,
            This->ex, This->no_window_changes, This->dri_backend);

    return hr;
}

static void WINAPI DRIPresentGroup_GetVersion(struct DRIPresentGroup *This,
        int *major, int *minor)
{
    *major = WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MAJOR;
    *minor = WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR;
}

static ID3DPresentGroupVtbl DRIPresentGroup_vtable = {
    (void *)DRIPresentGroup_QueryInterface,
    (void *)DRIPresentGroup_AddRef,
    (void *)DRIPresentGroup_Release,
    (void *)DRIPresentGroup_GetMultiheadCount,
    (void *)DRIPresentGroup_GetPresent,
    (void *)DRIPresentGroup_CreateAdditionalPresent,
    (void *)DRIPresentGroup_GetVersion
};

HRESULT present_create_present_group(Display *gdi_display, const WCHAR *device_name,
        UINT adapter, HWND focus_wnd, D3DPRESENT_PARAMETERS *params,
        unsigned nparams, ID3DPresentGroup **group, boolean ex, DWORD BehaviorFlags,
        struct dri_backend *dri_backend)
{
    struct DRIPresentGroup *This;
    DISPLAY_DEVICEW dd;
    HRESULT hr;
    unsigned i;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct DRIPresentGroup));
    if (!This)
    {
        WINE_ERR("Out of memory.\n");
        return E_OUTOFMEMORY;
    }

    This->gdi_display = gdi_display;
    This->vtable = &DRIPresentGroup_vtable;
    This->refs = 1;
    This->ex = ex;
    This->dri_backend = dri_backend;
    This->npresent_backends = nparams;
    This->no_window_changes = !!(BehaviorFlags & D3DCREATE_NOWINDOWCHANGES);
    This->present_backends = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            This->npresent_backends * sizeof(struct DRIPresent *));
    if (!This->present_backends)
    {
        DRIPresentGroup_Release(This);
        WINE_ERR("Out of memory.\n");
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
            WINE_WARN("Couldn't find subdevice %d from `%s'\n",
                    i, wine_dbgstr_w(device_name));
        }

        /* create an ID3DPresent for it */
        hr = DRIPresent_new(gdi_display, dd.DeviceName, &params[i],
                focus_wnd, &This->present_backends[i], ex, This->no_window_changes,
                This->dri_backend);
        if (FAILED(hr))
        {
            DRIPresentGroup_Release(This);
            return hr;
        }
    }

    *group = (ID3DPresentGroup *)This;
    WINE_TRACE("Returning %p\n", *group);

    return D3D_OK;
}

HRESULT present_create_adapter9(Display *gdi_display, HDC hdc,
        struct dri_backend *dri_backend, ID3DAdapter9 **out)
{
    HRESULT hr;
    int fd;

    if (!d3d9_drm)
    {
        WINE_ERR("DRM drivers are not supported on your system.\n");
        return D3DERR_DRIVERINTERNALERROR;
    }

    if (!get_wine_drawable_from_dc(hdc, NULL))
        return D3DERR_DRIVERINTERNALERROR;

    fd = dri_backend->funcs->get_fd(dri_backend->priv);
    if (fd < 0) {
        WINE_ERR("Got invalid fd from backend (fd=%d)\n", fd);
        return D3DERR_DRIVERINTERNALERROR;
    }

    hr = d3d9_drm->create_adapter(fd, out);
    if (FAILED(hr))
    {
        WINE_ERR("Unable to create ID3DAdapter9 (fd=%d)\n", fd);
        return hr;
    }

    WINE_TRACE("Created ID3DAdapter9 with fd %d\n", fd);

    return D3D_OK;
}

BOOL present_has_d3dadapter(Display *gdi_display)
{
    static const void * WINAPI (*pD3DAdapter9GetProc)(const char *);
    static void *handle = NULL;
    static int done = 0;
    char *pathbuf = NULL;

    /* like in opengl.c (single threaded assumption OK?) */
    if (done)
        return handle != NULL;
    done = 1;

    handle = common_load_d3dadapter(&pathbuf, NULL);

    if (!handle)
        goto cleanup;

    /* find our entry point in d3dadapter9 */
    pD3DAdapter9GetProc = dlsym(handle, "D3DAdapter9GetProc");
    if (!pD3DAdapter9GetProc)
    {
        WINE_ERR("Failed to get the entry point from %s: %s\n", pathbuf, dlerror());
        goto cleanup;
    }

    /* get a handle to the drm backend struct */
    d3d9_drm = pD3DAdapter9GetProc("drm");
    if (!d3d9_drm)
    {
        WINE_ERR("%s doesn't support the drm backend.\n", pathbuf);
        goto cleanup;
    }

    /* verify that we're binary compatible */
    if (d3d9_drm->major_version != 0)
    {
        WINE_ERR("Version mismatch. %s has %d.%d, was expecting 0.x\n",
                pathbuf, d3d9_drm->major_version, d3d9_drm->minor_version);
        goto cleanup;
    }

    /* this will be used to store d3d_drawables */
    d3d_hwnd_context = XUniqueContext();

    if (!PRESENTCheckExtension(gdi_display, 1, 0))
    {
        WINE_ERR("Unable to query PRESENT.\n");
        goto cleanup;
    }

    if (!backend_probe(gdi_display))
    {
        WINE_ERR("No available backends.\n");
        goto cleanup;
    }

    return TRUE;

cleanup:
    wine_dbg_printf("\033[1;31mNative Direct3D 9 will be unavailable."
            "\nFor more information visit " NINE_URL "\033[0m\n");

    if (handle)
    {
        dlclose(handle);
        handle = NULL;
    }

    free(pathbuf);

    return FALSE;
}

BOOL enable_device_vtable_wrapper(void)
{
    if (!d3d9_drm)
    {
        WINE_ERR("enable_device_vtable_wrapper call before init.\n");
        return FALSE;
    }
    /* Since minor version 1, we can assume a copy of the internal vtable is stored in second pos.
     * For now always enable if possible the wrapper (enables Steam overlay for example),
     * we might in the future let user choose. */
    return d3d9_drm->minor_version >= 1;
}

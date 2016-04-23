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

#include <d3dadapter/d3dadapter9.h>
#include <d3dadapter/drm.h>
#include <X11/Xutil.h>

#include "dri3.h"

#include "wine/library.h" // for wine_dl*
#include "wine/unicode.h" // for strcpyW

#ifndef D3DPRESENT_DONOTWAIT
#define D3DPRESENT_DONOTWAIT      0x00000001
#endif

#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MAJOR 1
#ifdef ID3DPresent_GetWindowOccluded
#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR 1
#else
#define WINE_D3DADAPTER_DRIVER_PRESENT_VERSION_MINOR 0
#endif

static const struct D3DAdapter9DRM *d3d9_drm = NULL;
#ifdef D3D9NINE_DRI2
static int is_dri2_fallback = 0;
#endif

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
    RECT                     dc_rect;      /* DC rectangle relative to drawable */
};

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
    RECT dc_rect; /* rect relative to the X11 drawable */
    HDC hdc;
    HWND wnd; /* HWND (for convenience) */
};

#ifdef ID3DPresent_GetWindowOccluded
static HHOOK hhook;

struct d3d_wnd_hooks
{
    HWND focus_wnd;
    struct DRI3Present *present;
    struct d3d_wnd_hooks *prev;
    struct d3d_wnd_hooks *next;
};

static HRESULT dri3_present_unregister_window_hook( struct DRI3Present *This );
static HRESULT dri3_present_register_window_hook( struct DRI3Present *This );

static struct d3d_wnd_hooks d3d_hooks;
#endif

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
    BOOL occluded;
    Display *gdi_display;
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
    d3d->dc_rect = extesc.dc_rect;

    return d3d;
}

static struct d3d_drawable *get_d3d_drawable(Display *gdi_display, HWND hwnd)
{
    struct d3d_drawable *d3d, *race;

    EnterCriticalSection(&context_section);
    if (!XFindContext(gdi_display, (XID)hwnd, d3d_hwnd_context, (char **)&d3d))
    {
        struct x11drv_escape_get_drawable extesc = { X11DRV_GET_DRAWABLE };

        /* check if the window has moved since last we used it */
        if (ExtEscape(d3d->hdc, X11DRV_ESCAPE, sizeof(extesc), (LPCSTR)&extesc,
                sizeof(extesc), (LPSTR)&extesc) <= 0)
        {
            WARN("Window update check failed (hwnd=%p, hdc=%p)\n",
                 hwnd, d3d->hdc);
        }

        if (!EqualRect(&d3d->dc_rect, &extesc.dc_rect))
            d3d->dc_rect = extesc.dc_rect;

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
#ifdef ID3DPresent_GetWindowOccluded
        dri3_present_unregister_window_hook(This);
#endif
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
        D3DPRESENT_PARAMETERS *params,
        BOOL first_time);

static HRESULT WINAPI DRI3Present_SetPresentParameters(struct DRI3Present *This,
        D3DPRESENT_PARAMETERS *pPresentationParameters,
        D3DDISPLAYMODEEX *pFullscreenDisplayMode)
{
    if (pFullscreenDisplayMode)
        FIXME("Ignoring pFullscreenDisplayMode\n");
#ifdef ID3DPresent_GetWindowOccluded
    dri3_present_register_window_hook(This);
#endif
    return DRI3Present_ChangePresentParameters(This, pPresentationParameters, FALSE);
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
        DRI2FallbackPRESENTPixmap(This->present_priv, This->dri2_priv,
                dmaBufFd, width, height, stride, depth,
                bpp, &((*out)->present_pixmap_priv));
        return D3D_OK;
    }
#endif
    if (!DRI3PixmapFromDmaBuf(This->gdi_display, DefaultScreen(This->gdi_display),
            dmaBufFd, width, height, stride, depth, bpp, &pixmap))
        return D3DERR_DRIVERINTERNALERROR;

    *out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct D3DWindowBuffer));
    PRESENTPixmapInit(This->present_priv, pixmap, &((*out)->present_pixmap_priv));
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
    PRESENTWaitPixmapReleased(buffer->present_pixmap_priv);
    return D3D_OK;
}

static HRESULT WINAPI DRI3Present_FrontBufferCopy(struct DRI3Present *This,
        struct D3DWindowBuffer *buffer)
{
#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback)
        return D3DERR_DRIVERINTERNALERROR;
#endif
    /* TODO: use dc_rect */
    if (PRESENTHelperCopyFront(This->gdi_display, buffer->present_pixmap_priv))
        return D3D_OK;
    else
        return D3DERR_DRIVERINTERNALERROR;
}

static HRESULT WINAPI DRI3Present_PresentBuffer( struct DRI3Present *This,
        struct D3DWindowBuffer *buffer, HWND hWndOverride, const RECT *pSourceRect,
        const RECT *pDestRect, const RGNDATA *pDirtyRegion, DWORD Flags )
{
    struct d3d_drawable *d3d;
    RECT dest_translate;

    if (hWndOverride)
        d3d = get_d3d_drawable(This->gdi_display, hWndOverride);
    else if (This->params.hDeviceWindow)
        d3d = get_d3d_drawable(This->gdi_display, This->params.hDeviceWindow);
    else
        d3d = get_d3d_drawable(This->gdi_display, This->focus_wnd);

    if (!d3d)
        return D3DERR_DRIVERINTERNALERROR;

    /* TODO: should we use a list here instead ? */
    if (This->d3d && (This->d3d->wnd != d3d->wnd))
        destroy_d3dadapter_drawable(This->gdi_display, This->d3d->wnd);

    This->d3d = d3d;

    if ((d3d->dc_rect.top != 0) && (d3d->dc_rect.left != 0))
    {
        if (!pDestRect)
            pDestRect = (const RECT *) &(d3d->dc_rect);
        else
        {
            dest_translate.top = pDestRect->top + d3d->dc_rect.top;
            dest_translate.left = pDestRect->left + d3d->dc_rect.left;
            dest_translate.bottom = pDestRect->bottom + d3d->dc_rect.bottom;
            dest_translate.right = pDestRect->right + d3d->dc_rect.right;
            pDestRect = (const RECT *) &dest_translate;
        }
    }

    if (!PRESENTPixmap(This->gdi_display, d3d->drawable, buffer->present_pixmap_priv,
            &This->params, pSourceRect, pDestRect, pDirtyRegion))
    {
        release_d3d_drawable(d3d);
        return D3DERR_DRIVERINTERNALERROR;
    }
    release_d3d_drawable(d3d);

    return D3D_OK;
}

static HRESULT WINAPI DRI3Present_GetRasterStatus( struct DRI3Present *This,
        D3DRASTER_STATUS *pRasterStatus )
{
    FIXME("(%p, %p), stub!\n", This, pRasterStatus);
    return D3DERR_INVALIDCALL;
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

#ifdef ID3DPresent_GetWindowOccluded
static struct d3d_wnd_hooks *get_last_hook(void)
{
    struct d3d_wnd_hooks *hook = &d3d_hooks;
    while (hook->next)
    {
        hook = hook->next;
    }
    return hook;
}

LRESULT CALLBACK HookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    struct d3d_wnd_hooks *hook = &d3d_hooks;
    if (nCode < 0)
        return CallNextHookEx(hhook, nCode, wParam, lParam);

    if (lParam)
    {
        CWPSTRUCT wndprocparams = *((CWPSTRUCT*)lParam);
        while (hook->next)
        {
            hook = hook->next;
            /* skip messages for other hwnds */
            if (hook->focus_wnd != wndprocparams.hwnd)
                continue;
            switch (wndprocparams.message)
            {
                case WM_ACTIVATE:
                    if(wndprocparams.wParam == WA_INACTIVE)
                    {
                        if (hook->present && !hook->present->params.Windowed)
                        {
                            ShowWindow(hook->present->params.hDeviceWindow, SW_MINIMIZE);
                            ChangeDisplaySettingsExW(hook->present->devname,
                                    &(hook->present->initial_mode), 0, 0, NULL);
                            hook->present->occluded = TRUE;
                        }
                    }
                    else
                    {
                        if (hook->present && !hook->present->params.Windowed &&
                                hook->present->occluded)
                        {
                            ShowWindow(hook->present->params.hDeviceWindow, SW_RESTORE);
                            hook->present->occluded = FALSE;
                        }
                    }
                break;
                /* TODO: handle other window messages here */
                default:
                break;
            }
        }
    }

    return CallNextHookEx(hhook, nCode, wParam, lParam);
}

static HRESULT dri3_present_register_window_hook(struct DRI3Present *This)
{
    struct d3d_wnd_hooks *lasthook;
    struct d3d_wnd_hooks *hook = &d3d_hooks;

    HWND hWnd = This->focus_wnd;
    /* let's see if already hooked */
    while (hook->next)
    {
        hook = hook->next;
        if (hook->focus_wnd == hWnd && hook->present == This)
            return D3D_OK;
    }
    /* create single WindowsHook in this process */
    if (!hhook)
    {
        // TODO: do we need to handle different threadIDs ?
        DWORD threadID = GetWindowThreadProcessId(hWnd, NULL);
        hhook = SetWindowsHookExW(WH_CALLWNDPROC, HookCallback, NULL, threadID);
        if (!hhook)
        {
            ERR("SetWindowsHookEx failed with 0x%08x\n", GetLastError());
            return D3DERR_DRIVERINTERNALERROR;
        }
    }
    lasthook = get_last_hook();
    hook = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(struct d3d_wnd_hooks));
    if (!hook)
        return E_OUTOFMEMORY;
    /* add window hwnd to list */
    lasthook->next = hook;
    hook->prev = lasthook;
    hook->focus_wnd = hWnd;
    hook->present = This;
    return D3D_OK;
}

static HRESULT dri3_present_unregister_window_hook(struct DRI3Present *This)
{
    struct d3d_wnd_hooks *hook = &d3d_hooks;

    HWND hWnd = This->focus_wnd;
    /* find hook and remove it */
    while (hook->next)
    {
        hook = hook->next;
        if(hook->focus_wnd == hWnd && hook->present == This)
        {
            /* remove hook */
            hook->prev->next = hook->next;
            HeapFree(GetProcessHeap(), 0, hook);
            /* start again at list head */
            hook = &d3d_hooks;
        }
    }
    /* remove single process WindowsHook */
    if (get_last_hook() == &d3d_hooks && hhook)
    {
       if (!UnhookWindowsHookEx(hhook))
           ERR("UnhookWindowsHookEx failed with 0x%08x\n", GetLastError());
       hhook = NULL;
    }
    return D3D_OK;
}

static BOOL WINAPI DRI3Present_GetWindowOccluded(struct DRI3Present *This)
{
    return This->occluded;
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
#ifdef ID3DPresent_GetWindowOccluded
    (void *)DRI3Present_GetWindowOccluded
#endif
};

static HRESULT DRI3Present_ChangePresentParameters(struct DRI3Present *This,
         D3DPRESENT_PARAMETERS *params, BOOL first_time)
{
    HWND draw_window;
    RECT rect;
    LONG hr;

    (void) first_time; /* will be used to manage screen res if windowed mode change */
    /* TODO: don't do anything if nothing changed */
    /* sanitize presentation parameters */
    draw_window = params->hDeviceWindow ? params->hDeviceWindow : This->focus_wnd;

    if (!GetClientRect(draw_window, &rect))
    {
        WARN("GetClientRect failed.\n");
        rect.right = 640;
        rect.bottom = 480;
    }

    if (params->BackBufferWidth == 0)
        params->BackBufferWidth = rect.right - rect.left;

    if (params->BackBufferHeight == 0)
        params->BackBufferHeight = rect.bottom - rect.top;

    if (!params->Windowed)
    {
        /* TODO Store initial config and restore it when leaving fullscreen, or when leaving wine*/
        LONG style, exstyle;
        DEVMODEW newMode;

        ZeroMemory(&newMode, sizeof(DEVMODEW));
        newMode.dmPelsWidth = params->BackBufferWidth;
        newMode.dmPelsHeight = params->BackBufferHeight;
        newMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        newMode.dmSize = sizeof(DEVMODEW);
        hr = ChangeDisplaySettingsExW(This->devname, &newMode, 0, CDS_FULLSCREEN, NULL);
        if (hr != DISP_CHANGE_SUCCESSFUL)
        {
            ERR("ChangeDisplaySettingsExW failed with 0x%08X\n", hr);
            return D3DERR_INVALIDCALL;
        }
        style = fullscreen_style(0);
        exstyle = fullscreen_exstyle(0);

        SetWindowLongW(draw_window, GWL_STYLE, style);
        SetWindowLongW(draw_window, GWL_EXSTYLE, exstyle);
        hr = SetWindowPos(draw_window, HWND_TOPMOST, 0, 0, params->BackBufferWidth, params->BackBufferHeight,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        if (!hr)
        {
            ERR("SetWindowLongW failed with 0x%08X\n", GetLastError());
            return D3DERR_INVALIDCALL;
        }
    }
    else if (!first_time && !This->params.Windowed)
    {
        hr = ChangeDisplaySettingsExW(This->devname, &(This->initial_mode), 0, CDS_FULLSCREEN, NULL);
        if (hr != DISP_CHANGE_SUCCESSFUL)
        {
            ERR("ChangeDisplaySettingsExW failed with 0x%08X\n", hr);
            return D3DERR_INVALIDCALL;
        }
    }
    SetActiveWindow(draw_window);

    This->params = *params;
    return D3D_OK;
}

static HRESULT DRI3Present_new(Display *gdi_display, const WCHAR *devname,
        D3DPRESENT_PARAMETERS *params, HWND focus_wnd, struct DRI3Present **out)
{
    struct DRI3Present *This;
    HRESULT hr;

    if (!focus_wnd)
        focus_wnd = params->hDeviceWindow;
    if (!focus_wnd)
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

    strcpyW(This->devname, devname);

    ZeroMemory(&(This->initial_mode), sizeof(This->initial_mode));
    This->initial_mode.dmSize = sizeof(This->initial_mode);

    EnumDisplaySettingsExW(This->devname, ENUM_CURRENT_SETTINGS, &(This->initial_mode), 0);

    hr = DRI3Present_ChangePresentParameters(This, params, TRUE);
    if (hr != D3D_OK)
        return hr;

    PRESENTInit(gdi_display, &(This->present_priv));
#ifdef D3D9NINE_DRI2
    if (is_dri2_fallback)
        DRI2FallbackInit(gdi_display, &(This->dri2_priv));
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

    struct DRI3Present **present_backends;
    unsigned npresent_backends;
    Display *gdi_display;
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
            pPresentationParameters, 0, (struct DRI3Present **)ppPresent);
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
        unsigned nparams, ID3DPresentGroup **group)
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
    This->npresent_backends = nparams;
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
                focus_wnd, &This->present_backends[i]);
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
        handle = wine_dlopen(path,
                RTLD_GLOBAL | RTLD_NOW, errbuf, sizeof(errbuf));
        if (!handle)
        {
            ERR("Failed to load %s: %s\n", path, errbuf);
            goto cleanup;
        }
        memcpy(pathbuf, path, size >= sizeof(pathbuf) ? (sizeof(pathbuf)-1) : size);
        pathbuf[sizeof(pathbuf)-1] = 0;
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
            ERR("Failed to load %s: %s\n", D3D9NINE_MODULEPATH, errbuf);
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
    HeapFree(GetProcessHeap(), 0, path);

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


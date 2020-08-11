/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine present interface
 *
 * Copyright 2015 Patrick Rudolph
 */

#ifndef __NINE_PRESENT_H
#define __NINE_PRESENT_H

#include <d3dadapter/present.h>
#include <X11/Xlib.h>

struct dri_backend;

HRESULT present_create_present_group(Display *gdi_display, const WCHAR *device_name,
        HWND focus, D3DPRESENT_PARAMETERS *params, unsigned nparams, ID3DPresentGroup **group,
        boolean ex, DWORD BehaviorFlags, struct dri_backend *dri_backend);

HRESULT present_create_adapter9(Display *gdi_display, HDC hdc,
        struct dri_backend *dri_backend, ID3DAdapter9 **adapter);

BOOL present_has_d3dadapter(Display *gdi_display);

BOOL enable_device_vtable_wrapper(void);

#endif /* __NINE_PRESENT_H */

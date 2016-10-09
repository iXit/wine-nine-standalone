/*
 * Wine present interface
 *
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

#ifndef __WINE_PRESENT_H
#define __WINE_PRESENT_H

#ifndef __WINE_CONFIG_H
# error You must include config.h to use this header
#endif

#include <X11/Xlib.h>

HRESULT present_create_present_group(Display *gdi_display, const WCHAR *device_name, UINT adapter,
        HWND focus, D3DPRESENT_PARAMETERS *params, unsigned nparams, ID3DPresentGroup **group,
        boolean ex, DWORD BehaviorFlags);

HRESULT present_create_adapter9(Display *gdi_display, HDC hdc, ID3DAdapter9 **adapter);

BOOL present_has_d3dadapter(Display *gdi_display);

BOOL enable_device_vtable_wrapper(void);

#endif /* __WINE_PRESENT_H */

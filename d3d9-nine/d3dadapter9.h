/*
 * D3DAdapter9 interface
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

#ifndef __WINE_D3D9ADAPTER_H
#define __WINE_D3D9ADAPTER_H

#include <X11/Xlib.h>

void d3dadapter9_init(HINSTANCE hinst);

void d3dadapter9_destroy(HINSTANCE hinst);

HRESULT d3dadapter9_new(Display *gdi_display, boolean ex, IDirect3D9Ex **ppOut);

#endif /* __WINE_D3D9ADAPTER_H */

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Direct3D wine internal interface main
 *
 * Copyright 2002-2003 The wine-d3d team
 * Copyright 2002-2003 Raphael Junqueira
 * Copyright 2004      Jason Edmeades
 * Copyright 2007-2008 Stefan Dösinger for CodeWeavers
 * Copyright 2009 Henri Verbeet for CodeWeavers
 */

#ifndef __NINE_WNDPROC_H
#define __NINE_WNDPROC_H

#include <windef.h>

struct DRIPresent;

BOOL nine_register_window(HWND window, struct DRIPresent *present);
BOOL nine_unregister_window(HWND window);

BOOL nine_dll_init(HINSTANCE hInstDLL);
BOOL nine_dll_destroy(HINSTANCE hInstDLL);

LRESULT device_process_message(struct DRIPresent *present, HWND window, BOOL unicode,
        UINT message, WPARAM wparam, LPARAM lparam, WNDPROC proc);

#endif /* __NINE_WNDPROC_H */

/*
 * Copyright 2016 Patrick Rudolph
 *
 * Based on the file wined3d_main.c taken from wined3d:
 * All credits go to the original developers:
 *
 * Copyright 2002-2003 The wine-d3d team
 * Copyright 2002-2003 Raphael Junqueira
 * Copyright 2004      Jason Edmeades
 * Copyright 2007-2008 Stefan Dösinger for CodeWeavers
 * Copyright 2009 Henri Verbeet for CodeWeavers
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

#include <stdarg.h>
#include <math.h>
#include <limits.h>
#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/debug.h"
#include "wine/unicode.h"

#include "wndproc.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dadapter);

struct nine_wndproc
{
    HWND window;
    BOOL unicode;
    WNDPROC proc;
    struct DRI3Present *present;
};

struct nine_wndproc_table
{
    struct nine_wndproc *entries;
    unsigned int count;
    unsigned int size;
};

static struct nine_wndproc_table wndproc_table;

static CRITICAL_SECTION nine_wndproc_cs;
static CRITICAL_SECTION_DEBUG nine_wndproc_cs_debug =
{
    0, 0, &nine_wndproc_cs,
    {&nine_wndproc_cs_debug.ProcessLocksList,
    &nine_wndproc_cs_debug.ProcessLocksList},
    0, 0, {(DWORD_PTR)(__FILE__ ": nine_wndproc_cs")}
};
static CRITICAL_SECTION nine_wndproc_cs = {&nine_wndproc_cs_debug, -1, 0, 0, 0, 0};

BOOL nine_dll_init(HINSTANCE hInstDLL)
{
    WNDCLASSA wc;

    /* We need our own window class for a fake window which we use to retrieve GL capabilities */
    /* We might need CS_OWNDC in the future if we notice strange things on Windows.
     * Various articles/posts about OpenGL problems on Windows recommend this. */
    wc.style                = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc          = DefWindowProcA;
    wc.cbClsExtra           = 0;
    wc.cbWndExtra           = 0;
    wc.hInstance            = hInstDLL;
    wc.hIcon                = LoadIconA(NULL, (const char *)IDI_WINLOGO);
    wc.hCursor              = LoadCursorA(NULL, (const char *)IDC_ARROW);
    wc.hbrBackground        = NULL;
    wc.lpszMenuName         = NULL;
    wc.lpszClassName        = NINE_WINDOW_CLASS_NAME;

    if (!RegisterClassA(&wc))
    {
        ERR("Failed to register window class '%s'!\n", NINE_WINDOW_CLASS_NAME);
        return FALSE;
    }

    DisableThreadLibraryCalls(hInstDLL);

    return TRUE;
}

BOOL nine_dll_destroy(HINSTANCE hInstDLL)
{
    unsigned int i;

    for (i = 0; i < wndproc_table.count; ++i)
    {
        /* Trying to unregister these would be futile. These entries can only
         * exist if either we skipped them in nine_unregister_window() due
         * to the application replacing the wndproc after the entry was
         * registered, or if the application still has an active nine
         * device. In the latter case the application has bigger problems than
         * these entries. */
        WARN("Leftover wndproc table entry %p.\n", &wndproc_table.entries[i]);
    }
    HeapFree(GetProcessHeap(), 0, wndproc_table.entries);

    UnregisterClassA(NINE_WINDOW_CLASS_NAME, hInstDLL);

    DeleteCriticalSection(&nine_wndproc_cs);
    return TRUE;
}

static void nine_wndproc_mutex_lock(void)
{
    EnterCriticalSection(&nine_wndproc_cs);
}

static void nine_wndproc_mutex_unlock(void)
{
    LeaveCriticalSection(&nine_wndproc_cs);
}

static struct nine_wndproc *nine_find_wndproc(HWND window)
{
    unsigned int i;

    for (i = 0; i < wndproc_table.count; ++i)
    {
        if (wndproc_table.entries[i].window == window)
        {
            return &wndproc_table.entries[i];
        }
    }

    return NULL;
}

static LRESULT CALLBACK nine_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    struct nine_wndproc *entry;
    struct DRI3Present *present;
    BOOL unicode;
    WNDPROC proc;

    nine_wndproc_mutex_lock();
    entry = nine_find_wndproc(window);

    if (!entry)
    {
        nine_wndproc_mutex_unlock();
        ERR("Window %p is not registered with nine.\n", window);
        return DefWindowProcW(window, message, wparam, lparam);
    }

    present = entry->present;
    unicode = entry->unicode;
    proc = entry->proc;
    nine_wndproc_mutex_unlock();

    if (present)
        return device_process_message(present, window, unicode, message, wparam, lparam, proc);
    if (unicode)
        return CallWindowProcW(proc, window, message, wparam, lparam);
    return CallWindowProcA(proc, window, message, wparam, lparam);
}

BOOL nine_register_window(HWND window, struct DRI3Present *present)
{
    struct nine_wndproc *entry;

    nine_wndproc_mutex_lock();

    if (nine_find_wndproc(window))
    {
        nine_wndproc_mutex_unlock();
        WARN("Window %p is already registered with nine.\n", window);
        return TRUE;
    }

    if (wndproc_table.size == wndproc_table.count)
    {
        unsigned int new_size = max(1, wndproc_table.size * 2);
        struct nine_wndproc *new_entries;

        if (!wndproc_table.entries) new_entries = HeapAlloc(GetProcessHeap(), 0, new_size * sizeof(*new_entries));
        else new_entries = HeapReAlloc(GetProcessHeap(), 0, wndproc_table.entries, new_size * sizeof(*new_entries));

        if (!new_entries)
        {
            nine_wndproc_mutex_unlock();
            ERR("Failed to grow table.\n");
            return FALSE;
        }

        wndproc_table.entries = new_entries;
        wndproc_table.size = new_size;
    }

    entry = &wndproc_table.entries[wndproc_table.count++];
    entry->window = window;
    entry->unicode = IsWindowUnicode(window);
    /* Set a window proc that matches the window. Some applications (e.g. NoX)
     * replace the window proc after we've set ours, and expect to be able to
     * call the previous one (ours) directly, without using CallWindowProc(). */
    if (entry->unicode)
        entry->proc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)nine_wndproc);
    else
        entry->proc = (WNDPROC)SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)nine_wndproc);
    entry->present = present;

    nine_wndproc_mutex_unlock();

    return TRUE;
}

BOOL nine_unregister_window(HWND window)
{
    struct nine_wndproc *entry, *last;
    LONG_PTR proc;

    nine_wndproc_mutex_lock();

    if (!(entry = nine_find_wndproc(window)))
    {
        nine_wndproc_mutex_unlock();
        return FALSE;
    }

    if (entry->unicode)
    {
        proc = GetWindowLongPtrW(window, GWLP_WNDPROC);
        if (proc != (LONG_PTR)nine_wndproc)
        {
            entry->present = NULL;
            nine_wndproc_mutex_unlock();
            WARN("Not unregistering window %p, window proc %#lx doesn't match nine window proc %p.\n",
                    window, proc, nine_wndproc);
            return FALSE;
        }

        SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)entry->proc);
    }
    else
    {
        proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
        if (proc != (LONG_PTR)nine_wndproc)
        {
            entry->present = NULL;
            nine_wndproc_mutex_unlock();
            WARN("Not unregistering window %p, window proc %#lx doesn't match nine window proc %p.\n",
                    window, proc, nine_wndproc);
            return FALSE;
        }

        SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)entry->proc);
    }

    last = &wndproc_table.entries[--wndproc_table.count];
    if (entry != last) *entry = *last;

    nine_wndproc_mutex_unlock();
    return TRUE;
}

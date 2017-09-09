/*
 * NineWineCfg main entry point
 *
 * Copyright 2002 Jaco Greeff
 * Copyright 2003 Dimitrie O. Paun
 * Copyright 2003 Mike Hearn
 * Copyright 2017 Patrick Rudolph
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
 *
 */

#define WIN32_LEAN_AND_MEAN
#define NONAMELESSUNION

#include "config.h"
#include <wine/port.h>
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <wine/debug.h>
#include <wine/library.h>

#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(ninecfg);

/*
 * Winecfg
 */
/* this is called from the WM_SHOWWINDOW handlers of each tab page.
 *
 * it's a nasty hack, necessary because the property sheet insists on resetting the window title
 * to the title of the tab, which is utterly useless. dropping the property sheet is on the todo list.
 */
void set_window_title(HWND dialog)
{
    WCHAR newtitle[256];

    LoadStringW (GetModuleHandleW(NULL), IDS_NINECFG_TITLE, newtitle,
         sizeof(newtitle)/sizeof(newtitle[0]));

    WINE_TRACE("setting title to %s\n", wine_dbgstr_w (newtitle));
    SendMessageW (GetParent(dialog), PSM_SETTITLEW, 0, (LPARAM) newtitle);
}


WCHAR* load_string (UINT id)
{
    WCHAR buf[1024];
    int len;
    WCHAR* newStr;

    LoadStringW (GetModuleHandleW(NULL), id, buf, sizeof(buf)/sizeof(buf[0]));

    len = lstrlenW (buf);
    newStr = HeapAlloc (GetProcessHeap(), 0, (len + 1) * sizeof (WCHAR));
    memcpy (newStr, buf, len * sizeof (WCHAR));
    newStr[len] = 0;
    return newStr;
}

/*
 * Gallium nine
 */
static BOOL nine_get(void)
{
    BOOL ret = 0;
    HKEY regkey;

    if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\DllRedirects", &regkey))
    {
        DWORD type;
        DWORD size = 0;
        LSTATUS rc;
        rc = RegQueryValueExA(regkey, "d3d9", 0, &type, NULL, &size);
        if (rc != ERROR_FILE_NOT_FOUND && type == REG_SZ)
        {
            char *val = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 1);
            if (!val)
            {
                RegCloseKey(regkey);
                return 0;
            }
            rc = RegQueryValueExA(regkey, "d3d9", 0, &type, (LPBYTE)val, &size);
            if (rc == ERROR_SUCCESS)
            {
                ret = !!val && !strcmp(val, "d3d9-nine.dll");
            }
            else
            {
                WINE_ERR("Failed to read value 'd3d9'. rc = %d\n", rc);
            }
            HeapFree(GetProcessHeap(), 0, val);
        }
        else
        {
            WINE_ERR("Failed to read value 'd3d9'. rc = %d\n", rc);
        }
        RegCloseKey(regkey);
    }
    else
    {
        WINE_ERR("Failed to open path 'HKCU\\Software\\Wine\\DllRedirects'\n");
    }

    return ret;
}

static void nine_set(BOOL status)
{
    HKEY regkey;

    if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\DllRedirects", &regkey))
    {
        LSTATUS rc;

        if (!status)
        {
            rc = RegDeleteValueA(regkey, "d3d9");
        }
        else
        {
            rc = RegSetValueExA(regkey, "d3d9", 0, REG_SZ, (LPBYTE)"d3d9-nine.dll", strlen("d3d9-nine.dll"));
        }
        if (rc != NO_ERROR)
        {
            WINE_ERR("Failed to write 'HKCU\\Software\\Wine\\DllRedirects\\d3d9'. rc = %d\n", rc);
        }
        RegCloseKey(regkey);
    }
    else
    {
        WINE_ERR("Failed to open path 'HKCU\\Software\\Wine\\DllRedirects'\n");
    }
}

typedef HRESULT (WINAPI *LPDIRECT3DCREATE9EX)( UINT, void **);

static const WCHAR emptyW[1];

static void load_staging_settings(HWND dialog)
{
    HMODULE hmod = NULL;
    char have_d3d9nine = 0;
    char have_modpath = 0;
    char *mod_path = NULL;
    LPDIRECT3DCREATE9EX Direct3DCreate9ExPtr = NULL;
    HRESULT ret = -1;
    void *iface = NULL;
    HKEY regkey;
    void *handle;
    char errbuf[1024];

#if defined(HAVE_D3D9NINE)
    have_d3d9nine = 1;
#endif
#if defined(D3D9NINE_MODULEPATH)
    have_modpath = 1;
    mod_path = (char*)D3D9NINE_MODULEPATH;
#endif

    CheckDlgButton(dialog, IDC_ENABLE_NATIVE_D3D9, nine_get() ? BST_CHECKED : BST_UNCHECKED);

    SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP1, WM_SETTEXT, 1, (LPARAM)emptyW);
    SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP2, WM_SETTEXT, 1, (LPARAM)emptyW);
    SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP3, WM_SETTEXT, 1, (LPARAM)emptyW);
    SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP4, WM_SETTEXT, 1, (LPARAM)emptyW);
    SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP5, WM_SETTEXT, 1, (LPARAM)emptyW);

    CheckDlgButton(dialog, IDC_NINE_STATE1, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE2, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE3, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE4, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE5, BST_UNCHECKED);

    if (have_d3d9nine)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE1, BST_CHECKED);
    }
    else
    {
        SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP1, WM_SETTEXT, 1,
                (LPARAM)load_string (IDS_NINECFG_NINE_SUPPORT_NOT_COMPILED));
        goto out;
    }

    if (!have_modpath && !RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\Direct3DNine", &regkey))
    {
        DWORD type;
        DWORD size = 0;
        LSTATUS rc;

        rc = RegQueryValueExA(regkey, "ModulePath", 0, &type, NULL, &size);
        if (rc != ERROR_FILE_NOT_FOUND && type == REG_SZ)
        {
            mod_path = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 1);
            rc = RegQueryValueExA(regkey, "ModulePath", 0, &type, (LPBYTE)mod_path, &size);
            if (rc == ERROR_SUCCESS)
            {
                have_modpath = 1;
            }
        }
        RegCloseKey(regkey);
    }

    if (have_modpath)
    {
        SendDlgItemMessageA(dialog, IDC_NINE_STATE_TIP2, WM_SETTEXT, 1, (LPARAM)mod_path);
        CheckDlgButton(dialog, IDC_NINE_STATE2, BST_CHECKED);
    }
    else
    {
        goto out;
    }

    handle = wine_dlopen(mod_path, RTLD_GLOBAL | RTLD_NOW, errbuf, sizeof(errbuf));
    if (handle)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE3, BST_CHECKED);
    }
    else
    {
        SendDlgItemMessageA(dialog, IDC_NINE_STATE_TIP3, WM_SETTEXT, 1, (LPARAM)errbuf);
        goto out;
    }

    hmod = LoadLibraryA("d3d9-nine.dll");
    if (hmod)
        Direct3DCreate9ExPtr = (LPDIRECT3DCREATE9EX)
                GetProcAddress(hmod, "Direct3DCreate9Ex");

    if (hmod && Direct3DCreate9ExPtr)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE4, BST_CHECKED);
    }
    else
    {
        wchar_t buf[256];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)buf, 256, NULL);

        SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP4, WM_SETTEXT, 1, (LPARAM)buf);
        goto out;
    }

    ret = Direct3DCreate9ExPtr(0, &iface);
    if (!ret && iface)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE5, BST_CHECKED);
    }
    else
    {
        SendDlgItemMessageW(dialog, IDC_NINE_STATE_TIP5, WM_SETTEXT, 1,
                (LPARAM)load_string (IDS_NINECFG_D3D_ERROR));
        goto out;
    }

    return;
out:
    EnableWindow(GetDlgItem(dialog, IDC_ENABLE_NATIVE_D3D9), 0);

    if (hmod)
        FreeLibrary(hmod);
}



static INT_PTR CALLBACK AppDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        break;

    case WM_NOTIFY:
        if (((LPNMHDR)lParam)->code == PSN_SETACTIVE)
            load_staging_settings(hDlg);
        break;

    case WM_SHOWWINDOW:
        set_window_title(hDlg);
        break;

    case WM_DESTROY:
        break;

    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) break;
        switch (LOWORD(wParam))
        {
        case IDC_ENABLE_NATIVE_D3D9:
            nine_set(IsDlgButtonChecked(hDlg, IDC_ENABLE_NATIVE_D3D9) == BST_CHECKED);
            SendMessageW(GetParent(hDlg), PSM_CHANGED, 0, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT CALLBACK
PropSheetCallback (HWND hWnd, UINT uMsg, LPARAM lParam)
{
    return 0;
}

static INT_PTR
doPropertySheet (HINSTANCE hInstance, HWND hOwner)
{
    PROPSHEETPAGEW psp[2];
    PROPSHEETHEADERW psh;


    psp[0].dwSize = sizeof (PROPSHEETPAGEW);
    psp[0].dwFlags = PSP_USETITLE;
    psp[0].hInstance = hInstance;
    psp[0].u.pszTemplate = MAKEINTRESOURCEW (IDD_NINE);
    psp[0].u2.pszIcon = NULL;
    psp[0].pfnDlgProc = AppDlgProc;
    psp[0].pszTitle = load_string (IDS_TAB_MAIN);
    psp[0].lParam = 0;

    psp[1].dwSize = sizeof (PROPSHEETPAGEW);
    psp[1].dwFlags = PSP_USETITLE;
    psp[1].hInstance = hInstance;
    psp[1].u.pszTemplate = MAKEINTRESOURCEW (IDD_ABOUT);
    psp[1].u2.pszIcon = NULL;
    psp[1].pfnDlgProc = NULL;
    psp[1].pszTitle = load_string (IDS_TAB_ABOUT);
    psp[1].lParam = 0;

    /*
     * Fill out the PROPSHEETHEADER
     */
    psh.dwSize = sizeof (PROPSHEETHEADERW);
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_USEICONID | PSH_USECALLBACK;
    psh.hwndParent = hOwner;
    psh.hInstance = hInstance;
    psh.u.pszIcon = NULL;
    psh.pszCaption =  load_string (IDS_NINECFG_TITLE);
    psh.nPages = sizeof(psp) / sizeof(psp[0]);
    psh.u3.ppsp = &psp[0];
    psh.pfnCallback = PropSheetCallback;
    psh.u2.nStartPage = 0;

    /*
     * Display the modal property sheet
     */
    return PropertySheetW (&psh);
}

/*****************************************************************************
 * Name       : WinMain
 * Description: Main windows entry point
 * Parameters : hInstance
 *              hPrev
 *              szCmdLine
 *              nShow
 * Returns    : Program exit code
 */
int WINAPI
WinMain (HINSTANCE hInstance, HINSTANCE hPrev, LPSTR szCmdLine, int nShow)
{
    BOOL is_wow64;

    if (IsWow64Process( GetCurrentProcess(), &is_wow64 ) && is_wow64)
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        WCHAR filename[MAX_PATH];
        void *redir;
        DWORD exit_code;

        memset( &si, 0, sizeof(si) );
        si.cb = sizeof(si);
        GetModuleFileNameW( 0, filename, MAX_PATH );

        Wow64DisableWow64FsRedirection( &redir );
        if (CreateProcessW( filename, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
        {
            WINE_TRACE( "restarting %s\n", wine_dbgstr_w(filename) );
            WaitForSingleObject( pi.hProcess, INFINITE );
            GetExitCodeProcess( pi.hProcess, &exit_code );
            ExitProcess( exit_code );
        }
        else WINE_ERR( "failed to restart 64-bit %s, err %d\n", wine_dbgstr_w(filename), GetLastError() );
        Wow64RevertWow64FsRedirection( redir );
    }

    /*
     * The next 9 lines should be all that is needed
     * for the Wine Configuration property sheet
     */
    InitCommonControls ();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (doPropertySheet (hInstance, NULL) > 0) {
        WINE_TRACE("OK\n");
    } else {
        WINE_TRACE("Cancel\n");
    }
    CoUninitialize();
    ExitProcess (0);

    return 0;
}

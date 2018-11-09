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
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <winternl.h>
#include <wine/debug.h>
#include <wine/library.h>

#include <wine/svcctl.h>
#include <wine/unicode.h>
#include <wine/library.h>
#include <wine/debug.h>

#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(ninecfg);

#ifndef WINE_STAGING
#warning DO NOT DEFINE WINE_STAGING TO 1 ON STABLE BRANCHES, ONLY ON WINE-STAGING ENABLED WINES
#define WINE_STAGING 1
#endif

#if !WINE_STAGING

#if HAVE_DLADDR
#define _GNU_SOURCE
#include <dlfcn.h>
#else
#error neither HAVE_DLADDR nor WINE_STAGING is set
#endif

static BOOL isWin64(void)
{
    return sizeof(void*) == 8;
}

static BOOL Call32bitNineWineCfg(BOOL state)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    CHAR buf[MAX_PATH];

    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );

    if (!GetSystemWow64DirectoryA((LPSTR)buf, sizeof(buf)))
        return FALSE;

    strcat(buf, "\\ninewinecfg.exe");

    if (state)
        strcat(buf, " -e -n");
    else
        strcat(buf, " -d -n");

    if (!CreateProcessA(NULL, buf, NULL, NULL,
        FALSE, 0, NULL, NULL, &si, &pi )) {
        WINE_ERR("Failed to call CreateProcess, error=%d", GetLastError());
        return FALSE;
    }
    else
        WaitForSingleObject( pi.hProcess, INFINITE );

    return TRUE;
}

static BOOL isWoW64(void)
{
    BOOL is_wow64;

    return IsWow64Process( GetCurrentProcess(), &is_wow64 ) && is_wow64;
}

static BOOL Call64bitNineWineCfg(BOOL state)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    void *redir;
    DWORD exit_code;
    CHAR buf[MAX_PATH];

    Wow64DisableWow64FsRedirection( &redir );

    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );

    if (!GetSystemDirectoryA((LPSTR)buf, sizeof(buf)))
        return FALSE;

    strcat(buf, "\\ninewinecfg.exe");

    if (state)
        strcat(buf, " -e -n");
    else
        strcat(buf, " -d -n");
 
    if (!CreateProcessA(NULL, buf, NULL, NULL,
        FALSE, 0, NULL, NULL, &si, &pi )) {
        WINE_ERR("Failed to call CreateProcess, error=%d", GetLastError());
        return FALSE;
    }
    else
        WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, &exit_code );

    Wow64RevertWow64FsRedirection( redir );
    return TRUE;
}

/* helper functions taken from NTDLL and KERNEL32 */
static LPWSTR FILE_name_AtoW(LPCSTR name, int optarg)
{
    ANSI_STRING str;
    UNICODE_STRING strW, *pstrW;
    NTSTATUS status;

    RtlInitAnsiString( &str, name );
    pstrW = &strW ;
    status = RtlAnsiStringToUnicodeString( pstrW, &str, TRUE );
    if (status == STATUS_SUCCESS) return pstrW->Buffer;
    return NULL;
}

static BOOL WINAPI CreateSymLinkW(LPCWSTR lpFileName, LPCSTR existingUnixFileName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    NTSTATUS status;
    UNICODE_STRING ntDest;
    ANSI_STRING unixDest;
    BOOL ret = FALSE;

    TRACE("(%s, %s, %p)\n", debugstr_w(lpFileName),
         existingUnixFileName, lpSecurityAttributes);

    ntDest.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U( lpFileName, &ntDest, NULL, NULL ))
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    unixDest.Buffer = NULL;
    status = wine_nt_to_unix_file_name( &ntDest, &unixDest, FILE_CREATE, FALSE );
    if (!status) /* destination must not exist */
    {
        status = STATUS_OBJECT_NAME_EXISTS;
    } else if (status == STATUS_NO_SUCH_FILE)
    {
        status = STATUS_SUCCESS;
    }

    if (status)
         SetLastError( RtlNtStatusToDosError(status) );
    else if (!symlink( existingUnixFileName, unixDest.Buffer ))
    {
        TRACE("Symlinked '%s' to '%s'\n", debugstr_a( unixDest.Buffer ),
            existingUnixFileName);
        ret = TRUE;
    }

    RtlFreeAnsiString( &unixDest );

err:
    RtlFreeUnicodeString( &ntDest );
    return ret;
}

static BOOL WINAPI CreateSymLinkA(LPCSTR lpFileName, LPCSTR lpExistingUnixFileName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    WCHAR *destW;
    BOOL res;

    if (!(destW = FILE_name_AtoW( lpFileName, TRUE )))
    {
        return FALSE;
    }

    res = CreateSymLinkW( destW, lpExistingUnixFileName, lpSecurityAttributes );

    HeapFree( GetProcessHeap(), 0, destW );

    return res;
}

static BOOL WINAPI IsFileSymLinkW(LPCWSTR lpExistingFileName)
{
    NTSTATUS status;
    UNICODE_STRING ntSource;
    ANSI_STRING unixSource;
    BOOL ret = FALSE;
    struct stat sb;

    TRACE("(%s)\n", debugstr_w(lpExistingFileName));

    ntSource.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U( lpExistingFileName, &ntSource, NULL, NULL ))
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    unixSource.Buffer = NULL;
    status = wine_nt_to_unix_file_name( &ntSource, &unixSource, FILE_OPEN, FALSE );
    if (status == STATUS_NO_SUCH_FILE)
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    if (!lstat( unixSource.Buffer, &sb) && (sb.st_mode & S_IFMT) == S_IFLNK)
    {
        ret = TRUE;
    }

    RtlFreeAnsiString( &unixSource );

err:
    RtlFreeUnicodeString( &ntSource );
    return ret;
}

static BOOL WINAPI IsFileSymLinkA(LPCSTR lpExistingFileName)
{
    WCHAR *sourceW;
    BOOL res;

    if (!(sourceW = FILE_name_AtoW( lpExistingFileName, TRUE )))
    {
        return FALSE;
    }

    res = IsFileSymLinkW( sourceW );

    HeapFree( GetProcessHeap(), 0, sourceW );

    return res;
}

static BOOL nine_get_system_path(CHAR *pOut, DWORD SizeOut)
{
    if (isWoW64()) {
        return !!GetSystemWow64DirectoryA((LPSTR)pOut, SizeOut);
    } else {
        return !!GetSystemDirectoryA((LPSTR)pOut, SizeOut);
    }
}

#endif

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

#if WINE_STAGING
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
            WINE_WARN("Failed to read value 'd3d9'. rc = %d\n", rc);

        RegCloseKey(regkey);
    }
    else
    {
        WINE_ERR("Failed to open path 'HKCU\\Software\\Wine\\DllRedirects'\n");
    }
#else

    CHAR buf[MAX_PATH];

    if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\DllOverrides", &regkey))
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
                ret = !!val && !strcmp(val, "native");
            }
            else
                ret = FALSE;

            HeapFree(GetProcessHeap(), 0, val);
        }
        else
            ret = FALSE;

        RegCloseKey(regkey);
    }
    else
        WINE_WARN("Failed to open path 'HKCU\\Software\\Wine\\DllOverrides'\n");

    if (!ret)
        return ret;

    if (!nine_get_system_path(buf, sizeof(buf))) {
        WINE_ERR("Failed to get system path\n");
        return FALSE;
    }

    strcat(buf, "\\d3d9.dll");
    /* FIXME: Test symlink destination */
    ret = IsFileSymLinkA(buf);
#endif
    return ret;
}

static void nine_set(BOOL status, BOOL NoOtherArch)
{
    HKEY regkey;

#if WINE_STAGING
    /* Active dll redirect */
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
        WINE_ERR("Failed to open path 'HKCU\\Software\\Wine\\DllRedirects'\n");
#else
    CHAR dst[MAX_PATH];

    /* Prevent infinite recursion if called from other arch already */
    if (!NoOtherArch) {
        /* Started as 64bit, call 32bit process */
        if (isWin64())
            Call32bitNineWineCfg(status);
        /* Started as 32bit, call 64bit process */
        else if (isWoW64())
            Call64bitNineWineCfg(status);
    }

    /* enable native dll */
    if (!RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Wine\\DllOverrides", &regkey))
    {
        LSTATUS rc;

        if (!status)
        {
            rc = RegDeleteValueA(regkey, "d3d9");
        }
        else
        {
            rc = RegSetValueExA(regkey, "d3d9", 0, REG_SZ, (LPBYTE)"native", strlen("native"));
        }
        if (rc != NO_ERROR)
            WINE_WARN("Failed to write 'HKCU\\Software\\Wine\\DllOverrides\\d3d9'. rc = %d\n", rc);

        RegCloseKey(regkey);
    }
    else
        WINE_WARN("Failed to open path 'HKCU\\Software\\Wine\\DllRedirects'\n");

    if (!nine_get_system_path(dst, sizeof(dst))) {
        WINE_ERR("Failed to get system path\n");
        return;
    }
    strcat(dst, "\\d3d9.dll");

    if (status) {
        HMODULE hmod;

        /* FIXME: Test symlink destination */
        if (IsFileSymLinkA(dst))
            return;

        /* Just in case native dll has been installed */
        DeleteFileA(dst);

        hmod = LoadLibraryExA("d3d9-nine.dll", NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (hmod)
        {
#if HAVE_DLADDR
            Dl_info info;

            if (dladdr(hmod, &info) && info.dli_fname)
            {
                if (!CreateSymLinkA(dst, info.dli_fname, NULL))
                    WINE_ERR("CreateSymLinkA(%s,%s) failed\n", dst, info.dli_fname);

            }
            else
                WINE_ERR("dladdr failed to get file path\n");
#endif

            FreeLibrary(hmod);
        }
        else
            WINE_ERR("d3d9-nine.dll not found.\n");
    }
    else
        DeleteFileA(dst);

#endif
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

    /* FIXME: don't leak iface here ... */
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

    if (hmod)
        FreeLibrary(hmod);

    return;
out:
    EnableWindow(GetDlgItem(dialog, IDC_ENABLE_NATIVE_D3D9), 0);

    if (hmod)
        FreeLibrary(hmod);
}

BOOL ProcessCmdLine(WCHAR *cmdline)
{
    WCHAR **argv;
    int argc, i;
    BOOL NoOtherArch = FALSE;
    BOOL NineSet = FALSE;
    BOOL NineClear = FALSE;

    argv = CommandLineToArgvW(cmdline, &argc);

    if (!argv)
        return FALSE;

    if (argc == 1)
    {
        LocalFree(argv);
        return FALSE;
    }

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] != '/' && argv[i][0] != '-')
            break; /* No flags specified. */

        if (!argv[i][1] && argv[i][0] == '-')
            break; /* '-' is a filename. It indicates we should use stdin. */

        if (argv[i][1] && argv[i][2] && argv[i][2] != ':')
            break; /* This is a file path beginning with '/'. */

        switch (toupperW(argv[i][1]))
        {
        case '?':
            WINE_ERR("\nSupported arguments: [ -e | -d ][ -n ]\n-e Enable nine\n-d Disable nine\n-n Do not call other arch exe\n");
            return TRUE;
        case 'E':
            NineSet = TRUE;
            break;
        case 'D':
            NineClear = TRUE;
            break;
        case 'N':
            NoOtherArch = TRUE;
            break;
        default:
            return FALSE;
        }
    }

    if (NineSet && !NineClear)
    {
        nine_set(TRUE, NoOtherArch);
        return TRUE;
    }
    else if (NineClear && !NineSet)
    {
        nine_set(FALSE, NoOtherArch);
        return TRUE;
    }

    return FALSE;
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
            nine_set(IsDlgButtonChecked(hDlg, IDC_ENABLE_NATIVE_D3D9) == BST_CHECKED, FALSE);
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
#if 0
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
#endif
    if (ProcessCmdLine(GetCommandLineW())) {
        return 0;
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

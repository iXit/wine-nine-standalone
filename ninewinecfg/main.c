/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * NineWineCfg main entry point
 *
 * Copyright 2002 Jaco Greeff
 * Copyright 2003 Dimitrie O. Paun
 * Copyright 2003 Mike Hearn
 * Copyright 2017 Patrick Rudolph
 */

#include <windows.h>
#include <ntstatus.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <winternl.h>
#include <d3d9.h>
#include <wine/debug.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <wctype.h>

#include "../common/library.h"
#include "../common/registry.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(ninecfg);

static const char * const fn_nine_dll = "d3d9-nine.dll";
static const char * const fn_d3d9_dll = "d3d9.dll";
static const char * const fn_nine_exe = "ninewinecfg.exe";

static const char * const err_unknown = "Unknown error";
static const char * const err_outofmemory = "Out of memory";
static const char * const err_d3d_notavailable = "No compatible GPU found. On a hybrid graphics setup, you might need to set DRI_PRIME=1 first";

static BOOL isWin64(void)
{
    return sizeof(void*) == 8;
}

static BOOL isWoW64(void)
{
    BOOL is_wow64;

    return IsWow64Process( GetCurrentProcess(), &is_wow64 ) && is_wow64;
}

static DWORD executeCmdline(LPSTR cmdline)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD exit_code;

    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );

    WINE_TRACE("Executing cmdline '%s'\n", cmdline);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL,
        FALSE, 0, NULL, NULL, &si, &pi ))
    {
        WINE_ERR("CreateProcessA failed, error=%d", GetLastError());
        return ~0u;
    }

    if (WaitForSingleObject( pi.hProcess, INFINITE ) != WAIT_OBJECT_0)
    {
        WINE_ERR("WaitForSingleObject failed, error=%d", GetLastError());
        return ~0u;
    }

    if (!GetExitCodeProcess( pi.hProcess, &exit_code ))
    {
        WINE_ERR("GetExitCodeProcess failed, error=%d", GetLastError());
        return ~0u;
    }

    WINE_TRACE("Exit code: %u\n", exit_code);

    return exit_code;
}

static BOOL Call32bitNineWineCfg(BOOL state)
{
    CHAR buf[MAX_PATH + 6];

    if (!GetSystemWow64DirectoryA(buf, sizeof(buf)))
        return FALSE;

    strcat(buf, "\\");
    strcat(buf, fn_nine_exe);

    if (state)
        strcat(buf, " -e -n");
    else
        strcat(buf, " -d -n");

    return executeCmdline(buf) == 0;
}

static BOOL Call64bitNineWineCfg(BOOL state)
{
    void *redir;
    CHAR buf[MAX_PATH + 6];
    BOOL res;

    Wow64DisableWow64FsRedirection( &redir );

    if (!GetSystemDirectoryA((LPSTR)buf, sizeof(buf)))
        return FALSE;

    strcat(buf, "\\");
    strcat(buf, fn_nine_exe);

    if (state)
        strcat(buf, " -e -n");
    else
        strcat(buf, " -d -n");

    res = executeCmdline(buf) == 0;

    Wow64RevertWow64FsRedirection( redir );

    return res;
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
    if (status == STATUS_SUCCESS)
        return pstrW->Buffer;

    return NULL;
}

static BOOL WINAPI DeleteSymLinkW(LPCWSTR lpFileName)
{
    NTSTATUS status;
    UNICODE_STRING ntDest;
    ANSI_STRING unixDest;
    BOOL ret = FALSE;

    WINE_TRACE("(%s)\n", wine_dbgstr_w(lpFileName));

    ntDest.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U( lpFileName, &ntDest, NULL, NULL ))
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    unixDest.Buffer = NULL;
    status = wine_nt_to_unix_file_name( &ntDest, &unixDest, 0, FALSE );
    if (!status)
    {
        if (!unlink(unixDest.Buffer))
        {
            WINE_TRACE("Removed symlink '%s'\n", wine_dbgstr_a( unixDest.Buffer ));
            ret = TRUE;
            status = STATUS_SUCCESS;
        }
        else
        {
            WINE_ERR("Failed to remove symlink\n");
        }
    }

    if (status)
         SetLastError( RtlNtStatusToDosError(status) );

    RtlFreeAnsiString( &unixDest );

err:
    RtlFreeUnicodeString( &ntDest );
    return ret;
}

static BOOL WINAPI DeleteSymLinkA(LPCSTR lpFileName)
{
    WCHAR *destW;
    BOOL res;

    if (!(destW = FILE_name_AtoW( lpFileName, TRUE )))
    {
        return FALSE;
    }

    res = DeleteSymLinkW( destW );

    HeapFree( GetProcessHeap(), 0, destW );

    return res;
}

static BOOL WINAPI CreateSymLinkW(LPCWSTR lpFileName, LPCSTR existingUnixFileName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    NTSTATUS status;
    UNICODE_STRING ntDest;
    ANSI_STRING unixDest;
    BOOL ret = FALSE;

    WINE_TRACE("(%s, %s, %p)\n", wine_dbgstr_w(lpFileName),
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
        WINE_TRACE("Symlinked '%s' to '%s'\n", wine_dbgstr_a( unixDest.Buffer ),
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

    WINE_TRACE("(%s)\n", wine_dbgstr_w(lpExistingFileName));

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
    if (isWoW64())
    {
        return !!GetSystemWow64DirectoryA((LPSTR)pOut, SizeOut);
    }
    else
    {
        return !!GetSystemDirectoryA((LPSTR)pOut, SizeOut);
    }
}

/*
 * Winecfg
 */
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
    BOOL ret = FALSE;
    LPSTR value;

    CHAR buf[MAX_PATH];

    if (common_get_registry_string(reg_path_dll_overrides, reg_key_d3d9, &value))
    {
        ret = !strcmp(value, reg_value_override);
        HeapFree(GetProcessHeap(), 0, value);
    }

    if (!nine_get_system_path(buf, sizeof(buf)))
    {
        WINE_ERR("Failed to get system path\n");
        return FALSE;
    }
    strcat(buf, "\\");
    strcat(buf, fn_d3d9_dll);

    if (!ret && IsFileSymLinkA(buf))
    {
        /* Sanity: Remove symlink if any */
        WINE_ERR("removing obsolete symlink\n");
        DeleteSymLinkA(buf);
        return FALSE;
    }

    ret = IsFileSymLinkA(buf);
    if (ret && !PathFileExistsA(buf))
    {
        /* broken symlink */
        DeleteSymLinkA(buf);
        WINE_ERR("removing dead symlink\n");
        return FALSE;
    }

    return ret;
}

static void nine_set(BOOL status, BOOL NoOtherArch)
{
    CHAR dst[MAX_PATH];

    /* Prevent infinite recursion if called from other arch already */
    if (!NoOtherArch)
    {
        /* Started as 64bit, call 32bit process */
        if (isWin64())
            Call32bitNineWineCfg(status);
        /* Started as 32bit, call 64bit process */
        else if (isWoW64())
            Call64bitNineWineCfg(status);
    }

    /* Delete unused DllRedirects key */
    common_del_registry_key(reg_path_dll_redirects, reg_key_d3d9);

    /* enable native dll */
    if (!status)
    {
        if (!common_del_registry_key(reg_path_dll_overrides, reg_key_d3d9))
            WINE_ERR("Failed to delete 'HKCU\\%s\\%s'\n'", reg_path_dll_overrides, reg_key_d3d9);
    }
    else
    {
        if (!common_set_registry_string(reg_path_dll_overrides, reg_key_d3d9, reg_value_override))
            WINE_ERR("Failed to write 'HKCU\\%s\\%s'\n", reg_path_dll_overrides, reg_key_d3d9);
    }

    if (!nine_get_system_path(dst, sizeof(dst))) {
        WINE_ERR("Failed to get system path\n");
        return;
    }
    strcat(dst, "\\");
    strcat(dst, fn_d3d9_dll);

    if (status)
    {
        HMODULE hmod;

        /* Sanity: Always recreate symlink */
        DeleteSymLinkA(dst);

        hmod = LoadLibraryExA(fn_nine_dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (hmod)
        {
            Dl_info info;

            if (dladdr(hmod, &info) && info.dli_fname)
            {
                if (!CreateSymLinkA(dst, info.dli_fname, NULL))
                    WINE_ERR("CreateSymLinkA(%s,%s) failed\n", dst, info.dli_fname);

            }
            else
                WINE_ERR("dladdr failed to get file path\n");

            FreeLibrary(hmod);
        }
        else
            WINE_ERR("%s not found.\n", fn_nine_dll);
    }
    else
        DeleteSymLinkA(dst);
}

typedef HRESULT (WINAPI *LPDIRECT3DCREATE9EX)(UINT, IDirect3D9Ex **);

static void load_settings(HWND dialog)
{
    HMODULE hmod = NULL;
    char *path = NULL, *err = NULL;
    LPDIRECT3DCREATE9EX Direct3DCreate9ExPtr = NULL;
    IDirect3D9Ex *iface = NULL;
    void *handle;
    HRESULT hr;

    EnableWindow(GetDlgItem(dialog, IDC_ENABLE_NATIVE_D3D9), 0);
    CheckDlgButton(dialog, IDC_ENABLE_NATIVE_D3D9, nine_get() ? BST_CHECKED : BST_UNCHECKED);

    SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_SO, NULL);
    SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_DLL, NULL);
    SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_CREATE, NULL);

    CheckDlgButton(dialog, IDC_NINE_STATE_SO, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE_DLL, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE_CREATE, BST_UNCHECKED);

    handle = common_load_d3dadapter(&path, &err);

    if (handle)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE_SO, BST_CHECKED);
        SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_SO, path);
    }
    else
    {
        SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_SO, err);
        goto out;
    }

    hmod = LoadLibraryA(fn_nine_dll);
    if (hmod)
        Direct3DCreate9ExPtr = (LPDIRECT3DCREATE9EX)
                GetProcAddress(hmod, "Direct3DCreate9Ex");

    if (hmod && Direct3DCreate9ExPtr)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE_DLL, BST_CHECKED);
        {
            Dl_info info;

            if (dladdr(hmod, &info) && info.dli_fname)
                SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_DLL, info.dli_fname);
            else
                SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_DLL, dlerror());
        }
    }
    else
    {
        wchar_t buf[256];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)buf, 256, NULL);

        SetDlgItemTextW(dialog, IDC_NINE_STATE_TIP_DLL, buf);
        goto out;
    }

    hr = Direct3DCreate9ExPtr(0, &iface);
    if (SUCCEEDED(hr))
    {
        IDirect3DDevice9_Release(iface);
        CheckDlgButton(dialog, IDC_NINE_STATE_CREATE, BST_CHECKED);
    }
    else
    {
        const char *msg;

        switch (hr)
        {
        case E_OUTOFMEMORY:
            msg = err_outofmemory;
            break;
        case D3DERR_NOTAVAILABLE:
            msg = err_d3d_notavailable;
            break;
        default:
            msg = err_unknown;
            break;
        }

        SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP_CREATE, msg);
        goto out;
    }

    EnableWindow(GetDlgItem(dialog, IDC_ENABLE_NATIVE_D3D9), 1);

out:
    if (hmod)
        FreeLibrary(hmod);

    free(path);
    free(err);
}

static BOOL ProcessCmdLine(WCHAR *cmdline, BOOL *result)
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

        switch (towupper(argv[i][1]))
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
        *result = nine_get();
        return TRUE;
    }
    else if (NineClear && !NineSet)
    {
        nine_set(FALSE, NoOtherArch);
        *result = !nine_get();
        return TRUE;
    }

    return FALSE;
}

static INT_PTR CALLBACK AppDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        load_settings(hDlg);
        break;

    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) break;
        switch (LOWORD(wParam))
        {
        case IDC_ENABLE_NATIVE_D3D9:
            nine_set(IsDlgButtonChecked(hDlg, IDC_ENABLE_NATIVE_D3D9) == BST_UNCHECKED, FALSE);
            CheckDlgButton(hDlg, IDC_ENABLE_NATIVE_D3D9, nine_get() ? BST_CHECKED : BST_UNCHECKED);
            SendMessageW(GetParent(hDlg), PSM_CHANGED, 0, 0);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        SetDlgItemTextA(hDlg, IDC_NINE_URL, "<a href=\"" NINE_URL "\">" NINE_URL "</a>");
        break;

    case WM_NOTIFY:
        switch (((LPNMHDR)lParam)->code)
        {
        case NM_CLICK:
        case NM_RETURN:
            if (wParam == IDC_NINE_URL)
              ShellExecuteA(NULL, "open", NINE_URL, NULL, NULL, SW_SHOW);

            break;
        }

        break;
    }

    return FALSE;
}

static INT_PTR
doPropertySheet (HINSTANCE hInstance, HWND hOwner)
{
    PROPSHEETPAGEW psp[2];
    PROPSHEETHEADERW psh;

    psp[0].dwSize = sizeof (PROPSHEETPAGEW);
    psp[0].dwFlags = PSP_USETITLE;
    psp[0].hInstance = hInstance;
    psp[0].pszTemplate = MAKEINTRESOURCEW (IDD_NINE);
    psp[0].pszIcon = NULL;
    psp[0].pfnDlgProc = AppDlgProc;
    psp[0].pszTitle = load_string (IDS_TAB_MAIN);
    psp[0].lParam = 0;

    psp[1].dwSize = sizeof (PROPSHEETPAGEW);
    psp[1].dwFlags = PSP_USETITLE;
    psp[1].hInstance = hInstance;
    psp[1].pszTemplate = MAKEINTRESOURCEW (IDD_ABOUT);
    psp[1].pszIcon = NULL;
    psp[1].pfnDlgProc = AboutDlgProc;
    psp[1].pszTitle = load_string (IDS_TAB_ABOUT);
    psp[1].lParam = 0;

    /*
     * Fill out the PROPSHEETHEADER
     */
    psh.dwSize = sizeof (PROPSHEETHEADERW);
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_USEICONID | PSH_USECALLBACK | PSH_NOAPPLYNOW;
    psh.hwndParent = hOwner;
    psh.hInstance = hInstance;
    psh.pszIcon = NULL;
    psh.pszCaption =  load_string (IDS_NINECFG_TITLE);
    psh.nPages = sizeof(psp) / sizeof(psp[0]);
    psh.ppsp = psp;
    psh.pfnCallback = NULL;
    psh.nStartPage = 0;

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
    BOOL res = FALSE;

    if (ProcessCmdLine(GetCommandLineW(), &res))
    {
        if (!res)
            return 1;

        return 0;
    }

    /*
     * The next 9 lines should be all that is needed
     * for the Wine Configuration property sheet
     */
    InitCommonControls ();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (doPropertySheet (hInstance, NULL) > 0)
    {
        WINE_TRACE("OK\n");
    }
    else
    {
        WINE_TRACE("Cancel\n");
    }
    CoUninitialize();
    ExitProcess (0);

    return 0;
}

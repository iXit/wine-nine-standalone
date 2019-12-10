/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <windows.h>

#include "../common/debug.h"
#include "registry.h"

const char * const reg_path_dll_overrides = "Software\\Wine\\DllOverrides";
const char * const reg_path_dll_redirects = "Software\\Wine\\DllRedirects";
const char * const reg_key_d3d9 = "d3d9";
const char * const reg_path_nine = "Software\\Wine\\Direct3DNine";
const char * const reg_key_module_path = "ModulePath";
const char * const reg_value_override = "native";

BOOL common_get_registry_string(LPCSTR path, LPCSTR name, LPSTR *value)
{
    HKEY regkey;
    DWORD type;
    DWORD size = 0;

    TRACE("Getting string key '%s' at 'HKCU\\%s'\n", name, path);

    if (RegOpenKeyA(HKEY_CURRENT_USER, path, &regkey) != ERROR_SUCCESS)
    {
        TRACE("Failed to open path 'HKCU\\%s'\n", path);
        return FALSE;
    }

    if (RegQueryValueExA(regkey, name, 0, &type, NULL, &size) != ERROR_SUCCESS)
    {
        TRACE("Failed to query key '%s' at 'HKCU\\%s'\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    if (type != REG_SZ)
    {
        TRACE("Key '%s' at 'HKCU\\%s' is not a string\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    *value = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 1);
    if (!(*value))
    {
        RegCloseKey(regkey);
        return FALSE;
    }

    if (RegQueryValueExA(regkey, name, 0, &type, (LPBYTE)*value, &size) != ERROR_SUCCESS)
    {
        TRACE("Failed to read value of key '%s' at 'HKCU\\%s'\n", name, path);
        HeapFree(GetProcessHeap(), 0, *value);
        RegCloseKey(regkey);
        return FALSE;
    }

    RegCloseKey(regkey);

    TRACE("Value is '%s'\n", *value);

    return TRUE;
}

BOOL common_set_registry_string(LPCSTR path, LPCSTR name, LPCSTR value)
{
    HKEY regkey;

    TRACE("Setting key '%s' at 'HKCU\\%s' to '%s'\n", name, path, value);

    if (RegCreateKeyA(HKEY_CURRENT_USER, path, &regkey) != ERROR_SUCCESS)
    {
        TRACE("Failed to open path 'HKCU\\%s'\n", path);
        return FALSE;
    }

    if (RegSetValueExA(regkey, name, 0, REG_SZ, (LPBYTE)value, strlen(value)) != ERROR_SUCCESS)
    {
        TRACE("Failed to write key '%s' at 'HKCU\\%s'\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    RegCloseKey(regkey);

    return TRUE;
}

BOOL common_del_registry_key(LPCSTR path, LPCSTR name)
{
    HKEY regkey;
    LSTATUS rc;

    TRACE("Deleting key '%s' at 'HKCU\\%s'\n", name, path);

    rc = RegOpenKeyA(HKEY_CURRENT_USER, path, &regkey);
    if (rc == ERROR_FILE_NOT_FOUND)
        return TRUE;

    if (rc != ERROR_SUCCESS)
    {
        TRACE("Failed to open path 'HKCU\\%s'\n", path);
        return FALSE;
    }

    rc = RegDeleteValueA(regkey, name);
    if (rc != ERROR_FILE_NOT_FOUND && rc != ERROR_SUCCESS)
    {
        TRACE("Failed to delete key '%s' at 'HKCU\\%s'\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    RegCloseKey(regkey);

    return TRUE;
}

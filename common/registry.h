/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __COMMON_REGISTRY_H
#define __COMMON_REGISTRY_H

#include <windows.h>

extern const char * const reg_path_dll_overrides;
extern const char * const reg_path_dll_redirects;
extern const char * const reg_key_d3d9;
extern const char * const reg_path_nine;
extern const char * const reg_key_module_path;
extern const char * const reg_value_override;

BOOL common_get_registry_string(LPCSTR path, LPCSTR name, LPSTR *value);
BOOL common_set_registry_string(LPCSTR path, LPCSTR name, LPCSTR value);
BOOL common_del_registry_key(LPCSTR path, LPCSTR name);

#endif /* __COMMON_REGISTRY_H */

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Direct3D 9 ShaderValidator
 *
 * Copyright 2016 Patrick Rudolph
 */

#ifndef __NINE_SHADER_VALIDATOR_H
#define __NINE_SHADER_VALIDATOR_H

#include <windef.h>

typedef struct IDirect3DShaderValidator9Impl
{
    /* IUnknown fields */
    void *lpVtbl;
    LONG ref;
} IDirect3DShaderValidator9Impl;

extern const void *IDirect3DShaderValidator9Vtbl[6];

#endif /* __NINE_SHADER_VALIDATOR_H */

/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright 2016 Axel Davy
 */

#ifndef __NINE_DEVICE_WRAP_H
#define __NINE_DEVICE_WRAP_H

#include <d3dadapter/d3dadapter9.h>

IDirect3DDevice9ExVtbl *get_device_vtable(void);

#endif /* __NINE_DEVICE_WRAP_H */

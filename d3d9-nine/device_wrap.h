/*
 * Copyright 2016 Axel Davy
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

#ifndef __NINE_DEVICE_WRAP_H
#define __NINE_DEVICE_WRAP_H

#include <d3dadapter/d3dadapter9.h>

IDirect3DDevice9ExVtbl *get_device_vtable(void);

#endif /* __NINE_DEVICE_WRAP_H */

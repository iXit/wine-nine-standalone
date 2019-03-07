# SPDX-License-Identifier: LGPL-2.1-or-later

d3d9_res = custom_target(
  'version.res',
  input   : 'version.rc',
  output  : 'version.res',
  command : wrc,
)

d3d9_dll = shared_library(
  'd3d9-nine.dll',
  [
    'backend.c',
    'd3d9_main.c',
    'd3dadapter9.c',
    'device_wrap.c',
    'dri2.c',
    'dri3.c',
    'present.c',
    'shader_validator.c',
    'wndproc.c',
    'xcb_present.c',
    d3d9_res,
  ],
  name_prefix    : '',
  link_with      : [
                     libd3d9common,
                   ],
  dependencies   : [
                     dep_d3dadapter9,
                     dep_dl,
                     dep_x11,
                     dep_x11_xcb,
                     dep_xcb,
                     dep_xcb_dri2,
                     dep_xcb_dri3,
                     dep_xcb_present,
                     dep_xcb_xfixes,
                     dep_gl,
                     dep_egl,
                     dep_dxguid,
                     dep_uuid,
                     dep_advapi32,
                     dep_user32,
                   ],
  install        : true,
  vs_module_defs : 'd3d9.spec',
  objects        : 'd3d9.spec',
)

d3d9_fake = shared_library(
  'd3d9-nine',
  objects        : d3d9_dll.extract_all_objects(),
  name_prefix    : '',
  name_suffix    : 'dll.fake',
  link_with      : [
                     libd3d9common,
                   ],
  install        : true,
)

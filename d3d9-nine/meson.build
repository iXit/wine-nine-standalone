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
  [
    d3d9_res,
  ],
  name_prefix    : '',
  name_suffix    : 'dll.fake',
  install        : true,
)

if wine_version.version_compare('>= 7.3')
  d3d9_forwarder_builtin = shared_library(
    'd3d9-nine-forwarder',
    [
      d3d9_res,
    ],
    name_prefix    : '',
    name_suffix    : 'dll.builtin',
    install        : false,
    vs_module_defs : 'd3d9-nine-forwarder.spec',
    objects        : 'd3d9-nine-forwarder.spec',
    link_args      : [
                       '-Wb,--data-only',
                       '-Wb,--filename=d3d9',
                     ],
  )

  # Wine checks for a string in the header to identify builtin DLLs.
  # Work around this with a simple substitution.
  d3d9_forwarder = custom_target(
    'd3d9-nine-forwarder.dll',
    input          : d3d9_forwarder_builtin,
    output         : 'd3d9-nine-forwarder.dll',
    install        : false,
    command        : [
                       sed,
                       '-e', 's|Wine builtin DLL|Nine forwarder  |',
                       '@INPUT@',
                     ],
    capture : true,
  )
else
  # Support for building data-only modules was added in wine 7.3.
  # For binary releases, use a prebuilt version (without resources).
  # This is a small (8KiB) and reproducible file that contains
  # no executable code, so it shouldn't be a problem.
  if target_machine.cpu_family() == 'x86_64'
    d3d9_forwarder = files('d3d9-nine-forwarder64.dll')
  else
    d3d9_forwarder = files('d3d9-nine-forwarder32.dll')
  endif
endif

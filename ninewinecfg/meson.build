# SPDX-License-Identifier: LGPL-2.1-or-later

ninewinecfg_res = custom_target(
  'ninewinecfg.res',
  input        : [
                   'ninewinecfg.rc',
                   d3d9_forwarder,
                 ],
  output       : 'ninewinecfg.res',
  depend_files : [
                   'resource.h',
                   'ninewinecfg.manifest',
                   'nls/',
                 ],
  command      : wrc + [
                   '-D_FORWARDER_DLL="@INPUT1@"',
                 ],
)

ninewinecfg_exe = executable(
  'ninewinecfg.exe.so',
  [
    'main.c',
    ninewinecfg_res,
  ],
  gui_app        : true,
  link_with      : [
                     libd3d9common,
                   ],
  dependencies   : [
                     dep_dl,
                     dep_shell32,
                     dep_gdi32,
                     dep_advapi32,
                     dep_user32,
                     dep_comctl32,
                     dep_ole32,
                   ],
  install        : true
)

ninewinecfg_fake = executable(
  'ninewinecfg',
  [
    ninewinecfg_res,
  ],
  name_prefix    : '',
  name_suffix    : 'exe.fake',
  install        : true,
)

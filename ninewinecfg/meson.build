# SPDX-License-Identifier: LGPL-2.1-or-later

ninewinecfg_res = custom_target(
  'ninewinecfg.res',
  input        : 'ninewinecfg.rc',
  output       : 'ninewinecfg.res',
  depend_files : [
                   'resource.h',
                   'ninewinecfg.manifest',
                   'nls/',
                 ],
  command      : wrc,
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
  objects        : ninewinecfg_exe.extract_all_objects(),
  name_prefix    : '',
  name_suffix    : 'exe.fake',
  link_with      : [
                     libd3d9common,
                   ],
  install        : true,
)

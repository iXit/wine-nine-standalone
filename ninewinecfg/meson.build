ninewinecfg_res = custom_target(
  'ninewinecfg.res',
  input        : 'ninewinecfg.rc',
  output       : 'ninewinecfg.res',
  depend_files : ['ninewinecfg.manifest'],
  command      : [wrc, '-i', '@INPUT@', '-o', '@OUTPUT@'],
)

ninewinecfg_exe = executable(
  'ninewinecfg.exe.so',
  [
    'main.c',
    ninewinecfg_res,
  ],
  gui_app        : true,
  dependencies   : [
                     dep_dl,
                     dep_shell32,
                     dep_gdi32,
                     dep_advapi32,
                     dep_user32,
                     dep_comctl32,
                     dep_ole32,
                     dep_shlwapi,
                   ],
  install        : true
)
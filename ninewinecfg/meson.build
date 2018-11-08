ninewinecfg_exe = executable(
  'ninewinecfg.exe.so',
  [
    'main.c',
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

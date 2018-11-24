project(
  'Gallium Nine Standalone',
  ['c'],
  version : run_command(
              find_program('tools/get_version.sh', native : true)
            ).stdout(),
  license : 'LGPL2.1+',
  meson_version : '>= 0.45',
  default_options : [
    'buildtype=debugoptimized',
  ],
)

project_url = 'https://github.com/dhewg/nine'

cc = meson.get_compiler('c')

null_dep = dependency('', required : false)

pp_args = [
  # undefine winegcc's mingw compatiblity macros, x11-xcb and egl get confused
  '-U_WIN32', '-U__WIN32', '-U__WIN32__', '-U__WINNT', '-U__WINNT__',
  '-D_GNU_SOURCE',
  '-DDECLSPEC_HOTPATCH=__attribute__((__ms_hook_prologue__))',
  '-DNINE_VERSION="v' + meson.project_version() + '"',
  '-DNINE_URL="' + project_url + '"',
  '-DWINE_STAGING=0',
]

code = '''#ifndef __WINE__
#error 1
#endif'''

if not cc.compiles(code, name: 'winelib check')
  error('A WINE cross compiler is required')
endif

foreach h : ['wine/debug.h']
  if not cc.has_header(h)
    error('WINE headers not found')
  endif
endforeach

version = meson.project_version().split('.')
wrc = [
        find_program('wrc'),
        '-DNINE_MAJOR=' + version[0],
        '-DNINE_MINOR=' + version[1],
        '-DNINE_BUILD=' + version[2],
        '-DNINE_REVISION=' + version[3],
        '-DNINE_VERSION="v' + meson.project_version() + '"',
        '-DNINE_PROJECT="' + meson.project_name() + '"',
        '-i', '@INPUT@',
        '-o', '@OUTPUT@',
      ]

if cc.has_function('dlopen')
  dep_dl = null_dep
else
  dep_dl = cc.find_library('dl')
endif

foreach fn : ['dlopen', 'dlclose', 'dlsym', 'dladdr']
  if not cc.has_function(fn, dependencies : dep_dl)
    error(fn + ' is required')
  endif
endforeach

dep_d3dadapter9 = dependency('d3dadapter9', required : false)
if not dep_d3dadapter9.found()
  dep_d3dadapter9 = dependency('d3d')
endif

dep_x11 = dependency('x11')
dep_x11_ext = dependency('xext')
dep_x11_xcb = dependency('x11-xcb')
dep_xcb = dependency('xcb')
dep_xcb_dri3 = dependency('xcb-dri3')
dep_xcb_present = dependency('xcb-present')
dep_xcb_xfixes = dependency('xcb-xfixes')

dep_gl = null_dep
dep_egl = null_dep
_dri2 = get_option('dri2')
if _dri2 != 'false'
  dep_gl = dependency('gl', required : _dri2 == 'true')
  dep_egl = dependency('egl', required : _dri2 == 'true')
  pp_args += '-DD3D9NINE_DRI2=1'
endif

dep_dxguid = cc.find_library('dxguid')
dep_uuid = cc.find_library('uuid')
dep_advapi32 = cc.find_library('advapi32')
dep_gdi32 = cc.find_library('gdi32')
dep_user32 = cc.find_library('user32')
dep_shell32 = cc.find_library('shell32')
dep_comctl32 = cc.find_library('comctl32')
dep_ole32 = cc.find_library('ole32')
dep_shlwapi = cc.find_library('shlwapi')

moduledir = dep_d3dadapter9.get_pkgconfig_variable('moduledir')
if moduledir != ''
  pp_args += '-DD3D9NINE_MODULEPATH="' + moduledir + '/d3dadapter9.so.1"'
endif

foreach arg : pp_args
  add_project_arguments(arg, language : ['c'])
endforeach

subdir('d3d9-nine')
subdir('ninewinecfg')

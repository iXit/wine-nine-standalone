# SPDX-License-Identifier: LGPL-2.1-or-later

project(
  'Gallium Nine Standalone',
  ['c'],
  version : run_command(
              find_program('tools/get_version.sh', native : true)
            ).stdout(),
  license : 'LGPL2.1+',
  meson_version : '>= 0.46',
  default_options : [
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
  ],
)

project_url = 'https://github.com/iXit/wine-nine-standalone'

cc = meson.get_compiler('c', native : false)
find_program(cc.cmd_array()[0], native : false)

null_dep = dependency('', required : false)

pp_args = [
  # undefine winegcc's mingw compatiblity macros, x11-xcb and egl get confused
  '-U_WIN32', '-U__WIN32', '-U__WIN32__', '-U__WINNT', '-U__WINNT__',
  '-D_GNU_SOURCE',
  '-DWIN32_LEAN_AND_MEAN',
  '-DDECLSPEC_HOTPATCH=__attribute__((__ms_hook_prologue__))',
  '-DNINE_VERSION="v' + meson.project_version() + '"',
  '-DNINE_URL="' + project_url + '"',
]

code = '''#ifndef __WINE__
#error 1
#endif'''

if not cc.compiles(code, name: 'winegcc check')
  error('A WINE cross compiler is required')
endif

if not cc.has_header_symbol('windows.h', 'wine_get_unix_file_name')
  error('WINE headers not found')
endif

version = meson.project_version().split('-')[0].split('.')
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

dep_d3dadapter9 = dependency('d3d', required : false)
if not dep_d3dadapter9.found()
  message('trying alternative name for d3d...')
  dep_d3dadapter9 = dependency('d3dadapter9')
endif

dep_x11 = dependency('x11')
dep_x11_xcb = dependency('x11-xcb')
dep_xcb = dependency('xcb')
dep_xcb_dri3 = dependency('xcb-dri3')
dep_xcb_present = dependency('xcb-present')
dep_xcb_xfixes = dependency('xcb-xfixes')

dep_gl = null_dep
dep_egl = null_dep
dep_xcb_dri2 = null_dep
_dri2 = get_option('dri2')
if _dri2 != 'false'
  dep_gl = dependency('gl', required : _dri2 == 'true')
  dep_egl = dependency('egl', required : _dri2 == 'true')
  dep_xcb_dri2 = dependency('xcb-dri2', required: _dri2 == 'true')
  if dep_gl.found() and dep_egl.found() and dep_xcb_dri2.found()
    dep_gl = dep_gl.partial_dependency(includes : true)
    dep_egl = dep_egl.partial_dependency(includes : true)
    pp_args += '-DD3D9NINE_DRI2=1'
    message('DRI2 support is enabled')
  else
    warning('DRI2 support disabled, dependencies not found')
  endif
else
  message('DRI2 support is disabled')
endif

dep_dxguid = cc.find_library('dxguid')
dep_uuid = cc.find_library('uuid')
dep_advapi32 = cc.find_library('advapi32')
dep_gdi32 = cc.find_library('gdi32')
dep_user32 = cc.find_library('user32')
dep_shell32 = cc.find_library('shell32')
dep_comctl32 = cc.find_library('comctl32')
dep_ole32 = cc.find_library('ole32')

moduledir = dep_d3dadapter9.get_pkgconfig_variable('moduledir')

_distroindependent = get_option('distro-independent')
if (not _distroindependent) and (moduledir == '')
  error('no d3dadapter9.so.1 default module path available')
endif

if _distroindependent
  message('distro independent build')
else
  message('d3dadapter9.so.1 default module path: ' + moduledir)
  pp_args += '-DD3D9NINE_MODULEPATH="' + moduledir + '"'
endif

foreach arg : pp_args
  add_project_arguments(arg, language : ['c'])
endforeach

subdir('common')
subdir('d3d9-nine')
subdir('ninewinecfg')

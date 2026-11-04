winemetal_src = [
  'main.c',
  'airconv_thunks.c',
  'winemetal_thunks.c'
]

winemetal_ld_args = []

winemetal_link_depends = []

# MacRunner c16 (17.10.2026): ONE unix-call table for the PE and unix halves. Checked at configure time
# (immediate error) and as a build step that winemetal.dll and winemetal.so both depend on.
winemetal_table_python = find_program('python3')
winemetal_table_checker = files('check_unix_table.py')
winemetal_table_check = run_command(winemetal_table_python, winemetal_table_checker, meson.current_source_dir(), check : true)
message(winemetal_table_check.stdout().strip())
winemetal_table_stamp = custom_target('winemetal_unix_table_check',
  input : [ winemetal_table_checker, 'winemetal_thunks.c', 'airconv_thunks.h', 'unix/winemetal_unix.c' ],
  output : 'winemetal_unix_table.ok',
  command : [ winemetal_table_python, '@INPUT0@', meson.current_source_dir() ],
  capture : true,
  build_by_default : true,
)
winemetal_format_checker = files('check_format_abi.py')
winemetal_format_check = run_command(winemetal_table_python, winemetal_format_checker, join_paths(meson.project_source_root(), 'src'), check : true)
message(winemetal_format_check.stdout().strip())
winemetal_format_stamp = custom_target('winemetal_format_abi_check',
  input : [ winemetal_format_checker, 'winemetal.h', 'unix/winemetal_unix.c', 'winemetal_thunks.c' ],
  output : 'winemetal_format_abi.ok',
  command : [ winemetal_table_python, '@INPUT0@', join_paths(meson.project_source_root(), 'src') ],
  capture : true,
  build_by_default : true,
)
winemetal_link_depends += [ winemetal_table_stamp, winemetal_format_stamp ]

wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')
if wine_build_path != ''
  if not wine_build_path.startswith('/')
    wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
  endif
  lib_winecrt0 = cc.find_library('winecrt0', dirs: [
      join_paths(wine_build_path, 'libs/winecrt0', windows_builtin_install_dir),
      join_paths(wine_build_path, 'dlls/winecrt0', windows_builtin_install_dir),
    ],
  )
  lib_ntdll = cc.find_library('ntdll', dirs : [ join_paths(wine_build_path, 'dlls/ntdll', windows_builtin_install_dir)], static: true)
  lib_dbghelp = cc.find_library('dbghelp', dirs : [ join_paths(wine_build_path, 'dlls/dbghelp', windows_builtin_install_dir) ])
  winebuild = join_paths(wine_build_path, 'tools/winebuild/winebuild')
elif wine_install_path != ''
  if not wine_install_path.startswith('/')
    wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
  endif
  lib_winecrt0 = cc.find_library('winecrt0', dirs : [ join_paths(wine_install_path,'lib/wine', windows_builtin_install_dir) ])
  lib_ntdll = cc.find_library('ntdll', dirs : [ join_paths(wine_install_path, 'lib/wine', windows_builtin_install_dir)], static: true)
  lib_dbghelp = cc.find_library('dbghelp', dirs : [ join_paths(wine_install_path, 'lib/wine', windows_builtin_install_dir) ])
  winebuild = join_paths(wine_install_path, 'bin/winebuild')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

winemetal_dll = shared_library('winemetal', winemetal_src,
  name_prefix         : '',
  dependencies        : [ lib_winecrt0, lib_ntdll, lib_dbghelp, airconv_forward_dep ],
  include_directories : dxmt_include_path,
  install             : true,
  install_dir         : windows_builtin_install_dir,
  link_args           : winemetal_ld_args,
  link_depends        : [ winemetal_link_depends ],
)

winemetal_dep = declare_dependency(
  link_with           : [ winemetal_dll ],
  include_directories : [ include_directories('.') ],
)

custom_target('postprocess_lib',
  input : winemetal_dll,
  output: 'winemetal.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : winemetal_dll,
  build_by_default : true
)

if cpu_family == 'x86_64' or cpu_family == 'aarch64'
  subdir('unix')
endif

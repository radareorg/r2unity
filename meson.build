project(
  'r2unity',
  'c',
  version: '0.0.0',
  default_options: [
    'c_std=c99',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

r_util_dep = dependency('r_util')

lib_inc = include_directories('src/lib')
lib_sources = files(
  'src/lib/elf.c',
  'src/lib/lib.c',
  'src/lib/macho.c',
  'src/lib/paths.c',
  'src/lib/pe.c',
)

r2unity = executable(
  'r2unity',
  files('src/main.c') + lib_sources,
  include_directories: lib_inc,
  dependencies: r_util_dep,
  install: true,
)

test('r2unity-help', r2unity, args: ['-h'])

plugins_opt = get_option('plugins')
r_core_dep = dependency('r_core', required: plugins_opt)
r_bin_dep = dependency('r_bin', required: plugins_opt)
build_plugins = plugins_opt.enabled() or (
  plugins_opt.auto() and r_core_dep.found() and r_bin_dep.found()
)

if build_plugins
  r2_plugindir = get_option('r2_plugindir')
  if r2_plugindir == ''
    r2 = find_program('r2', required: false)
    if r2.found()
      r2_user_plugins = run_command(r2, '-H', 'R2_USER_PLUGINS', check: false)
      if r2_user_plugins.returncode() == 0
        r2_plugindir = r2_user_plugins.stdout().strip()
      endif
    endif
  endif

  if r2_plugindir == ''
    r2_plugindir = join_paths(get_option('libdir'), 'radare2', 'plugins')
    warning('r2 was not found; installing plugins under ' + r2_plugindir)
  endif

  shared_library(
    'core_r2unity',
    files('src/r2/core_r2unity.c') + lib_sources,
    include_directories: lib_inc,
    dependencies: r_core_dep,
    name_prefix: '',
    install: true,
    install_dir: r2_plugindir,
  )

  shared_library(
    'bin_r2unity',
    files('src/r2/bin_r2unity.c') + lib_sources,
    include_directories: lib_inc,
    dependencies: r_bin_dep,
    name_prefix: '',
    install: true,
    install_dir: r2_plugindir,
  )
endif

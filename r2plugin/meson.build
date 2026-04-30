# r2unity XPS integration for radare2.
# Included from libr/xps/meson.build when r2unity sources live under
# libr/xps/p/r2unity/. Appends sources and include paths to the r_core
# aggregate so the static plugin is linked into libr_core.

core_plugins += ['r2unity']

r_core_additional_inc += include_directories(join_paths('..', 'src', 'lib'))

r_core_additional_sources += files(
  '../src/lib/elf.c',
  '../src/lib/lib.c',
  '../src/lib/macho.c',
  '../src/lib/paths.c',
  '../src/lib/pe.c',
  '../src/r2/core_r2unity.c',
)

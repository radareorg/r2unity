shared_library(
  'core_r2unity',
  '../src/r2/core_r2unity.c',
  '../src/lib/elf.c',
  '../src/lib/lib.c',
  '../src/lib/macho.c',
  '../src/lib/paths.c',
  '../src/lib/pe.c',
  include_directories: ['../src/lib', include_dirs],
  dependencies: r2_core,
  override_options: ['werror=false'],
  install: true,
  install_dir: r2_plugdir,
)

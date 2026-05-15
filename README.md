# r2unity

[![CI](https://github.com/radareorg/r2unity/actions/workflows/ci.yml/badge.svg)](https://github.com/radareorg/r2unity/actions/workflows/ci.yml)

<img src="r2unity.png" alt="r2unity logo" width="140px" height="140px" align="left">

`r2unity` is a command-line tool and radare2 plugin set for inspecting Unity
IL2CPP builds. It parses `global-metadata.dat`, correlates it with the native
IL2CPP binary, and exposes managed metadata for reverse engineering.

## Highlights

- Parses IL2CPP metadata wire versions v24.1+ through v35, plus v38/v39
  metadata used by recent Unity 6 builds.
- Detects companion files for iOS, Android extracted APKs, macOS, Windows,
  Linux, and flat fixture layouts.
- Recovers managed images, assemblies, types, methods, method flags, and `ldstr`
  string literals.
- Finds method-pointer tables heuristically in ELF, Mach-O, and PE binaries.
- Lists P/Invoke and v29+ reverse-P/Invoke metadata, and emits CycloneDX 1.5
  SBOMs for managed assemblies.
- Provides both a core r2 command plugin and an `r_bin` plugin for direct
  `global-metadata.dat` inspection.

## Build

`r2unity` requires radare2 development files available through `pkg-config`.

```sh
make
make plugin
make user-install
```

Or with Meson:

```sh
meson setup build
meson compile -C build
meson install -C build
```

Most users can install it through r2pm:

```sh
r2pm -ci r2unity
```

`make` builds the CLI. `make plugin` builds `core_r2unity` and `bin_r2unity`.
`make user-install` installs both the CLI and plugins for the current user.
Meson builds the CLI by default, matching `make`. Use `-Dplugins=enabled` to
build the radare2 plugins too, or `-Dr2_plugindir=/path/to/plugins` to override
the plugin install directory.

## CLI

The normal inputs are the native IL2CPP binary and the matching
`global-metadata.dat`.

```sh
# detect companion files and platform
./r2unity -D /path/to/unity-build

# compact metadata summary
./r2unity -j /path/to/GameAssembly.dll /path/to/global-metadata.dat

# recover method flags/comments as r2 commands
./r2unity -f /path/to/GameAssembly.dll /path/to/global-metadata.dat > methods.r2

# list managed strings, interop metadata, or managed-assembly SBOM data
./r2unity -z /path/to/global-metadata.dat
./r2unity -P -j /path/to/GameAssembly.dll /path/to/global-metadata.dat
./r2unity -R -j /path/to/GameAssembly.dll /path/to/global-metadata.dat
./r2unity -S /path/to/GameAssembly.dll /path/to/global-metadata.dat > sbom.txt
./r2unity -S -j /path/to/GameAssembly.dll /path/to/global-metadata.dat > sbom.json
```

## radare2

After installing the plugins, open a Unity binary in r2 and use:

```text
r2unity?       show help
r2unity-D      detect and cache companion file paths
r2unity-i[j]   show metadata summary
r2unity-s      apply managed method flags/comments
r2unity-s*     print the r2 commands instead of applying them
r2unity-z[j]   list managed string literals
r2unity-P[*j]  list P/Invoke entries
r2unity-R[*j]  list reverse-P/Invoke entries
r2unity-S      emit managed-assembly SBOM text summary
r2unity-Sj     emit managed-assembly CycloneDX JSON
```

Set `r2unity.metadata` and `r2unity.library` manually when auto-detection is not
enough. The `bin_r2unity` plugin also lets radare2/rabin2 treat
`global-metadata.dat` as a binary format, exposing sections, strings, symbols,
classes, imports, libraries, and header fields.

## Current Limits

- v24.0 metadata, v36/v37 metadata, and WebAssembly are not supported.
- Method-pointer recovery is heuristic; manual `-a` / `-c` pointer reads are
  not implemented yet.
- P/Invoke and reverse-P/Invoke output is metadata-first and does not fully
  recover native wrapper addresses or every `DllImportAttribute` detail.
- SBOM output covers managed assemblies only, not native dependencies or file
  hashes.

Deep technical notes live in `doc/`.

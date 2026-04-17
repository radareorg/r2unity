# r2unity

`r2unity` is a small C tool that reads Unity IL2CPP `global-metadata.dat` files and emits useful output for the matching native binary. Today it ships as a CLI linked against `r_util`; the radare2 core plugin is still planned and is not part of the current build.

## Current Features

- Parses Unity IL2CPP metadata with wire versions `24` through `31`.
- Decodes metadata strings, managed string literals, type definitions, method definitions, image definitions, assembly definitions, and referenced-assembly edges.
- Generates radare2 script output for recovered method addresses.
- Scans native method-pointer tables heuristically in:
  - ELF (`libil2cpp.so`, `GameAssembly.so`)
  - Mach-O (`UnityFramework`, `GameAssembly.dylib`)
  - PE (`GameAssembly.dll`)
- Handles ELF relative relocations before scanning (`REL`, `RELA`, `RELR`).
- Emits one-line JSON status with `-j`.
- Emits a managed-assembly CycloneDX 1.5 SBOM with `-S`.
- Enumerates P/Invoke methods with `-P`.
- Enumerates reverse-P/Invoke methods with `-R` on v29+ metadata.
- Enumerates managed `ldstr` string literals with `-z`.
- Supports quiet mode, output limiting, and verbose debug tracing.

## What It Does Not Do Yet

- No radare2 core plugin is built yet.
- No automatic `global-metadata.dat` discovery exists in the CLI.
- `-a` / `-c` are exposed in the CLI but the manual pointer-table reader is still a stub.
- P/Invoke output does not yet recover `DllImport` DLL names or entry-point names, so those fields stay unresolved.
- Reverse-P/Invoke output identifies annotated methods, but does not yet recover wrapper addresses from `CodeRegistration`.
- SBOM output is metadata-only: no native-library inventory, hashes, UUIDs, or build IDs yet.
- Field, property, event, and parameter decoding are not exposed by the current CLI.

## Required Inputs

You normally give `r2unity` two files:

1. The native IL2CPP binary:
   - Android: `libil2cpp.so`
   - iOS: `UnityFramework`
   - macOS: `GameAssembly.dylib`
   - Windows: `GameAssembly.dll`
   - Linux: `GameAssembly.so`
2. The matching `global-metadata.dat`

For `-z`, metadata alone is enough:

```sh
./r2unity -z path/to/global-metadata.dat
```

## Build

```sh
make
```

Requirements:

- C compiler
- `pkg-config`
- radare2 development headers for `r_util`

The current `Makefile` builds only the CLI and links only against `r_util`.

## Usage

```text
Usage: ./r2unity [options] <executable> <global-metadata.dat>

Options:
  -j            One-line JSON status, or JSON output with -P
  -r            Emit r2-script-style text for -P / -R
  -q            Quiet mode
  -l N          Limit emitted entries to N
  -f            Auto-detect ELF/Mach-O/PE and scan method pointers
  -a 0xADDR     Manual method-pointer table address (currently stubbed)
  -c N          Pointer count for -a
  -S            Emit CycloneDX 1.5 JSON for managed assemblies
  -P            Enumerate P/Invoke methods
  -R            Enumerate reverse-P/Invoke methods (v29+)
  -z            Enumerate managed string literals
  -v            Verbose debug tracing on stderr
  -h            Show help
```

## Typical Workflows

Generate an r2 script from an iOS or Android sample:

```sh
./r2unity -f path/to/UnityFramework path/to/global-metadata.dat > unity.r2
r2 -i unity.r2 path/to/UnityFramework
```

Get a stable JSON smoke result:

```sh
./r2unity -j -f path/to/libil2cpp.so path/to/global-metadata.dat
```

Dump managed assemblies as a CycloneDX document:

```sh
./r2unity -S path/to/UnityFramework path/to/global-metadata.dat
```

List P/Invoke methods:

```sh
./r2unity -P -q path/to/UnityFramework path/to/global-metadata.dat
```

List reverse-P/Invoke methods:

```sh
./r2unity -R -q path/to/UnityFramework path/to/global-metadata.dat
```

Dump managed string literals:

```sh
./r2unity -z -q path/to/global-metadata.dat
```

## Output Modes

Default mode prints radare2 commands when method addresses are recovered. With `-f`, the scanner tries to find the method-pointer table automatically. Without pointers, the CLI can still walk metadata, but it cannot emit useful addresses.

`-j` behaves differently by mode:

- Plain CLI: one-line summary with `ok`, metadata `version`, `types`, `methods`, and `has_ptrs`
- `-P`: JSON array of P/Invoke methods
- `-R`: JSON array of reverse-P/Invoke methods

`-r` is currently only meaningful with `-P` and `-R`; it emits r2-script-style comment lines describing the interop symbols, but not resolved wrapper addresses.

## Platform Notes

- ELF support covers little-endian 32-bit and 64-bit files.
- Mach-O support covers 64-bit thin files and FAT files; FAT handling currently picks the first ARM64 slice, otherwise the first slice.
- PE support covers 32-bit and 64-bit Windows-style images.

## Known Limitations

- Reverse-P/Invoke detection is metadata-driven on v29+ by looking for `MonoPInvokeCallbackAttribute` and `UnmanagedCallersOnlyAttribute`. It does not yet recover wrapper VAs.
- The SBOM path currently uses metadata tables only. The executable path is used for naming the top-level component, not for native dependency extraction.
- Wire version support is broad, but not every older sub-version has a fully dedicated stride table yet.

## checks

```sh
```

The check suite currently covers:

- JSON summary mode
- P/Invoke enumeration
- Reverse-P/Invoke enumeration
- Managed string-literal extraction


## Related Docs

- `doc/r2unity.md` - current implementation notes
- `doc/strings.md` - managed string-literal mode
- `doc/sbom.md` - current SBOM behavior and limits
- `doc/pinvoke.md` - current interop enumeration behavior
- `doc/future.md` - remaining roadmap items

# r2unity

<img src="r2unity.png" alt="logo" width="150px" height="150px" align="left">

`r2unity` is a commandline tool and radare2 plugin for inspecting Unity apps.

Unity apps are written in **C#**, compiled into **MSIL** and then transpiled into **C++** with the il2cpp program.

The resulting binary is an AOT executable that picks from the `global-metadata.dat` file all the strings, symbols, classes, structs and many other ecma335 metadata from DotNet.

With this plugin we load all that information and expose it as plaintext, json or inside radare2, ready for proper analysis.

For idiomatic decompilation it is recommended to use **decai** with `-e decai.lang=csharp`.

--pancake

## Features

* Support from il2cpp from v24 to v31, plus Unity 6 metadata v39
* Support 32 and 64bit ELF, FatMACHO, MACHO and PE
* Heuristic method-pointer scanner
* Works on Windows, Linux, macOS, iOS and Android
* Mainly validated on arm64, but should support Intel too
* Parse headers, strings, literals, method names, types, images, assemblies
* Decodes managed `ldstr` string literals from the payload table region
* Find il2cpp companion files relative to the main app executable (`-D`)
* Interop (managed <=> native) listings (PInvoke and reverse-PInvoke calls)
* Support SBOM listings, emit CycloneDX 1.5 JSON with the assemblies (`-S`)

radare2 plugin support:

- `make plugin` builds `src/r2/core_r2unity.$(so|dylib)`.
- `make install-plugin` copies the plugin into the user radare2 plugin
  directory.
- The plugin adds `r2unity.*` eval variables and can auto-detect metadata
  and IL2CPP library paths from the currently loaded binary.
- Method-symbol mode can apply flags/comments directly to the r2 session,
  print r2 commands, or return JSON.

## Build

The commandline tool and the r2 plugin require **pkg-config** and **radare2** libraries to work:

```sh
make
make plugin
make user-install  # install user-plugin and cli in the r2pm path
```

Most users may just run `r2pm -ci r2unity` to get the repo cloned, built and installed.

After installing the plugin, open a Unity binary in r2 and use:

```text
r2unity?       show help
```

And you may also have it available via `r2pm -r r2unity` unless you setup the *PATH* environ.


## CLI Usage

```text
Usage: ./r2unity [options] <executable> <global-metadata.dat>

Options:
  -a 0xADDR     Read the method pointer table starting at virtual address 0xADDR
  -c N          Read N pointer entries (pair with -a)
  -D            Detect companion files from the given executable path and exit
  -f            Fast path: auto-detect ELF/Mach-O/PE and scan method pointers
  -h            Show help and exit
  -j            One-line JSON status, or JSON output with -P
  -l N          Limit emitted entries to N
  -P            Enumerate P/Invoke (managed -> native) methods
  -q            Quiet mode: omit banner and informational comments
  -r            Emit r2 script commands (flags + comments); pairs with -P/-R
  -R            Enumerate reverse-P/Invoke (native -> managed) methods (v29+)
  -S            Emit a CycloneDX SBOM (JSON) of the managed assemblies
  -v            Verbose debug tracing on stderr
  -z            Enumerate managed string literals (`ldstr`) from metadata
```

Normal symbol recovery needs two inputs but the `-D` will automatically resolve it for you.

1. The native IL2CPP binary:
   - iOS: `UnityFramework`
   - Android: `libil2cpp.so`
   - macOS: `GameAssembly.dylib`
   - Windows: `GameAssembly.dll`
   - Linux: `GameAssembly.so`
2. The matching `global-metadata.dat`

## radare2 Plugin

After installing the plugin, open a Unity binary in r2 and use:

```console
r2unity-i      metadata summary
r2unity-ij     metadata summary as JSON
r2unity-s      apply managed method flags/comments to the current r2 session
```

The commands suffixed with `*` will print r2 commands, so you can run them by prefixing it with a dot `.`:

```text
.r2unity-R*
```

Plugin configuration variables:

```text
r2unity.metadata    path to global-metadata.dat
r2unity.library     path to IL2CPP native library
```

If either variable is empty, the plugin tries to detect it from the loaded binary path using the `-D` approach.


## Known Limitations

- `-a` / `-c` are wired into the CLI, but `r2unity_read_method_pointers_at()` is still a stub.
- Method-pointer discovery is heuristic and can lock onto a wrong count-prefixed table on unfamiliar binaries.
- No full `Il2CppCodeRegistration` or `Il2CppMetadataRegistration` structural recovery exists yet.
- P/Invoke currently identifies managed extern methods, but does not decode `DllImportAttribute` DLL names, entry-point names, charset, calling convention, or `SetLastError`.
- Reverse-P/Invoke currently identifies annotated v29+ managed methods, but does not recover native wrapper VAs from `reversePInvokeWrappers`.
- Pre-v29 reverse-P/Invoke attribute argument recovery is not implemented; those values live in native custom-attribute generator thunks.
- Interop wrapper recovery from `Il2CppInteropData` is not implemented.
- SBOM output is managed-metadata-only. It does not enumerate native dependencies, imports, build IDs, UUIDs, PDB GUIDs, code signatures, toolchain data, or file hashes.
- Field, parameter, property, event, generic, interface, vtable, RGCTX, default-value, marshaling, and field-offset tables are declared in the header but not exposed by the CLI/plugin yet.
- Type signatures and parameter names are not printed yet, so methods are named with an argument count rather than a full C# signature.
- WebAssembly is unsupported.
- The plugin and CLI SBOM paths are similar but not identical; the CLI currently emits more assembly properties.

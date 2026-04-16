# r2unity

A lightweight C tool and (upcoming) radare2 core plugin that parses Unity's `global-metadata.dat` and annotates the matching IL2CPP-compiled binary with class, method, and field information.

## Motivation

Unity with the IL2CPP backend compiles C# to native code. The symbolic information (namespaces, classes, method names, tokens) is stripped from the native binary and kept in a separate file called `global-metadata.dat`. Without it, the binary is a blob of anonymous functions.

**r2unity** reads the `.dat`, locates the method pointer table inside the native binary, and emits radare2 commands that flag functions, add comments, and apply types — directly in your r2 session, with no runtime dependencies.

Inspired by [Il2CppDumper](https://github.com/Perfare/Il2CppDumper) and [lib-global-metadata](https://github.com/axstin/lib-global-metadata), but focused on tight integration with [radare2](https://github.com/radareorg/radare2).

## What r2unity needs

Exactly **two files**:

1. The **IL2CPP native binary** (this is what you open in radare2):
   - Android: `libil2cpp.so`
   - iOS: `UnityFramework` (a Mach-O inside `UnityFramework.framework`)
   - Windows: `GameAssembly.dll`
   - macOS: `GameAssembly.dylib`
2. The matching **`global-metadata.dat`**.

The **main application executable is NOT needed**:
- On iOS, `Payload/<App>.app/<App>` is a ~50–100 KB thin launcher whose only job is to load `UnityFramework.framework`. All IL2CPP code lives in `UnityFramework` (tens of MB).
- On Android, `lib/<arch>/libmain.so` is a small shim (a few KB). All IL2CPP code lives in `libil2cpp.so`.

So you only pass r2unity the Unity binary and the `.dat` — never the launcher.

## Where to find the files

### Android (`.apk` / `.apks` / `.aab`)

An APK is a zip. Extract (or browse with `r2 apk://…`) and look at:

```
<apk-root>/
├── lib/
│   ├── arm64-v8a/
│   │   └── libil2cpp.so            ← open this in radare2
│   └── armeabi-v7a/
│       └── libil2cpp.so            ← or this, for 32-bit
└── assets/
    └── bin/
        └── Data/
            └── Managed/
                └── Metadata/
                    └── global-metadata.dat   ← pass this to r2unity
```

Pick the `libil2cpp.so` for the architecture you care about (typically `arm64-v8a`).

### iOS (`.ipa`)

An IPA is also a zip. Inside:

```
Payload/
└── <AppName>.app/
    ├── <AppName>                                   ← thin launcher (NOT needed)
    ├── Frameworks/
    │   └── UnityFramework.framework/
    │       └── UnityFramework                      ← open this in radare2
    └── Data/
        └── Managed/
            └── Metadata/
                └── global-metadata.dat             ← pass this to r2unity
```

Note that on iOS the metadata lives under `<AppName>.app/Data/…`, **not** under the `.framework/` directory.

### Windows / desktop builds

```
<BuildRoot>/
├── GameAssembly.dll                                ← the IL2CPP binary (Windows)
└── <AppName>_Data/
    └── il2cpp_data/
        └── Metadata/
            └── global-metadata.dat
```

macOS standalone builds use `GameAssembly.dylib` inside the `.app` bundle, with metadata at `<App>.app/Contents/Resources/Data/il2cpp_data/Metadata/global-metadata.dat`.

## Automatic metadata discovery

The (planned) radare2 core plugin locates `global-metadata.dat` automatically by walking upward from the directory of the currently loaded binary and probing the well-known relative paths above:

| Loaded binary | Candidate `global-metadata.dat` paths tried (relative to binary) |
| --- | --- |
| `…/lib/<arch>/libil2cpp.so` (Android) | `../../assets/bin/Data/Managed/Metadata/global-metadata.dat` |
| `…/UnityFramework.framework/UnityFramework` (iOS) | `../../Data/Managed/Metadata/global-metadata.dat` |
| `…/GameAssembly.dll` (Windows) | `./*_Data/il2cpp_data/Metadata/global-metadata.dat` |
| `…/GameAssembly.dylib` (macOS) | `../Resources/Data/il2cpp_data/Metadata/global-metadata.dat` |
| any | `./global-metadata.dat` (user already placed it next to the binary) |

If none of these resolve, the plugin asks the user to supply the path explicitly. The CLI always takes the two paths as arguments — no auto-detection — to keep scripted use predictable.

## Getting started

### Build

```bash
git clone https://github.com/radareorg/r2unity.git
cd r2unity
make
```

Requires a C compiler and `pkg-config` with radare2 dev headers installed (`r_util`, `r_core`).

### Usage (CLI)

```bash
# Generate an r2 script from the Unity binary + metadata:
./r2unity path/to/libil2cpp.so path/to/global-metadata.dat > symbols.r2

# Apply the script when opening the binary in radare2:
r2 -i symbols.r2 path/to/libil2cpp.so
```

On iOS, swap `libil2cpp.so` for `…/UnityFramework.framework/UnityFramework`.

Useful flags:

- `-j` — emit a single-line JSON status report (used by checks).
- `-q` — quiet, omit the `#` header comments.
- `-f` — fast path: auto-detect ELF/Mach-O and scan for the method pointer table.
- `-a 0xADDR -c N` — read N method pointers starting at address `0xADDR` inside the binary.
- `-l N` — limit number of emitted entries.
- `-v` — verbose debug output.

### Example output

```
'@0x1a2b3c'f sym.unity.Assembly-CSharp.dll.Player.Update(0)
'@0x1a2b3c'CCu Method: [Assembly-CSharp.dll] public Player.Update(0)
'@0x2b3c4d'f sym.unity.mscorlib.dll.System.String.Concat(2)
...
```




## Philosophy

- **Do one thing well** — turn Unity metadata into radare2 commands.
- **Native and dependency-free** — plain C, no .NET or Python runtime.
- **Integrate, don't convert** — output is directly consumable by r2, not a detour through JSON or C# headers.

## Acknowledgments

- [Il2CppDumper](https://github.com/Perfare/Il2CppDumper) — the reference implementation for Unity IL2CPP reversing.
- [lib-global-metadata](https://github.com/axstin/lib-global-metadata) — clean reference for metadata parsing.
- The [radare2](https://github.com/radareorg/radare2) community.

## License

MIT. See [LICENSE](LICENSE).

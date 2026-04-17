# r2unity — Internal Technical Documentation

This document describes the internal design of r2unity, how the current
implementation works, what each piece of source code is responsible for,
and what is still missing for full portability across iOS and Android
builds produced by different Unity versions.

It complements the top level `README.md`, `AGENTS.md`, `INFO.md`,
`INFO2.md` and `RVA.md` notes — those cover the *why* and the *file
formats*; this file covers the *how* and the *gaps*.

---

## 1. High level architecture

r2unity is split into three deliverables:

1. **`src/lib/`** — a small static library in C that parses
   `global-metadata.dat` and scans a native IL2CPP binary (ELF or
   Mach-O) to recover the per-method pointer table.
2. **`src/main.c`** — a CLI entry point that glues the library to a
   radare2 script generator. This is what the `Makefile` currently
   produces as `./r2unity`.
3. **radare2 core plugin** *(planned, not wired into the build yet)*
   — will consume the same library plus `r_core` to register flags,
   comments and types directly in the running r2 session, and will
   auto-discover `global-metadata.dat` from the loaded binary's path.

The library links only against `r_util` (via `pkg-config r_util`) — no
`r_bin`, no `r_core`. The ELF and Mach-O parsers in `src/lib/elf.c` and
`src/lib/macho.c` are intentionally self-contained so the CLI stays
small and can be embedded later into the plugin without pulling the
full radare2 core.

```
   +---------------------+         +-----------------------+
   | global-metadata.dat |         | libil2cpp.so / .dylib |
   +----------+----------+         +-----------+-----------+
              |                                |
              v                                v
   +---------------------+         +-----------------------+
   | src/lib/lib.c       |         | src/lib/elf.c         |
   |  - header parsing   |         | src/lib/macho.c       |
   |  - strings          |         |  - segment walk       |
   |  - type/method tbls |<--------+  - relocation apply   |
   |  - images           |         |  - CodeRegistration / |
   +---------+-----------+         |    method-ptr scan    |
             |                     +-----------+-----------+
             +-------------+-------------------+
                           v
                  +-----------------+        +------------------+
                  | src/main.c      |        | r2 core plugin   |
                  |  CLI (-j -q -f) |        | (not yet built)  |
                  +--------+--------+        +---------+--------+
                           v                           v
                   r2 command script          r_flag / r_anal / r_meta
```

---

## 1.1. Files

| OS / target | Native file required | Metadata file required | Typical metadata location relative to binary | Current status in docs |
|---|---|---|---|---|
| Android | libil2cpp.so | global-metadata.dat | ../../assets/bin/Data/Managed/Metadata/global-metadata.dat | Main supported target; arm64 works best (doc/r2unity.md:419, doc/r2unity.md:469) |
| iOS | UnityFramework | global-metadata.dat | ../../Data/Managed/Metadata/global-metadata.dat | Main supported target for ARM64 Mach-O (doc/r2unity.md:389, doc/r2unity.md:470) |
| macOS standalone | GameAssembly.dylib | global-metadata.dat | ../Resources/Data/il2cpp_data/Metadata/global- metadata.dat | Mach-O path technically works, but CLI auto-discovery is not wired yet (doc/r2unity.md:447,
| Windows standalone | GameAssembly.dll | global-metadata.dat | *_Data/il2cpp_data/Metadata/global- metadata.dat | Good |

## 2. `global-metadata.dat` parser (`src/lib/lib.c`, `src/lib/lib.h`)

### 2.1 Header versions

Three C-level header structures are declared in `lib.h`:
`Il2CppGlobalMetadataHeader_v24`, `_v27` and `_v29`. They differ by the
trailing fields:

- **v24** — baseline fields up to `exportedTypeDefinitions`.
- **v27** — adds `rgctxEntries{Offset,Size}`.
- **v29** — adds `rgctxEntriesData{Offset,Size}`.

They are stored in a `union Il2CppGlobalMetadataHeader` and the selected
variant is picked at parse time:

```c
if      (version < 27) header_size = sizeof (v24);
else if (version < 29) header_size = sizeof (v27);
else                   header_size = sizeof (v29);   // 29, 30, 31
```

Metadata version is read from the second 32-bit word of the file
(the first is the `0xFAB11BAF` magic). Only versions `[24, 31]` are
accepted; anything outside that range makes `r2unity_parse_metadata`
return `NULL`.

Although three header layouts exist, `R2UnityMetadata` keeps a set of
*normalized* fields that are copied out of whichever variant was
selected:

```c
meta->stringOffset        meta->stringSize
meta->stringLiteralOffset meta->stringLiteralSize
meta->methodsOffset       meta->methodsSize
meta->typeDefinitionsOffset  meta->typeDefinitionsSize
```

The rest of the library only touches these normalized fields, which
keeps version-specific knowledge localized to the parse function.

### 2.2 String table

`r2unity_get_string (meta, index)` returns a `const char *` (actually an
independently malloc'd copy — callers `free()` it) for a given string
index. Two notable details:

- The string table is mapped as an `RBuffer` slice (`meta->strings`)
  via `r_buf_new_slice`, so nothing is copied eagerly.
- If `index` happens to point into the middle of a string (which does
  happen with some metadata payloads), the reader walks *backwards*
  until it hits a null byte, then reads forward until the next null.
  This "realign to start of string" heuristic covers malformed
  `nameIndex` values seen in the wild.

There is a second string region — `stringLiteralOffset` — used by
`ldstr` opcodes. It is exposed as `meta->string_literals` but not yet
consumed by the CLI.

### 2.3 Type / method / image definitions

Three on-disk record types are parsed manually (not via `memcpy` of a
packed struct) to stay endian- and alignment-safe:

| Table | Entry size | Decoder |
| --- | ---: | --- |
| `Il2CppTypeDefinition`   | 88 bytes | `r2unity_get_type_definitions` |
| `Il2CppMethodDefinition` | 32 bytes | `r2unity_get_method_definitions` |
| `Il2CppImageDefinition`  | 40 bytes | `r2unity_get_images` |

Each row is decoded with `RD_LE32` / `RD_LE16` helpers, so the parser
works identically on LE and BE hosts. The returned arrays are
heap-allocated and owned by the caller.

The fixed entry sizes assume the *current* layout used by Unity 2019+
IL2CPP (metadata versions 24…31). This is one of the main portability
risks — see §7.

### 2.4 Ownership and lifetime

- `RBuffer *buf` passed to `r2unity_parse_metadata` is retained by the
  metadata (not `r_ref`'d itself; the caller must `r_unref` the outer
  buffer after freeing the metadata).
- `meta->strings` and `meta->string_literals` are slices — they are
  `r_unref`'d by `r2unity_free_metadata`.
- Type/method/image arrays are pulled into plain C arrays so that
  downstream iteration does not need to deal with `RBuffer` at all.

### 2.5 Debug switch

A single file-static boolean `g_debug`, controlled by
`r2unity_set_debug(bool)`, gates `[r2unity]` / `[r2unity/elf]` /
`[r2unity/macho]` trace lines on `stderr`. The CLI wires `-v` to this.

---

## 3. ELF parser (`src/lib/elf.c`)

Purpose: open an Android `libil2cpp.so`, resolve a method-pointer
table, and return an array of absolute virtual addresses the size of
`methodsSize / sizeof(Il2CppMethodDefinition)`.

### 3.1 Loading

`elf_load` slurps the entire file with `open`/`read`, then walks the
program header table:

- 32-bit ELF (`EI_CLASS == 1`) and 64-bit ELF (`EI_CLASS == 2`) are
  both supported.
- Only little-endian (`EI_DATA == 1`) ELF is accepted.
- Segments of type `PT_LOAD` (1) and `PT_DYNAMIC` (2) are captured into
  a fixed 128-entry table; everything else is dropped.
- `base_vaddr` is computed as `min(p_vaddr over PT_LOAD)`. Code
  ("text") range is the union of `PT_LOAD` segments that are both
  readable and executable (`PF_X | PF_R`).

### 3.2 Dynamic relocation fixup

Modern Android NDK toolchains emit `DT_RELA`, `DT_REL`, and/or
`DT_RELR`. The IL2CPP method pointer table is typically emitted as an
array of `R_AARCH64_RELATIVE` (type 1027) or `R_*_RELATIVE` (type 8 /
type 23 on x86_64 / ARM32), so before scanning the data segments the
parser iterates all three relocation sections and patches the in-memory
image:

- `DT_RELA`: `newv = base_vaddr + r_addend`
- `DT_REL`:  `newv = base_vaddr + *loc`
- `DT_RELR` (64-bit only): bitmap-encoded relative relocations, applied
  to each location implied by the bitmap.

Only *relative* relocation types are applied. Symbolic / PLT / GOT
relocations are intentionally ignored — they cannot resolve without a
dynamic linker, and they are not needed for the method pointer arrays.

The dynamic tag set consumed:

| Tag | Meaning |
| ---:| --- |
| 7   | `DT_RELA`      |
| 8   | `DT_RELASZ`    |
| 9   | `DT_RELAENT`   |
| 17  | `DT_REL`       |
| 18  | `DT_RELSZ`     |
| 19  | `DT_RELENT`    |
| 35  | `DT_RELRSZ`    |
| 36  | `DT_RELR`      |
| 37  | `DT_RELRENT`   |

### 3.3 Method-pointer discovery

Two passes run over every `PF_W`-bearing data segment:

1. **CodeRegistration-shaped pair** — looks for a `{count32, pad,
   pointer}` tuple where `count >= 32` and a sample of 128 entries at
   `*pointer[]` is dominated by values that land in the text range.
2. **Generic `{count32, pointer}` record** — less strict, used when the
   CodeRegistration shape is not found. Uses a *window*
   `expected/2 ≤ count ≤ expected*2` around
   `methodsSize/sizeof(Il2CppMethodDefinition)` to avoid locking onto a
   different count-prefixed array (string table, type table, etc.).

The "good" heuristic counts entries that either already fall inside
`[text_lo, text_hi)` or that fall inside `[text_lo, text_hi)` after
adding `base_vaddr`. This covers both pre-relocated images (where the
table already contains absolute VAs) and raw RVAs (where it stores
offsets relative to the module base). Once a candidate array is
accepted, all entries are normalized to absolute VAs in the output.

At least 8 entries must land in text for the table to be accepted —
this prevents false positives on small, count-prefixed arrays that
happen to alias the heuristic.

---

## 4. Mach-O parser (`src/lib/macho.c`)

Purpose: same as §3, but for iOS `UnityFramework` and macOS
`GameAssembly.dylib`.

### 4.1 Loading

- Accepts thin `MH_MAGIC_64` (`0xFEEDFACF`) directly.
- Accepts FAT (`0xCAFEBABE`, big-endian header) and selects the first
  ARM64 slice (`cputype == 0x0100000c`), otherwise the first slice.
  **Only one ARM64 slice is dumped** — cross-slice iteration is not
  yet implemented.
- Only 64-bit Mach-O is supported (`mh_magic != 0xFEEDFACF` is a hard
  error). 32-bit (`0xFEEDFACE`) and the little-endian FAT
  `0xBEBAFECA` variant fall through.
- Walks `LC_SEGMENT_64` (`0x19`) commands, capturing `segname`,
  `vmaddr`, `vmsize`, `fileoff`, `filesize`, `maxprot` for up to 128
  segments.
- `vm_base = min(vmaddr over segments)`.

### 4.2 Text range

`macho_vm_in_text` computes `[text_lo, text_hi)` as the union of
executable segments (`maxprot & VM_PROT_EXECUTE` or segname
`__TEXT`). When no executable segment is detected it falls back to the
union of *all* segments — this is a safety net for unusual Mach-O
layouts but normally isn't triggered.

### 4.3 Method-pointer discovery

Same two-pass approach as the ELF scanner: CodeRegistration shape
first, generic `{count, ptr}` second, with identical heuristics. Only
8-byte pointers are considered (`ptrsz = 8`).

Mach-O does *not* have dynamic relative relocations that need to be
applied manually — the linker already baked values in, modulo the
`vm_base` offset which the discovery loop accounts for.

---

## 5. CLI entry point (`src/main.c`)

### 5.1 Flag surface

```
-q       Quiet — omit "# r2 script…" banner
-f       Fast path: auto-detect ELF/Mach-O, scan for ptr table
-l N     Limit emitted entries to N
-a 0xA   Read pointer table starting at VA 0xA
-c N     ... for N entries (pair with -a)
-v       Verbose / debug
```

The positional arguments are `<executable> <global-metadata.dat>`.

### 5.2 Output format (radare2 script)

For every method the CLI emits two lines using the r2 "temporary
seek" prefix `'@0xADDR'`:

```
'@0x1a2b3c'f sym.unity.<image>.<Namespace.Class.Method(N)>
'@0x1a2b3c'CCu Method: [<image>] <attrs> <Namespace.Class.Method(N)>
```

- `f` defines a flag.
- `CCu` sets a *unique* per-address comment.
- `sym.unity.` namespacing keeps Unity symbols separate from whatever
  radare2 already detected (e.g. exported symbols).
- `r_name_filter(fullname, -1)` sanitizes the flag name so it is safe
  to use in r2 (no spaces, dots, parens).
- Method attributes (`public`, `private`, `static`, `virtual`, etc.)
  are computed from `flags` in `build_method_attrs_string` using the
  subset of `System.Reflection.MethodAttributes` that is meaningful.

The assembly/image name is looked up via a `type2img[typeIndex]` map
built once from `Il2CppImageDefinition.typeStart` /`typeCount`.

### 5.3 `-j` JSON status

and friends:

```json
{"ok":true,"version":29,"types":41234,"methods":123456,"has_ptrs":true}
```

`has_ptrs` is `true` when *any* non-zero pointer made it into the
array — this is the closest we can get to "did the scan work" without
running the full r2 session.

### 5.4 Address mapping

The mapping `method index j → pointer[j + mp_shift]` is kept 1:1
(`mp_shift = 0`). This matches the upstream invariant that
`g_MethodPointers[i]` corresponds to method definition *i*, with
zeros for abstract/generic/external methods.

Only pointers with `addr > 0x1000` are emitted — a trivial way to skip
the zero-padded slots.

---



- `cli-gmp` — smoke check against a generic `global-metadata.dat`.
- `json-one-line` — validates `-j` emits exactly one JSON line.
- `json-unity` — stricter JSON shape + content check.


---

## 7. Portability status

The implementation is working end-to-end on the two main targets
(Android arm64 ELF, iOS arm64 Mach-O), but there are several known
gaps before we can claim broad coverage.

### 7.1 Metadata versions

| Version | Header struct | Status |
| ------: | ------------- | ------ |
| 16–23   | —             | **Not supported.** Rejected by `r2unity_parse_metadata`. |
| 24      | `v24`         | Supported. |
| 27      | `v27`         | Supported (RGCTX offsets parsed but unused). |
| 27.1/.2 | `v27`         | Assumed compatible — not explicitly validated. |
| 29      | `v29`         | Supported. |
| 30, 31  | `v29`         | **Treated as v29-compatible.** Any layout change in these versions will silently produce wrong offsets. |


**TODO:** decode the `token` fields — they carry the ECMA-335
metadata token which is useful for cross-referencing with managed
DLLs.

### 7.2 On-disk entry sizes are hard-coded

`r2unity_get_type_definitions`, `r2unity_get_method_definitions` and
`r2unity_get_images` use literal byte sizes (88, 32, 40). These match
metadata versions 24–31 *today* but will break the moment Unity adds
or removes a field. Detection should be derived from
`(sectionSize / entryCount)` or from version-gated constants declared
next to the header structs. This is the single biggest source of
silent corruption risk.

### 7.3 iOS (Mach-O)

Working:
- ARM64 thin Mach-O (`0xFEEDFACF`).
- ARM64 slice of FAT Mach-O (first ARM64 slice selected).

Known gaps:
- **No ARM64e slice selection** — `cputype` match only checks
  `0x0100000c` (CPU_TYPE_ARM64). ARM64e (with `CPU_SUBTYPE_ARM64E`) is
  the same `cputype`, so it works by accident, but we don't expose a
  way to pick it explicitly if both slices coexist.
- **No 32-bit ARMv7 Mach-O.** Unity hasn't shipped 32-bit iOS since
- **No chained fixups / ARM64e pointer authentication handling.**
  Recent Xcode produces `LC_DYLD_CHAINED_FIXUPS` (`0x34`) instead of
  classic rebase opcodes. For the specific tables we scan this is not
  a problem because values land in the text range either way, but if
  we ever need to follow pointers *inside* the chained-fixup region we
  will need to parse the chain.
- **No `LC_DYLD_INFO{,_ONLY}` rebase opcode interpretation.** Same
  reasoning as above; not blocking today, will matter if pointer
  scanning shifts to resolving `CodeRegistration` more precisely.
- **Entitlements / encrypted segments (`LC_ENCRYPTION_INFO_64`)** —
  commercial `.ipa` from the App Store are FairPlay-encrypted until
  decrypted on a jailbroken device. r2unity currently has no check for
  this; scanning will silently return garbage on encrypted binaries.
  We should detect the load command and emit a warning.
- **`UnityFramework.framework` vs `GameAssembly.dylib`** — the parser
  treats both the same way. That is correct; just noting it.

### 7.4 Android (ELF)

Working:
- 64-bit arm64 (`aarch64`) `libil2cpp.so` with `DT_RELA` and/or
  `DT_RELR`.
- 32-bit ELF headers are parsed, but…

Known gaps:
- **ARMv7 (`armeabi-v7a`) `libil2cpp.so` is parsed but rarely
  validated.** The relocation-apply path supports `R_ARM_RELATIVE`
  quirk (e.g. `DT_ANDROID_RELA`) will go unnoticed.
- **`DT_ANDROID_REL{A,R}` / packed relocations** (`DT_ANDROID_RELA =
  0x60000010`, `DT_ANDROID_RELASZ = 0x60000011`) are **not handled**.
  Play-Store builds compressed with `androidx.sqlite`'s packed-relocs
  format will read as "no relocations" and the method pointer array
  will look unrelocated. This is the biggest Android gap — a real
- **x86 / x86_64 Android emulator builds** have relocation types
  (`R_X86_64_RELATIVE = 8`, `R_386_RELATIVE = 8`) that the matching
- **No symbol version (`DT_VERSYM`/`DT_VERNEED`) handling.** Not
  needed for the method pointer scan but would help identify the Unity
  version from the `.so` directly.
- The ELF loader slurps the entire file with `read()` — fine for a
  ~40 MB `libil2cpp.so`, but not ideal. Should move to `mmap` once
  the plugin lands inside r2 and inherits `r_core`'s IO layer.

### 7.5 Windows / macOS standalone

Unsupported today:
- `GameAssembly.dll` (Windows PE) — no PE parser.
- `GameAssembly.dylib` (macOS standalone) — technically works via the
  Mach-O path, but the metadata auto-discovery relative paths listed
  in `README.md` aren't wired into the CLI (only the plugin will use
  them).

**TODO:** add a minimal PE scanner in `src/lib/pe.c`. It only needs to
enumerate sections, pick `.data` / `.rdata`, and reuse the same
`{count, ptr}` heuristic. No relocations need to be applied — PE
`.dll`s are already relocated at link time (image base is concrete).

### 7.6 Auto-detection & plugin integration

`src/main.c:-f` detects the file by its first 4 bytes. This is a
minimal heuristic and should be moved, together with the metadata path
discovery described in `README.md`, into the core plugin. Specifically:

- Plugin registers as an `r_core` plugin and hooks file-open.
- On open, walk upward from the binary path probing the well-known
  metadata locations (Android `../../assets/bin/Data/Managed/Metadata/`,
  iOS `../../Data/Managed/Metadata/`, Windows `*_Data/il2cpp_data/…`,
  macOS `../Resources/Data/il2cpp_data/…`, and a final fallback of
  `./global-metadata.dat`).
- Replace the printf'd command script with direct `r_flag_set` /
  `r_meta_set` / `r_anal_function_add` calls — faster and avoids
  shelling a second r2 process.

### 7.7 CodeRegistration / MetadataRegistration

Currently the discovery heuristic locks onto the *method pointer*
array directly. A more robust approach, described in `RVA.md`, would
be to locate `Il2CppCodeRegistration` and `Il2CppMetadataRegistration`
by pattern-matching expected counts (methodPointersCount must equal
`methodsSize/sizeof(Il2CppMethodDefinition)`, etc.) and then pulling
all pointer arrays (`g_MethodPointers`, `g_InvokerPointers`,
`g_FieldOffsetTable`, …) from a single anchor. This would also give
us:

- Field offsets (not exposed at all today).
- VTables.
- Invoker pointers → better demangling of generic method
  instantiations.
- Reverse P/Invoke wrappers.

**TODO:** implement `CodeRegistration` / `MetadataRegistration`
anchor detection and expose the full symbol set, not just methods.

### 7.8 Other missing features

- Generic methods and `MethodSpec` expansion (needs both `.dat` and
  the binary's generic instantiation table).
- String literals — `meta->string_literals` is parsed but never
  emitted; we could add `Cs` string annotations at the literal VAs.
- Attributes — `attributesInfo` / `attributeTypes` tables are in the
  header but not decoded.
- Field offsets — requires `Il2CppMetadataRegistration`.
- Proper `pf.` (print format) struct registration for the header,
  type defs and method defs, as suggested in `INFO2.md §4`.
- Windows PE support (see §7.5).
- `ET_EXEC` vs `ET_DYN` handling: we assume PIC shared objects for
  ELF. A statically linked IL2CPP executable would break the
  `base_vaddr` normalization.

---

## 8. Roadmap summary

Short-term, in rough priority order:

1. Handle `DT_ANDROID_RELA` / packed relocations (biggest real-world
   Android gap).
2. Replace hard-coded entry sizes with version-gated constants and add
   still holds.
3. Land the r2 core plugin (auto metadata discovery + native
   `r_flag`/`r_meta` calls).
4. Add PE support for `GameAssembly.dll`.
5. Pattern-match `Il2CppCodeRegistration` so we get field offsets and
   invoker pointers, not just method pointers.
6. Detect FairPlay-encrypted `UnityFramework` and bail out cleanly.
7. Emit string-literal annotations and type-definition struct info.

Longer term:

- Generic instantiations and `MethodSpec` decoding.
- Symbolicating trampolines / adjustors.
  checks.

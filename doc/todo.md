# r2unity TODO

This file tracks implementation work that is still open after the
current metadata parser, method-pointer recovery, and pointer-table notes
in `doc/ptrtables.md`.

## Current Baseline

Implemented or partially implemented today:

- Parse supported `global-metadata.dat` headers and core sections.
- Decode type definitions, method definitions, field definitions, images,
  assemblies, string literals, and several index tables.
- Emit method/class names from metadata.
- Recover method body VAs through native method-pointer tables when
  possible.
- Use symbols or user overrides for `Il2CppCodeRegistration` and
  `Il2CppMetadataRegistration` when available.
- Walk `Il2CppCodeGenModule` method tables when a usable
  `Il2CppCodeRegistration` anchor is available.
- Fall back to a generic `{count, pointer}` section scan for stripped
  binaries.

Still missing:

- Structural native anchor discovery.
- Full `Il2CppMetadataRegistration` parsing.
- Field layout recovery.
- Generic method instantiation recovery.
- Invoker, reverse-P/Invoke wrapper, unresolved-call, and interop-data
  native joins.

## Priority Backlog

1. Add structural registration discovery.

   Find `Il2CppCodeRegistration` and `Il2CppMetadataRegistration` in
   stripped binaries by validating native table shapes against counts
   derived from `global-metadata.dat`. This should replace dependence on
   symbols, manual overrides, and the weak generic section scan whenever
   possible.

2. Harden modern method recovery.

   Modern Unity stores methods per image through `codeGenModules[]`.
   Make the module walker robust across v24.2+ layouts, validate module
   names/counts, handle null entries cleanly, and add more version
   samples. A single flat method-pointer table is only correct for older
   layouts.

3. Implement manual pointer-table reads.

   Wire up `-a` / class emission manual reads so a user can provide a
   method-pointer table VA directly. Detect pointer width, validate the
   table against text ranges, and report confidence/source in CLI and
   plugin output.

4. Parse `Il2CppMetadataRegistration`.

   Decode the native-side companion tables:

   - `types`
   - `genericInsts`
   - `genericClasses`
   - `methodSpecs`
   - `genericMethodTable`
   - `fieldOffsets`
   - `typeDefinitionsSizes`
   - `metadataUsages` for pre-v27 binaries

5. Recover class and struct field layouts.

   Join metadata field rows with native `fieldOffsets` and
   `typeDefinitionsSizes`. Emit class/struct layouts, field offsets, type
   names, static/thread-static sizes, and enough data to label
   `[this + offset]` accesses in r2.

6. Recover generic method instantiations.

   Join:

   ```text
   methodSpecs
   genericMethodTable
   genericMethodPointers
   genericAdjustorThunks
   genericInsts
   ```

   Use this to name concrete generic bodies such as
   `List<int>.Add` or `Foo<string>.Bar<float>`.

7. Attach reverse-P/Invoke wrapper VAs.

   Decode module `reversePInvokeWrapperIndices[]`, join those indices to
   `CodeRegistration.reversePInvokeWrappers[]`, and attach wrapper VAs
   to P/Invoke/reverse-P/Invoke output.

8. Decode invoker and unresolved-call tables.

   Parse `invokerPointers`, module `invokerIndices[]`, unresolved
   virtual call tables, and v29.1+ instance/static unresolved call
   tables. Use metadata range/signature information to name dispatch
   stubs.

9. Parse `Il2CppInteropData`.

   Recover P/Invoke marshal wrappers, delegate wrappers, COM bridges,
   GUID/type associations, and native wrapper VAs from `interopData[]`.

10. Decode custom attributes fully.

    - v29+: decode inline custom-attribute BLOBs, especially
      `DllImportAttribute` DLL and entry-point names.
    - pre-v29: recover constructor arguments by interpreting
      `customAttributeGenerators` thunks.

11. Finish higher-level metadata tables.

    Add or improve decoding for:

    - parameters and full method signatures
    - default values for fields and parameters
    - properties and events
    - nested types
    - interfaces and interface offsets
    - vtable method tables
    - RGCTX ranges and entries
    - generic containers, generic parameters, and constraints

12. Expand version and architecture coverage.

    Add fixtures and tests for:

    - v24.0, v24.1, v24.2, v24.5
    - v27, v27.1, v27.2
    - v29 and v29.1
    - v31
    - v38/v39 compact-index metadata
    - 32-bit ARM/x86
    - relocation-heavy ELF/Mach-O/PE samples
    - Mach-O FAT and arm64e/pointer-authentication edge cases

    Native walkers must be pointer-width aware and version-gated instead
    of using one hardcoded registration layout.

## SBOM Work

- Add SHA-256 hashes for the executable and metadata file.
- Add native dependency extraction:
  - ELF `DT_NEEDED`, `DT_SONAME`, build ID, Android notes.
  - Mach-O dylib load commands, UUID, build version, code-signature
    summary.
  - PE imports, CodeView/PDB GUID, timestamp, Rich header.
- Add a CycloneDX `serialNumber`.
- Bring plugin SBOM properties up to parity with CLI SBOM output.

## Plugin and UX Work

- Add plugin commands for field/type browsing once native layout recovery
  exposes those records.
- Make plugin symbol application report skipped methods and pointer-source
  confidence.
- Keep CLI and plugin JSON schemas aligned.
- Surface partial recovery clearly: global old-style table, per-module
  modern table, heuristic single-table hit, manual table, or no native
  table.

## Reference Docs

- `doc/ptrtables.md` - native pointer-table inventory and recovery model.
- `doc/r2unity.md` - internal technical reference.
- `doc/future.md` - version/layout matrix and row-layout deltas.
- `doc/datvsbin.md` - metadata-vs-binary split.
- `doc/strings.md` - managed string-literal mode.
- `doc/sbom.md` - managed SBOM behavior and missing native SBOM work.
- `doc/pinvoke.md` - interop enumeration behavior and missing recovery
  work.
- `INFO.md` - high-level explanation of what metadata can and cannot
  provide alone.
- `INFO2.md` - implementation-oriented metadata parsing guide.
- `RVA.md` - mapping metadata indices to native addresses.

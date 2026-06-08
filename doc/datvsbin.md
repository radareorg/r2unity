# IL2CPP Metadata vs Native Binary

Unity IL2CPP builds split managed-code information across two files:

- `global-metadata.dat`
- the native IL2CPP binary (`GameAssembly.dll`, `GameAssembly.so`,
  `GameAssembly.dylib`, `libil2cpp.so`, `UnityFramework`, etc.)

They must be read together to recover named native symbols and runtime
layouts.

## `global-metadata.dat`

`global-metadata.dat` is the managed/logical metadata blob. It is
platform-independent for a given IL2CPP run: it stores byte offsets and
dense table indices, not native pointers.

Typical contents:

- metadata magic, version, and table-of-contents header
- assemblies and images
- type definitions for classes, structs, enums, and interfaces
- method definitions: names, signatures, parameters, return types,
  tokens, and method indices
- field definitions
- properties and events
- nested-type, interface, and vtable index tables
- generic containers, generic parameters, and method specs
- custom-attribute and default-value blobs, depending on metadata
  version
- identifier strings and managed `ldstr` literal payloads

It does not contain:

- native method bodies
- method RVAs or function addresses
- final field offsets
- final type sizes
- native vtables, invokers, wrappers, or trampolines
- Unity asset, scene, prefab, or AssetBundle data

In short, the `.dat` file says what managed things exist and how they
are named, typed, and indexed.

## Native IL2CPP binary

The native binary is the physical/runtime side of the same build. It
contains the compiled machine code and the registration structures that
tie the generated code back to metadata indices.

Important native-side data:

- compiled method bodies
- `Il2CppCodeRegistration`
- `Il2CppMetadataRegistration`
- `methodPointers`
- `genericMethodPointers`
- `reversePInvokeWrappers`
- `invokerPointers`
- `codeGenModules`
- `fieldOffsets`
- `typeDefinitionsSizes`
- native type and generic-instantiation tables
- runtime helper code, wrappers, trampolines, and runtime strings

These tables provide the addresses and runtime layouts that
`global-metadata.dat` intentionally does not carry. `doc/ptrtables.md`
is the detailed native pointer-table reference.

## Mapping model

The two files are correlated by indices:

```text
global-metadata.dat              native IL2CPP binary
-------------------              --------------------
MethodDefinition.methodIndex --> CodeRegistration / CodeGenModule methodPointers[index]
TypeDefinition index          --> MetadataRegistration.fieldOffsets[type]
Type/generic indices          --> MetadataRegistration.types and generic tables
```

For r2unity this means:

- parsing only `global-metadata.dat` can recover names, signatures,
  tokens, string literals, and table relationships
- resolving those names to native addresses requires locating
  `Il2CppCodeRegistration` in the binary; r2unity accepts an explicit
  address, an r2 flag/r_bin symbol, or the r_bin/simple-parser
  section-scan fallback
- recovering field offsets and final type sizes requires
  `Il2CppMetadataRegistration`
- a complete symbol map needs both files from the same build

Today r2unity takes managed structure from `.dat` and native addresses
from the binary:

- `.dat`: image/type/method rows, method indices, names, tokens,
  strings, assemblies, P/Invoke metadata, and reverse-P/Invoke
  attribute metadata
- binary: executable address ranges, native symbols/flags for
  `g_CodeRegistration` and `g_MetadataRegistration`, and method pointer
  tables reached from CodeRegistration or the r_bin/simple-parser
  fallback scan

The current method-address path only needs `g_CodeRegistration`.
`g_MetadataRegistration` is tracked as the companion anchor for native
layout work such as field offsets and type sizes.

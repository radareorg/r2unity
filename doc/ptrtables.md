# IL2CPP Native Pointer Tables

Unity IL2CPP builds split information across two sides:

- `global-metadata.dat`: logical metadata tables, names, tokens, type
  definitions, method definitions, field definitions, generic metadata,
  and dense indices.
- The native IL2CPP binary: compiled code, registration structures,
  function-pointer tables, layout tables, and runtime helper data.

The metadata file does not contain real code addresses. Native addresses
come from tables reachable from `Il2CppCodeRegistration` and
`Il2CppMetadataRegistration`, then are joined back to metadata by row
indices and per-image/per-type ranges.

## What r2unity Resolves Today

The current native recovery code resolves method bodies only:

- It may find or accept `Il2CppCodeRegistration`.
- It may find or accept `Il2CppMetadataRegistration`, but only reports
  and caches that VA today.
- It copies recovered method body addresses into
  `R2UnityNativeResult.method_ptrs[]`.
- It reports the source as `symbol`, `override`, or `heuristic`.

The current code does not yet parse the rest of
`Il2CppMetadataRegistration`:

- field offsets
- type sizes
- `Il2CppType` pool
- metadata usages
- generic method tables
- invoker pointers
- reverse-P/Invoke wrappers
- `Il2CppInteropData`

Those tables are documented below because they are the natural next
native-recovery targets.

## Minimal Binary Knowledge Required

The heuristic carver does not need a full semantic binary loader, but it
does need a memory view:

- pointer size: 4 or 8 bytes
- readable/data ranges to scan
- executable text range to validate code pointers
- base VA, for rebasing raw RVAs or low addresses
- a `ptr_at(va)` callback that maps a virtual address to file or memory
  bytes

For ELF/Mach-O/PE files, this information usually comes from segments or
sections. With r2 loaded, r2unity asks r_bin for sections, symbols,
relocations, pointer size, and VA mapping. The simple fallback readers
read only enough format data to build the same generic view.

Relocations matter. Many IL2CPP tables are in relocatable data sections,
so raw file bytes can contain unpatched addends instead of final VAs. The
simple ELF path applies common relative relocations before scanning.

## Address Normalization

Native table entries may be absolute VAs or raw values that need the
module base added. r2unity treats a raw code pointer as valid when:

```text
raw is inside executable text
or
base_vaddr + raw is inside executable text
```

Data pointers are accepted when they can be dereferenced through the
current memory view.

This is intentionally conservative: a table candidate must point to
readable data, and sampled entries must point to executable code.

## How Method Indices Come From Metadata

For the supported modern metadata layouts, the method index is the row
ordinal in the decoded `Il2CppMethodDefinition[]` table:

```text
methods[0] -> method index 0
methods[1] -> method index 1
...
methods[n] -> method index n
```

The metadata header gives the method table offset and size. r2unity
decodes each row with a version-gated row width. The array index becomes
the native method-pointer index.

Types give structure over that flat method row array:

```text
Il2CppTypeDefinition.methodStart
Il2CppTypeDefinition.method_count
```

For a type:

```text
for k in 0..method_count-1:
    method_index = methodStart + k
    method = methods[method_index]
```

Images give a larger grouping over type rows:

```text
Il2CppImageDefinition.typeStart
Il2CppImageDefinition.typeCount
```

For modern per-image codegen modules, r2unity matches a native module
name to a metadata image name, walks that image's type range, and scatters
module-local method pointers into the global `method_ptrs[method_index]`
array.

Older metadata rows carried explicit native-side indices such as
`methodIndex`, `invokerIndex`, `delegateWrapperIndex`, and RGCTX ranges.
Those fields disappeared in the v24.1 row shrink. Modern recovery relies
more heavily on row order plus native codegen-module index arrays.

## Top-Level Native Anchors

The two important native anchors are:

```c
typedef struct {
    uintptr_t methodPointersCount;       /* pre-v24.2 only */
    uintptr_t methodPointers;            /* pre-v24.2 only */
    uintptr_t reversePInvokeWrapperCount;
    uintptr_t reversePInvokeWrappers;
    uintptr_t genericMethodPointersCount;
    uintptr_t genericMethodPointers;
    uintptr_t genericAdjustorThunks;     /* version-gated */
    uintptr_t invokerPointersCount;
    uintptr_t invokerPointers;
    uintptr_t customAttributeCount;
    uintptr_t customAttributeGenerators; /* removed in v29 */
    uintptr_t unresolvedVirtualCallCount;
    uintptr_t unresolvedVirtualCallPointers;
    uintptr_t interopDataCount;
    uintptr_t interopData;               /* v23+ */
    uintptr_t windowsRuntimeFactoryCount;
    uintptr_t windowsRuntimeFactoryTable;
    uintptr_t codeGenModulesCount;       /* v24.2+ */
    uintptr_t codeGenModules;            /* v24.2+ */
} Il2CppCodeRegistration;

typedef struct {
    intptr_t  genericClassesCount;
    uintptr_t genericClasses;
    intptr_t  genericInstsCount;
    uintptr_t genericInsts;
    intptr_t  genericMethodTableCount;
    uintptr_t genericMethodTable;
    intptr_t  typesCount;
    uintptr_t types;
    intptr_t  methodSpecsCount;
    uintptr_t methodSpecs;
    intptr_t  fieldOffsetsCount;
    uintptr_t fieldOffsets;
    intptr_t  typeDefinitionsSizesCount;
    uintptr_t typeDefinitionsSizes;
    uintptr_t metadataUsagesCount;
    uintptr_t metadataUsages;
} Il2CppMetadataRegistration;
```

The real struct shape is version-dependent. Parsers should not hardcode
one final layout across Unity versions. Treat these as the conceptual
field families.

## Table Shape Taxonomy

Not every native table is an array of pointers.

Common shapes:

- `void * []`: flat array of function/data pointers.
- `Struct * []`: array of pointers to structs.
- `Struct []`: array of inline structs.
- `int32_t []`: index array into another native or metadata table.
- `int32_t **`: pointer-of-pointers, usually one subarray per type.
- Count/pointer pairs inside a registration struct:
  `{ count, pointer_to_table }`.

The heuristic carver looks only for the count/pointer shape and validates
that the pointed-to table looks like function pointers.

## CodeRegistration Tables

### `methodPointers`

Shape:

```text
void *methodPointers[]
```

Meaning:

- native address of compiled managed method bodies
- slot `i` maps to metadata method row `i` on older/global layouts
- zero entries are normal for abstract methods, interface declarations,
  open generics, some extern/P-Invoke methods, and runtime intrinsics

Version behavior:

- pre-v24.2: global table is directly in `Il2CppCodeRegistration`
- v24.2+: global table disappears; method pointers move into each
  `Il2CppCodeGenModule`

r2unity behavior:

- resolved today
- if `CodeRegistration` is known, it first tries the codegen-module path
  and then the older global method-pointer path
- if no anchor is known, the heuristic scanner may find a single
  `{count, ptr}` table, but on v24.2+ that may be only one image's
  method table

### `codeGenModules`

Shape:

```text
Il2CppCodeGenModule *codeGenModules[]
```

Meaning:

- one module per metadata image/assembly
- introduced in v24.2
- each module owns per-image method pointer tables and several
  per-image index tables

Core fields:

```c
typedef struct {
    uintptr_t moduleName;                    /* char * */
    intptr_t  methodPointerCount;
    uintptr_t methodPointers;                /* void * [] */
    uintptr_t adjustorThunks;                /* void * [], version-gated */
    uintptr_t adjustorThunkCount;
    uintptr_t invokerIndices;                /* int32_t [] per method */
    uintptr_t reversePInvokeWrapperCount;
    uintptr_t reversePInvokeWrapperIndices;  /* int32_t [] */
    intptr_t  rgctxRangesCount;
    uintptr_t rgctxRanges;                   /* range structs */
    intptr_t  rgctxsCount;
    uintptr_t rgctxs;                        /* RGCTX structs */
    uintptr_t debuggerMetadata;
    uintptr_t customAttributeCacheGenerator; /* v27-era */
    uintptr_t moduleInitializer;             /* v27+ */
    uintptr_t staticConstructorTypeIndices;  /* v27+ int32_t [] */
    uintptr_t metadataRegistration;          /* v27+ */
    uintptr_t codeRegistration;              /* v27+ */
} Il2CppCodeGenModule;
```

Mapping model:

```text
moduleName -> metadata image name
image.typeStart/typeCount -> types belonging to this module
type.methodStart/method_count -> method rows
module.methodPointers[local_index] -> method_ptrs[method_index]
```

### `invokerPointers`

Shape:

```text
void *invokerPointers[]
```

Meaning:

- reflection-dispatch invokers
- not one per managed method
- usually one per method-signature/runtime-call shape

Association:

- old layouts: method row carried an `invokerIndex`
- v24.2+: `Il2CppCodeGenModule.invokerIndices[]` has one index per
  module-local method

r2unity behavior:

- not parsed today

### `reversePInvokeWrappers`

Shape:

```text
void *reversePInvokeWrappers[]
```

Meaning:

- native-to-managed trampoline stubs for callbacks
- relevant to `[MonoPInvokeCallback]`, unmanaged callbacks, native SDK
  hooks, audio callbacks, analytics callbacks, and similar surfaces

Association:

- old layouts had explicit wrapper index fields in method metadata rows
- v24.2+: `Il2CppCodeGenModule.reversePInvokeWrapperIndices[]` has one
  index per method; non-negative entries index the global wrapper table

r2unity behavior:

- metadata-only reverse-P/Invoke enumeration exists
- wrapper VAs are not attached yet

### `genericMethodPointers`

Shape:

```text
void *genericMethodPointers[]
```

Meaning:

- native function bodies for concrete generic method instantiations
- separate from ordinary `methodPointers[]`

Association:

```text
MetadataRegistration.genericMethodTable[k]
    -> methodSpecs[entry.methodIndex]
    -> genericMethodPointers[entry.indices.methodIndex]
```

Value:

- recovers names for concrete instantiations such as
  `List<int>.Add` or `Foo<string>.Bar<float>`

r2unity behavior:

- not parsed today

### `genericAdjustorThunks`

Shape:

```text
void *genericAdjustorThunks[]
```

Meaning:

- small thunks used for generic method calls that need value-type vs
  reference-type adjustment
- often skip the `Il2CppObject` header before tail-calling the real body

Version behavior:

- appears in late v24.x / Unity 2019.4-era layouts
- appears again in v27.1+

r2unity behavior:

- not parsed today

### `customAttributeGenerators`

Shape:

```text
void (*customAttributeGenerators[])(void *)
```

Meaning:

- pre-v29 native thunks that construct custom attribute objects
- each thunk encodes constructor arguments in machine code

Version behavior:

- used before v29
- v29 replaces this with inline custom-attribute BLOB data in
  `global-metadata.dat`
- v27-era layouts may move cache generators per module

r2unity behavior:

- not parsed/interpreted today

### `unresolvedVirtualCallPointers`

Shape:

```text
void *unresolvedVirtualCallPointers[]
```

Meaning:

- dispatch stubs used when virtual/interface call targets are resolved
  dynamically
- metadata has parameter/range tables that can name these stubs

Version behavior:

- v22..v29: one unresolved virtual call pointer table
- v29.1+: split into instance and static unresolved call pointer arrays

r2unity behavior:

- not parsed today

### `interopData`

Shape:

```text
Il2CppInteropData interopData[]
```

This is an array of structs, not a flat pointer array.

Conceptual element:

```c
typedef struct {
    uintptr_t delegatePInvokeWrapperFunction;
    uintptr_t pinvokeMarshalToNativeFunction;
    uintptr_t pinvokeMarshalFromNativeFunction;
    uintptr_t marshalToNativeFunction;
    uintptr_t marshalFromNativeFunction;
    uintptr_t marshalCleanupFunction;
    uintptr_t ccwFunction;
    uintptr_t guid;
    uintptr_t type; /* Il2CppType * */
} Il2CppInteropData;
```

Meaning:

- P/Invoke and COM interop functions for one type
- useful for attaching native wrapper VAs to `[DllImport]` methods and
  COM bridge code

Version behavior:

- v23+

r2unity behavior:

- not parsed today

### `windowsRuntimeFactoryTable`

Shape:

- UWP/WinRT-specific table

Meaning:

- Windows Runtime factory data
- usually absent or empty on iOS, Android, Linux, macOS, and normal
  desktop builds

r2unity behavior:

- not parsed today

## MetadataRegistration Tables

### `types`

Shape:

```text
Il2CppType pool/table
```

Conceptual element:

```c
typedef struct {
    uintptr_t data; /* type def index, nested type ptr, array ptr,
                       generic class ptr, generic parameter index, etc. */
    uint32_t bits;  /* attrs, ELEMENT_TYPE, byref, pinned, version bits */
} Il2CppType;
```

Meaning:

- every metadata `typeIndex` resolves through this table
- method return types, parameter types, field types, interface entries,
  generic constraints, and many other metadata records point here

Value:

- required for full type names beyond simple type definitions:
  arrays, pointers, byrefs, generic instances, generic parameters,
  modifiers, function pointers

r2unity behavior:

- not parsed today
- current names are limited by what can be reconstructed directly from
  decoded metadata rows

### `genericInsts` and `genericClasses`

Shape:

- generic instantiation tables and pointer/struct pools

Meaning:

- concrete type argument tuples such as `(int)` or `(string, int)`
- instantiated generic classes

Association:

- consumed by `Il2CppType` decoding and generic method name recovery

r2unity behavior:

- not parsed today

### `methodSpecs`

Shape:

```c
typedef struct {
    int32_t methodDefinitionIndex;
    int32_t classIndexIndex;
    int32_t methodIndexIndex;
} Il2CppMethodSpec;
```

Meaning:

- describes a concrete generic method instantiation
- points back to the open generic method definition and the class/method
  generic argument tuples

r2unity behavior:

- not parsed today

### `genericMethodTable`

Shape:

```c
typedef struct {
    int32_t methodIndex; /* -> methodSpecs */
    struct {
        int32_t methodIndex;   /* -> genericMethodPointers */
        int32_t invokerIndex;  /* -> invokerPointers */
        int32_t adjustorThunk; /* version-gated */
    } indices;
} Il2CppGenericMethodFunctionsDefinitions;
```

Meaning:

- bridge from a generic method spec to the concrete native function
  pointer table

Association:

```text
genericMethodTable[k].methodIndex
    -> methodSpecs[]
    -> open method + generic args

genericMethodTable[k].indices.methodIndex
    -> CodeRegistration.genericMethodPointers[]
    -> function VA
```

r2unity behavior:

- not parsed today

### `fieldOffsets`

Shape:

```text
pre-v22: int32_t fieldOffsets[]
v22+:    int32_t *fieldOffsets[]
```

Meaning:

- final runtime field offsets
- this is native-side layout data, not present in `global-metadata.dat`

Association:

- pre-v22: indexed by global field index
- v22+: one offset array per type definition

Notes:

- value-type instance fields need object-header adjustment in some
  contexts
- once decoded, this table lets analysis label `[this + offset]` memory
  accesses with field names and types

r2unity behavior:

- not parsed today

### `typeDefinitionsSizes`

Shape:

```c
typedef struct {
    uint32_t instance_size;
    uint32_t native_size;
    uint32_t static_fields_size;
    uint32_t thread_static_fields_size;
} Il2CppTypeDefinitionSizes;
```

Meaning:

- runtime class/struct sizes
- array element stride
- marshalled native size
- static and thread-static storage sizes

r2unity behavior:

- not parsed today

### `metadataUsages`

Shape:

```text
void *metadataUsages[]
```

Meaning:

- native side of old metadata usage lists/pairs
- each slot is a runtime metadata object pointer, such as:
  `Il2CppClass *`, `Il2CppType *`, `MethodInfo *`, `FieldInfo *`,
  managed string object, or generic `MethodInfo *`

Version behavior:

- v19..v26
- v27+ replaces this model with per-site inline globals and
  module-initializer setup patterns

r2unity behavior:

- not parsed today

## Heuristic Carver

The fallback section scanner searches readable/data sections for:

```text
u32 count
padding if 64-bit
ptr table
```

For each candidate pair:

1. `count` must be plausible.
2. `table` must dereference as data.
3. The first up to 128 entries are sampled.
4. Enough entries must be non-zero.
5. Enough entries must normalize to executable text VAs.
6. Accepted entries are copied into `method_ptrs[]`.

This finds flat method-pointer-like tables. It does not understand:

- `Il2CppCodeRegistration` layout
- `Il2CppMetadataRegistration` layout
- `Il2CppCodeGenModule` arrays unless an anchor path reaches them
- generic method table joins
- reverse-P/Invoke wrapper index joins
- field-offset pointer-of-pointer layouts

Because modern Unity stores method pointers per image, a single
heuristically found `{count, ptr}` table can be incomplete.

## Structural CodeRegistration Method Recovery

When `Il2CppCodeRegistration` is known, r2unity uses a stronger path.

First it searches near the registration VA for a count/pointer pair where
the count equals metadata image count:

```text
count == image_count
ptr   == codeGenModules[]
```

Then for every module:

```text
moduleName
methodPointerCount
methodPointers
```

The module name is matched to a metadata image name. After that:

```text
image.typeStart/typeCount
    -> type rows
type.methodStart/method_count
    -> method rows
module.methodPointers[local]
    -> method_ptrs[global_method_index]
```

If the module path fails, r2unity tries the older global
`methodPointersCount + methodPointers` pair near `CodeRegistration` and
copies it positionally.

## Version-sensitive native layouts

Native pointer-table layouts are version-gated. This document mentions
version constraints only where they affect a specific table shape, such as
pre-v24.2 global method pointers vs. v24.2+ per-image codegen-module
method tables. The canonical Unity-release, metadata wire-version,
sub-version, and row-layout matrix lives in `doc/future.md`.

## Common Pitfalls

- `global-metadata.dat` does not contain method RVAs.
- Method table slot order is not recovered from names; it is row order
  and native index arrays.
- One flat method-pointer table is only the old layout. Modern Unity
  needs the `codeGenModules[]` walk for full method coverage.
- Null method pointer entries are normal.
- Generic method instantiations are not ordinary method rows. They need
  `methodSpecs`, `genericMethodTable`, and `genericMethodPointers`.
- Invokers are not method bodies and are not one per method.
- Reverse-P/Invoke wrapper VAs require wrapper index joins.
- Field offsets are layout data from `MetadataRegistration`, not from
  field definitions alone.
- Metadata registration is a companion native anchor, not an automatic
  VA association for every metadata row.
- Pointer width and relocation state must be correct before scanning.

## Practical Recovery Order

For broad native-symbol recovery, the most useful order is:

1. Parse `global-metadata.dat` tables and counts.
2. Establish pointer size, base VA, readable ranges, and text range.
3. Resolve or structurally find `Il2CppCodeRegistration`.
4. Resolve or structurally find `Il2CppMetadataRegistration`.
5. Walk `codeGenModules[]` for v24.2+ method pointers.
6. Fall back to global `methodPointers[]` for older layouts.
7. Parse `MetadataRegistration.types`, `fieldOffsets`, and
   `typeDefinitionsSizes`.
8. Join `methodSpecs`, `genericMethodTable`, and
   `genericMethodPointers`.
9. Join reverse-P/Invoke wrapper indices to
   `reversePInvokeWrappers[]`.
10. Decode invoker indices, unresolved call stubs, metadata usages, and
    interop data.

This order keeps method naming useful early while preserving the path to
full class layout, generic-instantiation, interop, and dispatch recovery.

# r2unity - Internal Technical Documentation

This document is a technical reference for the `global-metadata.dat`
file format, for the native IL2CPP registration structures that pair
with it, and for how r2unity decodes both today. It focuses on what
the bytes mean, how they were produced by Unity's IL2CPP toolchain,
what changed between wire versions, which ECMA standards apply, and
which data is present on disk but not yet consumed.

The CLI surface itself is minimal and self-documenting (`./r2unity -h`
and `README.md`); this file is not a usage manual.

## 0. Context: what IL2CPP is and why the format exists

IL2CPP is Unity's AOT pipeline for producing managed-code binaries on
platforms where Mono/JIT is impractical or forbidden (iOS, consoles,
WebGL, Android release builds, UWP/HoloLens). For each managed
assembly the pipeline does the following:

1. Build the game's C# sources against the shipping BCL reference
	assemblies. Output is ordinary ECMA-335 CLI PE assemblies (the
	unmanaged-header form; see ECMA-335 §II.25).
2. Consume those PE assemblies plus Unity's runtime IL and hand them
	to `il2cpp.exe`, which transpiles every method body from CIL into
	equivalent C++ (hence the name). Reflection, generics, attributes,
	P/Invoke and marshalling are all rewritten as ordinary C++ calls
	into the `libil2cpp` runtime.
3. Compile that C++ with the platform toolchain (Xcode/clang,
	Android NDK, MSVC, Emscripten) and link against `libil2cpp`.

At the end of the pipeline, **all the logical metadata from every
input PE assembly is flattened into a single on-disk blob** —
`global-metadata.dat` — and all the physical data (function pointer
arrays, field offsets, generic-instantiation tables, etc.) is emitted
as normal `.rodata` / `.data` structures inside the produced ELF /
Mach-O / PE binary. The runtime cross-references them by index.

This split explains why the file format looks the way it does:

- It serializes ECMA-335 logical tables (type defs, method defs,
	field defs, parameter defs, property/event defs, generic metadata,
	custom attributes, assembly refs…) as flat offset-tagged arrays
	keyed by dense integer indices.
- It carries no pointers, because pointer width is a property of the
	target binary, not the metadata.
- The native binary carries parallel arrays of pointers/offsets,
	indexed by the same integers.

Upstream references (also used to cross-check every decoder):

- Microsoft's Common Language Infrastructure standard:
	ECMA-335 "Common Language Infrastructure", 6th ed.,
	<https://ecma-international.org/publications-and-standards/standards/ecma-335/>.
- Perfare/Il2CppDumper — the authoritative C# reference
	implementation, vendored under `third_party/Il2CppDumper/`;
	<https://github.com/Perfare/Il2CppDumper>.
- djkaty/Il2CppInspector — second-opinion decoder with a public
	Unity-version matrix;
	<https://github.com/djkaty/Il2CppInspector>.
- REAndroid/lib-global-metadata — Java rewrite, independent
	cross-check of every on-disk record and its version range;
	<https://github.com/REAndroid/lib-global-metadata>.
- Unity blog, "IL2CPP internals" series (Joshua Peterson, 2015):
	<https://blog.unity.com/technology/il2cpp-internals-method-calls>
	and neighbouring posts.

## 1. On-disk layout of `global-metadata.dat`

### 1.1 Byte 0: magic, version, header

```text
+0x00  uint32_t  sanity    = 0xFAB11BAF
+0x04  int32_t   version   ∈ {16,19,20,21,22,23,24,27,29,31,38,39}
+0x08  Il2CppGlobalMetadataHeader (version-dependent size)
```

The magic is a constant that never changes across the file's entire
history. `version` selects the layout of the header that follows. On
disk, `version` is normally one of the shipped values listed above;
values between those releases were reserved, experimental, or have

All scalars in the file are **little-endian**, regardless of host
platform. There are no native pointers anywhere in the file; only
byte offsets into the file and dense indices into other tables.

### 1.2 The header: a table of contents

The pre-v38 header is an array of `{uint32_t offset, int32_t size}`
pairs, one per logical table. Starting at v38, each entry becomes an
`Il2CppMetadataSection` triple:

```c
typedef struct {
	uint32_t offset;
	uint32_t size;
	uint32_t count;
} Il2CppMetadataSection;
```

`count` is the row count for real tables. This removes most stride
guessing for new metadata: when `count != 0`, row size is
`size / count`; when `count == 0`, the section is a byte pool.

`lib.h` declares four C layouts:

- `Il2CppGlobalMetadataHeader_v24` — ends at
	`exportedTypeDefinitions`; trailing WinRT slots are present but
	empty on non-UWP builds.
- `Il2CppGlobalMetadataHeader_v27` — adds `rgctxEntries{Offset,Size}`.
	RGCTX (Runtime Generic Context) ranges move into the `.dat` itself
	instead of being emitted only on the native side.
- `Il2CppGlobalMetadataHeader_v29` — adds
	`rgctxEntriesData{Offset,Size}`, i.e. the out-of-line BLOB pool
	used for RGCTX payloads and — critically — for the new v29
	custom-attribute encoding (see §2.11).
- `Il2CppGlobalMetadataHeader_v38` — replaces all offset/size pairs
	with 31 `{offset,size,count}` sections, in the same logical order
	as the older header up through `exportedTypeDefinitions`.

Each entry encodes `[offset, offset+size)` of one table relative to
the file start. Sections are not required to be contiguous or in any
particular order; the header is the only ground truth.

The four structs are held in a `union Il2CppGlobalMetadataHeader`.
Selection is purely by `version`:

```c
if (version >= 38)       layout = v38;
else if (version < 27)   layout = v24;
else if (version < 29)   layout = v27;
else                     layout = v29;  /* 29..35 */
```

r2unity normalizes both forms into `R2UnityMetadata.sections[]`.
For legacy headers, `count` is zero and row counts are still derived
from known or inferred entry sizes. For v38+, section counts are
trusted after the usual file-bounds checks.

There are more than two dozen tables in total. r2unity currently
consumes the header offsets/sizes for:

`stringOffset`, `stringSize`, `stringLiteralOffset`,
`stringLiteralSize`, `stringLiteralDataOffset`,
`stringLiteralDataSize`, `methodsOffset`, `methodsSize`,
`typeDefinitionsOffset`, `typeDefinitionsSize`, `imagesOffset`,
`imagesSize`, `assembliesOffset`, `assembliesSize`,
`referencedAssembliesOffset`, `referencedAssembliesSize`, and — on
v29+ — the BLOB pool fields at `attributesInfo*` / `attributeTypes*`
(legacy names for `attributeDataRange` / `attributeData`). On v38+
the same logical regions are consumed through `R2U_SEC_*` section
ids instead of individual `*Offset` and `*Size` fields.

Every other header entry is declared but unused. §2.11 and
`doc/future.md` enumerate what's still on the floor and why each
would be worth decoding.

### 1.3 Two distinct string mechanisms

There are two independent string pools in the file, and they are
addressed differently:

1. **Metadata string pool** at `stringOffset .. +stringSize`:
	NUL-terminated UTF-8 byte strings. Every `nameIndex`,
	`namespaceIndex`, `cultureIndex`, `moduleName*Index` etc. is an
	offset *inside this pool*. Because entries are NUL-terminated and
	referenced only by starting offset, the pool is de-duplicated
	aggressively by the packer.
2. **Managed string-literal payload area** at
	`stringLiteralDataOffset .. +stringLiteralDataSize`, indexed by
	rows in `stringLiteralOffset .. +stringLiteralSize`. Up through
	v31, each row is `{uint32_t length, uint32_t dataIndex}`. On
	v32+, rows store only `dataIndex`; the length is inferred from
	the next row's `dataIndex`, or from `stringLiteralDataSize` for
	the last row. These are ECMA-335 `ldstr` constants (§III.4.16).
	They can contain embedded NULs, raw UTF-16 payloads, or arbitrary
	binary blobs (`byte[]` literals are sometimes produced with
	`RuntimeHelpers.InitializeArray`).

This separation matters for tooling: a naïve "dump all strings"
pass on just the metadata pool misses every `ldstr` constant.
`doc/strings.md` covers the literal pool in detail.

### 1.4 Indices, tokens, and what they mean

There are two distinct integer namespaces used throughout the file.

#### Dense row indices

Every table in the file is an array; every reference between tables
is an index into the target table. On older metadata these indices
are usually 32-bit integers. Newer metadata can serialize common
index types in 1, 2, or 4 bytes depending on the target table count.
`Il2CppMethodDefinition.declaringType` is an index into
`typeDefinitions`; `Il2CppTypeDefinition.methodStart` is an index
into `methods`; `Il2CppFieldDefinition.typeIndex` is an index into
the native-side `Il2CppMetadataRegistration.types` pool (§3). No
cross-reference uses a file offset.

Sentinel "none" is all-bits-one at the serialized width:
`0xFF`, `0xFFFF`, or `0xFFFFFFFF`. r2unity expands all of them to
`-1` in its public structs.

For v38/v39 support, r2unity currently derives:

- `TypeIndex` width from the `parameters` row size.
- `TypeDefinitionIndex` width from `typeDefinitions.count`.
- `GenericContainerIndex` width from `genericContainers.count`.
- `ParameterIndex` width from `parameters.count` on v39+; older
	accepted metadata keeps this as 32-bit.

Il2CppDumper's v39 reader has compact-index read paths from v35, but
its metadata loader initializes those widths to 4 bytes until v38
section counts are available. Practically, v32..v35 keep 32-bit
indices in the current reference source.

#### ECMA-335 metadata tokens

Most logical rows also carry a `uint32_t token` field. This is an
**ECMA-335 metadata token** (ECMA-335 §II.22), layout:

```text
uint32_t token:
    bits 24..31 -> table id (0x02 TypeDef, 0x06 MethodDef,
                             0x04 FieldDef, 0x08 ParamDef,
                             0x14 Event,    0x17 Property,
                             0x20 Assembly, 0x21 AssemblyRef, …)
    bits  0..23 -> 1-based row index inside *the original PE assembly*
```

Tokens therefore encode the identity of the symbol **as it existed
in the pre-IL2CPP managed assembly**. They are not unique across
the flattened global metadata — two assemblies can each have a
TypeDef with token `0x02000001`. Any correct token-driven lookup
must scope by image first (§2.7). r2unity's reverse-P/Invoke
resolver builds per-image sorted `(token, method_index)` slices
specifically for this reason.

Token `customAttributeStart/Count` fields (pre-v29) used to index a
parallel token-range table; on v29+ they index BLOB offset ranges
instead (§2.11).

## 2. The logical tables

This section walks through each logical record type the file
declares, with current r2unity coverage called out where relevant.

### 2.1 `Il2CppTypeDefinition` — classes, structs, enums, delegates

Every CLR type that survives into the IL2CPP build gets one row.
88 B on the v24.1..v31 layout. Pre-v24 rows
carried extra legacy interop slots (`delegateWrapperFromManagedToNativeIndex`,
`marshalingFunctionsIndex`, `ccwFunctionIndex`, `guidIndex`), and
v24.0 still carried inline `customAttributeIndex` before token-based
lookup took over at v24.1.

On v32+ metadata, `elementTypeIndex` is no longer serialized in this
row. On v38+ metadata, the type-like indices inside the row can use
compact 1/2/4-byte serialized widths. r2unity decodes those rows into
the stable public `Il2CppTypeDefinition` shape; the removed
`elementTypeIndex` currently falls back to `parentIndex`.

Fields (v24.1+):

```c
uint32_t nameIndex, namespaceIndex;
int32_t  byvalTypeIndex;            /* -> MetadataRegistration.types */
int32_t  declaringTypeIndex;        /* nested types */
int32_t  parentIndex;               /* base class */
int32_t  elementTypeIndex;          /* element type for enums / arrays */
int32_t  genericContainerIndex;     /* -> genericContainers */
uint32_t flags;                     /* TypeAttributes, ECMA-335 §II.23.1.15 */
int32_t  fieldStart, methodStart, eventStart, propertyStart;
int32_t  nestedTypesStart, interfacesStart;
int32_t  vtableStart, interfaceOffsetsStart;
uint16_t method_count, property_count, field_count, event_count;
uint16_t nested_type_count, vtable_count;
uint16_t interfaces_count, interface_offsets_count;
uint32_t bitfield;                  /* packed is_enum/is_valuetype flags */
uint32_t token;                     /* 0x02?????? */
```

`bitfield` encodes runtime-state flags IL2CPP uses at load time:
`isEnum`, `isValueType`, `hasStaticConstructor`,
`hasFinalizer`, "static members ready", etc. The exact bit layout is
documented in `Il2CppDumper/Il2Cpp/Il2CppClass.cs`.

`vtableStart/Count` slices the global `vtableMethods[]` table
(§2.10); each slot is an `EncodedMethodIndex` that resolves to a
concrete method or a generic MethodSpec.

Not yet exposed by r2unity: `flags`, `bitfield`, `vtableStart/Count`,
and everything downstream (`fields`, `properties`, `events`,
nested-types list).

### 2.2 `Il2CppMethodDefinition` — every managed method

32 B (v24.1..v30) or 36 B (v31+) before compact-index shrinkage.
r2unity computes the current stride from the active version and
serialized index widths.

v24.1..v30 layout:

```c
uint32_t nameIndex;
int32_t  declaringType;          /* -> typeDefinitions */
int32_t  returnType;             /* -> MetadataRegistration.types */
int32_t  parameterStart;         /* -> parameters (§2.3) */
int32_t  genericContainerIndex;  /* -> genericContainers */
uint32_t token;                  /* 0x06?????? */
uint16_t flags;                  /* MethodAttributes, ECMA-335 §II.23.1.10 */
uint16_t iflags;                 /* MethodImplAttributes, §II.23.1.11 */
uint16_t slot;                   /* vtable slot; 0xFFFF if none */
uint16_t parameterCount;
```

v31+ inserts a `uint32_t returnParameterToken` between `declaringType`
and `returnType` (hence 36 B instead of 32 B). It carries the
`0x08??????` ParamDef token for the synthetic "return parameter"
row, aligning IL2CPP with ECMA-335's treatment of return-type
custom attributes (§II.22.33: the `Param` table row with `Sequence =
0` represents the return value).

Il2CppDumper's compact-index readers are wired from v35, but current
v39 sources keep those widths at 4 bytes until v38 section counts
exist. On v38+ metadata, `declaringType`, `returnType`, and
`genericContainerIndex` can be compact serialized indices. On v39+,
`parameterStart` can also be compact. The public struct still exposes
the decoded values as signed 32-bit integers.

v24.0 and below carried an additional set of inline indices:
`customAttributeIndex` (before tokens took over), `methodIndex`,
`invokerIndex`, `delegateWrapperIndex`, `rgctxStartIndex`,
`rgctxCount`. Those were removed at v24.1 and replaced by token-based
lookup + per-module RGCTX ranges (§3.8). r2unity does not decode
that layout today.

`flags` is an ECMA-335 `MethodAttributes` bitmask, with
`PINVOKE_IMPL = 0x2000` being the one bit r2unity's `-P` uses. The
full list matters for method attribute formatting in the emitted
r2 script (`build_method_attrs_string` in `src/main.c`).

`iflags` is ECMA-335 `MethodImplAttributes` (code type, managed vs
unmanaged, forwarding). `flags | iflags` together are enough to
classify a method as P/Invoke, reverse-P/Invoke, extern, internal
call, or synchronized.

### 2.3 `Il2CppParameterDefinition` — 16 B (v≤24.0) or 12 B (v≥24.1)

Indexed from `Il2CppMethodDefinition.parameterStart`. Fields:

```c
uint32_t nameIndex;
uint32_t token;                    /* 0x08?????? */
int32_t  customAttributeIndex;     /* removed at v24.1 */
int32_t  typeIndex;                /* -> MetadataRegistration.types */
```

Not decoded by r2unity today. Without it, every dumped method
degrades to `Method()` with no parameter types or names. Combined
with `parameterDefaultValues` (§2.4) this is what reconstructs real
C# signatures like `Foo(string name, int count = 0)`.

### 2.4 Default values: `fieldDefaultValues`, `parameterDefaultValues`,
`fieldAndParameterDefaultValueData`

```c
typedef struct { int fieldIndex;    int typeIndex; int dataIndex; } Il2CppFieldDefaultValue;
typedef struct { int parameterIndex; int typeIndex; int dataIndex; } Il2CppParameterDefaultValue;
```

`typeIndex` selects which `Il2CppTypeEnum` (ECMA-335 §II.23.1.16)
the payload decodes as: `I1/U1/I2/U2/I4/U4/I8/U8/R4/R8/STRING/CHAR/
BOOLEAN/SZARRAY/TYPEDEF/CLASS` etc. `dataIndex` is an offset into
the flat `fieldAndParameterDefaultValueData` BLOB heap.

The BLOB encoding for primitives is identical to ECMA-335's little-
endian scalar serialization. For `STRING`, IL2CPP writes a 7-bit
compressed length followed by the UTF-8 bytes (same compressed-int
scheme as ECMA-335 §II.23.2). v29 reuses this encoding for the
custom-attribute BLOB too (§2.11).

Decoded by `Il2CppExecutor.GetConstantValueFromBlob` in the
reference implementation. Not decoded by r2unity today, so
hard-coded URLs, API keys, feature flag names, and default argument
values are still invisible in the output.

### 2.5 `Il2CppFieldDefinition` — 12 B (v≤24.0) or 8 B (v≥24.1)

```c
uint32_t nameIndex;
int32_t  typeIndex;                /* -> MetadataRegistration.types */
int32_t  customAttributeIndex;     /* removed at v24.1 */
uint32_t token;                    /* 0x04?????? — added at v19 */
```

Without this table, r2unity cannot label any `this`-relative memory
access. Joined with `MetadataRegistration.fieldOffsets` (§3.6), every
`ldr X, [x0, #0x18]` in disassembly would become
`ldr X, [this, #Player.health]`. Second only to the method-pointer
table in RE value.

### 2.6 Properties and events

```c
typedef struct {    /* Il2CppPropertyDefinition */
    uint32_t nameIndex;
    int32_t  get;                   /* MethodDefIndex, -1 if no getter */
    int32_t  set;                   /* MethodDefIndex, -1 if no setter */
    uint32_t attrs;                 /* MethodSemanticsAttributes */
    int32_t  customAttributeIndex;  /* ≤24 */
    uint32_t token;                 /* ≥19 */
} Il2CppPropertyDefinition;

typedef struct {    /* Il2CppEventDefinition */
    uint32_t nameIndex;
    int32_t  typeIndex;             /* -> MetadataRegistration.types */
    int32_t  add, remove, raise;    /* MethodDefIndex, -1 when absent */
    int32_t  customAttributeIndex;  /* ≤24 */
    uint32_t token;                 /* ≥19 */
} Il2CppEventDefinition;
```

Both are pure back-references into the methods table. Decoding them
lets a reconstructor rejoin `get_X`/`set_X`/`add_X`/`remove_X` into
proper C# `property` and `event` declarations, and to identify auto-
properties via the `<X>k__BackingField` pattern.

Not exposed by r2unity today.

### 2.7 `Il2CppImageDefinition` and `Il2CppAssemblyDefinition`

One image per managed PE assembly; one assembly per image in every
wire version r2unity currently targets. Images are 40 B (v24.1+)
before compact-index shrinkage, 36 B (v24.0), 28 B (v19..v23), and
24 B (pre-v18). On v38+ metadata, `typeStart` and
`exportedTypeStart` can use compact `TypeDefinitionIndex` widths.

```c
typedef struct {    /* Il2CppImageDefinition (v24.1+, 40 B) */
    uint32_t nameIndex;                /* usually the PE file name    */
    int32_t  assemblyIndex;            /* back-edge to assemblies     */
    int32_t  typeStart;                /* slice of typeDefinitions    */
    uint32_t typeCount;
    int32_t  exportedTypeStart, exportedTypeCount; /* type forwarders */
    int32_t  entryPointIndex;          /* MethodDefIndex or -1        */
    uint32_t token;                    /* 0x00000001 (module row)     */
    int32_t  customAttributeStart;     /* v24.1+: token-range start   */
    uint32_t customAttributeCount;
} Il2CppImageDefinition;
```

Images are the ownership boundary for everything else. Because
ECMA-335 metadata tokens are per-assembly 1-based row indices (§1.4),
every token lookup needs a `typeIndex -> imageIndex` map first,
followed by a per-image `(token, row)` search. r2unity builds this
map once from `typeStart`/`typeCount` and reuses it for both
P/Invoke image resolution and reverse-P/Invoke attribute matching.

Assembly rows follow the same image ordering and carry a trailing
`Il2CppAssemblyNameDefinition`:

```c
typedef struct {
    uint32_t name_idx, culture_idx, public_key_idx;
    uint32_t hash_value_idx;        /* present when wire < 24.3 */
    uint32_t hash_alg;              /* AssemblyHashAlgorithm enum */
    int32_t  hash_len;
    uint32_t flags;                 /* AssemblyFlags */
    int32_t  major, minor, build, revision;
    uint8_t  public_key_token[8];   /* SHA1-truncated strong-name key */
} Il2CppAssemblyNameDefinition;
```

ECMA-335 §II.22.2 defines the `Assembly` table and §II.6.3 defines
the public-key-token derivation (low 8 bytes of SHA-1 of the public
key, byte-reversed). r2unity decodes both tail layouts:

- 52 B (`hashValueIndex` present)
- 48 B (`hashValueIndex` removed in v24.3+)

The tail is picked by inferring `assembliesSize / image_count` and
matching on legacy headers, or by using the v38+ section count.
Starting at v38, `Il2CppAssemblyDefinition` also has a `moduleToken`
field before the name tail, so the tail starts at row offset 20
instead of 16.

### 2.8 `referencedAssemblies` — flat `int32_t[]`

Since v20, each `Il2CppAssemblyDefinition` carries a
`referenced_start`/`referenced_count` slice into this flat index
array; each entry is an index into the `assemblies` table. This is
the managed-to-managed dependency graph (mscorlib.dll,
UnityEngine.CoreModule.dll, …). Drives `-S` dependency edges.

### 2.9 Generics: `genericContainers`, `genericParameters`,
`genericParameterConstraints`

```c
typedef struct {    /* Il2CppGenericContainer, 16 B */
    int32_t ownerIndex;               /* TypeDef or MethodDef index   */
    int32_t type_argc;
    int32_t is_method;                /* 0 = class generic, 1 = method */
    int32_t genericParameterStart;
} Il2CppGenericContainer;

typedef struct {    /* Il2CppGenericParameter, 16 B */
    int32_t  ownerIndex;
    uint32_t nameIndex;               /* "T", "TKey", …               */
    int16_t  constraintsStart, constraintsCount;
    uint16_t num;                     /* argument position             */
    uint16_t flags;                   /* GenericParameterAttributes    */
} Il2CppGenericParameter;
```

`genericParameterConstraints` is a flat `int32_t[]` of type indices
(again into `MetadataRegistration.types`). `GenericParameterAttributes`
encodes variance (`in`/`out`) and special constraints
(`class`/`struct`/`new()`) per ECMA-335 §II.23.1.7.

Gate for `where TKey : IComparable<TKey>`-style reconstruction and
for using `methodSpecs` / `genericInsts` (§3.9) to produce real
instantiation names like `List<int>::Add`.

Not decoded by r2unity today.

### 2.10 Interfaces, interface offsets, vtable methods

```c
/* interfaces[] — raw int32_t[] of type indices; per-type slice via
   typeDef.interfacesStart/interfaces_count. */

typedef struct {    /* Il2CppInterfaceOffsetPair, 8 B */
    int interfaceTypeIndex;
    int offset;                      /* vtable slot where its methods */
} Il2CppInterfaceOffsetPair;

/* vtableMethods[] — raw uint32_t[]; each slot is an EncodedMethodIndex */
```

`EncodedMethodIndex` decoding (Il2CppDumper's convention — matches
Unity's IL2CPP codegen):

```c
usageType = (idx & 0xE0000000) >> 29;   /* Il2CppMetadataUsage enum  */
decoded   = (version >= 27)
              ? ((idx & 0x1FFFFFFE) >> 1)
              : (idx & 0x1FFFFFFF);
```

`Il2CppMetadataUsage`:
`{ Invalid=0, TypeInfo=1, Il2CppType=2, MethodDef=3, FieldInfo=4,
  StringLiteral=5, MethodRef=6 }`.

`usageType == 3` → concrete method-definition index;
`usageType == 6` → index into `methodSpecs` (§3.9).

Exposing this unlocks full virtual dispatch resolution, real
interface implementation chains, and `find-all-implementers`
queries.

### 2.11 Custom attributes — two very different encodings

**Pre-v29 (wire 21..27.2)** uses a token-range table plus a flat
list of attribute *types* and a native-side per-range generator
thunk:

- `attributesInfoOffset/Size`:
	`Il2CppCustomAttributeTypeRange[] = { uint token; int start; int count; }`
	(12 B, or 8 B pre-24.1 without the token field).
- `attributeTypesOffset/Size`: flat `int32_t[]` of attribute type
	indices, sliced by each range's `start/count`.
- `CodeRegistration.customAttributeGenerators`: one `void(void*)`
	function pointer per range. Its body `new`s up the attribute
	instances using whatever constructor arguments were baked at
	transpile time. Recovering the constructor arguments therefore
	requires reading native code — i.e. running a small interpreter
	over the thunk. This is what Il2CppDumper's
	`GetCustomAttributeValuesFromGenerator` does.

**v29+** replaces the entire scheme with an inline BLOB:

- `attributesInfoOffset/Size` is repurposed as
	`attributeDataRangeOffset/Size`:
	`{ uint token; uint startOffset; }` array; each entry points into
	the BLOB.
- `attributeTypesOffset/Size` is repurposed as
	`attributeDataOffset/Size`: raw BLOB heap.
- The BLOB uses an ECMA-335-like encoding
	(ECMA-335 §II.23.3 `CustomAttrib`) with IL2CPP's 7-bit
	compressed-int extension: leading compressed count of constructor
	references, each constructor encoded as a method index and
	inline-serialized argument values.
- `customAttributeGenerators` is gone.

This is why r2unity's reverse-P/Invoke path requires v29+: the
attribute constructors identifying `[MonoPInvokeCallback]` and
`[UnmanagedCallersOnly]` are actually inspectable from the metadata
alone starting at v29.

Attributes worth recovering for RE purposes: `[Preserve]` (flags
reflection-only-reachable code), `[SerializeField]`,
`[DllImport("...",EntryPoint="...")]`, `[Obsolete]`,
`[RequireComponent(typeof(X))]`,
`[RuntimeInitializeOnLoadMethod]`, `[JsonProperty]`.

### 2.12 Pre-v27 metadata-usage tables

```c
typedef struct { uint32_t start; uint32_t count;         } Il2CppMetadataUsageList;
typedef struct { uint32_t destinationIndex; uint32_t encodedSourceIndex; } Il2CppMetadataUsagePair;
```

`encodedSourceIndex` uses the same `Il2CppMetadataUsage`-tagged
encoding as vtable slots (§2.10). `destinationIndex` is the slot in
the native `MetadataRegistration.metadataUsages[]` pointer table
(§3.10).

On pre-v27 binaries, compiled code loads every metadata pointer via
`ldr X, [g_MetadataUsages + const]`. Labelling the array turns every
such site into a readable reference: `TypeInfo System.String`,
`StringLiteral_42 "hello"`, etc.

From v27 onward this indirection is replaced by per-site inline
globals lazily initialized by `Il2CppCodeGenModule.moduleInitializer`,
and these tables vanish from the `.dat`.

### 2.13 `fieldRefs` — v19+

```c
typedef struct { int typeIndex; int fieldIndex; } Il2CppFieldRef;
```

Backing list for `FieldInfo`-typed metadata-usage slots (§2.12),
especially for fields of generic-instantiation types that cannot be
addressed directly by `(typeDef, fieldDef)` alone.

### 2.14 `fieldMarshaledSizes` — v19+

```c
typedef struct { int fieldIndex; int typeIndex; int size; } Il2CppFieldMarshaledSize;
```

Holds `[MarshalAs]` and `[StructLayout]` sizes. Matters only when the
game talks to native plugins with explicit marshalling; safe to
ignore otherwise.

### 2.15 `unresolvedVirtualCallParameterTypes` /
`unresolvedVirtualCallParameterRanges` — v22+

```c
/* Types: raw TypeIndex[] */
typedef struct { int start; int length; } Il2CppRange;
```

Pairs with `CodeRegistration.unresolvedVirtualCallPointers` (§3.5)
to name each unresolved-virtual-call stub with its parameter
signature.

### 2.16 WinRT-only tables

`windowsRuntimeTypeNames`, `windowsRuntimeStrings`,
`windowsRuntimeFactoryTable`. On iOS, Android, Linux, macOS and
standalone Windows these sizes are always zero; a correct parser
handles them defensively and otherwise skips.

### 2.17 `exportedTypeDefinitions` — v24+

Raw `TypeDefinitionIndex[]`. Per-image via
`imageDef.exportedTypeStart/Count`. These are .NET type forwarders,
used when System.Runtime-style façade assemblies re-export types
into mscorlib. Required only if a tool wants to faithfully reproduce
runtime assemblies; otherwise optional.

### 2.18 RGCTX tables

```c
typedef struct {    /* Il2CppRGCTXDefinition — shape at v≥24.1 on disk */
    int32_t type;                       /* Il2CppRGCTXDataType enum  */
    Il2CppRGCTXDefinitionData data;     /* typically int rgctxDataDummy */
} Il2CppRGCTXDefinition;                /* 8 B (12 B post-27.2 on disk) */
```

At v27.2, `type_post29` widens to `uint64_t` and `_data` widens to
`uint64_t` → row grows to 16 B. This is one of the rare cases where
a fractional sub-version leaks into the on-disk layout.

`Il2CppRGCTXDataType` = `{ Invalid, Type, Class, Method, Array,
Constrained }`. Per-type via `typeDef.rgctxStartIndex/rgctxCount`
(pre-v24.1), per-method similarly; from v24.2 onward RGCTX tables
migrate to `Il2CppCodeGenModule.rgctxs` + `rgctxRanges` on the
native side (§3.7).

**RGCTX** = "Runtime Generic Context" — the per-instantiation
lookup table that lets a single compiled generic method body
resolve `typeof(T)`, `default(T)`, and generic-virtual dispatch
sites without recompiling per instantiation.

## 3. The native side: CodeRegistration / MetadataRegistration

The other half of the IL2CPP runtime knowledge lives in the binary,
typically in `.rodata` / `.data.rel.ro`. Two top-level anchors
exist; every other array hangs off one of them.

```c
typedef struct {    /* Il2CppCodeRegistration (shape varies by version) */
    ulong methodPointersCount;              /* pre-v24.2 only           */
    ulong methodPointers;                   /* pre-v24.2 only           */
    ulong reversePInvokeWrapperCount;
    ulong reversePInvokeWrappers;
    ulong genericMethodPointersCount;
    ulong genericMethodPointers;
    ulong genericAdjustorThunks;            /* v24.5 / v27.1+           */
    ulong invokerPointersCount;
    ulong invokerPointers;
    ulong customAttributeCount;
    ulong customAttributeGenerators;        /* removed at v29           */
    ulong unresolvedVirtualCallCount;
    ulong unresolvedVirtualCallPointers;    /* split at v29.1           */
    ulong interopDataCount;
    ulong interopData;                      /* v23+                     */
    ulong windowsRuntimeFactoryCount;
    ulong windowsRuntimeFactoryTable;
    ulong codeGenModulesCount;              /* v24.2+                   */
    ulong codeGenModules;                   /* v24.2+                   */
} Il2CppCodeRegistration;

typedef struct {    /* Il2CppMetadataRegistration */
    long  genericClassesCount;            ulong genericClasses;
    long  genericInstsCount;              ulong genericInsts;
    long  genericMethodTableCount;        ulong genericMethodTable;
    long  typesCount;                     ulong types;
    long  methodSpecsCount;               ulong methodSpecs;
    long  fieldOffsetsCount;              ulong fieldOffsets;
    long  typeDefinitionsSizesCount;      ulong typeDefinitionsSizes;
    ulong metadataUsagesCount;
    ulong metadataUsages;
} Il2CppMetadataRegistration;
```

`ulong` means host pointer width — 8 B on arm64, x86-64, Windows
x64, macOS; 4 B on armeabi-v7a, x86, wasm32. Parsers must branch on
`EI_CLASS` for ELF, `MH_MAGIC_64` vs `MH_MAGIC` for Mach-O,
`IMAGE_FILE_MACHINE_*` for PE. r2unity today only walks arm64 /
x86-64 targets robustly; 32-bit paths exist but are less exercised.

Finding the anchors without symbols typically relies on structural
matching: `methodPointersCount` or
`codeGenModulesCount * typesCount * methodsCount` must equal known
integers derivable from the `.dat`. Il2CppDumper's
`ExecuteBinarySearch` does this.

### 3.1 `methodPointers` (pre-v24.2 global; v24.2+ per-module)

Method `i`'s entry at slot `i`. Zeros for abstract, generic-open,
external/P-Invoke, and intrinsic runtime methods.

From v24.2 onward the array moves **inside each `Il2CppCodeGenModule`**
(one module per image). A global `methodPointers` no longer exists.
A scanner that locks onto a single `{count, ptr}` on a v24.2+ binary
only extracts one image's methods. This is the single most common
"why is my dumper missing half the methods" bug on modern Unity.

r2unity's current heuristic finds one `{count, ptr}` and is
therefore vulnerable to this on Unity ≥ 2019.3. Walking
`codeGenModules[]` and enumerating each module is mandatory for
full coverage.

### 3.2 `invokerPointers`

Reflection-dispatch invokers. One per unique
`(returnType, paramCount, blittable-ness)` signature shape, not per
method. Methods reference them via `methodDef.invokerIndex`
(pre-v24.2) or `Il2CppCodeGenModule.invokerIndices[]` (v24.2+).

Naming these clusters methods by signature; very useful for
discovering reflection hook points (`MethodInfo.Invoke` calls route
through here).

### 3.3 `customAttributeGenerators` (pre-v29)

Per-range `void(void*)` function pointer, each one news up the
attribute object(s) for one token range. The generator body is
typically a few dozen instructions: load constructor argument
constants, call the attribute's constructor, store the result. A
tiny arch-specific interpreter can recover every attribute
argument. Gone at v29 (§2.11).

### 3.4 `reversePInvokeWrappers` (v22+)

Array of native-to-managed trampolines for every
`[MonoPInvokeCallback]`-annotated method. Every callback the game
registers with a native library (ad SDK callback, audio driver
callback, analytics pingback) goes through one of these. Naming
them `ReversePInvoke_<method>` lights up the native plugin callback
surface immediately.

### 3.5 `unresolvedVirtualCallPointers` (v22..v29; split at v29.1
into `unresolvedInstanceCallPointers` + `unresolvedStaticCallPointers`)

Stubs the runtime patches when a virtual or interface call target
becomes known at run time. 1:1 with the metadata ranges in §2.15.

### 3.6 `interopData` (v23+)

Array of `Il2CppInteropData` — each element bundles P/Invoke and
COM-interop function pointers **for one type**:

```c
typedef struct {
    ulong delegatePInvokeWrapperFunction;
    ulong pinvokeMarshalToNativeFunction;
    ulong pinvokeMarshalFromNativeFunction;
    ulong marshalToNativeFunction;
    ulong marshalFromNativeFunction;
    ulong marshalCleanupFunction;
    ulong ccwFunction;                     /* COM only          */
    ulong guid;                            /* COM IID pointer   */
    ulong type;                            /* -> Il2CppType *   */
} Il2CppInteropData;
```

Walking this array gives the native VA of every `[DllImport]` target
wrapper and every COM CCW/RCW bridge. This is where real P/Invoke
target recovery lives — r2unity does not read it today.

### 3.7 `codeGenModules` (v24.2+)

One `Il2CppCodeGenModule` per image. Per Il2CppDumper's
`Il2CppClass.cs`:

```c
typedef struct {
    ulong moduleName;                    /* C string  */
    long  methodPointerCount;
    ulong methodPointers;                /* PER-IMAGE method table */
    ulong adjustorThunks;                /* v24.5 / v27.1+        */
    ulong adjustorThunkCount;
    ulong invokerIndices;                /* int32[] per method    */
    ulong reversePInvokeWrapperCount;
    ulong reversePInvokeWrapperIndices;
    long  rgctxRangesCount;
    ulong rgctxRanges;                   /* Il2CppTokenRangePair[] */
    long  rgctxsCount;
    ulong rgctxs;                        /* Il2CppRGCTXDefinition[] */
    ulong debuggerMetadata;
    ulong customAttributeCacheGenerator; /* v27 only              */
    ulong moduleInitializer;             /* v27+ <Module>..cctor  */
    ulong staticConstructorTypeIndices;  /* v27+                  */
    ulong metadataRegistration;          /* v27+ per-assembly     */
    ulong codeRegistaration;             /* sic — v27+            */
} Il2CppCodeGenModule;
```

`moduleInitializer` is each image's `<Module>..cctor` — the per-
image static-init entry point.
`staticConstructorTypeIndices` lists types whose `.cctor` runs at
load time. Both are prime targets in obfuscated games where encryption
keys or obfuscated strings are materialised at startup.

**`adjustorThunks`** (v24.5, v27.1+) are the struct-vs-class `T`
fixup thunks for generic methods: `mov x0, [x0, #8]; b real_body`
(skip the `Il2CppObject` header when `T` is a value type). Labelling
them separately from the real body reduces disassembly confusion.

### 3.8 `MetadataRegistration.types` — the `Il2CppType` pool

```c
typedef struct {    /* Il2CppType, 16 B */
    ulong datapoint;               /* union: klassIndex / type* /
                                      array* / genericParamIdx /
                                      generic_class* */
    uint  bits;                    /* packed fields:
       attrs:16 | type:8 | num_mods:{5|6} | byref:1 | pinned:1
       [ | valuetype:1 at v27.2+ ] */
} Il2CppType;
```

Every `typeIndex` used anywhere in the metadata — method
return types, parameter types, field types, interface lists —
resolves into this pool. Without it, the textual form of any type
beyond a simple `TypeDef` (so: arrays, pointers, by-ref, generic
instantiations, generic parameters, `modreq`/`modopt`) cannot be
reconstructed.

`type` field at bits 16..23 is the ECMA-335 `ELEMENT_TYPE_*`
(ECMA-335 §II.23.1.16). Walking it recursively produces qualified
names like `List<Dictionary<int, string>>[]&`.

### 3.9 Generics on the native side

```c
/* genericInsts[]: concrete type-argument tuples
   ( (int), (string,int), (Player, Dictionary<int,float>), … ) */

typedef struct {    /* Il2CppMethodSpec, 12 B */
    int methodDefinitionIndex;          /* the open generic method */
    int classIndexIndex;                /* -> genericInsts (-1 if none) */
    int methodIndexIndex;               /* -> genericInsts (-1 if none) */
} Il2CppMethodSpec;

typedef struct {    /* Il2CppGenericMethodFunctionsDefinitions */
    int methodIndex;                    /* -> methodSpecs          */
    struct {
        int methodIndex;                /* -> genericMethodPointers */
        int invokerIndex;
        int adjustorThunk;              /* v24.5+                   */
    } indices;
} Il2CppGenericMethodFunctionsDefinitions;
```

Consumed together with `CodeRegistration.genericMethodPointers`:

```c
foreach (entry in genericMethodTable) {
    spec = methodSpecs[entry.methodIndex];
    va   = genericMethodPointers[entry.indices.methodIndex];
    // emit "List<int>::Add @ va"
}
```

Il2CppDumper's `GetMethodSpecName` reconstructs the full
`Foo<string>.Bar<int>` textual form.

### 3.10 Field offsets, type sizes, metadata usages

- `MetadataRegistration.fieldOffsets`:
	- pre-v22: flat `int32_t[]` indexed by global field index
	- v22+: pointer-of-pointers — one `int32_t *` per type definition,
		pointing at that type's field-offset array
- `typeDefinitionsSizes`:
	```c
	typedef struct {
	    uint32_t instance_size;             /* includes Il2CppObject hdr */
	    uint32_t native_size;               /* P/Invoke marshalled       */
	    uint32_t static_fields_size;
	    uint32_t thread_static_fields_size;
	} Il2CppTypeDefinitionSizes;             /* 16 B                     */
	```
	`instance_size` is the array element stride:
	`ldr X, [Xn + Xindex*instance_size]` in disassembly.
- `metadataUsages`: native side of §2.12, void-pointer table.
	On pre-v27 binaries, every metadata-aware load in the code
	dereferences a slot here.

ECMA-335 §II.25.4 documents instance layout (the `MonitorData *` +
`Il2CppClass *` header prefix that makes non-static value-type
fields appear at offset `-8`/`-16` from the `Il2CppObject` pointer).
`GetFieldOffsetFromIndex` in Il2CppDumper's `Il2Cpp.cs:274` has the
exact math.

## 4. Wire-version history and per-version layout deltas

This is a condensed changelog; `doc/future.md` has the full
Unity-release → wire-version → sub-version mapping.

- **v16..v20** (Unity 5.0..5.2): pre-public-IL2CPP-shipped era.
	Rejected.
- **v21** (Unity 5.3.0..5.3.5): first on-iOS IL2CPP. Rejected.
- **v22** (Unity 5.3.6..5.4): drops delegate-wrapper tables; adds
	`unresolvedVirtualCall*` tables. Rejected.
- **v23** (Unity 5.5): adds `interopData` and WinRT tables.
	Rejected.
- **v24** (Unity 5.6..2018.x): adds `exportedTypeDefinitions`. v24.1
	(2018.3) switches all custom-attribute resolution to token-based
	lookup and drops `customAttributeIndex` from every def. v24.2
	(2019.1) introduces `Il2CppCodeGenModule` and per-image method
	arrays. v24.5 (2019.4.15) adds `genericAdjustorThunks`.
- **v27** (Unity 2020.2..2021.3): `customAttributeGenerators` move
	per-image into `codeGenModule.customAttributeCacheGenerator`.
	v27.1 re-adds `genericAdjustorThunks`. v27.2 widens the RGCTX
	`type`/`data` fields from 32 to 64 bits on disk and repacks
	`Il2CppType.valuetype` as a dedicated bit. `MethodInfo` struct
	changes on the native side.
- **v29** (Unity 2022.1..2022.3): replaces the custom-attribute
	generator thunks with inline BLOB encoding (§2.11). v29.1 splits
	`unresolvedVirtualCallPointers` into separate instance/static
	arrays.
- **v30**: reserved, never shipped.
- **v31** (Unity 2023.1..2023.x): adds `returnParameterToken` to
	`Il2CppMethodDefinition`, aligning with ECMA-335's return-value
	param-row model.
- **v32..v35** (gap range in current references): keep the legacy
	offset/size header. v32 removes `Il2CppStringLiteral.length` and
	`Il2CppTypeDefinition.elementTypeIndex`; v35 introduces compact-
	index reader hooks but keeps 4-byte widths in the current
	Il2CppDumper source.
- **v36..v37**: no header layout is declared by the v39 reference;
- **v38** (Unity 6 era): switches the header from offset/size pairs
	to 31 `{offset,size,count}` section records.
	and compacts additional serialized indices, including
	`ParameterIndex`.

r2unity's current shipped-version acceptance band is **v24.1..v31**
plus **v38..v39**. It also accepts unobserved v32..v35 with the
legacy offset/size header, the v32 row deltas above, and 4-byte
header shape. Since the on-disk `version` field is `24` for both
v24.0 and v24.1+, the parser probes the image-row stride (32 B on
v24.0, 40 B on v24.1+) and rejects v24.0 explicitly to avoid silent
mis-decoding of TypeDefinition/MethodDefinition/ImageDefinition rows.

## 5. How the parser is wired in r2unity

File-by-file map:

- `src/lib/lib.h` — every structure, constant, and public entry
	point declared. The `Il2CppGlobalMetadataHeader_v{24,27,29,38}`
	layouts are here, in a `union`, plus the normalized
	`R2UnityMetadata` snapshot.
- `src/lib/lib.c` — header parsing, string pool, literal decoding,
	type/method/image/assembly/referenced-assembly decoders, P/Invoke
	and reverse-P/Invoke enumerators, endian-safe LE readers
	(`RD_LE32`, `RD_LE16`).
- `src/lib/elf.c` — ELF32/64 loader, dynamic-table walk, relative
	relocation application (`DT_REL`, `DT_RELA`, `DT_RELR`),
	method-pointer-array heuristic.
- `src/lib/macho.c` — Mach-O 64 loader (thin + FAT first-ARM64),
	`LC_SEGMENT_64` walk, method-pointer-array heuristic.
- `src/lib/pe.c` — PE32/PE32+ loader, section walk, method-pointer-
	array heuristic.
- `src/main.c` — CLI entry point and output emitters.

Every row decoder reads via `r_read_le32`/`r_read_le16` (LE on all
hosts), so the parser is valid on big-endian hosts. Arrays returned
by the decoders are plain C arrays; the underlying `RBuffer` is only
retained for the two string pools.

## 6. Native-binary scanning, in one picture

```text
ELF/Mach-O/PE image on disk
    │
    ├─ load & parse segments/sections
    │       ↓
    │   segments { vaddr/vmaddr, size, perms, file mapping }
    │       ↓
    │   [text_lo, text_hi)  (executable union)
    │
    ├─ ELF only: apply DT_REL / DT_RELA / DT_RELR relative fixups
    │            so data-segment pointer arrays match the runtime
    │            state (addends resolved, RELR bitmap expanded).
    │
    ├─ scan each writable/data segment:
    │     pass 1: {count32, pad32, ptr} tuple
    │             (CodeRegistration-shaped anchor pair)
    │     pass 2: {count32, ptr} generic
    │
    └─ accept if a sample of entries at *ptr[] lands in text,
       either already (post-relocations) or after + base_vaddr
       (raw RVA case). Emit absolute VAs, one per method index.
```

The heuristic is deliberately weaker than a structural
`Il2CppCodeRegistration` match, but it works on every supported
target and doesn't need symbol tables. It does, however, lock onto
**one** `{count, ptr}` array, which on v24.2+ means one image's
methods, not all of them (§3.1 / §3.7). A proper structural match
that walks `codeGenModules[]` is on the roadmap.

For ELF the relocation pass matters because the Android linker
produces method-pointer arrays almost entirely as
`R_AARCH64_RELATIVE` (type 1027) entries. Without applying them,
the raw array on disk is a run of zeros. Packed Android relocations
(`DT_ANDROID_RELA`, `DT_ANDROID_RELR`) are not handled yet and
cause the same "empty array" symptom on Play Store builds.

For Mach-O and PE the linker has already materialised concrete
values; no explicit relocation pass is required for the tables
r2unity currently scans.

## 7. Data we can extract today vs. data we do not

Already extracted by r2unity (library + CLI):

| Source                      | Shape                       |
|-----------------------------|-----------------------------|
| metadata header             | version, sections/offsets   |
| metadata string pool        | on-demand `get_string`      |
| managed string literals     | full dump (`-z`)            |
| type definitions            | per-row decoder             |
| method definitions          | per-row decoder             |
| image definitions           | per-row decoder             |
| assembly definitions        | per-row decoder             |
| referenced assemblies       | flat int32 array            |
| P/Invoke marker methods     | `-P` enumeration            |
| reverse-P/Invoke on v29+    | `-R` enumeration via BLOB   |
| method-pointer VA (global)  | ELF/Mach-O/PE heuristic     |

Data present on disk / in the binary but **not yet consumed**:

- `parameters` + `parameterDefaultValues` + BLOB heap → real method
	signatures with names, types, and default arguments (§2.3–2.4).
- `fields` + `fieldDefaultValues` + BLOB heap → field names, types,
	and compile-time constants; prerequisite for labelling
	`[this+N]` accesses (§2.5).
- `properties` + `events` → recomposed C# `property` / `event`
	declarations (§2.6).
- `genericContainers` + `genericParameters` +
	`genericParameterConstraints` → generic-parameter names and
	constraints (`where T : IComparable<T>`) (§2.9).
- `interfaces` + `interfaceOffsets` + `vtableMethods` → fully
	resolved virtual dispatch and interface-implementation graph
	(§2.10).
- `nestedTypes` → correct `Outer.Inner` naming (§2).
- `attributesInfo`/`attributeTypes` (pre-v29) or
	`attributeDataRange`/`attributeData` (v29+) → every `[Attribute]`
	on every declaration; `[Preserve]` reflection markers;
	`[DllImport]` DLL+entry args (§2.11).
- `metadataUsageLists/Pairs` (pre-v27) → labels every indirect
	metadata load in compiled code (§2.12).
- `fieldMarshaledSizes`, `unresolvedVirtualCall*`, WinRT tables,
	`exportedTypeDefinitions`, RGCTX tables (§2.14–2.18).
- Native-side `CodeRegistration` and `MetadataRegistration` walk →
	`invokerPointers`, `customAttributeGenerators`,
	`reversePInvokeWrappers`, `genericMethodPointers`,
	`interopData`, `codeGenModules[]`, `types`, `fieldOffsets`,
	`typeDefinitionsSizes`, `metadataUsages` (§3).
- Richer native scanning: per-module `methodPointers` on v24.2+,
	packed Android relocations, Mach-O FAT multi-slice,
	chained-fixups, PE import table.

## 8. validation corpus


- `json-one-line`, `json-unity` — `-j` shape/content
- `pinvoke-count`, `pinvoke-list` — `-P`
- `reverse-pinvoke-count`, `reverse-pinvoke-list` — `-R`
- `strings-list` — `-z`

## 9. Further reading

Primary references used by r2unity's decoders:

- ECMA-335, "Common Language Infrastructure" — the foundational spec
	for every token format, attribute BLOB, signature encoding, and
	type-code enum reused by IL2CPP
	<https://ecma-international.org/publications-and-standards/standards/ecma-335/>
- ECMA-334, "C# Language Specification" — useful for attribute
	semantics; IL2CPP preserves every CLR-visible attribute
	<https://ecma-international.org/publications-and-standards/standards/ecma-334/>
- Perfare/Il2CppDumper (C# reference)
	<https://github.com/Perfare/Il2CppDumper>
- Il2CppDumper v39 metadata structs used for the v38/v39 layout
	update:
	<https://raw.githubusercontent.com/roytu/Il2CppDumper/v39/Il2CppDumper/Il2Cpp/MetadataClass.cs>
- Local Unity 6000.4.5f1 IL2CPP runtime sources:
	`/Applications/Unity/Hub/Editor/6000.4.5f1/Unity.app/Contents/Resources/Scripting/il2cpp/libil2cpp/vm/GlobalMetadataFileInternals.h`,
	`MetadataDeserialization.h`, `MetadataDeserialization.cpp`, and
	`GlobalMetadata.cpp`.
- djkaty/Il2CppInspector (alternative C# reference + Unity
	version matrix)
	<https://github.com/djkaty/Il2CppInspector>
- REAndroid/lib-global-metadata (Java rewrite, independent
	cross-check)
	<https://github.com/REAndroid/lib-global-metadata>
- katyscode/Il2CppVersions.cs — per-version stride table
	<https://github.com/djkaty/Il2CppInspector/blob/master/Il2CppInspector.Common/IL2CPP/MetadataVersions.cs>
- Joshua Peterson, "IL2CPP internals" (Unity blog, 2015–2017):
	<https://blog.unity.com/technology/il2cpp-internals-method-calls>
	and neighbouring posts on strings, generics, debugging, vtables,
	P/Invoke.
- Android packed relocations:
	<https://android.googlesource.com/platform/bionic/+/master/linker/linker_relocate.cpp>
- Mach-O chained fixups:
	<https://github.com/apple-oss-distributions/dyld> — see
	`dyld-1165.3/mach_o/ChainedFixups.cpp`.
- ARM64e pointer authentication (for future Mach-O work):
	<https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/pointer-auth-v7.pdf>

The `third_party/Il2CppDumper/` and
`third_party/lib-global-metadata/` vendored trees are the
authoritative sources when this document and the code disagree.

# r2unity - IL2CPP Wire Versions, Layout Deltas, And Unused Metadata

This document is a technical reference for:

1. Which `global-metadata.dat` wire versions exist, what changed in
	each, which fractional sub-versions affect the on-disk layout,
	and which Unity release each one maps to.
2. Which logical records inside `global-metadata.dat` and which
	native-binary structures are declared but not yet decoded by
	r2unity, with enough detail to add decoders against without
	re-reading Il2CppDumper.

The numbers and field deltas below are cross-checked against three
authoritative reference implementations vendored or cited:

- `third_party/Il2CppDumper/` — C# reference
	(`Il2CppDumper/Il2Cpp/MetadataClass.cs` is the per-version struct
	layout source of truth).
	<https://github.com/Perfare/Il2CppDumper>
- `third_party/lib-global-metadata/` — Java rewrite, independent
	cross-check of every record range.
	<https://github.com/REAndroid/lib-global-metadata>
- djkaty/Il2CppInspector — public Unity-version matrix.
	<https://github.com/djkaty/Il2CppInspector>

## 1. IL2CPP metadata wire versions

### 1.1 Wire versions observed on disk

The `int32_t` at file offset `4` of `global-metadata.dat` has only
ever taken these values:

```text
16, 19, 20, 21, 22, 23, 24, 27, 29, 31
```

`17, 18, 25, 26, 28, 30` were reserved but never shipped. The
Il2CppDumper dispatcher
(`third_party/Il2CppDumper/Il2Cpp/Metadata.cs:55`) rejects anything
outside `[16, 31]`:

```csharp
if (version < 16 || version > 31)
    throw new NotSupportedException(...);
```

### 1.2 Fractional sub-versions

Within each wire version Il2CppDumper tracks fractional sub-versions
(`24.1`, `24.2`, `24.3`, `24.4`, `24.5`, `27.1`, `27.2`, `29.1`).
These are **not** written to disk. They are reverse-engineered
signatures inferred by probing:

- section sizes divided by row counts (e.g. `assembliesSize /
	imageCount` → 48 B or 52 B tail)
- field counts and pointer layouts on the native side
	(`CodeRegistration` / `MetadataRegistration` struct size probes)
- the presence of specific arrays in the binary (`genericAdjustorThunks`
	exists from v24.5 onward)

For most wire versions the on-disk `.dat` is byte-identical across
the fractional family; the sub-version only selects how the native
binary's registration structures are laid out. A few exceptions do
leak into the `.dat`:

- `Il2CppRGCTXDefinition._data` widens from `int32` → `uint64` at
	**v27.2** (reference:
	`Il2CppDumper/Il2Cpp/MetadataClass.cs`, `type_post29` + `_data`
	fields). Row grows from 8 B to 12 B/16 B.
- `Il2CppMethodDefinition` gains `returnParameterToken` at **v31**.
	Row grows from 32 B to 36 B.
- All `customAttributeIndex` fields (on `TypeDef`, `MethodDef`,
	`FieldDef`, `ParameterDef`, `EventDef`, `PropertyDef`,
	`AssemblyDef`, `ImageDef`) are removed at **v24.1** in favour of
	token-based lookup. Affects row sizes of every def type.
- `Il2CppAssemblyNameDefinition.hashValueIndex` is removed at
	**v24.3+**, shrinking assembly rows by 4 B.
- Attribute tables (`attributesInfo` + `attributeTypes`) are
	replaced by a BLOB layout (`attributeDataRange` + `attributeData`)
	at **v29**. Same header slots, entirely new semantics.

### 1.3 Unity release → wire-version mapping

Reconstructed from Il2CppDumper's probe logic and Il2CppInspector's
published matrix. "Wire version" is what lands on disk; "Sub-version"
is the fractional binary-side refinement.

| Unity release            | Wire version | Sub-version | Notes                                                                  |
|--------------------------|--------------|-------------|------------------------------------------------------------------------|
| 5.3.0 – 5.3.5            | 21           | 21          | First public IL2CPP on iOS.                                            |
| 5.3.6 – 5.4              | 22           | 22          | Drops delegate-wrapper tables; adds `unresolvedVirtualCall*`.          |
| 5.5                      | 23           | 23          | Adds `interopData`, WinRT tables.                                      |
| 5.6 – 2017.x – 2018.2    | 24           | 24          | Adds `exportedTypeDefinitions`.                                        |
| 2018.3 – 2018.4          | 24           | 24.1        | Token-based attribute ranges; drops `customAttributeIndex` from defs.  |
| 2019.1 – 2019.3.6        | 24           | 24.2        | `Il2CppCodeGenModule` introduced; per-image method tables.             |
| 2019.3.7 – 2019.4.14     | 24           | 24.3        | WinRT factory table.                                                   |
| 2019.4.15 – 2019.4.20    | 24           | 24.4        | `genericAdjustorThunks` added; `hashValueIndex` dropped.               |
| 2019.4.21+               | 24           | 24.5        | Drops `customAttributeGenerators` from `CodeRegistration`.             |
| 2020.1                   | 24           | 24.4        | Same layout as 2019.4.15.                                              |
| 2020.2 – 2020.3          | 27           | 27          | `customAttributeCacheGenerator` moves per-image into `CodeGenModule`.  |
| 2021.1                   | 27           | 27.1        | Re-adds `genericAdjustorThunks`.                                       |
| 2021.2 – 2021.3          | 27           | 27.2        | RGCTX widens to 64 bits on disk; `Il2CppType` repacks `valuetype` bit. |
| 2022.1                   | 29           | 29          | Inline custom-attribute BLOB; `customAttributeGenerators` removed.     |
| 2022.2 – 2022.3          | 29           | 29.1        | `unresolvedVirtualCallPointers` splits into instance + static arrays.  |
| 2023.1 – 2023.2          | 31           | 31          | `Il2CppMethodDefinition` adds `returnParameterToken`.                  |
| 6000.x (Unity 6)         | 31           | 31          | Provisional — no on-disk change known.                                 |

Il2CppDumper's bundled README claims "Unity 5.3 – 2022.2", i.e.
wire versions 21–29. Support for wire v31 (Unity 2023+) is present
in current master via the `[Version(Min = 31)]` annotation on
`returnParameterToken` in `MetadataClass.cs`.

### 1.4 r2unity's current acceptance band

From `r2unity_parse_metadata()` in `src/lib/lib.c`:

```c
if (version < 24 || version > 31)  return NULL;
if (version < 27)       header_size = sizeof (v24);
else if (version < 29)  header_size = sizeof (v27);
else                    header_size = sizeof (v29);   /* 29, 30, 31 */
```

| Wire version | Status                            | Reality                                                                                    |
|--------------|-----------------------------------|--------------------------------------------------------------------------------------------|
| 16, 19, 20   | Rejected                          | Pre-Unity-5.5 builds, effectively extinct.                                                 |
| 21, 22, 23   | Rejected                          | Unity 5.3 – 5.5 IPAs exist but are rare.                                                   |
| 27           | Accepted via `_v27` header struct | Header correct. Row decoders still assume the v24.1+ layout; v27.2 RGCTX widening not handled (only affects RGCTX which is unused). |
| 29           | Accepted via `_v29` header struct | Header correct. Method rows decoded as 32 B. Reverse-P/Invoke BLOB path is the only v29-specific code. |
| 30           | Accepted                          | Never shipped on disk; dead slot, safe.                                                    |
| 31           | Accepted                          | Method rows decoded as 36 B (dedicated v31 branch); other row types still assume v24.1+ layout. |

**Bottom line**: the acceptance band covers every commercially
relevant Unity build from 2017 onward, but within the band the
fixed 88 / 32 / 40 / 48–52 strides in the row decoders are correct
only for a subset. A per-version size table is the single
highest-priority portability fix. The required values are itemised
in §4.

### 1.5 Architecture-independence of `global-metadata.dat`

The file contains **zero pointers**. Every scalar is 16- or 32-bit
little-endian; inline fixed arrays (`public_key_token[8]`) are
byte-addressed. The reference implementations (`Il2CppDumper`,
REAndroid Java port) both read the file with unconditional
`ReadInt32()` / `ReadUInt16()`; there is no architecture branch.

Therefore, for a given wire version, `global-metadata.dat` is
byte-identical across:

- iOS ARM64 / ARM64e
- Android arm64-v8a
- Android armv7a (32-bit)
- Android x86 / x86_64 (emulator)
- Windows x64 (`GameAssembly.dll`)
- macOS x64 / arm64 standalone (`GameAssembly.dylib`)
- Linux x64 (`GameAssembly.so`)
- UWP / HoloLens
- Nintendo Switch
- WebAssembly (wasm32)

r2unity's `RD_LE32` / `RD_LE16` helpers are correct for every
target. The only endianness concern is a big-endian host, which the
in-place `r_read_le32` sweep already handles.

The **native** side is not architecture-independent:
`Il2CppCodeRegistration`, `Il2CppMetadataRegistration`,
`Il2CppCodeGenModule`, and `Il2CppInteropData` all contain
host-width `ulong` pointers. Format and relocation semantics differ
between ELF, Mach-O and PE:

- 8-byte pointers: iOS ARM64/ARM64e, Android arm64-v8a, Windows
	x64, macOS, Switch, Linux x64.
- 4-byte pointers: Android armv7a, Android x86, WebAssembly (wasm32).

r2unity's ELF/Mach-O paths handle 64-bit correctly; 32-bit Android
ELF is parsed but not validated; WebAssembly is unsupported.

## 2. Tables declared but not decoded

`src/lib/lib.h` already lays out the full
`Il2CppGlobalMetadataHeader` with every offset/size pair, but
`src/lib/lib.c` consumes only six of them in general (`string`,
`stringLiteral`, `stringLiteralData`, `methods`, `typeDefinitions`,
`images`) plus `assemblies` / `referencedAssemblies` / v29
`attributesInfo`+`attributeTypes` for SBOM and reverse-P/Invoke.
Everything else is dead weight today.

Each subsection below gives the on-disk shape, the indexing
invariant, and the reverse-engineering value of decoding it.

### 2.1 `parameters` — `Il2CppParameterDefinition[]`

```c
uint32_t nameIndex;
uint32_t token;                  /* 0x08?????? */
int32_t  customAttributeIndex;   /* removed at v24.1 */
int32_t  typeIndex;              /* -> MetadataRegistration.types */
```

Size: **16 B** (v≤24.0) / **12 B** (v≥24.1). Sliced by
`methodDef.parameterStart`, length `methodDef.parameterCount`.

Value: every method currently dumps as `Method()`. With this table
we emit real C# signatures `Foo(string name, int count, Vector3
pos)`. Combined with `parameterDefaultValues` (§2.3) we also
recover optional parameters. This is the highest-leverage table
r2unity does not read today.

### 2.2 `fields` — `Il2CppFieldDefinition[]`

```c
uint32_t nameIndex;
int32_t  typeIndex;              /* -> MetadataRegistration.types */
int32_t  customAttributeIndex;   /* removed at v24.1 */
uint32_t token;                  /* 0x04??????, added at v19 */
```

Size: **12 B** (v≤24.0) / **8 B** (v≥24.1). Sliced by
`typeDef.fieldStart` / `typeDef.field_count`.

Value: names and types for every field. Indirectly unlocks
`MetadataRegistration.fieldOffsets` (§3.12), which is what actually
lets us label `ldr X, [x0, #0x18]` as `Player.health`. Without
fields, struct-internal code is unreadable.

### 2.3 `fieldDefaultValues` / `parameterDefaultValues` + BLOB

```c
typedef struct { int fieldIndex;     int typeIndex; int dataIndex; } Il2CppFieldDefaultValue;
typedef struct { int parameterIndex; int typeIndex; int dataIndex; } Il2CppParameterDefaultValue;
```

Each entry indexes into
`fieldAndParameterDefaultValueData` (raw BLOB heap), decoded by
`Il2CppExecutor.GetConstantValueFromBlob`
(`Il2CppDumper/Utils/Il2CppExecutor.cs:343`). The decoder handles
ECMA-335 primitive type codes (ECMA-335 §II.23.1.16):

```text
I1/U1/I2/U2/I4/U4/I8/U8      — little-endian scalars
R4/R8                         — IEEE-754 floats
STRING                        — compressed-int length + UTF-8 bytes
CHAR/BOOLEAN                  — 2 B / 1 B
SZARRAY                       — compressed length + elements
IL2CPP_TYPE_INDEX             — type reference
CLASS                         — null reference marker
```

v29 reuses this same compressed-int scheme for the custom-attribute
BLOB (§2.9).

Value: every `const string URL = "..."`, `static readonly int MaxHp
= 42`, and default argument value. Major for finding hardcoded
endpoints, feature flags, encryption constants.

### 2.4 `properties` — `Il2CppPropertyDefinition[]`

```c
uint32_t nameIndex;
int32_t  get;                    /* MethodDefIndex, -1 if no getter */
int32_t  set;                    /* MethodDefIndex, -1 if no setter */
uint32_t attrs;                  /* MethodSemanticsAttributes, ECMA-335 §II.23.1.12 */
int32_t  customAttributeIndex;   /* ≤24 */
uint32_t token;                  /* ≥19, 0x17?????? */
```

Value: rejoin `get_X` / `set_X` method pairs into a single C#
property. Auto-properties identifiable via `<X>k__BackingField`
patterns. `[JsonProperty]` / `[SerializeField]` most often decorate
properties.

### 2.5 `events` — `Il2CppEventDefinition[]`

```c
uint32_t nameIndex;
int32_t  typeIndex;              /* -> MetadataRegistration.types */
int32_t  add;                    /* MethodDefIndex for add_X      */
int32_t  remove;                 /* MethodDefIndex for remove_X   */
int32_t  raise;                  /* MethodDefIndex for raise_X, often -1 */
int32_t  customAttributeIndex;   /* ≤24 */
uint32_t token;                  /* ≥19, 0x14?????? */
```

Value: C# `event` declarations. Labels every `add_` / `remove_`
delegate-subscription method. Unity `UnityEvent`s, C# events on
`MonoBehaviour`, INotifyPropertyChanged patterns.

### 2.6 Generics: `genericContainers` + `genericParameters` +
`genericParameterConstraints`

```c
typedef struct {    /* Il2CppGenericContainer, 16 B */
    int32_t ownerIndex;                  /* TypeDef or MethodDef index  */
    int32_t type_argc;
    int32_t is_method;                   /* 0 class, 1 method           */
    int32_t genericParameterStart;
} Il2CppGenericContainer;

typedef struct {    /* Il2CppGenericParameter, 16 B */
    int32_t  ownerIndex;
    uint32_t nameIndex;                  /* "T", "TKey", …              */
    int16_t  constraintsStart;
    int16_t  constraintsCount;
    uint16_t num;                        /* argument position            */
    uint16_t flags;                      /* GenericParameterAttributes
                                             (variance + constraint kinds),
                                             ECMA-335 §II.23.1.7         */
} Il2CppGenericParameter;

/* genericParameterConstraints: raw int32_t[] of type indices */
```

Value: reconstruct `Dictionary<TKey, TValue> where TKey :
IComparable<TKey>`. Gate for using `methodSpecs` / `genericInsts`
(§3) to emit real instantiation names like `List<int>::Add`.

### 2.7 `interfaces` + `interfaceOffsets` + `vtableMethods`

- `interfaces` — raw `int32_t[]`; per-type via
	`typeDef.interfacesStart` / `typeDef.interfaces_count`. Each
	entry is a type index (into `MetadataRegistration.types`).
- `interfaceOffsets` — `Il2CppInterfaceOffsetPair[] = { int
	interfaceTypeIndex; int offset; }` (8 B). Per-type via
	`typeDef.interfaceOffsetsStart` / `interface_offsets_count`. Maps
	each implemented interface to the vtable slot where its methods
	begin.
- `vtableMethods` — raw `uint32_t[]`; each slot is an
	`EncodedMethodIndex`:

	```c
	usageType = (idx & 0xE0000000) >> 29;   /* Il2CppMetadataUsage */
	decoded   = (version >= 27)
	              ? ((idx & 0x1FFFFFFE) >> 1)
	              : (idx & 0x1FFFFFFF);
	```

	`usageType == 3` → concrete method definition,
	`usageType == 6` → `methodSpecs` (§3.11) index.

Value: fully resolved virtual dispatch. Every `callvirt` / indexed
vtable load gets a real method name. `class Foo : IDisposable,
IEnumerable<int>` declarations. Find-all-implementers searches for a
given interface.

### 2.8 `nestedTypes` — `TypeDefinitionIndex[]`

Raw `int32_t[]`; per-type via `typeDef.nestedTypesStart` /
`nested_type_count`. Reconstructs `Outer.Inner` hierarchies —
important for accurate symbol naming in compiler-generated types
(`<DisplayClass>` etc.).

### 2.9 Attributes — two very different encodings

**Pre-v29 (wire 21..27.2)** — token-range table + flat type list +
native-side generators:

- `attributesInfo` —
	`Il2CppCustomAttributeTypeRange[] = { uint token; int start; int
	count; }` (12 B; 8 B pre-24.1 without the token).
- `attributeTypes` — flat `int32_t[]` of attribute type indices
	(into `MetadataRegistration.types`).
- `CodeRegistration.customAttributeGenerators` — one
	`void(void*)` function pointer per range. Its body `new`s up the
	attribute instance(s), baking constructor arguments as immediate
	operands. Reading the thunk recovers the arguments; this is what
	Il2CppDumper's `GetCustomAttributeValuesFromGenerator` does.

**v29+** — inline BLOB:

- `attributesInfo*` header slots repurposed as `attributeDataRange`:
	`{ uint token; uint startOffset; }` array.
- `attributeTypes*` header slots repurposed as `attributeData`:
	raw BLOB heap.
- BLOB encoding matches ECMA-335 §II.23.3 `CustomAttrib` extended
	with IL2CPP's 7-bit compressed-int (same as ECMA-335 §II.23.2):
	leading compressed count of constructor references; each ref
	encoded as a method index plus inline-serialized argument
	values.
- Decoded by `Il2CppDumper/Utils/CustomAttributeDataReader.cs`.
- `customAttributeGenerators` no longer exists in `CodeRegistration`.

Value: `[Obsolete]`, `[Serializable]`, `[SerializeField]`,
`[Preserve]`, `[RequireComponent(typeof(Rigidbody))]`,
`[RuntimeInitializeOnLoadMethod]`,
`[DllImport("x.dll",EntryPoint="y")]`. `[Preserve]` in particular
flags code reachable only via reflection — a high-value RE signal.

Cross-reference:
<https://ecma-international.org/publications-and-standards/standards/ecma-335/>

### 2.10 Pre-v27 metadata usage tables

- `metadataUsageLists` — `Il2CppMetadataUsageList[] = { uint start;
	uint count; }` (8 B).
- `metadataUsagePairs` — `Il2CppMetadataUsagePair[] = { uint
	destinationIndex; uint encodedSourceIndex; }` (8 B).

`encodedSourceIndex`'s top three bits encode `Il2CppMetadataUsage ∈
{ Invalid, TypeInfo, Il2CppType, MethodDef, FieldInfo,
StringLiteral, MethodRef }`. `destinationIndex` is the slot in the
native `MetadataRegistration.metadataUsages[]` pointer table (§3.14).

Value: on v19–v26 builds, the compiled code is full of `ldr X,
[g_MetadataUsages + offset]` loads. These tables plus
`metadataUsages` together let a decoder label each slot as
`[TypeInfo] System.String`, `[StringLiteral_42] "hello world"`, etc.
From v27+ the mechanism was replaced by per-site inline globals
lazily initialized by `Il2CppCodeGenModule.moduleInitializer`, and
these tables disappear from the `.dat`.

### 2.11 `assemblies` + `referencedAssemblies`

```c
typedef struct {    /* Il2CppAssemblyDefinition */
    int32_t  imageIndex;
    uint32_t token;                      /* ≥24.1, 0x20?????? */
    int32_t  customAttributeIndex;       /* ≤24 */
    int32_t  referencedAssemblyStart;    /* ≥20 */
    int32_t  referencedAssemblyCount;    /* ≥20 */
    Il2CppAssemblyNameDefinition aname;
} Il2CppAssemblyDefinition;

typedef struct {    /* Il2CppAssemblyNameDefinition */
    uint32_t nameIndex, cultureIndex, publicKeyIndex;
    uint32_t hashValueIndex;             /* removed at v24.3+ */
    uint32_t hash_alg;                   /* AssemblyHashAlgorithm enum */
    int32_t  hash_len;
    uint32_t flags;                      /* AssemblyFlags, ECMA-335 §II.22.2 */
    int32_t  major, minor, build, revision;
    uint8_t  public_key_token[8];        /* low 8 bytes of SHA1(publicKey), byte-reversed */
} Il2CppAssemblyNameDefinition;
```

Value: distinguish Unity-shipped assemblies (`mscorlib.dll`,
`UnityEngine.*`, `UnityEditor.*`) from game assemblies. Recover
`AssemblyVersion` quadruples and strong-name public-key tokens.
Dependency graph between assemblies — needed for emitting a
compilable C# skeleton or for filtering dumps by assembly.

This is the one large "still-declared" table r2unity **does**
decode today, and it drives the `-S` SBOM output.

### 2.12 `fieldRefs` (v19+)

```c
typedef struct { int typeIndex; int fieldIndex; } Il2CppFieldRef;
```

Canonical list of field references used at metadata-usage
destinations, typically for fields of generic-instantiation types
that cannot be addressed directly by `(typeDef, fieldDef)`.
Dereferences the `FieldInfo` kind of metadata-usage slot (§2.10).

### 2.13 `exportedTypeDefinitions` (v24+)

Raw `TypeDefinitionIndex[]`. Per-image via
`imageDef.exportedTypeStart` / `exportedTypeCount`. These are .NET
type forwarders — matters for faithfully reproducing runtime
assemblies (`System.Runtime.dll` re-exports into `mscorlib.dll`).
Optional for most application-level dumping.

### 2.14 RGCTX tables (≥v24.1 on disk; moved to
`codeGenModule` from v24.2)

```c
typedef struct {    /* Il2CppRGCTXDefinition (v≤27.1) */
    int32_t type;                        /* Il2CppRGCTXDataType enum  */
    Il2CppRGCTXDefinitionData data;      /* typically int rgctxDataDummy */
} Il2CppRGCTXDefinition;                 /* 8 B                         */
/* v≥27.2: type_post29 widens to ulong; _data widens to ulong -> 16 B */
```

`Il2CppRGCTXDataType` = `{ Invalid, Type, Class, Method, Array,
Constrained }`. Per-type via `typeDef.rgctxStartIndex` /
`rgctxCount` (pre-v24.1); per-method similarly. From v24.2 onward
these tables move per-image into
`Il2CppCodeGenModule.rgctxs` + `rgctxRanges`.

Value: Runtime Generic Context — per-instantiation lookup table.
Resolves generic-virtual-dispatch sites and `typeof(T)` /
`default(T)` accesses inside generic methods. "The 3rd RGCTX slot
of `List<T>::Sort`" → concrete type/method resolution.

### 2.15 `fieldMarshaledSizes`

```c
typedef struct { int fieldIndex; int typeIndex; int size; } Il2CppFieldMarshaledSize;
```

Marshalled size for `[MarshalAs]` / `[StructLayout]` interop
structs. Relevant only when the game talks heavily to native
plugins via custom marshalling; safe to ignore for plain RE.

### 2.16 `unresolvedVirtualCallParameterTypes` /
`unresolvedVirtualCallParameterRanges` (v22+)

```c
/* Types: raw TypeIndex[] */
typedef struct { int start; int length; } Il2CppRange;
```

Correlates with
`CodeRegistration.unresolvedVirtualCallPointers` (§3.5 of
`r2unity.md`). Lets us name each unresolved-virtual-call stub with
its parameter signature.

### 2.17 `windowsRuntimeTypeNames` + `windowsRuntimeStrings` +
`windowsRuntimeFactoryTable`

UWP / HoloLens only. For iOS + Android + desktop builds these sizes
are always zero; a defensive parser bounds-checks and skips.

## 3. Native binary — pointer arrays we don't follow

`src/lib/elf.c` and `src/lib/macho.c` currently locate exactly one
array: `CodeRegistration.methodPointers` (or its v24.2+ per-image
equivalent, if the heuristic happens to land on the right
`Il2CppCodeGenModule`). The registration structures actually expose
many more pointer arrays, each with its own metadata table partner.
Each entry below is a `{ ulong count; ulong ptr; }` pair inside the
registration (see §3 of `doc/r2unity.md` for the full struct).

### 3.1 `methodPointers` (≥v24.1 global; v≥24.2 per-module)

What r2unity extracts today. From v24.2 onwards this field is on
`Il2CppCodeGenModule`, **not** `CodeRegistration`, and there is one
module per image. If r2unity finds a single `{count, ptr}` on a
v24.2+ binary it is only extracting **one image's** methods.
Walking `codeGenModules[]` and enumerating each module is mandatory
for full coverage.

### 3.2 `invokerPointers`

Reflection-dispatch invokers — one per unique method-signature
shape (return type + param count + blittable-ness). Methods
reference them via `methodDef.invokerIndex` (≤v24.1) or
`Il2CppCodeGenModule.invokerIndices[]` (v≥24.2).

Value: naming invokers doesn't rename methods directly, but it
clusters methods by signature and makes reflection-based hook
points discoverable.

### 3.3 `customAttributeGenerators` (pre-v29)

Per-range `void(void*)` — each one news up the attribute objects
for one range. Given `attributesInfo` (§2.9), a tiny
arch-specific interpreter walking the generator body recovers every
attribute argument (strings, `typeof` references, enums). Required
for pre-v29 reverse-P/Invoke DLL-name recovery.

### 3.4 `reversePInvokeWrappers` (v22+)

Array of native-to-managed thunks for `[MonoPInvokeCallback]`
methods. Every callback the game registers with a C library routes
through these. Naming them `ReversePInvoke_<method>` lights up
native plugin callback surfaces (ads SDKs, analytics, audio,
networking).

### 3.5 `genericMethodPointers` + `genericAdjustorThunks`

Array of function pointers for every generic-method instantiation
present in the binary. Bridged to metadata via
`MetadataRegistration.genericMethodTable` and `methodSpecs`:

```c
foreach (genericMethodTable[k]) {
    spec = methodSpecs[k.genericMethodIndex];
    addr = genericMethodPointers[k.indices.methodIndex];
    /* emit "List<int>::Add @ 0x401234" */
}
```

`genericAdjustorThunks` (v24.5, v27.1+) are struct-vs-class `T`
fixup thunks: `mov x0, [x0, #8]; b real_body` (skip the
`Il2CppObject` header for value-type `T`). Labelling both separates
the thunk from the real body.

Value: enormous on generics-heavy games. Without this, every
instantiation is a distinct unnamed `sub_xxx`.

### 3.6 `unresolvedVirtualCallPointers` (v22..v29); split at v29.1
into `unresolvedInstanceCallPointers` +
`unresolvedStaticCallPointers`

Stubs the runtime patches when a virtual / interface call target
becomes known. 1:1 with the metadata ranges in §2.16.

### 3.7 `interopData` (v23+)

Array of `Il2CppInteropData`, each containing a bundle of
P/Invoke / COM-interop function pointers for one type. See §3.6 of
`doc/r2unity.md` for the struct. Lights up every `[DllImport]`
target wrapper and every COM bridge (CCW/RCW).

### 3.8 `codeGenModules` (v24.2+)

One `Il2CppCodeGenModule` per image. Struct definition in §3.7 of
`doc/r2unity.md`.

Highlights that are particularly useful on obfuscated games:

- `moduleInitializer` — each image's `<Module>..cctor`, the per-
	image static-init entry point.
- `staticConstructorTypeIndices` — types whose `.cctor` runs at
	load. Prime targets when keys/strings are materialised at
	startup.
- `adjustorThunks` + `adjustorThunkCount` — the generic-method
	struct-vs-class fixup thunks.

### 3.9 `MetadataRegistration.types` — the `Il2CppType` pool

Every `typeIndex` anywhere in metadata indexes this array. Without
it, we can't resolve any method return type, parameter type, field
type, or interface name that goes beyond a plain `TypeDef`.

`Il2CppExecutor.GetTypeName`
(`third_party/Il2CppDumper/Utils/Il2CppExecutor.cs:61`) walks it
recursively to produce qualified names, decoding the ECMA-335
`ELEMENT_TYPE_*` byte (§II.23.1.16) to branch on:

- primitive (`I1`, `U4`, `R8`, …)
- `VALUETYPE`, `CLASS` (→ `typeDef.name`)
- `SZARRAY`, `ARRAY` (→ rank + element recursion)
- `PTR`, `BYREF` (→ pointee recursion)
- `VAR`, `MVAR` (→ generic-parameter name)
- `GENERICINST` (→ open type + type-argument tuple from
	`genericInsts`)
- `FNPTR`, `MODOPT`, `MODREQ`

### 3.10 `MetadataRegistration.genericInsts` + `genericClasses`

Pool of concrete type-argument tuples (`(int)`, `(string, int)`,
`(Player, Dictionary<int, float>)`, …). Every generic-instantiation
reference in the binary indexes into this array.

### 3.11 `MetadataRegistration.methodSpecs` + `genericMethodTable`

See §3.9 of `doc/r2unity.md` for the structs and consumption
pattern. `GetMethodSpecName` reconstructs the full
`Foo<string>.Bar<int>` form.

### 3.12 `MetadataRegistration.fieldOffsets`

- Pre-v22: flat `int32_t[]` indexed by global field index.
- v22+: pointer-of-pointers — one `int32_t *` per type definition,
	pointing at that type's field-offset array.

`GetFieldOffsetFromIndex`
(`third_party/Il2CppDumper/Il2Cpp/Il2Cpp.cs:274`) has the full
logic, including the `-8` / `-16` adjustment for non-static
value-type fields (to account for the `Il2CppObject` header:
`MonitorData *` + `Il2CppClass *`; see ECMA-335 §II.25.4 and Unity's
`Il2CppClass.cs`).

Value: the byte-level layout of every class / struct. Once read, we
can label every `ldr/str [this + N]` with a field name and type.
Second only to method pointers in RE value.

### 3.13 `MetadataRegistration.typeDefinitionsSizes`

```c
typedef struct {
    uint32_t instance_size;              /* sizeof(object) w/ Il2CppObject header */
    uint32_t native_size;                /* P/Invoke marshal size, 0 if N/A       */
    uint32_t static_fields_size;
    uint32_t thread_static_fields_size;
} Il2CppTypeDefinitionSizes;             /* 16 B                                  */
```

Total size of every class / struct. Distinguishes 16-byte vs 24-byte
Vector types. Gives us the element stride for array indexing
(`ldr X, [Xn + Xindex * instance_size]`). `static_fields_size`
lets us label every static-field access once we find the per-class
statics pointer.

### 3.14 `MetadataRegistration.metadataUsages` (v19..v26)

The native side of §2.10. Huge `void *[]`. Each slot is an
`Il2CppClass *`, `Il2CppType *`, `MethodInfo *`, `FieldInfo *`,
managed `string *`, or generic `MethodInfo *` — decoded by
combining with `metadataUsageLists` + `metadataUsagePairs`.

Value: on pre-v27 binaries, labelling this array turns every
indirect metadata load in the disassembly into a readable reference.
From v27+ the mechanism was replaced by per-site inline globals and
this array is gone; labels must instead be derived per-site via
`codeGenModule.staticConstructorTypeIndices` + `moduleInitializer`
and code scanning.

## 4. Implications for r2unity

### 4.1 Portability fixes (correctness)

1. **Version-gated entry sizes.** Replace the literal 88 / 32 / 40
	in `r2unity_get_type_definitions` /
	`r2unity_get_method_definitions` / `r2unity_get_images` with a
	lookup keyed on wire version. Critical values:

	- `Il2CppMethodDefinition`:
		- v≤24.0: 52 B (includes `customAttributeIndex`,
			`methodIndex`, `invokerIndex`, `delegateWrapperIndex`,
			`rgctxStartIndex`, `rgctxCount`).
		- v24.1 – v30: 32 B (current assumption — correct only here).
		- v31+: 36 B (`returnParameterToken` added).
	- `Il2CppImageDefinition`:
		- v≤18: 24 B.
		- v19 – v23: 28 B (adds `token`).
		- v24 – v24.0: 36 B (adds exported-type range).
		- v24.1+: 40 B (adds `customAttribute` range) — current
			assumption.
	- `Il2CppTypeDefinition`:
		- pre-v24: extra legacy fields
			(`delegateWrapperFromManagedToNativeIndex`,
			`marshalingFunctionsIndex`, `ccwFunctionIndex`,
			`guidIndex`).
		- v24 – v24.0: `customAttributeIndex` + `byrefTypeIndex` +
			`rgctxStartIndex` + `rgctxCount`.
		- v24.1+: 88 B — current assumption.

	Always source the size from
	`third_party/Il2CppDumper/Il2Cpp/MetadataClass.cs` per wire
	version.

2. **Per-version header struct.** Distinguish v24.0 from v24.1+
	headers so assembly / image entries are decoded correctly on
	unresolved-virtual-call pointer arrays.

	distinct wire version we claim to support (21, 22, 23, 24, 27,
	29, 31). Pull candidates from the Il2CppDumper issue-tracker
	corpus if new ones are needed.

4. **Fail loudly on truly unknown versions.** Current code treats
	v30/v31 as v29-shaped; that's correct for the header but should
	be an informative warning (not silence) when row decoders
	haven't been audited for a new sub-version.

### 4.2 Feature additions (coverage)

Ordered by RE value vs. implementation effort:

1. `parameters` table — small, unlocks real method signatures.
2. `fields` table — small, prerequisite for field offsets.
3. `CodeRegistration` / `MetadataRegistration` structural anchor
	detection — unlocks everything in §3.
4. `MetadataRegistration.types` + `fieldOffsets` +
	`typeDefinitionsSizes` — class layouts + field labels.
5. `codeGenModules` walk for v24.2+ — currently dropping most
	methods on modern Unity.
6. Attributes bundle:
	- pre-v29: `attributesInfo` + `attributeTypes` +
		`customAttributeGenerators` thunk interpreter
	- v29+: `attributeDataRange` + `attributeData` BLOB decoder
7. Generics bundle: `genericContainers` + `genericParameters` +
	`methodSpecs` + `genericInsts` + `genericMethodPointers` +
	`genericMethodTable`. Recovers `List<int>::Add`-style native
	functions.
8. `properties` + `events` — recompose `get_X`/`set_X`/`add_X` /
	`remove_X` into clean declarations.
9. `metadataUsageLists/Pairs` + `MetadataRegistration.metadataUsages`
	— pre-v27 only, labels every indirect metadata load.
10. `interfaces` + `interfaceOffsets` + `vtableMethods` — full
	resolved virtual dispatch tables.
11. `nestedTypes` — correct `Outer.Inner` naming.
12. Richer SBOM: native dependencies (`DT_NEEDED`, `LC_LOAD_DYLIB`,
	PE import table), file hashes, ELF build IDs, Mach-O UUIDs, PE
	debug GUIDs, SPDX output.
13. Manual `-a`/`-c` pointer read with pointer-width detection.

### 4.3 Architecture-neutral vs architecture-branching code

- `.dat` parser stays architecture-neutral. No branching needed.
- `CodeRegistration` / `MetadataRegistration` walkers must be
	written with `ptrsz = is64bit ? 8 : 4` and `{u32|u64}` readers
	accordingly. This generalises cleanly from the existing
	Mach-O / ELF scanner scaffolding.

### 4.4 Format-specific scanner gaps

- **ELF**: packed Android relocations (`DT_ANDROID_RELA =
	0x60000010`, `DT_ANDROID_RELR = 0x6fffe000`) not handled —
	Play-Store builds compressed with packed relocs read as "no
	`DT_VERSYM`/`DT_VERNEED` not consulted (would help identify
	Unity version from the `.so` directly). `ET_EXEC` vs `ET_DYN`
	branching (we assume PIC shared objects).
- **Mach-O**: 32-bit, FAT multi-slice selection, `LC_DYLD_CHAINED_FIXUPS`
	(`0x34`) interpretation, `LC_ENCRYPTION_INFO_64` detection
	(FairPlay-encrypted App Store IPAs silently return garbage),
	ARM64e pointer-authentication awareness.
	`IMAGE_DIRECTORY_ENTRY_BASERELOC` (not needed today, link-time
	`ImageBase` is concrete).

### 4.5 Sources and caveats

- The Unity 6 (`6000.x`) row in §1.3 is provisional. The mapping is
	inferred from `[Version(Min = 31)] returnParameterToken` being
	the current high-water mark in `Il2CppDumper` plus release-date
	alignment. No Unity 6 metadata binary was directly inspected.
- Il2CppDumper's bundled snapshot is what drives these layout
	tables; newer Unity releases may introduce a `v33` / `v31.x`
	sub-version that won't be caught until the vendored copy is
	refreshed. A quarterly bump of `third_party/Il2CppDumper/` is
	the lightest way to track new Unity releases.
- Il2CppInspector's `MetadataClasses.cs` / `MetadataVersions.cs` is
	an independent second opinion; pulling it as a cross-check would
	let us confirm a few uncertain sub-version boundaries (notably
	the 2020.x → v24.4 vs v27 transition month).

## 5. Further reading

- ECMA-335, "Common Language Infrastructure" (6th edition) — the
	primary source for token encoding (§II.22), metadata BLOB
	signatures (§II.23.2, §II.23.3), element-type codes
	(§II.23.1.16), attribute enums (`TypeAttributes`,
	`MethodAttributes`, `MethodSemanticsAttributes`,
	`GenericParameterAttributes`, `AssemblyFlags`), instance layout
	and `Il2CppObject` header (§II.25.4), strong-name public-key
	derivation (§II.6.3).
	<https://ecma-international.org/publications-and-standards/standards/ecma-335/>
- ECMA-334, "C# Language Specification" — attribute semantics,
	property/event syntactic sugar.
	<https://ecma-international.org/publications-and-standards/standards/ecma-334/>
- Il2CppDumper — vendored reference implementation.
	`Il2Cpp/MetadataClass.cs` is the authoritative per-version
	layout source; `Utils/Il2CppExecutor.cs` is the authoritative
	BLOB decoder.
	<https://github.com/Perfare/Il2CppDumper>
- Il2CppInspector — per-version deltas and Unity-version matrix.
	<https://github.com/djkaty/Il2CppInspector>
- REAndroid/lib-global-metadata — independent Java port.
	<https://github.com/REAndroid/lib-global-metadata>
- Unity blog series on IL2CPP internals (method calls, generics,
	strings, P/Invoke):
	<https://blog.unity.com/technology/il2cpp-internals-method-calls>
- Android linker packed-relocation format:
	<https://android.googlesource.com/platform/bionic/+/master/linker/linker_relocate.cpp>
- Mach-O chained fixups (dyld source):
	<https://github.com/apple-oss-distributions/dyld>
- PE/COFF specification (Microsoft):
	<https://learn.microsoft.com/windows/win32/debug/pe-format>

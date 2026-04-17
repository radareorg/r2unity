# r2unity - Managed/Native Interop In IL2CPP

This document is a technical reference for how managed ↔ native
interop is actually represented in `global-metadata.dat` and in the
IL2CPP native binary, which parts r2unity decodes today (`-P` and
`-R`), which parts it does not, and where the missing data lives.

## 0. The three data sources for Unity IL2CPP interop

Any correct interop reconstruction needs to fuse information from
three places.

### 0.1 Managed-side method flags (cheap, always available)

Every `Il2CppMethodDefinition.flags` word is an ECMA-335
`MethodAttributes` bitmask (ECMA-335 §II.23.1.10). The relevant bit
for P/Invoke is:

```c
#define METHOD_ATTRIBUTE_PINVOKE_IMPL   0x2000  /* bit 13 */
```

When this bit is set, the method is declared in managed code with
`extern ... [DllImport("...")]`, meaning the CLR runtime (or
IL2CPP's equivalent) must resolve and call a native symbol on its
behalf.

This check is architecture-independent and does not require native
binary parsing. It is all r2unity's `-P` uses today.

### 0.2 Managed-side custom attributes (medium, v29+ is free)

The flag tells you *a* method is P/Invoke; the actual target DLL
and entry-point come from custom attributes:

- `System.Runtime.InteropServices.DllImportAttribute` — managed →
	native. Carries `Value` (DLL name), `EntryPoint`, `CharSet`,
	`SetLastError`, `CallingConvention`, `ExactSpelling`,
	`BestFitMapping`, `ThrowOnUnmappableChar`.
- `AOT.MonoPInvokeCallbackAttribute` — native → managed. Marks a
	managed method as callable from native code. Takes one argument:
	the delegate type it bridges (`typeof(MyCallbackDelegate)`).
- `System.Runtime.InteropServices.UnmanagedCallersOnlyAttribute`
	(.NET 5+) — newer replacement for `MonoPInvokeCallback`. Takes
	optional `CallConvs[]` and `EntryPoint`.

On **v29+** metadata, attribute constructor arguments are stored
inline in an ECMA-335-like BLOB inside the metadata file itself
(see §2.11 of `doc/future.md`). r2unity decodes this today for
reverse-P/Invoke.

On **pre-v29** metadata, constructor arguments are not in the
`.dat` at all. They exist only as immediate operands baked into
tiny native thunks in `CodeRegistration.customAttributeGenerators`
— one thunk per `Il2CppCustomAttributeTypeRange`. Recovering the
arguments means running a small arch-specific interpreter over the
thunk. r2unity does not do this today.

### 0.3 Native-side registration structures (rich, unused by r2unity)

The IL2CPP runtime exposes interop targets through several fields
on `Il2CppCodeRegistration` and through one per-type struct:

```c
/* Il2CppCodeRegistration (relevant slice) */
ulong reversePInvokeWrapperCount;
ulong reversePInvokeWrappers;        /* v22+ */
ulong customAttributeCount;
ulong customAttributeGenerators;     /* removed at v29 */
ulong interopDataCount;
ulong interopData;                   /* v23+ */

/* Il2CppInteropData — one per type involved in P/Invoke or COM */
typedef struct {
    ulong delegatePInvokeWrapperFunction;
    ulong pinvokeMarshalToNativeFunction;
    ulong pinvokeMarshalFromNativeFunction;
    ulong marshalToNativeFunction;
    ulong marshalFromNativeFunction;
    ulong marshalCleanupFunction;
    ulong ccwFunction;                 /* COM CCW               */
    ulong guid;                        /* COM IID ptr           */
    ulong type;                        /* -> Il2CppType *       */
} Il2CppInteropData;
```

This is where the **real native VAs** live: the actual marshalled
thunk that converts managed arguments to native, calls the
`[DllImport]` target, and unmarshals return values. r2unity does
not read any of this today.

## 1. Managed → native: `[DllImport]` and r2unity's `-P`

### 1.1 How `[DllImport]` is represented

Given:

```csharp
[DllImport("libfoo.so", EntryPoint = "foo_init", CharSet = CharSet.Ansi)]
public static extern int FooInit(string name);
```

On every wire version:

- `FooInit`'s `MethodDef` has `flags & PINVOKE_IMPL` set.
- `iflags` (ECMA-335 `MethodImplAttributes`, §II.23.1.11) carries
	`CodeTypeMask = 3` (IL/Native/OPTIL/Runtime) and
	`ManagedMask = 4` (managed vs unmanaged).

On v29+:

- An `Il2CppCustomAttributeTypeRange` exists with a token matching
	`FooInit`'s `0x06??????` MethodDef token. Its BLOB rows reference
	a constructor of `System.Runtime.InteropServices.DllImportAttribute`
	and serialize the argument values inline:
	- `Value` ("libfoo.so") — positional ctor arg, ECMA-335
		`STRING` encoding (compressed length + UTF-8 bytes)
	- `EntryPoint` ("foo_init") — named-argument kind
	- other properties as named arguments

On pre-v29:

- The same range table exists
	(`attributesInfo` + `attributeTypes`) but carries only the
	attribute **type list**, not values. The values are immediate
	operands inside
	`CodeRegistration.customAttributeGenerators[i]` on the native
	side.

On v23+ (independent of metadata version):

- An `Il2CppInteropData` entry for the declaring type carries
	`pinvokeMarshalToNativeFunction` — a pointer to the native thunk
	that actually calls `dlsym("libfoo.so", "foo_init")` (or
	statically links, depending on platform) and marshals the
	`string name` argument.

### 1.2 What r2unity's `-P` does today

`r2unity_enumerate_pinvokes()` (in `src/lib/lib.c`) currently:

1. Decodes all `Il2CppMethodDefinition` rows.
2. Filters by `flags & IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL`
	(`0x2000`).
3. Builds a qualified name from
	`typeDef[declaringType].name` + `"."` + `methodName`.
4. Resolves the owning image from the
	`typeIndex -> imageIndex` map.

This is intentionally simple and entirely metadata-driven. It
identifies the managed side completely and says nothing about the
native side.

Each emitted `R2UnityInterop` row (declared in `src/lib/lib.h`)
carries:

```c
int32_t  method_index;
uint32_t token;
int32_t  image_index;
uint16_t flags;            /* copy of methodDef.flags  */
uint16_t iflags;           /* copy of methodDef.iflags */
char    *name;             /* "Ns.Class.Method"        */
char    *image_name;       /* "Assembly-CSharp"        */
char    *dll_name;         /* NULL until attribute decoding lands */
char    *entry_name;       /* NULL until attribute decoding lands */
ut64     wrapper_va;       /* 0 — requires interopData walk       */
uint32_t wrapper_index;    /* UINT32_MAX — requires the same      */
uint8_t  kind;             /* R2U_INTEROP_PINVOKE                  */
uint8_t  confidence;
```

So `-P` today answers:

- which managed methods are marked as P/Invoke
- which assembly/image they belong to

and does not answer:

- which native library they import from (needs
	`DllImportAttribute.Value`)
- which symbol name they import (needs
	`DllImportAttribute.EntryPoint`, defaulting to the method name)
- where the marshalled thunk lives in memory (needs
	`Il2CppInteropData.pinvokeMarshalToNativeFunction`)

### 1.3 What is needed to complete `-P`


- Walk `attributeDataRange[]`. For each entry, fetch the BLOB at
	`attributeData + startOffset`. Decode a compressed-int count,
	then for each attribute: resolve the constructor method index,
	follow it to its declaring type, match against
	`DllImportAttribute`, then decode positional + named ctor args
	using ECMA-335 §II.23.3's CustomAttrib format (extended with
	IL2CPP's 7-bit compressed-int). `Il2CppDumper`'s
	`Utils/CustomAttributeDataReader.cs` is the reference decoder.


- Locate `CodeRegistration` structurally (§3 of `doc/r2unity.md`),
	walk `customAttributeGenerators[range_index]`, step through the
	native thunk body, recover immediate operands that correspond to
	`string` ctor arguments. Architecture-specific interpreter work.

**For native target recovery (all versions):**

- Walk `interopData[]` (v23+). For each entry, record
	`pinvokeMarshalToNativeFunction` as the wrapper VA for every
	managed P/Invoke method on that type.

## 2. Native → managed: callbacks and r2unity's `-R`

### 2.1 Two attribute families

Unity has historically supported two ways to declare a native-
callable managed method. They predate the general `UnmanagedCallersOnly`
attribute introduced by the .NET runtime at .NET 5.

**`[MonoPInvokeCallback]`** (Mono-era, still dominant in Unity):

```csharp
[MonoPInvokeCallback(typeof(MyCallbackDelegate))]
static int OnNativeEvent(int code, IntPtr data) { ... }
```

Always passes one argument: the delegate type the method bridges.
Used by virtually every iOS SDK that takes a C callback (Adjust,
Firebase, IronSource, Unity Ads, audio drivers, etc.).

**`[UnmanagedCallersOnly]`** (.NET 5+):

```csharp
[UnmanagedCallersOnly(EntryPoint = "my_exported_symbol")]
static int MyExport() { ... }
```

Optional named args `CallConvs` and `EntryPoint`. Newer Unity
versions (using newer BCLs) will emit this for games that migrate.

Both end up with a native trampoline that translates native calling
conventions to IL2CPP calling conventions before entering the
managed method body.

### 2.2 `CodeRegistration.reversePInvokeWrappers` (v22+)

Physical address of the native trampoline stub for each such
managed method:

```c
ulong reversePInvokeWrapperCount;
ulong reversePInvokeWrappers;     /* ut64*, indexed by wrapperIndex */
```

The link from a managed method to its wrapper index comes from
the native side too:

- Pre-v24.2: `methodDef.reversePInvokeWrapperIndex` used to exist
	in the 52 B row layout (removed at v24.1 as part of the
	token-based-lookup overhaul).
- v24.2+:
	`Il2CppCodeGenModule.reversePInvokeWrapperIndices[]` — one
	`int32_t` per method in the module; non-negative entries index
	into the global `reversePInvokeWrappers[]`.

Naming each slot `ReversePInvoke_<method>` immediately reveals
every native-plugin callback surface.

### 2.3 What r2unity's `-R` does today

`r2unity_enumerate_reverse_pinvokes()` (in `src/lib/lib.c`)
requires metadata wire version **29+** and does the following:

1. Decode methods, types, and images.
2. Load the v29 attribute-range rows from the repurposed
	`attributesInfo` / `attributeTypes` header slots — these now
	point at `attributeDataRange` (`{ uint token; uint
	startOffset; }`) and the inline BLOB pool.
3. For each range:
	- read the BLOB bytes at `attributeData + startOffset`
	- decode a compressed-int count of constructor references
	- resolve each constructor method index (into `methods[]`)
	- resolve the constructor's declaring type (into
		`typeDefinitions[]`)
	- compare the declaring type's name against
		`MonoPInvokeCallbackAttribute` and
		`UnmanagedCallersOnlyAttribute`
4. Use token-scoped lookup (per-image sorted
	`(token, method_idx)` slices) to resolve the owning managed
	method. This is required because ECMA-335 method tokens
	(`0x06??????`) are per-assembly 1-based row indices, not
	globally unique across the flattened IL2CPP metadata.

Each emitted row carries `kind = R2U_INTEROP_REVERSE_PINVOKE` (or
`R2U_INTEROP_UNMANAGED_ONLY`), the attribute name, flags, and the
standard image/method fields. Wrapper VA is always 0 today.

### 2.4 Why the image-scoped token lookup matters

Consider two managed assemblies that each define a type in
namespace `A` with a method `Cb`. Both methods share the token
`0x06000042` inside their respective PE files. In the flattened
`global-metadata.dat`, both rows survive, each with token
`0x06000042` — but one belongs to `Assembly-CSharp.dll` and the
other to `SomePlugin.dll`. Without image scoping, a token lookup
would return whichever row the linear scan hits first, silently
attaching the attribute to the wrong method.

r2unity therefore builds, for every image:

- `typeIndex -> imageIndex` (derived from
	`Il2CppImageDefinition.typeStart/typeCount`)
- a sorted `(token, method_idx)` slice over the subset of methods
	owned by that image

and performs a binary search inside the correct slice when
resolving an attribute-range token.

### 2.5 Pre-v29 is currently unreachable

The v29 BLOB replacement is what made this kind of resolution
metadata-only. On pre-v29 the attribute **types** are visible in
`attributesInfo` / `attributeTypes`, but the
`typeof(MyDelegate)` argument that distinguishes a real
`MonoPInvokeCallback` from a false positive lives inside the native
`customAttributeGenerators[i]` thunk. Without that, r2unity can at
best report "this range references
`MonoPInvokeCallbackAttribute`", which is a weaker signal.

Implementation: port the thunk interpreter from Il2CppDumper's
`Il2CppExecutor.GetCustomAttributeTypeName` +
`GetCustomAttributeValuesFromGenerator`; it is arch-specific but
small.

### 2.6 Wrapper address recovery

Current `-R` identifies **annotated managed callbacks**, but not
the **native trampoline stubs** that call them. Steps to close the
gap:

1. Structurally locate `Il2CppCodeRegistration` in the binary
	(§3 of `doc/r2unity.md`).
2. Read `reversePInvokeWrapperCount` and `reversePInvokeWrappers`.
3. For each Mach-O/ELF/PE image: walk `codeGenModules[]`, read
	each module's `reversePInvokeWrapperIndices[]`, and for every
	method where the index is ≥ 0, record
	`reversePInvokeWrappers[index]` as the wrapper VA for that
	method.

## 3. `iflags` — the other classification axis

`Il2CppMethodDefinition.iflags` is ECMA-335 `MethodImplAttributes`
(§II.23.1.11). The relevant bit layouts:

```text
CodeTypeMask         0x0003
    IL                 0x0000
    Native             0x0001
    OPTIL              0x0002
    Runtime            0x0003
ManagedMask          0x0004
    Unmanaged          0x0004
    Managed            0x0000
ForwardRef           0x0010
PreserveSig          0x0080      <- often set on P/Invoke methods
InternalCall         0x1000
Synchronized         0x0020
NoInlining           0x0008
AggressiveInlining   0x0100
NoOptimization       0x0040
```

r2unity copies `iflags` into every `R2UnityInterop` row but does
not currently use it to classify entries. Useful follow-ups:

- `InternalCall` (`0x1000`) marks icalls — runtime-backed methods
	like `System.Type.GetType`. Worth distinguishing from real
	P/Invokes in the `-P` output.
- `PreserveSig` distinguishes HRESULT-returning COM interop from
	plain P/Invoke.
- `Native | Unmanaged` combination confirms a real native-code
	method vs. a delegate-backed wrapper.

## 4. Output shapes (reference only)

### 4.1 Plain table

```text
image                       method                                     dll           entry        flags                  confidence
AdjustSdk.Scripts.dll       AdjustSdk.AdjustiOS._AdjustDisable         <unresolved>  <default>    private static extern  100
AdjustSdk.Scripts.dll       AdjustSdk.AdjustiOS.AdidGetterMonoPInvoke  MonoPInvokeCallback        private static         100
```

### 4.2 JSON

`-P -j` and `-R -j` wrap rows in `{"pinvokes":[...]}` and
`{"reverse_pinvokes":[...]}` respectively. Current fields per row:

- `image`, `method`, `token`, `flags`, `iflags`, `confidence`
- `-P`: optional `dll`, optional `entry` (empty today)
- `-R`: `attribute` (`MonoPInvokeCallback` /
	`UnmanagedCallersOnly`)

### 4.3 r2-script comment form (`-r`)

Descriptive comment lines, not executable `f` / `CCu` definitions,
because the wrapper VA is not recovered yet:

```text
# PInvoke sym.unity.AdjustSdk.Scripts.dll.AdjustSdk.AdjustiOS._AdjustDisable -> <unresolved>
# ReversePInvoke sym.unity.reverse.AdjustSdk.Scripts.dll.AdjustSdk.AdjustiOS.AdidGetterMonoPInvoke [MonoPInvokeCallback]
```

Once wrapper VAs land, `-r` will switch to real
`'@0xADDR'f sym.unity.reverse.<…>` and
`'@0xADDR'CCu ReversePInvoke <managed method>` lines.

## 5. Cross-reference summary

| Want to recover                  | From (metadata)                          | From (binary)                                          | r2unity today |
|----------------------------------|------------------------------------------|--------------------------------------------------------|---------------|
| Which methods are P/Invoke       | `methodDef.flags & 0x2000`               | —                                                      | Yes (`-P`)    |
| Managed method name + assembly   | `methods` + `typeDefinitions` + `images` | —                                                      | Yes           |
| `[DllImport]` DLL name           | v29+: `attributeData` BLOB               | pre-v29: `customAttributeGenerators` thunk             | No            |
| `[DllImport]` entry-point name   | v29+: `attributeData` BLOB               | pre-v29: `customAttributeGenerators` thunk             | No            |
| Native P/Invoke target wrapper   | —                                        | `interopData[].pinvokeMarshalToNativeFunction`         | No            |
| Reverse-P/Invoke annotation      | v29+: `attributeData` BLOB               | pre-v29: `customAttributeGenerators` thunk             | v29+ only     |
| Reverse-P/Invoke wrapper VA      | —                                        | `codeGenModule.reversePInvokeWrapperIndices[]` + `CodeRegistration.reversePInvokeWrappers[]` | No            |
| COM CCW / IID                    | —                                        | `interopData[].ccwFunction` / `.guid`                  | No            |

## 6. Further reading

- ECMA-335, "Common Language Infrastructure" — `MethodAttributes`
	(§II.23.1.10), `MethodImplAttributes` (§II.23.1.11), custom
	attribute BLOB format (`CustomAttrib`, §II.23.3),
	compressed-int encoding (§II.23.2), ImplMap table for
	P/Invoke (§II.22.22).
	<https://ecma-international.org/publications-and-standards/standards/ecma-335/>
- Microsoft docs: `DllImportAttribute` class.
	<https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.dllimportattribute>
- Microsoft docs: `UnmanagedCallersOnlyAttribute` class.
	<https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.unmanagedcallersonlyattribute>
- Unity docs: `MonoPInvokeCallbackAttribute` and IL2CPP interop.
	<https://docs.unity3d.com/Manual/ScriptingRestrictions.html>
- Unity blog: "IL2CPP internals: generic sharing + P/Invoke".
	<https://blog.unity.com/technology/il2cpp-internals-method-calls>
- Il2CppDumper's `Utils/CustomAttributeDataReader.cs` — v29+ BLOB
	decoder, authoritative reference.
	<https://github.com/Perfare/Il2CppDumper>
- `doc/r2unity.md` §3 — full `Il2CppCodeRegistration` /
	`Il2CppMetadataRegistration` field list.
- `doc/future.md` §2.9 and §3 — attribute table layouts per wire
	version and the native-side interop pointer arrays.

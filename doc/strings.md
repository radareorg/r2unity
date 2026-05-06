# r2unity - Managed String Literals In IL2CPP

This document is a technical reference for the two distinct string
mechanisms inside `global-metadata.dat`, for how IL2CPP serialises
and resolves managed `ldstr` constants, and for what r2unity
extracts today.

## 0. Two kinds of "string" in a Unity IL2CPP build

It helps to distinguish four bucket types up front, because each
lives in a different place on disk and needs a different tool to
recover:

1. **Managed identifier strings** — every type name, namespace,
	method name, field name, assembly name, parameter name, culture.
	These live in the **metadata string pool** inside
	`global-metadata.dat`.
2. **Managed `ldstr` literal constants** — every string constant
	that appears in C# source and ends up compiled to the ECMA-335
	`ldstr` IL opcode (ECMA-335 §III.4.16). These live in the
	**string-literal payload area**, also inside
	`global-metadata.dat`, in a different region with a different
	encoding.
3. **Native-binary constants** — C/C++ string literals inside
	`libil2cpp` itself, constants emitted by the transpiled C++
	code, symbols, runtime error messages. These live in
	`.rodata` / `__cstring` / `.rdata` of the native binary and
	are recovered with ordinary binary-analysis tools (`rabin2 -z`,
	`rafind2 -S`, `strings`, etc.).
4. **Asset / resource strings** — localized resources, text assets,
	ScriptableObject data, serialized dialog. Live in Unity asset
	files (`data.unity3d`, `sharedassets*.assets`, AssetBundles) and
	require Unity asset-deserialization tooling (AssetStudio,
	UnityEX, AssetRipper).

`r2unity -z` is *only* about bucket #2 — the managed `ldstr`
literals. The metadata identifier pool (bucket #1) is already used
implicitly through every name-returning API (`r2unity_get_string`);
it is not dumped wholesale today.

## 1. On-disk layout

### 1.1 Metadata identifier string pool

```text
stringOffset    .. +stringSize    — flat bytes, contains UTF-8
                                    NUL-terminated identifier strings
```

Addressed by byte offset relative to `stringOffset`. Every
`nameIndex`, `namespaceIndex`, `cultureIndex`, `moduleName*Index`,
etc. is an offset *into this region*, not an array index. Strings
are aggressively de-duplicated: the packer emits each unique UTF-8
byte sequence once and lets all references point to the same
offset.

`nameIndex` values that point **into the middle** of an existing
string rather than at its start. r2unity's `r2unity_get_string()`
handles this by walking backward from `index` until it finds a NUL
byte, then reading forward. This is a documented tolerance baked
into Il2CppDumper too.

### 1.2 Managed string-literal table

```text
stringLiteralOffset     .. +stringLiteralSize      — table of rows
stringLiteralDataOffset .. +stringLiteralDataSize  — payload bytes
```

Up through v31, each row in the table is 8 bytes:

```c
typedef struct {
    uint32_t length;       /* byte length of the literal payload */
    uint32_t dataIndex;    /* offset into the payload area       */
} Il2CppStringLiteral;
```

So `stringLiteralSize / 8` is the total number of `ldstr` constants
in the build. The payload for literal `i` lives at bytes
`stringLiteralDataOffset + row[i].dataIndex ..
 + row[i].dataIndex + row[i].length`, and is *not* NUL-terminated.

On v38+ metadata, the string-literal section is described by an
`Il2CppMetadataSection` with an explicit `count`, and each row stores
only:

```c
uint32_t dataIndex;       /* offset into the payload area */
```

The payload length is inferred from `row[i + 1].dataIndex -
row[i].dataIndex`; the last row runs to `stringLiteralDataSize`.
This is why v38+ literal decoding must use the section count instead
of `stringLiteralSize / 8`.

### 1.3 Why two regions

The split exists because:

- Identifier strings are NUL-terminated, aggressively de-duplicated,
	and accessed by offset (ECMA-335 §II.24.2.4 `#Strings` heap).
	They can be loaded once and referenced by offset for the lifetime
	of the AppDomain.
- Literal strings are length-prefixed, not de-duplicated the same
	way, and can contain embedded NULs, arbitrary bytes, or
	`InitializeArray`-backed payloads. They're the runtime target of
	the `ldstr` opcode, which ECMA-335 specifies as loading a
	`System.String` reference from a module's `#US` heap
	(§II.24.2.4). IL2CPP flattens all per-assembly `#US` heaps into
	this one table.

## 2. How IL2CPP gets from C# `ldstr` to a payload byte range

Concretely, given:

```csharp
string msg = "\tInitializationInfo...... {0}\n";
```

The C# compiler emits this in the `.cs` → PE path as an `ldstr`
that references a row in the PE's `#US` user-string heap
(ECMA-335 §II.24.2.4: the heap is UTF-16-encoded, with each entry
a compressed-int length followed by UTF-16 bytes, followed by a
one-byte terminator that encodes "contains non-ASCII" / "contains
high-Unicode").

When IL2CPP transpiles this method to C++, the `ldstr` call becomes
a call into the IL2CPP runtime like:

```cpp
Il2CppString* s = il2cpp_codegen_string_new_wrapper_for_ldstr(
    /*literal_index=*/42 );
```

and the runtime resolves `literal_index` to the payload bytes via
`stringLiteral[42]`.

The payload on disk is typically **UTF-8**, which is a design
departure from the original PE-level `#US` heap's UTF-16 format.
Many IL2CPP runtimes reencode `#US` entries into UTF-8 at build
time to save space (since `mscorlib`-heavy code is dominated by
ASCII-range strings). A handful of non-printable or `byte[]`-shaped
literals may pass through as raw bytes; see §3.

## 3. Encoding reality


1. **UTF-8 printable text** — the common case. Decodes cleanly,
	may include embedded ASCII control codes (`\t`, `\n`, `\r`,
	NUL).
2. **Empty strings** — `length == 0`. Valid.
3. **Control/binary fragments** — short payloads that look like
	integers, lookup tables, or compressed-int blobs. These exist
	because C#'s `RuntimeHelpers.InitializeArray(...)` is sometimes
	wired through the literal pool (an array constant gets placed in
	the assembly's `#Blob` heap, and IL2CPP may route it through the
	literal store).
4. **UTF-16 payloads** — rare, but possible when a specific
	literal was compiled in a way that IL2CPP preserved the original
	`#US` UTF-16 bytes rather than reencoding. Visually these look
	like alternating letters and zero bytes.

For this reason the CLI prints an **escaped** representation with
these rules:

- printable ASCII bytes are shown directly
- `\n`, `\r`, `\t`, `\\`, `\"` are escaped to their two-character
	form
- bytes outside ASCII that do **not** form a valid UTF-8 sequence
	are emitted as `\xNN`
- valid UTF-8 multibyte sequences are passed through as-is

This keeps output terminal-safe and deterministic across locales.

## 4. r2unity's implementation

### 4.1 API path

- `r2unity_get_string_literals(meta, &count)` — decodes legacy
	8-byte rows or v38+ 4-byte `dataIndex` rows into an
	`Il2CppStringLiteral *` array. On v38+ the length is synthesized
	from neighbouring indices.
- `r2unity_read_string_literal(meta, &row, &out_bytes, &out_len)` —
	bounds-checks the row against `stringLiteralDataSize` and
	returns a freshly-allocated copy of the payload. When the row
	points outside the payload region, returns `false` and the CLI
	substitutes `<invalid>`.
- `emit_string_literals()` in `src/main.c` — formats the dump for
	CLI output.

### 4.2 Output format

A tab-separated table:

```text
<index>   <abs_file_offset>   <byte_length>   "<escaped text>"
```

`abs_file_offset` is `stringLiteralDataOffset + dataIndex`, so it
points at the actual payload bytes inside the `.dat` file — useful
for cross-referencing with a hex dump or a radare2 session on the
metadata file itself.

Example:

```text
45	0x1c44a	30	"\tInitializationInfo...... {0}\n"
49	0x1c49d	30	"\tPrefetchBanner.......... {0}\n"
52	0x1c4f7	61	"\tTitle: {0}\n\tInformation: {1}\n\tCallToAction: {2}\n\tInform: {3}"
```

### 4.3 Performance and memory

The payload region is mapped as an `RBuffer` slice
(`r_buf_new_slice`), so no eager copy happens. Each `ldstr` row is
decoded with endian-safe LE reads. Payloads are copied lazily, one
per `r2unity_read_string_literal()` call.

## 5. What `-z` does not currently decode

### 5.1 Cross-linking to code references

The native binary's transpiled C++ turns every `ldstr` into a call
into the IL2CPP runtime indexed by literal index. A follow-up
feature would:

1. Scan the native binary for instruction patterns that load an
	integer immediate into the first-argument register and then call
	into the IL2CPP string-resolution runtime symbol.
2. Correlate each site with a literal index → payload text.
3. Emit `Cs` annotations in radare2 at each call site so
	disassembly shows the literal text inline.

This would bridge managed string literals into the disassembly
similarly to how `rabin2 -z` already handles native constants.

### 5.2 JSON output

`-z` currently has no JSON mode; only the plain tab-separated
table. Adding `-z -j` to emit
`{"literals":[{"index":42,"offset":0x1c44a,"length":30,"text":"…"}…]}`
would match the shape used by `-P`/`-R`.

### 5.3 Filtering / classification

- UTF-8 vs raw-bytes detection as a column flag.
- Whitelist/blacklist by regex (useful on huge dumps).
- Entropy sort (find likely base64/hex blobs at the top).

## 6. Why managed literal extraction matters

Managed literal strings tend to be much higher-signal than native
`.rodata` strings for game/app triage:

- They carry **C# semantic structure**: format strings with `{0}`
	`{1}` placeholders identifying API surface.
- They frequently hold hardcoded endpoints, API keys, feature
	flags, gameplay identifiers, ad-unit IDs.
- They include PlayerPrefs keys (`"playerLevel"`, `"coins"`),
	Unity scene names, `ScriptableObject` paths.
- Ad SDK callback identifiers (`"OnRewardedAdCompleted"`) surface
	here, which combined with reverse-P/Invoke (`doc/pinvoke.md`)
	gives full managed-to-native routing visibility.
- UI format strings reveal application state machines without
	needing to decompile the rest of the code.

Native-binary `strings` output is dominated by compiler-emitted
noise; managed literal output is almost entirely author-authored
text.

## 7. Cross-reference summary

| Want                                      | Where to look                                                      | Tool                                         |
|-------------------------------------------|--------------------------------------------------------------------|----------------------------------------------|
| Managed class/method/field names          | metadata string pool at `stringOffset`                             | r2unity (implicit via names) / Il2CppDumper  |
| Managed `ldstr` literal text              | `stringLiteralData` indexed by `stringLiteral[]` rows              | r2unity `-z`                                 |
| Native IL2CPP runtime strings / errors    | `.rodata` / `__cstring` of `libil2cpp`                             | `rabin2 -z`, `strings`                       |
| Code-emitted C++ transpilation strings    | `.rodata` / `__cstring`                                            | `rabin2 -z`                                  |
| Unity engine version string               | embedded literal in the native binary (`"2022.3.42f1"`)            | `rafind2`, `strings`                         |
| Localized resources / asset text          | Unity asset files (`*.assets`, AssetBundles)                       | AssetStudio, AssetRipper                     |
| Dynamically constructed strings           | not on disk verbatim                                               | runtime inspection / hooking                 |

## 8. Further reading

- ECMA-335, "Common Language Infrastructure" — `#US` user-string
	heap format (§II.24.2.4); `ldstr` opcode semantics (§III.4.16);
	`#Strings` metadata heap (§II.24.2.4).
	<https://ecma-international.org/publications-and-standards/standards/ecma-335/>
- Unicode 15.1 UTF-8 encoding rules (§3.9 of the standard);
	r2unity's escaper matches the "U+0000..U+007F → 1 byte,
	U+0080..U+07FF → 2 bytes, …" table.
	<https://www.unicode.org/versions/Unicode15.1.0/>
- Unity blog, "IL2CPP internals: a tour of generated code (strings)":
	<https://blog.unity.com/technology/il2cpp-internals-a-tour-of-generated-code>
- Il2CppDumper's string-literal emitter: `Il2CppDumper/Outputs/*`.
	<https://github.com/Perfare/Il2CppDumper>
- `doc/r2unity.md` §1.3 and §3.4 — metadata pool vs literal table
	addressing model and the r2unity decode path.

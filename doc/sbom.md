# r2unity - Software Bill Of Materials For IL2CPP Builds

This document is a technical reference for how Unity IL2CPP builds
carry enough information to produce a software bill of materials
(SBOM), how r2unity's `-S` mode reconstructs the managed layer today
as CycloneDX 1.5, and which native-layer and toolchain data is
declared in the binary but not yet consumed.

## 0. What an IL2CPP SBOM looks like in theory

An IL2CPP build is a layered artefact. A correct SBOM has three
corresponding layers.

### 0.1 Managed layer

Derived entirely from `global-metadata.dat`:

- Every managed assembly the game links against. In IL2CPP builds
	this always includes, at minimum:
	- `mscorlib.dll` (BCL core)
	- `System.dll`, `System.Core.dll`, `System.Xml.dll`, …
	- `UnityEngine.*.dll` — split into dozens of per-module
		assemblies since 2017.2 (`UnityEngine.CoreModule.dll`,
		`UnityEngine.PhysicsModule.dll`, `UnityEngine.UIModule.dll`,
		etc.).
	- `Assembly-CSharp.dll` and `Assembly-CSharp-firstpass.dll` —
		the generated assemblies containing the game's own scripts.
	- any third-party SDK DLLs (`AdjustSdk.Scripts.dll`,
		`Firebase.Analytics.dll`, …).
- For each assembly: version quadruple, culture, strong-name public
	key token, assembly flags, hash algorithm id.
- Dependency edges between assemblies (`AssemblyRef` equivalents).

This information is a literal serialisation of a subset of ECMA-335
§II.22.2 (`Assembly` table), §II.22.5 (`AssemblyRef` table), and
§II.6.3 (strong-name public-key derivation).

### 0.2 Native layer

Derived from the executable:

- **ELF** (`libil2cpp.so`, `GameAssembly.so`):
	`DT_NEEDED` entries → shared-library dependency list;
	`DT_SONAME` / `DT_RPATH` / `DT_RUNPATH`; build ID from
	`NT_GNU_BUILD_ID` note (`.note.gnu.build-id`); architecture
	from `e_machine`; interpreter from `PT_INTERP`; imports from
	`.dynsym`.
- **Mach-O** (`UnityFramework`, `GameAssembly.dylib`):
	`LC_LOAD_DYLIB`, `LC_LOAD_WEAK_DYLIB`, `LC_REEXPORT_DYLIB`
	commands → dylib dependency list; `LC_UUID` → 16-byte UUID;
	`LC_BUILD_VERSION` → minimum OS and SDK versions;
	`LC_CODE_SIGNATURE` → code-signing summary (team id,
	entitlements).
- **PE** (`GameAssembly.dll`):
	Import Address Table (`IMAGE_DIRECTORY_ENTRY_IMPORT`) →
	imported DLLs and symbols; `IMAGE_DEBUG_DIRECTORY` →
	`RSDS`/`CodeView` PDB GUID + age + path (the standard
	PE→PDB linkage); `TimeDateStamp` or Rich header →
	toolchain fingerprint.
- File hashes of the binary and `global-metadata.dat` itself
	(SHA-256 per CycloneDX best practice).

None of this is implemented in r2unity today.

### 0.3 Engine / toolchain layer

Inferred:

- Unity version range from the metadata wire version (mapping in
	`doc/future.md`).
- IL2CPP runtime version from symbol signatures or strings in
	the native binary (e.g. the `il2cpp_init` / `il2cpp_runtime_version`
	exports or a compiled-in string constant).
- Toolchain: Xcode/clang (iOS), Android NDK version (Android), MSVC
	(Windows), Emscripten (wasm). Inferable from format-specific
	metadata (Mach-O `LC_BUILD_VERSION`, ELF `.note.android.ident`
	/ `.comment`, PE Rich header).
- Managed BCL version: from `mscorlib.dll`'s version quadruple
	inside the managed layer.

r2unity currently produces only the wire-version → Unity release
mapping.

## 1. What r2unity's `-S` emits today

The implementation lives in `r2unity_sbom_tostring()` in
`src/lib/sbom.c`, and is shared by the command-line tool and the r2
core plugin. The default output is a text summary (`r2unity -S` or
`r2unity-S`); CycloneDX JSON is available as `r2unity -S -j` and
`r2unity-Sj`.

### 1.1 CycloneDX 1.5 top-level

- `bomFormat = "CycloneDX"`
- `specVersion = "1.5"`
- `version = 1`
- `serialNumber` — not currently emitted (should be a
	`urn:uuid:...` per CycloneDX 1.5).
- tool component named `r2unity`
- top-level `application` component named from the executable path
	passed on the CLI
- `metadata.component.properties` embeds:
	- Unity version range inferred from the wire version
	- the metadata file path
	- the metadata wire version
	- a `metadata confidence` tag set to `range`

Reference: CycloneDX 1.5 JSON schema,
<https://cyclonedx.org/specification/overview/>.

### 1.2 Managed component rows

For each `Il2CppAssemblyDefinition` + `Il2CppImageDefinition` pair
(one-to-one on every wire version r2unity accepts today):

- `bom-ref` — stable identifier derived from assembly name +
	version.
- `type = "library"`
- `name` — from `aname.name_idx` (metadata string pool).
- `version` — `"major.minor.build.revision"` from the four
	`int32_t` version fields of `Il2CppAssemblyNameDefinition`.
- `purl` — a generic package-URL of the form
	`pkg:generic/unity/<name>@<version>`.
- custom `properties`:
	- `dotnet.culture` — from `aname.culture_idx`
	- `dotnet.public_key_token` — 8-byte
		`aname.public_key_token` in hex, or an empty string for
		unsigned assemblies
	- `dotnet.hash_alg` — `aname.hash_alg` (1 = MD5,
		0x8003/32771 = SHA1, etc.; see
		`AssemblyHashAlgorithm` enum in the BCL)
	- `dotnet.flags` — `aname.flags` from ECMA-335
		§II.23.1.2
	- `il2cpp.image` — from
		`Il2CppImageDefinition.nameIndex`
	- `il2cpp.image_index` — the image's row index
	- `il2cpp.token` — `Il2CppAssemblyDefinition.token`
		(0x20000001 for the module-like row)

### 1.3 Dependency edges

The `dependencies` section is built from:

- the flat `referencedAssemblies` array (just `int32_t` indices)
- each assembly's `referenced_start` / `referenced_count` slice

`referenced_start/count` exist from wire version 20 onward; pre-v20
assemblies do not carry dependency edges inline.

Each dep-ref's target `bom-ref` is resolved back through the
`assemblies` table; the graph is emitted as a flat adjacency list
per CycloneDX 1.5 `dependencies[]`.

## 2. How the managed-layer decoding actually works

### 2.1 Tables touched

- `Il2CppImageDefinition` (40 B rows on v24.1..v31; compact
	`TypeDefinitionIndex` fields can shrink newer rows).
- `Il2CppAssemblyDefinition` (variable row size inferred as
	`assembliesSize / imageCount` on legacy headers, or read from the
	v38+ section count because there is one assembly per image on
	every supported wire version).
- `Il2CppAssemblyNameDefinition` (trailing struct; 48 B with
	`hashValueIndex` removed, 52 B with `hashValueIndex` present —
	the tail is chosen to match the inferred row size).
- `referencedAssemblies` — flat `int32_t[]`; indices into
	`assemblies`.
- Metadata string pool for names, cultures, image names.

### 2.2 Why the stride is inferred, not looked up

`assembliesSize` is the raw byte size of the assembly section.
On legacy headers, `imageCount` is known from the decoded image table
and `assembliesSize / imageCount` gives the per-row stride for
`Il2CppAssemblyDefinition`. On v38+ headers the section count can be
used directly. The assembly row varies across wire versions because
the trailing `Il2CppAssemblyNameDefinition` changed size and because
v38 inserted a `moduleToken` before that tail:

- `hashValueIndex` removed at v24.3+ → row shrinks 4 B.
- `customAttributeIndex` removed at v24.1 → another 4 B.
- `token` added at v24.1 → 4 B back.
- `moduleToken` added at v38 → `Il2CppAssemblyNameDefinition` starts
	at row offset 20 instead of 16.

### 2.3 Version-range inference

The wire version is the only reliable hint about Unity release
date, because the fully-flattened metadata does not carry a Unity
version string (the Unity-version string exists as a constant
inside the native binary, typically `"20xx.y.z.*"` or `"6000.y.z"`).

r2unity uses the wire-version mapping documented in `doc/future.md`.
That file owns the Unity-release ranges, sub-version notes, accepted
version set, rejected versions, and v38/v39 Unity 6 metadata break.

## 3. What the shipped `-S` does not do

### 3.1 Native-library enumeration

Per-format omissions:

- **ELF**: no `DT_NEEDED` walk, no `DT_SONAME` capture, no
	`.note.gnu.build-id` read, no `.note.android.ident` decoding.
- **Mach-O**: no `LC_LOAD_DYLIB` iteration, no `LC_UUID` capture,
	no `LC_BUILD_VERSION` (`sdk`, `minos`, `platform`), no
	`LC_CODE_SIGNATURE` summary, no `LC_ENCRYPTION_INFO_64`
	detection (FairPlay-encrypted App Store slices).
- **PE**: no import-table walk (`IMAGE_DIRECTORY_ENTRY_IMPORT`),
	no `RSDS` CodeView GUID read, no Rich header parse.

### 3.2 File hashes

CycloneDX 1.5 allows `hashes: [{"alg":"SHA-256","content":"..."}]`
on every component. r2unity computes none. Hashing the native
binary and `global-metadata.dat` would match CycloneDX's spec
directly.

### 3.3 Build identifiers and timestamps

- ELF build ID (`NT_GNU_BUILD_ID`, 16-byte random blob that
	uniquely identifies a specific binary build) — not read.
- Mach-O `LC_UUID` (16-byte UUID) — not read.
- PE `IMAGE_DEBUG_DIRECTORY` → `CV_INFO_PDB70` → PDB GUID + age —
	not read. This is the field that links a PE executable to its
	matching `.pdb` symbol file via the
	`Microsoft/Symbols/GUIDAge/Pdb` cache layout.
- Compile / link timestamps (PE `TimeDateStamp`) — not read.

### 3.4 SPDX output

Only CycloneDX 1.5 is emitted. SPDX 2.3 JSON (for projects that
require it by policy) is not currently an option.

### 3.5 Managed assembly hash value

When `hashValueIndex` is present (wire v24.0..v24.2), the string
pool entry it points at holds the assembly's content hash as
defined in `AssemblyName.HashValue`. r2unity emits the hash
**algorithm** id but not the hash **value** itself.

### 3.6 Encrypted binaries

Commercial `.ipa` files from the App Store are FairPlay-encrypted
until extracted from a jailbroken device. A correct SBOM emitter
should detect `LC_ENCRYPTION_INFO_64.cryptid != 0` and either bail
or emit a degraded native layer with a warning.

## 4. Why an IL2CPP-aware SBOM is worth producing

Most SBOM tools treat a Unity IPA / APK as a single "application"
with no internal decomposition, or at best list the shipped
third-party dylib files. That misses:

- Every managed assembly the game *actually links against* — a
	Unity game can easily pull in 40+ managed assemblies, many of
	which map to well-known OSS components (Newtonsoft.Json,
	YamlDotNet, SharpZipLib, Google.Protobuf).
- Third-party SDKs that ship as managed wrappers over a dylib
	(Adjust, Firebase, IronSource, Appsflyer, Facebook). Both the
	managed wrapper (visible in `assemblies[]`) and the native dylib
	(visible in `LC_LOAD_DYLIB`) matter for compliance.
- Unity engine version itself, inferable from wire version + native
	version string.

The managed layer alone — which is what r2unity emits today — is
already enough to drive:

- License-compliance checks against known OSS Nugets.
- "Which games use which ad SDK" audits across a corpus.
- Assembly-version pinning (catch when a game downgrades a known-
	vulnerable `Newtonsoft.Json` to an older version).
- Build provenance (was this IPA actually produced by the
	publisher's CI, or repackaged?).

## 5. Recommended next steps

In rough priority order:

1. Replace inferred `Il2CppAssemblyDefinition` sizing with an
	explicit version-aware layout table keyed on wire +
	sub-version.
2. Emit SHA-256 file hashes for the executable and metadata file
	as `component.hashes[]`.
3. Extend ELF/Mach-O/PE readers to enumerate native dependencies
	and add them as `library` components with their own `purl`
	(`pkg:generic/dylib/libcurl.4.dylib`, etc.).
4. Capture ELF build IDs, Mach-O UUIDs, PE debug GUIDs into
	`component.properties`.
5. Read the Unity version string out of the native binary (it's a
	literal `"20xx.y.z"` constant) to pin the Unity release more
	precisely than the wire-version range allows.
6. Detect FairPlay-encrypted Mach-O slices and emit a degraded SBOM
	with a warning instead of garbage.
7. Optional: SPDX 2.3 JSON output for projects that require it.

## 6. Further reading

- ECMA-335, "Common Language Infrastructure" — `Assembly` (§II.22.2)
	and `AssemblyRef` (§II.22.5) tables, `AssemblyFlags` (§II.23.1.2),
	strong-name public-key derivation (§II.6.3).
	<https://ecma-international.org/publications-and-standards/standards/ecma-335/>
- CycloneDX specification (the format r2unity emits):
	<https://cyclonedx.org/specification/overview/>
- SPDX specification (alternative format, not yet emitted):
	<https://spdx.dev/specifications/>
- Package URL spec (purl, used in `component.purl`):
	<https://github.com/package-url/purl-spec>
- ELF dynamic tags reference:
	<https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.dynamic.html>
- Mach-O reference (Apple):
	<https://github.com/apple-oss-distributions/cctools>
- PE/COFF reference (Microsoft):
	<https://learn.microsoft.com/windows/win32/debug/pe-format>
- `doc/r2unity.md` §2.7 — `Il2CppAssemblyDefinition` and
	`Il2CppAssemblyNameDefinition` layouts.
- `doc/future.md` §2.11 — assembly table's per-sub-version deltas.

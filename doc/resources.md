# Unity Resources, Assets, Sidecars, and Save Data

This document describes the resource-like files found in Unity Player and
IL2CPP builds, how the formats relate to one another, what radare2 and
r2unity support today, and what remains to be implemented.

The most important point is that similar-looking names refer to several
unrelated systems:

```text
Unity content
├── data.unity3d / *.bundle       Unity archive (usually UnityFS)
│   ├── resources.assets          Unity SerializedFile object database
│   ├── sharedassets*.assets      Unity SerializedFile object database
│   ├── *.resS                    streamed texture and mesh bytes
│   └── *.resource                streamed audio and video bytes
│
Managed/.NET resources
└── Data/Managed/Resources/*.dll-resources.dat
    ├── IL2CPP manifest-resource table
    └── payloads: XML, icons, binary data, or sometimes *.resources
        └── *.resources           .NET name/value resource dictionary

Save data
└── Application.persistentDataPath / PlayerPrefs
    └── filenames and formats selected by each game
```

Consequently:

- `.assets`, `.resS`, and `.resource` normally contain read-only build
  content, not saved game state.
- `.resources`, `.resource`, `.resS`, and `*-resources.dat` are distinct
  formats despite their similar names.
- `.dat` is only a generic suffix. Its meaning must be inferred from its
  path, filename, magic, structure, and the code that reads it.
- A literal `.res` file is not the same as Unity's case-sensitive `.resS`
  sidecar.

## 1. Format summary

| File or suffix | Meaning | Self-contained? | Current support |
| --- | --- | --- | --- |
| `global-metadata.dat` | IL2CPP managed metadata | Yes, but native address correlation needs the matching IL2CPP binary | r2unity |
| `*.dll-resources.dat` | IL2CPP assembly manifest-resource pack | Yes | r2unity |
| `*.resources` | Standard binary .NET resource dictionary | Yes | Outer payload only when embedded in a PE; no entry parser |
| `resources.assets` | Serialized assets originating in project `Resources` directories | Mostly; large payloads can be in sidecars | r2unity SerializedFile v22 baseline |
| `sharedassets*.assets` | Serialized assets shared by scenes | Mostly; large payloads can be in sidecars | r2unity SerializedFile v22 baseline |
| `globalgamemanagers.assets` | Assets referenced by global engine/project settings | Mostly; large payloads can be in sidecars | r2unity SerializedFile v22 baseline |
| `level*`, `CAB-*`, `*.sharedAssets` | Other Unity SerializedFiles | Mostly; may reference sidecars and other SerializedFiles | r2unity SerializedFile v22 baseline |
| `*.resS` | Raw streamed texture and mesh data | No; the owner supplies offsets, sizes, types, and names | Owner references reported; sidecar parsing pending |
| `*.resource` | Raw streamed audio and video data | Usually no; the owner supplies offsets, sizes, types, and names | Owner references reported; sidecar parsing pending |
| `data.unity3d`, `*.bundle` | Unity archive containing SerializedFiles and sidecars | Yes as a container | None |
| Addressables `catalog.bin` / `catalog.json` | Key-to-location and dependency mapping | No; it points at bundles and providers | None |
| Addressables `catalog.hash` | Catalog version/update fingerprint | No | None |
| literal `*.res` | Usually a compiled Windows resource file, or an application-specific file | Yes for Win32 `.res` | Linked PE resources only; no standalone parser |
| BGDatabase `*.bytes`, `*.sav`, or `*.dat` | Third-party BansheeGz database or saved repository | Yes | r2unity BGDatabase v6 baseline |
| arbitrary `*.dat` | Application-specific data, cache, save, or one of the known `.dat` formats above | Unknown | Format-dependent |

## 2. Unity's build-content graph

Unity serializes content as a graph rather than as a directory of independent
media files. A typical asset is recovered by following several layers:

```text
UnityFS archive member
    ↓
SerializedFile object-table entry
    ↓
class/type information and object payload
    ↓
optional stream reference: path + offset + size
    ↓
bytes in a .resS or .resource sidecar
```

Object references can also cross SerializedFiles. A scene object can point to
an asset in `sharedassets0.assets`, `resources.assets`, another bundle, or a
built-in resource file. Correct extraction therefore needs a deployment-wide
view, not merely a parser for one extension.

Unity documents the high-level organization, but does not publish a complete
stable specification for every version of the low-level SerializedFile and
UnityFS encodings. It also warns that build layouts and filenames may change
between versions. An r2unity implementation must use versioned parsers and a
corpus covering old and current Unity releases.

References:

- [Unity build content output](https://docs.unity3d.com/ja/current/Manual/build-content-output.html)
- [Unity AssetBundle file format](https://docs.unity3d.com/6000.0/Documentation/Manual/assetbundles-file-format.html)
- [Unity AssetBundle compression](https://docs.unity3d.com/6000.0/Documentation/Manual/assetbundles-compression-format.html)

## 3. Unity SerializedFiles: `.assets`, `level*`, and `CAB-*`

### 3.1 Purpose

A Unity SerializedFile is an object database. Common filenames include:

- `resources.assets`
- `sharedassets0.assets`, `sharedassets1.assets`, and so on
- `globalgamemanagers` and `globalgamemanagers.assets`
- `level0`, `level1`, and other scene files
- `CAB-<hash>` members inside AssetBundles
- `PlayerBuild-<scene>` and accompanying shared-asset members
- built-in files such as `unity default resources` and
  `unity_builtin_extra`

The extension is not a reliable detector: several SerializedFiles have no
extension, while unrelated files can use `.assets`. SerializedFiles normally
do not begin with a textual magic string.

### 3.2 Logical contents

The versioned header and metadata describe at least:

- metadata size and total file size
- SerializedFile format version
- object-data offset
- file endianness
- Unity engine version, when present
- target platform
- whether a serialized type tree is present
- serialized type/class definitions
- object records
- script type references
- external SerializedFile references
- reference types and user information in newer versions

Each object record associates a path ID with a type and a byte range. The
object's stored offset is resolved relative to the file's data area. The
exact width, alignment, and interpretation of fields depend on the
SerializedFile version.

The type information may contain a type tree describing serialized fields.
When the type tree is stripped, built-in Unity class layouts must be selected
from a versioned database. `MonoBehaviour` and `ScriptableObject` payloads
also require the corresponding managed script schema. For IL2CPP builds,
r2unity can use `global-metadata.dat` to recover script field names and types,
although the final Unity serialization layout still has to obey Unity's own
serialization rules and version-specific alignment.

### 3.3 `resources.assets`

`resources.assets` is not a .NET `.resources` file. It is a Unity
SerializedFile containing objects collected from project directories named
`Resources`. Unity includes those objects in the build even when no scene
directly references them, and `Resources.Load` resolves objects from this
content.

Dependencies pulled in by a resource object may be stored alongside it or in
other shared files. The logical name used by `Resources.Load` is not simply a
filesystem entry in `resources.assets`; the SerializedFile objects and their
container/name metadata must be decoded.

### 3.4 What should become an `RBinResource`

Not every serialized Unity object should be presented as an extractable
resource. A useful mapping is:

- sections/fields: file header, metadata, type trees, object table, and data
  area
- symbols: object path IDs and known object names
- classes: Unity class IDs and recovered managed script types
- resources: payload-bearing objects such as `TextAsset`, `Texture2D`,
  `Mesh`, `AudioClip`, `VideoClip`, shaders, fonts, and selected serialized
  script blobs

An `RBinResource` for a Unity asset should carry:

- a logical object or asset name when recoverable
- the Unity class as its type
- the owning SerializedFile as its origin
- the inline or external offset and size
- a stable index and, where useful, the path ID

Objects with bytes in a sidecar need special handling described in section 4.

### 3.5 Implemented v22 baseline and reference file

`bin_r2unity` now identifies loose SerializedFile format v22 inputs by their
extended header and validates the declared metadata size, file size, data
offset, endianness, and Unity version string before accepting the file. The
parser is implemented in `src/lib/serialized_file.c` using bounded `RBuffer`
reads.

For the `sharedassets10.assets` reference file, it recovers:

- Unity `2020.3.33f1`, little endian, platform 19, with a stripped type tree
- 8 serialized types, 18 objects, 2 script references, and 4 external files
- `Material`, `Texture2D`, two `TextAsset` objects, ten `AnimationClip`
  objects, an `AnimatorController`, two `MonoBehaviour` objects, and
  `PreloadData`
- the inline 348-byte Spine atlas and 12,931-byte JSON skeleton payloads
- a 1695 by 195 RGBA32 `Texture2D` whose 1,322,100 data bytes are stored at
  offset zero in `sharedassets10.assets.resS`

The bin plugin exposes the fixed header, metadata, alignment padding, data
area, and every object as sections; objects and inline `TextAsset` payloads as
symbols; serialized types as classes; header/object descriptors as fields;
object names and external paths as strings; external SerializedFiles as
libraries; and named objects through `RBinResource`.

This is deliberately a v22 baseline, not a claim of universal SerializedFile
support. Built-in object names are recovered from known layouts, and the
`Texture2D` and `TextAsset` decoders cover the layouts exercised by this
reference file. Other format versions, interpreted type trees, more built-in
classes, and managed `MonoBehaviour` schemas still require versioned decoders.

## 4. Streamed sidecars: `.resS` and `.resource`

### 4.1 `.resS`

Unity uses `.resS` sidecars primarily for large texture and mesh data. The
capital `S` is significant on case-sensitive filesystems.

A `.resS` file is generally a byte store, not a self-describing object
database. The owning Texture2D, Mesh, or related object supplies a stream
reference containing some combination of:

- sidecar or archive path
- byte offset
- byte size

Without the owning SerializedFile, r2unity can identify the file only as a
raw data sidecar. It normally cannot assign names, types, or reliable record
boundaries.

### 4.2 `.resource`

Unity uses `.resource` sidecars for audio and video content. As with `.resS`,
the owning SerializedFile identifies the useful ranges.

The contents are not guaranteed to be one universal codec. For example, the
Unity 6 iOS build inspected while preparing this document has a
`sharedassets0.resource` beginning with `FSB5`, an FMOD sound-bank signature.
Other builds can contain different audio/video encodings or several streamed
ranges.

Opening a `.resource` file independently may reveal a recognizable inner
container, but that does not replace the Unity object metadata needed to map
the bytes back to `AudioClip` or `VideoClip` objects.

### 4.3 Why the current r2 resource API is insufficient for sidecars

`r_bin_file_get_resource_data()` currently obtains resource bytes by slicing
`resource->paddr .. resource->paddr + resource->size` from the current
`RBinFile` buffer. The `origin` member is descriptive metadata and is not
consulted for extraction.

That model works for inline objects and `*.dll-resources.dat`, but not when a
resource listed while `resources.assets` is open resides in
`resources.assets.resS`.

Possible solutions are:

1. Add an optional `RBinPlugin` callback that returns an `RBuffer` for a
   resource, falling back to the current-file slice when absent.
2. Represent sidecars as separate logical `RBinFile` objects and make the
   owning object graph point to them.
3. Mount the entire Unity deployment or UnityFS archive as a filesystem and
   resolve stream paths through that mount.

The first option is the smallest generic radare2 enhancement. The second and
third remain useful for navigation and container analysis.

## 5. UnityFS, `data.unity3d`, and AssetBundles

### 5.1 Unity archives

Modern AssetBundles and LZ4-compressed Player content normally use Unity's
archive format and begin with the NUL-terminated signature `UnityFS`.
`data.unity3d` is commonly such an archive.

The archive header identifies the archive format and Unity/generator
versions, total size, compressed and uncompressed block-information sizes,
and flags. The block-information structure describes compressed content
blocks and directory nodes. Directory nodes name logical files such as:

- `CAB-<hash>` SerializedFiles
- scene SerializedFiles
- `.resS` sidecars
- `.resource` sidecars
- other build products

The low-level integers in common UnityFS headers are stored in big-endian
form even though most contained SerializedFiles target little-endian
platforms. A parser must treat container and member endianness separately.

### 5.2 Compression

Unity supports:

- full-content LZMA compression
- chunked LZ4/LZ4HC compression
- uncompressed content

LZ4 bundles divide content into independently decompressible blocks; Unity's
documentation describes 128 KiB chunks. A member can span block boundaries,
so extraction must reconstruct the logical uncompressed address space rather
than treating a directory offset as a raw file offset.

radare2 already contains LZ4 decompression primitives. Full UnityFS support
also needs LZMA support or an optional portable dependency.

### 5.3 Correct radare2 abstraction

Archive members are files, not merely `RBinResource` records. UnityFS is best
implemented as either:

- an `RBinXtrPlugin` that returns decompressed member buffers, or
- an `RFSPlugin` that mounts and lists archive members

After extraction, the normal SerializedFile plugin can process `CAB-*`,
`.assets`, and scene members recursively.

Older Unity releases also used `UnityRaw` and `UnityWeb` bundle variants.
They can be added after UnityFS support and require separate fixtures.

## 6. IL2CPP `*.dll-resources.dat`

### 6.1 Purpose and path

IL2CPP externalizes assembly manifest resources into files under the Player
data directory's `Resources` directory. The generated runtime constructs the
name as:

```text
<Runtime data directory>/Resources/
    <assembly image name>-resources.dat
```

Because an assembly image name normally includes `.dll`, common files are:

```text
mscorlib.dll-resources.dat
System.Data.dll-resources.dat
System.Drawing.dll-resources.dat
```

This behavior is implemented by generated libil2cpp code in:

```text
Il2CppOutputProject/IL2CPP/libil2cpp/icalls/mscorlib/
    System.Reflection/RuntimeAssembly.cpp
```

The runtime memory-maps the complete file and returns named resource slices
to managed reflection APIs such as `GetManifestResourceStream`.

### 6.2 On-disk layout

The observed and generated-runtime layout is:

```c
int32_t resourceRecordsSize;
int32_t numberOfResources;

for (int32_t i = 0; i < numberOfResources; i++) {
    uint32_t resourceDataSize;
    int32_t resourceNameSize;
    uint8_t resourceName[resourceNameSize]; /* not NUL-terminated */
}

/* Resource payloads follow consecutively in record order. */
uint8_t resourceData[];
```

The first payload starts at:

```text
4 + resourceRecordsSize
```

The offset of each later payload is the previous payload offset plus its
declared `resourceDataSize`.

Current little-endian fixtures therefore decode as:

```text
offset  size  meaning
0x00    4     resource-record area size, excluding this field
0x04    4     number of resources
0x08    4     first payload size
0x0c    4     first name size
0x10    ...   first name bytes
...           following descriptors
4+size  ...   concatenated payload bytes
```

Generated libil2cpp reads integers with native `memcpy` rather than explicit
byte swapping. Existing desktop/mobile fixtures are little-endian; a robust
implementation should use companion target information or reject ambiguous
endianness rather than silently misparsing unusual targets.

### 6.3 Example payloads

Fixtures inspected in `test/bins/other/unityapks` contain:

- `System.Data.dll-resources.dat`
  - one `System.Data.SqlClient.SqlMetaData.xml` XML document
- `System.Drawing.dll-resources.dat`
  - `Mono.ico`
  - `Information.ico`
  - `Error.ico`
  - `Warning.ico`
  - `Question.ico`
  - `Shield.ico`
- `mscorlib.dll-resources.dat`
  - seven `collation.*.bin` payloads

Manifest-resource payloads are opaque to the outer pack. A payload can be
XML, an image, an arbitrary binary, or a standard `.resources` dictionary.

### 6.4 Detection and validation

This format has no fixed magic. A plugin should combine a filename/path hint
with complete structural validation:

- require a plausible nonnegative record-area size and resource count
- cap the number of entries and name length
- keep every descriptor inside the declared record area
- reject negative name lengths
- check every addition for integer overflow
- require the payload area to start inside the file
- require every payload to fit inside the remaining bytes
- optionally require descriptor consumption to equal the declared record
  area exactly
- reject overlapping or trailing layouts unless a real fixture demonstrates
  that Unity permits them

Filename matching alone must not claim arbitrary `.dat` files.

### 6.5 `RBinResource` mapping

The format maps directly onto radare2's resource API:

```text
RBinResource.name       manifest resource name
RBinResource.type       detected MIME/format or MANIFESTRESOURCE
RBinResource.encoding   raw unless the outer data is actually encoded
RBinResource.origin     assembly name or pack filename
RBinResource.paddr      payload offset
RBinResource.vaddr      payload offset for a non-VA file
RBinResource.size       payload size
RBinResource.id         UT64_MAX
RBinResource.index      descriptor index
RBinResource.named      true
```

This is the highest-value short-term resource implementation for r2unity:
the layout is small, deterministic, and immediately enables `iU`, `iUj`, and
`iUx`.

### 6.6 Implemented resource-pack baseline

The existing `bin_r2unity` plugin now recognizes `*-resources.dat` inputs and
uses a dedicated bounded `RBuffer` parser. Automatic detection requires the
filename suffix because the format has no magic. The parser validates the
descriptor-area size, resource count, UTF-8 names, exact descriptor
consumption, every payload range, and exact end-of-file consumption.

The plugin exposes header, descriptor, and payload sections; payload symbols;
descriptor fields and strings; the owning assembly as a library; and one
`RBinResource` per payload. XML, icon, .NET `.resources`, JSON, text, PNG,
JPEG, GIF, ZIP, and FSB5 payloads receive conservative content types, with
unknown payloads remaining `application/octet-stream`.

The fixture corpus currently covers:

- one XML resource from `System.Data.dll-resources.dat`
- six icon resources from `System.Drawing.dll-resources.dat`
- seven binary collation resources from `mscorlib.dll-resources.dat`

All payloads are inline, so generic `iU`, `iUj`, and `iUx` extraction works
without the external-sidecar API needed by `.resS` and `.resource`.

## 7. Standard .NET `.resources`

### 7.1 Relationship to assemblies and IL2CPP packs

A `.resources` file is a compiled .NET name/value resource dictionary. It can
be standalone or embedded as one manifest resource in an assembly. Under
IL2CPP, the same manifest resource can become one payload in an assembly's
`*.dll-resources.dat` file.

The layers must not be conflated:

```text
System.Example.dll-resources.dat     IL2CPP outer pack
└── Company.Product.Strings.resources
    ├── MainWindow.Title             .NET dictionary entry
    ├── Error.Network                .NET dictionary entry
    └── Logo                         .NET dictionary entry
```

### 7.2 Header and tables

Standard binary `.resources` files begin with the little-endian magic value:

```text
0xBEEFCACE
```

The ResourceManager header contains:

1. magic number
2. ResourceManager header version
3. number of bytes to skip past the remaining ResourceManager header
4. length-prefixed resource-reader type name
5. length-prefixed ResourceSet type name

The ResourceReader data that follows contains versioned information including:

- resource count
- serialized/user type count and names
- aligned name-hash table
- name-position table
- data-section offset
- name section
- typed value records in the data section

Resource names are used as dictionary keys. Value records carry a type code
and can represent strings, byte arrays, streams, primitive values, or
user-defined serialized types. A safe reverse-engineering parser should
enumerate and extract raw value data without instantiating arbitrary managed
types.

References:

- [Microsoft: create resource files](https://learn.microsoft.com/en-us/dotnet/core/extensions/create-resource-files)
- [Microsoft: ResourceReader](https://learn.microsoft.com/en-us/dotnet/fundamentals/runtime-libraries/system-resources-resourcereader)
- [Microsoft reference source: ResourceManager header](https://github.com/microsoft/referencesource/blob/master/mscorlib/system/resources/resourcemanager.cs)

### 7.3 Current radare2 behavior

radare2's PE parser handles two resource families:

- native PE resource-directory entries
- embedded CLR `ManifestResource` rows whose implementation field indicates
  that the payload resides in the current assembly

For a managed resource, r2 reads the CLR resource-directory length prefix and
creates one `RBinResource` for the complete manifest payload. It does not
currently inspect the payload for `0xBEEFCACE` or enumerate the inner
`.resources` dictionary.

A standalone `.resources` bin plugin belongs in radare2 rather than r2unity
because the format is generic .NET. It should also be usable recursively on
payloads obtained from PE or IL2CPP resource containers.

## 8. Win32 `.res`

A literal `.res` file usually means the binary output of the Windows resource
compiler. It is unrelated to Unity `.resS` and .NET `.resources`.

A compiled `.res` file contains concatenated, DWORD-aligned entries. Each
entry declares its header and data sizes and describes:

- resource type, either numeric or named
- resource name/identifier
- data version
- memory flags
- language
- resource version
- characteristics
- type-specific payload

The linker transforms these entries into the resource directory embedded in
a PE executable or DLL. radare2 already enumerates the linked PE resource
tree, but no standalone `.res` parser was found in the current source.

Reference:

- [Microsoft: Resource File Formats](https://learn.microsoft.com/en-us/windows/win32/menurc/resource-file-formats)

If a game ships a file with a lowercase `.res` suffix on a non-Windows
platform, the suffix alone is not enough to identify it. Inspect its magic,
path, and reader code before treating it as Win32 data.

## 9. `.dat` files

`.dat` has no common file format. Several unrelated files in one Unity build
can use it.

### 9.1 `global-metadata.dat`

`global-metadata.dat` is IL2CPP's managed metadata database. Its sanity value
is `0xFAB11BAF` and the next word identifies the metadata wire version.

It stores such information as:

- assemblies and images
- types, methods, fields, properties, and events
- generic metadata
- custom attributes and default-value blobs
- identifier strings
- managed `ldstr` literal data

It does not contain Unity textures, scenes, prefabs, audio, or AssetBundle
object databases. See `doc/datvsbin.md` and `doc/strings.md` for the detailed
r2unity coverage.

### 9.2 `*.dll-resources.dat`

These files use the IL2CPP manifest-resource pack described in section 6.
They are identified by their path, filename, and validated record layout, not
by the generic `.dat` suffix.

### 9.3 Catalogs, caches, and application files

Packages or games can assign `.dat` to custom catalogs, caches, databases,
network content, or obfuscated/encrypted files. The only reliable definition
is the producing/consuming code.

### 9.4 Saved games

Unity does not define a universal binary save format. A developer can write
any bytes under `Application.persistentDataPath`, including:

- JSON or XML
- a custom BinaryReader/BinaryWriter layout
- SQLite
- protocol buffers or MessagePack
- compressed data
- encrypted or authenticated data
- a mixture of versioned headers and application-specific records

The `.dat` extension does not distinguish any of these.

### 9.5 BGDatabase repositories and saves

BGDatabase is a third-party managed database used by some Unity games. Its
binary format is not a Unity SerializedFile. The normal shipped repository is
named `bansheegz_database.bytes`, but applications can store complete
repositories or Save/Load snapshots under `.sav`, `.dat`, or arbitrary names.

The `r2unity` bin plugin implements a conservative format-v6 baseline. It
requires a valid length-prefixed `BansheeGz.BGDatabase.*` addon descriptor and
the exact number of declared canonical table blocks before accepting a file.
For the `SaveFile.dat` reference fixture it recovers:

- repository format version 6 and the 16-byte repository identifier
- one `BGAddonSaveLoad` descriptor with a 360-byte configuration payload
- three columnar table regions with 6, 23, and 10 fields
- 50 validated length-prefixed UTF-8 strings
- header/addon/schema/table sections, symbols, classes, libraries, and fields

Unknown proprietary field encodings remain inside their bounded table region.
Ordinary database cells are exposed as fields or strings, not as
`RBinResource` entries. Future versions should add a versioned BGDatabase field
type registry and only expose actual byte-array/document cells as resources.

The relevant upstream descriptions are the
[binary repository setup](https://www.bansheegz.com/BGDatabase/Setup/) and
[Save/Load addon](https://www.bansheegz.com/BGDatabase/Addons/SaveLoad/)
documentation. They describe the public API and deployment model, but not the
binary wire format; the layout above is therefore based on bounded structural
recovery from the reference fixture.

## 10. Saved state, PlayerPrefs, and StreamingAssets

### 10.1 `Application.persistentDataPath`

Unity exposes `Application.persistentDataPath` as a per-application directory
for data retained between runs. Platform paths differ, but filenames and
formats inside the directory are chosen by the game.

Reference:

- [Unity: Application.persistentDataPath](https://docs.unity3d.com/kr/current/ScriptReference/Application-persistentDataPath.html)

An r2unity save-analysis workflow should therefore:

1. identify the exact file path and platform
2. inspect magic, entropy, strings, and compression
3. search IL2CPP metadata for likely save/model/serializer types
4. locate file and serialization calls in the native IL2CPP binary
5. reconstruct a schema specific to that game and version

r2unity can provide triage and schema-recovery assistance, but cannot provide
one parser for every `save.dat`.

### 10.2 PlayerPrefs

`PlayerPrefs` stores string, integer, and floating-point preferences through
platform-native storage. Current Unity documentation lists, among others:

- Android SharedPreferences XML
- iOS `NSUserDefaults`
- macOS plist files
- the Windows registry
- Linux configuration paths
- UWP `playerprefs.dat`

PlayerPrefs is not encrypted and should not be confused with arbitrary files
under `persistentDataPath`.

Reference:

- [Unity: PlayerPrefs](https://docs.unity3d.com/ja/current/ScriptReference/PlayerPrefs.html)

### 10.3 StreamingAssets

Files placed in a project's `StreamingAssets` directory are generally copied
into the build as files rather than converted into `resources.assets`.
Applications commonly use this area for databases, media, configuration, or
custom packs. These are shipped inputs and are often read-only on installed
platforms; writable state belongs under `persistentDataPath`.

A `.dat` found in StreamingAssets is therefore likely application-supplied
content, not automatically a saved game.

## 11. Addressables

Addressables add a lookup layer above AssetBundles. A content catalog maps
keys and labels to physical locations, provider information, dependencies,
and bundle entries.

Common artifacts include:

- `catalog.json` or a binary/compressed local catalog
- `catalog.bin` in some package/build configurations
- `catalog.hash`
- platform directories containing `*.bundle`

The hash determines whether a remote catalog differs from the local catalog.
The catalog does not replace AssetBundle or SerializedFile parsing; it tells
the loader which bundle and provider contain a requested address.

Reference:

- [Unity Addressables content catalogs](https://docs.unity3d.com/Packages/com.unity.addressables%402.7/manual/build-content-catalogs.html)

A complete r2unity implementation should parse catalogs only after UnityFS
and SerializedFile support exists, then use them to build a deployment graph:

```text
address / label
    → catalog location
    → bundle and dependencies
    → UnityFS member
    → SerializedFile object
    → optional .resS/.resource slice
```

## 12. Quick identification guide

| Leading bytes or evidence | Likely format |
| --- | --- |
| `UnityFS\0` | Modern Unity archive / AssetBundle |
| sanity `0xFAB11BAF` (`af 1b b1 fa` on little-endian disk) plus plausible version | `global-metadata.dat` |
| `0xBEEFCACE` (`ce ca ef be` on disk) | Standard .NET `.resources` |
| `FSB5` | FMOD sound bank, sometimes inside a Unity `.resource` sidecar |
| valid IL2CPP record area plus `*.dll-resources.dat` path/name | IL2CPP manifest-resource pack |
| plausible versioned header and Unity version string, but no textual magic | Unity SerializedFile |
| little-endian version 6, repository ID, and bounded `BansheeGz.BGDatabase.*` addon descriptors | BGDatabase v6 repository/save |
| `PK\x03\x04`, gzip, SQLite, JSON, XML, etc. | Inner/common format; especially relevant for custom saves and StreamingAssets |
| no magic and high entropy | Possibly compressed, encrypted, packed, or simply raw media data |

Identification must always validate lengths and offsets. Magic matching alone
is insufficient for hostile or corrupted input.

## 13. radare2 resource API

### 13.1 Data model

Current radare2 exposes binary resources with `RBinResource`, whose useful
members include:

- `name`
- `type`
- `encoding`
- `language`
- `timestamp`
- `origin`
- `vaddr`, `paddr`, and `size`
- numeric and textual identifiers
- resource index, type ID, language ID, and code page

A bin plugin implements `load_resources(RBinFile *bf)` and appends entries to
`bf->bo->resources_vec`. Loading is lazy and cached. The generic extractor
validates that an inline range fits within the current file.

Supported generic `encoding` transformations currently include raw data,
base64, data URIs, gzip/zlib, LZ4, and UTF-16 variants. `encoding` describes
the bytes of one resource; it must not be used as a substitute for mounting a
compressed container such as UnityFS.

### 13.2 Commands

radare2 commands are:

```text
iU              list resources and metadata
iU,             table output/query
iU*             emit resource flags as r2 commands
iUj             JSON output
iUq             address, size, type, and name
iUqq            names only
iUx [directory] extract resources
ixU [directory] extraction alias
```

`rabin2` provides:

```text
rabin2 -U file
rabin2 -jU file
rabin2 -xU [-o directory] file
```

Extraction sanitizes generated filenames and refuses to overwrite existing
files.

### 13.3 r2hermes precedent

The r2hermes HBC plugin demonstrates how a non-PE plugin can use the API. It
examines HBC string data for complete SVG, HTML, XML, PEM, and data-URI
payloads, then records the underlying byte range as an `RBinResource` with a
MIME type and origin.

The same conservative classifier can be reused for complete documents or
data URIs in r2unity's managed `ldstr` literals. Ordinary strings should stay
in the string API and must not all become resources.

## 14. Current r2unity and radare2 coverage

### 14.1 r2unity today

The current `bin_r2unity` plugin recognizes IL2CPP `global-metadata.dat`,
IL2CPP `*-resources.dat`, Unity SerializedFile v22, and BGDatabase v6. The
metadata path exposes
metadata sections, strings, symbols, imports, classes, fields, and header
information. The SerializedFile path exposes structural sections, object and
payload symbols, type classes, fields, strings, external dependencies, header
information, and named objects through `load_resources`.

Companion discovery currently locates:

- the main executable/input
- the IL2CPP native binary
- `global-metadata.dat`
- the Player Data directory

Resource packs opened directly expose named payloads through `RBinResource`.
Companion discovery does not yet enumerate `Managed/Resources`, loose
SerializedFiles, UnityFS archives, sidecars, or Addressables content. A loose
v22 SerializedFile can nevertheless be opened directly by r2 or rabin2.

The same `bin_r2unity` plugin recognizes the strongly validated BGDatabase v6
repository layout described in section 9.5. The dedicated BGDatabase parser
keeps its weak-magic detection separate from the Unity SerializedFile parser
while sharing one radare2 bin plugin entry point.

The existing documentation correctly states that Unity asset, scene, prefab,
and AssetBundle data is not stored in `global-metadata.dat`.

### 14.2 radare2 today

radare2 provides the generic resource API and commands, and currently exposes:

- native PE resource-directory entries
- embedded CLR ManifestResource payloads
- resources from plugins that implement `load_resources`, such as the
  Pebble resource-pack plugin and r2hermes when installed

No parser in radare2 core was found for:

- UnityFS
- Unity `.resS` or `.resource` sidecars
- IL2CPP `*.dll-resources.dat`
- nested/standalone .NET `.resources` entries
- standalone Windows `.res`

The out-of-tree r2unity plugin now fills this gap for SerializedFile v22 and
IL2CPP manifest-resource packs.
Running core radare2 without that plugin still produces no Unity asset
resources, confirming that generic command wiring and format parsing are
separate concerns.

## 15. Implementation roadmap

### Stage 0: fixtures and format boundaries

- preserve small valid fixtures for each format and version
- include malformed/truncated cases
- document which fixtures are distributable
- distinguish container, object database, sidecar, and inner payload APIs
- add file-identification tests before enabling weak/no-magic detection

### Stage 1: IL2CPP manifest-resource packs in r2unity

Implemented in the existing `bin_r2unity` plugin with a dedicated library
parser for `*.dll-resources.dat`:

- descriptors and payload ranges are fully bounded
- records are exposed through `RBinResource`
- common payload types are detected from extension and magic
- the assembly is recorded as `origin`
- `iU`, JSON, flags, and safe extraction work without new commands
- System.Data, System.Drawing, and mscorlib fixtures are covered

Keeping a dedicated parser and object kind preserves the distinct detector
and lifetime while avoiding an additional bin plugin.

### Stage 2: generic .NET `.resources` in radare2

Add a standalone parser that:

- validates `0xBEEFCACE` and all table offsets
- enumerates dictionary keys and value records
- identifies built-in primitive, string, byte-array, and stream types
- exposes raw values safely without deserializing arbitrary objects
- works when opened directly
- can be invoked recursively on a PE or IL2CPP manifest-resource payload

This belongs in radare2 because it benefits every managed binary format.

### Stage 3: conservative resources from metadata literals

Optionally add `load_resources` to the existing `bin_r2unity` plugin and
reuse the r2hermes classifier for:

- complete XML documents
- SVG
- HTML
- PEM blocks
- data URIs

This is heuristic convenience, not Unity asset parsing. All ordinary `ldstr`
literals remain strings.

### Stage 4: Unity SerializedFile library and bin plugin

The v22 structural baseline is implemented: bounded header/metadata parsing,
serialized types, object records, script and external references, sections,
symbols, classes, fields, strings, libraries, common names, `TextAsset`
payloads, `Texture2D` stream information, and `RBinResource` records.

Continue with versioned parsing for:

- older and newer SerializedFile headers and metadata additions
- interpreted type trees rather than structural skipping
- a broader versioned database of built-in object layouts
- managed `MonoBehaviour` and `ScriptableObject` schemas

Expand the current loose `.assets` support across Unity versions before using
it recursively inside UnityFS archives.

### Stage 5: external sidecar resource access in radare2

Add a plugin-level resource-data resolver or equivalent abstraction so that a
resource owned by one `RBinFile` can return bytes from a companion buffer.

Requirements include:

- safe path resolution relative to the deployment/archive
- retaining or reopening companion buffers safely
- offset/size bounds checks against the sidecar
- showing the real origin in `iU`/JSON
- extraction without changing the current seek/file

### Stage 6: `.resS` and `.resource` resolution in r2unity

The v22 `Texture2D` stream reference is now reported. Continue decoding
references for common classes and map them to sidecars:

- Mesh
- AudioClip
- VideoClip
- other version-specific streamed data objects

Initially expose raw slices. Codec-specific plugins such as FSB5 can be added
independently or invoked on extracted buffers.

### Stage 7: UnityFS extraction or mounting

Implement:

- `UnityFS` validation
- block-info placement and flags
- uncompressed blocks
- LZ4/LZ4HC blocks
- LZMA content
- directory nodes
- members spanning block boundaries
- recursive opening of SerializedFiles

An extractor and a filesystem view are both useful, but one shared archive
library should implement parsing and decompression.

### Stage 8: AssetBundle and Addressables semantics

- decode AssetBundle container objects
- map asset names to path IDs
- follow external SerializedFile and bundle dependencies
- parse JSON and binary/compressed catalogs
- map addresses and labels to bundle members and Unity objects
- expose a cross-file object/resource graph

### Stage 9: standalone Win32 `.res` in radare2

Reuse PE resource types and naming where possible, but parse the compiler's
concatenated `.res` representation directly. This is generic Windows support,
not Unity-specific work.

### Stage 10: save-data triage

Add discovery and analysis helpers rather than claiming a universal save
format:

- identify PlayerPrefs and persistent-data locations
- recognize common inner formats and compression
- find likely save and serializer types in IL2CPP metadata
- correlate file I/O and serializer calls with native methods
- let per-game plugins/scripts describe recovered schemas

## 16. Parser safety requirements

All these formats are attacker-controlled inputs from radare2's perspective.
Every implementation should:

- perform checked arithmetic before adding offsets and sizes
- cap counts, strings, type-tree depth, and decompressed sizes
- validate every range before reading or slicing
- reject overlapping or contradictory regions unless specified
- avoid native object deserialization for `.resources` and saves
- protect recursive container traversal from cycles
- protect dependency traversal from path escape
- treat compressed-size and uncompressed-size fields as untrusted
- include truncated and oversized fuzz cases
- avoid detector false positives for no-magic formats

UnityFS, SerializedFile, and `.resources` parsers should be fuzzed separately
from higher-level object decoders so that structural bugs are isolated from
class-schema bugs.

## 17. Recommended ownership split

The clean project boundary is:

### r2unity

- `global-metadata.dat`
- `*.dll-resources.dat`
- Unity SerializedFile
- `.resS` / `.resource` stream resolution
- UnityFS and AssetBundle semantics
- Addressables lookup
- IL2CPP-assisted save-schema recovery

### radare2

- generic `RBinResource` commands and extraction
- plugin-supplied/external resource-buffer support
- standalone and nested .NET `.resources`
- standalone Win32 `.res`
- reusable LZ4/LZMA primitives and container infrastructure

Unity-specific parsers can remain out-of-tree in r2unity while maturing. Only
generic APIs and formats shared with non-Unity binaries need to enter radare2
core.

## 18. Practical next milestone

After the SerializedFile v22 baseline, the next useful milestone is:

1. add fixture-backed SerializedFile versions around v22 and more built-in
   object decoders
2. add the optional document/data-URI classifier to managed literals
3. implement generic `.resources` parsing in radare2 next

This produces immediate resource visibility without pretending that raw
`.resS`, arbitrary `.dat` saves, or full Unity object serialization are the
same problem. SerializedFile and UnityFS support can then grow on top of a
clear container/resource abstraction. The current v22 implementation provides
that initial object-database layer.

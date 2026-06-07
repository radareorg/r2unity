#ifndef R2UNITY_LIB_H
#define R2UNITY_LIB_H

#include <r_util.h>

struct r_bin_t;
struct r_bin_file_t;

#define IL2CPP_MAGIC 0xFAB11BAF
#define R2UNITY_METADATA_BASE_SECTION_COUNT 31
#define R2UNITY_METADATA_SECTION_COUNT 33

typedef enum {
	R2U_SEC_STRING_LITERALS,
	R2U_SEC_STRING_LITERAL_DATA,
	R2U_SEC_STRINGS,
	R2U_SEC_EVENTS,
	R2U_SEC_PROPERTIES,
	R2U_SEC_METHODS,
	R2U_SEC_PARAMETER_DEFAULT_VALUES,
	R2U_SEC_FIELD_DEFAULT_VALUES,
	R2U_SEC_FIELD_AND_PARAMETER_DEFAULT_VALUE_DATA,
	R2U_SEC_FIELD_MARSHALED_SIZES,
	R2U_SEC_PARAMETERS,
	R2U_SEC_FIELDS,
	R2U_SEC_GENERIC_PARAMETERS,
	R2U_SEC_GENERIC_PARAMETER_CONSTRAINTS,
	R2U_SEC_GENERIC_CONTAINERS,
	R2U_SEC_NESTED_TYPES,
	R2U_SEC_INTERFACES,
	R2U_SEC_VTABLE_METHODS,
	R2U_SEC_INTERFACE_OFFSETS,
	R2U_SEC_TYPE_DEFINITIONS,
	R2U_SEC_IMAGES,
	R2U_SEC_ASSEMBLIES,
	R2U_SEC_FIELD_REFS,
	R2U_SEC_REFERENCED_ASSEMBLIES,
	R2U_SEC_ATTRIBUTE_DATA,
	R2U_SEC_ATTRIBUTE_DATA_RANGES,
	R2U_SEC_UNRESOLVED_INDIRECT_CALL_PARAMETER_TYPES,
	R2U_SEC_UNRESOLVED_INDIRECT_CALL_PARAMETER_RANGES,
	R2U_SEC_WINDOWS_RUNTIME_TYPE_NAMES,
	R2U_SEC_WINDOWS_RUNTIME_STRINGS,
	R2U_SEC_EXPORTED_TYPE_DEFINITIONS,
	R2U_SEC_RGCTX_ENTRIES,
	R2U_SEC_RGCTX_ENTRIES_DATA
} R2UMetadataSectionId;

typedef struct {
	uint32_t offset;
	uint32_t size;
	uint32_t count;
} Il2CppMetadataSection;

typedef struct {
	int32_t version;
	RBuffer *buf;
	RBuffer *strings;
	RBuffer *string_literals;
	/* Normalized header fields for easy access across versions */
	uint32_t stringOffset;
	int32_t stringSize;
	uint32_t stringLiteralOffset;
	int32_t stringLiteralSize;
	uint32_t stringLiteralDataOffset;
	int32_t stringLiteralDataSize;
	uint32_t methodsOffset;
	int32_t methodsSize;
	uint32_t typeDefinitionsOffset;
	int32_t typeDefinitionsSize;
	Il2CppMetadataSection sections[R2UNITY_METADATA_SECTION_COUNT];
	int typeIndexSize;
	int typeDefinitionIndexSize;
	int genericContainerIndexSize;
	int parameterIndexSize;
} R2UnityMetadata;

typedef struct {
	uint32_t length;
	uint32_t dataIndex;
} Il2CppStringLiteral;

typedef struct {
	uint32_t nameIndex;
	uint32_t namespaceIndex;
	int32_t byvalTypeIndex;
	int32_t declaringTypeIndex;
	int32_t parentIndex;
	int32_t elementTypeIndex;
	int32_t genericContainerIndex;
	uint32_t flags;
	int32_t fieldStart;
	int32_t methodStart;
	int32_t eventStart;
	int32_t propertyStart;
	int32_t nestedTypesStart;
	int32_t interfacesStart;
	int32_t vtableStart;
	int32_t interfaceOffsetsStart;
	uint16_t method_count;
	uint16_t property_count;
	uint16_t field_count;
	uint16_t event_count;
	uint16_t nested_type_count;
	uint16_t vtable_count;
	uint16_t interfaces_count;
	uint16_t interface_offsets_count;
	uint32_t bitfield;
	uint32_t token;
} Il2CppTypeDefinition;

typedef struct {
	uint32_t nameIndex;
	int32_t declaringType;
	int32_t returnType;
	int32_t parameterStart;
	int32_t genericContainerIndex;
	uint32_t token;
	uint16_t flags;
	uint16_t iflags;
	uint16_t slot;
	uint16_t parameterCount;
} Il2CppMethodDefinition;

typedef struct {
	uint32_t nameIndex;
	int32_t typeIndex;
	uint32_t token;
} Il2CppFieldDefinition;

typedef struct {
	uint32_t nameIndex;
	int32_t assemblyIndex;
	int32_t typeStart;
	uint32_t typeCount;
	int32_t exportedTypeStart;
	uint32_t exportedTypeCount;
	int32_t entryPointIndex;
	uint32_t token;
	int32_t customAttributeStart;
	uint32_t customAttributeCount;
} Il2CppImageDefinition;

typedef struct {
	uint32_t name_idx;
	uint32_t culture_idx;
	uint32_t public_key_idx;
	uint32_t hash_value_idx; /* 0 when not present on disk (wire >= 24.4) */
	uint32_t hash_alg;
	int32_t hash_len;
	uint32_t flags;
	int32_t major;
	int32_t minor;
	int32_t build;
	int32_t revision;
	uint8_t public_key_token[8];
} Il2CppAssemblyNameDefinition;

typedef struct {
	int32_t image_index;
	uint32_t token; /* 0 when not present (wire <= 24.0) */
	int32_t referenced_start;
	int32_t referenced_count;
	Il2CppAssemblyNameDefinition aname;
} Il2CppAssemblyDefinition;

/* ECMA-335 MethodAttributes.PinvokeImpl — bit 13 on methodDef.flags */
#define IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL 0x2000

enum {
	R2U_INTEROP_PINVOKE = 1, /* managed -> native, [DllImport] */
	R2U_INTEROP_REVERSE_PINVOKE = 2, /* native -> managed, [MonoPInvokeCallback] */
	R2U_INTEROP_UNMANAGED_ONLY = 3, /* [UnmanagedCallersOnly] (.NET 5+) */
	R2U_INTEROP_INTEROP_DATA = 4 /* Il2CppInteropData entry (opaque) */
};

/* A single P/Invoke or reverse-P/Invoke entry.
 * String fields are owned by the struct and freed by r2unity_free_interop. */
typedef struct {
	int32_t method_index; /* -1 for anonymous native wrapper */
	uint32_t token; /* 0 if not applicable */
	int32_t image_index; /* -1 if unresolved */
	uint16_t flags; /* copy of methodDef.flags */
	uint16_t iflags; /* copy of methodDef.iflags */
	char *name; /* "Ns.Class.Method" or NULL */
	char *image_name; /* "Assembly-CSharp" or NULL */
	char *dll_name; /* NULL if unrecovered */
	char *entry_name; /* NULL if unrecovered (=> method name) */
	ut64 wrapper_va; /* 0 if no native wrapper */
	uint32_t wrapper_index; /* global reversePInvokeWrappers index, or UINT32_MAX */
	uint8_t kind; /* R2U_INTEROP_* */
	uint8_t confidence; /* 0..100 */
} R2UnityInterop;

typedef enum {
	R2U_NAME_WITH_PARAMS = 1,
	R2U_NAME_FALLBACK_TYPE = 2
} R2UnityNameFlags;

typedef enum {
	R2U_SBOM_TEXT,
	R2U_SBOM_JSON
} R2UnitySbomFormat;

typedef enum {
	R2U_NATIVE_SOURCE_NONE,
	R2U_NATIVE_SOURCE_SYMBOL,
	R2U_NATIVE_SOURCE_OVERRIDE,
	R2U_NATIVE_SOURCE_HEURISTIC
} R2UnityNativeSource;

typedef struct {
	const char *name;
	ut64 va;
} R2UnitySymbolOverride;

typedef struct {
	bool force_heuristic;
	ut64 code_registration_va;
	ut64 metadata_registration_va;
	const R2UnitySymbolOverride *symbols;
	size_t symbols_count;
} R2UnityNativeOptions;

typedef struct {
	ut64 *method_ptrs; /* owned; free with r2unity_native_result_fini() */
	bool has_method_ptrs;
	R2UnityNativeSource source;
	ut64 code_registration_va;
	ut64 metadata_registration_va;
	ut64 method_pointers_va;
	ut64 code_gen_modules_va;
	int ptr_size;
} R2UnityNativeResult;

R_API R2UnityMetadata *r2unity_parse_metadata(RBuffer *buf);
R_API void r2unity_free_metadata(R2UnityMetadata *meta);
/* Caller owns the returned string and must free() it. NULL on missing/empty. */
R_API char *r2unity_get_string(R2UnityMetadata *meta, uint32_t index);
R_API char *r2unity_type_fullname(R2UnityMetadata *meta, const Il2CppTypeDefinition *td, size_t type_idx, int flags);
R_API char *r2unity_method_fullname(R2UnityMetadata *meta, const Il2CppMethodDefinition *m, const Il2CppTypeDefinition *td, size_t type_idx, int flags);
R_API Il2CppStringLiteral *r2unity_get_string_literals(R2UnityMetadata *meta, size_t *count);
R_API bool r2unity_read_string_literal(R2UnityMetadata *meta, const Il2CppStringLiteral *lit, ut8 **out_bytes, size_t *out_len);
R_API Il2CppTypeDefinition *r2unity_get_type_definitions(R2UnityMetadata *meta, size_t *count);
R_API Il2CppMethodDefinition *r2unity_get_method_definitions(R2UnityMetadata *meta, size_t *count);
R_API Il2CppFieldDefinition *r2unity_get_field_definitions(R2UnityMetadata *meta, size_t *count);
R_API int32_t *r2unity_get_type_index_table(R2UnityMetadata *meta, R2UMetadataSectionId id, size_t *count);
R_API Il2CppImageDefinition *r2unity_get_images(R2UnityMetadata *meta, size_t *count);
R_API int *r2unity_build_type_image_map(const Il2CppImageDefinition *images, size_t image_count, size_t type_count);
R_API int r2unity_image_index_for_method(const int *type2img, size_t type_count, const Il2CppMethodDefinition *m);
R_API Il2CppAssemblyDefinition *r2unity_get_assemblies(R2UnityMetadata *meta, size_t *count);
R_API int32_t *r2unity_get_referenced_assemblies(R2UnityMetadata *meta, size_t *count);
R_API R2UnityInterop *r2unity_enumerate_pinvokes(R2UnityMetadata *meta, size_t *count);
/* Reverse-P/Invoke (native -> managed) enumeration via v29+ attribute BLOB.
 * Returns methods tagged with [MonoPInvokeCallback] or [UnmanagedCallersOnly].
 * Pre-v29 metadata returns NULL (attribute ctor args live in generator stubs). */
R_API R2UnityInterop *r2unity_enumerate_reverse_pinvokes(R2UnityMetadata *meta, size_t *count);
R_API void r2unity_free_interop(R2UnityInterop *items, size_t count);
R_API char *r2unity_sbom_tostring(R2UnityMetadata *meta, const char *exe_path, const char *metadata_path, R2UnitySbomFormat format);
R_API const char *r2unity_unity_range_from_wire(int wire);
R_API const char *r2unity_metadata_section_name(R2UMetadataSectionId id);
R_API bool r2unity_metadata_section(R2UnityMetadata *meta, R2UMetadataSectionId id, Il2CppMetadataSection *section);
R_API ut64 r2unity_metadata_section_entry_size(R2UnityMetadata *meta, R2UMetadataSectionId id);
R_API ut64 r2unity_metadata_section_count(R2UnityMetadata *meta, R2UMetadataSectionId id);
R_API ut64 r2unity_metadata_header_size(R2UnityMetadata *meta);
R_API const char *r2unity_native_source_name(R2UnityNativeSource source);
R_API const char *const *r2unity_native_code_registration_names(void);
R_API const char *const *r2unity_native_metadata_registration_names(void);
R_API void r2unity_native_result_fini(R2UnityNativeResult *result);
R_API bool r2unity_find_method_pointers(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result);
R_API bool r2unity_find_method_pointers_rbin(R2UnityMetadata *meta, struct r_bin_t *bin, struct r_bin_file_t *bf, const R2UnityNativeOptions *options, R2UnityNativeResult *result);

/* Companion-file discovery.
 *
 * Given any file or root directory inside a Unity IL2CPP deployment (the main
 * app binary, the IL2CPP native library, or a sibling dropped into a flat
 * fixture directory), r2unity_detect_paths probes the well-known layouts for
 * iOS, macOS, Windows, Linux, Android (extracted APK) and the r2unity
 * companion-file layout. Returns NULL if no layout matches or
 * `global-metadata.dat` cannot be located. */
typedef struct {
	char *platform; /* "ios", "macos", "windows", "linux", "android", "fixture" */
	char *main_executable; /* absolute path of the input file */
	char *il2cpp_binary; /* UnityFramework / GameAssembly.* / libil2cpp.so — may be NULL */
	char *metadata; /* global-metadata.dat (always set when the function succeeds) */
	char *data_dir; /* root Data folder — may be NULL */
} R2UnityPaths;

R_API R2UnityPaths *r2unity_detect_paths(const char *main_exe_path);
R_API void r2unity_free_paths(R2UnityPaths *p);
R_API const char *r2unity_platform_il2cpp_name(const char *platform);

#endif

#ifndef R2UNITY_LIB_H
#define R2UNITY_LIB_H

#include <r_util.h>

#define IL2CPP_MAGIC 0xFAB11BAF

typedef struct {
	uint32_t sanity;
	int32_t version;
	uint32_t stringLiteralOffset;
	int32_t stringLiteralSize;
	uint32_t stringLiteralDataOffset;
	int32_t stringLiteralDataSize;
	uint32_t stringOffset;
	int32_t stringSize;
	uint32_t eventsOffset;
	int32_t eventsSize;
	uint32_t propertiesOffset;
	int32_t propertiesSize;
	uint32_t methodsOffset;
	int32_t methodsSize;
	uint32_t parameterDefaultValuesOffset;
	int32_t parameterDefaultValuesSize;
	uint32_t fieldDefaultValuesOffset;
	int32_t fieldDefaultValuesSize;
	uint32_t fieldAndParameterDefaultValueDataOffset;
	int32_t fieldAndParameterDefaultValueDataSize;
	uint32_t fieldMarshaledSizesOffset;
	int32_t fieldMarshaledSizesSize;
	uint32_t parametersOffset;
	int32_t parametersSize;
	uint32_t fieldsOffset;
	int32_t fieldsSize;
	uint32_t genericParametersOffset;
	int32_t genericParametersSize;
	uint32_t genericParameterConstraintsOffset;
	int32_t genericParameterConstraintsSize;
	uint32_t genericContainersOffset;
	int32_t genericContainersSize;
	uint32_t nestedTypesOffset;
	int32_t nestedTypesSize;
	uint32_t interfacesOffset;
	int32_t interfacesSize;
	uint32_t vtableMethodsOffset;
	int32_t vtableMethodsSize;
	uint32_t interfaceOffsetsOffset;
	int32_t interfaceOffsetsSize;
	uint32_t typeDefinitionsOffset;
	int32_t typeDefinitionsSize;
	uint32_t imagesOffset;
	int32_t imagesSize;
	uint32_t assembliesOffset;
	int32_t assembliesSize;
	uint32_t fieldRefsOffset;
	int32_t fieldRefsSize;
	uint32_t referencedAssembliesOffset;
	int32_t referencedAssembliesSize;
	uint32_t attributesInfoOffset;
	int32_t attributesInfoSize;
	uint32_t attributeTypesOffset;
	int32_t attributeTypesSize;
	uint32_t unresolvedVirtualCallParameterTypesOffset;
	int32_t unresolvedVirtualCallParameterTypesSize;
	uint32_t unresolvedVirtualCallParameterRangesOffset;
	int32_t unresolvedVirtualCallParameterRangesSize;
	uint32_t windowsRuntimeTypeNamesOffset;
	int32_t windowsRuntimeTypeNamesSize;
	uint32_t windowsRuntimeStringsOffset;
	int32_t windowsRuntimeStringsSize;
	uint32_t exportedTypeDefinitionsOffset;
	int32_t exportedTypeDefinitionsSize;
} Il2CppGlobalMetadataHeader_v24;

typedef struct {
	uint32_t sanity;
	int32_t version;
	uint32_t stringLiteralOffset;
	int32_t stringLiteralSize;
	uint32_t stringLiteralDataOffset;
	int32_t stringLiteralDataSize;
	uint32_t stringOffset;
	int32_t stringSize;
	uint32_t eventsOffset;
	int32_t eventsSize;
	uint32_t propertiesOffset;
	int32_t propertiesSize;
	uint32_t methodsOffset;
	int32_t methodsSize;
	uint32_t parameterDefaultValuesOffset;
	int32_t parameterDefaultValuesSize;
	uint32_t fieldDefaultValuesOffset;
	int32_t fieldDefaultValuesSize;
	uint32_t fieldAndParameterDefaultValueDataOffset;
	int32_t fieldAndParameterDefaultValueDataSize;
	uint32_t fieldMarshaledSizesOffset;
	int32_t fieldMarshaledSizesSize;
	uint32_t parametersOffset;
	int32_t parametersSize;
	uint32_t fieldsOffset;
	int32_t fieldsSize;
	uint32_t genericParametersOffset;
	int32_t genericParametersSize;
	uint32_t genericParameterConstraintsOffset;
	int32_t genericParameterConstraintsSize;
	uint32_t genericContainersOffset;
	int32_t genericContainersSize;
	uint32_t nestedTypesOffset;
	int32_t nestedTypesSize;
	uint32_t interfacesOffset;
	int32_t interfacesSize;
	uint32_t vtableMethodsOffset;
	int32_t vtableMethodsSize;
	uint32_t interfaceOffsetsOffset;
	int32_t interfaceOffsetsSize;
	uint32_t typeDefinitionsOffset;
	int32_t typeDefinitionsSize;
	uint32_t imagesOffset;
	int32_t imagesSize;
	uint32_t assembliesOffset;
	int32_t assembliesSize;
	uint32_t fieldRefsOffset;
	int32_t fieldRefsSize;
	uint32_t referencedAssembliesOffset;
	int32_t referencedAssembliesSize;
	uint32_t attributesInfoOffset;
	int32_t attributesInfoSize;
	uint32_t attributeTypesOffset;
	int32_t attributeTypesSize;
	uint32_t unresolvedVirtualCallParameterTypesOffset;
	int32_t unresolvedVirtualCallParameterTypesSize;
	uint32_t unresolvedVirtualCallParameterRangesOffset;
	int32_t unresolvedVirtualCallParameterRangesSize;
	uint32_t windowsRuntimeTypeNamesOffset;
	int32_t windowsRuntimeTypeNamesSize;
	uint32_t windowsRuntimeStringsOffset;
	int32_t windowsRuntimeStringsSize;
	uint32_t exportedTypeDefinitionsOffset;
	int32_t exportedTypeDefinitionsSize;
	uint32_t rgctxEntriesOffset;
	int32_t rgctxEntriesSize;
} Il2CppGlobalMetadataHeader_v27;

typedef struct {
	uint32_t sanity;
	int32_t version;
	uint32_t stringLiteralOffset;
	int32_t stringLiteralSize;
	uint32_t stringLiteralDataOffset;
	int32_t stringLiteralDataSize;
	uint32_t stringOffset;
	int32_t stringSize;
	uint32_t eventsOffset;
	int32_t eventsSize;
	uint32_t propertiesOffset;
	int32_t propertiesSize;
	uint32_t methodsOffset;
	int32_t methodsSize;
	uint32_t parameterDefaultValuesOffset;
	int32_t parameterDefaultValuesSize;
	uint32_t fieldDefaultValuesOffset;
	int32_t fieldDefaultValuesSize;
	uint32_t fieldAndParameterDefaultValueDataOffset;
	int32_t fieldAndParameterDefaultValueDataSize;
	uint32_t fieldMarshaledSizesOffset;
	int32_t fieldMarshaledSizesSize;
	uint32_t parametersOffset;
	int32_t parametersSize;
	uint32_t fieldsOffset;
	int32_t fieldsSize;
	uint32_t genericParametersOffset;
	int32_t genericParametersSize;
	uint32_t genericParameterConstraintsOffset;
	int32_t genericParameterConstraintsSize;
	uint32_t genericContainersOffset;
	int32_t genericContainersSize;
	uint32_t nestedTypesOffset;
	int32_t nestedTypesSize;
	uint32_t interfacesOffset;
	int32_t interfacesSize;
	uint32_t vtableMethodsOffset;
	int32_t vtableMethodsSize;
	uint32_t interfaceOffsetsOffset;
	int32_t interfaceOffsetsSize;
	uint32_t typeDefinitionsOffset;
	int32_t typeDefinitionsSize;
	uint32_t imagesOffset;
	int32_t imagesSize;
	uint32_t assembliesOffset;
	int32_t assembliesSize;
	uint32_t fieldRefsOffset;
	int32_t fieldRefsSize;
	uint32_t referencedAssembliesOffset;
	int32_t referencedAssembliesSize;
	uint32_t attributesInfoOffset;
	int32_t attributesInfoSize;
	uint32_t attributeTypesOffset;
	int32_t attributeTypesSize;
	uint32_t unresolvedVirtualCallParameterTypesOffset;
	int32_t unresolvedVirtualCallParameterTypesSize;
	uint32_t unresolvedVirtualCallParameterRangesOffset;
	int32_t unresolvedVirtualCallParameterRangesSize;
	uint32_t windowsRuntimeTypeNamesOffset;
	int32_t windowsRuntimeTypeNamesSize;
	uint32_t windowsRuntimeStringsOffset;
	int32_t windowsRuntimeStringsSize;
	uint32_t exportedTypeDefinitionsOffset;
	int32_t exportedTypeDefinitionsSize;
	uint32_t rgctxEntriesOffset;
	int32_t rgctxEntriesSize;
	uint32_t rgctxEntriesDataOffset;
	int32_t rgctxEntriesDataSize;
} Il2CppGlobalMetadataHeader_v29;

typedef union {
	Il2CppGlobalMetadataHeader_v24 v24;
	Il2CppGlobalMetadataHeader_v27 v27;
	Il2CppGlobalMetadataHeader_v29 v29;
} Il2CppGlobalMetadataHeader;

typedef struct {
	int32_t version;
	Il2CppGlobalMetadataHeader header;
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
	int32_t  hash_len;
	uint32_t flags;
	int32_t  major;
	int32_t  minor;
	int32_t  build;
	int32_t  revision;
	uint8_t  public_key_token[8];
} Il2CppAssemblyNameDefinition;

typedef struct {
	int32_t  image_index;
	uint32_t token;            /* 0 when not present (wire <= 24.0) */
	int32_t  referenced_start;
	int32_t  referenced_count;
	Il2CppAssemblyNameDefinition aname;
} Il2CppAssemblyDefinition;

/* ECMA-335 MethodAttributes.PinvokeImpl — bit 13 on methodDef.flags */
#define IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL 0x2000

enum {
	R2U_INTEROP_PINVOKE         = 1, /* managed -> native, [DllImport] */
	R2U_INTEROP_REVERSE_PINVOKE = 2, /* native -> managed, [MonoPInvokeCallback] */
	R2U_INTEROP_UNMANAGED_ONLY  = 3, /* [UnmanagedCallersOnly] (.NET 5+) */
	R2U_INTEROP_INTEROP_DATA    = 4  /* Il2CppInteropData entry (opaque) */
};

/* A single P/Invoke or reverse-P/Invoke entry.
 * String fields are owned by the struct and freed by r2unity_free_interop. */
typedef struct {
	int32_t  method_index;   /* -1 for anonymous native wrapper */
	uint32_t token;          /* 0 if not applicable */
	int32_t  image_index;    /* -1 if unresolved */
	uint16_t flags;          /* copy of methodDef.flags */
	uint16_t iflags;         /* copy of methodDef.iflags */
	char    *name;           /* "Ns.Class.Method" or NULL */
	char    *image_name;     /* "Assembly-CSharp" or NULL */
	char    *dll_name;       /* NULL if unrecovered */
	char    *entry_name;     /* NULL if unrecovered (=> method name) */
	ut64     wrapper_va;     /* 0 if no native wrapper */
	uint32_t wrapper_index;  /* global reversePInvokeWrappers index, or UINT32_MAX */
	uint8_t  kind;           /* R2U_INTEROP_* */
	uint8_t  confidence;     /* 0..100 */
} R2UnityInterop;

R_API R2UnityMetadata *r2unity_parse_metadata (RBuffer *buf);
R_API void r2unity_free_metadata (R2UnityMetadata *meta);
R_API const char *r2unity_get_string (R2UnityMetadata *meta, uint32_t index);
R_API Il2CppStringLiteral *r2unity_get_string_literals (R2UnityMetadata *meta, size_t *count);
R_API bool r2unity_read_string_literal (R2UnityMetadata *meta, const Il2CppStringLiteral *lit, ut8 **out_bytes, size_t *out_len);
R_API Il2CppTypeDefinition *r2unity_get_type_definitions (R2UnityMetadata *meta, size_t *count);
R_API Il2CppMethodDefinition *r2unity_get_method_definitions (R2UnityMetadata *meta, size_t *count);
R_API Il2CppImageDefinition *r2unity_get_images (R2UnityMetadata *meta, size_t *count);
R_API Il2CppAssemblyDefinition *r2unity_get_assemblies (R2UnityMetadata *meta, size_t *count);
R_API int32_t *r2unity_get_referenced_assemblies (R2UnityMetadata *meta, size_t *count);
R_API R2UnityInterop *r2unity_enumerate_pinvokes (R2UnityMetadata *meta, size_t *count);
/* Reverse-P/Invoke (native -> managed) enumeration via v29+ attribute BLOB.
 * Returns methods tagged with [MonoPInvokeCallback] or [UnmanagedCallersOnly].
 * Pre-v29 metadata returns NULL (attribute ctor args live in generator stubs). */
R_API R2UnityInterop *r2unity_enumerate_reverse_pinvokes (R2UnityMetadata *meta, size_t *count);
R_API void r2unity_free_interop (R2UnityInterop *items, size_t count);
/* Simplified API: use format-specific finders, or manual read stub. */
R_API bool r2unity_read_method_pointers_at (R2UnityMetadata *meta, const char *exe_path, ut64 addr, size_t count, ut64 **out_ptrs);
R_API bool r2unity_find_method_pointers_macho (R2UnityMetadata *meta, const char *macho_path, ut64 **out_ptrs);
R_API bool r2unity_find_method_pointers_elf (R2UnityMetadata *meta, const char *elf_path, ut64 **out_ptrs);
R_API bool r2unity_find_method_pointers_pe (R2UnityMetadata *meta, const char *pe_path, ut64 **out_ptrs);

R_API void r2unity_set_debug (bool v);
R_API bool r2unity_is_debug (void);

#endif

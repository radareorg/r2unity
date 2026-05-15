/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.lib"

#include "lib.h"
#include <r_util.h>

/* On-disk header layouts. Each newer wire version appends fields, so the older
 * structs are prefixes of the newer ones (except v38 which switched to section
 * tuples). Only used during parsing; the normalized data lives in
 * R2UnityMetadata.sections[]. */
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
	/* v27+ */
	uint32_t rgctxEntriesOffset;
	int32_t rgctxEntriesSize;
	/* v29+ */
	uint32_t rgctxEntriesDataOffset;
	int32_t rgctxEntriesDataSize;
} Il2CppGlobalMetadataHeader_legacy;

typedef struct {
	uint32_t sanity;
	int32_t version;
	Il2CppMetadataSection sections[R2UNITY_METADATA_BASE_SECTION_COUNT];
} Il2CppGlobalMetadataHeader_v38;

static const char *metadata_section_names[R2UNITY_METADATA_SECTION_COUNT] = {
	"string_literals",
	"string_literal_data",
	"strings",
	"events",
	"properties",
	"methods",
	"parameter_default_values",
	"field_default_values",
	"field_and_parameter_default_value_data",
	"field_marshaled_sizes",
	"parameters",
	"fields",
	"generic_parameters",
	"generic_parameter_constraints",
	"generic_containers",
	"nested_types",
	"interfaces",
	"vtable_methods",
	"interface_offsets",
	"type_definitions",
	"images",
	"assemblies",
	"field_refs",
	"referenced_assemblies",
	"attribute_data",
	"attribute_data_ranges",
	"unresolved_indirect_call_parameter_types",
	"unresolved_indirect_call_parameter_ranges",
	"windows_runtime_type_names",
	"windows_runtime_strings",
	"exported_type_definitions",
	"rgctx_entries",
	"rgctx_entries_data"
};

static int index_size_from_count(uint32_t count) {
	if (count <= 0xff) {
		return 1;
	}
	if (count <= 0xffff) {
		return 2;
	}
	return 4;
}

static bool valid_index_size(int size) {
	return size == 1 || size == 2 || size == 4;
}

static int32_t read_sized_index(const ut8 *p, int size) {
	switch (size) {
	case 1: {
		uint32_t value = p[0];
		return value == 0xff? -1: (int32_t)value;
	}
	case 2: {
		uint32_t value = r_read_le16 (p);
		return value == 0xffff? -1: (int32_t)value;
	}
	case 4:
	default: {
		uint32_t value = r_read_le32 (p);
		return value == UT32_MAX? -1: (int32_t)value;
	}
	}
}

static uint32_t read_u32p(const ut8 **p) {
	uint32_t value = r_read_le32 (*p);
	*p += 4;
	return value;
}

static int32_t read_i32p(const ut8 **p) {
	return (int32_t)read_u32p (p);
}

static uint16_t read_u16p(const ut8 **p) {
	uint16_t value = r_read_le16 (*p);
	*p += 2;
	return value;
}

static int32_t read_indexp(const ut8 **p, int size) {
	int32_t value = read_sized_index (*p, size);
	*p += size;
	return value;
}

static void set_section(Il2CppMetadataSection *s, uint32_t off, int32_t size) {
	s->offset = off;
	s->size = (uint32_t)R_MAX (0, size);
	s->count = 0;
}

static void init_legacy_sections(R2UnityMetadata *meta, const Il2CppGlobalMetadataHeader_legacy *h) {
	Il2CppMetadataSection *s = meta->sections;
	set_section (&s[R2U_SEC_STRING_LITERALS], h->stringLiteralOffset, h->stringLiteralSize);
	set_section (&s[R2U_SEC_STRING_LITERAL_DATA], h->stringLiteralDataOffset, h->stringLiteralDataSize);
	set_section (&s[R2U_SEC_STRINGS], h->stringOffset, h->stringSize);
	set_section (&s[R2U_SEC_EVENTS], h->eventsOffset, h->eventsSize);
	set_section (&s[R2U_SEC_PROPERTIES], h->propertiesOffset, h->propertiesSize);
	set_section (&s[R2U_SEC_METHODS], h->methodsOffset, h->methodsSize);
	set_section (&s[R2U_SEC_PARAMETER_DEFAULT_VALUES], h->parameterDefaultValuesOffset, h->parameterDefaultValuesSize);
	set_section (&s[R2U_SEC_FIELD_DEFAULT_VALUES], h->fieldDefaultValuesOffset, h->fieldDefaultValuesSize);
	set_section (&s[R2U_SEC_FIELD_AND_PARAMETER_DEFAULT_VALUE_DATA], h->fieldAndParameterDefaultValueDataOffset, h->fieldAndParameterDefaultValueDataSize);
	set_section (&s[R2U_SEC_FIELD_MARSHALED_SIZES], h->fieldMarshaledSizesOffset, h->fieldMarshaledSizesSize);
	set_section (&s[R2U_SEC_PARAMETERS], h->parametersOffset, h->parametersSize);
	set_section (&s[R2U_SEC_FIELDS], h->fieldsOffset, h->fieldsSize);
	set_section (&s[R2U_SEC_GENERIC_PARAMETERS], h->genericParametersOffset, h->genericParametersSize);
	set_section (&s[R2U_SEC_GENERIC_PARAMETER_CONSTRAINTS], h->genericParameterConstraintsOffset, h->genericParameterConstraintsSize);
	set_section (&s[R2U_SEC_GENERIC_CONTAINERS], h->genericContainersOffset, h->genericContainersSize);
	set_section (&s[R2U_SEC_NESTED_TYPES], h->nestedTypesOffset, h->nestedTypesSize);
	set_section (&s[R2U_SEC_INTERFACES], h->interfacesOffset, h->interfacesSize);
	set_section (&s[R2U_SEC_VTABLE_METHODS], h->vtableMethodsOffset, h->vtableMethodsSize);
	set_section (&s[R2U_SEC_INTERFACE_OFFSETS], h->interfaceOffsetsOffset, h->interfaceOffsetsSize);
	set_section (&s[R2U_SEC_TYPE_DEFINITIONS], h->typeDefinitionsOffset, h->typeDefinitionsSize);
	set_section (&s[R2U_SEC_IMAGES], h->imagesOffset, h->imagesSize);
	set_section (&s[R2U_SEC_ASSEMBLIES], h->assembliesOffset, h->assembliesSize);
	set_section (&s[R2U_SEC_FIELD_REFS], h->fieldRefsOffset, h->fieldRefsSize);
	set_section (&s[R2U_SEC_REFERENCED_ASSEMBLIES], h->referencedAssembliesOffset, h->referencedAssembliesSize);
	set_section (&s[R2U_SEC_ATTRIBUTE_DATA], h->attributesInfoOffset, h->attributesInfoSize);
	set_section (&s[R2U_SEC_ATTRIBUTE_DATA_RANGES], h->attributeTypesOffset, h->attributeTypesSize);
	set_section (&s[R2U_SEC_UNRESOLVED_INDIRECT_CALL_PARAMETER_TYPES], h->unresolvedVirtualCallParameterTypesOffset, h->unresolvedVirtualCallParameterTypesSize);
	set_section (&s[R2U_SEC_UNRESOLVED_INDIRECT_CALL_PARAMETER_RANGES], h->unresolvedVirtualCallParameterRangesOffset, h->unresolvedVirtualCallParameterRangesSize);
	set_section (&s[R2U_SEC_WINDOWS_RUNTIME_TYPE_NAMES], h->windowsRuntimeTypeNamesOffset, h->windowsRuntimeTypeNamesSize);
	set_section (&s[R2U_SEC_WINDOWS_RUNTIME_STRINGS], h->windowsRuntimeStringsOffset, h->windowsRuntimeStringsSize);
	set_section (&s[R2U_SEC_EXPORTED_TYPE_DEFINITIONS], h->exportedTypeDefinitionsOffset, h->exportedTypeDefinitionsSize);
	if (meta->version >= 27) {
		set_section (&s[R2U_SEC_RGCTX_ENTRIES], h->rgctxEntriesOffset, h->rgctxEntriesSize);
	}
	if (meta->version >= 29) {
		set_section (&s[R2U_SEC_RGCTX_ENTRIES_DATA], h->rgctxEntriesDataOffset, h->rgctxEntriesDataSize);
	}
}

static ut8 *read_metadata_table(R2UnityMetadata *meta, R2UMetadataSectionId id, ut64 entry, size_t *count) {
	Il2CppMetadataSection s = meta->sections[id];
	*count = 0;
	if (!entry || s.size < entry) {
		return NULL;
	}
	*count = s.count? (size_t)s.count: (size_t)(s.size / entry);
	ut64 read_size = (ut64)*count * entry;
	if (!*count || read_size > s.size) {
		*count = 0;
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, read_size);
	if (!buf) {
		*count = 0;
		return NULL;
	}
	if (r_buf_read_at (meta->buf, s.offset, buf, read_size) != (st64)read_size) {
		R_FREE (buf);
		*count = 0;
		return NULL;
	}
	return buf;
}

static ut64 string_literal_entry_size(R2UnityMetadata *meta) {
	return meta->version <= 31? 8: 4;
}

static ut64 type_definition_entry_size(R2UnityMetadata *meta) {
	const bool compact_indices = (meta->version >= 35);
	const bool has_element_type = (meta->version <= 31);
	const int type_index_size = compact_indices? meta->typeIndexSize: 4;
	const int generic_container_size = compact_indices? meta->genericContainerIndexSize: 4;
	return 4 + 4
		+ type_index_size + type_index_size + type_index_size
		+ (has_element_type? 4: 0)
		+ generic_container_size
		+ 4 + 32 + 16 + 4 + 4;
}

static ut64 method_definition_entry_size(R2UnityMetadata *meta) {
	const bool has_return_parameter = (meta->version >= 31);
	const bool compact_indices = (meta->version >= 35);
	const int type_definition_size = compact_indices? meta->typeDefinitionIndexSize: 4;
	const int type_index_size = compact_indices? meta->typeIndexSize: 4;
	const int parameter_index_size = meta->version >= 39? meta->parameterIndexSize: 4;
	const int generic_container_size = compact_indices? meta->genericContainerIndexSize: 4;
	return 4
		+ type_definition_size
		+ type_index_size
		+ (has_return_parameter? 4: 0)
		+ parameter_index_size
		+ generic_container_size
		+ 4 + 2 + 2 + 2 + 2;
}

static ut64 image_definition_entry_size(R2UnityMetadata *meta) {
	const bool compact_indices = (meta->version >= 35);
	const int type_definition_size = compact_indices? meta->typeDefinitionIndexSize: 4;
	return 4 + 4 + type_definition_size + 4
		+ type_definition_size + 4 + 4 + 4 + 4 + 4;
}

static ut64 field_definition_entry_size(R2UnityMetadata *meta) {
	const bool compact_indices = (meta->version >= 35);
	const int type_index_size = compact_indices? meta->typeIndexSize: 4;
	return 4 + type_index_size + 4;
}

/* Detect v24.0 vs v24.1+ (both land on disk as version==24).
 *
 * At v24.1 the ImageDefinition row grew by 8 bytes (added
 * customAttributeStart/Count), so imagesSize is a multiple of 40 on v24.1+
 * and 32 on v24.0. When imagesSize is divisible by both (a multiple of 160),
 * probe the 4 bytes at offset 40: under v24.1+ that is row 1's nameIndex
 *(a valid string-pool offset); under v24.0 it is mid-row.
 *
 * Fixing row layouts for v24.0 (larger TypeDefinition, MethodDefinition,
 * ImageDefinition) is tracked separately; reject for now so decoders don't
 * silently return wrong offsets. */
static bool is_v24_0(R2UnityMetadata *meta) {
	Il2CppMetadataSection sec = meta->sections[R2U_SEC_IMAGES];
	ut64 ioff = sec.offset;
	ut64 isize = sec.size;
	if (!isize) {
		return false;
	}
	bool div40 = (isize % 40) == 0;
	bool div32 = (isize % 32) == 0;
	if (div40 && !div32) {
		return false;
	}
	if (div32 && !div40) {
		return true;
	}
	if (isize >= 44) {
		ut8 probe[4];
		if (r_buf_read_at (meta->buf, ioff + 40, probe, 4) == 4) {
			uint32_t name_idx = r_read_le32 (probe);
			if (name_idx < (uint32_t)meta->stringSize) {
				return false;
			}
		}
	}
	return true;
}

/* Wire-version header sizes on disk; v27/v29 are v24 + extra rgctx fields. */
#define R2U_HDR_V24 248
#define R2U_HDR_V27 256
#define R2U_HDR_V29 264

static size_t header_size_for(int32_t version) {
	if (version >= 38) {
		return sizeof (Il2CppGlobalMetadataHeader_v38);
	}
	if (version < 27) {
		return R2U_HDR_V24;
	}
	if (version < 29) {
		return R2U_HDR_V27;
	}
	/* v29..v35 share layout (v30, v31 reuse these fields). */
	return R2U_HDR_V29;
}

R_API R2UnityMetadata *r2unity_parse_metadata(RBuffer *buf) {
	R_RETURN_VAL_IF_FAIL (buf, NULL);
	ut8 preamble[8];
	if (r_buf_read_at (buf, 0, preamble, sizeof (preamble)) != (st64)sizeof (preamble)) {
		return NULL;
	}
	if (r_read_le32 (preamble) != IL2CPP_MAGIC) {
		return NULL;
	}
	int32_t version = (int32_t)r_read_le32 (preamble + 4);
	if (version < 24 || version > 39 || (version > 35 && version < 38)) {
		return NULL;
	}
	const size_t header_size = header_size_for (version);
	R2UnityMetadata *meta = R_NEW0 (R2UnityMetadata);
	meta->version = version;
	meta->buf = buf;
	meta->typeIndexSize = 4;
	meta->typeDefinitionIndexSize = 4;
	meta->genericContainerIndexSize = 4;
	meta->parameterIndexSize = 4;
	if (version >= 38) {
		Il2CppGlobalMetadataHeader_v38 hdr = {0};
		if (r_buf_read_at (buf, 0, (ut8 *)&hdr, header_size) != (st64)header_size) {
			R_FREE (meta);
			return NULL;
		}
		/* All Il2Cpp header fields are 32-bit words; sweep in-place for BE
		 * hosts (no-op on LE). */
		uint32_t *w = (uint32_t *)&hdr;
		const size_t n = header_size / sizeof (uint32_t);
		for (size_t i = 0; i < n; i++) {
			w[i] = r_read_le32 (&w[i]);
		}
		memcpy (meta->sections, hdr.sections,
			sizeof (Il2CppMetadataSection) * R2UNITY_METADATA_BASE_SECTION_COUNT);
		Il2CppMetadataSection *s = meta->sections;
		if (s[R2U_SEC_PARAMETERS].count) {
			uint32_t param_entry = s[R2U_SEC_PARAMETERS].size / s[R2U_SEC_PARAMETERS].count;
			if (param_entry >= 8) {
				meta->typeIndexSize = (int)param_entry - 8;
			}
		}
		if (!valid_index_size (meta->typeIndexSize)) {
			meta->typeIndexSize = 4;
		}
		meta->typeDefinitionIndexSize = index_size_from_count (s[R2U_SEC_TYPE_DEFINITIONS].count);
		meta->genericContainerIndexSize = index_size_from_count (s[R2U_SEC_GENERIC_CONTAINERS].count);
		meta->parameterIndexSize = version >= 39? index_size_from_count (s[R2U_SEC_PARAMETERS].count): 4;
	} else {
		Il2CppGlobalMetadataHeader_legacy hdr = {0};
		if (r_buf_read_at (buf, 0, (ut8 *)&hdr, header_size) != (st64)header_size) {
			R_FREE (meta);
			return NULL;
		}
		uint32_t *w = (uint32_t *)&hdr;
		const size_t n = header_size / sizeof (uint32_t);
		for (size_t i = 0; i < n; i++) {
			w[i] = r_read_le32 (&w[i]);
		}
		init_legacy_sections (meta, &hdr);
	}
	Il2CppMetadataSection *s = meta->sections;
	meta->stringOffset = s[R2U_SEC_STRINGS].offset;
	meta->stringSize = (int32_t)s[R2U_SEC_STRINGS].size;
	meta->stringLiteralOffset = s[R2U_SEC_STRING_LITERALS].offset;
	meta->stringLiteralSize = (int32_t)s[R2U_SEC_STRING_LITERALS].size;
	meta->stringLiteralDataOffset = s[R2U_SEC_STRING_LITERAL_DATA].offset;
	meta->stringLiteralDataSize = (int32_t)s[R2U_SEC_STRING_LITERAL_DATA].size;
	meta->methodsOffset = s[R2U_SEC_METHODS].offset;
	meta->methodsSize = (int32_t)s[R2U_SEC_METHODS].size;
	meta->typeDefinitionsOffset = s[R2U_SEC_TYPE_DEFINITIONS].offset;
	meta->typeDefinitionsSize = (int32_t)s[R2U_SEC_TYPE_DEFINITIONS].size;
	if (meta->version == 24 && is_v24_0 (meta)) {
		R_LOG_ERROR ("v24.0 metadata (Unity 5.6..2018.2) not supported: "
			"TypeDefinition/MethodDefinition/ImageDefinition row layouts "
			"differ from v24.1+ and would silently decode wrong offsets");
		r2unity_free_metadata (meta);
		return NULL;
	}
	meta->strings = r_buf_new_slice (buf, meta->stringOffset, meta->stringSize);
	meta->string_literals = r_buf_new_slice (buf, meta->stringLiteralOffset, meta->stringLiteralSize);
	R_LOG_DEBUG ("meta version=%d strings=%u@0x%x methods=%d@0x%x types=%d@0x%x",
		meta->version, (unsigned)meta->stringSize, (unsigned)meta->stringOffset,
		(int)meta->methodsSize, (unsigned)meta->methodsOffset,
		(int)meta->typeDefinitionsSize, (unsigned)meta->typeDefinitionsOffset);
	if (version >= 38) {
		R_LOG_DEBUG ("v38+ counts literals=%u methods=%u types=%u images=%u index_sizes=%d/%d/%d/%d",
			(unsigned)s[R2U_SEC_STRING_LITERALS].count,
			(unsigned)s[R2U_SEC_METHODS].count,
			(unsigned)s[R2U_SEC_TYPE_DEFINITIONS].count,
			(unsigned)s[R2U_SEC_IMAGES].count,
			meta->typeIndexSize,
			meta->typeDefinitionIndexSize,
			meta->genericContainerIndexSize,
			meta->parameterIndexSize);
	}
	return meta;
}

R_API void r2unity_free_metadata(R2UnityMetadata *meta) {
	if (!meta) {
		return;
	}
	r_unref (meta->strings);
	r_unref (meta->string_literals);
	R_FREE (meta);
}

R_API const char *r2unity_unity_range_from_wire(int wire) {
	switch (wire) {
	case 21: return "5.3.0-5.3.5";
	case 22: return "5.3.6-5.4";
	case 23: return "5.5";
	case 24: return "5.6-2020.1";
	case 27: return "2020.2-2021.3";
	case 29: return "2022.1-2022.3";
	case 31: return "2023.x-6000.x";
	case 39: return "6000.x";
	default: return "unknown";
	}
}

R_API const char *r2unity_metadata_section_name(R2UMetadataSectionId id) {
	if (id < 0 || id >= R2UNITY_METADATA_SECTION_COUNT) {
		return NULL;
	}
	return metadata_section_names[id];
}

R_API bool r2unity_metadata_section(R2UnityMetadata *meta, R2UMetadataSectionId id, Il2CppMetadataSection *section) {
	R_RETURN_VAL_IF_FAIL (meta && section, false);
	if (id < 0 || id >= R2UNITY_METADATA_SECTION_COUNT) {
		return false;
	}
	*section = meta->sections[id];
	return true;
}

R_API ut64 r2unity_metadata_section_entry_size(R2UnityMetadata *meta, R2UMetadataSectionId id) {
	R_RETURN_VAL_IF_FAIL (meta, 0);
	if (id < 0 || id >= R2UNITY_METADATA_SECTION_COUNT) {
		return 0;
	}
	Il2CppMetadataSection sec = meta->sections[id];
	if (sec.count) {
		return sec.size / sec.count;
	}
	switch (id) {
	case R2U_SEC_STRING_LITERALS:
		return string_literal_entry_size (meta);
	case R2U_SEC_METHODS:
		return method_definition_entry_size (meta);
	case R2U_SEC_FIELDS:
		return field_definition_entry_size (meta);
	case R2U_SEC_TYPE_DEFINITIONS:
		return type_definition_entry_size (meta);
	case R2U_SEC_IMAGES:
		return image_definition_entry_size (meta);
	case R2U_SEC_ASSEMBLIES: {
		ut64 image_count = r2unity_metadata_section_count (meta, R2U_SEC_IMAGES);
		return image_count? sec.size / image_count: 0;
	}
	case R2U_SEC_REFERENCED_ASSEMBLIES:
	case R2U_SEC_NESTED_TYPES:
	case R2U_SEC_INTERFACES:
	case R2U_SEC_VTABLE_METHODS:
	case R2U_SEC_EXPORTED_TYPE_DEFINITIONS:
	case R2U_SEC_RGCTX_ENTRIES:
		return 4;
	default:
		return 0;
	}
}

R_API ut64 r2unity_metadata_section_count(R2UnityMetadata *meta, R2UMetadataSectionId id) {
	R_RETURN_VAL_IF_FAIL (meta, 0);
	if (id < 0 || id >= R2UNITY_METADATA_SECTION_COUNT) {
		return 0;
	}
	Il2CppMetadataSection sec = meta->sections[id];
	if (sec.count) {
		return sec.count;
	}
	ut64 entry = r2unity_metadata_section_entry_size (meta, id);
	return entry? sec.size / entry: 0;
}

R_API ut64 r2unity_metadata_header_size(R2UnityMetadata *meta) {
	R_RETURN_VAL_IF_FAIL (meta, 0);
	return header_size_for (meta->version);
}

R_API char *r2unity_get_string(R2UnityMetadata *meta, uint32_t index) {
	R_RETURN_VAL_IF_FAIL (meta, NULL);
	if (!meta->strings || index >= r_buf_size (meta->strings)) {
		return NULL;
	}
	// Align index to the beginning of the string in case it points mid-string
	ut64 idx = index;
	while (idx > 0) {
		ut8 bprev = 0;
		if (r_buf_read_at (meta->strings, idx - 1, &bprev, 1) != 1 || !bprev) {
			break;
		}
		idx--;
	}
	// Read until null terminator
	ut64 len = 0;
	ut8 byte;
	while (r_buf_read_at (meta->strings, idx + len, &byte, 1) == 1 && byte) {
		len++;
	}
	if (!len) {
		return NULL;
	}
	char *str = R_NEWS (char, len + 1);
	if (str) {
		r_buf_read_at (meta->strings, idx, (ut8 *)str, len);
		str[len] = 0;
	}
	return str;
}

R_API Il2CppStringLiteral *r2unity_get_string_literals(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	const bool has_literal_length = meta->version <= 31;
	const ut64 entry = has_literal_length? 8: 4;
	ut8 *buf = read_metadata_table (meta, R2U_SEC_STRING_LITERALS, entry, count);
	if (!buf) {
		return NULL;
	}
	Il2CppStringLiteral *lits = R_NEWS (Il2CppStringLiteral, *count);
	if (!lits) {
		R_FREE (buf);
		*count = 0;
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		if (has_literal_length) {
			lits[i].length = r_read_le32 (p);
			lits[i].dataIndex = r_read_le32 (p + 4);
		} else {
			uint32_t data_index = r_read_le32 (p);
			uint32_t next_index = (i + 1 < *count)
				? r_read_le32 (p + entry)
				: (uint32_t)meta->stringLiteralDataSize;
			lits[i].dataIndex = data_index;
			lits[i].length = (next_index >= data_index)? next_index - data_index: 0;
		}
	}
	R_FREE (buf);
	return lits;
}

R_API bool r2unity_read_string_literal(R2UnityMetadata *meta, const Il2CppStringLiteral *lit, ut8 **out_bytes, size_t *out_len) {
	R_RETURN_VAL_IF_FAIL (meta && lit && out_bytes && out_len, false);
	*out_bytes = NULL;
	*out_len = 0;
	if (lit->length == 0) {
		return true;
	}
	ut64 end = (ut64)lit->dataIndex + (ut64)lit->length;
	if (end > (ut64)meta->stringLiteralDataSize) {
		return false;
	}
	ut8 *bytes = R_NEWS (ut8, lit->length);
	if (!bytes) {
		return false;
	}
	if (r_buf_read_at (meta->buf, (ut64)meta->stringLiteralDataOffset + lit->dataIndex, bytes, lit->length) != (st64)lit->length) {
		R_FREE (bytes);
		return false;
	}
	*out_bytes = bytes;
	*out_len = lit->length;
	return true;
}

R_API Il2CppTypeDefinition *r2unity_get_type_definitions(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	const bool compact_indices = (meta->version >= 35);
	const bool has_element_type = (meta->version <= 31);
	const int type_index_size = compact_indices? meta->typeIndexSize: 4;
	const int generic_container_size = compact_indices? meta->genericContainerIndexSize: 4;
	const ut64 entry = type_definition_entry_size (meta);
	ut8 *buf = read_metadata_table (meta, R2U_SEC_TYPE_DEFINITIONS, entry, count);
	if (!buf) {
		return NULL;
	}
	Il2CppTypeDefinition *types = R_NEWS (Il2CppTypeDefinition, *count);
	if (!types) {
		R_FREE (buf);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		types[i].nameIndex = read_u32p (&p);
		types[i].namespaceIndex = read_u32p (&p);
		types[i].byvalTypeIndex = read_indexp (&p, type_index_size);
		types[i].declaringTypeIndex = read_indexp (&p, type_index_size);
		types[i].parentIndex = read_indexp (&p, type_index_size);
		types[i].elementTypeIndex = has_element_type? read_i32p (&p): types[i].parentIndex;
		types[i].genericContainerIndex = read_indexp (&p, generic_container_size);
		types[i].flags = read_u32p (&p);
		types[i].fieldStart = read_i32p (&p);
		types[i].methodStart = read_i32p (&p);
		types[i].eventStart = read_i32p (&p);
		types[i].propertyStart = read_i32p (&p);
		types[i].nestedTypesStart = read_i32p (&p);
		types[i].interfacesStart = read_i32p (&p);
		types[i].vtableStart = read_i32p (&p);
		types[i].interfaceOffsetsStart = read_i32p (&p);
		types[i].method_count = read_u16p (&p);
		types[i].property_count = read_u16p (&p);
		types[i].field_count = read_u16p (&p);
		types[i].event_count = read_u16p (&p);
		types[i].nested_type_count = read_u16p (&p);
		types[i].vtable_count = read_u16p (&p);
		types[i].interfaces_count = read_u16p (&p);
		types[i].interface_offsets_count = read_u16p (&p);
		types[i].bitfield = read_u32p (&p);
		types[i].token = read_u32p (&p);
	}
	R_FREE (buf);
	return types;
}

R_API Il2CppMethodDefinition *r2unity_get_method_definitions(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	/* On-disk Il2CppMethodDefinition:
	 *   v24.1 - v30: 32 bytes
	 *   v31+:        36 bytes (returnParameterToken inserted at +12,
	 *                          pushing parameterStart.. by +4)
	 *   v39+:        compact TypeIndex/TypeDefinitionIndex/ParameterIndex
	 *                fields shrink rows when table counts fit in 16 bits.
	 * See doc/future.md §4.1 and third_party/Il2CppDumper/.../MetadataClass.cs. */
	const bool has_return_parameter = (meta->version >= 31);
	const bool compact_indices = (meta->version >= 35);
	const int type_definition_size = compact_indices? meta->typeDefinitionIndexSize: 4;
	const int type_index_size = compact_indices? meta->typeIndexSize: 4;
	const int parameter_index_size = meta->version >= 39? meta->parameterIndexSize: 4;
	const int generic_container_size = compact_indices? meta->genericContainerIndexSize: 4;
	const ut64 entry = method_definition_entry_size (meta);
	ut8 *buf = read_metadata_table (meta, R2U_SEC_METHODS, entry, count);
	if (!buf) {
		return NULL;
	}
	Il2CppMethodDefinition *methods = R_NEWS (Il2CppMethodDefinition, *count);
	if (!methods) {
		R_FREE (buf);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		methods[i].nameIndex = read_u32p (&p);
		methods[i].declaringType = read_indexp (&p, type_definition_size);
		methods[i].returnType = read_indexp (&p, type_index_size);
		if (has_return_parameter) {
			p += 4;
		}
		methods[i].parameterStart = read_indexp (&p, parameter_index_size);
		methods[i].genericContainerIndex = read_indexp (&p, generic_container_size);
		methods[i].token = read_u32p (&p);
		methods[i].flags = read_u16p (&p);
		methods[i].iflags = read_u16p (&p);
		methods[i].slot = read_u16p (&p);
		methods[i].parameterCount = read_u16p (&p);
	}
	R_FREE (buf);
	return methods;
}

R_API Il2CppImageDefinition *r2unity_get_images(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	const bool compact_indices = (meta->version >= 35);
	const int type_definition_size = compact_indices? meta->typeDefinitionIndexSize: 4;
	const ut64 entry = image_definition_entry_size (meta);
	ut8 *buf = read_metadata_table (meta, R2U_SEC_IMAGES, entry, count);
	if (!buf) {
		return NULL;
	}
	Il2CppImageDefinition *imgs = R_NEWS (Il2CppImageDefinition, *count);
	if (!imgs) {
		R_FREE (buf);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		imgs[i].nameIndex = read_u32p (&p);
		imgs[i].assemblyIndex = read_i32p (&p);
		imgs[i].typeStart = read_indexp (&p, type_definition_size);
		imgs[i].typeCount = read_u32p (&p);
		imgs[i].exportedTypeStart = read_indexp (&p, type_definition_size);
		imgs[i].exportedTypeCount = read_u32p (&p);
		imgs[i].entryPointIndex = read_i32p (&p);
		imgs[i].token = read_u32p (&p);
		imgs[i].customAttributeStart = read_i32p (&p);
		imgs[i].customAttributeCount = read_u32p (&p);
	}
	R_FREE (buf);
	return imgs;
}

R_API Il2CppAssemblyDefinition *r2unity_get_assemblies(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	Il2CppMetadataSection sec = meta->sections[R2U_SEC_ASSEMBLIES];
	ut64 asize = sec.size;
	if (!asize) {
		return NULL;
	}
	/* Probe the per-row stride by joining against the images table:
	 * there is one assembly per image on every wire version. Expected
	 * strides are 56, 60, 64 or 68 bytes (see doc/sbom.md §4.1). */
	size_t img_count = 0;
	Il2CppImageDefinition *imgs = r2unity_get_images (meta, &img_count);
	R_FREE (imgs);
	if (!img_count) {
		return NULL;
	}
	if (asize % img_count != 0) {
		R_LOG_WARN ("assembliesSize=%" PFMT64u " not divisible by image_count=%u",
			asize,
			(unsigned)img_count);
		return NULL;
	}
	const ut64 entry = asize / img_count;
	if (entry < 56 || entry > 80) {
		R_LOG_WARN ("implausible assembly entry stride %" PFMT64u, entry);
		return NULL;
	}
	/* Within a row:
	 *   [0..4)    imageIndex
	 *   [4..8)    token (wire >= 24.1) OR customAttributeIndex (wire <= 24.0)
	 *   [8..12)   moduleToken (wire >= 38 only)
	 *   [8/12..)  referencedAssemblyStart / Count
	 *   [16/20..entry) Il2CppAssemblyNameDefinition
	 * aname size is (entry - 16): 48 when hashValueIndex dropped (>=24.4),
	 * 52 when it is still present (<=24.3). Layout within aname:
	 *   nameIndex, cultureIndex, [hashValueIndex if 52], publicKeyIndex,
	 *   hash_alg, hash_len, flags, major, minor, build, revision,
	 *   public_key_token[8]. */
	const ut64 aname_offset = meta->version >= 38? 20: 16;
	const ut64 aname_size = entry - aname_offset;
	bool has_hash_value = (aname_size == 52);
	if (aname_size != 48 && aname_size != 52) {
		R_LOG_WARN ("unexpected aname size %" PFMT64u, aname_size);
		return NULL;
	}
	*count = img_count;
	Il2CppAssemblyDefinition *out = R_NEWS0 (Il2CppAssemblyDefinition, *count);
	if (!out) {
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, asize);
	if (!buf) {
		R_FREE (out);
		return NULL;
	}
	if (r_buf_read_at (meta->buf, sec.offset, buf, asize) != (st64)asize) {
		R_FREE (buf);
		R_FREE (out);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		out[i].image_index = (int32_t)r_read_le32 (p + 0);
		out[i].token = (meta->version >= 24)? r_read_le32 (p + 4): 0;
		if (meta->version >= 38) {
			out[i].referenced_start = (int32_t)r_read_le32 (p + 12);
			out[i].referenced_count = (int32_t)r_read_le32 (p + 16);
		} else {
			out[i].referenced_start = (int32_t)r_read_le32 (p + 8);
			out[i].referenced_count = (int32_t)r_read_le32 (p + 12);
		}
		const ut8 *an = p + aname_offset;
		out[i].aname.name_idx = r_read_le32 (an + 0);
		out[i].aname.culture_idx = r_read_le32 (an + 4);
		ut64 o;
		if (has_hash_value) {
			out[i].aname.hash_value_idx = r_read_le32 (an + 8);
			out[i].aname.public_key_idx = r_read_le32 (an + 12);
			o = 16;
		} else {
			out[i].aname.hash_value_idx = 0;
			out[i].aname.public_key_idx = r_read_le32 (an + 8);
			o = 12;
		}
		out[i].aname.hash_alg = r_read_le32 (an + o);
		o += 4;
		out[i].aname.hash_len = (int32_t)r_read_le32 (an + o);
		o += 4;
		out[i].aname.flags = r_read_le32 (an + o);
		o += 4;
		out[i].aname.major = (int32_t)r_read_le32 (an + o);
		o += 4;
		out[i].aname.minor = (int32_t)r_read_le32 (an + o);
		o += 4;
		out[i].aname.build = (int32_t)r_read_le32 (an + o);
		o += 4;
		out[i].aname.revision = (int32_t)r_read_le32 (an + o);
		o += 4;
		memcpy (out[i].aname.public_key_token, an + o, 8);
	}
	R_FREE (buf);
	return out;
}

R_API int32_t *r2unity_get_referenced_assemblies(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	const Il2CppMetadataSection sec = meta->sections[R2U_SEC_REFERENCED_ASSEMBLIES];
	if (sec.size < 4 || (sec.size % 4) != 0) {
		return NULL;
	}
	const size_t n = sec.size / 4;
	int32_t *out = R_NEWS (int32_t, n);
	if (!out) {
		return NULL;
	}
	if (r_buf_read_at (meta->buf, sec.offset, (ut8 *)out, sec.size) != (st64)sec.size) {
		R_FREE (out);
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		out[i] = (int32_t)r_read_le32 (out + i);
	}
	*count = n;
	return out;
}

/* Per-image sorted (token, method_idx) index used for scoped token lookups.
 * Method tokens are only unique within their owning image. */
typedef struct {
	uint32_t token;
	int32_t idx;
} R2UTokIdx;

static int cmp_tokidx(const void *a, const void *b) {
	uint32_t ta = ((const R2UTokIdx *)a)->token;
	uint32_t tb = ((const R2UTokIdx *)b)->token;
	return (ta < tb)? -1: (ta > tb);
}

/* Build a typeIndex -> imageIndex map once per enumeration pass. Returns NULL
 * if no images are known or the type table is empty. */
static int *build_type2img(const Il2CppImageDefinition *images, size_t image_count, size_t type_count) {
	if (!images || !type_count) {
		return NULL;
	}
	int *type2img = R_NEWS (int, type_count);
	if (!type2img) {
		return NULL;
	}
	for (size_t ti = 0; ti < type_count; ti++) {
		type2img[ti] = -1;
	}
	for (size_t ii = 0; ii < image_count; ii++) {
		int start = images[ii].typeStart;
		int tcount = (int)images[ii].typeCount;
		if (start < 0 || tcount <= 0) {
			continue;
		}
		for (int k = 0; k < tcount && (size_t) (start + k) < type_count; k++) {
			type2img[start + k] = (int)ii;
		}
	}
	return type2img;
}

/* Resolve the owning image (DLL) of a given method via its declaring type. */
static int image_index_for_method(const int *type2img, size_t type_count, const Il2CppMethodDefinition *m) {
	if (!type2img || m->declaringType < 0 || (size_t)m->declaringType >= type_count) {
		return -1;
	}
	return type2img[m->declaringType];
}

static char *build_qualified_name(R2UnityMetadata *meta,
	const Il2CppTypeDefinition *td,
	uint32_t method_name_idx) {
	char *ns = td? r2unity_get_string (meta, td->namespaceIndex): NULL;
	char *tn = td? r2unity_get_string (meta, td->nameIndex): NULL;
	char *mn = r2unity_get_string (meta, method_name_idx);
	if (!mn) {
		free (ns);
		free (tn);
		return NULL;
	}
	char *out;
	if (ns && *ns && tn && *tn) {
		out = r_str_newf ("%s.%s.%s", ns, tn, mn);
	} else if (tn && *tn) {
		out = r_str_newf ("%s.%s", tn, mn);
	} else {
		out = strdup (mn);
	}
	free (ns);
	free (tn);
	free (mn);
	return out;
}

/* Shared context for P/Invoke and reverse P/Invoke enumeration. */
typedef struct {
	R2UnityMetadata *meta;
	Il2CppMethodDefinition *methods;
	size_t method_count;
	Il2CppTypeDefinition *types;
	size_t type_count;
	Il2CppImageDefinition *images;
	size_t image_count;
	int *type2img;
} InteropCtx;

static bool interop_ctx_init(InteropCtx *c, R2UnityMetadata *meta) {
	memset (c, 0, sizeof (*c));
	c->meta = meta;
	c->methods = r2unity_get_method_definitions (meta, &c->method_count);
	if (!c->methods || !c->method_count) {
		return false;
	}
	c->types = r2unity_get_type_definitions (meta, &c->type_count);
	c->images = r2unity_get_images (meta, &c->image_count);
	c->type2img = build_type2img (c->images, c->image_count, c->type_count);
	return true;
}

static void interop_ctx_fini(InteropCtx *c) {
	R_FREE (c->methods);
	R_FREE (c->types);
	R_FREE (c->images);
	R_FREE (c->type2img);
}

static void interop_fill(R2UnityInterop *it, const InteropCtx *c, int32_t method_index, uint8_t kind) {
	const Il2CppMethodDefinition *m = &c->methods[method_index];
	it->kind = kind;
	it->method_index = method_index;
	it->token = m->token;
	it->flags = m->flags;
	it->iflags = m->iflags;
	it->wrapper_va = 0;
	it->wrapper_index = UINT32_MAX;
	it->confidence = 100;

	const Il2CppTypeDefinition *td = NULL;
	if (c->types && m->declaringType >= 0 && (size_t)m->declaringType < c->type_count) {
		td = &c->types[m->declaringType];
	}
	it->name = build_qualified_name (c->meta, td, m->nameIndex);

	int img_idx = image_index_for_method (c->type2img, c->type_count, m);
	it->image_index = img_idx;
	if (img_idx >= 0 && c->images && (size_t)img_idx < c->image_count) {
		it->image_name = r2unity_get_string (c->meta, c->images[img_idx].nameIndex);
	}
}

R_API R2UnityInterop *r2unity_enumerate_pinvokes(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	InteropCtx c;
	if (!interop_ctx_init (&c, meta)) {
		interop_ctx_fini (&c);
		return NULL;
	}
	size_t n = 0;
	for (size_t j = 0; j < c.method_count; j++) {
		if (c.methods[j].flags & IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL) {
			n++;
		}
	}
	R2UnityInterop *out = n? R_NEWS0 (R2UnityInterop, n): NULL;
	if (out) {
		size_t k = 0;
		for (size_t j = 0; j < c.method_count && k < n; j++) {
			if (c.methods[j].flags & IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL) {
				/* DLL name / entry-point recovery requires attribute-BLOB
				 * decoding (v29+) or generator disassembly (v<=27.2);
				 * not implemented yet. */
				interop_fill (&out[k++], &c, (int32_t)j, R2U_INTEROP_PINVOKE);
			}
		}
		*count = n;
	}
	interop_ctx_fini (&c);
	return out;
}

/* v29+ attribute data range: { uint32 token, uint32 startOffset }. The end of
 * an entry's BLOB is the next entry's startOffset, or attributeDataSize for
 * the last entry. See doc/pinvoke.md §4.1 and Il2CppDumper Metadata.cs:131. */
typedef struct {
	uint32_t token;
	uint32_t start_offset;
} R2UAttrRange;

static R2UAttrRange *load_attribute_ranges(R2UnityMetadata *meta, size_t *count, ut64 *out_data_off, ut64 *out_data_size) {
	*count = 0;
	*out_data_off = 0;
	*out_data_size = 0;
	if (meta->version < 29) {
		return NULL;
	}
	Il2CppMetadataSection data = meta->sections[R2U_SEC_ATTRIBUTE_DATA];
	Il2CppMetadataSection range = meta->sections[R2U_SEC_ATTRIBUTE_DATA_RANGES];
	ut64 range_size = range.size;
	if (!range_size || (range_size % 8) != 0) {
		return NULL;
	}
	size_t n = range_size / 8;
	R2UAttrRange *ranges = R_NEWS (R2UAttrRange, n);
	if (!ranges) {
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, range_size);
	if (!buf) {
		R_FREE (ranges);
		return NULL;
	}
	if (r_buf_read_at (meta->buf, range.offset, buf, range_size) != (st64)range_size) {
		R_FREE (buf);
		R_FREE (ranges);
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		ranges[i].token = r_read_le32 (buf + i * 8 + 0);
		ranges[i].start_offset = r_read_le32 (buf + i * 8 + 4);
	}
	R_FREE (buf);
	*count = n;
	*out_data_off = data.offset;
	*out_data_size = data.size;
	return ranges;
}

/* ECMA-335 §II.23.2 compressed uint32. Returns 0 on malformed input. */
static uint32_t read_compressed_uint(const ut8 *buf, size_t bufsize, size_t *pos) {
	if (*pos >= bufsize) {
		return 0;
	}
	ut8 b0 = buf[*pos];
	if ((b0 & 0x80) == 0) {
		*pos += 1;
		return b0;
	}
	if ((b0 & 0xC0) == 0x80) {
		if (*pos + 2 > bufsize) {
			return 0;
		}
		uint32_t v = ((uint32_t) (b0 & 0x3F) << 8) | buf[*pos + 1];
		*pos += 2;
		return v;
	}
	if (*pos + 4 > bufsize) {
		return 0;
	}
	uint32_t v = ((uint32_t) (b0 & 0x1F) << 24) | ((uint32_t)buf[*pos + 1] << 16) | ((uint32_t)buf[*pos + 2] << 8) | buf[*pos + 3];
	*pos += 4;
	return v;
}

/* For attribute entry at `range_idx` (v29+), read the ctor method indices
 *(first 4 bytes after count header) and check if any ctor's declaring type
 * matches the target attribute type name.
 *
 * Returns the matched attribute kind (R2U_INTEROP_REVERSE_PINVOKE or
 * R2U_INTEROP_UNMANAGED_ONLY), or 0 if no match. */
static uint8_t attribute_range_interop_kind(
	R2UnityMetadata *meta,
	const R2UAttrRange *ranges,
	size_t nranges,
	size_t range_idx,
	ut64 attr_data_off,
	ut64 attr_data_size,
	const Il2CppMethodDefinition *methods,
	size_t nmethods,
	const Il2CppTypeDefinition *types,
	size_t ntypes) {
	if (range_idx >= nranges) {
		return 0;
	}
	ut64 blob_start = ranges[range_idx].start_offset;
	ut64 blob_end = (range_idx + 1 < nranges)
		? ranges[range_idx + 1].start_offset
		: attr_data_size;
	if (blob_end <= blob_start || blob_end > attr_data_size) {
		return 0;
	}
	ut64 blob_len = blob_end - blob_start;
	if (blob_len > 0x10000) {
		return 0;
	}
	ut8 *blob = R_NEWS (ut8, (size_t)blob_len);
	if (!blob) {
		return 0;
	}
	if (r_buf_read_at (meta->buf, attr_data_off + blob_start, blob, blob_len) != (st64)blob_len) {
		R_FREE (blob);
		return 0;
	}
	size_t pos = 0;
	uint32_t n_attrs = read_compressed_uint (blob, blob_len, &pos);
	if (n_attrs == 0 || (ut64)pos + (ut64)n_attrs * 4 > blob_len) {
		R_FREE (blob);
		return 0;
	}
	uint8_t kind = 0;
	for (uint32_t i = 0; i < n_attrs; i++) {
		uint32_t ctor_idx = r_read_le32 (blob + pos + i * 4);
		if (ctor_idx >= nmethods) {
			continue;
		}
		int32_t declaring = methods[ctor_idx].declaringType;
		if (declaring < 0 || (size_t)declaring >= ntypes) {
			continue;
		}
		const char *name = r2unity_get_string (meta, types[declaring].nameIndex);
		if (!name) {
			continue;
		}
		if (!strcmp (name, "MonoPInvokeCallbackAttribute")) {
			kind = R2U_INTEROP_REVERSE_PINVOKE;
			free ((void *)name);
			break;
		}
		if (!strcmp (name, "UnmanagedCallersOnlyAttribute")) {
			kind = R2U_INTEROP_UNMANAGED_ONLY;
			free ((void *)name);
			break;
		}
		free ((void *)name);
	}
	R_FREE (blob);
	return kind;
}

/* Per-image sorted (token, method_idx) index for scoped binary search. */
typedef struct {
	int32_t *start;
	int32_t *count;
	R2UTokIdx *map;
} TokIndex;

static void tokindex_free(TokIndex *t) {
	R_FREE (t->start);
	R_FREE (t->count);
	R_FREE (t->map);
}

static bool tokindex_build(TokIndex *t, const InteropCtx *c) {
	memset (t, 0, sizeof (*t));
	if (!c->type2img || !c->image_count) {
		return false;
	}
	t->start = R_NEWS0 (int32_t, c->image_count);
	t->count = R_NEWS0 (int32_t, c->image_count);
	t->map = R_NEWS (R2UTokIdx, c->method_count);
	if (!t->start || !t->count || !t->map) {
		tokindex_free (t);
		return false;
	}
	for (size_t j = 0; j < c->method_count; j++) {
		int ii = image_index_for_method (c->type2img, c->type_count, &c->methods[j]);
		if (ii < 0 || (size_t)ii >= c->image_count || !c->methods[j].token) {
			continue;
		}
		t->count[ii]++;
	}
	int32_t acc = 0;
	for (size_t ii = 0; ii < c->image_count; ii++) {
		t->start[ii] = acc;
		acc += t->count[ii];
		t->count[ii] = 0;
	}
	for (size_t j = 0; j < c->method_count; j++) {
		int ii = image_index_for_method (c->type2img, c->type_count, &c->methods[j]);
		if (ii < 0 || (size_t)ii >= c->image_count || !c->methods[j].token) {
			continue;
		}
		int32_t pos = t->start[ii] + t->count[ii]++;
		t->map[pos].token = c->methods[j].token;
		t->map[pos].idx = (int32_t)j;
	}
	for (size_t ii = 0; ii < c->image_count; ii++) {
		qsort (t->map + t->start[ii], (size_t)t->count[ii], sizeof (R2UTokIdx), cmp_tokidx);
	}
	return true;
}

static int32_t tokindex_lookup(const TokIndex *t, size_t image_idx, uint32_t token) {
	if (!t->map) {
		return -1;
	}
	size_t lo = (size_t)t->start[image_idx];
	size_t hi = lo + (size_t)t->count[image_idx];
	while (lo < hi) {
		size_t mid = (lo + hi) >> 1;
		if (t->map[mid].token < token) {
			lo = mid + 1;
		} else if (t->map[mid].token > token) {
			hi = mid;
		} else {
			return t->map[mid].idx;
		}
	}
	return -1;
}

R_API R2UnityInterop *r2unity_enumerate_reverse_pinvokes(R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	if (meta->version < 29) {
		/* Pre-v29: attribute args live in native generator stubs; see doc/pinvoke.md §4.2. */
		return NULL;
	}
	InteropCtx c;
	if (!interop_ctx_init (&c, meta)) {
		interop_ctx_fini (&c);
		return NULL;
	}
	ut64 attr_data_off = 0, attr_data_size = 0;
	size_t nranges = 0;
	R2UAttrRange *ranges = load_attribute_ranges (meta, &nranges, &attr_data_off, &attr_data_size);
	TokIndex tok = {0};
	R2UnityInterop *out = NULL;
	size_t n = 0;
	if (!ranges || !nranges) {
		goto done;
	}
	tokindex_build (&tok, &c);

	size_t cap = 32;
	out = R_NEWS0 (R2UnityInterop, cap);
	if (!out) {
		goto done;
	}
	for (size_t ii = 0; ii < c.image_count; ii++) {
		int32_t r_start = c.images[ii].customAttributeStart;
		int32_t r_end = r_start + (int32_t)c.images[ii].customAttributeCount;
		if (r_start < 0 || r_end < 0 || (size_t)r_end > nranges) {
			continue;
		}
		for (int32_t ri = r_start; ri < r_end; ri++) {
			uint32_t token = ranges[ri].token;
			if ((token >> 24) != 0x06) {
				continue;
			}
			uint8_t kind = attribute_range_interop_kind (meta, ranges, nranges, (size_t)ri, attr_data_off, attr_data_size, c.methods, c.method_count, c.types, c.type_count);
			if (!kind) {
				continue;
			}
			int32_t mi = tokindex_lookup (&tok, ii, token);
			if (mi < 0) {
				continue;
			}
			if (n == cap) {
				size_t ncap = cap * 2;
				R2UnityInterop *noo = realloc (out, ncap * sizeof (R2UnityInterop));
				if (!noo) {
					break;
				}
				memset (noo + cap, 0, (ncap - cap) * sizeof (R2UnityInterop));
				out = noo;
				cap = ncap;
			}
			interop_fill (&out[n++], &c, mi, kind);
		}
	}
	if (!n) {
		R_FREE (out);
	}
done:
	R_FREE (ranges);
	tokindex_free (&tok);
	interop_ctx_fini (&c);
	*count = n;
	return out;
}

R_API void r2unity_free_interop(R2UnityInterop *items, size_t count) {
	if (!items) {
		return;
	}
	for (size_t i = 0; i < count; i++) {
		free (items[i].name);
		free (items[i].image_name);
		free (items[i].dll_name);
		free (items[i].entry_name);
	}
	free (items);
}

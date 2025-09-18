#include "lib.h"
#include <r_util.h>
#include <r_util/r_buf.h>

R_API R2UnityMetadata *r2unity_parse_metadata (RBuffer *buf) {
	if (!buf) {
		return NULL;
	}
	R2UnityMetadata *meta = R_NEW0 (R2UnityMetadata);
	if (!meta) {
		return NULL;
	}
	r_buf_seek (buf, 0, R_BUF_SET);
	uint32_t magic = r_buf_read_le32 (buf);
	if (magic != IL2CPP_MAGIC) {
		R_FREE (meta);
		return NULL;
	}
	r_buf_seek (buf, 4, R_BUF_SET);
	size_t header_size = 0;
	// First read a small part to get the version
	uint32_t sanity;
	int32_t version;
	if (r_buf_read (buf, (ut8 *)&sanity, 4) != 4) {
		R_FREE (meta);
		return NULL;
	}
	if (r_buf_read (buf, (ut8 *)&version, 4) != 4) {
		R_FREE (meta);
		return NULL;
	}
	if (sanity != IL2CPP_MAGIC) {
		R_FREE (meta);
		return NULL;
	}
	meta->version = version;
	switch (version) {
	case 24:
		header_size = sizeof (Il2CppGlobalMetadataHeader_v24) - 8; // subtract sanity and version already read
		break;
	case 27:
		header_size = sizeof (Il2CppGlobalMetadataHeader_v27) - 8;
		break;
	case 29:
		header_size = sizeof (Il2CppGlobalMetadataHeader_v29) - 8;
		break;
	default:
		R_FREE (meta);
		return NULL;
	}
	if (r_buf_read (buf, (ut8 *)&meta->header.v24.stringLiteralOffset, header_size) != (st64)header_size) {
		R_FREE (meta);
		return NULL;
	}
	meta->strings = r_buf_new_slice (buf, meta->header.v24.stringOffset, meta->header.v24.stringSize);
	meta->string_literals = r_buf_new_slice (buf, meta->header.v24.stringLiteralOffset, meta->header.v24.stringLiteralSize);
	return meta;
}

R_API void r2unity_free_metadata (R2UnityMetadata *meta) {
	if (!meta) {
		return;
	}
	r_buf_free (meta->strings);
	r_buf_free (meta->string_literals);
	R_FREE (meta);
}

R_API const char *r2unity_get_string (R2UnityMetadata *meta, uint32_t index) {
	if (!meta || !meta->strings || index >= r_buf_size (meta->strings)) {
		return NULL;
	}
	// Read until null terminator
	char *str = NULL;
	ut64 len = 0;
	ut8 byte;
	while (r_buf_read_at (meta->strings, index + len, &byte, 1) == 1 && byte != 0) {
		len++;
	}
	if (len > 0) {
		str = R_NEWS (char, len + 1);
		if (str) {
			r_buf_read_at (meta->strings, index, (ut8 *)str, len);
			str[len] = 0;
		}
	}
	return str;
}

R_API Il2CppTypeDefinition *r2unity_get_type_definitions (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	*count = meta->header.v24.typeDefinitionsSize / sizeof (Il2CppTypeDefinition);
	if (*count == 0) {
		return NULL;
	}
	Il2CppTypeDefinition *types = R_NEWS (Il2CppTypeDefinition, *count);
	if (!types) {
		return NULL;
	}
	r_buf_seek (meta->strings, meta->header.v24.typeDefinitionsOffset, R_BUF_SET);
	if (r_buf_read (meta->strings, (ut8 *)types, meta->header.v24.typeDefinitionsSize) != meta->header.v24.typeDefinitionsSize) {
		R_FREE (types);
		return NULL;
	}
	return types;
}

R_API Il2CppMethodDefinition *r2unity_get_method_definitions (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	*count = meta->header.v24.methodsSize / sizeof (Il2CppMethodDefinition);
	if (*count == 0) {
		return NULL;
	}
	Il2CppMethodDefinition *methods = R_NEWS (Il2CppMethodDefinition, *count);
	if (!methods) {
		return NULL;
	}
	r_buf_seek (meta->strings, meta->header.v24.methodsOffset, R_BUF_SET);
	if (r_buf_read (meta->strings, (ut8 *)methods, meta->header.v24.methodsSize) != meta->header.v24.methodsSize) {
		R_FREE (methods);
		return NULL;
	}
	return methods;
}

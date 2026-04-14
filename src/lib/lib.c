#include "lib.h"
#include <r_util.h>
#include <r_util/r_buf.h>
#undef RD_LE32
static inline ut32 RD_LE32 (const ut8 *p) {
	return ((ut32)p[0]) | ((ut32)p[1] << 8) | ((ut32)p[2] << 16) | ((ut32)p[3] << 24);
}
static inline ut16 RD_LE16 (const ut8 *p) {
	return (ut16)((ut16)p[0] | ((ut16)p[1] << 8));
}
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

static bool g_debug = false;

R_API void r2unity_set_debug (bool v) {
	g_debug = v;
}

R_API bool r2unity_is_debug (void) {
	return g_debug;
}


R_API R2UnityMetadata *r2unity_parse_metadata (RBuffer *buf) {
	if (!buf) {
		return NULL;
	}
	R2UnityMetadata *meta = R_NEW0 (R2UnityMetadata);
	if (!meta) {
		return NULL;
	}
	// read sanity and version first
	uint32_t sanity = 0;
	int32_t version = 0;
	if (r_buf_read_at (buf, 0, (ut8 *)&sanity, 4) != 4) {
		R_FREE (meta);
		return NULL;
	}
	if (sanity != IL2CPP_MAGIC) {
		R_FREE (meta);
		return NULL;
	}
	if (r_buf_read_at (buf, 4, (ut8 *)&version, 4) != 4) {
		R_FREE (meta);
		return NULL;
	}
	meta->version = version;
	size_t header_size = 0;
	if (version < 24 || version > 31) {
		R_FREE (meta);
		return NULL;
	}
	if (version < 27) {
		header_size = sizeof (Il2CppGlobalMetadataHeader_v24);
	} else if (version < 29) {
		header_size = sizeof (Il2CppGlobalMetadataHeader_v27);
	} else {
		// versions 29 and above (e.g. 30, 31) are compatible for the fields we use
		header_size = sizeof (Il2CppGlobalMetadataHeader_v29);
	}
	// read full header for selected version
	if (r_buf_read_at (buf, 0, (ut8 *)&meta->header.v24, header_size) != (st64)header_size) {
		R_FREE (meta);
		return NULL;
	}
	meta->buf = buf;
	/* Normalize header fields depending on version to simplify later access */
	if (meta->version < 27) {
		meta->stringOffset = meta->header.v24.stringOffset;
		meta->stringSize = meta->header.v24.stringSize;
		meta->stringLiteralOffset = meta->header.v24.stringLiteralOffset;
		meta->stringLiteralSize = meta->header.v24.stringLiteralSize;
		meta->methodsOffset = meta->header.v24.methodsOffset;
		meta->methodsSize = meta->header.v24.methodsSize;
		meta->typeDefinitionsOffset = meta->header.v24.typeDefinitionsOffset;
		meta->typeDefinitionsSize = meta->header.v24.typeDefinitionsSize;
	} else if (meta->version < 29) {
		meta->stringOffset = meta->header.v27.stringOffset;
		meta->stringSize = meta->header.v27.stringSize;
		meta->stringLiteralOffset = meta->header.v27.stringLiteralOffset;
		meta->stringLiteralSize = meta->header.v27.stringLiteralSize;
		meta->methodsOffset = meta->header.v27.methodsOffset;
		meta->methodsSize = meta->header.v27.methodsSize;
		meta->typeDefinitionsOffset = meta->header.v27.typeDefinitionsOffset;
		meta->typeDefinitionsSize = meta->header.v27.typeDefinitionsSize;
	} else {
		meta->stringOffset = meta->header.v29.stringOffset;
		meta->stringSize = meta->header.v29.stringSize;
		meta->stringLiteralOffset = meta->header.v29.stringLiteralOffset;
		meta->stringLiteralSize = meta->header.v29.stringLiteralSize;
		meta->methodsOffset = meta->header.v29.methodsOffset;
		meta->methodsSize = meta->header.v29.methodsSize;
		meta->typeDefinitionsOffset = meta->header.v29.typeDefinitionsOffset;
		meta->typeDefinitionsSize = meta->header.v29.typeDefinitionsSize;
	}
	meta->strings = r_buf_new_slice (buf, meta->stringOffset, meta->stringSize);
	meta->string_literals = r_buf_new_slice (buf, meta->stringLiteralOffset, meta->stringLiteralSize);
	if (g_debug) {
		fprintf (stderr, "[r2unity] meta version=%d strings=%u@0x%x methods=%d@0x%x types=%d@0x%x\n",
			meta->version,
			(unsigned) meta->stringSize, (unsigned) meta->stringOffset,
			(int) meta->methodsSize, (unsigned) meta->methodsOffset,
			(int) meta->typeDefinitionsSize, (unsigned) meta->typeDefinitionsOffset);
	}

	return meta;
}

R_API void r2unity_free_metadata (R2UnityMetadata *meta) {
	if (!meta) {
		return;
	}
	r_unref (meta->strings);
	r_unref (meta->string_literals);
	R_FREE (meta);
}

R_API const char *r2unity_get_string (R2UnityMetadata *meta, uint32_t index) {
	if (!meta || !meta->strings || index >= r_buf_size (meta->strings)) {
		return NULL;
	}
	// Align index to the beginning of the string in case it points mid-string
	ut8 bprev = 0;
	ut64 idx = index;
	while (idx > 0) {
		if (r_buf_read_at (meta->strings, idx - 1, &bprev, 1) != 1) break;
		if (bprev == 0) break;
		idx--;
	}
	// Read until null terminator
	char *str = NULL;
	ut64 len = 0;
	ut8 byte;
	while (r_buf_read_at (meta->strings, idx + len, &byte, 1) == 1 && byte != 0) {
		len++;
	}
	if (len > 0) {
		str = R_NEWS (char, len + 1);
		if (str) {
			r_buf_read_at (meta->strings, idx, (ut8 *)str, len);
			str[len] = 0;
		}
	}
	return str;
}

R_API Il2CppTypeDefinition *r2unity_get_type_definitions (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	// On-disk Il2CppTypeDefinition is 88 bytes for v24+ (little-endian)
	const ut64 entry = 88;
	ut64 tsize = (ut64) (ut64) meta->typeDefinitionsSize;
	if (tsize < entry) {
		*count = 0;
		return NULL;
	}
	*count = (size_t) (tsize / entry);
	Il2CppTypeDefinition *types = R_NEWS (Il2CppTypeDefinition, *count);
	if (!types) {
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, tsize);
	if (!buf) {
		R_FREE (types);
		return NULL;
	}
	if (r_buf_read_at (meta->buf, meta->typeDefinitionsOffset, buf, tsize) != (st64) tsize) {
		R_FREE (buf);
		R_FREE (types);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		types[i].nameIndex = RD_LE32 (p + 0);
		types[i].namespaceIndex = RD_LE32 (p + 4);
		types[i].byvalTypeIndex = (int32_t) RD_LE32 (p + 8);
		types[i].declaringTypeIndex = (int32_t) RD_LE32 (p + 12);
		types[i].parentIndex = (int32_t) RD_LE32 (p + 16);
		types[i].elementTypeIndex = (int32_t) RD_LE32 (p + 20);
		types[i].genericContainerIndex = (int32_t) RD_LE32 (p + 24);
		types[i].flags = RD_LE32 (p + 28);
		types[i].fieldStart = (int32_t) RD_LE32 (p + 32);
		types[i].methodStart = (int32_t) RD_LE32 (p + 36);
		types[i].eventStart = (int32_t) RD_LE32 (p + 40);
		types[i].propertyStart = (int32_t) RD_LE32 (p + 44);
		types[i].nestedTypesStart = (int32_t) RD_LE32 (p + 48);
		types[i].interfacesStart = (int32_t) RD_LE32 (p + 52);
		types[i].vtableStart = (int32_t) RD_LE32 (p + 56);
		types[i].interfaceOffsetsStart = (int32_t) RD_LE32 (p + 60);
		types[i].method_count = RD_LE16 (p + 64);
		types[i].property_count = RD_LE16 (p + 66);
		types[i].field_count = RD_LE16 (p + 68);
		types[i].event_count = RD_LE16 (p + 70);
		types[i].nested_type_count = RD_LE16 (p + 72);
		types[i].vtable_count = RD_LE16 (p + 74);
		types[i].interfaces_count = RD_LE16 (p + 76);
		types[i].interface_offsets_count = RD_LE16 (p + 78);
		types[i].bitfield = RD_LE32 (p + 80);
		types[i].token = RD_LE32 (p + 84);
	}
	R_FREE (buf);
	return types;
}

R_API Il2CppMethodDefinition *r2unity_get_method_definitions (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	// On-disk Il2CppMethodDefinition is 32 bytes (little-endian)
	const ut64 entry = 32;
	if ((ut64) (ut64) meta->methodsSize < entry) {
		*count = 0;
		return NULL;
	}
	*count = (ut64) meta->methodsSize / entry;
	Il2CppMethodDefinition *methods = R_NEWS (Il2CppMethodDefinition, *count);
	if (!methods) {
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, (ut64) meta->methodsSize);
	if (!buf) {
		R_FREE (methods);
		return NULL;
	}
	if (r_buf_read_at (meta->buf, meta->methodsOffset, buf, (ut64) meta->methodsSize) != (st64) meta->methodsSize) {
		R_FREE (buf);
		R_FREE (methods);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		methods[i].nameIndex = RD_LE32 (p + 0);
		methods[i].declaringType = (int32_t) RD_LE32 (p + 4);
		methods[i].returnType = (int32_t) RD_LE32 (p + 8);
		methods[i].parameterStart = (int32_t) RD_LE32 (p + 12);
		methods[i].genericContainerIndex = (int32_t) RD_LE32 (p + 16);
		methods[i].token = RD_LE32 (p + 20);
		methods[i].flags = RD_LE16 (p + 24);
		methods[i].iflags = RD_LE16 (p + 26);
		methods[i].slot = RD_LE16 (p + 28);
		methods[i].parameterCount = RD_LE16 (p + 30);
	}
	R_FREE (buf);
	return methods;
}

R_API Il2CppImageDefinition *r2unity_get_images (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	*count = 0;
	ut64 ioff = 0;
	ut64 isize = 0;
	if (meta->version < 27) {
		off_t o = meta->header.v24.imagesOffset;
		ioff = (ut64) o;
		isize = (ut64) meta->header.v24.imagesSize;
	} else if (meta->version < 29) {
		off_t o = meta->header.v27.imagesOffset;
		ioff = (ut64) o;
		isize = (ut64) meta->header.v27.imagesSize;
	} else {
		off_t o = meta->header.v29.imagesOffset;
		ioff = (ut64) o;
		isize = (ut64) meta->header.v29.imagesSize;
	}
	if (!isize) {
		return NULL;
	}
	// We parse a minimal 40-byte structure per entry
	const ut64 entry = 40;
	*count = (size_t) (isize / entry);
	if (!*count) return NULL;
	Il2CppImageDefinition *imgs = R_NEWS (Il2CppImageDefinition, *count);
	if (!imgs) return NULL;
	ut8 *buf2 = R_NEWS (ut8, isize);
	if (!buf2) {
		R_FREE (imgs);
		return NULL;
	}
	if (r_buf_read_at (meta->buf, ioff, buf2, isize) != (st64) isize) {
		R_FREE (buf2);
		R_FREE (imgs);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf2 + i * entry;
		imgs[i].nameIndex = RD_LE32 (p + 0);
		imgs[i].assemblyIndex = (int32_t) RD_LE32 (p + 4);
		imgs[i].typeStart = (int32_t) RD_LE32 (p + 8);
		imgs[i].typeCount = RD_LE32 (p + 12);
		imgs[i].exportedTypeStart = (int32_t) RD_LE32 (p + 16);
		imgs[i].exportedTypeCount = RD_LE32 (p + 20);
		imgs[i].entryPointIndex = (int32_t) RD_LE32 (p + 24);
		imgs[i].token = RD_LE32 (p + 28);
		imgs[i].customAttributeStart = (int32_t) RD_LE32 (p + 32);
		imgs[i].customAttributeCount = RD_LE32 (p + 36);
	}
	R_FREE (buf2);
	return imgs;
}

R_API bool r2unity_read_method_pointers_at (R2UnityMetadata *meta, const char *exe_path, ut64 addr, size_t count, ut64 **out_ptrs) {
	(void) meta; (void) exe_path; (void) addr; (void) count; (void) out_ptrs;
	return false;
}

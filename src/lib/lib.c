#define R_LOG_ORIGIN "r2unity.lib"

#include "lib.h"
#include <r_util.h>

static bool g_debug = false;

R_API void r2unity_set_debug (bool v) {
	g_debug = v;
}

R_API bool r2unity_is_debug (void) {
	return g_debug;
}


/* Detect v24.0 vs v24.1+ (both land on disk as version==24).
 *
 * At v24.1 the ImageDefinition row grew by 8 bytes (added
 * customAttributeStart/Count), so imagesSize is a multiple of 40 on v24.1+
 * and 32 on v24.0. When imagesSize is divisible by both (a multiple of 160),
 * probe the 4 bytes at offset 40: under v24.1+ that is row 1's nameIndex
 * (a valid string-pool offset); under v24.0 it is mid-row.
 *
 * Fixing row layouts for v24.0 (larger TypeDefinition, MethodDefinition,
 * ImageDefinition) is tracked separately; reject for now so decoders don't
 * silently return wrong offsets. */
static bool is_v24_0 (R2UnityMetadata *meta) {
	ut64 ioff = meta->header.v24.imagesOffset;
	ut64 isize = meta->header.v24.imagesSize;
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
			if (name_idx < (uint32_t) meta->stringSize) {
				return false;
			}
		}
	}
	return true;
}

R_API R2UnityMetadata *r2unity_parse_metadata (RBuffer *buf) {
	R_RETURN_VAL_IF_FAIL (buf, NULL);
	ut8 preamble[8];
	if (r_buf_read_at (buf, 0, preamble, sizeof (preamble)) != (st64) sizeof (preamble)) {
		return NULL;
	}
	if (r_read_le32 (preamble) != IL2CPP_MAGIC) {
		return NULL;
	}
	int32_t version = (int32_t) r_read_le32 (preamble + 4);
	if (version < 24 || version > 31) {
		return NULL;
	}
	size_t header_size;
	if (version < 27) {
		header_size = sizeof (Il2CppGlobalMetadataHeader_v24);
	} else if (version < 29) {
		header_size = sizeof (Il2CppGlobalMetadataHeader_v27);
	} else {
		/* v29+ shares layout (v30, v31 reuse these fields). */
		header_size = sizeof (Il2CppGlobalMetadataHeader_v29);
	}
	R2UnityMetadata *meta = R_NEW0 (R2UnityMetadata);
	meta->version = version;
	meta->buf = buf;
	if (r_buf_read_at (buf, 0, (ut8 *) &meta->header.v24, header_size) != (st64) header_size) {
		R_FREE (meta);
		return NULL;
	}
	/* All Il2Cpp header fields are 32-bit words; sweep in-place for BE hosts
	 * (no-op on LE). This also normalises the fields accessed below. */
	uint32_t *hdr_words = (uint32_t *) &meta->header;
	size_t nfields = header_size / sizeof (uint32_t);
	for (size_t i = 0; i < nfields; i++) {
		hdr_words[i] = r_read_le32 (&hdr_words[i]);
	}
	/* v24/v27/v29 share identical layout for fields up to exportedTypeDefinitions,
	 * so accessing them via the v24 view is valid for every supported version. */
	const Il2CppGlobalMetadataHeader_v24 *h = &meta->header.v24;
	meta->stringOffset = h->stringOffset;
	meta->stringSize = h->stringSize;
	meta->stringLiteralOffset = h->stringLiteralOffset;
	meta->stringLiteralSize = h->stringLiteralSize;
	meta->stringLiteralDataOffset = h->stringLiteralDataOffset;
	meta->stringLiteralDataSize = h->stringLiteralDataSize;
	meta->methodsOffset = h->methodsOffset;
	meta->methodsSize = h->methodsSize;
	meta->typeDefinitionsOffset = h->typeDefinitionsOffset;
	meta->typeDefinitionsSize = h->typeDefinitionsSize;
	if (meta->version == 24 && is_v24_0 (meta)) {
		R_LOG_ERROR ("v24.0 metadata (Unity 5.6..2018.2) not supported: "
			"TypeDefinition/MethodDefinition/ImageDefinition row layouts "
			"differ from v24.1+ and would silently decode wrong offsets");
		r2unity_free_metadata (meta);
		return NULL;
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
	R_RETURN_VAL_IF_FAIL (meta, NULL);
	if (!meta->strings || index >= r_buf_size (meta->strings)) {
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

R_API Il2CppStringLiteral *r2unity_get_string_literals (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	if (!meta->string_literals || meta->stringLiteralSize < 8 || (meta->stringLiteralSize % 8) != 0) {
		return NULL;
	}
	*count = (size_t) ((ut64) meta->stringLiteralSize / 8);
	Il2CppStringLiteral *lits = R_NEWS (Il2CppStringLiteral, *count);
	if (!lits) {
		*count = 0;
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, (ut64) meta->stringLiteralSize);
	if (!buf) {
		R_FREE (lits);
		*count = 0;
		return NULL;
	}
	if (r_buf_read_at (meta->string_literals, 0, buf, (ut64) meta->stringLiteralSize) != (st64) meta->stringLiteralSize) {
		R_FREE (buf);
		R_FREE (lits);
		*count = 0;
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * 8;
		lits[i].length = r_read_le32 (p + 0);
		lits[i].dataIndex = r_read_le32 (p + 4);
	}
	R_FREE (buf);
	return lits;
}

R_API bool r2unity_read_string_literal (R2UnityMetadata *meta, const Il2CppStringLiteral *lit, ut8 **out_bytes, size_t *out_len) {
	R_RETURN_VAL_IF_FAIL (meta && lit && out_bytes && out_len, false);
	*out_bytes = NULL;
	*out_len = 0;
	if (lit->length == 0) {
		return true;
	}
	ut64 end = (ut64) lit->dataIndex + (ut64) lit->length;
	if (end > (ut64) meta->stringLiteralDataSize) {
		return false;
	}
	ut8 *bytes = R_NEWS (ut8, lit->length);
	if (!bytes) {
		return false;
	}
	if (r_buf_read_at (meta->buf, (ut64) meta->stringLiteralDataOffset + lit->dataIndex, bytes, lit->length) != (st64) lit->length) {
		R_FREE (bytes);
		return false;
	}
	*out_bytes = bytes;
	*out_len = lit->length;
	return true;
}

R_API Il2CppTypeDefinition *r2unity_get_type_definitions (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
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
		types[i].nameIndex = r_read_le32 (p + 0);
		types[i].namespaceIndex = r_read_le32 (p + 4);
		types[i].byvalTypeIndex = (int32_t) r_read_le32 (p + 8);
		types[i].declaringTypeIndex = (int32_t) r_read_le32 (p + 12);
		types[i].parentIndex = (int32_t) r_read_le32 (p + 16);
		types[i].elementTypeIndex = (int32_t) r_read_le32 (p + 20);
		types[i].genericContainerIndex = (int32_t) r_read_le32 (p + 24);
		types[i].flags = r_read_le32 (p + 28);
		types[i].fieldStart = (int32_t) r_read_le32 (p + 32);
		types[i].methodStart = (int32_t) r_read_le32 (p + 36);
		types[i].eventStart = (int32_t) r_read_le32 (p + 40);
		types[i].propertyStart = (int32_t) r_read_le32 (p + 44);
		types[i].nestedTypesStart = (int32_t) r_read_le32 (p + 48);
		types[i].interfacesStart = (int32_t) r_read_le32 (p + 52);
		types[i].vtableStart = (int32_t) r_read_le32 (p + 56);
		types[i].interfaceOffsetsStart = (int32_t) r_read_le32 (p + 60);
		types[i].method_count = r_read_le16 (p + 64);
		types[i].property_count = r_read_le16 (p + 66);
		types[i].field_count = r_read_le16 (p + 68);
		types[i].event_count = r_read_le16 (p + 70);
		types[i].nested_type_count = r_read_le16 (p + 72);
		types[i].vtable_count = r_read_le16 (p + 74);
		types[i].interfaces_count = r_read_le16 (p + 76);
		types[i].interface_offsets_count = r_read_le16 (p + 78);
		types[i].bitfield = r_read_le32 (p + 80);
		types[i].token = r_read_le32 (p + 84);
	}
	R_FREE (buf);
	return types;
}

R_API Il2CppMethodDefinition *r2unity_get_method_definitions (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	/* On-disk Il2CppMethodDefinition:
	 *   v24.1 - v30: 32 bytes
	 *   v31+:        36 bytes (returnParameterToken inserted at +12,
	 *                          pushing parameterStart.. by +4)
	 * See doc/future.md §4.1 and third_party/Il2CppDumper/.../MetadataClass.cs. */
	const bool v31_layout = (meta->version >= 31);
	const ut64 entry = v31_layout ? 36 : 32;
	const int off_parameterStart  = v31_layout ? 16 : 12;
	const int off_genericContainer = v31_layout ? 20 : 16;
	const int off_token           = v31_layout ? 24 : 20;
	const int off_flags           = v31_layout ? 28 : 24;
	const int off_iflags          = v31_layout ? 30 : 26;
	const int off_slot            = v31_layout ? 32 : 28;
	const int off_parameterCount  = v31_layout ? 34 : 30;

	if ((ut64) meta->methodsSize < entry) {
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
		methods[i].nameIndex = r_read_le32 (p + 0);
		methods[i].declaringType = (int32_t) r_read_le32 (p + 4);
		methods[i].returnType = (int32_t) r_read_le32 (p + 8);
		methods[i].parameterStart = (int32_t) r_read_le32 (p + off_parameterStart);
		methods[i].genericContainerIndex = (int32_t) r_read_le32 (p + off_genericContainer);
		methods[i].token = r_read_le32 (p + off_token);
		methods[i].flags = r_read_le16 (p + off_flags);
		methods[i].iflags = r_read_le16 (p + off_iflags);
		methods[i].slot = r_read_le16 (p + off_slot);
		methods[i].parameterCount = r_read_le16 (p + off_parameterCount);
	}
	R_FREE (buf);
	return methods;
}

R_API Il2CppImageDefinition *r2unity_get_images (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	/* Common-field offsets are identical across v24/v27/v29 headers. */
	ut64 ioff = meta->header.v24.imagesOffset;
	ut64 isize = meta->header.v24.imagesSize;
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
		imgs[i].nameIndex = r_read_le32 (p + 0);
		imgs[i].assemblyIndex = (int32_t) r_read_le32 (p + 4);
		imgs[i].typeStart = (int32_t) r_read_le32 (p + 8);
		imgs[i].typeCount = r_read_le32 (p + 12);
		imgs[i].exportedTypeStart = (int32_t) r_read_le32 (p + 16);
		imgs[i].exportedTypeCount = r_read_le32 (p + 20);
		imgs[i].entryPointIndex = (int32_t) r_read_le32 (p + 24);
		imgs[i].token = r_read_le32 (p + 28);
		imgs[i].customAttributeStart = (int32_t) r_read_le32 (p + 32);
		imgs[i].customAttributeCount = r_read_le32 (p + 36);
	}
	R_FREE (buf2);
	return imgs;
}

R_API Il2CppAssemblyDefinition *r2unity_get_assemblies (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	/* Common-field offsets are identical across v24/v27/v29 headers. */
	ut64 aoff = meta->header.v24.assembliesOffset;
	ut64 asize = meta->header.v24.assembliesSize;
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
		R_LOG_WARN ("assembliesSize=%"PFMT64u" not divisible by image_count=%u",
			asize, (unsigned) img_count);
		return NULL;
	}
	const ut64 entry = asize / img_count;
	if (entry < 56 || entry > 80) {
		R_LOG_WARN ("implausible assembly entry stride %"PFMT64u, entry);
		return NULL;
	}
	/* Within a row:
	 *   [0..4)    imageIndex
	 *   [4..8)    token (wire >= 24.1) OR customAttributeIndex (wire <= 24.0)
	 *   [8..12)   referencedAssemblyStart (wire >= 20)
	 *   [12..16)  referencedAssemblyCount
	 *   [16..entry) Il2CppAssemblyNameDefinition
	 * aname size is (entry - 16): 48 when hashValueIndex dropped (>=24.4),
	 * 52 when it is still present (<=24.3). Layout within aname:
	 *   nameIndex, cultureIndex, [hashValueIndex if 52], publicKeyIndex,
	 *   hash_alg, hash_len, flags, major, minor, build, revision,
	 *   public_key_token[8]. */
	const ut64 aname_size = entry - 16;
	bool has_hash_value = (aname_size == 52);
	if (aname_size != 48 && aname_size != 52) {
		R_LOG_WARN ("unexpected aname size %"PFMT64u, aname_size);
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
	if (r_buf_read_at (meta->buf, aoff, buf, asize) != (st64) asize) {
		R_FREE (buf);
		R_FREE (out);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		out[i].image_index = (int32_t) r_read_le32 (p + 0);
		out[i].token = (meta->version >= 24) ? r_read_le32 (p + 4) : 0;
		out[i].referenced_start = (int32_t) r_read_le32 (p + 8);
		out[i].referenced_count = (int32_t) r_read_le32 (p + 12);
		const ut8 *an = p + 16;
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
		out[i].aname.hash_alg = r_read_le32 (an + o); o += 4;
		out[i].aname.hash_len = (int32_t) r_read_le32 (an + o); o += 4;
		out[i].aname.flags = r_read_le32 (an + o); o += 4;
		out[i].aname.major = (int32_t) r_read_le32 (an + o); o += 4;
		out[i].aname.minor = (int32_t) r_read_le32 (an + o); o += 4;
		out[i].aname.build = (int32_t) r_read_le32 (an + o); o += 4;
		out[i].aname.revision = (int32_t) r_read_le32 (an + o); o += 4;
		memcpy (out[i].aname.public_key_token, an + o, 8);
	}
	R_FREE (buf);
	return out;
}

R_API int32_t *r2unity_get_referenced_assemblies (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	ut64 roff = meta->header.v24.referencedAssembliesOffset;
	ut64 rsize = meta->header.v24.referencedAssembliesSize;
	if (rsize < 4 || (rsize % 4) != 0) {
		return NULL;
	}
	*count = (size_t) (rsize / 4);
	int32_t *out = R_NEWS (int32_t, *count);
	if (!out) {
		*count = 0;
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, rsize);
	if (!buf) {
		R_FREE (out);
		*count = 0;
		return NULL;
	}
	if (r_buf_read_at (meta->buf, roff, buf, rsize) != (st64) rsize) {
		R_FREE (buf);
		R_FREE (out);
		*count = 0;
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		out[i] = (int32_t) r_read_le32 (buf + i * 4);
	}
	R_FREE (buf);
	return out;
}

R_API bool r2unity_read_method_pointers_at (R2UnityMetadata *meta, const char *exe_path, ut64 addr, size_t count, ut64 **out_ptrs) {
	R_RETURN_VAL_IF_FAIL (meta && exe_path && out_ptrs, false);
	(void) addr;
	(void) count;
	return false;
}

/* Per-image sorted (token, method_idx) index used for scoped token lookups.
 * Method tokens are only unique within their owning image. */
typedef struct { uint32_t token; int32_t idx; } R2UTokIdx;

static int cmp_tokidx (const void *a, const void *b) {
	uint32_t ta = ((const R2UTokIdx *) a)->token;
	uint32_t tb = ((const R2UTokIdx *) b)->token;
	return (ta < tb) ? -1 : (ta > tb);
}

/* Build a typeIndex -> imageIndex map once per enumeration pass. Returns NULL
 * if no images are known or the type table is empty. */
static int *build_type2img (const Il2CppImageDefinition *images, size_t image_count,
		size_t type_count) {
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
		int tcount = (int) images[ii].typeCount;
		if (start < 0 || tcount <= 0) {
			continue;
		}
		for (int k = 0; k < tcount && (size_t) (start + k) < type_count; k++) {
			type2img[start + k] = (int) ii;
		}
	}
	return type2img;
}

/* Resolve the owning image (DLL) of a given method via its declaring type. */
static int image_index_for_method (const int *type2img, size_t type_count,
		const Il2CppMethodDefinition *m) {
	if (!type2img || m->declaringType < 0 || (size_t) m->declaringType >= type_count) {
		return -1;
	}
	return type2img[m->declaringType];
}

static char *build_qualified_name (R2UnityMetadata *meta,
		const Il2CppTypeDefinition *td, uint32_t method_name_idx) {
	char *ns = td ? (char *) r2unity_get_string (meta, td->namespaceIndex) : NULL;
	char *tn = td ? (char *) r2unity_get_string (meta, td->nameIndex) : NULL;
	char *mn = (char *) r2unity_get_string (meta, method_name_idx);
	if (!mn) {
		free (ns);
		free (tn);
		return NULL;
	}
	char buf[1024];
	if (ns && *ns && tn && *tn) {
		snprintf (buf, sizeof (buf), "%s.%s.%s", ns, tn, mn);
	} else if (tn && *tn) {
		snprintf (buf, sizeof (buf), "%s.%s", tn, mn);
	} else {
		snprintf (buf, sizeof (buf), "%s", mn);
	}
	char *out = strdup (buf);
	free (ns);
	free (tn);
	free (mn);
	return out;
}

R_API R2UnityInterop *r2unity_enumerate_pinvokes (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;

	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);
	if (!methods || !method_count) {
		R_FREE (methods);
		return NULL;
	}
	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	size_t image_count = 0;
	Il2CppImageDefinition *images = r2unity_get_images (meta, &image_count);
	int *type2img = build_type2img (images, image_count, type_count);

	/* First pass: count P/Invoke methods. */
	size_t n = 0;
	for (size_t j = 0; j < method_count; j++) {
		if (methods[j].flags & IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL) {
			n++;
		}
	}
	if (!n) {
		R_FREE (methods);
		R_FREE (types);
		R_FREE (images);
		R_FREE (type2img);
		return NULL;
	}

	R2UnityInterop *out = R_NEWS0 (R2UnityInterop, n);
	if (!out) {
		R_FREE (methods);
		R_FREE (types);
		R_FREE (images);
		R_FREE (type2img);
		return NULL;
	}

	size_t k = 0;
	for (size_t j = 0; j < method_count && k < n; j++) {
		Il2CppMethodDefinition *m = &methods[j];
		if (!(m->flags & IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL)) {
			continue;
		}
		R2UnityInterop *it = &out[k++];
		it->kind = R2U_INTEROP_PINVOKE;
		it->method_index = (int32_t) j;
		it->token = m->token;
		it->flags = m->flags;
		it->iflags = m->iflags;
		it->wrapper_va = 0;
		it->wrapper_index = UINT32_MAX;
		it->confidence = 100;

		const Il2CppTypeDefinition *td = NULL;
		if (types && m->declaringType >= 0 && (size_t) m->declaringType < type_count) {
			td = &types[m->declaringType];
		}
		it->name = build_qualified_name (meta, td, m->nameIndex);
		int img_idx = image_index_for_method (type2img, type_count, m);
		it->image_index = img_idx;
		if (img_idx >= 0 && images && (size_t) img_idx < image_count) {
			it->image_name = (char *) r2unity_get_string (meta, images[img_idx].nameIndex);
		}
		/* DLL name / entry-point recovery requires attribute-BLOB decoding
		 * (v29+) or generator disassembly (v<=27.2). Not implemented yet. */
	}

	R_FREE (methods);
	R_FREE (types);
	R_FREE (images);
	R_FREE (type2img);

	*count = n;
	return out;
}

/* v29+ attribute data range: { uint32 token, uint32 startOffset }. The end of
 * an entry's BLOB is the next entry's startOffset, or attributeDataSize for
 * the last entry. See doc/pinvoke.md §4.1 and Il2CppDumper Metadata.cs:131. */
typedef struct {
	uint32_t token;
	uint32_t start_offset;
} R2UAttrRange;

static R2UAttrRange *load_attribute_ranges (R2UnityMetadata *meta, size_t *count,
		ut64 *out_data_off, ut64 *out_data_size) {
	*count = 0;
	*out_data_off = 0;
	*out_data_size = 0;
	if (meta->version < 29) {
		return NULL;
	}
	/* v29+ repurposes the v27-era fields at this position:
	 *   attributesInfoOffset/Size  -> attributeDataOffset/Size
	 *   attributeTypesOffset/Size  -> attributeDataRangeOffset/Size
	 * The byte positions are identical; only the semantics changed. */
	ut64 data_off = meta->header.v29.attributesInfoOffset;
	ut64 data_size = meta->header.v29.attributesInfoSize;
	ut64 range_off = meta->header.v29.attributeTypesOffset;
	ut64 range_size = meta->header.v29.attributeTypesSize;
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
	if (r_buf_read_at (meta->buf, range_off, buf, range_size) != (st64) range_size) {
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
	*out_data_off = data_off;
	*out_data_size = data_size;
	return ranges;
}

/* ECMA-335 §II.23.2 compressed uint32. Returns 0 on malformed input. */
static uint32_t read_compressed_uint (const ut8 *buf, size_t bufsize, size_t *pos) {
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
	uint32_t v = ((uint32_t) (b0 & 0x1F) << 24)
		| ((uint32_t) buf[*pos + 1] << 16)
		| ((uint32_t) buf[*pos + 2] << 8)
		| buf[*pos + 3];
	*pos += 4;
	return v;
}

/* For attribute entry at `range_idx` (v29+), read the ctor method indices
 * (first 4 bytes after count header) and check if any ctor's declaring type
 * matches the target attribute type name.
 *
 * Returns the matched attribute kind (R2U_INTEROP_REVERSE_PINVOKE or
 * R2U_INTEROP_UNMANAGED_ONLY), or 0 if no match. */
static uint8_t attribute_range_interop_kind (
		R2UnityMetadata *meta,
		const R2UAttrRange *ranges, size_t nranges,
		size_t range_idx,
		ut64 attr_data_off, ut64 attr_data_size,
		const Il2CppMethodDefinition *methods, size_t nmethods,
		const Il2CppTypeDefinition *types, size_t ntypes) {
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
	ut8 *blob = R_NEWS (ut8, (size_t) blob_len);
	if (!blob) {
		return 0;
	}
	if (r_buf_read_at (meta->buf, attr_data_off + blob_start, blob, blob_len) != (st64) blob_len) {
		R_FREE (blob);
		return 0;
	}
	size_t pos = 0;
	uint32_t n_attrs = read_compressed_uint (blob, blob_len, &pos);
	if (n_attrs == 0 || (ut64) pos + (ut64) n_attrs * 4 > blob_len) {
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
		if (declaring < 0 || (size_t) declaring >= ntypes) {
			continue;
		}
		const char *name = r2unity_get_string (meta, types[declaring].nameIndex);
		if (!name) {
			continue;
		}
		if (!strcmp (name, "MonoPInvokeCallbackAttribute")) {
			kind = R2U_INTEROP_REVERSE_PINVOKE;
			free ((void *) name);
			break;
		}
		if (!strcmp (name, "UnmanagedCallersOnlyAttribute")) {
			kind = R2U_INTEROP_UNMANAGED_ONLY;
			free ((void *) name);
			break;
		}
		free ((void *) name);
	}
	R_FREE (blob);
	return kind;
}

R_API R2UnityInterop *r2unity_enumerate_reverse_pinvokes (R2UnityMetadata *meta, size_t *count) {
	R_RETURN_VAL_IF_FAIL (meta && count, NULL);
	*count = 0;
	if (meta->version < 29) {
		/* Pre-v29: attribute args live in native generator stubs; see doc/pinvoke.md §4.2. */
		return NULL;
	}

	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);
	if (!methods || !method_count) {
		R_FREE (methods);
		return NULL;
	}
	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	size_t image_count = 0;
	Il2CppImageDefinition *images = r2unity_get_images (meta, &image_count);

	ut64 attr_data_off = 0, attr_data_size = 0;
	size_t nranges = 0;
	R2UAttrRange *ranges = load_attribute_ranges (meta, &nranges, &attr_data_off, &attr_data_size);
	if (!ranges || !nranges) {
		R_FREE (methods);
		R_FREE (types);
		R_FREE (images);
		R_FREE (ranges);
		return NULL;
	}

	int *type2img = build_type2img (images, image_count, type_count);

	/* Build per-image sorted (token, method_idx) index. */
	int32_t *img_tok_start = NULL;
	int32_t *img_tok_count = NULL;
	R2UTokIdx *tmap = NULL;
	if (type2img && image_count) {
		img_tok_start = R_NEWS0 (int32_t, image_count);
		img_tok_count = R_NEWS0 (int32_t, image_count);
		tmap = R_NEWS (R2UTokIdx, method_count);
		if (!img_tok_start || !img_tok_count || !tmap) {
			/* Partial allocation: drop all three so the lookup loop's
			 * `if (tmap)` guard correctly skips the index. */
			R_FREE (img_tok_start);
			R_FREE (img_tok_count);
			R_FREE (tmap);
		} else {
			for (size_t j = 0; j < method_count; j++) {
				int img_idx = image_index_for_method (type2img, type_count, &methods[j]);
				if (img_idx < 0 || (size_t) img_idx >= image_count || !methods[j].token) {
					continue;
				}
				img_tok_count[img_idx]++;
			}
			int32_t acc = 0;
			for (size_t ii = 0; ii < image_count; ii++) {
				img_tok_start[ii] = acc;
				acc += img_tok_count[ii];
				img_tok_count[ii] = 0;  /* reused as append cursor */
			}
			for (size_t j = 0; j < method_count; j++) {
				int img_idx = image_index_for_method (type2img, type_count, &methods[j]);
				if (img_idx < 0 || (size_t) img_idx >= image_count || !methods[j].token) {
					continue;
				}
				int32_t pos = img_tok_start[img_idx] + img_tok_count[img_idx]++;
				tmap[pos].token = methods[j].token;
				tmap[pos].idx = (int32_t) j;
			}
			for (size_t ii = 0; ii < image_count; ii++) {
				qsort (tmap + img_tok_start[ii], (size_t) img_tok_count[ii],
					sizeof (R2UTokIdx), cmp_tokidx);
			}
		}
	}

	/* Iterate attribute ranges per-image, so method-token lookups stay scoped. */
	size_t cap = 32;
	R2UnityInterop *out = R_NEWS0 (R2UnityInterop, cap);
	size_t n = 0;
	if (!out) {
		goto done;
	}
	for (size_t ii = 0; ii < image_count; ii++) {
		int32_t r_start = images[ii].customAttributeStart;
		int32_t r_end = r_start + (int32_t) images[ii].customAttributeCount;
		if (r_start < 0 || r_end < 0 || (size_t) r_end > nranges) {
			continue;
		}
		for (int32_t ri = r_start; ri < r_end; ri++) {
			uint32_t tok = ranges[ri].token;
			if ((tok >> 24) != 0x06) {
				continue;
			}
			uint8_t kind = attribute_range_interop_kind (meta, ranges, nranges, (size_t) ri,
				attr_data_off, attr_data_size,
				methods, method_count, types, type_count);
			if (!kind) {
				continue;
			}
			/* Binary-search this image's token slice. */
			int32_t mi = -1;
			if (tmap) {
				size_t lo = (size_t) img_tok_start[ii];
				size_t hi = lo + (size_t) img_tok_count[ii];
				while (lo < hi) {
					size_t mid = (lo + hi) >> 1;
					if (tmap[mid].token < tok) {
						lo = mid + 1;
					} else if (tmap[mid].token > tok) {
						hi = mid;
					} else {
						mi = tmap[mid].idx;
						break;
					}
				}
			}
			if (mi < 0) {
				continue;
			}
			if (n + 1 > cap) {
			size_t ncap = cap * 2;
			R2UnityInterop *noo = realloc (out, ncap * sizeof (R2UnityInterop));
			if (!noo) {
				break;
			}
			memset (noo + cap, 0, (ncap - cap) * sizeof (R2UnityInterop));
			out = noo;
			cap = ncap;
		}
		R2UnityInterop *it = &out[n++];
		const Il2CppMethodDefinition *m = &methods[mi];
		it->kind = kind;
		it->method_index = mi;
		it->token = m->token;
		it->flags = m->flags;
		it->iflags = m->iflags;
		it->wrapper_va = 0;
		it->wrapper_index = UINT32_MAX;
		it->confidence = 100;

		const Il2CppTypeDefinition *td = NULL;
		if (types && m->declaringType >= 0 && (size_t) m->declaringType < type_count) {
			td = &types[m->declaringType];
		}
		it->name = build_qualified_name (meta, td, m->nameIndex);

		int img_idx = -1;
		if (type2img && m->declaringType >= 0 && (size_t) m->declaringType < type_count) {
			img_idx = type2img[m->declaringType];
		}
		it->image_index = img_idx;
		if (img_idx >= 0 && images && (size_t) img_idx < image_count) {
			it->image_name = (char *) r2unity_get_string (meta, images[img_idx].nameIndex);
		}
		}  /* close for ri */
	}  /* close for ii */
	if (n == 0) {
		R_FREE (out);
		out = NULL;
	}
done:
	R_FREE (ranges);
	R_FREE (methods);
	R_FREE (types);
	R_FREE (images);
	R_FREE (type2img);
	R_FREE (img_tok_start);
	R_FREE (img_tok_count);
	R_FREE (tmap);
	*count = n;
	return out;
}

R_API void r2unity_free_interop (R2UnityInterop *items, size_t count) {
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

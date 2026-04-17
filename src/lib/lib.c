#include "lib.h"
#include <r_util.h>

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
	// Read sanity and version from the on-disk little-endian layout.
	ut8 preamble[8];
	if (r_buf_read_at (buf, 0, preamble, sizeof (preamble)) != (st64) sizeof (preamble)) {
		R_FREE (meta);
		return NULL;
	}
	uint32_t sanity = r_read_le32 (preamble);
	if (sanity != IL2CPP_MAGIC) {
		R_FREE (meta);
		return NULL;
	}
	int32_t version = (int32_t) r_read_le32 (preamble + 4);
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
	if (r_buf_read_at (buf, 0, (ut8 *) &meta->header.v24, header_size) != (st64) header_size) {
		R_FREE (meta);
		return NULL;
	}
	// Normalize header from on-disk little-endian to native byte order.
	// Every field in the Il2Cpp header structs is a 32-bit word, so a single
	// in-place r_read_le32 sweep handles all versions and is a no-op on LE hosts.
	uint32_t *hdr_words = (uint32_t *) &meta->header;
	size_t nfields = header_size / sizeof (uint32_t);
	for (size_t i = 0; i < nfields; i++) {
		hdr_words[i] = r_read_le32 (&hdr_words[i]);
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
	if (!meta || !count) {
		return NULL;
	}
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

static void get_assemblies_section (R2UnityMetadata *meta, ut64 *out_off, ut64 *out_size) {
	if (meta->version < 27) {
		*out_off = (ut64) meta->header.v24.assembliesOffset;
		*out_size = (ut64) meta->header.v24.assembliesSize;
	} else if (meta->version < 29) {
		*out_off = (ut64) meta->header.v27.assembliesOffset;
		*out_size = (ut64) meta->header.v27.assembliesSize;
	} else {
		*out_off = (ut64) meta->header.v29.assembliesOffset;
		*out_size = (ut64) meta->header.v29.assembliesSize;
	}
}

static void get_referenced_section (R2UnityMetadata *meta, ut64 *out_off, ut64 *out_size) {
	if (meta->version < 27) {
		*out_off = (ut64) meta->header.v24.referencedAssembliesOffset;
		*out_size = (ut64) meta->header.v24.referencedAssembliesSize;
	} else if (meta->version < 29) {
		*out_off = (ut64) meta->header.v27.referencedAssembliesOffset;
		*out_size = (ut64) meta->header.v27.referencedAssembliesSize;
	} else {
		*out_off = (ut64) meta->header.v29.referencedAssembliesOffset;
		*out_size = (ut64) meta->header.v29.referencedAssembliesSize;
	}
}

R_API Il2CppAssemblyDefinition *r2unity_get_assemblies (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	*count = 0;
	ut64 aoff = 0, asize = 0;
	get_assemblies_section (meta, &aoff, &asize);
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
		if (g_debug) {
			fprintf (stderr, "[r2unity] assembliesSize=%"PFMT64u" not divisible by image_count=%u\n",
				asize, (unsigned) img_count);
		}
		return NULL;
	}
	const ut64 entry = asize / img_count;
	if (entry < 56 || entry > 80) {
		if (g_debug) {
			fprintf (stderr, "[r2unity] implausible assembly entry stride %"PFMT64u"\n", entry);
		}
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
		if (g_debug) {
			fprintf (stderr, "[r2unity] unexpected aname size %"PFMT64u"\n", aname_size);
		}
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
	if (!meta || !count) {
		return NULL;
	}
	*count = 0;
	ut64 roff = 0, rsize = 0;
	get_referenced_section (meta, &roff, &rsize);
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
	(void) meta; (void) exe_path; (void) addr; (void) count; (void) out_ptrs;
	return false;
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
	if (!meta || !count) {
		return NULL;
	}
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

	/* Build typeIndex -> imageIndex map once. */
	int *type2img = NULL;
	if (images && type_count) {
		type2img = R_NEWS (int, type_count);
		if (type2img) {
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
		}
	}

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

		int img_idx = -1;
		if (type2img && m->declaringType >= 0 && (size_t) m->declaringType < type_count) {
			img_idx = type2img[m->declaringType];
		}
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

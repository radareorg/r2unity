/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.bin"

#include <r_bin.h>
#include <r_lib.h>
#include "../lib/lib.h"

#if defined(R_LIB_ABIVERSION) && !defined(R2_ABIVERSION)
#define R2_ABIVERSION R_LIB_ABIVERSION
#endif

#if R2_ABIVERSION >= 100
#define R2UNITY_BIN_VEC_ABI 1
#else
#define R2UNITY_BIN_VEC_ABI 0
#endif

typedef struct {
	R2UnityMetadata *meta;
	RBuffer *buf;
	Sdb *kv;
	ut8 *strings;
	ut64 strings_size;
} R2UnityBinObj;

static R2UnityBinObj *get_obj(RBinFile *bf) {
	RBinObject *o = bf? bf->bo: NULL;
	return o? (R2UnityBinObj *)o->bin_obj: NULL;
}

static char *get_string(R2UnityBinObj *obj, uint32_t index) {
	if (!obj || !obj->strings || index >= obj->strings_size) {
		return NULL;
	}
	ut64 idx = index;
	while (idx > 0 && obj->strings[idx - 1]) {
		idx--;
	}
	ut64 len = 0;
	while (idx + len < obj->strings_size && obj->strings[idx + len]) {
		len++;
	}
	return len? r_str_ndup ((const char *)obj->strings + idx, (int)len): NULL;
}

static bool valid_wire_version(int32_t version) {
	return version >= 24 && version <= 39 && (version <= 35 || version >= 38);
}

static bool check(RBinFile *bf, RBuffer *b) {
	(void)bf;
	R_RETURN_VAL_IF_FAIL (b, false);
	ut8 preamble[8];
	if (r_buf_read_at (b, 0, preamble, sizeof (preamble)) != (st64)sizeof (preamble)) {
		return false;
	}
	if (r_read_le32 (preamble) != IL2CPP_MAGIC) {
		return false;
	}
	return valid_wire_version ((int32_t)r_read_le32 (preamble + 4));
}

static void fill_sdb(R2UnityBinObj *obj) {
	Sdb *kv = obj->kv;
	R2UnityMetadata *meta = obj->meta;
	sdb_num_set (kv, "version", (ut64)meta->version, 0);
	sdb_set (kv, "unity_range", r2unity_unity_range_from_wire (meta->version), 0);
	sdb_num_set (kv, "header.size", r2unity_metadata_header_size (meta), 0);
	for (int i = 0; i < R2UNITY_METADATA_SECTION_COUNT; i++) {
		Il2CppMetadataSection sec;
		if (!r2unity_metadata_section (meta, (R2UMetadataSectionId)i, &sec)) {
			continue;
		}
		const char *name = r2unity_metadata_section_name ((R2UMetadataSectionId)i);
		if (!name) {
			continue;
		}
		char key[192];
		snprintf (key, sizeof (key), "sections.%s.offset", name);
		sdb_num_set (kv, key, sec.offset, 0);
		snprintf (key, sizeof (key), "sections.%s.size", name);
		sdb_num_set (kv, key, sec.size, 0);
		snprintf (key, sizeof (key), "sections.%s.count", name);
		sdb_num_set (kv, key, r2unity_metadata_section_count (meta, (R2UMetadataSectionId)i), 0);
		snprintf (key, sizeof (key), "sections.%s.entry_size", name);
		sdb_num_set (kv, key, r2unity_metadata_section_entry_size (meta, (R2UMetadataSectionId)i), 0);
	}
	sdb_num_set (kv, "counts.types", r2unity_metadata_section_count (meta, R2U_SEC_TYPE_DEFINITIONS), 0);
	sdb_num_set (kv, "counts.methods", r2unity_metadata_section_count (meta, R2U_SEC_METHODS), 0);
	sdb_num_set (kv, "counts.images", r2unity_metadata_section_count (meta, R2U_SEC_IMAGES), 0);
	sdb_num_set (kv, "counts.assemblies", r2unity_metadata_section_count (meta, R2U_SEC_ASSEMBLIES), 0);
	sdb_num_set (kv, "counts.string_literals", r2unity_metadata_section_count (meta, R2U_SEC_STRING_LITERALS), 0);
}

static bool load(RBinFile *bf, RBuffer *buf, ut64 loadaddr) {
	(void)loadaddr;
	R_RETURN_VAL_IF_FAIL (bf && bf->bo && buf, false);
	R2UnityBinObj *obj = R_NEW0 (R2UnityBinObj);
	obj->buf = r_buf_new_slice (buf, 0, r_buf_size (buf));
	obj->meta = r2unity_parse_metadata (obj->buf);
	if (!obj->meta) {
		r_unref (obj->buf);
		free (obj);
		return false;
	}
	Il2CppMetadataSection strings_sec;
	r2unity_metadata_section (obj->meta, R2U_SEC_STRINGS, &strings_sec);
	obj->strings_size = strings_sec.size;
	if (obj->strings_size) {
		obj->strings = R_NEWS (ut8, obj->strings_size);
		if (obj->strings && r_buf_read_at (obj->buf, strings_sec.offset, obj->strings, obj->strings_size) != (st64)obj->strings_size) {
			R_FREE (obj->strings);
			obj->strings_size = 0;
		}
	}
	obj->kv = sdb_new0 ();
	fill_sdb (obj);
	bf->bo->bin_obj = obj;
	return true;
}

static void destroy(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	if (!obj) {
		return;
	}
	r2unity_free_metadata (obj->meta);
	r_unref (obj->buf);
	R_FREE (obj->strings);
	free (obj);
}

static Sdb *get_sdb(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	return obj? obj->kv: NULL;
}

static ut64 baddr(RBinFile *bf) {
	(void)bf;
	return 0;
}

static ut64 size(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	return obj? r_buf_size (obj->buf): 0;
}

static RBinInfo *info(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RBinInfo *ret = R_NEW0 (RBinInfo);
	ret->file = bf->file? strdup (bf->file): NULL;
	ret->type = r_str_newf ("Unity IL2CPP global-metadata v%d", obj->meta->version);
	ret->bclass = strdup ("Unity IL2CPP metadata");
	ret->rclass = strdup ("il2cpp");
	ret->machine = strdup ("Unity IL2CPP metadata");
	ret->os = strdup ("any");
	ret->arch = strdup ("cil");
	ret->bits = 32;
	ret->has_va = false;
	ret->big_endian = 0;
	ret->has_lit = true;
	ret->lang = "csharp";
	return ret;
}

static void init_section(RBinSection *sec, const char *name, ut64 off, ut64 size, bool has_strings) {
	sec->name = strdup (name);
	sec->paddr = off;
	sec->vaddr = off;
	sec->size = size;
	sec->vsize = size;
	sec->perm = R_PERM_R;
	sec->type = "DATA";
	sec->bits = 32;
	sec->has_strings = has_strings;
	sec->is_data = true;
	sec->add = true;
}

#if R2UNITY_BIN_VEC_ABI
static void append_section(RVecRBinSection *ret, const char *name, ut64 off, ut64 size, bool has_strings) {
	RBinSection *sec = RVecRBinSection_emplace_back (ret);
	if (sec) {
		init_section (sec, name, off, size, has_strings);
	}
}

static bool sections_vec(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (bf && bf->bo && obj, false);
	RVecRBinSection *ret = &bf->bo->sections_vec;
	RVecRBinSection_clear (ret);
	ut64 header_size = r2unity_metadata_header_size (obj->meta);
	if (header_size) {
		append_section (ret, "il2cpp.header", 0, header_size, false);
	}
	for (int i = 0; i < R2UNITY_METADATA_SECTION_COUNT; i++) {
		Il2CppMetadataSection sec;
		r2unity_metadata_section (obj->meta, (R2UMetadataSectionId)i, &sec);
		if (!sec.size) {
			continue;
		}
		const char *name = r2unity_metadata_section_name ((R2UMetadataSectionId)i);
		if (!name) {
			continue;
		}
		bool has_strings = i == R2U_SEC_STRINGS || i == R2U_SEC_STRING_LITERAL_DATA || i == R2U_SEC_WINDOWS_RUNTIME_STRINGS;
		char *sname = r_str_newf ("il2cpp.%s", name);
		append_section (ret, sname, sec.offset, sec.size, has_strings);
		free (sname);
	}
	return true;
}
#else
static void section_free(void *p) {
	RBinSection *sec = (RBinSection *)p;
	if (sec) {
		free (sec->name);
		free (sec->format);
		free (sec);
	}
}

static RBinSection *new_section(const char *name, ut64 off, ut64 size, bool has_strings) {
	RBinSection *sec = R_NEW0 (RBinSection);
	if (!sec) {
		return NULL;
	}
	init_section (sec, name, off, size, has_strings);
	return sec;
}

static RList *sections(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RList *ret = r_list_newf (section_free);
	ut64 header_size = r2unity_metadata_header_size (obj->meta);
	if (header_size) {
		r_list_append (ret, new_section ("il2cpp.header", 0, header_size, false));
	}
	for (int i = 0; i < R2UNITY_METADATA_SECTION_COUNT; i++) {
		Il2CppMetadataSection sec;
		r2unity_metadata_section (obj->meta, (R2UMetadataSectionId)i, &sec);
		if (!sec.size) {
			continue;
		}
		const char *name = r2unity_metadata_section_name ((R2UMetadataSectionId)i);
		if (!name) {
			continue;
		}
		bool has_strings = i == R2U_SEC_STRINGS || i == R2U_SEC_STRING_LITERAL_DATA || i == R2U_SEC_WINDOWS_RUNTIME_STRINGS;
		char *sname = r_str_newf ("il2cpp.%s", name);
		r_list_append (ret, new_section (sname, sec.offset, sec.size, has_strings));
		free (sname);
	}
	return ret;
}
#endif

static RBinAttribute method_attr(uint16_t flags, const char *name) {
	enum {
		MemberAccessMask = 0x0007,
		MdPrivate = 0x0001,
		MdAssembly = 0x0003,
		MdFamily = 0x0004,
		MdPublic = 0x0006,
		MdStatic = 0x0010,
		MdFinal = 0x0020,
		MdVirtual = 0x0040,
		MdAbstract = 0x0400
	};
	RBinAttribute attr = R_BIN_ATTR_NONE;
	switch (flags & MemberAccessMask) {
	case MdPublic:
		attr |= R_BIN_ATTR_PUBLIC;
		break;
	case MdFamily:
		attr |= R_BIN_ATTR_PROTECTED;
		break;
	case MdAssembly:
		attr |= R_BIN_ATTR_INTERNAL;
		break;
	case MdPrivate:
		attr |= R_BIN_ATTR_PRIVATE;
		break;
	}
	if (flags & MdStatic) {
		attr |= R_BIN_ATTR_STATIC;
	}
	if (flags & MdFinal) {
		attr |= R_BIN_ATTR_FINAL;
	}
	if (flags & MdVirtual) {
		attr |= R_BIN_ATTR_VIRTUAL;
	}
	if (flags & MdAbstract) {
		attr |= R_BIN_ATTR_ABSTRACT;
	}
	if (flags & IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL) {
		attr |= R_BIN_ATTR_EXTERN | R_BIN_ATTR_NATIVE;
	}
	if (name && (!strcmp (name, ".ctor") || !strcmp (name, ".cctor"))) {
		attr |= R_BIN_ATTR_CONSTRUCTOR;
	}
	return attr;
}

static RBinAttribute type_attr(uint32_t flags) {
	enum {
		TypeVisibilityMask = 0x00000007,
		TypePublic = 0x00000001,
		TypeNestedPublic = 0x00000002,
		TypeNestedPrivate = 0x00000003,
		TypeNestedFamily = 0x00000004,
		TypeAbstract = 0x00000080,
		TypeSealed = 0x00000100,
		TypeInterface = 0x00000020
	};
	RBinAttribute attr = R_BIN_ATTR_NONE;
	switch (flags & TypeVisibilityMask) {
	case TypePublic:
	case TypeNestedPublic:
		attr |= R_BIN_ATTR_PUBLIC;
		break;
	case TypeNestedPrivate:
		attr |= R_BIN_ATTR_PRIVATE;
		break;
	case TypeNestedFamily:
		attr |= R_BIN_ATTR_PROTECTED;
		break;
	}
	if (flags & TypeAbstract) {
		attr |= R_BIN_ATTR_ABSTRACT;
	}
	if (flags & TypeSealed) {
		attr |= R_BIN_ATTR_SEALED;
	}
	if (flags & TypeInterface) {
		attr |= R_BIN_ATTR_INTERFACE;
	}
	return attr;
}

static RBinSymbol *new_symbol(const char *name, ut64 paddr, ut64 size, const char *type, RBinAttribute attr, const char *classname, int ordinal) {
	RBinSymbol *sym = r_bin_symbol_new (name, paddr, paddr);
	if (!sym) {
		return NULL;
	}
	sym->size = (ut32)R_MIN (size, UT32_MAX);
	sym->type = type;
	sym->attr = attr;
	sym->ordinal = ordinal;
	sym->lang = R_BIN_LANG_NONE;
	if (classname) {
		sym->classname = strdup (classname);
	}
	return sym;
}

#if R2UNITY_BIN_VEC_ABI
typedef RVecRBinSymbol R2UnitySymbolSink;

static void append_symbol(R2UnitySymbolSink *ret, RBinSymbol *sym) {
	if (!sym) {
		return;
	}
	RBinSymbol *dst = RVecRBinSymbol_emplace_back (ret);
	if (!dst) {
		r_bin_symbol_free (sym);
		return;
	}
	*dst = *sym;
	free (sym);
}
#else
typedef RList R2UnitySymbolSink;

static void append_symbol(R2UnitySymbolSink *ret, RBinSymbol *sym) {
	if (sym) {
		r_list_append (ret, sym);
	}
}
#endif

static void append_class_method(RBinClass *klass, RBinSymbol *sym) {
	if (!sym) {
		return;
	}
#if R2UNITY_BIN_VEC_ABI
	append_symbol (&klass->methods, sym);
#else
	r_list_append (klass->methods, sym);
#endif
}

static bool symbols_fill(RBinFile *bf, R2UnitySymbolSink *ret) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, false);
	R2UnityMetadata *meta = obj->meta;
	int ordinal = 0;

	for (int i = 0; i < R2UNITY_METADATA_SECTION_COUNT; i++) {
		Il2CppMetadataSection sec;
		r2unity_metadata_section (meta, (R2UMetadataSectionId)i, &sec);
		if (!sec.size) {
			continue;
		}
		const char *name = r2unity_metadata_section_name ((R2UMetadataSectionId)i);
		if (!name) {
			continue;
		}
		char *sname = r_str_newf ("section.il2cpp.%s", name);
		RBinSymbol *sym = new_symbol (sname, sec.offset, sec.size, R_BIN_TYPE_SECTION_STR, R_BIN_ATTR_READONLY, NULL, ordinal++);
		append_symbol (ret, sym);
		free (sname);
	}

	size_t image_count = 0;
	Il2CppImageDefinition *images = r2unity_get_images (meta, &image_count);
	ut64 image_entry = r2unity_metadata_section_entry_size (meta, R2U_SEC_IMAGES);
	Il2CppMetadataSection image_sec;
	r2unity_metadata_section (meta, R2U_SEC_IMAGES, &image_sec);
	for (size_t i = 0; images && i < image_count; i++) {
		char *name = get_string (obj, images[i].nameIndex);
		if (name) {
			char *sname = r_str_newf ("image.%s", name);
			RBinSymbol *sym = new_symbol (sname, image_sec.offset + i * image_entry, image_entry, R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY, NULL, ordinal++);
			append_symbol (ret, sym);
			free (sname);
			free (name);
		}
	}

	size_t asm_count = 0;
	Il2CppAssemblyDefinition *asms = r2unity_get_assemblies (meta, &asm_count);
	ut64 asm_entry = r2unity_metadata_section_entry_size (meta, R2U_SEC_ASSEMBLIES);
	Il2CppMetadataSection asm_sec;
	r2unity_metadata_section (meta, R2U_SEC_ASSEMBLIES, &asm_sec);
	for (size_t i = 0; asms && i < asm_count; i++) {
		char *name = get_string (obj, asms[i].aname.name_idx);
		if (name) {
			char *sname = r_str_newf ("assembly.%s", name);
			RBinSymbol *sym = new_symbol (sname, asm_sec.offset + i * asm_entry, asm_entry, R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY, NULL, ordinal++);
			append_symbol (ret, sym);
			free (sname);
			free (name);
		}
	}

	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	ut64 type_entry = r2unity_metadata_section_entry_size (meta, R2U_SEC_TYPE_DEFINITIONS);
	Il2CppMetadataSection type_sec;
	r2unity_metadata_section (meta, R2U_SEC_TYPE_DEFINITIONS, &type_sec);
	for (size_t i = 0; types && i < type_count; i++) {
		char *name = r2unity_type_fullname (meta, &types[i], i, R2U_NAME_FALLBACK_TYPE);
		RBinSymbol *sym = new_symbol (name, type_sec.offset + i * type_entry, type_entry, R_BIN_TYPE_OBJECT_STR, type_attr (types[i].flags), NULL, ordinal++);
		append_symbol (ret, sym);
		free (name);
	}

	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);
	ut64 method_entry = r2unity_metadata_section_entry_size (meta, R2U_SEC_METHODS);
	Il2CppMetadataSection method_sec;
	r2unity_metadata_section (meta, R2U_SEC_METHODS, &method_sec);
	for (size_t i = 0; methods && i < method_count; i++) {
		const Il2CppTypeDefinition *td = NULL;
		size_t type_idx = 0;
		if (methods[i].declaringType >= 0 && (size_t)methods[i].declaringType < type_count) {
			type_idx = (size_t)methods[i].declaringType;
			td = &types[type_idx];
		}
		char *name = r2unity_method_fullname (meta, &methods[i], td, type_idx, R2U_NAME_WITH_PARAMS | R2U_NAME_FALLBACK_TYPE);
		if (!name) {
			continue;
		}
		char *mn = get_string (obj, methods[i].nameIndex);
		char *classname = NULL;
		if (td) {
			classname = r2unity_type_fullname (meta, td, type_idx, R2U_NAME_FALLBACK_TYPE);
		}
		RBinSymbol *sym = new_symbol (name, method_sec.offset + i * method_entry, method_entry, R_BIN_TYPE_METH_STR, method_attr (methods[i].flags, mn), classname, ordinal++);
		append_symbol (ret, sym);
		free (classname);
		free (mn);
		free (name);
	}

	R_FREE (methods);
	R_FREE (types);
	R_FREE (asms);
	R_FREE (images);
	return true;
}

#if R2UNITY_BIN_VEC_ABI
static bool symbols_vec(RBinFile *bf) {
	R_RETURN_VAL_IF_FAIL (bf && bf->bo, false);
	RVecRBinSymbol_clear (&bf->bo->symbols_vec);
	return symbols_fill (bf, &bf->bo->symbols_vec);
}
#else
static RList *symbols(RBinFile *bf) {
	RList *ret = r_list_newf ((RListFree)r_bin_symbol_free);
	if (!ret || !symbols_fill (bf, ret)) {
		r_list_free (ret);
		return NULL;
	}
	return ret;
}
#endif

static RList *classes(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	R2UnityMetadata *meta = obj->meta;
	RList *ret = r_list_newf ((RListFree)r_bin_class_free);

	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);
	Il2CppMetadataSection type_sec;
	Il2CppMetadataSection method_sec;
	r2unity_metadata_section (meta, R2U_SEC_TYPE_DEFINITIONS, &type_sec);
	r2unity_metadata_section (meta, R2U_SEC_METHODS, &method_sec);
	ut64 type_entry = r2unity_metadata_section_entry_size (meta, R2U_SEC_TYPE_DEFINITIONS);
	ut64 method_entry = r2unity_metadata_section_entry_size (meta, R2U_SEC_METHODS);

	for (size_t i = 0; types && i < type_count; i++) {
		char *ns = get_string (obj, types[i].namespaceIndex);
		char *tn = get_string (obj, types[i].nameIndex);
		char *fallback = NULL;
		if (!tn || !*tn) {
			fallback = r_str_newf ("type.%zu", i);
		}
		RBinClass *klass = r_bin_class_new ((tn && *tn)? tn: fallback, NULL, type_attr (types[i].flags));
		klass->index = (int)i;
		klass->addr = type_sec.offset + i * type_entry;
		klass->lang = R_BIN_LANG_NONE;
		if (ns && *ns) {
			klass->ns = strdup (ns);
		}
		int start = types[i].methodStart;
		int count = types[i].method_count;
		for (int k = 0; methods && k < count && start >= 0 && (size_t)(start + k) < method_count; k++) {
			size_t mi = (size_t)(start + k);
			const Il2CppTypeDefinition *td = &types[i];
			size_t type_idx = i;
			if (methods[mi].declaringType >= 0 && (size_t)methods[mi].declaringType < type_count) {
				type_idx = (size_t)methods[mi].declaringType;
				td = &types[type_idx];
			}
			char *mname = r2unity_method_fullname (meta, &methods[mi], td, type_idx, R2U_NAME_WITH_PARAMS | R2U_NAME_FALLBACK_TYPE);
			char *raw = get_string (obj, methods[mi].nameIndex);
			RBinSymbol *sym = new_symbol (mname? mname: raw, method_sec.offset + mi * method_entry, method_entry, R_BIN_TYPE_METH_STR, method_attr (methods[mi].flags, raw), NULL, (int)mi);
			append_class_method (klass, sym);
			free (raw);
			free (mname);
		}
		r_list_append (ret, klass);
		free (fallback);
		free (tn);
		free (ns);
	}

	R_FREE (methods);
	R_FREE (types);
	return ret;
}

static void append_bin_string(RList *ret, char *text, ut64 paddr, ut32 size, ut32 ordinal) {
	RBinString *str = R_NEW0 (RBinString);
	str->string = text;
	str->paddr = paddr;
	str->vaddr = paddr;
	str->size = size;
	str->length = strlen (text);
	str->ordinal = ordinal;
	str->type = R_STRING_TYPE_UTF8;
	r_list_append (ret, str);
}

static void add_metadata_strings(R2UnityBinObj *obj, RList *ret, ut32 *ordinal) {
	if (!obj->strings || !obj->strings_size) {
		return;
	}
	Il2CppMetadataSection sec;
	r2unity_metadata_section (obj->meta, R2U_SEC_STRINGS, &sec);
	ut64 i = 0;
	while (i < obj->strings_size) {
		while (i < obj->strings_size && !obj->strings[i]) {
			i++;
		}
		ut64 start = i;
		while (i < obj->strings_size && obj->strings[i]) {
			i++;
		}
		ut64 len = i - start;
		if (len > 1 && len < R_BIN_SIZEOF_STRINGS) {
			char *text = r_str_ndup ((const char *)obj->strings + start, (int)len);
			append_bin_string (ret, text, sec.offset + start, (ut32)(len + 1), (*ordinal)++);
		}
		i++;
	}
}

static void add_literal_strings(R2UnityBinObj *obj, RList *ret, ut32 *ordinal) {
	size_t count = 0;
	Il2CppStringLiteral *lits = r2unity_get_string_literals (obj->meta, &count);
	for (size_t i = 0; lits && i < count; i++) {
		ut8 *bytes = NULL;
		size_t len = 0;
		if (!r2unity_read_string_literal (obj->meta, &lits[i], &bytes, &len)) {
			continue;
		}
		if (len > 1 && len < R_BIN_SIZEOF_STRINGS) {
			char *text = r_str_ndup ((const char *)bytes, (int)len);
			append_bin_string (ret, text, obj->meta->stringLiteralDataOffset + lits[i].dataIndex, (ut32)len, (*ordinal)++);
		}
		R_FREE (bytes);
	}
	R_FREE (lits);
}

static RList *strings(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RList *ret = r_list_newf ((RListFree)r_bin_string_free);
	ut32 ordinal = 0;
	add_metadata_strings (obj, ret, &ordinal);
	add_literal_strings (obj, ret, &ordinal);
	return ret;
}

#if R2UNITY_BIN_VEC_ABI
typedef RVecRBinImport R2UnityImportSink;

static void append_import(R2UnityImportSink *ret, RBinImport *imp) {
	if (!imp) {
		return;
	}
	RBinImport *dst = RVecRBinImport_emplace_back (ret);
	if (!dst) {
		r_bin_import_free (imp);
		return;
	}
	*dst = *imp;
	free (imp);
}
#else
typedef RList R2UnityImportSink;

static void append_import(R2UnityImportSink *ret, RBinImport *imp) {
	if (imp) {
		r_list_append (ret, imp);
	}
}
#endif

static bool imports_fill(RBinFile *bf, R2UnityImportSink *ret) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, false);
	size_t count = 0;
	R2UnityInterop *items = r2unity_enumerate_pinvokes (obj->meta, &count);
	for (size_t i = 0; items && i < count; i++) {
		R2UnityInterop *it = &items[i];
		RBinImport *imp = R_NEW0 (RBinImport);
		imp->name = r_bin_name_new (it->entry_name? it->entry_name: it->name);
		imp->libname = strdup (it->dll_name? it->dll_name: "<unresolved>");
		imp->classname = it->name? strdup (it->name): NULL;
		imp->type = R_BIN_TYPE_FUNC_STR;
		imp->bind = R_BIN_BIND_GLOBAL_STR;
		imp->ordinal = (ut32)i;
		append_import (ret, imp);
	}
	r2unity_free_interop (items, count);
	return true;
}

#if R2UNITY_BIN_VEC_ABI
static bool imports_vec(RBinFile *bf) {
	R_RETURN_VAL_IF_FAIL (bf && bf->bo, false);
	RVecRBinImport_clear (&bf->bo->imports_vec);
	return imports_fill (bf, &bf->bo->imports_vec);
}
#else
static RList *imports(RBinFile *bf) {
	RList *ret = r_list_newf ((RListFree)r_bin_import_free);
	if (!ret || !imports_fill (bf, ret)) {
		r_list_free (ret);
		return NULL;
	}
	return ret;
}
#endif

static RList *libs(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RList *ret = r_list_newf (free);
	size_t count = 0;
	Il2CppAssemblyDefinition *asms = r2unity_get_assemblies (obj->meta, &count);
	for (size_t i = 0; asms && i < count; i++) {
		char *name = get_string (obj, asms[i].aname.name_idx);
		if (name) {
			r_list_append (ret, name);
		}
	}
	R_FREE (asms);
	return ret;
}

static RList *fields(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RList *ret = r_list_newf ((RListFree)r_bin_field_free);
	r_list_append (ret, r_bin_field_new (0, 0, IL2CPP_MAGIC, 4, "metadata.sanity", NULL, "x", false));
	r_list_append (ret, r_bin_field_new (4, 4, obj->meta->version, 4, "metadata.version", NULL, "i", false));
	bool v38 = obj->meta->version >= 38;
	for (int i = 0; i < R2UNITY_METADATA_SECTION_COUNT; i++) {
		Il2CppMetadataSection sec;
		r2unity_metadata_section (obj->meta, (R2UMetadataSectionId)i, &sec);
		if (!sec.size) {
			continue;
		}
		const char *name = r2unity_metadata_section_name ((R2UMetadataSectionId)i);
		ut64 desc = 0;
		if (v38) {
			if (i >= R2UNITY_METADATA_BASE_SECTION_COUNT) {
				continue;
			}
			desc = 8 + (ut64)i * 12;
		} else {
			desc = 8 + (ut64)i * 8;
		}
		char *field = r_str_newf ("metadata.%s.offset", name);
		r_list_append (ret, r_bin_field_new (desc, desc, sec.offset, 4, field, NULL, "x", false));
		free (field);
		field = r_str_newf ("metadata.%s.size", name);
		r_list_append (ret, r_bin_field_new (desc + 4, desc + 4, sec.size, 4, field, NULL, "x", false));
		free (field);
		if (v38) {
			field = r_str_newf ("metadata.%s.count", name);
			r_list_append (ret, r_bin_field_new (desc + 8, desc + 8, sec.count, 4, field, NULL, "i", false));
			free (field);
		}
	}
	return ret;
}

static char *header(RBinFile *bf, int mode) {
	(void)mode;
	R2UnityBinObj *obj = get_obj (bf);
	if (!obj) {
		return NULL;
	}
	RStrBuf *sb = r_strbuf_new ("");
	r_strbuf_appendf (sb, "pf.r2unity_global_metadata_header @ 0x%08"PFMT64x"\n", (ut64)0);
	r_strbuf_appendf (sb, "0x00000000  Sanity          0x%08x\n", IL2CPP_MAGIC);
	r_strbuf_appendf (sb, "0x00000004  Version         %d (%s)\n", obj->meta->version, r2unity_unity_range_from_wire (obj->meta->version));
	r_strbuf_appendf (sb, "0x%08"PFMT64x"  HeaderSize      %"PFMT64u"\n", (ut64)0, r2unity_metadata_header_size (obj->meta));
	for (int i = 0; i < R2UNITY_METADATA_SECTION_COUNT; i++) {
		Il2CppMetadataSection sec;
		r2unity_metadata_section (obj->meta, (R2UMetadataSectionId)i, &sec);
		if (!sec.size) {
			continue;
		}
		const char *name = r2unity_metadata_section_name ((R2UMetadataSectionId)i);
		r_strbuf_appendf (sb, "0x%08x  %-40s size=0x%08x count=%"PFMT64u" entry=%"PFMT64u"\n",
			sec.offset,
			name? name: "unknown",
			sec.size,
			r2unity_metadata_section_count (obj->meta, (R2UMetadataSectionId)i),
			r2unity_metadata_section_entry_size (obj->meta, (R2UMetadataSectionId)i));
	}
	return r_strbuf_drain (sb);
}

RBinPlugin r_bin_plugin_r2unity = {
	.meta = {
		.name = "r2unity",
		.desc = "Unity IL2CPP global-metadata.dat",
		.author = "pancake",
		.license = "MIT",
	},
	.get_sdb = &get_sdb,
	.load = &load,
	.destroy = &destroy,
	.check = &check,
	.baddr = &baddr,
	.size = &size,
	.info = &info,
#if R2UNITY_BIN_VEC_ABI
	.sections_vec = &sections_vec,
	.symbols_vec = &symbols_vec,
	.imports_vec = &imports_vec,
#else
	.sections = &sections,
	.symbols = &symbols,
	.imports = &imports,
#endif
	.strings = &strings,
	.libs = &libs,
	.classes = &classes,
	.fields = &fields,
	.header = &header,
	.minstrlen = 2,
};

#ifndef R2_PLUGIN_INCORE
R_API RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_BIN,
	.data = &r_bin_plugin_r2unity,
	.version = R2_VERSION
};
#endif

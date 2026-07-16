/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.bin"

#include <ctype.h>
#include <r_bin.h>
#include <r_lib.h>
#include "../lib/bgdatabase.h"
#include "../lib/lib.h"
#include "../lib/serialized_file.h"

#if defined(R_LIB_ABIVERSION) && !defined(R2_ABIVERSION)
#define R2_ABIVERSION R_LIB_ABIVERSION
#endif

#if R2_ABIVERSION >= 100
#define R2UNITY_BIN_VEC_ABI 1
#else
#define R2UNITY_BIN_VEC_ABI 0
#endif

#if R2_ABIVERSION >= 104
#define R2UNITY_BIN_STRINGS_VEC_ABI 1
#else
#define R2UNITY_BIN_STRINGS_VEC_ABI 0
#endif

typedef enum {
	R2UNITY_BIN_METADATA,
	R2UNITY_BIN_SERIALIZED_FILE,
	R2UNITY_BIN_BGDATABASE,
} R2UnityBinKind;

typedef struct {
	R2UnityBinKind kind;
	R2UnityMetadata *meta;
	R2UnitySerializedFile *sf;
	R2UnityBGDatabase *bgdb;
	RBuffer *buf;
	Sdb *kv;
	ut8 *strings;
	ut64 strings_size;
} R2UnityBinObj;

static R2UnityBinObj *get_obj(RBinFile *bf) {
	RBinObject *o = bf? bf->bo: NULL;
	return o? (R2UnityBinObj *)o->bin_obj: NULL;
}

static char *bgdatabase_addon_name(const char *type) {
	const char *end = strchr (type, ',');
	if (!end) {
		end = type + strlen (type);
	}
	const char *start = end;
	while (start > type && start[-1] != '.') {
		start--;
	}
	return r_str_ndup (start, (int)(end - start));
}

static char *bgdatabase_addon_assembly(const char *type) {
	const char *comma = strchr (type, ',');
	if (!comma) {
		return strdup (type);
	}
	comma++;
	while (*comma == ' ') {
		comma++;
	}
	const char *end = strchr (comma, ',');
	if (!end) {
		end = comma + strlen (comma);
	}
	return r_str_ndup (comma, (int)(end - comma));
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
	if (r_buf_read_at (b, 0, preamble, sizeof (preamble)) == (st64)sizeof (preamble)
		&& r_read_le32 (preamble) == IL2CPP_MAGIC
		&& valid_wire_version ((int32_t)r_read_le32 (preamble + 4))) {
		return true;
	}
	return r2unity_serialized_file_check (b)
		|| r2unity_bgdatabase_check (b);
}

static void fill_metadata_sdb(R2UnityBinObj *obj) {
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

static void fill_serialized_file_sdb(R2UnityBinObj *obj) {
	Sdb *kv = obj->kv;
	R2UnitySerializedFile *sf = obj->sf;
	sdb_set (kv, "format", "serialized_file", 0);
	sdb_num_set (kv, "version", sf->version, 0);
	sdb_set (kv, "unity_version", sf->unity_version, 0);
	sdb_num_set (kv, "target_platform", (ut64)(st64)sf->target_platform, 0);
	sdb_num_set (kv, "big_endian", sf->big_endian, 0);
	sdb_num_set (kv, "type_tree", sf->enable_type_tree, 0);
	sdb_num_set (kv, "header.size", sf->header_size, 0);
	sdb_num_set (kv, "metadata.offset", sf->header_size, 0);
	sdb_num_set (kv, "metadata.size", sf->metadata_size, 0);
	sdb_num_set (kv, "data.offset", sf->data_offset, 0);
	sdb_num_set (kv, "counts.types", sf->type_count, 0);
	sdb_num_set (kv, "counts.objects", sf->object_count, 0);
	sdb_num_set (kv, "counts.scripts", sf->script_count, 0);
	sdb_num_set (kv, "counts.externals", sf->external_count, 0);
	for (size_t i = 0; i < sf->external_count; i++) {
		char key[128];
		snprintf (key, sizeof (key), "externals.%zu.path", i);
		sdb_set (kv, key, sf->externals[i].path, 0);
	}
	for (size_t i = 0; i < sf->object_count; i++) {
		R2UnitySerializedObject *asset = &sf->objects[i];
		char key[128];
		snprintf (key, sizeof (key), "objects.%zu.path_id", i);
		sdb_num_set (kv, key, (ut64)asset->path_id, 0);
		snprintf (key, sizeof (key), "objects.%zu.class_id", i);
		sdb_num_set (kv, key, (ut64)(st64)asset->class_id, 0);
		snprintf (key, sizeof (key), "objects.%zu.class", i);
		sdb_set (kv, key, r2unity_serialized_class_name (asset->class_id), 0);
		snprintf (key, sizeof (key), "objects.%zu.offset", i);
		sdb_num_set (kv, key, asset->offset, 0);
		snprintf (key, sizeof (key), "objects.%zu.size", i);
		sdb_num_set (kv, key, asset->size, 0);
		if (asset->name) {
			snprintf (key, sizeof (key), "objects.%zu.name", i);
			sdb_set (kv, key, asset->name, 0);
		}
		if (asset->payload_size) {
			snprintf (key, sizeof (key), "objects.%zu.payload.offset", i);
			sdb_num_set (kv, key, asset->payload_offset, 0);
			snprintf (key, sizeof (key), "objects.%zu.payload.size", i);
			sdb_num_set (kv, key, asset->payload_size, 0);
		}
		if (asset->stream_path) {
			snprintf (key, sizeof (key), "objects.%zu.stream.path", i);
			sdb_set (kv, key, asset->stream_path, 0);
			snprintf (key, sizeof (key), "objects.%zu.stream.offset", i);
			sdb_num_set (kv, key, asset->stream_offset, 0);
			snprintf (key, sizeof (key), "objects.%zu.stream.size", i);
			sdb_num_set (kv, key, asset->stream_size, 0);
		}
	}
}

static void fill_bgdatabase_sdb(R2UnityBinObj *obj) {
	Sdb *kv = obj->kv;
	R2UnityBGDatabase *db = obj->bgdb;
	sdb_set (kv, "format", "bgdatabase", 0);
	sdb_num_set (kv, "version", db->version, 0);
	char *id = r2unity_bgdatabase_id_string (db->repository_id);
	sdb_set (kv, "repository.id", id, 0);
	free (id);
	sdb_num_set (kv, "header.value", db->header_value, 0);
	sdb_num_set (kv, "body.offset", db->body_offset, 0);
	sdb_num_set (kv, "counts.addons", db->addon_count, 0);
	sdb_num_set (kv, "counts.tables", db->table_count, 0);
	sdb_num_set (kv, "counts.strings", db->string_count, 0);
	for (size_t i = 0; i < db->addon_count; i++) {
		R2UnityBGDatabaseAddon *addon = &db->addons[i];
		char key[128];
		snprintf (key, sizeof (key), "addons.%zu.type", i);
		sdb_set (kv, key, addon->type, 0);
		snprintf (key, sizeof (key), "addons.%zu.payload.offset", i);
		sdb_num_set (kv, key, addon->payload_offset, 0);
		snprintf (key, sizeof (key), "addons.%zu.payload.size", i);
		sdb_num_set (kv, key, addon->payload_size, 0);
	}
	for (size_t i = 0; i < db->table_count; i++) {
		R2UnityBGDatabaseTable *table = &db->tables[i];
		char key[128];
		snprintf (key, sizeof (key), "tables.%zu.offset", i);
		sdb_num_set (kv, key, table->offset, 0);
		snprintf (key, sizeof (key), "tables.%zu.size", i);
		sdb_num_set (kv, key, table->size, 0);
		snprintf (key, sizeof (key), "tables.%zu.fields", i);
		sdb_num_set (kv, key, table->field_count, 0);
	}
}

static bool load(RBinFile *bf, RBuffer *buf, ut64 loadaddr) {
	(void)loadaddr;
	R_RETURN_VAL_IF_FAIL (bf && bf->bo && buf, false);
	R2UnityBinObj *obj = R_NEW0 (R2UnityBinObj);
	obj->buf = r_buf_new_slice (buf, 0, r_buf_size (buf));
	if (!obj->buf) {
		free (obj);
		return false;
	}
	ut8 preamble[8];
	bool is_metadata = r_buf_read_at (obj->buf, 0, preamble, sizeof (preamble)) == sizeof (preamble)
		&& r_read_le32 (preamble) == IL2CPP_MAGIC
		&& valid_wire_version ((int32_t)r_read_le32 (preamble + 4));
	if (is_metadata) {
		obj->kind = R2UNITY_BIN_METADATA;
		obj->meta = r2unity_parse_metadata (obj->buf);
	} else {
		obj->kind = R2UNITY_BIN_SERIALIZED_FILE;
		obj->sf = r2unity_serialized_file_parse (obj->buf);
		if (!obj->sf) {
			obj->kind = R2UNITY_BIN_BGDATABASE;
			obj->bgdb = r2unity_bgdatabase_parse (obj->buf);
		}
	}
	if (!obj->meta && !obj->sf && !obj->bgdb) {
		r_unref (obj->buf);
		free (obj);
		return false;
	}
	if (obj->kind == R2UNITY_BIN_METADATA) {
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
	}
	obj->kv = sdb_new0 ();
	if (!obj->kv) {
		r2unity_free_metadata (obj->meta);
		r2unity_serialized_file_free (obj->sf);
		r2unity_bgdatabase_free (obj->bgdb);
		r_unref (obj->buf);
		R_FREE (obj->strings);
		free (obj);
		return false;
	}
	if (obj->kind == R2UNITY_BIN_METADATA) {
		fill_metadata_sdb (obj);
	} else if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		fill_serialized_file_sdb (obj);
	} else {
		fill_bgdatabase_sdb (obj);
	}
	bf->bo->bin_obj = obj;
	return true;
}

static void destroy(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	if (!obj) {
		return;
	}
	r2unity_free_metadata (obj->meta);
	r2unity_serialized_file_free (obj->sf);
	r2unity_bgdatabase_free (obj->bgdb);
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
	if (obj->kind == R2UNITY_BIN_METADATA) {
		ret->type = r_str_newf ("Unity IL2CPP global-metadata v%d", obj->meta->version);
		ret->bclass = strdup ("Unity IL2CPP metadata");
		ret->rclass = strdup ("il2cpp");
		ret->machine = strdup ("Unity IL2CPP metadata");
		ret->os = strdup ("any");
		ret->arch = strdup ("cil");
		ret->bits = 32;
		ret->big_endian = 0;
		ret->lang = "csharp";
	} else if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		ret->type = r_str_newf ("Unity SerializedFile v%u (%s)", obj->sf->version, obj->sf->unity_version);
		ret->bclass = strdup ("Unity SerializedFile");
		ret->rclass = strdup ("unity");
		ret->machine = strdup (obj->sf->unity_version);
		ret->os = strdup ("unity");
		ret->subsystem = strdup ("assets");
		ret->bits = 64;
		ret->big_endian = obj->sf->big_endian;
	} else {
		ret->type = r_str_newf ("BGDatabase repository v%u", obj->bgdb->version);
		ret->bclass = strdup ("BGDatabase repository");
		ret->rclass = strdup ("bgdatabase");
		ret->machine = strdup ("BansheeGz BGDatabase");
		ret->os = strdup ("unity");
		ret->subsystem = strdup ("save/database");
		ret->bits = 32;
		ret->big_endian = false;
		ret->lang = "csharp";
	}
	ret->has_va = false;
	ret->has_lit = true;
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

static void init_bgdatabase_section(RBinSection *sec, const char *name,
		ut64 off, ut64 size, bool has_strings) {
	init_section (sec, name, off, size, has_strings);
	if (size >= sizeof (ut32)) {
		sec->format = r_str_newf ("Cd 4[%"PFMT64u"]", size / sizeof (ut32));
	}
}

static char *serialized_object_label(const R2UnitySerializedObject *asset, const char *prefix) {
	const char *class_name = r2unity_serialized_class_name (asset->class_id);
	char *clean = asset->name? r_name_filter_dup (asset->name): NULL;
	char *label;
	if (clean && *clean) {
		label = r_str_newf ("%s.%s.%s", prefix, class_name, clean);
	} else if (asset->class_id && !strcmp (class_name, "Object")) {
		label = r_str_newf ("%s.Class%d.path_%"PFMT64d, prefix, asset->class_id, asset->path_id);
	} else {
		label = r_str_newf ("%s.%s.path_%"PFMT64d, prefix, class_name, asset->path_id);
	}
	free (clean);
	return label;
}

#if R2UNITY_BIN_VEC_ABI
static void append_section(RVecRBinSection *ret, const char *name, ut64 off, ut64 size, bool has_strings) {
	RBinSection *sec = RVecRBinSection_emplace_back (ret);
	if (sec) {
		init_section (sec, name, off, size, has_strings);
	}
}

static void append_bgdatabase_section(RVecRBinSection *ret, const char *name,
		ut64 off, ut64 size, bool has_strings) {
	RBinSection *sec = RVecRBinSection_emplace_back (ret);
	if (sec) {
		init_bgdatabase_section (sec, name, off, size, has_strings);
	}
}

static bool sections_vec(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (bf && bf->bo && obj, false);
	RVecRBinSection *ret = &bf->bo->sections_vec;
	RVecRBinSection_clear (ret);
	if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		R2UnitySerializedFile *sf = obj->sf;
		append_section (ret, "unity.header", 0, sf->header_size, false);
		append_section (ret, "unity.metadata", sf->header_size, sf->metadata_size, true);
		ut64 metadata_end = sf->header_size + sf->metadata_size;
		if (sf->data_offset > metadata_end) {
			append_section (ret, "unity.padding", metadata_end, sf->data_offset - metadata_end, false);
		}
		if (sf->file_size > sf->data_offset) {
			append_section (ret, "unity.data", sf->data_offset, sf->file_size - sf->data_offset, true);
		}
		for (size_t i = 0; i < sf->object_count; i++) {
			R2UnitySerializedObject *asset = &sf->objects[i];
			char *name = serialized_object_label (asset, "unity.object");
			append_section (ret, name, asset->offset, asset->size,
				asset->class_id == 49 || asset->name_size > 0);
			free (name);
		}
		return true;
	}
	if (obj->kind == R2UNITY_BIN_BGDATABASE) {
		R2UnityBGDatabase *db = obj->bgdb;
		append_bgdatabase_section (ret, "bgdb.header", 0,
			R2UNITY_BGDB_HEADER_SIZE, false);
		for (size_t i = 0; i < db->addon_count; i++) {
			R2UnityBGDatabaseAddon *addon = &db->addons[i];
			char *addon_name = bgdatabase_addon_name (addon->type);
			char *name = r_str_newf ("bgdb.addon.%s", addon_name);
			append_bgdatabase_section (ret, name, addon->offset,
				addon->payload_offset + addon->payload_size - addon->offset, true);
			free (name);
			free (addon_name);
		}
		ut64 schema_end = db->table_count? db->tables[0].offset: db->size;
		if (schema_end > db->body_offset) {
			append_bgdatabase_section (ret, "bgdb.schema", db->body_offset,
				schema_end - db->body_offset, true);
		}
		for (size_t i = 0; i < db->table_count; i++) {
			char *name = r_str_newf ("bgdb.table.%zu", i);
			append_bgdatabase_section (ret, name, db->tables[i].offset,
				db->tables[i].size, true);
			free (name);
		}
		return true;
	}
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
	init_section (sec, name, off, size, has_strings);
	return sec;
}

static RBinSection *new_bgdatabase_section(const char *name, ut64 off,
		ut64 size, bool has_strings) {
	RBinSection *sec = R_NEW0 (RBinSection);
	init_bgdatabase_section (sec, name, off, size, has_strings);
	return sec;
}

static RList *sections(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RList *ret = r_list_newf (section_free);
	if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		R2UnitySerializedFile *sf = obj->sf;
		r_list_append (ret, new_section ("unity.header", 0, sf->header_size, false));
		r_list_append (ret, new_section ("unity.metadata", sf->header_size, sf->metadata_size, true));
		ut64 metadata_end = sf->header_size + sf->metadata_size;
		if (sf->data_offset > metadata_end) {
			r_list_append (ret, new_section ("unity.padding", metadata_end, sf->data_offset - metadata_end, false));
		}
		if (sf->file_size > sf->data_offset) {
			r_list_append (ret, new_section ("unity.data", sf->data_offset, sf->file_size - sf->data_offset, true));
		}
		for (size_t i = 0; i < sf->object_count; i++) {
			R2UnitySerializedObject *asset = &sf->objects[i];
			char *name = serialized_object_label (asset, "unity.object");
			r_list_append (ret, new_section (name, asset->offset, asset->size,
				asset->class_id == 49 || asset->name_size > 0));
			free (name);
		}
		return ret;
	}
	if (obj->kind == R2UNITY_BIN_BGDATABASE) {
		R2UnityBGDatabase *db = obj->bgdb;
		r_list_append (ret, new_bgdatabase_section ("bgdb.header", 0,
			R2UNITY_BGDB_HEADER_SIZE, false));
		for (size_t i = 0; i < db->addon_count; i++) {
			R2UnityBGDatabaseAddon *addon = &db->addons[i];
			char *addon_name = bgdatabase_addon_name (addon->type);
			char *name = r_str_newf ("bgdb.addon.%s", addon_name);
			r_list_append (ret, new_bgdatabase_section (name, addon->offset,
				addon->payload_offset + addon->payload_size - addon->offset, true));
			free (name);
			free (addon_name);
		}
		ut64 schema_end = db->table_count? db->tables[0].offset: db->size;
		if (schema_end > db->body_offset) {
			r_list_append (ret, new_bgdatabase_section ("bgdb.schema", db->body_offset,
				schema_end - db->body_offset, true));
		}
		for (size_t i = 0; i < db->table_count; i++) {
			char *name = r_str_newf ("bgdb.table.%zu", i);
			r_list_append (ret, new_bgdatabase_section (name, db->tables[i].offset,
				db->tables[i].size, true));
			free (name);
		}
		return ret;
	}
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
	int ordinal = 0;
	if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		R2UnitySerializedFile *sf = obj->sf;
		for (size_t i = 0; i < sf->object_count; i++) {
			R2UnitySerializedObject *asset = &sf->objects[i];
			char *name = serialized_object_label (asset, "unity");
			RBinSymbol *sym = new_symbol (name, asset->offset, asset->size,
				R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY,
				r2unity_serialized_class_name (asset->class_id), ordinal++);
			append_symbol (ret, sym);
			if (asset->payload_size) {
				char *payload_name = r_str_newf ("resource.%s", name);
				sym = new_symbol (payload_name, asset->payload_offset, asset->payload_size,
					R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY,
					r2unity_serialized_class_name (asset->class_id), ordinal++);
				append_symbol (ret, sym);
				free (payload_name);
			}
			free (name);
		}
		return true;
	}
	if (obj->kind == R2UNITY_BIN_BGDATABASE) {
		R2UnityBGDatabase *db = obj->bgdb;
		append_symbol (ret, new_symbol ("bgdb.repository", 0,
			db->body_offset, R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY,
			NULL, ordinal++));
		for (size_t i = 0; i < db->addon_count; i++) {
			R2UnityBGDatabaseAddon *addon = &db->addons[i];
			char *addon_name = bgdatabase_addon_name (addon->type);
			char *name = r_str_newf ("bgdb.addon.%s", addon_name);
			append_symbol (ret, new_symbol (name, addon->payload_offset,
				addon->payload_size, R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY,
				NULL, ordinal++));
			free (name);
			free (addon_name);
		}
		for (size_t i = 0; i < db->table_count; i++) {
			R2UnityBGDatabaseTable *table = &db->tables[i];
			char *classname = r_str_newf ("BGDatabase.Table_%zu", i);
			char *name = r_str_newf ("bgdb.table.%zu", i);
			append_symbol (ret, new_symbol (name, table->offset, table->size,
				R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY, classname, ordinal++));
			free (name);
			name = r_str_newf ("bgdb.table.%zu.name_field", i);
			append_symbol (ret, new_symbol (name, table->name_field_offset, 30,
				R_BIN_TYPE_OBJECT_STR, R_BIN_ATTR_READONLY, classname, ordinal++));
			free (name);
			free (classname);
		}
		return true;
	}
	R2UnityMetadata *meta = obj->meta;

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
	if (!symbols_fill (bf, ret)) {
		r_list_free (ret);
		return NULL;
	}
	return ret;
}
#endif

static RList *classes(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RList *ret = r_list_newf ((RListFree)r_bin_class_free);
	if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		R2UnitySerializedFile *sf = obj->sf;
		for (size_t i = 0; i < sf->type_count; i++) {
			R2UnitySerializedType *type = &sf->types[i];
			const char *class_name = r2unity_serialized_class_name (type->class_id);
			char *name = NULL;
			if (type->class_id == 114 && type->script_type_index >= 0) {
				name = r_str_newf ("%s.script_%d", class_name, type->script_type_index);
			} else if (!strcmp (class_name, "Object")) {
				name = r_str_newf ("Class%d", type->class_id);
			} else {
				name = strdup (class_name);
			}
			RBinClass *klass = r_bin_class_new (name, NULL, R_BIN_ATTR_NONE);
			if (klass) {
				klass->index = (int)i;
				klass->addr = type->offset;
				klass->lang = R_BIN_LANG_NONE;
				r_list_append (ret, klass);
			}
			free (name);
		}
		return ret;
	}
	if (obj->kind == R2UNITY_BIN_BGDATABASE) {
		for (size_t i = 0; i < obj->bgdb->table_count; i++) {
			char *name = r_str_newf ("BGDatabase.Table_%zu", i);
			RBinClass *klass = r_bin_class_new (name, NULL, R_BIN_ATTR_NONE);
			klass->index = (int)i;
			klass->addr = obj->bgdb->tables[i].offset;
			klass->lang = R_BIN_LANG_NONE;
			r_list_append (ret, klass);
			free (name);
		}
		return ret;
	}
	R2UnityMetadata *meta = obj->meta;

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

#if R2UNITY_BIN_STRINGS_VEC_ABI
typedef RVecRBinString R2UnityStringSink;

static void append_bin_string(R2UnityStringSink *ret, char *text, ut64 paddr, ut32 size, ut32 ordinal) {
	if (!text) {
		return;
	}
	RBinString *str = RVecRBinString_emplace_back (ret);
	if (!str) {
		free (text);
		return;
	}
	str->string = text;
	str->paddr = paddr;
	str->vaddr = paddr;
	str->size = size;
	str->length = strlen (text);
	str->ordinal = ordinal;
	str->type = R_STRING_TYPE_UTF8;
}
#else
typedef RList R2UnityStringSink;

static void append_bin_string(R2UnityStringSink *ret, char *text, ut64 paddr, ut32 size, ut32 ordinal) {
	if (!text) {
		return;
	}
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
#endif

static void add_metadata_strings(R2UnityBinObj *obj, R2UnityStringSink *ret, ut32 *ordinal) {
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

static void add_literal_strings(R2UnityBinObj *obj, R2UnityStringSink *ret, ut32 *ordinal) {
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

static void add_serialized_strings(R2UnityBinObj *obj, R2UnityStringSink *ret, ut32 *ordinal) {
	R2UnitySerializedFile *sf = obj->sf;
	for (size_t i = 0; i < sf->object_count; i++) {
		R2UnitySerializedObject *asset = &sf->objects[i];
		if (asset->name && asset->name_size > 1 && asset->name_size < R_BIN_SIZEOF_STRINGS) {
			append_bin_string (ret, strdup (asset->name), asset->name_offset,
				asset->name_size, (*ordinal)++);
		}
		if (asset->payload_size > 1 && asset->payload_size < R_BIN_SIZEOF_STRINGS) {
			ut8 *bytes = R_NEWS (ut8, asset->payload_size + 1);
			if (bytes && r_buf_read_at (obj->buf, asset->payload_offset, bytes,
					asset->payload_size) == (st64)asset->payload_size
				&& !memchr (bytes, 0, asset->payload_size)) {
				bytes[asset->payload_size] = 0;
				append_bin_string (ret, (char *)bytes, asset->payload_offset,
					asset->payload_size, (*ordinal)++);
				bytes = NULL;
			}
			free (bytes);
		}
	}
	for (size_t i = 0; i < sf->external_count; i++) {
		R2UnitySerializedExternal *external = &sf->externals[i];
		ut64 length = strlen (external->path);
		if (length > 1 && length < R_BIN_SIZEOF_STRINGS) {
			append_bin_string (ret, strdup (external->path), external->path_offset,
				(ut32)length, (*ordinal)++);
		}
	}
}

static void add_bgdatabase_strings(R2UnityBinObj *obj, R2UnityStringSink *ret,
		ut32 *ordinal) {
	for (size_t i = 0; i < obj->bgdb->string_count; i++) {
		R2UnityBGDatabaseString *string = &obj->bgdb->strings[i];
		if (string->size < R_BIN_SIZEOF_STRINGS) {
			append_bin_string (ret, strdup (string->value), string->offset,
				string->size, (*ordinal)++);
		}
	}
}

#if R2UNITY_BIN_STRINGS_VEC_ABI
static RVecRBinString *strings(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RVecRBinString *ret = RVecRBinString_new ();
	if (!ret) {
		return NULL;
	}
	ut32 ordinal = 0;
	if (obj->kind == R2UNITY_BIN_METADATA) {
		add_metadata_strings (obj, ret, &ordinal);
		add_literal_strings (obj, ret, &ordinal);
	} else if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		add_serialized_strings (obj, ret, &ordinal);
	} else {
		add_bgdatabase_strings (obj, ret, &ordinal);
	}
	return ret;
}
#else
static RList *strings(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, NULL);
	RList *ret = r_list_newf ((RListFree)r_bin_string_free);
	ut32 ordinal = 0;
	if (obj->kind == R2UNITY_BIN_METADATA) {
		add_metadata_strings (obj, ret, &ordinal);
		add_literal_strings (obj, ret, &ordinal);
	} else if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		add_serialized_strings (obj, ret, &ordinal);
	} else {
		add_bgdatabase_strings (obj, ret, &ordinal);
	}
	return ret;
}
#endif

#if R2UNITY_BIN_VEC_ABI
typedef RVecRBinImport R2UnityImportSink;

static void append_import(R2UnityImportSink *ret, RBinImport *imp) {
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
	r_list_append (ret, imp);
}
#endif

static bool imports_fill(RBinFile *bf, R2UnityImportSink *ret) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (obj, false);
	if (obj->kind != R2UNITY_BIN_METADATA) {
		return true;
	}
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
	if (!imports_fill (bf, ret)) {
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
	if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		for (size_t i = 0; i < obj->sf->external_count; i++) {
			r_list_append (ret, strdup (obj->sf->externals[i].path));
		}
		return ret;
	}
	if (obj->kind == R2UNITY_BIN_BGDATABASE) {
		for (size_t i = 0; i < obj->bgdb->addon_count; i++) {
			r_list_append (ret,
				bgdatabase_addon_assembly (obj->bgdb->addons[i].type));
		}
		return ret;
	}
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
	if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		R2UnitySerializedFile *sf = obj->sf;
		r_list_append (ret, r_bin_field_new (8, 8, sf->version, 4,
			"serialized.version", NULL, "i", false));
		r_list_append (ret, r_bin_field_new (16, 16, sf->big_endian, 1,
			"serialized.endian", NULL, "i", false));
		r_list_append (ret, r_bin_field_new (20, 20, sf->metadata_size, 4,
			"serialized.metadata_size", NULL, "x", false));
		r_list_append (ret, r_bin_field_new (24, 24, sf->file_size, 8,
			"serialized.file_size", NULL, "x", false));
		r_list_append (ret, r_bin_field_new (32, 32, sf->data_offset, 8,
			"serialized.data_offset", NULL, "x", false));
		for (size_t i = 0; i < sf->object_count; i++) {
			R2UnitySerializedObject *asset = &sf->objects[i];
			char *name = r_str_newf ("serialized.objects.%zu.path_id", i);
			r_list_append (ret, r_bin_field_new (asset->table_offset,
				asset->table_offset, asset->path_id, 8, name, NULL, "i", false));
			free (name);
			name = r_str_newf ("serialized.objects.%zu.byte_start", i);
			r_list_append (ret, r_bin_field_new (asset->table_offset + 8,
				asset->table_offset + 8, asset->offset - sf->data_offset, 8,
				name, NULL, "x", false));
			free (name);
			name = r_str_newf ("serialized.objects.%zu.byte_size", i);
			r_list_append (ret, r_bin_field_new (asset->table_offset + 16,
				asset->table_offset + 16, asset->size, 4, name, NULL, "x", false));
			free (name);
			name = r_str_newf ("serialized.objects.%zu.type_index", i);
			r_list_append (ret, r_bin_field_new (asset->table_offset + 20,
				asset->table_offset + 20, asset->type_index, 4, name, NULL, "i", false));
			free (name);
		}
		return ret;
	}
	if (obj->kind == R2UNITY_BIN_BGDATABASE) {
		R2UnityBGDatabase *db = obj->bgdb;
		r_list_append (ret, r_bin_field_new (0, 0, db->version, 4,
			"bgdb.version", NULL, "i", false));
		r_list_append (ret, r_bin_field_new (4, 4,
			r_read_le64 (db->repository_id), 8,
			"bgdb.repository_id.low", NULL, "x", false));
		r_list_append (ret, r_bin_field_new (12, 12,
			r_read_le64 (db->repository_id + 8), 8,
			"bgdb.repository_id.high", NULL, "x", false));
		r_list_append (ret, r_bin_field_new (20, 20, db->header_value, 4,
			"bgdb.header_value", NULL, "x", false));
		r_list_append (ret, r_bin_field_new (24, 24, db->addon_count, 4,
			"bgdb.addon_count", NULL, "i", false));
		for (size_t i = 0; i < db->addon_count; i++) {
			R2UnityBGDatabaseAddon *addon = &db->addons[i];
			char *name = r_str_newf ("bgdb.addons.%zu.type_size", i);
			r_list_append (ret, r_bin_field_new (addon->offset, addon->offset,
				addon->type_size, 4, name, NULL, "i", false));
			free (name);
			name = r_str_newf ("bgdb.addons.%zu.payload_size", i);
			r_list_append (ret, r_bin_field_new (addon->payload_size_offset,
				addon->payload_size_offset, addon->payload_size, 4,
				name, NULL, "x", false));
			free (name);
		}
		r_list_append (ret, r_bin_field_new (db->body_offset, db->body_offset,
			db->table_count, 4, "bgdb.table_count", NULL, "i", false));
		for (size_t i = 0; i < db->table_count; i++) {
			R2UnityBGDatabaseTable *table = &db->tables[i];
			char *name = r_str_newf ("bgdb.tables.%zu.field_count", i);
			r_list_append (ret, r_bin_field_new (table->offset, table->offset,
				table->field_count, 4, name, NULL, "i", false));
			free (name);
		}
		return ret;
	}
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
	if (obj->kind == R2UNITY_BIN_SERIALIZED_FILE) {
		R2UnitySerializedFile *sf = obj->sf;
		r_strbuf_appendf (sb, "Unity SerializedFile v%u (%s)\n", sf->version, sf->unity_version);
		r_strbuf_appendf (sb, "0x00000000  HeaderSize      0x%08"PFMT64x"\n", sf->header_size);
		r_strbuf_appendf (sb, "0x00000014  MetadataSize    0x%08x\n", sf->metadata_size);
		r_strbuf_appendf (sb, "0x00000018  FileSize        0x%08"PFMT64x"\n", sf->file_size);
		r_strbuf_appendf (sb, "0x00000020  DataOffset      0x%08"PFMT64x"\n", sf->data_offset);
		r_strbuf_appendf (sb, "              Endian          %s\n", sf->big_endian? "big": "little");
		r_strbuf_appendf (sb, "              TargetPlatform  %d\n", sf->target_platform);
		r_strbuf_appendf (sb, "              TypeTree        %s\n", sf->enable_type_tree? "yes": "no");
		r_strbuf_appendf (sb, "              Types           %zu\n", sf->type_count);
		r_strbuf_appendf (sb, "              Objects         %zu\n", sf->object_count);
		r_strbuf_appendf (sb, "              Scripts         %zu\n", sf->script_count);
		r_strbuf_appendf (sb, "              Externals       %zu\n", sf->external_count);
		for (size_t i = 0; i < sf->object_count; i++) {
			R2UnitySerializedObject *asset = &sf->objects[i];
			r_strbuf_appendf (sb, "0x%08"PFMT64x"  path=%"PFMT64d" %-18s size=0x%08"PFMT64x"%s%s\n",
				asset->offset, asset->path_id,
				r2unity_serialized_class_name (asset->class_id), asset->size,
				asset->name? " name=": "", asset->name? asset->name: "");
			if (asset->stream_path) {
				r_strbuf_appendf (sb, "              stream=%s offset=0x%08"PFMT64x" size=0x%08"PFMT64x"\n",
					asset->stream_path, asset->stream_offset, asset->stream_size);
			}
		}
		return r_strbuf_drain (sb);
	}
	if (obj->kind == R2UNITY_BIN_BGDATABASE) {
		R2UnityBGDatabase *db = obj->bgdb;
		char *id = r2unity_bgdatabase_id_string (db->repository_id);
		r_strbuf_appendf (sb, "BGDatabase repository v%u\n", db->version);
		r_strbuf_appendf (sb, "0x00000004  RepositoryId    %s\n", id);
		r_strbuf_appendf (sb, "0x00000014  HeaderValue     %u\n",
			db->header_value);
		r_strbuf_appendf (sb, "0x00000018  Addons          %zu\n",
			db->addon_count);
		free (id);
		for (size_t i = 0; i < db->addon_count; i++) {
			R2UnityBGDatabaseAddon *addon = &db->addons[i];
			r_strbuf_appendf (sb,
				"0x%08"PFMT64x"  addon=%s payload=0x%08"PFMT64x"+0x%08"PFMT64x"\n",
				addon->offset, addon->type, addon->payload_offset,
				addon->payload_size);
		}
		r_strbuf_appendf (sb, "0x%08"PFMT64x"  Tables          %zu\n",
			db->body_offset, db->table_count);
		for (size_t i = 0; i < db->table_count; i++) {
			R2UnityBGDatabaseTable *table = &db->tables[i];
			r_strbuf_appendf (sb,
				"0x%08"PFMT64x"  table=%zu fields=%u size=0x%08"PFMT64x"\n",
				table->offset, i, table->field_count, table->size);
		}
		return r_strbuf_drain (sb);
	}
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

#if R2_ABIVERSION >= 121
static char *serialized_resource_type(R2UnityBinObj *obj, const R2UnitySerializedObject *asset) {
	if (asset->class_id != 49 || !asset->payload_size) {
		return r_str_newf ("Unity.%s", r2unity_serialized_class_name (asset->class_id));
	}
	if (asset->name && r_str_endswith (asset->name, ".atlas")) {
		return strdup ("text/plain");
	}
	ut8 prefix[64];
	ut64 length = R_MIN (asset->payload_size, sizeof (prefix));
	if (r_buf_read_at (obj->buf, asset->payload_offset, prefix, length) == (st64)length) {
		for (ut64 i = 0; i < length; i++) {
			if (isspace (prefix[i])) {
				continue;
			}
			if (prefix[i] == '{' || prefix[i] == '[') {
				return strdup ("application/json");
			}
			break;
		}
	}
	return strdup ("text/plain");
}

static bool load_resources(RBinFile *bf) {
	R2UnityBinObj *obj = get_obj (bf);
	R_RETURN_VAL_IF_FAIL (bf && bf->bo && obj, false);
	if (obj->kind != R2UNITY_BIN_SERIALIZED_FILE) {
		return true;
	}
	ut32 index = 0;
	for (size_t i = 0; i < obj->sf->object_count; i++) {
		R2UnitySerializedObject *asset = &obj->sf->objects[i];
		if (!asset->name || asset->class_id == 150) {
			continue;
		}
		RBinResource *resource = RVecRBinResource_emplace_back (&bf->bo->resources_vec);
		if (!resource) {
			return false;
		}
		resource->name = strdup (asset->name);
		resource->type = serialized_resource_type (obj, asset);
		if (asset->stream_path) {
			resource->origin = r_str_newf ("%s@0x%08"PFMT64x"+0x%08"PFMT64x,
				asset->stream_path, asset->stream_offset, asset->stream_size);
		} else {
			resource->origin = r_str_newf ("SerializedFile.path_%"PFMT64d, asset->path_id);
		}
		resource->paddr = asset->payload_size? asset->payload_offset: asset->offset;
		resource->vaddr = resource->paddr;
		resource->size = asset->payload_size? asset->payload_size: asset->size;
		resource->id = UT64_MAX;
		resource->index = index++;
		resource->type_id = asset->class_id;
		resource->language_id = UT32_MAX;
		resource->codepage = asset->class_id == 49? 65001: 0;
		resource->named = true;
		if (!resource->name || !resource->type || !resource->origin) {
			return false;
		}
	}
	return true;
}
#endif

RBinPlugin r_bin_plugin_r2unity = {
	.meta = {
		.name = "r2unity",
		.desc = "Unity IL2CPP metadata, SerializedFiles and BGDatabase saves",
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
#if R2_ABIVERSION >= 121
	.load_resources = &load_resources,
#endif
};

#ifndef R2_PLUGIN_INCORE
R_API RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_BIN,
	.data = &r_bin_plugin_r2unity,
	.version = R2_VERSION
};
#endif

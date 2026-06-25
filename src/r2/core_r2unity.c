/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity"

#include <r_core.h>
#include <r_lib.h>
#include "../lib/lib.h"

#define R2UNITY_NATIVE_TARGET_LIMIT 0x10000

// clang-format off
static const char *g_help_msg[] = {
	"Usage:", "r2unity[-subcmd]", " Unity IL2CPP analyzer",
	"r2unity", "", "show this help",
	"r2unity?", "", "show this help",
	"r2unity-c", "[*j]", "enumerate classes, inheritance, methods, and fields",
	"r2unity-D", "", "auto-detect companion files from current binary path",
	"r2unity-i", "[j]", "summary (metadata version, type/method counts)",
	"r2unity-s", "[*j]", "apply/list managed/native symbols as flags + comments",
	"r2unity-z", "[j]", "list managed string literals",
	"r2unity-P", "[*j]", "list P/Invoke (managed -> native)",
	"r2unity-R", "[*j]", "list reverse-P/Invoke (native -> managed)",
	"r2unity-S", "[j]", "emit managed-assembly SBOM (text or JSON)", "Variables:", "", "",
	"r2unity.metadata", "", "path to global-metadata.dat",
	"r2unity.library", "", "path to IL2CPP native library",
	"r2unity.code_registration", "", "Il2CppCodeRegistration VA override/resolved VA",
	"r2unity.metadata_registration", "", "Il2CppMetadataRegistration VA override/resolved VA",
	"r2unity.force_heuristic", "", "force section-scan fallback",
	NULL
};
// clang-format on

static char *method_attrs_str(unsigned flags) {
	enum {
		MemberAccessMask = 0x0007,
		MdPrivate = 0x0001,
		MdFamANDAssem = 0x0002,
		MdAssembly = 0x0003,
		MdFamily = 0x0004,
		MdFamORAssem = 0x0005,
		MdPublic = 0x0006,
		MdStatic = 0x0010,
		MdFinal = 0x0020,
		MdVirtual = 0x0040,
		MdAbstract = 0x0400,
		MdPinvokeImpl = IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL
	};
	static const struct {
		unsigned bit;
		const char *name;
	} bits[] = {
		{ MdStatic, "static" },
		{ MdAbstract, "abstract" },
		{ MdVirtual, "virtual" },
		{ MdFinal, "final" },
		{ MdPinvokeImpl, "extern" },
};
	const char *vis = "";
	switch (flags & MemberAccessMask) {
	case MdPublic: vis = "public"; break;
	case MdFamily: vis = "protected"; break;
	case MdAssembly: vis = "internal"; break;
	case MdFamORAssem: vis = "protected internal"; break;
	case MdFamANDAssem: vis = "private protected"; break;
	case MdPrivate: vis = "private"; break;
	}
	RStrBuf *sb = r_strbuf_new (*vis? vis: "");
	for (size_t i = 0; i < sizeof (bits) / sizeof (bits[0]); i++) {
		if (flags & bits[i].bit) {
			r_strbuf_appendf (sb, "%s%s", r_strbuf_length (sb) > 0? " ": "", bits[i].name);
		}
	}
	return r_strbuf_drain (sb);
}

static const char *current_binary_path(RCore *core) {
	if (core && core->io && core->io->desc && core->io->desc->name) {
		return core->io->desc->name;
	}
	return NULL;
}

static ut64 flag_addr_prefixed(RCore *core, const char *prefix, const char *name) {
	char buf[256];
	int n = snprintf (buf, sizeof (buf), "%s%s", prefix, name);
	if (n > 0 && n < (int)sizeof (buf)) {
		RFlagItem *fi = r_flag_get (core->flags, buf);
		if (fi && fi->addr) {
			return fi->addr;
		}
	}
	if (*name != '_') {
		n = snprintf (buf, sizeof (buf), "%s_%s", prefix, name);
		if (n > 0 && n < (int)sizeof (buf)) {
			RFlagItem *fi = r_flag_get (core->flags, buf);
			return fi? fi->addr: 0;
		}
	}
	return 0;
}

static ut64 flag_addr_native_alias(RCore *core, const char *const *names) {
	static const char *const prefixes[] = {
		"",
		"sym.",
		"obj.",
		NULL
	};
	if (!core || !core->flags || !names) {
		return 0;
	}
	for (size_t i = 0; names[i]; i++) {
		for (size_t j = 0; prefixes[j]; j++) {
			ut64 addr = flag_addr_prefixed (core, prefixes[j], names[i]);
			if (addr) {
				return addr;
			}
		}
	}
	return 0;
}

static bool current_binary_matches_path(RCore *core, const char *path) {
	if (!path || !*path) {
		return true;
	}
	const char *cur = current_binary_path (core);
	if (!cur || !*cur) {
		return false;
	}
	if (!strcmp (cur, path)) {
		return true;
	}
	return !strcmp (r_file_basename (cur), r_file_basename (path));
}

static void native_options_from_core(RCore *core, R2UnityNativeOptions *opts) {
	memset (opts, 0, sizeof (*opts));
	opts->force_heuristic = r_config_get_b (core->config, "r2unity.force_heuristic");
	opts->code_registration_va = r_config_get_i (core->config, "r2unity.code_registration");
	opts->metadata_registration_va = r_config_get_i (core->config, "r2unity.metadata_registration");
	if (!opts->code_registration_va) {
		opts->code_registration_va = flag_addr_native_alias (core, r2unity_native_code_registration_names ());
	}
	if (!opts->metadata_registration_va) {
		opts->metadata_registration_va = flag_addr_native_alias (core, r2unity_native_metadata_registration_names ());
	}
}

static void native_result_to_config(RCore *core, const R2UnityNativeResult *result) {
	if (!core || !result) {
		return;
	}
	if (result->code_registration_va) {
		r_config_set_i (core->config, "r2unity.code_registration", result->code_registration_va);
	}
	if (result->metadata_registration_va) {
		r_config_set_i (core->config, "r2unity.metadata_registration", result->metadata_registration_va);
	}
}

static void pj_native_result(PJ *pj, const R2UnityNativeResult *result) {
	pj_ks (pj, "native_source", result? r2unity_native_source_name (result->source): "none");
	pj_kn (pj, "code_registration", result? result->code_registration_va: 0);
	pj_kn (pj, "metadata_registration", result? result->metadata_registration_va: 0);
	pj_kn (pj, "method_pointers", result? result->method_pointers_va: 0);
	pj_kn (pj, "code_gen_modules", result? result->code_gen_modules_va: 0);
	pj_ko (pj, "native_tables");
	for (R2UnityNativeTableId i = 0; i < R2U_NATIVE_TABLE_COUNT; i++) {
		const R2UnityNativeTable *table = result? &result->tables[i]: NULL;
		if (!table || !table->va) {
			continue;
		}
		pj_ko (pj, r2unity_native_table_name (i));
		pj_kn (pj, "count", table->count);
		pj_kn (pj, "va", table->va);
		pj_end (pj);
	}
	pj_end (pj);
}

static void print_native_result(RCore *core, const R2UnityNativeResult *result) {
	r_cons_printf (core->cons,
		"# native_source=%s code_registration=0x%" PFMT64x " metadata_registration=0x%" PFMT64x " method_pointers=0x%" PFMT64x " code_gen_modules=0x%" PFMT64x "\n",
		result? r2unity_native_source_name (result->source): "none",
		result? result->code_registration_va: 0,
		result? result->metadata_registration_va: 0,
		result? result->method_pointers_va: 0,
		result? result->code_gen_modules_va: 0);
}

static void native_flag(RCore *core, bool print, const char *name, ut64 addr, ut32 size) {
	if (print) {
		r_cons_printf (core->cons, "'@0x%08"PFMT64x"'f %s %u\n", addr, name, size);
	} else {
		r_flag_set (core->flags, name, addr, size);
	}
}

static void apply_native_tables(RCore *core, const R2UnityNativeResult *result, bool print) {
	int ptr_size = result->ptr_size == 4? 4: 8;
	for (R2UnityNativeTableId i = 0; i < R2U_NATIVE_TABLE_COUNT; i++) {
		const R2UnityNativeTable *table = &result->tables[i];
		if (!table->va) {
			continue;
		}
		const char *name = r2unity_native_table_name (i);
		ut32 size = (ut32)R_MIN ((ut64)UT32_MAX, R_MAX ((ut64)1, table->count * ptr_size));
		char flag[128];
		snprintf (flag, sizeof (flag), "r2unity.table.%s", name);
		native_flag (core, print, flag, table->va, size);
		ut64 count = R_MIN (table->count, (ut64)R2UNITY_NATIVE_TARGET_LIMIT);
		if (i >= R2U_NATIVE_TABLE_INTEROP_DATA || !count) {
			continue;
		}
		int len = (int)(count * ptr_size);
		ut8 *buf = malloc (len);
		if (!buf || !r_io_read_at (core->io, table->va, buf, len)) {
			free (buf);
			continue;
		}
		for (ut64 j = 0; j < count; j++) {
			const ut8 *p = buf + j * ptr_size;
			ut64 addr = (ptr_size == 8? r_read_le64 (p): (ut64)r_read_le32 (p)) & ~(ut64)1;
			if (addr <= 0x1000 || !r_io_is_valid_offset (core->io, addr, R_PERM_X)) {
				continue;
			}
			snprintf (flag, sizeof (flag), "sym.r2unity.%s.%" PFMT64u, name, j);
			native_flag (core, print, flag, addr, 1);
			if (print) {
				r_cons_printf (core->cons, "af+ 0x%" PFMT64x " %s\n", addr, flag);
			} else if (core->anal && !r_anal_get_function_at (core->anal, addr)) {
				r_anal_create_function (core->anal, flag, addr, R_ANAL_FCN_TYPE_FCN, NULL);
			}
		}
		free (buf);
	}
}

/* Resolve (and on first use, cache into the eval vars) the metadata path for
 * the current session. Returns a pointer owned by the RConfig, or NULL. */
static const char *resolve_metadata_path(RCore *core) {
	const char *v = r_config_get (core->config, "r2unity.metadata");
	if (R_STR_ISNOTEMPTY (v)) {
		return v;
	}
	const char *bin = current_binary_path (core);
	if (!bin) {
		return NULL;
	}
	R2UnityPaths *p = r2unity_detect_paths (bin);
	if (!p) {
		return NULL;
	}
	if (p->metadata) {
		r_config_set (core->config, "r2unity.metadata", p->metadata);
	}
	if (p->il2cpp_binary) {
		r_config_set (core->config, "r2unity.library", p->il2cpp_binary);
	}
	r2unity_free_paths (p);
	v = r_config_get (core->config, "r2unity.metadata");
	return R_STR_ISNOTEMPTY (v)? v: NULL;
}

static const char *resolve_library_path(RCore *core) {
	const char *v = r_config_get (core->config, "r2unity.library");
	if (R_STR_ISNOTEMPTY (v)) {
		return v;
	}
	const char *bin = current_binary_path (core);
	if (!bin) {
		return NULL;
	}
	R2UnityPaths *p = r2unity_detect_paths (bin);
	if (!p) {
		return NULL;
	}
	if (p->il2cpp_binary) {
		r_config_set (core->config, "r2unity.library", p->il2cpp_binary);
	}
	if (p->metadata) {
		r_config_set (core->config, "r2unity.metadata", p->metadata);
	}
	r2unity_free_paths (p);
	v = r_config_get (core->config, "r2unity.library");
	return R_STR_ISNOTEMPTY (v)? v: NULL;
}

static R2UnityMetadata *open_metadata(RCore *core, RBuffer **out_buf) {
	const char *path = resolve_metadata_path (core);
	if (!path) {
		R_LOG_ERROR ("r2unity.metadata is not set. Run r2unity-D or set it manually");
		return NULL;
	}
	RBuffer *buf = r_buf_new_file (path, O_RDONLY, 0);
	if (!buf) {
		R_LOG_ERROR ("cannot open metadata file: %s", path);
		return NULL;
	}
	R2UnityMetadata *meta = r2unity_parse_metadata (buf);
	if (!meta) {
		R_LOG_ERROR ("failed to parse metadata: %s", path);
		r_unref (buf);
		return NULL;
	}
	*out_buf = buf;
	return meta;
}

static void close_metadata(R2UnityMetadata *meta, RBuffer *buf) {
	if (meta) {
		r2unity_free_metadata (meta);
	}
	if (buf) {
		r_unref (buf);
	}
}

static void pj_string_or_null(PJ *pj, const char *key, const char *value) {
	if (value) {
		pj_ks (pj, key, value);
	} else {
		pj_knull (pj, key);
	}
}

/* ---------- r2unity-D (detect paths) ---------- */
static int cmd_detect(RCore *core, bool as_json) {
	const char *bin = current_binary_path (core);
	if (!bin) {
		R_LOG_ERROR ("no file currently loaded");
		return 1;
	}
	R2UnityPaths *p = r2unity_detect_paths (bin);
	if (!p) {
		if (as_json) {
			PJ *pj = pj_new ();
			pj_o (pj);
			pj_kb (pj, "ok", false);
			pj_ks (pj, "input", bin);
			pj_end (pj);
			r_cons_println (core->cons, pj_string (pj));
			pj_free (pj);
		} else {
			R_LOG_ERROR ("could not detect Unity IL2CPP layout from %s", bin);
		}
		return 1;
	}
	if (p->metadata) {
		r_config_set (core->config, "r2unity.metadata", p->metadata);
	}
	if (p->il2cpp_binary) {
		r_config_set (core->config, "r2unity.library", p->il2cpp_binary);
	}
	if (as_json) {
		PJ *pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_string_or_null (pj, "platform", p->platform);
		pj_string_or_null (pj, "main_executable", p->main_executable);
		pj_string_or_null (pj, "il2cpp_binary", p->il2cpp_binary);
		pj_string_or_null (pj, "metadata", p->metadata);
		pj_string_or_null (pj, "data_dir", p->data_dir);
		pj_end (pj);
		r_cons_println (core->cons, pj_string (pj));
		pj_free (pj);
	} else {
		r_cons_printf (core->cons, "platform:        %s\n", p->platform? p->platform: "-");
		r_cons_printf (core->cons, "main_executable: %s\n", p->main_executable? p->main_executable: "-");
		r_cons_printf (core->cons, "il2cpp_binary:   %s\n", p->il2cpp_binary? p->il2cpp_binary: "-");
		r_cons_printf (core->cons, "metadata:        %s\n", p->metadata? p->metadata: "-");
		r_cons_printf (core->cons, "data_dir:        %s\n", p->data_dir? p->data_dir: "-");
	}
	r2unity_free_paths (p);
	return 0;
}

/* ---------- r2unity-i (summary) ---------- */
static int cmd_info(RCore *core, bool as_json) {
	RBuffer *buf = NULL;
	R2UnityMetadata *meta = open_metadata (core, &buf);
	if (!meta) {
		return 1;
	}
	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);
	size_t img_count = 0;
	Il2CppImageDefinition *images = r2unity_get_images (meta, &img_count);
	size_t asm_count = 0;
	Il2CppAssemblyDefinition *asms = r2unity_get_assemblies (meta, &asm_count);

	if (as_json) {
		PJ *pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_ki (pj, "version", meta->version);
		pj_ks (pj, "unity_range", r2unity_unity_range_from_wire (meta->version));
		pj_kn (pj, "types", (ut64)type_count);
		pj_kn (pj, "methods", (ut64)method_count);
		pj_kn (pj, "images", (ut64)img_count);
		pj_kn (pj, "assemblies", (ut64)asm_count);
		pj_end (pj);
		r_cons_println (core->cons, pj_string (pj));
		pj_free (pj);
	} else {
		r_cons_printf (core->cons, "wire_version: %d (%s)\n", meta->version, r2unity_unity_range_from_wire (meta->version));
		r_cons_printf (core->cons, "types:        %zu\n", type_count);
		r_cons_printf (core->cons, "methods:      %zu\n", method_count);
		r_cons_printf (core->cons, "images:       %zu\n", img_count);
		r_cons_printf (core->cons, "assemblies:   %zu\n", asm_count);
	}

	R_FREE (types);
	R_FREE (methods);
	R_FREE (images);
	R_FREE (asms);
	close_metadata (meta, buf);
	return 0;
}

static bool find_method_pointers(RCore *core, R2UnityMetadata *meta, const char *path, R2UnityNativeResult *result) {
	R2UnityNativeOptions opts = { 0 };
	native_options_from_core (core, &opts);
	bool ok = false;
	if (core && core->bin && r_bin_cur (core->bin) && current_binary_matches_path (core, path)) {
		ok = r2unity_find_method_pointers_rbin (meta, core->bin, r_bin_cur (core->bin), &opts, result);
	}
	if (!ok && path && *path) {
		ok = r2unity_find_method_pointers_simple (meta, path, &opts, result);
	}
	native_result_to_config (core, result);
	return ok;
}

static int type_definition_for_type_index(const Il2CppTypeDefinition *types, size_t type_count, int32_t type_index) {
	if (type_index >= 0) {
		for (size_t i = 0; i < type_count; i++) {
			if (types[i].byvalTypeIndex == type_index) {
				return (int)i;
			}
		}
	}
	return -1;
}

static char *type_name_from_index(R2UnityMetadata *meta, const Il2CppTypeDefinition *types, size_t type_count, int32_t type_index, bool fallback) {
	int idx = type_definition_for_type_index (types, type_count, type_index);
	if (idx >= 0) {
		return r2unity_type_fullname (meta, &types[idx], (size_t)idx, R2U_NAME_FALLBACK_TYPE);
	}
	if (fallback && type_index >= 0) {
		return r_str_newf ("type_index.%d", type_index);
	}
	return NULL;
}

static char *field_name_or_fallback(R2UnityMetadata *meta, const Il2CppFieldDefinition *field, size_t index) {
	char *name = r2unity_get_string (meta, field->nameIndex);
	if (R_STR_ISNOTEMPTY (name)) {
		return name;
	}
	free (name);
	return r_str_newf ("field.%zu", index);
}

static char *method_name_or_fallback(R2UnityMetadata *meta, const Il2CppMethodDefinition *method, size_t index) {
	char *name = r2unity_get_string (meta, method->nameIndex);
	if (R_STR_ISNOTEMPTY (name)) {
		return name;
	}
	free (name);
	return r_str_newf ("method.%zu", index);
}

static char *r2_ic_name(const char *prefix, const char *name, size_t index, bool append_index) {
	char *out = R_STR_ISNOTEMPTY (name)
		? (append_index? r_str_newf ("%s_%zu", name, index): r_str_newf ("%s", name))
		: r_str_newf ("%s_%zu", prefix, index);
	if (out) {
		r_name_filter (out, -1);
	}
	for (char *p = out; p && *p; p++) {
		if (*p == '.' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			*p = '_';
		}
	}
	if (R_STR_ISEMPTY (out)) {
		R_FREE (out);
		return r_str_newf ("%s_%zu", prefix, index);
	}
	return out;
}

static void pj_hex(PJ *pj, const char *key, ut64 value, int width) {
	char hex[64];
	snprintf (hex, sizeof (hex), "0x%0*" PFMT64x, width, value);
	pj_ks (pj, key, hex);
}

static void emit_class_text(RCore *core,
	R2UnityMetadata *meta,
	const Il2CppTypeDefinition *types,
	size_t type_count,
	const Il2CppMethodDefinition *methods,
	size_t method_count,
	const Il2CppFieldDefinition *fields,
	size_t field_count,
	const int32_t *interfaces,
	size_t interface_count,
	size_t type_index) {
	const Il2CppTypeDefinition *td = &types[type_index];
	char *name = r2unity_type_fullname (meta, td, type_index, R2U_NAME_FALLBACK_TYPE);
	char *base = type_name_from_index (meta, types, type_count, td->parentIndex, true);
	r_cons_printf (core->cons, "class %zu %s", type_index, name? name: "-");
	if (base) {
		r_cons_printf (core->cons, " : %s", base);
	}
	if (interfaces && td->interfaces_count > 0 && td->interfacesStart >= 0) {
		bool first = true;
		for (size_t k = 0; k < td->interfaces_count && (size_t)(td->interfacesStart + k) < interface_count; k++) {
			char *iname = type_name_from_index (meta, types, type_count, interfaces[td->interfacesStart + k], true);
			if (iname) {
				r_cons_printf (core->cons, "%s%s", first? " implements ": ", ", iname);
				first = false;
			}
			free (iname);
		}
	}
	r_cons_printf (core->cons, " token=0x%08x fields=%u methods=%u\n", td->token, (unsigned)td->field_count, (unsigned)td->method_count);

	int fstart = td->fieldStart;
	for (int k = 0; fields && k < td->field_count && fstart >= 0 && (size_t)(fstart + k) < field_count; k++) {
		size_t fi = (size_t)(fstart + k);
		char *fname = field_name_or_fallback (meta, &fields[fi], fi);
		char *ftype = type_name_from_index (meta, types, type_count, fields[fi].typeIndex, true);
		r_cons_printf (core->cons, "  field  %zu %s %s token=0x%08x type_index=%d\n",
			fi,
			ftype? ftype: "-",
			fname? fname: "-",
			fields[fi].token,
			fields[fi].typeIndex);
		free (ftype);
		free (fname);
	}

	int mstart = td->methodStart;
	for (int k = 0; methods && k < td->method_count && mstart >= 0 && (size_t)(mstart + k) < method_count; k++) {
		size_t mi = (size_t)(mstart + k);
		char *mname = method_name_or_fallback (meta, &methods[mi], mi);
		char *attrs = method_attrs_str (methods[mi].flags);
		r_cons_printf (core->cons, "  method %zu %s%s%s(%u) token=0x%08x flags=0x%04x\n",
			mi,
			attrs && *attrs? attrs: "",
			attrs && *attrs? " ": "",
			mname? mname: "-",
			(unsigned)methods[mi].parameterCount,
			methods[mi].token,
			methods[mi].flags);
		free (attrs);
		free (mname);
	}
	free (base);
	free (name);
}

static void emit_class_json(PJ *pj,
	R2UnityMetadata *meta,
	const Il2CppTypeDefinition *types,
	size_t type_count,
	const Il2CppMethodDefinition *methods,
	size_t method_count,
	const Il2CppFieldDefinition *fields,
	size_t field_count,
	const int32_t *interfaces,
	size_t interface_count,
	const ut64 *method_ptrs,
	bool has_ptrs,
	size_t type_index) {
	const Il2CppTypeDefinition *td = &types[type_index];
	char *name = r2unity_type_fullname (meta, td, type_index, R2U_NAME_FALLBACK_TYPE);
	char *ns = r2unity_get_string (meta, td->namespaceIndex);
	char *short_name = r2unity_get_string (meta, td->nameIndex);
	char *base = type_name_from_index (meta, types, type_count, td->parentIndex, true);
	pj_o (pj);
	pj_kn (pj, "index", (ut64)type_index);
	pj_string_or_null (pj, "name", name);
	pj_string_or_null (pj, "namespace", ns);
	pj_string_or_null (pj, "short_name", short_name);
	pj_string_or_null (pj, "base", base);
	pj_ki (pj, "base_type_index", td->parentIndex);
	pj_hex (pj, "flags", td->flags, 8);
	pj_hex (pj, "token", td->token, 8);

	pj_ka (pj, "interfaces");
	if (interfaces && td->interfaces_count > 0 && td->interfacesStart >= 0) {
		for (size_t k = 0; k < td->interfaces_count && (size_t)(td->interfacesStart + k) < interface_count; k++) {
			int32_t idx = interfaces[td->interfacesStart + k];
			char *iname = type_name_from_index (meta, types, type_count, idx, true);
			pj_o (pj);
			pj_ki (pj, "type_index", idx);
			pj_string_or_null (pj, "name", iname);
			pj_end (pj);
			free (iname);
		}
	}
	pj_end (pj);

	pj_ka (pj, "fields");
	int fstart = td->fieldStart;
	for (int k = 0; fields && k < td->field_count && fstart >= 0 && (size_t)(fstart + k) < field_count; k++) {
		size_t fi = (size_t)(fstart + k);
		char *fname = field_name_or_fallback (meta, &fields[fi], fi);
		char *ftype = type_name_from_index (meta, types, type_count, fields[fi].typeIndex, true);
		pj_o (pj);
		pj_kn (pj, "index", (ut64)fi);
		pj_string_or_null (pj, "name", fname);
		pj_ki (pj, "type_index", fields[fi].typeIndex);
		pj_string_or_null (pj, "type", ftype);
		pj_hex (pj, "token", fields[fi].token, 8);
		pj_end (pj);
		free (ftype);
		free (fname);
	}
	pj_end (pj);

	pj_ka (pj, "methods");
	int mstart = td->methodStart;
	for (int k = 0; methods && k < td->method_count && mstart >= 0 && (size_t)(mstart + k) < method_count; k++) {
		size_t mi = (size_t)(mstart + k);
		char *mname = method_name_or_fallback (meta, &methods[mi], mi);
		char *fullname = r2unity_method_fullname (meta, &methods[mi], td, type_index, R2U_NAME_WITH_PARAMS | R2U_NAME_FALLBACK_TYPE);
		char *attrs = method_attrs_str (methods[mi].flags);
		ut64 addr = (has_ptrs && method_ptrs)? method_ptrs[mi]: 0;
		pj_o (pj);
		pj_kn (pj, "index", (ut64)mi);
		pj_string_or_null (pj, "name", mname);
		pj_string_or_null (pj, "full_name", fullname);
		pj_kn (pj, "parameter_count", (ut64)methods[mi].parameterCount);
		pj_kn (pj, "addr", addr);
		pj_hex (pj, "token", methods[mi].token, 8);
		pj_hex (pj, "flags", methods[mi].flags, 4);
		pj_hex (pj, "iflags", methods[mi].iflags, 4);
		pj_string_or_null (pj, "attrs", (attrs && *attrs)? attrs: NULL);
		pj_end (pj);
		free (attrs);
		free (fullname);
		free (mname);
	}
	pj_end (pj);
	pj_end (pj);
	free (base);
	free (short_name);
	free (ns);
	free (name);
}

static void emit_class_r2(RCore *core,
	R2UnityMetadata *meta,
	const Il2CppTypeDefinition *types,
	size_t type_count,
	const Il2CppMethodDefinition *methods,
	size_t method_count,
	const Il2CppFieldDefinition *fields,
	size_t field_count,
	const int32_t *interfaces,
	size_t interface_count,
	const ut64 *method_ptrs,
	bool has_ptrs,
	size_t type_index) {
	const Il2CppTypeDefinition *td = &types[type_index];
	char *name = r2unity_type_fullname (meta, td, type_index, R2U_NAME_FALLBACK_TYPE);
	char *r2klass = r2_ic_name ("type", name, type_index, false);
	char *base = type_name_from_index (meta, types, type_count, td->parentIndex, false);
	if (base) {
		r_cons_printf (core->cons, "# class %s : %s\n", name? name: r2klass, base);
	} else {
		r_cons_printf (core->cons, "# class %s\n", name? name: r2klass);
	}
	r_cons_printf (core->cons, "ic+%s @ 0\n", r2klass);
	if (base) {
		char *r2base = r2_ic_name ("type", base, 0, false);
		r_cons_printf (core->cons, "ic+%s:%s @ 0\n", r2klass, r2base);
		free (r2base);
	}
	free (base);
	if (interfaces && td->interfaces_count > 0 && td->interfacesStart >= 0) {
		for (size_t k = 0; k < td->interfaces_count && (size_t)(td->interfacesStart + k) < interface_count; k++) {
			char *iname = type_name_from_index (meta, types, type_count, interfaces[td->interfacesStart + k], false);
			if (iname) {
				char *r2iface = r2_ic_name ("type", iname, 0, false);
				r_cons_printf (core->cons, "ic+%s:%s @ 0\n", r2klass, r2iface);
				free (r2iface);
			}
			free (iname);
		}
	}

	int mstart = td->methodStart;
	for (int k = 0; methods && k < td->method_count && mstart >= 0 && (size_t)(mstart + k) < method_count; k++) {
		size_t mi = (size_t)(mstart + k);
		ut64 addr = (has_ptrs && method_ptrs)? method_ptrs[mi]: 0;
		addr = addr > 0x1000? addr: 0;
		char *mname = method_name_or_fallback (meta, &methods[mi], mi);
		char *r2meth = r2_ic_name ("method", mname, mi, true);
		r_cons_printf (core->cons, "ic+%s.%s @ 0x%" PFMT64x "\n", r2klass, r2meth, addr);
		free (r2meth);
		free (mname);
	}

	int fstart = td->fieldStart;
	for (int k = 0; fields && k < td->field_count && fstart >= 0 && (size_t)(fstart + k) < field_count; k++) {
		size_t fi = (size_t)(fstart + k);
		char *fname = field_name_or_fallback (meta, &fields[fi], fi);
		char *r2field = r2_ic_name ("field", fname, fi, false);
		char *ftype = type_name_from_index (meta, types, type_count, fields[fi].typeIndex, true);
		r_cons_printf (core->cons, "ic+%s..%s %s @ 0\n",
			r2klass,
			r2field,
			ftype? ftype: "unknown");
		free (ftype);
		free (r2field);
		free (fname);
	}
	free (r2klass);
	free (name);
}

static int cmd_classes(RCore *core, char mode) {
	const char *metadata_path = resolve_metadata_path (core);
	RBuffer *buf = NULL;
	R2UnityMetadata *meta = open_metadata (core, &buf);
	if (!meta) {
		return 1;
	}
	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	if (!types) {
		R_LOG_ERROR ("unable to decode type definitions");
		close_metadata (meta, buf);
		return 1;
	}
	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);
	size_t field_count = 0;
	Il2CppFieldDefinition *fields = r2unity_get_field_definitions (meta, &field_count);
	size_t interface_count = 0;
	int32_t *interfaces = r2unity_get_type_index_table (meta, R2U_SEC_INTERFACES, &interface_count);

	R2UnityNativeResult native_result = { 0 };
	ut64 *method_ptrs = NULL;
	bool has_ptrs = false;
	const char *lib = resolve_library_path (core);
	if (lib && *lib) {
		has_ptrs = find_method_pointers (core, meta, lib, &native_result);
		method_ptrs = native_result.method_ptrs;
	}

	if (mode == 'j') {
		PJ *pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_ki (pj, "version", meta->version);
		pj_ks (pj, "unity_range", r2unity_unity_range_from_wire (meta->version));
		pj_kb (pj, "has_ptrs", has_ptrs);
		pj_native_result (pj, &native_result);
		pj_kn (pj, "types", (ut64)type_count);
		pj_kn (pj, "methods", (ut64)method_count);
		pj_kn (pj, "fields", (ut64)field_count);
		pj_ka (pj, "classes");
		for (size_t i = 0; i < type_count; i++) {
			emit_class_json (pj, meta, types, type_count, methods, method_count, fields, field_count, interfaces, interface_count, method_ptrs, has_ptrs, i);
		}
		pj_end (pj);
		pj_end (pj);
		r_cons_println (core->cons, pj_string (pj));
		pj_free (pj);
	} else if (mode == '*') {
		r_cons_println (core->cons, "# r2 script generated by r2unity-c");
		r_cons_printf (core->cons, "# Input file: %s\n", metadata_path && *metadata_path? metadata_path: "-");
		print_native_result (core, &native_result);
		if (!has_ptrs) {
			r_cons_println (core->cons, "# Method addresses default to 0; run r2unity-D or set r2unity.library to recover native addresses.");
		}
		for (size_t i = 0; i < type_count; i++) {
			emit_class_r2 (core, meta, types, type_count, methods, method_count, fields, field_count, interfaces, interface_count, method_ptrs, has_ptrs, i);
		}
	} else {
		r_cons_printf (core->cons, "# classes from %s\n", metadata_path && *metadata_path? metadata_path: "-");
		r_cons_printf (core->cons, "# wire_version=%d (%s) types=%zu methods=%zu fields=%zu\n",
			meta->version,
			r2unity_unity_range_from_wire (meta->version),
			type_count,
			method_count,
			field_count);
		for (size_t i = 0; i < type_count; i++) {
			emit_class_text (core, meta, types, type_count, methods, method_count, fields, field_count, interfaces, interface_count, i);
		}
	}

	r2unity_native_result_fini (&native_result);
	R_FREE (interfaces);
	R_FREE (fields);
	R_FREE (methods);
	R_FREE (types);
	close_metadata (meta, buf);
	return 0;
}

/* ---------- r2unity-s (methods) ---------- */
static int cmd_symbols(RCore *core, char mode) {
	/* mode: 0 = apply to r2 session, '*' = print r2 commands, 'j' = JSON */
	const char *lib = resolve_library_path (core);
	if (!lib) {
		R_LOG_ERROR ("r2unity.library is not set. Run r2unity-D or set it manually");
		return 1;
	}
	RBuffer *buf = NULL;
	R2UnityMetadata *meta = open_metadata (core, &buf);
	if (!meta) {
		return 1;
	}
	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);
	size_t img_count = 0;
	Il2CppImageDefinition *images = r2unity_get_images (meta, &img_count);

	R2UnityNativeResult native_result = { 0 };
	bool has_ptrs = find_method_pointers (core, meta, lib, &native_result);
	ut64 *method_ptrs = native_result.method_ptrs;

	int *type2img = r2unity_build_type_image_map (images, img_count, type_count);

	PJ *pj = NULL;
	if (mode == 'j') {
		pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_ki (pj, "version", meta->version);
		pj_kb (pj, "has_ptrs", has_ptrs);
		pj_native_result (pj, &native_result);
		pj_ka (pj, "methods");
	} else if (mode == '*') {
		print_native_result (core, &native_result);
	}

	if (mode != 'j') {
		apply_native_tables (core, &native_result, mode == '*');
	}
	ut64 applied = 0, listed = 0;
	for (size_t j = 0; j < method_count; j++) {
		Il2CppMethodDefinition *m = &methods[j];
		const Il2CppTypeDefinition *td = NULL;
		size_t type_idx = 0;
		if (m->declaringType >= 0 && (size_t)m->declaringType < type_count) {
			type_idx = (size_t)m->declaringType;
			td = &types[m->declaringType];
		}
		ut64 addr = (has_ptrs && method_ptrs)? method_ptrs[j]: 0;
		const Il2CppImageDefinition *img = NULL;
		int ii = r2unity_image_index_for_method (type2img, type_count, m);
		if (ii >= 0 && (size_t)ii < img_count) {
			img = &images[ii];
		}
		char *fullname = r2unity_method_fullname (meta, m, td, type_idx, R2U_NAME_WITH_PARAMS);
		if (!fullname) {
			continue;
		}
		char *imgname = img? r2unity_get_string (meta, img->nameIndex): NULL;
		char flag_buf[1024];
		if (imgname && *imgname) {
			snprintf (flag_buf, sizeof (flag_buf), "sym.unity.%s.%s", imgname, fullname);
		} else {
			snprintf (flag_buf, sizeof (flag_buf), "sym.unity.%s", fullname);
		}
		r_name_filter (flag_buf, -1);

		char *attrs = method_attrs_str (m->flags);

		if (mode == 'j') {
			pj_o (pj);
			pj_ks (pj, "name", fullname);
			pj_ks (pj, "flag", flag_buf);
			if (imgname) {
				pj_ks (pj, "image", imgname);
			}
			pj_kn (pj, "addr", addr);
			if (*attrs) {
				pj_ks (pj, "attrs", attrs);
			}
			pj_end (pj);
		} else if (mode == '*') {
			if (addr > 0x1000) {
				r_cons_printf (core->cons, "'@0x%" PFMT64x "'f %s\n", addr, flag_buf);
				if (imgname && *imgname) {
					r_cons_printf (core->cons, "'@0x%" PFMT64x "'CCu Method: [%s]%s%s %s\n", addr, imgname, *attrs? " ": "", attrs, fullname);
				} else if (*attrs) {
					r_cons_printf (core->cons, "'@0x%" PFMT64x "'CCu Method: %s %s\n", addr, attrs, fullname);
				} else {
					r_cons_printf (core->cons, "'@0x%" PFMT64x "'CCu Method: %s\n", addr, fullname);
				}
				listed++;
			}
		} else {
			/* apply to r2 session */
			if (addr > 0x1000) {
				r_flag_set (core->flags, flag_buf, addr, 1);
				char *comment = NULL;
				if (imgname && *imgname) {
					comment = r_str_newf ("Method: [%s]%s%s %s",
						imgname,
						*attrs? " ": "",
						attrs,
						fullname);
				} else if (*attrs) {
					comment = r_str_newf ("Method: %s %s", attrs, fullname);
				} else {
					comment = r_str_newf ("Method: %s", fullname);
				}
				if (comment) {
					r_meta_set_string (core->anal, R_META_TYPE_COMMENT, addr, comment);
					free (comment);
				}
				applied++;
			}
		}

		free (attrs);
		free (imgname);
		free (fullname);
	}

	if (mode == 'j') {
		pj_end (pj);
		pj_kn (pj, "applied", applied);
		pj_kn (pj, "listed", listed);
		pj_end (pj);
		r_cons_println (core->cons, pj_string (pj));
		pj_free (pj);
	} else if (mode == 0) {
		R_LOG_INFO ("r2unity: applied %" PFMT64u " method symbols from %s",
			applied,
			lib);
	}

	r2unity_native_result_fini (&native_result);
	R_FREE (type2img);
	R_FREE (images);
	R_FREE (methods);
	R_FREE (types);
	close_metadata (meta, buf);
	return 0;
}

/* ---------- r2unity-z (string literals) ---------- */
static int cmd_strings(RCore *core, char mode) {
	RBuffer *buf = NULL;
	R2UnityMetadata *meta = open_metadata (core, &buf);
	if (!meta) {
		return 1;
	}
	size_t count = 0;
	Il2CppStringLiteral *lits = r2unity_get_string_literals (meta, &count);
	if (!lits) {
		R_LOG_ERROR ("no string literals found");
		close_metadata (meta, buf);
		return 1;
	}
	PJ *pj = NULL;
	if (mode == 'j') {
		pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_kn (pj, "count", (ut64)count);
		pj_ka (pj, "strings");
	} else {
		r_cons_printf (core->cons, "# managed string literals (count=%zu)\n", count);
		r_cons_printf (core->cons, "# idx\tdata_off\tlen\ttext\n");
	}
	for (size_t i = 0; i < count; i++) {
		ut8 *bytes = NULL;
		size_t len = 0;
		ut32 data_off = meta->stringLiteralDataOffset + lits[i].dataIndex;
		if (!r2unity_read_string_literal (meta, &lits[i], &bytes, &len)) {
			if (mode != 'j') {
				r_cons_printf (core->cons, "%zu\t0x%x\t%u\t<invalid>\n", i, data_off, lits[i].length);
			}
			continue;
		}
		if (mode == 'j') {
			pj_o (pj);
			pj_kn (pj, "idx", (ut64)i);
			pj_kn (pj, "data_off", (ut64)data_off);
			pj_kn (pj, "len", (ut64)lits[i].length);
			char *text = r_str_ndup ((const char *)bytes, (int)len);
			if (text) {
				pj_ks (pj, "text", text);
				free (text);
			} else {
				pj_knull (pj, "text");
			}
			pj_end (pj);
		} else {
			char *text = r_str_ndup ((const char *)bytes, (int)len);
			char *escaped = text? r_str_escape (text): NULL;
			r_cons_printf (core->cons, "%zu\t0x%x\t%u\t\"%s\"\n", i, data_off, lits[i].length, escaped? escaped: "");
			free (escaped);
			free (text);
		}
		R_FREE (bytes);
	}
	if (mode == 'j') {
		pj_end (pj);
		pj_end (pj);
		r_cons_println (core->cons, pj_string (pj));
		pj_free (pj);
	}
	R_FREE (lits);
	close_metadata (meta, buf);
	return 0;
}

/* ---------- r2unity-P / r2unity-R (interop) ---------- */
static int interop_cmp(const void *a, const void *b) {
	const R2UnityInterop *x = (const R2UnityInterop *)a;
	const R2UnityInterop *y = (const R2UnityInterop *)b;
	const char *xi = x->image_name? x->image_name: "";
	const char *yi = y->image_name? y->image_name: "";
	int c = strcmp (xi, yi);
	if (c) {
		return c;
	}
	const char *xn = x->name? x->name: "";
	const char *yn = y->name? y->name: "";
	return strcmp (xn, yn);
}

static const char *interop_kind_label(uint8_t kind) {
	switch (kind) {
	case R2U_INTEROP_REVERSE_PINVOKE: return "MonoPInvokeCallback";
	case R2U_INTEROP_UNMANAGED_ONLY: return "UnmanagedCallersOnly";
	default: return "reverse";
	}
}

static int cmd_interop(RCore *core, bool reverse, char mode) {
	RBuffer *buf = NULL;
	R2UnityMetadata *meta = open_metadata (core, &buf);
	if (!meta) {
		return 1;
	}
	size_t n = 0;
	R2UnityInterop *items = reverse
		? r2unity_enumerate_reverse_pinvokes (meta, &n)
		: r2unity_enumerate_pinvokes (meta, &n);
	if (items && n > 1) {
		qsort (items, n, sizeof (R2UnityInterop), interop_cmp);
	}

	PJ *pj = NULL;
	if (mode == 'j') {
		pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_ki (pj, "version", meta->version);
		pj_ka (pj, reverse? "reverse_pinvokes": "pinvokes");
	} else if (mode == 0 && !reverse) {
		r_cons_printf (core->cons, "# P/Invoke methods (managed -> native), wire version %d\n", meta->version);
		r_cons_printf (core->cons, "# IMAGE\tMETHOD\tDLL\tENTRY\tFLAGS\tCONFIDENCE\n");
	} else if (mode == 0 && reverse) {
		r_cons_printf (core->cons, "# Reverse-P/Invoke methods (native -> managed), wire version %d\n", meta->version);
		r_cons_printf (core->cons, "# IMAGE\tMETHOD\tATTRIBUTE\tFLAGS\tCONFIDENCE\n");
	}

	for (size_t i = 0; i < n; i++) {
		R2UnityInterop *it = &items[i];
		char *attrs = method_attrs_str (it->flags);
		if (mode == 'j') {
			pj_o (pj);
			pj_ks (pj, "image", it->image_name? it->image_name: "");
			pj_ks (pj, "method", it->name? it->name: "");
			pj_kn (pj, "token", (ut64)it->token);
			pj_kn (pj, "flags", (ut64)it->flags);
			pj_kn (pj, "iflags", (ut64)it->iflags);
			pj_kn (pj, "confidence", (ut64)it->confidence);
			if (reverse) {
				pj_ks (pj, "attribute", interop_kind_label (it->kind));
			} else {
				if (it->dll_name) {
					pj_ks (pj, "dll", it->dll_name);
				} else {
					pj_knull (pj, "dll");
				}
				if (it->entry_name) {
					pj_ks (pj, "entry", it->entry_name);
				} else {
					pj_knull (pj, "entry");
				}
			}
			pj_end (pj);
		} else if (mode == '*') {
			if (it->name) {
				char flag_buf[1024];
				const char *prefix = reverse? "sym.unity.reverse": "sym.unity";
				if (it->image_name && *it->image_name) {
					snprintf (flag_buf, sizeof (flag_buf), "%s.%s.%s", prefix, it->image_name, it->name);
				} else {
					snprintf (flag_buf, sizeof (flag_buf), "%s.%s", prefix, it->name);
				}
				r_name_filter (flag_buf, -1);
				if (reverse) {
					r_cons_printf (core->cons, "# ReversePInvoke %s [%s]\n", flag_buf, interop_kind_label (it->kind));
				} else if (it->dll_name) {
					r_cons_printf (core->cons, "# PInvoke %s -> %s!%s\n", flag_buf, it->dll_name, it->entry_name? it->entry_name: it->name);
				} else {
					r_cons_printf (core->cons, "# PInvoke %s -> <unresolved>\n", flag_buf);
				}
			}
		} else {
			if (reverse) {
				r_cons_printf (core->cons, "%s\t%s\t%s\t%s\t%u\n", it->image_name? it->image_name: "", it->name? it->name: "", interop_kind_label (it->kind), *attrs? attrs: "-", (unsigned)it->confidence);
			} else {
				r_cons_printf (core->cons, "%s\t%s\t%s\t%s\t%s\t%u\n", it->image_name? it->image_name: "", it->name? it->name: "", it->dll_name? it->dll_name: "<unresolved>", it->entry_name? it->entry_name: "<default>", *attrs? attrs: "-", (unsigned)it->confidence);
			}
		}
		free (attrs);
	}

	if (mode == 'j') {
		pj_end (pj);
		pj_kn (pj, "count", (ut64)n);
		pj_end (pj);
		r_cons_println (core->cons, pj_string (pj));
		pj_free (pj);
	} else if (mode == 0) {
		r_cons_printf (core->cons, "# summary: %s=%zu\n", reverse? "reverse_pinvokes": "pinvokes", n);
	}
	r2unity_free_interop (items, n);
	close_metadata (meta, buf);
	return 0;
}

/* ---------- r2unity-S (SBOM) ---------- */
static int cmd_sbom(RCore *core, char mode) {
	const char *metadata_path = resolve_metadata_path (core);
	const char *exe_path = current_binary_path (core);
	RBuffer *buf = NULL;
	R2UnityMetadata *meta = open_metadata (core, &buf);
	if (!meta) {
		return 1;
	}
	char *out = r2unity_sbom_tostring (meta, exe_path, metadata_path, mode == 'j'? R2U_SBOM_JSON: R2U_SBOM_TEXT);
	if (!out) {
		R_LOG_ERROR ("unable to decode assemblies table for wire version %d", meta->version);
		close_metadata (meta, buf);
		return 1;
	}
	r_cons_printf (core->cons, "%s", out);
	free (out);
	close_metadata (meta, buf);
	return 0;
}

/* ---------- dispatcher ---------- */
static bool r2unity_call(RCorePluginSession *cps, const char *input) {
	if (!input || !r_str_startswith (input, "r2unity")) {
		return false;
	}
	RCore *core = cps->core;
	const char *rest = input + strlen ("r2unity");

	/* "r2unity" alone, or "r2unity?" */
	if (*rest == 0 || *rest == '?') {
		r_core_cmd_help (core, g_help_msg);
		return true;
	}
	if (*rest != '-') {
		return false;
	}
	rest++;
	/* "r2unity-" followed by the subcommand letter, optionally a modifier
	 *('j' for JSON, '*' for r2 commands), optionally whitespace + args. */
	char sub = *rest;
	char mode = 0;
	if (sub) {
		rest++;
		if (*rest == 'j' || *rest == '*') {
			mode = *rest;
			rest++;
		}
		if (*rest && *rest != ' ' && *rest != '\t') {
			r_core_cmd_help (core, g_help_msg);
			return true;
		}
	}
	while (*rest == ' ' || *rest == '\t') {
		rest++;
	}

	switch (sub) {
	case 'c':
		return cmd_classes (core, mode) == 0;
	case 'D':
		return cmd_detect (core, mode == 'j') == 0;
	case 'i':
		return cmd_info (core, mode == 'j') == 0;
	case 's':
		return cmd_symbols (core, mode) == 0;
	case 'z':
		return cmd_strings (core, mode) == 0;
	case 'P':
		return cmd_interop (core, false, mode) == 0;
	case 'R':
		return cmd_interop (core, true, mode) == 0;
	case 'S':
		return cmd_sbom (core, mode) == 0;
	case '?':
	case 'h':
		r_core_cmd_help (core, g_help_msg);
		return true;
	default:
		r_core_cmd_help (core, g_help_msg);
		return true;
	}
}

static bool r2unity_init(RCorePluginSession *cps) {
	RCore *core = cps->core;
	RConfig *cfg = core->config;
	r_config_lock (cfg, false);
	r_config_node_desc (
		r_config_set (cfg, "r2unity.metadata", ""),
			"path to global-metadata.dat (empty = auto-detect)");
	r_config_node_desc (
		r_config_set (cfg, "r2unity.library", ""),
			"path to IL2CPP native library (empty = auto-detect)");
	r_config_node_desc (
		r_config_set (cfg, "r2unity.code_registration", ""),
			"Il2CppCodeRegistration VA override (empty = flags/RBin symbols)");
	r_config_node_desc (
		r_config_set (cfg, "r2unity.metadata_registration", ""),
			"Il2CppMetadataRegistration VA override (empty = flags/RBin symbols)");
	r_config_node_desc (
		r_config_set_b (cfg, "r2unity.force_heuristic", false),
			"force section-scan fallback instead of CodeRegistration parsing");
	r_config_lock (cfg, true);
	return true;
}

static bool r2unity_fini(RCorePluginSession *cps) {
	(void)cps;
	return true;
}

RCorePlugin r_core_plugin_r2unity = {
	.meta = {
		.name = "r2unity",
		.desc = "Unity IL2CPP analyzer (global-metadata.dat)",
		.author = "pancake",
		.license = "MIT",
	},
	.init = r2unity_init,
	.fini = r2unity_fini,
	.call = r2unity_call,
};

#ifndef R2_PLUGIN_INCORE
R_API RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_CORE,
	.data = &r_core_plugin_r2unity,
	.version = R2_VERSION
};
#endif

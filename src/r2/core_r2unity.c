#define R_LOG_ORIGIN "r2unity"

#include <r_core.h>
#include <r_lib.h>
#include "../lib/lib.h"

// clang-format off
static const char *g_help_msg[] = {
	"Usage:", "r2unity[-subcmd]", " Unity IL2CPP analyzer",
	"r2unity", "", "show this help",
	"r2unity?", "", "show this help",
	"r2unity-D", "", "auto-detect companion files from current binary path",
	"r2unity-i", "[j]", "summary (metadata version, type/method counts)",
	"r2unity-s", "[*j]", "apply/list managed method symbols as flags + comments",
	"r2unity-z", "[j]", "list managed string literals",
	"r2unity-P", "[*j]", "list P/Invoke (managed -> native)",
	"r2unity-R", "[*j]", "list reverse-P/Invoke (native -> managed)",
	"r2unity-S", "", "emit CycloneDX SBOM (JSON)", "Variables:", "", "",
	"r2unity.metadata", "", "path to global-metadata.dat",
	"r2unity.library", "", "path to IL2CPP native library",
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

static const char *unity_range_from_wire(int wire) {
	switch (wire) {
	case 21: return "5.3.0-5.3.5";
	case 22: return "5.3.6-5.4";
	case 23: return "5.5";
	case 24: return "5.6-2020.1";
	case 27: return "2020.2-2021.3";
	case 29: return "2022.1-2022.3";
	case 31: return "2023.x-6000.x";
	default: return "unknown";
	}
}

static const char *current_binary_path(RCore *core) {
	if (core && core->io && core->io->desc && core->io->desc->name) {
		return core->io->desc->name;
	}
	return NULL;
}

static const char *cfg_get_nonempty(RConfig *cfg, const char *key) {
	const char *v = r_config_get (cfg, key);
	return (v && *v)? v: NULL;
}

/* Resolve (and on first use, cache into the eval vars) the metadata path for
 * the current session. Returns a pointer owned by the RConfig, or NULL. */
static const char *resolve_metadata_path(RCore *core) {
	const char *v = cfg_get_nonempty (core->config, "r2unity.metadata");
	if (v) {
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
	return cfg_get_nonempty (core->config, "r2unity.metadata");
}

static const char *resolve_library_path(RCore *core) {
	const char *v = cfg_get_nonempty (core->config, "r2unity.library");
	if (v) {
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
	return cfg_get_nonempty (core->config, "r2unity.library");
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

static void json_escape_cons(PJ *pj, const char *key, const char *value) {
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
		json_escape_cons (pj, "platform", p->platform);
		json_escape_cons (pj, "main_executable", p->main_executable);
		json_escape_cons (pj, "il2cpp_binary", p->il2cpp_binary);
		json_escape_cons (pj, "metadata", p->metadata);
		json_escape_cons (pj, "data_dir", p->data_dir);
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
		pj_ks (pj, "unity_range", unity_range_from_wire (meta->version));
		pj_kn (pj, "types", (ut64)type_count);
		pj_kn (pj, "methods", (ut64)method_count);
		pj_kn (pj, "images", (ut64)img_count);
		pj_kn (pj, "assemblies", (ut64)asm_count);
		pj_end (pj);
		r_cons_println (core->cons, pj_string (pj));
		pj_free (pj);
	} else {
		r_cons_printf (core->cons, "wire_version: %d (%s)\n", meta->version, unity_range_from_wire (meta->version));
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

/* Sniff the exe magic and dispatch to the matching fast finder. */
static bool find_method_pointers(R2UnityMetadata *meta, const char *path, ut64 **out_ptrs) {
	ut8 magic[4] = { 0 };
	FILE *fp = fopen (path, "rb");
	if (fp) {
		(void)fread (magic, 1, 4, fp);
		fclose (fp);
	}
	if (!memcmp (magic, "\x7f"
		"ELF",
		4)) {
		return r2unity_find_method_pointers_elf (meta, path, out_ptrs);
	}
	ut32 m = r_read_le32 (magic);
	if (m == 0xfeedfacf || m == 0xcffaedfe || m == 0xcafebabe || m == 0xbebafeca) {
		return r2unity_find_method_pointers_macho (meta, path, out_ptrs);
	}
	if (magic[0] == 'M' && magic[1] == 'Z') {
		return r2unity_find_method_pointers_pe (meta, path, out_ptrs);
	}
	return false;
}

/* Build the fully-qualified method name "Ns.Class.Method(argc)". Caller owns. */
static char *build_method_fullname(R2UnityMetadata *meta,
	const Il2CppMethodDefinition *m,
	const Il2CppTypeDefinition *td) {
	char *mn = r2unity_get_string (meta, m->nameIndex);
	if (!mn) {
		return NULL;
	}
	char *ns = td? r2unity_get_string (meta, td->namespaceIndex): NULL;
	char *tn = td? r2unity_get_string (meta, td->nameIndex): NULL;
	unsigned pc = (unsigned)m->parameterCount;
	char *out = NULL;
	if (ns && *ns) {
		out = r_str_newf ("%s.%s.%s(%u)", ns, tn? tn: "", mn, pc);
	} else if (tn && *tn) {
		out = r_str_newf ("%s.%s(%u)", tn, mn, pc);
	} else {
		out = r_str_newf ("%s(%u)", mn, pc);
	}
	free (ns);
	free (tn);
	free (mn);
	return out;
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

	ut64 *method_ptrs = NULL;
	bool has_ptrs = find_method_pointers (meta, lib, &method_ptrs);
	if (method_ptrs && !has_ptrs) {
		for (size_t k = 0; k < method_count; k++) {
			if (method_ptrs[k]) {
				has_ptrs = true;
				break;
			}
		}
	}

	int *type2img = NULL;
	if (images && type_count > 0) {
		type2img = R_NEWS (int, type_count);
		for (size_t ti = 0; ti < type_count; ti++) {
			type2img[ti] = -1;
		}
		for (size_t ii = 0; ii < img_count; ii++) {
			int start = images[ii].typeStart;
			int count = (int)images[ii].typeCount;
			for (int k = 0; k < count && start >= 0 && (size_t) (start + k) < type_count; k++) {
				type2img[start + k] = (int)ii;
			}
		}
	}

	PJ *pj = NULL;
	if (mode == 'j') {
		pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_ki (pj, "version", meta->version);
		pj_kb (pj, "has_ptrs", has_ptrs);
		pj_ka (pj, "methods");
	}

	ut64 applied = 0, listed = 0;
	for (size_t j = 0; j < method_count; j++) {
		Il2CppMethodDefinition *m = &methods[j];
		const Il2CppTypeDefinition *td = NULL;
		if (m->declaringType >= 0 && (size_t)m->declaringType < type_count) {
			td = &types[m->declaringType];
		}
		ut64 addr = (has_ptrs && method_ptrs)? method_ptrs[j]: 0;
		const Il2CppImageDefinition *img = NULL;
		if (type2img && td) {
			int ii = type2img[m->declaringType];
			if (ii >= 0 && (size_t)ii < img_count) {
				img = &images[ii];
			}
		}
		char *fullname = build_method_fullname (meta, m, td);
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

	R_FREE (method_ptrs);
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

/* ---------- r2unity-S (SBOM JSON) ---------- */
static int cmd_sbom(RCore *core) {
	const char *metadata_path = resolve_metadata_path (core);
	const char *exe_path = current_binary_path (core);
	RBuffer *buf = NULL;
	R2UnityMetadata *meta = open_metadata (core, &buf);
	if (!meta) {
		return 1;
	}
	size_t img_count = 0;
	Il2CppImageDefinition *imgs = r2unity_get_images (meta, &img_count);
	size_t asm_count = 0;
	Il2CppAssemblyDefinition *asms = r2unity_get_assemblies (meta, &asm_count);
	size_t ref_count = 0;
	int32_t *refs = r2unity_get_referenced_assemblies (meta, &ref_count);
	if (!asms || !asm_count) {
		R_LOG_ERROR ("unable to decode assemblies table for wire version %d",
			meta->version);
		R_FREE (imgs);
		R_FREE (asms);
		R_FREE (refs);
		close_metadata (meta, buf);
		return 1;
	}

	PJ *pj = pj_new ();
	pj_o (pj);
	pj_ks (pj, "bomFormat", "CycloneDX");
	pj_ks (pj, "specVersion", "1.5");
	pj_ki (pj, "version", 1);
	pj_ko (pj, "metadata");
	pj_ko (pj, "tools");
	pj_ka (pj, "components");
	pj_o (pj);
	pj_ks (pj, "type", "application");
	pj_ks (pj, "name", "r2unity");
	pj_end (pj);
	pj_end (pj);
	pj_end (pj);
	pj_ko (pj, "component");
	pj_ks (pj, "type", "application");
	pj_ks (pj, "name", exe_path? exe_path: "unity-build");
	pj_ks (pj, "version", unity_range_from_wire (meta->version));
	pj_ka (pj, "properties");
	pj_o (pj);
	pj_ks (pj, "name", "unity.metadata.path");
	pj_ks (pj, "value", metadata_path? metadata_path: "");
	pj_end (pj);
	pj_o (pj);
	pj_ks (pj, "name", "unity.metadata.wire_version");
	char wv[16];
	snprintf (wv, sizeof (wv), "%d", meta->version);
	pj_ks (pj, "value", wv);
	pj_end (pj);
	pj_end (pj);
	pj_end (pj);
	pj_end (pj);

	pj_ka (pj, "components");
	for (size_t i = 0; i < asm_count; i++) {
		Il2CppAssemblyDefinition *a = &asms[i];
		char *name = r2unity_get_string (meta, a->aname.name_idx);
		char *culture = r2unity_get_string (meta, a->aname.culture_idx);
		const char *nm = name? name: "";
		const char *cl = (culture && *culture)? culture: "neutral";
		char *img_name_owned = NULL;
		const char *img = "";
		if (imgs && a->image_index >= 0 && (size_t)a->image_index < img_count) {
			img_name_owned = r2unity_get_string (meta, imgs[a->image_index].nameIndex);
			if (img_name_owned) {
				img = img_name_owned;
			}
		}
		char ver[64];
		snprintf (ver, sizeof (ver), "%d.%d.%d.%d", a->aname.major, a->aname.minor, a->aname.build, a->aname.revision);
		char bomref[512];
		snprintf (bomref, sizeof (bomref), "asm:%s:%s", nm, ver);
		char purl[512];
		snprintf (purl, sizeof (purl), "pkg:generic/unity/%s@%s", nm, ver);

		pj_o (pj);
		pj_ks (pj, "bom-ref", bomref);
		pj_ks (pj, "type", "library");
		pj_ks (pj, "name", nm);
		pj_ks (pj, "version", ver);
		pj_ks (pj, "purl", purl);
		pj_ka (pj, "properties");
		pj_o (pj);
		pj_ks (pj, "name", "dotnet.culture");
		pj_ks (pj, "value", cl);
		pj_end (pj);
		pj_o (pj);
		pj_ks (pj, "name", "il2cpp.image");
		pj_ks (pj, "value", img);
		pj_end (pj);
		pj_end (pj);
		pj_end (pj);
		free (name);
		free (culture);
		free (img_name_owned);
	}
	pj_end (pj);

	pj_ka (pj, "dependencies");
	for (size_t i = 0; i < asm_count; i++) {
		Il2CppAssemblyDefinition *a = &asms[i];
		if (a->referenced_count <= 0 || a->referenced_start < 0) {
			continue;
		}
		if ((size_t) (a->referenced_start + a->referenced_count) > ref_count) {
			continue;
		}
		char *name = r2unity_get_string (meta, a->aname.name_idx);
		const char *nm = name? name: "";
		char ver[64];
		snprintf (ver, sizeof (ver), "%d.%d.%d.%d", a->aname.major, a->aname.minor, a->aname.build, a->aname.revision);
		char bomref[512];
		snprintf (bomref, sizeof (bomref), "asm:%s:%s", nm, ver);
		pj_o (pj);
		pj_ks (pj, "ref", bomref);
		pj_ka (pj, "dependsOn");
		for (int k = 0; k < a->referenced_count; k++) {
			int32_t ridx = refs[a->referenced_start + k];
			if (ridx < 0 || (size_t)ridx >= asm_count) {
				continue;
			}
			Il2CppAssemblyDefinition *r = &asms[ridx];
			char *rname = r2unity_get_string (meta, r->aname.name_idx);
			char rver[64];
			snprintf (rver, sizeof (rver), "%d.%d.%d.%d", r->aname.major, r->aname.minor, r->aname.build, r->aname.revision);
			char rref[512];
			snprintf (rref, sizeof (rref), "asm:%s:%s", rname? rname: "", rver);
			pj_s (pj, rref);
			free (rname);
		}
		pj_end (pj);
		pj_end (pj);
		free (name);
	}
	pj_end (pj);

	pj_end (pj);
	r_cons_println (core->cons, pj_string (pj));
	pj_free (pj);

	R_FREE (imgs);
	R_FREE (asms);
	R_FREE (refs);
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
	}
	while (*rest == ' ' || *rest == '\t') {
		rest++;
	}

	switch (sub) {
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
		return cmd_sbom (core) == 0;
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

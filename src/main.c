/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <r2unity_config.h>
#include "lib/lib.h"

typedef struct {
	R2UnitySymbolOverride *items;
	size_t count;
} CliSymbolOverrides;

typedef struct {
	bool as_json;
	bool as_r2;
	bool fast;
	bool quiet;
	long limit;
	const R2UnityNativeOptions *native;
} CliEmitOptions;

/* System.Reflection.MethodAttributes subset. Returns an owned string
 *(possibly empty) describing the visibility/flags for a managed method. */
static char *method_attrs(unsigned flags) {
	enum {
		MemberAccessMask = 0x0007,
		MdPrivate = 0x0001,
		MdFamANDAssem = 0x0002, /* private protected */
		MdAssembly = 0x0003, /* internal */
		MdFamily = 0x0004, /* protected */
		MdFamORAssem = 0x0005, /* protected internal */
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

static void print_usage(FILE *out, const char *prog_name) {
	fprintf (out,
		"Usage: %s [options] <executable> <global-metadata.dat>\n"
		"\n"
		"Options:\n"
		"  -a 0xADDR     Read the method pointer table starting at virtual address 0xADDR\n"
		"  -c            Enumerate classes, inheritance, methods, and fields\n"
		"  -D            Detect companion files from the given executable path and exit\n"
		"  -f            Recover native method pointers through symbols/sections\n"
		"  -H            Force section-scan fallback instead of CodeRegistration\n"
		"  -h            Show this help and exit\n"
		"  -j            One-line JSON status, or JSON output with -c/-P/-R/-S\n"
		"  -l N          Limit emitted entries to N\n"
		"  -O N=A        Override a native symbol address, e.g. g_CodeRegistration=0x1234\n"
		"  -P            Enumerate P/Invoke (managed -> native) methods\n"
		"  -q            Quiet mode: omit banner and informational comments\n"
		"  -r            Emit r2 script commands; pairs with -c/-P/-R\n"
		"  -R            Enumerate reverse-P/Invoke (native -> managed) methods (v29+)\n"
		"  -S            Emit a text SBOM of the managed assemblies\n"
		"  -V            Verbose debug tracing on stderr\n"
		"  -v            Show version and exit\n"
		"  -z            Enumerate managed string literals (`ldstr`) from metadata\n"
		"\n"
		"Arguments:\n"
		"  <executable>           Native IL2CPP binary for the target platform\n"
		"  <global-metadata.dat>  Unity IL2CPP metadata file\n"
		"                         -c and -z also accept metadata-only input\n"
		"\n"
		"Expected files per target platform:\n"
		"  iOS build:          UnityFramework      + global-metadata.dat\n"
		"                      metadata: ../../Data/Managed/Metadata/global-metadata.dat\n"
		"  macOS standalone:   GameAssembly.dylib  + global-metadata.dat\n"
		"                      metadata: ../Resources/Data/il2cpp_data/Metadata/global-metadata.dat\n"
		"  Windows standalone: GameAssembly.dll    + global-metadata.dat\n"
		"                      metadata: *_Data/il2cpp_data/Metadata/global-metadata.dat\n"
		"  Android build:      libil2cpp.so        + global-metadata.dat\n"
		"                      metadata: ../../assets/bin/Data/Managed/Metadata/global-metadata.dat\n"
		"  Linux standalone:   GameAssembly.so     + global-metadata.dat\n"
		"                      metadata: *_Data/il2cpp_data/Metadata/global-metadata.dat\n",
		prog_name);
}

static bool add_symbol_override(CliSymbolOverrides *overrides, const char *arg) {
	if (!overrides || !arg) {
		return false;
	}
	const char *eq = strchr (arg, '=');
	if (!eq || eq == arg || !eq[1]) {
		return false;
	}
	R2UnitySymbolOverride *items = realloc (overrides->items, (overrides->count + 1) * sizeof (*items));
	if (!items) {
		return false;
	}
	overrides->items = items;
	char *name = r_str_ndup (arg, (int)(eq - arg));
	if (!name) {
		return false;
	}
	overrides->items[overrides->count].name = name;
	overrides->items[overrides->count].va = (ut64)strtoull (eq + 1, NULL, 0);
	overrides->count++;
	return true;
}

static void free_symbol_overrides(CliSymbolOverrides *overrides) {
	if (!overrides) {
		return;
	}
	for (size_t i = 0; i < overrides->count; i++) {
		free ((char *)overrides->items[i].name);
	}
	R_FREE (overrides->items);
	overrides->count = 0;
}

static void pj_native_result(PJ *pj, const R2UnityNativeResult *native) {
	pj_ks (pj, "native_source", native? r2unity_native_source_name (native->source): "none");
	pj_kn (pj, "code_registration", native? native->code_registration_va: 0);
	pj_kn (pj, "metadata_registration", native? native->metadata_registration_va: 0);
	pj_kn (pj, "method_pointers", native? native->method_pointers_va: 0);
	pj_kn (pj, "code_gen_modules", native? native->code_gen_modules_va: 0);
}

static void print_native_comment(const R2UnityNativeResult *native) {
	printf ("# native_source=%s code_registration=0x%" PFMT64x " metadata_registration=0x%" PFMT64x " method_pointers=0x%" PFMT64x " code_gen_modules=0x%" PFMT64x "\n",
		native? r2unity_native_source_name (native->source): "none",
		native? native->code_registration_va: 0,
		native? native->metadata_registration_va: 0,
		native? native->method_pointers_va: 0,
		native? native->code_gen_modules_va: 0);
}

static void json_escape(FILE *f, const char *s) {
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '"': fputs ("\\\"", f); break;
		case '\\': fputs ("\\\\", f); break;
		case '\b': fputs ("\\b", f); break;
		case '\f': fputs ("\\f", f); break;
		case '\n': fputs ("\\n", f); break;
		case '\r': fputs ("\\r", f); break;
		case '\t': fputs ("\\t", f); break;
		default:
			if (c < 0x20) {
				fprintf (f, "\\u%04x", c);
			} else {
				fputc (c, f);
			}
		}
	}
}

static void print_escaped_literal(FILE *f, const ut8 *buf, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned char c = buf[i];
		if (c < 0x80) {
			switch (c) {
			case '\\': fputs ("\\\\", f); break;
			case '"': fputs ("\\\"", f); break;
			case '\n': fputs ("\\n", f); break;
			case '\r': fputs ("\\r", f); break;
			case '\t': fputs ("\\t", f); break;
			default:
				if (c >= 0x20 && c < 0x7f) {
					fputc (c, f);
				} else {
					fprintf (f, "\\x%02x", c);
				}
				break;
			}
			i++;
			continue;
		}
		RRune ch = 0;
		int n = r_utf8_decode (buf + i, (int) (len - i), &ch);
		if (n > 0) {
			fwrite (buf + i, 1, n, f);
			i += n;
		} else {
			fprintf (f, "\\x%02x", c);
			i++;
		}
	}
}

static int emit_string_literals(R2UnityMetadata *meta, const char *metadata_path, bool quiet, long limit) {
	size_t count = 0;
	Il2CppStringLiteral *lits = r2unity_get_string_literals (meta, &count);
	if (!lits) {
		R_LOG_ERROR ("no string literals found in %s", metadata_path);
		return 1;
	}
	if (!quiet) {
		printf ("# managed string literals from %s\n", metadata_path);
		printf ("# idx\tdata_off\tlen\ttext\n");
	}
	size_t max = count;
	if (limit >= 0 && (size_t)limit < max) {
		max = (size_t)limit;
	}
	for (size_t i = 0; i < max; i++) {
		ut8 *bytes = NULL;
		size_t len = 0;
		if (!r2unity_read_string_literal (meta, &lits[i], &bytes, &len)) {
			printf ("%zu\t0x%x\t%u\t<invalid>\n",
				i,
				meta->stringLiteralDataOffset + lits[i].dataIndex,
				lits[i].length);
			continue;
		}
		printf ("%zu\t0x%x\t%u\t\"", i, meta->stringLiteralDataOffset + lits[i].dataIndex, lits[i].length);
		print_escaped_literal (stdout, bytes, len);
		printf ("\"\n");
		R_FREE (bytes);
	}
	R_FREE (lits);
	return 0;
}

/* Cross-entry sort for stable output: sort by image_name, then name. */
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

static int emit_pinvokes(R2UnityMetadata *meta, const char *exe_path, bool is_json, bool is_r2, bool quiet) {
	(void)exe_path;
	size_t n = 0;
	R2UnityInterop *items = r2unity_enumerate_pinvokes (meta, &n);
	if (items && n > 1) {
		qsort (items, n, sizeof (R2UnityInterop), interop_cmp);
	}

	if (is_json) {
		FILE *f = stdout;
		fprintf (f, "{\"ok\":true,\"version\":%d,\"pinvokes\":[", meta->version);
		for (size_t i = 0; i < n; i++) {
			R2UnityInterop *it = &items[i];
			if (i) {
				fputc (',', f);
			}
			fprintf (f, "{\"image\":\"");
			json_escape (f, it->image_name? it->image_name: "");
			fprintf (f, "\",\"method\":\"");
			json_escape (f, it->name? it->name: "");
			fprintf (f, "\",\"token\":\"0x%08x\",\"flags\":\"0x%04x\",\"iflags\":\"0x%04x\",\"confidence\":%u", it->token, it->flags, it->iflags, (unsigned)it->confidence);
			if (it->dll_name) {
				fprintf (f, ",\"dll\":\"");
				json_escape (f, it->dll_name);
				fprintf (f, "\"");
			} else {
				fprintf (f, ",\"dll\":null");
			}
			if (it->entry_name) {
				fprintf (f, ",\"entry\":\"");
				json_escape (f, it->entry_name);
				fprintf (f, "\"");
			} else {
				fprintf (f, ",\"entry\":null");
			}
			fputc ('}', f);
		}
		fprintf (f, "],\"reverse_pinvokes\":[],\"anonymous_reverse_pinvokes\":[],\"interop_data\":null}\n");
	} else if (is_r2) {
		for (size_t i = 0; i < n; i++) {
			R2UnityInterop *it = &items[i];
			if (!it->name) {
				continue;
			}
			char buf[1024];
			if (it->image_name && *it->image_name) {
				snprintf (buf, sizeof (buf), "sym.unity.%s.%s", it->image_name, it->name);
			} else {
				snprintf (buf, sizeof (buf), "sym.unity.%s", it->name);
			}
			r_name_filter (buf, -1);
			if (it->dll_name) {
				printf ("# PInvoke %s -> %s!%s\n", buf, it->dll_name, it->entry_name? it->entry_name: it->name);
			} else {
				printf ("# PInvoke %s -> <unresolved>\n", buf);
			}
		}
	} else {
		if (!quiet) {
			printf ("# P/Invoke methods (managed -> native), metadata wire version %d\n", meta->version);
			printf ("# IMAGE\tMETHOD\tDLL\tENTRY\tFLAGS\tCONFIDENCE\n");
		}
		for (size_t i = 0; i < n; i++) {
			R2UnityInterop *it = &items[i];
			char *attrs = method_attrs (it->flags);
			printf ("%s\t%s\t%s\t%s\t%s\t%u\n",
				it->image_name? it->image_name: "",
				it->name? it->name: "",
				it->dll_name? it->dll_name: "<unresolved>",
				it->entry_name? it->entry_name: "<default>",
				*attrs? attrs: "-",
				(unsigned)it->confidence);
			free (attrs);
		}
		if (!quiet) {
			printf ("# summary: pinvokes=%zu\n", n);
		}
	}

	r2unity_free_interop (items, n);
	return 0;
}

static const char *interop_kind_label(uint8_t kind) {
	switch (kind) {
	case R2U_INTEROP_REVERSE_PINVOKE: return "MonoPInvokeCallback";
	case R2U_INTEROP_UNMANAGED_ONLY: return "UnmanagedCallersOnly";
	default: return "reverse";
	}
}

static int emit_reverse_pinvokes(R2UnityMetadata *meta, const char *exe_path, bool is_json, bool is_r2, bool quiet) {
	(void)exe_path;
	size_t n = 0;
	R2UnityInterop *items = r2unity_enumerate_reverse_pinvokes (meta, &n);
	if (items && n > 1) {
		qsort (items, n, sizeof (R2UnityInterop), interop_cmp);
	}

	if (is_json) {
		FILE *f = stdout;
		fprintf (f, "{\"ok\":true,\"version\":%d,\"reverse_pinvokes\":[", meta->version);
		for (size_t i = 0; i < n; i++) {
			R2UnityInterop *it = &items[i];
			if (i) {
				fputc (',', f);
			}
			fprintf (f, "{\"image\":\"");
			json_escape (f, it->image_name? it->image_name: "");
			fprintf (f, "\",\"method\":\"");
			json_escape (f, it->name? it->name: "");
			fprintf (f, "\",\"token\":\"0x%08x\",\"flags\":\"0x%04x\",\"iflags\":\"0x%04x\",\"attribute\":\"%s\",\"confidence\":%u", it->token, it->flags, it->iflags, interop_kind_label (it->kind), (unsigned)it->confidence);
			fputc ('}', f);
		}
		fprintf (f, "]}\n");
	} else if (is_r2) {
		for (size_t i = 0; i < n; i++) {
			R2UnityInterop *it = &items[i];
			if (!it->name) {
				continue;
			}
			char buf[1024];
			if (it->image_name && *it->image_name) {
				snprintf (buf, sizeof (buf), "sym.unity.reverse.%s.%s", it->image_name, it->name);
			} else {
				snprintf (buf, sizeof (buf), "sym.unity.reverse.%s", it->name);
			}
			r_name_filter (buf, -1);
			printf ("# ReversePInvoke %s [%s]\n", buf, interop_kind_label (it->kind));
		}
	} else {
		if (!quiet) {
			printf ("# Reverse-P/Invoke methods (native -> managed), metadata wire version %d\n", meta->version);
			printf ("# IMAGE\tMETHOD\tATTRIBUTE\tFLAGS\tCONFIDENCE\n");
		}
		for (size_t i = 0; i < n; i++) {
			R2UnityInterop *it = &items[i];
			char *attrs = method_attrs (it->flags);
			printf ("%s\t%s\t%s\t%s\t%u\n",
				it->image_name? it->image_name: "",
				it->name? it->name: "",
				interop_kind_label (it->kind),
				*attrs? attrs: "-",
				(unsigned)it->confidence);
			free (attrs);
		}
		if (!quiet) {
			printf ("# summary: reverse_pinvokes=%zu\n", n);
		}
	}

	r2unity_free_interop (items, n);
	return 0;
}

static void json_kv_str(FILE *f, const char *key, const char *value) {
	fprintf (f, "\"%s\":", key);
	if (value) {
		fputc ('"', f);
		json_escape (f, value);
		fputc ('"', f);
	} else {
		fputs ("null", f);
	}
}

static int emit_detected_paths(const char *input, bool as_json) {
	R2UnityPaths *p = r2unity_detect_paths (input);
	if (!p) {
		if (as_json) {
			printf ("{\"ok\":false,\"input\":\"");
			json_escape (stdout, input);
			printf ("\"}\n");
		} else {
			R_LOG_ERROR ("could not detect Unity IL2CPP layout from %s", input);
		}
		return 1;
	}
	if (as_json) {
		FILE *f = stdout;
		fputs ("{\"ok\":true,", f);
		json_kv_str (f, "platform", p->platform);
		fputc (',', f);
		json_kv_str (f, "main_executable", p->main_executable);
		fputc (',', f);
		json_kv_str (f, "il2cpp_binary", p->il2cpp_binary);
		fputc (',', f);
		json_kv_str (f, "metadata", p->metadata);
		fputc (',', f);
		json_kv_str (f, "data_dir", p->data_dir);
		fputs ("}\n", f);
	} else {
		static const struct {
			const char *label;
			size_t offset;
		} rows[] = {
			{ "platform:        ", offsetof (R2UnityPaths, platform) },
			{ "main_executable: ", offsetof (R2UnityPaths, main_executable) },
			{ "il2cpp_binary:   ", offsetof (R2UnityPaths, il2cpp_binary) },
			{ "metadata:        ", offsetof (R2UnityPaths, metadata) },
			{ "data_dir:        ", offsetof (R2UnityPaths, data_dir) },
};
		for (size_t i = 0; i < sizeof (rows) / sizeof (rows[0]); i++) {
			const char *v = *(char **) ((char *)p + rows[i].offset);
			printf ("%s%s\n", rows[i].label, v? v: "-");
		}
	}
	r2unity_free_paths (p);
	return 0;
}

/* Emit one method as r2 script commands. Methods without a plausible native
 * address (addr <= 0x1000) are skipped entirely and do not count against the
 * emit limit. Returns true when output was produced. */
static bool emit_r2_method(R2UnityMetadata *meta,
	const Il2CppMethodDefinition *m,
	const Il2CppTypeDefinition *td,
	size_t type_idx,
	const Il2CppImageDefinition *img,
	bool have_img_map,
	ut64 addr) {
	if (addr <= 0x1000) {
		return false;
	}
	char *fullname = r2unity_method_fullname (meta, m, td, type_idx, R2U_NAME_WITH_PARAMS);
	if (!fullname) {
		return false;
	}
	/* No image map context: fall back to a plain comment. */
	if (!have_img_map || !td) {
		printf ("# %s\n", fullname);
		free (fullname);
		return true;
	}
	r_name_filter (fullname, -1);
	char *attrs = method_attrs (m->flags);
	char *im = img? r2unity_get_string (meta, img->nameIndex): NULL;
	if (im) {
		r_name_filter (im, -1);
	}
	if (im && *im) {
		printf ("'@0x%" PFMT64x "'f sym.unity.%s.%s\n", addr, im, fullname);
		printf ("'@0x%" PFMT64x "'CCu Method: [%s]%s%s %s\n",
			addr,
			im,
			*attrs? " ": "",
			attrs,
			fullname);
	} else if (img) {
		printf ("'@0x%" PFMT64x "'f sym.unity.%s\n", addr, fullname);
		printf ("'@0x%" PFMT64x "'CCu Method:%s%s %s\n",
			addr,
			*attrs? " ": "",
			attrs,
			fullname);
	} else {
		printf ("'@0x%" PFMT64x "'f sym.unity.%s\n", addr, fullname);
		if (*attrs) {
			printf ("'@0x%" PFMT64x "'CCu Method: %s\n", addr, attrs);
		}
	}
	free (im);
	free (attrs);
	free (fullname);
	return true;
}

static int type_definition_for_type_index(const Il2CppTypeDefinition *types, size_t type_count, int32_t type_index) {
	if (type_index < 0) {
		return -1;
	}
	for (size_t i = 0; i < type_count; i++) {
		if (types[i].byvalTypeIndex == type_index) {
			return (int)i;
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
	if (name && *name) {
		return name;
	}
	free (name);
	return r_str_newf ("field.%zu", index);
}

static char *method_name_or_fallback(R2UnityMetadata *meta, const Il2CppMethodDefinition *method, size_t index) {
	char *name = r2unity_get_string (meta, method->nameIndex);
	if (name && *name) {
		return name;
	}
	free (name);
	return r_str_newf ("method.%zu", index);
}

static char *r2_ic_name(const char *prefix, const char *name, size_t index) {
	char *out = R_STR_ISNOTEMPTY (name)? r_str_newf ("%s", name): r_str_newf ("%s_%zu", prefix, index);
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

static void pj_string_or_null(PJ *pj, const char *key, const char *value) {
	if (value) {
		pj_ks (pj, key, value);
	} else {
		pj_knull (pj, key);
	}
}

static void emit_class_text(R2UnityMetadata *meta,
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
	printf ("class %zu %s", type_index, name? name: "-");
	if (base) {
		printf (" : %s", base);
	}
	if (td->interfaces_count > 0 && td->interfacesStart >= 0) {
		bool first = true;
		for (size_t k = 0; k < td->interfaces_count && (size_t)(td->interfacesStart + k) < interface_count; k++) {
			char *iname = type_name_from_index (meta, types, type_count, interfaces[td->interfacesStart + k], true);
			if (iname) {
				printf ("%s%s", first? " implements ": ", ", iname);
				first = false;
			}
			free (iname);
		}
	}
	printf (" token=0x%08x fields=%u methods=%u\n", td->token, (unsigned)td->field_count, (unsigned)td->method_count);

	int fstart = td->fieldStart;
	for (int k = 0; fields && k < td->field_count && fstart >= 0 && (size_t)(fstart + k) < field_count; k++) {
		size_t fi = (size_t)(fstart + k);
		char *fname = field_name_or_fallback (meta, &fields[fi], fi);
		char *ftype = type_name_from_index (meta, types, type_count, fields[fi].typeIndex, true);
		printf ("  field  %zu %s %s token=0x%08x type_index=%d\n",
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
		char *attrs = method_attrs (methods[mi].flags);
		printf ("  method %zu %s%s%s(%u) token=0x%08x flags=0x%04x\n",
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
	if (td->interfaces_count > 0 && td->interfacesStart >= 0) {
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
		char *attrs = method_attrs (methods[mi].flags);
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

static void emit_class_r2(R2UnityMetadata *meta,
	const Il2CppTypeDefinition *types,
	size_t type_count,
	const Il2CppMethodDefinition *methods,
	size_t method_count,
	const Il2CppFieldDefinition *fields,
	size_t field_count,
	const ut64 *method_ptrs,
	bool has_ptrs,
	size_t type_index,
	bool quiet) {
	const Il2CppTypeDefinition *td = &types[type_index];
	char *name = r2unity_type_fullname (meta, td, type_index, R2U_NAME_FALLBACK_TYPE);
	char *r2klass = r2_ic_name ("type", name, type_index);
	if (!quiet) {
		char *base = type_name_from_index (meta, types, type_count, td->parentIndex, false);
		if (base) {
			printf ("# class %s : %s\n", name? name: r2klass, base);
		} else {
			printf ("# class %s\n", name? name: r2klass);
		}
		free (base);
	}
	printf ("ic+%s @ 0\n", r2klass);

	int mstart = td->methodStart;
	for (int k = 0; methods && k < td->method_count && mstart >= 0 && (size_t)(mstart + k) < method_count; k++) {
		size_t mi = (size_t)(mstart + k);
		ut64 addr = (has_ptrs && method_ptrs)? method_ptrs[mi]: 0;
		if (addr <= 0x1000) {
			continue;
		}
		char *mname = method_name_or_fallback (meta, &methods[mi], mi);
		char *r2meth = r2_ic_name ("method", mname, mi);
		printf ("ic+%s.%s @ 0x%" PFMT64x "\n", r2klass, r2meth, addr);
		free (r2meth);
		free (mname);
	}

	if (!quiet) {
		int fstart = td->fieldStart;
		for (int k = 0; fields && k < td->field_count && fstart >= 0 && (size_t)(fstart + k) < field_count; k++) {
			size_t fi = (size_t)(fstart + k);
			char *fname = field_name_or_fallback (meta, &fields[fi], fi);
			char *ftype = type_name_from_index (meta, types, type_count, fields[fi].typeIndex, true);
			printf ("# field %s.%s %s type_index=%d token=0x%08x\n",
				r2klass,
				fname? fname: "-",
				ftype? ftype: "-",
				fields[fi].typeIndex,
				fields[fi].token);
			free (ftype);
			free (fname);
		}
	}
	free (r2klass);
	free (name);
}

static int emit_classes(R2UnityMetadata *meta, const char *exe_path, const char *metadata_path, const CliEmitOptions *opts) {
	if (opts->as_json && opts->as_r2) {
		R_LOG_ERROR ("-j and -r are mutually exclusive with -c");
		return 1;
	}
	if (opts->fast && (!exe_path || !*exe_path)) {
		R_LOG_ERROR ("-c -f requires both executable and metadata paths");
		return 1;
	}

	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);
	if (!types) {
		R_LOG_ERROR ("unable to decode type definitions");
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
	if (opts->fast) {
		has_ptrs = r2unity_find_method_pointers (meta, exe_path, opts->native, &native_result);
		method_ptrs = native_result.method_ptrs;
	}

	size_t max = type_count;
	if (opts->limit >= 0 && (size_t)opts->limit < max) {
		max = (size_t)opts->limit;
	}

	if (opts->as_json) {
		PJ *pj = pj_new ();
		if (!pj) {
			R_LOG_ERROR ("unable to allocate JSON builder");
			R_FREE (method_ptrs);
			R_FREE (interfaces);
			R_FREE (fields);
			R_FREE (methods);
			R_FREE (types);
			return 1;
		}
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
		for (size_t i = 0; i < max; i++) {
			emit_class_json (pj, meta, types, type_count, methods, method_count, fields, field_count, interfaces, interface_count, method_ptrs, has_ptrs, i);
		}
		pj_end (pj);
		pj_end (pj);
		char *out = pj_drain (pj);
		if (out) {
			puts (out);
			free (out);
		}
	} else if (opts->as_r2) {
		if (!opts->quiet) {
			printf ("# r2 script generated by r2unity -c\n");
			printf ("# Input file: %s\n", metadata_path && *metadata_path? metadata_path: "-");
			print_native_comment (&native_result);
			if (!has_ptrs) {
				printf ("# Method ic+ entries need native addresses; pass -f with an executable to recover them.\n");
			}
		}
		for (size_t i = 0; i < max; i++) {
			emit_class_r2 (meta, types, type_count, methods, method_count, fields, field_count, method_ptrs, has_ptrs, i, opts->quiet);
		}
	} else {
		if (!opts->quiet) {
			printf ("# classes from %s\n", metadata_path && *metadata_path? metadata_path: "-");
			printf ("# wire_version=%d (%s) types=%zu methods=%zu fields=%zu\n",
				meta->version,
				r2unity_unity_range_from_wire (meta->version),
				type_count,
				method_count,
				field_count);
		}
		for (size_t i = 0; i < max; i++) {
			emit_class_text (meta, types, type_count, methods, method_count, fields, field_count, interfaces, interface_count, i);
		}
	}

	r2unity_native_result_fini (&native_result);
	R_FREE (interfaces);
	R_FREE (fields);
	R_FREE (methods);
	R_FREE (types);
	return 0;
}

int main(int argc, char *argv[]) {
	bool json_one_line = false;
	bool r2_script = false;
	bool quiet = false;
	bool debug = false;
	long limit = -1;
	ut64 gmp_addr = 0;
	bool fast = false;
	bool sbom = false;
	bool pinvokes = false;
	bool reverse_pinvokes = false;
	bool string_literals = false;
	bool detect_paths = false;
	bool classes = false;
	R2UnityNativeOptions native_options = { 0 };
	CliSymbolOverrides symbol_overrides = { 0 };
	int opt;
	RGetopt go;
	r_getopt_init (&go, argc, (const char **)argv, "chjrqfHVvSPRDzl:a:O:");
	while ((opt = r_getopt_next (&go)) != -1) {
		switch (opt) {
		case 'c': classes = true; break;
		case 'j': json_one_line = true; break;
		case 'r': r2_script = true; break;
		case 'q': quiet = true; break;
		case 'f': fast = true; break;
		case 'H':
			fast = true;
			native_options.force_heuristic = true;
			break;
		case 'V': debug = true; break;
		case 'v':
			printf ("r2unity %s\n", R2UNITY_VERSION);
			free_symbol_overrides (&symbol_overrides);
			return 0;
		case 'S': sbom = true; break;
		case 'P': pinvokes = true; break;
		case 'R': reverse_pinvokes = true; break;
		case 'D': detect_paths = true; break;
		case 'z': string_literals = true; break;
		case 'l': limit = strtol (go.arg, NULL, 0); break;
		case 'a': gmp_addr = (ut64)strtoull (go.arg, NULL, 0); break;
		case 'O':
			if (!add_symbol_override (&symbol_overrides, go.arg)) {
				R_LOG_ERROR ("invalid -O argument; expected name=addr");
				free_symbol_overrides (&symbol_overrides);
				return 1;
			}
			fast = true;
			break;
		case 'h':
			print_usage (stdout, argv[0]);
			free_symbol_overrides (&symbol_overrides);
			return 0;
		default:
			print_usage (stderr, argv[0]);
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
	}
	native_options.symbols = symbol_overrides.items;
	native_options.symbols_count = symbol_overrides.count;
	if (pinvokes && reverse_pinvokes) {
		R_LOG_ERROR ("-P and -R are mutually exclusive");
		free_symbol_overrides (&symbol_overrides);
		return 1;
	}
	if (detect_paths) {
		if (argc - go.ind != 1) {
			print_usage (stderr, argv[0]);
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
		int rc = emit_detected_paths (argv[go.ind], json_one_line);
		free_symbol_overrides (&symbol_overrides);
		return rc;
	}
	if (classes) {
		if (sbom || pinvokes || reverse_pinvokes || string_literals) {
			R_LOG_ERROR ("-c cannot be combined with -S, -P, -R or -z");
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
		if (argc - go.ind != 1 && argc - go.ind != 2) {
			print_usage (stderr, argv[0]);
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
		if (fast && argc - go.ind != 2) {
			R_LOG_ERROR ("-c -f requires both executable and metadata paths");
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
	} else if (string_literals) {
		if (json_one_line || fast || gmp_addr || sbom || pinvokes || reverse_pinvokes) {
			R_LOG_ERROR ("-z cannot be combined with -j, -f, -a, -S, -P or -R");
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
		if (argc - go.ind != 1 && argc - go.ind != 2) {
			print_usage (stderr, argv[0]);
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
	} else if (argc - go.ind != 2) {
		print_usage (stderr, argv[0]);
		free_symbol_overrides (&symbol_overrides);
		return 1;
	}

	const char *exe_path = NULL;
	const char *metadata_path = NULL;
	if ((string_literals || classes) && argc - go.ind == 1) {
		exe_path = "";
		metadata_path = argv[go.ind];
	} else {
		exe_path = argv[go.ind];
		metadata_path = argv[go.ind + 1];
	}

	RBuffer *buf = r_buf_new_file (metadata_path, O_RDONLY, 0);
	if (!buf) {
		perror ("Error opening file");
		free_symbol_overrides (&symbol_overrides);
		return 1;
	}

	if (debug) {
		r_log_set_level (R_LOG_LEVEL_DEBUG);
	}
	R2UnityMetadata *meta = r2unity_parse_metadata (buf);
	if (!meta) {
		R_LOG_ERROR ("Failed to parse metadata");
		r_unref (buf);
		free_symbol_overrides (&symbol_overrides);
		return 1;
	}

	if (sbom) {
		char *out = r2unity_sbom_tostring (meta, exe_path, metadata_path, json_one_line? R2U_SBOM_JSON: R2U_SBOM_TEXT);
		int rc = out? 0: 1;
		if (out) {
			fputs (out, stdout);
			free (out);
		} else {
			R_LOG_ERROR ("unable to decode assemblies table for wire version %d", meta->version);
		}
		r2unity_free_metadata (meta);
		r_unref (buf);
		free_symbol_overrides (&symbol_overrides);
		return rc;
	}

	if (pinvokes || reverse_pinvokes) {
		if (json_one_line && r2_script) {
			R_LOG_ERROR ("-j and -r are mutually exclusive");
			r2unity_free_metadata (meta);
			r_unref (buf);
			free_symbol_overrides (&symbol_overrides);
			return 1;
		}
		int rc = reverse_pinvokes
			? emit_reverse_pinvokes (meta, exe_path, json_one_line, r2_script, quiet)
			: emit_pinvokes (meta, exe_path, json_one_line, r2_script, quiet);
		r2unity_free_metadata (meta);
		r_unref (buf);
		free_symbol_overrides (&symbol_overrides);
		return rc;
	}

	if (classes) {
		CliEmitOptions emit_opts = {
			.as_json = json_one_line,
			.as_r2 = r2_script,
			.fast = fast,
			.quiet = quiet,
			.limit = limit,
			.native = &native_options
		};
		int rc = emit_classes (meta, exe_path, metadata_path, &emit_opts);
		r2unity_free_metadata (meta);
		r_unref (buf);
		free_symbol_overrides (&symbol_overrides);
		return rc;
	}

	if (string_literals) {
		int rc = emit_string_literals (meta, metadata_path, quiet, limit);
		r2unity_free_metadata (meta);
		r_unref (buf);
		free_symbol_overrides (&symbol_overrides);
		return rc;
	}

	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);

	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);

	R2UnityNativeResult native_result = { 0 };
	ut64 *method_ptrs = NULL;
	bool has_ptrs = false;
	if (gmp_addr) {
		R_LOG_WARN ("Manual method-pointer table reading (-a) is not implemented");
	} else if (fast) {
		has_ptrs = r2unity_find_method_pointers (meta, exe_path, &native_options, &native_result);
		method_ptrs = native_result.method_ptrs;
	}

	if (json_one_line) {
		PJ *pj = pj_new ();
		pj_o (pj);
		pj_kb (pj, "ok", true);
		pj_ki (pj, "version", meta->version);
		pj_kn (pj, "types", (ut64)type_count);
		pj_kn (pj, "methods", (ut64)method_count);
		pj_kb (pj, "has_ptrs", has_ptrs);
		pj_native_result (pj, &native_result);
		pj_end (pj);
		char *out = pj_drain (pj);
		if (out) {
			puts (out);
			free (out);
		}
		r2unity_native_result_fini (&native_result);
		R_FREE (methods);
		R_FREE (types);
		r2unity_free_metadata (meta);
		r_unref (buf);
		free_symbol_overrides (&symbol_overrides);
		return 0;
	}

	if (!quiet) {
		printf ("# r2 script generated by r2unity\n");
		printf ("# Input file: %s\n", metadata_path);
		print_native_comment (&native_result);
		printf ("\n");
	}

	size_t img_count = 0;
	Il2CppImageDefinition *images = r2unity_get_images (meta, &img_count);
	int *type2img = r2unity_build_type_image_map (images, img_count, type_count);

	long printed = 0;
	if (methods && types) {
		for (size_t j = 0; j < method_count; j++) {
			if (limit >= 0 && printed >= limit) {
				break;
			}
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
			if (emit_r2_method (meta, m, td, type_idx, img, type2img != NULL, addr)) {
				printed++;
			}
		}
	}

	r2unity_native_result_fini (&native_result);
	R_FREE (methods);
	R_FREE (types);
	R_FREE (images);
	R_FREE (type2img);

	r2unity_free_metadata (meta);
	r_unref (buf);
	free_symbol_overrides (&symbol_overrides);

	return 0;
}

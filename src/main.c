/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <r2unity_config.h>
#include "lib/lib.h"

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
		"  -c N          Read N pointer entries (pair with -a)\n"
		"  -D            Detect companion files from the given executable path and exit\n"
		"  -f            Fast path: auto-detect ELF/Mach-O/PE and scan method pointers\n"
		"  -h            Show this help and exit\n"
		"  -j            One-line JSON status, or JSON output with -P/-R/-S\n"
		"  -l N          Limit emitted entries to N\n"
		"  -P            Enumerate P/Invoke (managed -> native) methods\n"
		"  -q            Quiet mode: omit banner and informational comments\n"
		"  -r            Emit r2 script commands (flags + comments); pairs with -P\n"
		"  -R            Enumerate reverse-P/Invoke (native -> managed) methods (v29+)\n"
		"  -S            Emit a text SBOM of the managed assemblies\n"
		"  -V            Verbose debug tracing on stderr\n"
		"  -v            Show version and exit\n"
		"  -z            Enumerate managed string literals (`ldstr`) from metadata\n"
		"\n"
		"Arguments:\n"
		"  <executable>           Native IL2CPP binary for the target platform\n"
		"  <global-metadata.dat>  Unity IL2CPP metadata file\n"
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

/* Sniff the exe magic and dispatch to the matching fast finder. */
static bool find_method_pointers_fast(R2UnityMetadata *meta, const char *path, ut64 **out_ptrs) {
	ut8 magic[4] = { 0 };
	FILE *fp = fopen (path, "rb");
	if (fp) {
		if (fread (magic, 1, sizeof (magic), fp) != sizeof (magic)) {
			memset (magic, 0, sizeof (magic));
		}
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
	if (im && *im) {
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

int main(int argc, char *argv[]) {
	bool json_one_line = false;
	bool r2_script = false;
	bool quiet = false;
	bool debug = false;
	long limit = -1;
	ut64 gmp_addr = 0;
	size_t gmp_count = 0;
	bool fast = false;
	bool sbom = false;
	bool pinvokes = false;
	bool reverse_pinvokes = false;
	bool string_literals = false;
	bool detect_paths = false;
	int opt;
	while ((opt = getopt (argc, argv, "hjrqfVvSPRDzl:a:c:")) != -1) {
		switch (opt) {
		case 'j': json_one_line = true; break;
		case 'r': r2_script = true; break;
		case 'q': quiet = true; break;
		case 'f': fast = true; break;
		case 'V': debug = true; break;
		case 'v':
			printf ("r2unity %s\n", R2UNITY_VERSION);
			return 0;
		case 'S': sbom = true; break;
		case 'P': pinvokes = true; break;
		case 'R': reverse_pinvokes = true; break;
		case 'D': detect_paths = true; break;
		case 'z': string_literals = true; break;
		case 'l': limit = strtol (optarg, NULL, 0); break;
		case 'a': gmp_addr = (ut64)strtoull (optarg, NULL, 0); break;
		case 'c': gmp_count = (size_t)strtoull (optarg, NULL, 0); break;
		case 'h':
			print_usage (stdout, argv[0]);
			return 0;
		default:
			print_usage (stderr, argv[0]);
			return 1;
		}
	}
	if (pinvokes && reverse_pinvokes) {
		R_LOG_ERROR ("-P and -R are mutually exclusive");
		return 1;
	}
	if (detect_paths) {
		if (argc - optind != 1) {
			print_usage (stderr, argv[0]);
			return 1;
		}
		return emit_detected_paths (argv[optind], json_one_line);
	}
	if (string_literals) {
		if (json_one_line || fast || gmp_addr || sbom || pinvokes || reverse_pinvokes) {
			R_LOG_ERROR ("-z cannot be combined with -j, -f, -a/-c, -S, -P or -R");
			return 1;
		}
		if (argc - optind != 1 && argc - optind != 2) {
			print_usage (stderr, argv[0]);
			return 1;
		}
	} else if (argc - optind != 2) {
		print_usage (stderr, argv[0]);
		return 1;
	}

	const char *exe_path = NULL;
	const char *metadata_path = NULL;
	if (string_literals && argc - optind == 1) {
		exe_path = "";
		metadata_path = argv[optind];
	} else {
		exe_path = argv[optind];
		metadata_path = argv[optind + 1];
	}

	RBuffer *buf = r_buf_new_file (metadata_path, O_RDONLY, 0);
	if (!buf) {
		perror ("Error opening file");
		return 1;
	}

	if (debug) {
		r_log_set_level (R_LOG_LEVEL_DEBUG);
	}
	R2UnityMetadata *meta = r2unity_parse_metadata (buf);
	if (!meta) {
		R_LOG_ERROR ("Failed to parse metadata");
		r_unref (buf);
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
		return rc;
	}

	if (pinvokes || reverse_pinvokes) {
		if (json_one_line && r2_script) {
			R_LOG_ERROR ("-j and -r are mutually exclusive");
			r2unity_free_metadata (meta);
			r_unref (buf);
			return 1;
		}
		int rc = reverse_pinvokes
			? emit_reverse_pinvokes (meta, exe_path, json_one_line, r2_script, quiet)
			: emit_pinvokes (meta, exe_path, json_one_line, r2_script, quiet);
		r2unity_free_metadata (meta);
		r_unref (buf);
		return rc;
	}

	if (string_literals) {
		int rc = emit_string_literals (meta, metadata_path, quiet, limit);
		r2unity_free_metadata (meta);
		r_unref (buf);
		return rc;
	}

	size_t type_count = 0;
	Il2CppTypeDefinition *types = r2unity_get_type_definitions (meta, &type_count);

	size_t method_count = 0;
	Il2CppMethodDefinition *methods = r2unity_get_method_definitions (meta, &method_count);

	ut64 *method_ptrs = NULL;
	bool has_ptrs = false;
	if (gmp_addr) {
		R_LOG_WARN ("Manual method-pointer table reading (-a/-c) is not implemented");
		(void)gmp_count;
	} else if (fast) {
		has_ptrs = find_method_pointers_fast (meta, exe_path, &method_ptrs);
	}
	/* Fast-path may prealloc an all-zero table; upgrade has_ptrs only if any
	 * non-zero entry survived. */
	if (method_ptrs && !has_ptrs) {
		for (size_t k = 0; k < method_count; k++) {
			if (method_ptrs[k]) {
				has_ptrs = true;
				break;
			}
		}
	}

	if (json_one_line) {
		// Output a single stable JSON line
		printf ("{\"ok\":true,\"version\":%d,\"types\":%u,\"methods\":%u,\"has_ptrs\":%s}\n",
			meta->version,
			(unsigned)type_count,
			(unsigned)method_count,
			has_ptrs? "true": "false");
		R_FREE (method_ptrs);
		R_FREE (methods);
		R_FREE (types);
		r2unity_free_metadata (meta);
		r_unref (buf);
		return 0;
	}

	if (!quiet) {
		printf ("# r2 script generated by r2unity\n");
		printf ("# Input file: %s\n\n", metadata_path);
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

	R_FREE (method_ptrs);
	R_FREE (methods);
	R_FREE (types);
	R_FREE (images);
	R_FREE (type2img);

	r2unity_free_metadata (meta);
	r_unref (buf);

	return 0;
}

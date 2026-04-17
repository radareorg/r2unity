#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lib/lib.h"

static void build_method_attrs_string (char *o, size_t osz, unsigned int flags) {
	/* System.Reflection.MethodAttributes subset */
	const unsigned int MemberAccessMask = 0x0007;
	const unsigned int MdPrivate = 0x0001;
	const unsigned int MdFamANDAssem = 0x0002; /* private protected */
	const unsigned int MdAssembly = 0x0003;    /* internal */
	const unsigned int MdFamily = 0x0004;      /* protected */
	const unsigned int MdFamORAssem = 0x0005;  /* protected internal */
	const unsigned int MdPublic = 0x0006;
	const unsigned int MdStatic = 0x0010;
	const unsigned int MdFinal = 0x0020;
	const unsigned int MdVirtual = 0x0040;
	const unsigned int MdAbstract = 0x0400;
	const unsigned int MdPinvokeImpl = IL2CPP_METHOD_ATTRIBUTE_PINVOKE_IMPL;
	const char *vis = "";
	switch (flags & MemberAccessMask) {
	case MdPublic: vis = "public"; break;
	case MdFamily: vis = "protected"; break;
	case MdAssembly: vis = "internal"; break;
	case MdFamORAssem: vis = "protected internal"; break;
	case MdFamANDAssem: vis = "private protected"; break;
	case MdPrivate: vis = "private"; break;
	default: vis = ""; break;
	}
	char tmp[128]; tmp[0] = 0;
	if (*vis) {
		strncat (tmp, vis, sizeof (tmp) - 1);
	}
	if (flags & MdStatic) {
		if (*tmp) strncat (tmp, " ", sizeof (tmp) - 1);
		strncat (tmp, "static", sizeof (tmp) - 1);
	}
	if (flags & MdAbstract) {
		if (*tmp) strncat (tmp, " ", sizeof (tmp) - 1);
		strncat (tmp, "abstract", sizeof (tmp) - 1);
	}
	if (flags & MdVirtual) {
		if (*tmp) strncat (tmp, " ", sizeof (tmp) - 1);
		strncat (tmp, "virtual", sizeof (tmp) - 1);
	}
	if (flags & MdFinal) {
		if (*tmp) strncat (tmp, " ", sizeof (tmp) - 1);
		strncat (tmp, "final", sizeof (tmp) - 1);
	}
	if (flags & MdPinvokeImpl) {
		if (*tmp) strncat (tmp, " ", sizeof (tmp) - 1);
		strncat (tmp, "extern", sizeof (tmp) - 1);
	}
	if (!*tmp) {
		strncpy (o, "", osz);
	} else {
		strncpy (o, tmp, osz);
		o[osz? osz - 1: 0] = 0;
	}
}

static void print_usage (FILE *out, const char *prog_name) {
	fprintf (out,
		"Usage: %s [options] <executable> <global-metadata.dat>\n"
		"\n"
		"Options:\n"
		"  -j            One-line JSON status (ok/version/types/methods/has_ptrs)\n"
		"  -q            Quiet mode: omit banner and informational comments\n"
		"  -l N          Limit emitted entries to N\n"
		"  -f            Fast path: auto-detect ELF/Mach-O/PE and scan method pointers\n"
		"  -a 0xADDR     Read the method pointer table starting at virtual address 0xADDR\n"
		"  -c N          Read N pointer entries (pair with -a)\n"
		"  -S            Emit a CycloneDX SBOM (JSON) of the managed assemblies\n"
		"  -P            Enumerate P/Invoke (managed -> native) methods\n"
		"  -z            Enumerate managed string literals (`ldstr`) from metadata\n"
		"  -F FORMAT     Output format for -P: text|json|r2 (default: text)\n"
		"  -v            Verbose debug tracing on stderr\n"
		"  -h            Show this help and exit\n"
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
		"                      metadata: *_Data/il2cpp_data/Metadata/global-metadata.dat\n"
		, prog_name);
}

static const char *unity_range_from_wire (int wire) {
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

static void json_escape (FILE *f, const char *s) {
	for (; s && *s; s++) {
		unsigned char c = (unsigned char) *s;
		switch (c) {
		case '"':  fputs ("\\\"", f); break;
		case '\\': fputs ("\\\\", f); break;
		case '\b': fputs ("\\b", f); break;
		case '\f': fputs ("\\f", f); break;
		case '\n': fputs ("\\n", f); break;
		case '\r': fputs ("\\r", f); break;
		case '\t': fputs ("\\t", f); break;
		default:
			if (c < 0x20) fprintf (f, "\\u%04x", c);
			else fputc (c, f);
		}
	}
}

static void print_escaped_literal (FILE *f, const ut8 *buf, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned char c = buf[i];
		if (c < 0x80) {
			switch (c) {
			case '\\': fputs ("\\\\", f); break;
			case '"':  fputs ("\\\"", f); break;
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
		int n = r_utf8_decode (buf + i, (int)(len - i), &ch);
		if (n > 0) {
			fwrite (buf + i, 1, n, f);
			i += n;
		} else {
			fprintf (f, "\\x%02x", c);
			i++;
		}
	}
}

static int emit_string_literals (R2UnityMetadata *meta, const char *metadata_path, bool quiet, long limit) {
	size_t count = 0;
	Il2CppStringLiteral *lits = r2unity_get_string_literals (meta, &count);
	if (!lits) {
		fprintf (stderr, "r2unity: no string literals found in %s\n", metadata_path);
		return 1;
	}
	if (!quiet) {
		printf ("# managed string literals from %s\n", metadata_path);
		printf ("# idx\tdata_off\tlen\ttext\n");
	}
	size_t max = count;
	if (limit >= 0 && (size_t) limit < max) {
		max = (size_t) limit;
	}
	for (size_t i = 0; i < max; i++) {
		ut8 *bytes = NULL;
		size_t len = 0;
		if (!r2unity_read_string_literal (meta, &lits[i], &bytes, &len)) {
			printf ("%zu\t0x%x\t%u\t<invalid>\n",
				i, meta->stringLiteralDataOffset + lits[i].dataIndex, lits[i].length);
			continue;
		}
		printf ("%zu\t0x%x\t%u\t\"", i,
			meta->stringLiteralDataOffset + lits[i].dataIndex, lits[i].length);
		print_escaped_literal (stdout, bytes, len);
		printf ("\"\n");
		R_FREE (bytes);
	}
	R_FREE (lits);
	return 0;
}

static int emit_sbom (R2UnityMetadata *meta, const char *exe_path, const char *metadata_path) {
	size_t img_count = 0;
	Il2CppImageDefinition *imgs = r2unity_get_images (meta, &img_count);
	size_t asm_count = 0;
	Il2CppAssemblyDefinition *asms = r2unity_get_assemblies (meta, &asm_count);
	size_t ref_count = 0;
	int32_t *refs = r2unity_get_referenced_assemblies (meta, &ref_count);
	if (!asms || !asm_count) {
		fprintf (stderr, "r2unity: unable to decode assemblies table for wire version %d\n",
			meta->version);
		R_FREE (imgs);
		R_FREE (asms);
		R_FREE (refs);
		return 1;
	}

	FILE *f = stdout;
	fprintf (f, "{\n");
	fprintf (f, "  \"bomFormat\": \"CycloneDX\",\n");
	fprintf (f, "  \"specVersion\": \"1.5\",\n");
	fprintf (f, "  \"version\": 1,\n");
	fprintf (f, "  \"metadata\": {\n");
	fprintf (f, "    \"tools\": {\n");
	fprintf (f, "      \"components\": [\n");
	fprintf (f, "        { \"type\": \"application\", \"name\": \"r2unity\" }\n");
	fprintf (f, "      ]\n");
	fprintf (f, "    },\n");
	fprintf (f, "    \"component\": {\n");
	fprintf (f, "      \"type\": \"application\",\n");
	fprintf (f, "      \"name\": \"");
	json_escape (f, exe_path ? exe_path : "unity-build");
	fprintf (f, "\",\n");
	fprintf (f, "      \"version\": \"%s\",\n", unity_range_from_wire (meta->version));
	fprintf (f, "      \"properties\": [\n");
	fprintf (f, "        { \"name\": \"unity.metadata.path\",         \"value\": \"");
	json_escape (f, metadata_path);
	fprintf (f, "\" },\n");
	fprintf (f, "        { \"name\": \"unity.metadata.wire_version\", \"value\": \"%d\" },\n", meta->version);
	fprintf (f, "        { \"name\": \"unity.metadata.confidence\",   \"value\": \"range\" }\n");
	fprintf (f, "      ]\n");
	fprintf (f, "    }\n");
	fprintf (f, "  },\n");

	/* managed components */
	fprintf (f, "  \"components\": [\n");
	for (size_t i = 0; i < asm_count; i++) {
		Il2CppAssemblyDefinition *a = &asms[i];
		char *name = (char *) r2unity_get_string (meta, a->aname.name_idx);
		char *culture = (char *) r2unity_get_string (meta, a->aname.culture_idx);
		const char *nm = name ? name : "";
		const char *cl = (culture && *culture) ? culture : "neutral";

		/* image (DLL filename) via a->image_index */
		const char *img = "";
		char *img_name_owned = NULL;
		if (imgs && a->image_index >= 0 && (size_t) a->image_index < img_count) {
			img_name_owned = (char *) r2unity_get_string (meta, imgs[a->image_index].nameIndex);
			if (img_name_owned) img = img_name_owned;
		}

		char pkt_hex[17];
		int empty = 1;
		for (int k = 0; k < 8; k++) if (a->aname.public_key_token[k]) { empty = 0; break; }
		if (empty) {
			strcpy (pkt_hex, "null");
		} else {
			snprintf (pkt_hex, sizeof (pkt_hex), "%02x%02x%02x%02x%02x%02x%02x%02x",
				a->aname.public_key_token[0], a->aname.public_key_token[1],
				a->aname.public_key_token[2], a->aname.public_key_token[3],
				a->aname.public_key_token[4], a->aname.public_key_token[5],
				a->aname.public_key_token[6], a->aname.public_key_token[7]);
		}

		fprintf (f, "    {\n");
		fprintf (f, "      \"bom-ref\": \"asm:");
		json_escape (f, nm);
		fprintf (f, ":%d.%d.%d.%d\",\n",
			a->aname.major, a->aname.minor, a->aname.build, a->aname.revision);
		fprintf (f, "      \"type\": \"library\",\n");
		fprintf (f, "      \"name\": \"");
		json_escape (f, nm);
		fprintf (f, "\",\n");
		fprintf (f, "      \"version\": \"%d.%d.%d.%d\",\n",
			a->aname.major, a->aname.minor, a->aname.build, a->aname.revision);
		fprintf (f, "      \"purl\": \"pkg:generic/unity/");
		json_escape (f, nm);
		fprintf (f, "@%d.%d.%d.%d\",\n",
			a->aname.major, a->aname.minor, a->aname.build, a->aname.revision);
		fprintf (f, "      \"properties\": [\n");
		fprintf (f, "        { \"name\": \"dotnet.culture\",          \"value\": \"");
		json_escape (f, cl);
		fprintf (f, "\" },\n");
		fprintf (f, "        { \"name\": \"dotnet.public_key_token\", \"value\": ");
		if (empty) fprintf (f, "null"); else fprintf (f, "\"%s\"", pkt_hex);
		fprintf (f, " },\n");
		fprintf (f, "        { \"name\": \"dotnet.hash_alg\",         \"value\": \"0x%08x\" },\n", a->aname.hash_alg);
		fprintf (f, "        { \"name\": \"dotnet.flags\",            \"value\": \"0x%08x\" },\n", a->aname.flags);
		fprintf (f, "        { \"name\": \"il2cpp.image\",            \"value\": \"");
		json_escape (f, img);
		fprintf (f, "\" },\n");
		fprintf (f, "        { \"name\": \"il2cpp.image_index\",      \"value\": \"%d\" },\n", a->image_index);
		fprintf (f, "        { \"name\": \"il2cpp.token\",            \"value\": \"0x%08x\" }\n", a->token);
		fprintf (f, "      ]\n");
		fprintf (f, "    }%s\n", (i + 1 < asm_count) ? "," : "");

		free (name);
		free (culture);
		free (img_name_owned);
	}
	fprintf (f, "  ],\n");

	/* dependency edges from referencedAssemblies */
	fprintf (f, "  \"dependencies\": [\n");
	bool first_dep = true;
	for (size_t i = 0; i < asm_count; i++) {
		Il2CppAssemblyDefinition *a = &asms[i];
		if (a->referenced_count <= 0 || a->referenced_start < 0) continue;
		if ((size_t) (a->referenced_start + a->referenced_count) > ref_count) continue;
		char *name = (char *) r2unity_get_string (meta, a->aname.name_idx);
		const char *nm = name ? name : "";
		if (!first_dep) fprintf (f, ",\n");
		first_dep = false;
		fprintf (f, "    {\n      \"ref\": \"asm:");
		json_escape (f, nm);
		fprintf (f, ":%d.%d.%d.%d\",\n      \"dependsOn\": [",
			a->aname.major, a->aname.minor, a->aname.build, a->aname.revision);
		bool first_ref = true;
		for (int k = 0; k < a->referenced_count; k++) {
			int32_t ridx = refs[a->referenced_start + k];
			if (ridx < 0 || (size_t) ridx >= asm_count) continue;
			Il2CppAssemblyDefinition *r = &asms[ridx];
			char *rname = (char *) r2unity_get_string (meta, r->aname.name_idx);
			if (!first_ref) fprintf (f, ",");
			first_ref = false;
			fprintf (f, "\n        \"asm:");
			json_escape (f, rname ? rname : "");
			fprintf (f, ":%d.%d.%d.%d\"",
				r->aname.major, r->aname.minor, r->aname.build, r->aname.revision);
			free (rname);
		}
		fprintf (f, "\n      ]\n    }");
		free (name);
	}
	fprintf (f, "\n  ]\n}\n");

	R_FREE (imgs);
	R_FREE (asms);
	R_FREE (refs);
	return 0;
}

/* Cross-entry sort for stable output: sort by image_name, then name. */
static int interop_cmp (const void *a, const void *b) {
	const R2UnityInterop *x = (const R2UnityInterop *) a;
	const R2UnityInterop *y = (const R2UnityInterop *) b;
	const char *xi = x->image_name ? x->image_name : "";
	const char *yi = y->image_name ? y->image_name : "";
	int c = strcmp (xi, yi);
	if (c) {
		return c;
	}
	const char *xn = x->name ? x->name : "";
	const char *yn = y->name ? y->name : "";
	return strcmp (xn, yn);
}

static int emit_pinvokes (R2UnityMetadata *meta, const char *exe_path,
		const char *format, bool quiet) {
	(void) exe_path;
	size_t n = 0;
	R2UnityInterop *items = r2unity_enumerate_pinvokes (meta, &n);
	if (items && n > 1) {
		qsort (items, n, sizeof (R2UnityInterop), interop_cmp);
	}

	const bool is_json = format && !strcmp (format, "json");
	const bool is_r2 = format && !strcmp (format, "r2");

	if (is_json) {
		FILE *f = stdout;
		fprintf (f, "{\"ok\":true,\"version\":%d,\"pinvokes\":[", meta->version);
		for (size_t i = 0; i < n; i++) {
			R2UnityInterop *it = &items[i];
			if (i) fputc (',', f);
			fprintf (f, "{\"image\":\"");
			json_escape (f, it->image_name ? it->image_name : "");
			fprintf (f, "\",\"method\":\"");
			json_escape (f, it->name ? it->name : "");
			fprintf (f, "\",\"token\":\"0x%08x\",\"flags\":\"0x%04x\",\"iflags\":\"0x%04x\",\"confidence\":%u",
				it->token, it->flags, it->iflags, (unsigned) it->confidence);
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
				printf ("# PInvoke %s -> %s!%s\n", buf,
					it->dll_name,
					it->entry_name ? it->entry_name : it->name);
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
			char attrs[128];
			attrs[0] = 0;
			build_method_attrs_string (attrs, sizeof (attrs), it->flags);
			printf ("%s\t%s\t%s\t%s\t%s\t%u\n",
				it->image_name ? it->image_name : "",
				it->name ? it->name : "",
				it->dll_name ? it->dll_name : "<unresolved>",
				it->entry_name ? it->entry_name : "<default>",
				*attrs ? attrs : "-",
				(unsigned) it->confidence);
		}
		if (!quiet) {
			printf ("# summary: pinvokes=%zu reverse_pinvokes=0 (wrapper resolution not yet implemented)\n", n);
		}
	}

	r2unity_free_interop (items, n);
	return 0;
}

int main (int argc, char *argv[]) {
	bool json_one_line = false;
	bool quiet = false;
	bool debug = false;
	long limit = -1;
	ut64 gmp_addr = 0;
	size_t gmp_count = 0;
	bool fast = false;
	bool sbom = false;
	bool pinvokes = false;
	bool string_literals = false;
	const char *format = "text";
	int opt;
	while ((opt = getopt (argc, argv, "hjqfvSPzl:a:c:F:")) != -1) {
		switch (opt) {
		case 'j': json_one_line = true; break;
		case 'q': quiet = true; break;
		case 'f': fast = true; break;
		case 'v': debug = true; break;
		case 'S': sbom = true; break;
		case 'P': pinvokes = true; break;
		case 'z': string_literals = true; break;
		case 'F': format = optarg; break;
		case 'l': limit = strtol (optarg, NULL, 0); break;
		case 'a': gmp_addr = (ut64) strtoull (optarg, NULL, 0); break;
		case 'c': gmp_count = (size_t) strtoull (optarg, NULL, 0); break;
		case 'h':
			print_usage (stdout, argv[0]);
			return 0;
		default:
			print_usage (stderr, argv[0]);
			return 1;
		}
	}
	if (string_literals) {
		if (json_one_line || fast || gmp_addr || sbom || pinvokes) {
			fprintf (stderr, "r2unity: -z cannot be combined with -j, -f, -a/-c, -S or -P\n");
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

	/* configure debug */
	r2unity_set_debug (debug);
	R2UnityMetadata *meta = r2unity_parse_metadata (buf);
	if (!meta) {
		fprintf (stderr, "Failed to parse metadata\n");
		r_unref (buf);
		return 1;
	}

	if (sbom) {
		int rc = emit_sbom (meta, exe_path, metadata_path);
		r2unity_free_metadata (meta);
		r_unref (buf);
		return rc;
	}

	if (pinvokes) {
		if (strcmp (format, "text") && strcmp (format, "json") && strcmp (format, "r2")) {
			fprintf (stderr, "r2unity: unknown -F format %s (expected text|json|r2)\n", format);
			r2unity_free_metadata (meta);
			r_unref (buf);
			return 1;
		}
		int rc = emit_pinvokes (meta, exe_path, format, quiet);
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
		has_ptrs = r2unity_read_method_pointers_at (meta, exe_path, gmp_addr, gmp_count, &method_ptrs);
	} else if (fast) {
		/* Auto-detect file type by magic */
		unsigned char magic[4] = {0};
		FILE *fp = fopen (exe_path, "rb");
		if (fp) {
			(void)fread (magic, 1, 4, fp);
			fclose (fp);
		}
		ut32 m = (ut32)magic[0] | ((ut32)magic[1] << 8) | ((ut32)magic[2] << 16) | ((ut32)magic[3] << 24);
		if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
			has_ptrs = r2unity_find_method_pointers_elf (meta, exe_path, &method_ptrs);
		} else if (m == 0xfeedfacf || m == 0xcffaedfe || m == 0xcafebabe || m == 0xbebafeca) {
			has_ptrs = r2unity_find_method_pointers_macho (meta, exe_path, &method_ptrs);
		} else if (magic[0] == 'M' && magic[1] == 'Z') {
			has_ptrs = r2unity_find_method_pointers_pe (meta, exe_path, &method_ptrs);
		} else {
			has_ptrs = false;
		}
	} else {
		has_ptrs = false;
	}
	// Recompute has_ptrs by checking any non-zero entry
	if (method_ptrs) {
		for (size_t k = 0; k < method_count; k++) {
			if (method_ptrs[k]) { has_ptrs = true; break; }
		}
	}

	if (json_one_line) {
		// Output a single stable JSON line
		printf ("{\"ok\":true,\"version\":%d,\"types\":%u,\"methods\":%u,\"has_ptrs\":%s}\n",
			meta->version, (unsigned)type_count, (unsigned)method_count, has_ptrs? "true": "false");
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

	long printed = 0;
	// Keep 1:1 mapping between method index and pointer index.
	// Do NOT shift, gaps (zeros) are expected for abstract/generic/external methods.
	size_t mp_shift = 0;
	// Optionally enrich names with image/module prefix
	size_t img_count = 0;
	Il2CppImageDefinition *images = NULL;
	int *type2img = NULL;
	images = r2unity_get_images (meta, &img_count);
	if (images && type_count > 0) {
		type2img = R_NEWS (int, type_count);
		for (size_t ti = 0; ti < type_count; ti++) type2img[ti] = -1;
		for (size_t ii = 0; ii < img_count; ii++) {
			int start = images[ii].typeStart;
			int count = (int) images[ii].typeCount;
			if (start >= 0 && count > 0) {
				for (int k = 0; k < count && (size_t)(start + k) < type_count; k++) {
					type2img[start + k] = (int) ii;
				}
			}
		}
	}

	// Print methods first, then types, to ensure RVAs are shown in limited output
	if (methods && types && (limit < 0 || printed < limit)) {
		for (size_t j = 0; j < method_count; j++) {
			Il2CppMethodDefinition *m = &methods[j];
			const Il2CppTypeDefinition *td = NULL;
			if (m->declaringType >= 0 && (size_t) m->declaringType < type_count) {
				td = &types[m->declaringType];
			}
			char *ns = td ? (char *) r2unity_get_string (meta, td->namespaceIndex) : NULL;
			char *tn = td ? (char *) r2unity_get_string (meta, td->nameIndex) : NULL;
			char *mn = (char *) r2unity_get_string (meta, m->nameIndex);
			if (mn) {
				char fullname[1024];
				fullname[0] = 0;
				if (ns && *ns) {
					snprintf (fullname, sizeof (fullname), "%s.%s.%s(%u)", ns, tn ? tn : "", mn, (unsigned) m->parameterCount);
				} else if (tn && *tn) {
					snprintf (fullname, sizeof (fullname), "%s.%s(%u)", tn, mn, (unsigned) m->parameterCount);
				} else {
					snprintf (fullname, sizeof (fullname), "%s(%u)", mn, (unsigned) m->parameterCount);
				}
				ut64 addr = 0;
				if (has_ptrs && method_ptrs) {
					size_t idx = j + mp_shift;
					if (idx < method_count) {
						addr = method_ptrs[idx];
					}
				}
					if (limit < 0 || printed < limit) {
						if (addr > 0x1000) {
							r_name_filter (fullname, -1);
							if (type2img && td) {
								int ii = type2img[m->declaringType];
								if (ii >= 0 && (size_t)ii < img_count) {
									char *im = (char *) r2unity_get_string (meta, images[ii].nameIndex);
									if (im && *im) {
									printf ("'@0x%"PFMT64x"'f sym.unity.%s.%s\n", addr, im, fullname);
									char attrs[128]; attrs[0] = 0; build_method_attrs_string (attrs, sizeof (attrs), m->flags);
									if (*attrs) {
										printf ("'@0x%"PFMT64x"'CCu Method: [%s] %s %s\n", addr, im, attrs, fullname);
									} else {
										printf ("'@0x%"PFMT64x"'CCu Method: [%s] %s\n", addr, im, fullname);
									}
										free (im);
									} else {
									printf ("'@0x%"PFMT64x"'f sym.unity.%s\n", addr, fullname);
									char attrs2[128]; attrs2[0] = 0; build_method_attrs_string (attrs2, sizeof (attrs2), m->flags);
									if (*attrs2) {
										printf ("'@0x%"PFMT64x"'CCu Method: %s %s\n", addr, attrs2, fullname);
									} else {
										printf ("'@0x%"PFMT64x"'CCu Method: %s\n", addr, fullname);
									}
									}
								} else {
									printf ("'@0x%"PFMT64x"'f sym.unity.%s\n", addr, fullname);
									char attrs3[128]; attrs3[0] = 0; build_method_attrs_string (attrs3, sizeof (attrs3), m->flags);
									if (*attrs3) {
										printf ("'@0x%"PFMT64x"'CCu Method: %s\n", addr, attrs3);
									}
								}
							} else {
							printf ("# %s\n", fullname);
						}
						printed++;
					}
			}
			free (ns);
			free (tn);
			free (mn);
			if (limit >= 0 && printed >= limit) {
				break;
			}
			}
		}
	}

	if (types && (limit < 0 || printed < limit)) {
		for (size_t j = 0; j < type_count; j++) {
			char *type_name = (char *) r2unity_get_string (meta, types[j].nameIndex);
			char *namespace_name = (char *) r2unity_get_string (meta, types[j].namespaceIndex);
			if (type_name) {
					if (limit < 0 || printed < limit) {
						printed++;
					}
			}
			free (type_name);
			free (namespace_name);
			if (limit >= 0 && printed >= limit) {
				break;
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

/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.sbom"

#include "lib.h"

static void sbom_version(char *buf, size_t buf_size, const Il2CppAssemblyDefinition *a) {
	snprintf (buf, buf_size, "%d.%d.%d.%d",
		a->aname.major, a->aname.minor, a->aname.build, a->aname.revision);
}

static char *bomref_new(const char *name, const char *version) {
	return r_str_newf ("asm:%s:%s", name? name: "", version? version: "");
}

static char *purl_new(const char *name, const char *version) {
	char *enc_name = r_str_uri_encode (name? name: "");
	char *enc_version = r_str_uri_encode (version? version: "");
	char *purl = r_str_newf ("pkg:generic/unity/%s@%s",
		enc_name? enc_name: "",
		enc_version? enc_version: "");
	free (enc_name);
	free (enc_version);
	return purl;
}

static void text_bomref(RStrBuf *sb, const char *name, const char *version) {
	r_strbuf_appendf (sb, "asm:%s:%s", name? name: "", version);
}

static bool public_key_token_is_empty(const Il2CppAssemblyDefinition *a) {
	for (int i = 0; i < 8; i++) {
		if (a->aname.public_key_token[i]) {
			return false;
		}
	}
	return true;
}

static void public_key_token_hex(char out[17], const Il2CppAssemblyDefinition *a) {
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 8; i++) {
		ut8 b = a->aname.public_key_token[i];
		out[i * 2] = hex[b >> 4];
		out[i * 2 + 1] = hex[b & 0xf];
	}
	out[16] = 0;
}

static void pj_property_str(PJ *pj, const char *name, const char *value) {
	pj_o (pj);
	pj_ks (pj, "name", name);
	pj_ks (pj, "value", value? value: "");
	pj_end (pj);
}

static void pj_property_int(PJ *pj, const char *name, int value) {
	char buf[32];
	snprintf (buf, sizeof (buf), "%d", value);
	pj_property_str (pj, name, buf);
}

static void pj_property_hex32(PJ *pj, const char *name, ut32 value) {
	char buf[16];
	snprintf (buf, sizeof (buf), "0x%08x", value);
	pj_property_str (pj, name, buf);
}

static char *assembly_image_name(R2UnityMetadata *meta, const Il2CppImageDefinition *imgs, size_t img_count, const Il2CppAssemblyDefinition *a) {
	if (!imgs || a->image_index < 0 || (size_t)a->image_index >= img_count) {
		return NULL;
	}
	return r2unity_get_string (meta, imgs[a->image_index].nameIndex);
}

static bool assembly_ref_slice(const Il2CppAssemblyDefinition *a, size_t ref_count, size_t *start, size_t *count) {
	if (a->referenced_count <= 0 || a->referenced_start < 0) {
		return false;
	}
	size_t s = (size_t)a->referenced_start;
	size_t n = (size_t)a->referenced_count;
	if (s > ref_count || n > ref_count - s) {
		return false;
	}
	*start = s;
	*count = n;
	return true;
}

static void append_json_components(PJ *pj, R2UnityMetadata *meta, const Il2CppImageDefinition *imgs, size_t img_count, const Il2CppAssemblyDefinition *asms, size_t asm_count) {
	pj_ka (pj, "components");
	for (size_t i = 0; i < asm_count; i++) {
		const Il2CppAssemblyDefinition *a = &asms[i];
		char *name = r2unity_get_string (meta, a->aname.name_idx);
		char *culture = r2unity_get_string (meta, a->aname.culture_idx);
		char *img_name = assembly_image_name (meta, imgs, img_count, a);
		const char *nm = name? name: "";
		const char *cl = (culture && *culture)? culture: "neutral";
		const char *img = img_name? img_name: "";
		char ver[64];
		sbom_version (ver, sizeof (ver), a);
		char *bomref = bomref_new (nm, ver);
		char *purl = purl_new (nm, ver);

		pj_o (pj);
		pj_ks (pj, "bom-ref", bomref? bomref: "");
		pj_ks (pj, "type", "library");
		pj_ks (pj, "name", nm);
		pj_ks (pj, "version", ver);
		pj_ks (pj, "purl", purl? purl: "");
		pj_ka (pj, "properties");
		pj_property_str (pj, "dotnet.culture", cl);
		if (public_key_token_is_empty (a)) {
			pj_property_str (pj, "dotnet.public_key_token", NULL);
		} else {
			char pkt_hex[17];
			public_key_token_hex (pkt_hex, a);
			pj_property_str (pj, "dotnet.public_key_token", pkt_hex);
		}
		pj_property_hex32 (pj, "dotnet.hash_alg", a->aname.hash_alg);
		pj_property_hex32 (pj, "dotnet.flags", a->aname.flags);
		pj_property_str (pj, "il2cpp.image", img);
		pj_property_int (pj, "il2cpp.image_index", a->image_index);
		pj_property_hex32 (pj, "il2cpp.token", a->token);
		pj_end (pj);
		pj_end (pj);

		free (name);
		free (culture);
		free (img_name);
		free (bomref);
		free (purl);
	}
	pj_end (pj);
}

static void append_json_dependencies(PJ *pj, R2UnityMetadata *meta, const Il2CppAssemblyDefinition *asms, size_t asm_count, const int32_t *refs, size_t ref_count) {
	pj_ka (pj, "dependencies");
	for (size_t i = 0; i < asm_count; i++) {
		const Il2CppAssemblyDefinition *a = &asms[i];
		size_t start = 0;
		size_t count = 0;
		if (!assembly_ref_slice (a, ref_count, &start, &count)) {
			continue;
		}
		char *name = r2unity_get_string (meta, a->aname.name_idx);
		const char *nm = name? name: "";
		char ver[64];
		sbom_version (ver, sizeof (ver), a);
		char *bomref = bomref_new (nm, ver);
		pj_o (pj);
		pj_ks (pj, "ref", bomref? bomref: "");
		pj_ka (pj, "dependsOn");
		for (size_t k = 0; k < count; k++) {
			int32_t ridx = refs[start + k];
			if (ridx < 0 || (size_t)ridx >= asm_count) {
				continue;
			}
			const Il2CppAssemblyDefinition *r = &asms[ridx];
			char *rname = r2unity_get_string (meta, r->aname.name_idx);
			char rver[64];
			sbom_version (rver, sizeof (rver), r);
			char *rref = bomref_new (rname? rname: "", rver);
			pj_s (pj, rref? rref: "");
			free (rname);
			free (rref);
		}
		pj_end (pj);
		pj_end (pj);
		free (name);
		free (bomref);
	}
	pj_end (pj);
}

static char *sbom_json_tostring(R2UnityMetadata *meta, const char *exe_path, const char *metadata_path, const Il2CppImageDefinition *imgs, size_t img_count, const Il2CppAssemblyDefinition *asms, size_t asm_count, const int32_t *refs, size_t ref_count) {
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
	pj_ks (pj, "name", exe_path && *exe_path? exe_path: "unity-build");
	pj_ks (pj, "version", r2unity_unity_range_from_wire (meta->version));
	pj_ka (pj, "properties");
	pj_property_str (pj, "unity.metadata.path", metadata_path? metadata_path: "");
	pj_property_int (pj, "unity.metadata.wire_version", meta->version);
	pj_property_str (pj, "unity.metadata.confidence", "range");
	pj_end (pj);
	pj_end (pj);
	pj_end (pj);

	append_json_components (pj, meta, imgs, img_count, asms, asm_count);
	append_json_dependencies (pj, meta, asms, asm_count, refs, ref_count);
	pj_end (pj);
	char *out = pj_drain (pj);
	return out? r_str_append (out, "\n"): NULL;
}

static char *sbom_text_tostring(R2UnityMetadata *meta, const char *exe_path, const char *metadata_path, const Il2CppImageDefinition *imgs, size_t img_count, const Il2CppAssemblyDefinition *asms, size_t asm_count, const int32_t *refs, size_t ref_count) {
	RStrBuf *sb = r_strbuf_new ("");
	r_strbuf_append (sb, "# Unity IL2CPP SBOM\n");
	r_strbuf_appendf (sb, "target: %s\n", exe_path && *exe_path? exe_path: "unity-build");
	r_strbuf_appendf (sb, "metadata: %s\n", metadata_path && *metadata_path? metadata_path: "-");
	r_strbuf_appendf (sb, "wire_version: %d (%s)\n", meta->version, r2unity_unity_range_from_wire (meta->version));
	r_strbuf_appendf (sb, "assemblies: %zu\n\n", asm_count);
	r_strbuf_append (sb, "Components:\n");
	r_strbuf_append (sb, "NAME\tVERSION\tCULTURE\tIMAGE\tIMAGE_INDEX\tTOKEN\tPUBLIC_KEY_TOKEN\n");
	for (size_t i = 0; i < asm_count; i++) {
		const Il2CppAssemblyDefinition *a = &asms[i];
		char *name = r2unity_get_string (meta, a->aname.name_idx);
		char *culture = r2unity_get_string (meta, a->aname.culture_idx);
		char *img_name = assembly_image_name (meta, imgs, img_count, a);
		char ver[64];
		char pkt_hex[17];
		const char *pkt = "-";
		sbom_version (ver, sizeof (ver), a);
		if (!public_key_token_is_empty (a)) {
			public_key_token_hex (pkt_hex, a);
			pkt = pkt_hex;
		}
		r_strbuf_appendf (sb, "%s\t%s\t%s\t%s\t%d\t0x%08x\t%s\n",
			name? name: "",
			ver,
			(culture && *culture)? culture: "neutral",
			img_name? img_name: "",
			a->image_index,
			a->token,
			pkt);
		free (name);
		free (culture);
		free (img_name);
	}
	r_strbuf_append (sb, "\nDependencies:\n");
	bool any_dep = false;
	for (size_t i = 0; i < asm_count; i++) {
		const Il2CppAssemblyDefinition *a = &asms[i];
		size_t start = 0;
		size_t count = 0;
		if (!assembly_ref_slice (a, ref_count, &start, &count)) {
			continue;
		}
		char *name = r2unity_get_string (meta, a->aname.name_idx);
		char ver[64];
		sbom_version (ver, sizeof (ver), a);
		text_bomref (sb, name? name: "", ver);
		r_strbuf_append (sb, " -> ");
		bool first_ref = true;
		for (size_t k = 0; k < count; k++) {
			int32_t ridx = refs[start + k];
			if (ridx < 0 || (size_t)ridx >= asm_count) {
				continue;
			}
			const Il2CppAssemblyDefinition *r = &asms[ridx];
			char *rname = r2unity_get_string (meta, r->aname.name_idx);
			char rver[64];
			sbom_version (rver, sizeof (rver), r);
			if (!first_ref) {
				r_strbuf_append (sb, ", ");
			}
			first_ref = false;
			text_bomref (sb, rname? rname: "", rver);
			free (rname);
		}
		if (first_ref) {
			r_strbuf_append (sb, "-");
		}
		r_strbuf_append (sb, "\n");
		any_dep = true;
		free (name);
	}
	if (!any_dep) {
		r_strbuf_append (sb, "-\n");
	}
	return r_strbuf_drain (sb);
}

R_API char *r2unity_sbom_tostring(R2UnityMetadata *meta, const char *exe_path, const char *metadata_path, R2UnitySbomFormat format) {
	R_RETURN_VAL_IF_FAIL (meta, NULL);
	size_t img_count = 0;
	Il2CppImageDefinition *imgs = r2unity_get_images (meta, &img_count);
	size_t asm_count = 0;
	Il2CppAssemblyDefinition *asms = r2unity_get_assemblies (meta, &asm_count);
	size_t ref_count = 0;
	int32_t *refs = r2unity_get_referenced_assemblies (meta, &ref_count);
	if (!asms || !asm_count) {
		R_FREE (imgs);
		R_FREE (asms);
		R_FREE (refs);
		return NULL;
	}
	char *out = format == R2U_SBOM_TEXT
		? sbom_text_tostring (meta, exe_path, metadata_path, imgs, img_count, asms, asm_count, refs, ref_count)
		: sbom_json_tostring (meta, exe_path, metadata_path, imgs, img_count, asms, asm_count, refs, ref_count);
	R_FREE (imgs);
	R_FREE (asms);
	R_FREE (refs);
	return out;
}

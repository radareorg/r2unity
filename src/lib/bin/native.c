/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.native"

#include "native_internal.h"
#include <r_bin.h>
#include <stdio.h>
#include <string.h>

#define R2U_CODEGEN_SCAN_MAX_PROBES 128
#define R2U_RBIN_CACHE_SIZE 0x4000
#define R2U_NATIVE_TABLE_MAX_COUNT 0x400000

static const char *const code_registration_names[] = {
	"g_CodeRegistration",
	"s_CodeRegistration",
	"CodeRegistration",
	"g_Il2CppCodeRegistration",
	"Il2CppCodeRegistration",
	NULL
};

static const char *const metadata_registration_names[] = {
	"g_MetadataRegistration",
	"s_MetadataRegistration",
	"MetadataRegistration",
	"g_Il2CppMetadataRegistration",
	"Il2CppMetadataRegistration",
	NULL
};

static const char *const native_table_names[R2U_NATIVE_TABLE_COUNT] = {
	"reversePInvokeWrappers",
	"genericMethodPointers",
	"genericAdjustorThunks",
	"invokerPointers",
	"unresolvedVirtualCallPointers",
	"unresolvedInstanceCallPointers",
	"unresolvedStaticCallPointers",
	"interopData",
	"windowsRuntimeFactoryTable"
};

static const ut8 *view_ptr_at(R2UnityNativeView *view, ut64 va, ut64 *actual_va) {
	if (!view || !view->ptr_at) {
		return NULL;
	}
	const ut8 *p = view->ptr_at (view->user, va);
	if (p) {
		if (actual_va) {
			*actual_va = va;
		}
		return p;
	}
	if (view->base_vaddr != UT64_MAX && view->base_vaddr && va < view->base_vaddr) {
		ut64 rebased = view->base_vaddr + va;
		p = view->ptr_at (view->user, rebased);
		if (p) {
			if (actual_va) {
				*actual_va = rebased;
			}
			return p;
		}
	}
	return NULL;
}

static bool read_u32_at(R2UnityNativeView *view, ut64 va, ut32 *out) {
	const ut8 *p = view_ptr_at (view, va, NULL);
	if (!p) {
		return false;
	}
	*out = r_read_le32 (p);
	return true;
}

static bool read_ptr_at(R2UnityNativeView *view, ut64 va, ut64 *out) {
	const ut8 *p = view_ptr_at (view, va, NULL);
	if (!p) {
		return false;
	}
	*out = view->ptr_size == 8? r_read_le64 (p): (ut64)r_read_le32 (p);
	return true;
}

static bool read_count_ptr_pair(R2UnityNativeView *view, ut64 va, ut32 *count, ut64 *ptr) {
	if (!read_u32_at (view, va, count)) {
		return false;
	}
	return read_ptr_at (view, va + (view->ptr_size == 8? 8: 4), ptr);
}

static ut64 data_va_from_raw(R2UnityNativeView *view, ut64 raw) {
	if (!raw) {
		return 0;
	}
	ut64 actual = 0;
	if (view_ptr_at (view, raw, &actual)) {
		return actual;
	}
	return 0;
}

static ut64 code_va_from_raw(R2UnityNativeView *view, ut64 raw) {
	if (!raw) {
		return 0;
	}
	if (raw >= view->text_lo && raw < view->text_hi) {
		return raw;
	}
	if (view->base_vaddr != UT64_MAX && view->base_vaddr && raw < view->base_vaddr) {
		ut64 rebased = view->base_vaddr + raw;
		if (rebased >= view->text_lo && rebased < view->text_hi) {
			return rebased;
		}
	}
	return 0;
}

static char *read_cstr_at(R2UnityNativeView *view, ut64 va) {
	if (!va) {
		return NULL;
	}
	RStrBuf *sb = r_strbuf_new ("");
	if (!sb) {
		return NULL;
	}
	for (size_t i = 0; i < 512; i++) {
		const ut8 *p = view_ptr_at (view, va + i, NULL);
		if (!p) {
			r_strbuf_free (sb);
			return NULL;
		}
		if (!*p) {
			return r_strbuf_drain (sb);
		}
		if (*p < 0x20 || *p > 0x7e) {
			r_strbuf_free (sb);
			return NULL;
		}
		r_strbuf_appendf (sb, "%c", *p);
	}
	r_strbuf_free (sb);
	return NULL;
}

static bool image_name_eq(const char *module_name, const char *image_name) {
	if (!module_name || !image_name) {
		return false;
	}
	return !strcmp (r_file_basename (module_name), r_file_basename (image_name));
}

typedef struct {
	R2UnityMetadata *meta;
	Il2CppImageDefinition *images;
	Il2CppTypeDefinition *types;
	size_t image_count;
	size_t type_count;
	size_t method_count;
} R2UnityMethodTables;

static bool method_tables_init(R2UnityMethodTables *tables, R2UnityMetadata *meta) {
	memset (tables, 0, sizeof (*tables));
	tables->meta = meta;
	tables->images = r2unity_get_images (meta, &tables->image_count);
	tables->types = r2unity_get_type_definitions (meta, &tables->type_count);
	tables->method_count = (size_t)r2unity_metadata_section_count (meta, R2U_SEC_METHODS);
	return tables->images && tables->image_count && tables->types && tables->type_count && tables->method_count;
}

static void method_tables_fini(R2UnityMethodTables *tables) {
	R_FREE (tables->images);
	R_FREE (tables->types);
}

static int image_index_by_name(R2UnityMethodTables *tables, const char *module_name) {
	if (!module_name || !*module_name) {
		return -1;
	}
	for (size_t i = 0; i < tables->image_count; i++) {
		char *name = r2unity_get_string (tables->meta, tables->images[i].nameIndex);
		bool match = image_name_eq (module_name, name);
		free (name);
		if (match) {
			return (int)i;
		}
	}
	return -1;
}

static size_t image_method_count(R2UnityMethodTables *tables, size_t image_idx) {
	if (image_idx >= tables->image_count) {
		return 0;
	}
	const Il2CppImageDefinition *image = &tables->images[image_idx];
	if (image->typeStart < 0) {
		return 0;
	}
	size_t start = (size_t)image->typeStart;
	size_t end = R_MIN (tables->type_count, start + image->typeCount);
	size_t count = 0;
	for (size_t i = start; i < end; i++) {
		count += tables->types[i].method_count;
	}
	return count;
}

static size_t copy_image_method_table(R2UnityNativeView *view, R2UnityMethodTables *tables, size_t image_idx, ut64 table_va, ut32 table_count, ut64 *out_ptrs) {
	if (image_idx >= tables->image_count || !out_ptrs || !table_va) {
		return 0;
	}
	const Il2CppImageDefinition *image = &tables->images[image_idx];
	if (image->typeStart < 0) {
		return 0;
	}
	size_t local = 0;
	size_t copied = 0;
	size_t start = (size_t)image->typeStart;
	size_t end = R_MIN (tables->type_count, start + image->typeCount);
	for (size_t ti = start; ti < end && local < table_count; ti++) {
		const Il2CppTypeDefinition *td = &tables->types[ti];
		if (td->methodStart < 0) {
			local += td->method_count;
			continue;
		}
		for (size_t k = 0; k < td->method_count && local < table_count; k++, local++) {
			size_t mi = (size_t)td->methodStart + k;
			if (mi >= tables->method_count) {
				continue;
			}
			ut64 raw = 0;
			if (!read_ptr_at (view, table_va + (ut64)local * view->ptr_size, &raw)) {
				continue;
			}
			ut64 addr = code_va_from_raw (view, raw);
			if (addr) {
				out_ptrs[mi] = addr;
				copied++;
			}
		}
	}
	return copied;
}

static size_t copy_global_method_table(R2UnityNativeView *view, size_t method_count, ut64 table_va, ut32 table_count, ut64 *out_ptrs, size_t *out_seen) {
	if (out_seen) {
		*out_seen = 0;
	}
	if (!table_va || !table_count || !method_count || !out_ptrs) {
		return 0;
	}
	size_t max = R_MIN ((size_t)table_count, method_count);
	size_t copied = 0;
	size_t seen = 0;
	for (size_t i = 0; i < max; i++) {
		ut64 raw = 0;
		if (!read_ptr_at (view, table_va + (ut64)i * view->ptr_size, &raw)) {
			break;
		}
		if (raw) {
			seen++;
		}
		ut64 addr = code_va_from_raw (view, raw);
		if (addr) {
			out_ptrs[i] = addr;
			copied++;
		}
	}
	if (out_seen) {
		*out_seen = seen;
	}
	return copied;
}

static bool table_count_ok(ut32 count) {
	return count > 0 && count <= R2U_NATIVE_TABLE_MAX_COUNT;
}

static bool count_near(ut32 count, size_t expected) {
	return expected && count && count <= expected * 2 && (expected < 8 || count >= expected / 2);
}

static bool validate_code_table(R2UnityNativeView *view, ut64 table_va, ut32 count) {
	if (!table_count_ok (count) || !table_va) {
		return false;
	}
	ut32 sample = R_MIN ((ut32)128, count);
	for (ut32 i = 0; i < sample; i++) {
		ut64 raw = 0;
		if (!read_ptr_at (view, table_va + (ut64)i * view->ptr_size, &raw)) {
			return false;
		}
		if (code_va_from_raw (view, raw)) {
			return true;
		}
	}
	return false;
}

static bool validate_data_table(R2UnityNativeView *view, ut64 table_va, ut32 count) {
	return table_count_ok (count) && table_va && view_ptr_at (view, table_va, NULL);
}

static bool validate_native_table(R2UnityNativeView *view, R2UnityNativeTableId id, ut64 table_va, ut32 count) {
	return id == R2U_NATIVE_TABLE_INTEROP_DATA || id == R2U_NATIVE_TABLE_WINDOWS_RUNTIME_FACTORY_TABLE
		? validate_data_table (view, table_va, count)
		: validate_code_table (view, table_va, count);
}

static bool read_native_pair(R2UnityNativeView *view, ut64 *at, ut32 *count, ut64 *table_va) {
	ut64 raw_table = 0;
	if (!read_count_ptr_pair (view, *at, count, &raw_table)) {
		return false;
	}
	*at += view->ptr_size == 8? 16: 8;
	*table_va = data_va_from_raw (view, raw_table);
	return true;
}

static size_t add_native_pair(R2UnityNativeView *view, ut64 *at, R2UnityNativeTableId id, R2UnityNativeTable *tables) {
	ut32 count = 0;
	ut64 table_va = 0;
	if (!read_native_pair (view, at, &count, &table_va) || !validate_native_table (view, id, table_va, count)) {
		return 0;
	}
	tables[id].va = table_va;
	tables[id].count = count;
	return 1;
}

static bool add_native_ptr(R2UnityNativeView *view, ut64 at, R2UnityNativeTableId id, R2UnityNativeTable *tables, ut32 count) {
	ut64 raw_table = 0;
	if (!read_ptr_at (view, at, &raw_table)) {
		return false;
	}
	ut64 table_va = data_va_from_raw (view, raw_table);
	if (!validate_native_table (view, id, table_va, count)) {
		return false;
	}
	tables[id].va = table_va;
	tables[id].count = count;
	return true;
}

static bool old_custom_attribute_pair(R2UnityNativeView *view, ut64 *at) {
	ut32 count = 0;
	ut64 table_va = 0;
	return read_native_pair (view, at, &count, &table_va)
		&& (!count || validate_code_table (view, table_va, count));
}

static size_t resolve_native_tables_at(R2UnityNativeView *view, ut64 at, R2UnityNativeTable *tables, bool adjustor_thunks, int unresolved_mode) {
	memset (tables, 0, R2U_NATIVE_TABLE_COUNT * sizeof (*tables));
	size_t score = add_native_pair (view, &at, R2U_NATIVE_TABLE_REVERSE_PINVOKE_WRAPPERS, tables);
	score += add_native_pair (view, &at, R2U_NATIVE_TABLE_GENERIC_METHOD_POINTERS, tables);
	if (adjustor_thunks) {
		ut32 count = (ut32)tables[R2U_NATIVE_TABLE_GENERIC_METHOD_POINTERS].count;
		score += add_native_ptr (view, at, R2U_NATIVE_TABLE_GENERIC_ADJUSTOR_THUNKS, tables, count)? 1: 0;
		at += view->ptr_size;
	}
	score += add_native_pair (view, &at, R2U_NATIVE_TABLE_INVOKER_POINTERS, tables);
	if (unresolved_mode == 2 && !old_custom_attribute_pair (view, &at)) {
		return 0;
	}
	if (unresolved_mode == 0) {
		score += add_native_pair (view, &at, R2U_NATIVE_TABLE_UNRESOLVED_INSTANCE_CALL_POINTERS, tables);
		score += add_native_pair (view, &at, R2U_NATIVE_TABLE_UNRESOLVED_STATIC_CALL_POINTERS, tables);
	} else {
		score += add_native_pair (view, &at, R2U_NATIVE_TABLE_UNRESOLVED_VIRTUAL_CALL_POINTERS, tables);
	}
	score += add_native_pair (view, &at, R2U_NATIVE_TABLE_INTEROP_DATA, tables);
	score += add_native_pair (view, &at, R2U_NATIVE_TABLE_WINDOWS_RUNTIME_FACTORY_TABLE, tables);
	return score;
}

static bool code_registration_has_method_prefix(R2UnityMetadata *meta, R2UnityNativeView *view, ut64 code_registration_va) {
	ut64 at = code_registration_va;
	ut32 count = 0;
	ut64 table_va = 0;
	size_t method_count = (size_t)r2unity_metadata_section_count (meta, R2U_SEC_METHODS);
	return read_native_pair (view, &at, &count, &table_va)
		&& count_near (count, method_count)
		&& validate_code_table (view, table_va, count);
}

static void parse_code_registration_tables(R2UnityMetadata *meta, R2UnityNativeView *view, ut64 code_registration_va, R2UnityNativeResult *result) {
	if (meta->version <= 24 && code_registration_has_method_prefix (meta, view, code_registration_va)) {
		code_registration_va += view->ptr_size == 8? 16: 8;
	}
	R2UnityNativeTable candidate[R2U_NATIVE_TABLE_COUNT];
	size_t best_score = 0;
	int first = meta->version >= 29? 0: 1;
	int last = meta->version >= 31? 1: 3;
	for (int adjustor = 0; adjustor < 2; adjustor++) {
		for (int mode = first; mode < last; mode++) {
			size_t score = resolve_native_tables_at (view, code_registration_va, candidate, adjustor, mode);
			if (score > best_score) {
				memcpy (result->tables, candidate, sizeof (result->tables));
				best_score = score;
			}
		}
	}
}

static size_t copy_codegen_modules(R2UnityNativeView *view, R2UnityMethodTables *tables, ut64 modules_va, ut64 *out_ptrs, bool require_names) {
	if (!modules_va || !out_ptrs) {
		return 0;
	}
	ut64 *candidate = R_NEWS0 (ut64, tables->method_count);
	bool *used_images = R_NEWS0 (bool, tables->image_count);
	if (!candidate || !used_images) {
		R_FREE (candidate);
		R_FREE (used_images);
		return 0;
	}
	size_t exact_matches = 0;
	size_t copied = 0;
	for (int pass = 0; pass < (require_names? 1: 2); pass++) {
		for (size_t i = 0; i < tables->image_count; i++) {
			ut64 raw_module = 0;
			if (!read_ptr_at (view, modules_va + (ut64)i * view->ptr_size, &raw_module)) {
				continue;
			}
			ut64 module_va = data_va_from_raw (view, raw_module);
			if (!module_va) {
				continue;
			}
			ut64 raw_name = 0;
			ut32 mcount = 0;
			ut64 raw_table = 0;
			if (!read_ptr_at (view, module_va, &raw_name)
				|| !read_count_ptr_pair (view, module_va + view->ptr_size, &mcount, &raw_table)) {
				continue;
			}
			char *module_name = read_cstr_at (view, data_va_from_raw (view, raw_name));
			int image_idx = image_index_by_name (tables, module_name);
			if (pass == 0) {
				if (image_idx < 0) {
					free (module_name);
					continue;
				}
				exact_matches++;
			} else if (image_idx >= 0 || exact_matches > 0 || require_names) {
				free (module_name);
				continue;
			} else {
				image_idx = (i < tables->image_count)? (int)i: -1;
			}
			free (module_name);
			if (image_idx < 0 || (size_t)image_idx >= tables->image_count || used_images[image_idx]) {
				continue;
			}
			size_t expected = image_method_count (tables, (size_t)image_idx);
			if (!expected || !mcount || mcount > tables->method_count * 2 || (expected > 8 && mcount < expected / 2)) {
				continue;
			}
			ut64 table_va = data_va_from_raw (view, raw_table);
			if (!table_va) {
				continue;
			}
			size_t n = copy_image_method_table (view, tables, (size_t)image_idx, table_va, mcount, candidate);
			if (n) {
				used_images[image_idx] = true;
				copied += n;
			}
		}
	}
	if (copied > 0) {
		memcpy (out_ptrs, candidate, tables->method_count * sizeof (ut64));
	}
	R_FREE (candidate);
	R_FREE (used_images);
	return copied;
}

static bool parse_codegen_modules(R2UnityMetadata *meta, R2UnityNativeView *view, ut64 code_registration_va, ut64 *out_ptrs, R2UnityNativeResult *result) {
	R2UnityMethodTables tables;
	if (!method_tables_init (&tables, meta)) {
		method_tables_fini (&tables);
		return false;
	}
	size_t pair_span = view->ptr_size == 8? 16: 8;
	for (ut64 off = 0; off + pair_span <= 0x280; off += 4) {
		ut32 count = 0;
		ut64 raw_modules = 0;
		if (!read_count_ptr_pair (view, code_registration_va + off, &count, &raw_modules) || count != tables.image_count || !raw_modules) {
			continue;
		}
		ut64 modules_va = data_va_from_raw (view, raw_modules);
		size_t copied = copy_codegen_modules (view, &tables, modules_va, out_ptrs, false);
		if (copied) {
			result->code_gen_modules_va = modules_va;
			result->method_pointers_va = modules_va;
			R_LOG_DEBUG ("CodeRegistration codeGenModules=0x%" PFMT64x " copied=%zu", modules_va, copied);
			method_tables_fini (&tables);
			return true;
		}
	}
	method_tables_fini (&tables);
	return false;
}

static bool parse_global_method_pointers(R2UnityMetadata *meta, R2UnityNativeView *view, ut64 code_registration_va, ut64 *out_ptrs, R2UnityNativeResult *result) {
	size_t method_count = (size_t)r2unity_metadata_section_count (meta, R2U_SEC_METHODS);
	if (!method_count) {
		return false;
	}
	size_t pair_span = view->ptr_size == 8? 16: 8;
	for (ut64 off = 0; off + pair_span <= 0x100; off += 4) {
		ut32 count = 0;
		ut64 raw_table = 0;
		if (!read_count_ptr_pair (view, code_registration_va + off, &count, &raw_table)) {
			continue;
		}
		if (!count || count > method_count * 2 || (method_count > 8 && count < method_count / 2)) {
			continue;
		}
		ut64 table_va = data_va_from_raw (view, raw_table);
		if (!table_va) {
			continue;
		}
		ut64 *candidate = R_NEWS0 (ut64, method_count);
		if (!candidate) {
			return false;
		}
		size_t seen = 0;
		size_t copied = copy_global_method_table (view, method_count, table_va, count, candidate, &seen);
		size_t sample = R_MIN ((size_t)128, R_MIN ((size_t)count, method_count));
		size_t min_good = sample < 8? 1: 8;
		if (copied >= min_good || (copied && seen >= min_good)) {
			memcpy (out_ptrs, candidate, method_count * sizeof (ut64));
			result->method_pointers_va = table_va;
			R_FREE (candidate);
			R_LOG_DEBUG ("CodeRegistration methodPointers=0x%" PFMT64x " count=%u copied=%zu", table_va, count, copied);
			return true;
		}
		R_FREE (candidate);
	}
	return false;
}

static bool parse_code_registration(R2UnityMetadata *meta, R2UnityNativeView *view, ut64 code_registration_va, R2UnityNativeSource source, R2UnityNativeResult *result) {
	size_t method_count = (size_t)r2unity_metadata_section_count (meta, R2U_SEC_METHODS);
	if (!method_count) {
		return false;
	}
	ut64 actual_code_va = 0;
	if (!view_ptr_at (view, code_registration_va, &actual_code_va)) {
		return false;
	}
	parse_code_registration_tables (meta, view, actual_code_va, result);
	ut64 *method_ptrs = R_NEWS0 (ut64, method_count);
	if (!method_ptrs) {
		return false;
	}
	bool ok = parse_codegen_modules (meta, view, actual_code_va, method_ptrs, result);
	if (!ok) {
		ok = parse_global_method_pointers (meta, view, actual_code_va, method_ptrs, result);
	}
	if (!ok) {
		R_FREE (method_ptrs);
		return false;
	}
	R_FREE (result->method_ptrs);
	result->method_ptrs = method_ptrs;
	result->has_method_ptrs = true;
	result->source = source;
	result->code_registration_va = actual_code_va;
	result->ptr_size = view->ptr_size;
	return true;
}

static bool symbol_matches(const char *name, const char *alias) {
	if (!name || !alias) {
		return false;
	}
	if (!strcmp (name, alias)) {
		return true;
	}
	if (*name == '_' && !strcmp (name + 1, alias)) {
		return true;
	}
	if (*alias == '_' && !strcmp (name, alias + 1)) {
		return true;
	}
	return false;
}

static bool symbol_matches_any(const char *name, const char *const *names) {
	if (!name || !names) {
		return false;
	}
	for (size_t i = 0; names[i]; i++) {
		if (symbol_matches (name, names[i])) {
			return true;
		}
	}
	return false;
}

static ut64 resolve_override(const R2UnityNativeOptions *options, const char *const *names) {
	if (!options || !names) {
		return 0;
	}
	for (size_t i = 0; i < options->symbols_count; i++) {
		if (symbol_matches_any (options->symbols[i].name, names)) {
			return options->symbols[i].va;
		}
	}
	return 0;
}

static void take_heuristic_result(R2UnityNativeResult *result, ut64 *method_ptrs, int ptr_size) {
	R_FREE (result->method_ptrs);
	result->method_ptrs = method_ptrs;
	result->has_method_ptrs = true;
	result->source = R2U_NATIVE_SOURCE_HEURISTIC;
	result->ptr_size = ptr_size;
}

typedef struct {
	RBinFile *bf;
	RVecRBinSection *sections;
	ut64 cache_paddr;
	ut64 cache_size;
	ut8 cache[R2U_RBIN_CACHE_SIZE];
	ut8 scratch[8];
} R2UnityRBinView;

static bool rbin_section_contains(const RBinSection *s, ut64 va) {
	ut64 vsize = s->vsize? s->vsize: s->size;
	return vsize && va >= s->vaddr && va < s->vaddr + vsize;
}

static bool rbin_va_to_paddr(R2UnityRBinView *rv, ut64 va, ut64 *paddr) {
	if (rv->sections) {
		RBinSection *s;
		R_VEC_FOREACH (rv->sections, s) {
			if (!rbin_section_contains (s, va)) {
				continue;
			}
			ut64 delta = va - s->vaddr;
			if (delta < s->size) {
				*paddr = s->paddr + delta;
				return true;
			}
		}
	}
	ut64 sz = r_buf_size (rv->bf->buf);
	if (va < sz) {
		*paddr = va;
		return true;
	}
	return false;
}

static const ut8 *rbin_ptr_at(void *user, ut64 va) {
	R2UnityRBinView *rv = (R2UnityRBinView *)user;
	ut64 paddr = 0;
	if (!rv || !rv->bf || !rv->bf->buf || !rbin_va_to_paddr (rv, va, &paddr)) {
		return NULL;
	}
	if (paddr > UT64_MAX - sizeof (rv->scratch)) {
		return NULL;
	}
	ut64 end = paddr + sizeof (rv->scratch);
	if (rv->cache_size && paddr >= rv->cache_paddr) {
		ut64 delta = paddr - rv->cache_paddr;
		if (delta <= rv->cache_size && sizeof (rv->scratch) <= rv->cache_size - delta) {
			return rv->cache + delta;
		}
	}
	ut64 bsz = r_buf_size (rv->bf->buf);
	if (paddr < bsz) {
		ut64 base = paddr & ~(ut64)(R2U_RBIN_CACHE_SIZE - 1);
		ut64 len = R_MIN ((ut64)R2U_RBIN_CACHE_SIZE, bsz - base);
		if (r_buf_read_at (rv->bf->buf, base, rv->cache, len) == (st64)len) {
			rv->cache_paddr = base;
			rv->cache_size = len;
			if (end <= base + len) {
				return rv->cache + (paddr - base);
			}
		}
	}
	if (r_buf_read_at (rv->bf->buf, paddr, rv->scratch, sizeof (rv->scratch)) != (st64)sizeof (rv->scratch)) {
		return NULL;
	}
	return rv->scratch;
}

static int rbin_ptr_size(RBin *bin) {
	RBinInfo *info = r_bin_get_info (bin);
	return (info && info->bits == 32)? 4: 8;
}

static ut64 rbin_base_vaddr(RBinFile *bf, RVecRBinSection *sections) {
	ut64 base = bf? r_bin_file_get_baddr (bf): 0;
	if (base) {
		return base;
	}
	ut64 lo = UT64_MAX;
	if (sections) {
		RBinSection *s;
		R_VEC_FOREACH (sections, s) {
			if (s->vaddr && s->vaddr < lo) {
				lo = s->vaddr;
			}
		}
	}
	return lo == UT64_MAX? 0: lo;
}

static void rbin_text_range(RVecRBinSection *sections, ut64 *text_lo, ut64 *text_hi) {
	ut64 lo = UT64_MAX;
	ut64 hi = 0;
	if (sections) {
		RBinSection *s;
		R_VEC_FOREACH (sections, s) {
			ut64 vsize = s->vsize? s->vsize: s->size;
			if (!vsize || !(s->perm & R_PERM_X)) {
				continue;
			}
			if (s->vaddr < lo) {
				lo = s->vaddr;
			}
			if (s->vaddr + vsize > hi) {
				hi = s->vaddr + vsize;
			}
		}
		if (lo == UT64_MAX || hi <= lo) {
			R_VEC_FOREACH (sections, s) {
				ut64 vsize = s->vsize? s->vsize: s->size;
				if (!vsize) {
					continue;
				}
				if (s->vaddr < lo) {
					lo = s->vaddr;
				}
				if (s->vaddr + vsize > hi) {
					hi = s->vaddr + vsize;
				}
			}
		}
	}
	*text_lo = lo == UT64_MAX? 0: lo;
	*text_hi = hi;
}

static R2UnityNativeSection *rbin_sections(RVecRBinSection *sections, size_t *out_count) {
	if (out_count) {
		*out_count = 0;
	}
	if (!sections) {
		return NULL;
	}
	size_t count = 0;
	RBinSection *s;
	R_VEC_FOREACH (sections, s) {
		count++;
	}
	R2UnityNativeSection *out = R_NEWS0 (R2UnityNativeSection, count);
	if (!out) {
		return NULL;
	}
	size_t i = 0;
	R_VEC_FOREACH (sections, s) {
		ut64 vsize = s->vsize? s->vsize: s->size;
		out[i].name = s->name;
		out[i].vaddr = s->vaddr;
		out[i].vsize = vsize;
		out[i].size = R_MIN (s->size, vsize);
		out[i].perm = s->perm;
		out[i].is_data = s->is_data;
		i++;
	}
	if (out_count) {
		*out_count = count;
	}
	return out;
}

static bool bin_name_matches(RBinName *name, const char *const *aliases) {
	return name && (symbol_matches_any (name->name, aliases)
		|| symbol_matches_any (name->oname, aliases)
		|| symbol_matches_any (name->fname, aliases));
}

static ut64 rbin_find_symbol(RBinFile *bf, const char *const *aliases) {
	RVecRBinSymbol *symbols = r_bin_file_get_symbols_vec (bf);
	if (!symbols) {
		return 0;
	}
	RBinSymbol *sym;
	R_VEC_FOREACH (symbols, sym) {
		if (!bin_name_matches (sym->name, aliases)) {
			continue;
		}
		ut64 va = sym->vaddr? sym->vaddr: r_bin_file_get_vaddr (bf, sym->paddr, sym->vaddr);
		if (va) {
			return va;
		}
	}
	return 0;
}

static bool rbin_probe_table(R2UnityNativeView *view, ut64 arrptr, ut32 count, ut32 min_seen, ut32 min_good, size_t method_count, ut64 *candidate) {
	ut32 sample = R_MIN ((ut32)128, count);
	ut32 good = 0;
	ut32 seen = 0;
	for (ut32 k = 0; k < sample; k++) {
		ut64 raw = 0;
		if (!read_ptr_at (view, arrptr + (ut64)k * view->ptr_size, &raw)) {
			return false;
		}
		if (raw) {
			seen++;
		}
		if (code_va_from_raw (view, raw)) {
			good++;
		}
	}
	if (seen < min_seen || good < min_good) {
		return false;
	}
	memset (candidate, 0, method_count * sizeof (ut64));
	size_t tocopy = R_MIN ((size_t)count, method_count);
	size_t copied = 0;
	for (size_t i = 0; i < tocopy; i++) {
		ut64 raw = 0;
		if (!read_ptr_at (view, arrptr + (ut64)i * view->ptr_size, &raw)) {
			break;
		}
		ut64 addr = code_va_from_raw (view, raw);
		if (addr) {
			candidate[i] = addr;
			copied++;
		}
	}
	return copied >= R_MIN ((size_t)8, tocopy);
}

static bool section_can_scan(const R2UnityNativeSection *s, ut64 min_size) {
	if (!s || s->vaddr + s->size < s->vaddr) {
		return false;
	}
	return !(s->perm & R_PERM_X) && (s->is_data || (s->perm & R_PERM_R)) && s->size >= min_size;
}

static bool section_can_scan_codegen(const R2UnityNativeSection *s, ut64 min_size) {
	if (!section_can_scan (s, min_size)) {
		return false;
	}
	if (!s->name) {
		return s->perm & R_PERM_W;
	}
	return !strncmp (s->name, ".data.rel.ro", 12) || !strcmp (s->name, ".rdata") || !strncmp (s->name, "__DATA", 6);
}

static bool sections_contain_data(const R2UnityNativeSection *sections, size_t section_count, ut64 va, ut64 size) {
	if (!va || !size || va + size < va) {
		return false;
	}
	for (size_t i = 0; i < section_count; i++) {
		const R2UnityNativeSection *s = &sections[i];
		if (section_can_scan_codegen (s, size) && va >= s->vaddr && va + size <= s->vaddr + s->size) {
			return true;
		}
	}
	return false;
}

static bool scan_codegen_modules(R2UnityMetadata *meta, R2UnityNativeView *view, const R2UnityNativeSection *sections, size_t section_count, R2UnityNativeResult *result) {
	R2UnityMethodTables tables;
	ut64 *method_ptrs = NULL;
	if (!method_tables_init (&tables, meta) || !sections || !section_count) {
		goto done;
	}
	if (tables.image_count > UT64_MAX / view->ptr_size) {
		goto done;
	}
	method_ptrs = R_NEWS0 (ut64, tables.method_count);
	if (!method_ptrs) {
		goto done;
	}
	ut64 modules_size = (ut64)tables.image_count * view->ptr_size;
	size_t probes = 0;
	size_t pair_span = view->ptr_size == 8? 16: 8;
	ut64 step = view->ptr_size == 8? 8: 4;
	for (size_t i = 0; i < section_count && probes < R2U_CODEGEN_SCAN_MAX_PROBES; i++) {
		const R2UnityNativeSection *s = &sections[i];
		if (!section_can_scan_codegen (s, pair_span)) {
			continue;
		}
		for (ut64 off = 0; off + pair_span <= s->size && probes < R2U_CODEGEN_SCAN_MAX_PROBES; off += step) {
			ut32 count = 0;
			ut64 raw_modules = 0;
			if (!read_count_ptr_pair (view, s->vaddr + off, &count, &raw_modules) || count != tables.image_count || !raw_modules) {
				continue;
			}
			ut64 modules_va = data_va_from_raw (view, raw_modules);
			if (!sections_contain_data (sections, section_count, modules_va, modules_size)) {
				continue;
			}
			probes++;
			size_t copied = copy_codegen_modules (view, &tables, modules_va, method_ptrs, true);
			if (copied) {
				result->code_gen_modules_va = modules_va;
				result->method_pointers_va = modules_va;
				take_heuristic_result (result, method_ptrs, view->ptr_size);
				R_LOG_DEBUG ("heuristic codeGenModules=0x%" PFMT64x " copied=%zu", modules_va, copied);
				method_tables_fini (&tables);
				return true;
			}
		}
	}
done:
	method_tables_fini (&tables);
	R_FREE (method_ptrs);
	return false;
}

static bool scan_sections(R2UnityMetadata *meta, R2UnityNativeView *view, const R2UnityNativeSection *sections, size_t section_count, R2UnityNativeResult *result) {
	size_t method_count = (size_t)r2unity_metadata_section_count (meta, R2U_SEC_METHODS);
	if (!method_count || !sections || !section_count) {
		return false;
	}
	ut64 *candidate = R_NEWS0 (ut64, method_count);
	if (!candidate) {
		return false;
	}
	bool found = false;
	ut32 expected = (ut32)R_MAX ((ut64)64, (ut64)method_count);
	for (int pass = 0; pass < 2 && !found; pass++) {
		const ut32 min_count = pass == 0? 32: 64;
		const ut64 min_secsize = pass == 0? (ut64)(16 + view->ptr_size * 2): (ut64)(8 + view->ptr_size);
		for (size_t i = 0; i < section_count && !found; i++) {
			const R2UnityNativeSection *s = &sections[i];
			if (!section_can_scan (s, min_secsize)) {
				continue;
			}
			for (ut64 off = 0; off + min_secsize <= s->size; off += 4) {
				ut32 cnt = 0;
				if (!read_u32_at (view, s->vaddr + off, &cnt) || cnt < min_count) {
					continue;
				}
				if (pass == 1 && expected && (cnt > expected * 2 || cnt < expected / 2)) {
					continue;
				}
				ut64 raw_table = 0;
				if (!read_ptr_at (view, s->vaddr + off + (view->ptr_size == 8? 8: 4), &raw_table)) {
					continue;
				}
				ut64 table_va = data_va_from_raw (view, raw_table);
				if (!table_va) {
					continue;
				}
				ut32 sample = R_MIN ((ut32)128, cnt);
				ut32 min_seen = pass == 0? 8: sample / 2;
				ut32 min_good = pass == 0? 8: (sample * 3) / 4;
				if (rbin_probe_table (view, table_va, cnt, min_seen, min_good, method_count, candidate)) {
					result->method_pointers_va = table_va;
					found = true;
					break;
				}
			}
		}
	}
	if (found) {
		take_heuristic_result (result, candidate, view->ptr_size);
		return true;
	}
	R_FREE (candidate);
	return false;
}

static R2UnityNativeSource resolve_native_anchors(const R2UnityNativeOptions *options, RBinFile *bf, R2UnityNativeResult *result) {
	ut64 code_registration_va = options? options->code_registration_va: 0;
	ut64 metadata_registration_va = options? options->metadata_registration_va: 0;
	R2UnityNativeSource source = R2U_NATIVE_SOURCE_OVERRIDE;
	if (!code_registration_va) {
		code_registration_va = resolve_override (options, code_registration_names);
	}
	if (!metadata_registration_va) {
		metadata_registration_va = resolve_override (options, metadata_registration_names);
	}
	if (!code_registration_va && bf) {
		code_registration_va = rbin_find_symbol (bf, code_registration_names);
		source = R2U_NATIVE_SOURCE_SYMBOL;
	}
	if (!metadata_registration_va && bf) {
		metadata_registration_va = rbin_find_symbol (bf, metadata_registration_names);
	}
	result->code_registration_va = code_registration_va;
	result->metadata_registration_va = metadata_registration_va;
	return source;
}

static bool try_code_registration(R2UnityMetadata *meta, R2UnityNativeView *view, const R2UnityNativeOptions *options, R2UnityNativeResult *result, R2UnityNativeSource source) {
	if ((options && options->force_heuristic) || !result->code_registration_va) {
		return false;
	}
	return parse_code_registration (meta, view, result->code_registration_va, source, result);
}

bool r2unity_native_run_view(R2UnityMetadata *meta, R2UnityNativeView *view, const R2UnityNativeSection *sections, size_t section_count, const R2UnityNativeOptions *options, R2UnityNativeResult *result) {
	R_RETURN_VAL_IF_FAIL (meta && view && result, false);
	memset (result, 0, sizeof (*result));
	result->ptr_size = view->ptr_size;
	R2UnityNativeSource source = resolve_native_anchors (options, NULL, result);
	if (try_code_registration (meta, view, options, result, source)) {
		return true;
	}
	return scan_codegen_modules (meta, view, sections, section_count, result)
		|| scan_sections (meta, view, sections, section_count, result);
}

R_API const char *r2unity_native_source_name(R2UnityNativeSource source) {
	switch (source) {
	case R2U_NATIVE_SOURCE_SYMBOL: return "symbol";
	case R2U_NATIVE_SOURCE_OVERRIDE: return "override";
	case R2U_NATIVE_SOURCE_HEURISTIC: return "heuristic";
	default: return "none";
	}
}

R_API const char *r2unity_native_table_name(R2UnityNativeTableId id) {
	return id >= 0 && id < R2U_NATIVE_TABLE_COUNT? native_table_names[id]: NULL;
}

R_API const char *const *r2unity_native_code_registration_names(void) {
	return code_registration_names;
}

R_API const char *const *r2unity_native_metadata_registration_names(void) {
	return metadata_registration_names;
}

R_API void r2unity_native_result_fini(R2UnityNativeResult *result) {
	if (!result) {
		return;
	}
	R_FREE (result->method_ptrs);
	memset (result, 0, sizeof (*result));
}

R_API bool r2unity_find_method_pointers_rbin(R2UnityMetadata *meta, RBin *bin, RBinFile *bf, const R2UnityNativeOptions *options, R2UnityNativeResult *result) {
	R_RETURN_VAL_IF_FAIL (meta && bin && result, false);
	memset (result, 0, sizeof (*result));
	if (!bf) {
		bf = r_bin_cur (bin);
	}
	if (!bf || !bf->buf) {
		return false;
	}
	(void)r_bin_patch_relocs (bf);
	RVecRBinSection *sections = r_bin_file_get_sections_vec (bf);
	int ptr_size = rbin_ptr_size (bin);
	ut64 text_lo = 0;
	ut64 text_hi = 0;
	rbin_text_range (sections, &text_lo, &text_hi);
	R2UnityRBinView rv = {
		.bf = bf,
		.sections = sections
	};
	R2UnityNativeView view = {
		.user = &rv,
		.ptr_at = rbin_ptr_at,
		.ptr_size = ptr_size,
		.base_vaddr = rbin_base_vaddr (bf, sections),
		.text_lo = text_lo,
		.text_hi = text_hi
	};
	size_t section_count = 0;
	R2UnityNativeSection *native_sections = rbin_sections (sections, &section_count);
	result->ptr_size = ptr_size;
	R2UnityNativeSource source = resolve_native_anchors (options, bf, result);
	bool ok = try_code_registration (meta, &view, options, result, source)
		|| scan_codegen_modules (meta, &view, native_sections, section_count, result)
		|| scan_sections (meta, &view, native_sections, section_count, result);
	R_FREE (native_sections);
	return ok;
}

R_API bool r2unity_find_method_pointers(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result) {
	R_RETURN_VAL_IF_FAIL (meta && path && result, false);
	memset (result, 0, sizeof (*result));
	RBin *bin = r_bin_new ();
	bool ok = false;
	if (bin) {
		RIO *io = r_io_new ();
		if (io) {
			r_libstore_load (bin->libstore);
			r_io_bind (io, &bin->iob);
		}
		RBinFileOptions opt;
		r_bin_file_options_init (&opt, -1, 0, 0, 0);
		if (io) {
			ok = r_bin_open (bin, path, &opt);
			if (ok) {
				ok = r2unity_find_method_pointers_rbin (meta, bin, r_bin_cur (bin), options, result);
			}
		}
		r_io_free (io);
		r_bin_free (bin);
	}
	if (ok) {
		return true;
	}
	return r2unity_find_method_pointers_simple (meta, path, options, result);
}

R_API bool r2unity_find_method_pointers_simple(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result) {
	R_RETURN_VAL_IF_FAIL (meta && path && result, false);
	memset (result, 0, sizeof (*result));
	if (path && *path) {
		ut8 magic[4] = { 0 };
		FILE *fp = fopen (path, "rb");
		if (fp) {
			(void)fread (magic, 1, sizeof (magic), fp);
			fclose (fp);
		}
		if (!memcmp (magic, "\x7f""ELF", 4)) {
			return r2unity_find_method_pointers_elf (meta, path, options, result);
		}
		ut32 m = r_read_le32 (magic);
		if (m == 0xfeedfacf || m == 0xcffaedfe || m == 0xcafebabe || m == 0xbebafeca) {
			return r2unity_find_method_pointers_macho (meta, path, options, result);
		}
		if (magic[0] == 'M' && magic[1] == 'Z') {
			return r2unity_find_method_pointers_pe (meta, path, options, result);
		}
	}
	return false;
}

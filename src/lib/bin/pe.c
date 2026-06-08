/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.pe"

#include "native_internal.h"
#include <string.h>

typedef struct {
	char name[9];
	ut64 vaddr;
	ut64 vsize;
	ut64 fileoff;
	ut64 filesize;
	ut32 chars;
} PeSec;

typedef struct {
	ut8 *file;
	ut64 size;
	bool is64;
	ut64 image_base;
	PeSec secs[128];
	size_t nsecs;
} PeImg;

static bool pe_load(const char *path, PeImg *pe) {
	memset (pe, 0, sizeof (*pe));
	size_t size = 0;
	pe->file = (ut8 *)r_file_slurp (path, &size);
	if (!pe->file || size < 0x40) {
		R_FREE (pe->file);
		return false;
	}
	pe->size = size;
	if (pe->file[0] != 'M' || pe->file[1] != 'Z') {
		R_FREE (pe->file);
		return false;
	}
	ut32 e_lfanew = r_read_le32 (pe->file + 0x3c);
	if ((ut64)e_lfanew + 24 > pe->size) {
		R_FREE (pe->file);
		return false;
	}
	const ut8 *nt = pe->file + e_lfanew;
	if (r_read_le32 (nt) != 0x4550) {
		R_FREE (pe->file);
		return false;
	}
	ut16 nsec = r_read_le16 (nt + 6);
	ut16 optsz = r_read_le16 (nt + 20);
	ut64 optoff = (ut64)e_lfanew + 24;
	if (optoff + optsz > pe->size) {
		R_FREE (pe->file);
		return false;
	}
	const ut8 *opt = pe->file + optoff;
	ut16 magic = r_read_le16 (opt);
	if (magic == 0x20b) {
		pe->is64 = true;
		pe->image_base = r_read_le64 (opt + 24);
	} else if (magic == 0x10b) {
		pe->image_base = r_read_le32 (opt + 28);
	} else {
		R_FREE (pe->file);
		return false;
	}
	ut64 shoff = optoff + optsz;
	for (ut16 i = 0; i < nsec && pe->nsecs < R_ARRAY_SIZE (pe->secs); i++) {
		ut64 off = shoff + (ut64)i * 40;
		if (off + 40 > pe->size) {
			break;
		}
		const ut8 *sh = pe->file + off;
		PeSec *s = &pe->secs[pe->nsecs++];
		memcpy (s->name, sh, 8);
		s->name[8] = 0;
		s->vsize = r_read_le32 (sh + 8);
		s->vaddr = pe->image_base + r_read_le32 (sh + 12);
		s->filesize = r_read_le32 (sh + 16);
		s->fileoff = r_read_le32 (sh + 20);
		s->chars = r_read_le32 (sh + 36);
	}
	return pe->nsecs > 0;
}

static void pe_free(PeImg *pe) {
	R_FREE (pe->file);
}

static const ut8 *pe_ptr_at(void *user, ut64 va) {
	PeImg *pe = (PeImg *)user;
	for (size_t i = 0; i < pe->nsecs; i++) {
		const PeSec *s = &pe->secs[i];
		ut64 vsize = s->vsize? s->vsize: s->filesize;
		if (va < s->vaddr || va >= s->vaddr + vsize) {
			continue;
		}
		ut64 delta = va - s->vaddr;
		if (delta + 8 <= s->filesize && s->fileoff + delta + 8 <= pe->size) {
			return pe->file + s->fileoff + delta;
		}
		return NULL;
	}
	return NULL;
}

static bool pe_is_data_section(const PeSec *s) {
	if (s->chars & 0x20000000) {
		return false;
	}
	return !strncmp (s->name, ".data", 5) || !strncmp (s->name, ".rdata", 6) || (s->chars & 0x40);
}

static void pe_text_range(PeImg *pe, ut64 *lo, ut64 *hi) {
	*lo = UT64_MAX;
	*hi = 0;
	for (size_t i = 0; i < pe->nsecs; i++) {
		const PeSec *s = &pe->secs[i];
		if ((s->chars & 0x20000000) || !strncmp (s->name, ".text", 5)) {
			*lo = R_MIN (*lo, s->vaddr);
			*hi = R_MAX (*hi, s->vaddr + (s->vsize? s->vsize: s->filesize));
		}
	}
	if (*lo != UT64_MAX && *hi > *lo) {
		return;
	}
	*lo = UT64_MAX;
	*hi = 0;
	for (size_t i = 0; i < pe->nsecs; i++) {
		*lo = R_MIN (*lo, pe->secs[i].vaddr);
		ut64 vsize = pe->secs[i].vsize? pe->secs[i].vsize: pe->secs[i].filesize;
		*hi = R_MAX (*hi, pe->secs[i].vaddr + vsize);
	}
	if (*lo == UT64_MAX) {
		*lo = pe->image_base;
	}
}

static R2UnityNativeSection *pe_sections(PeImg *pe, size_t *count) {
	*count = pe->nsecs;
	R2UnityNativeSection *out = R_NEWS0 (R2UnityNativeSection, pe->nsecs);
	if (!out) {
		*count = 0;
		return NULL;
	}
	for (size_t i = 0; i < pe->nsecs; i++) {
		const PeSec *s = &pe->secs[i];
		ut64 vsize = s->vsize? s->vsize: s->filesize;
		out[i].vaddr = s->vaddr;
		out[i].vsize = vsize;
		out[i].size = R_MIN (s->filesize, vsize);
		out[i].perm = ((s->chars & 0x40000000)? R_PERM_R: 0) | ((s->chars & 0x80000000)? R_PERM_W: 0) | ((s->chars & 0x20000000)? R_PERM_X: 0);
		out[i].is_data = pe_is_data_section (s);
	}
	return out;
}

bool r2unity_find_method_pointers_pe(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result) {
	if (!meta || !path || !result) {
		return false;
	}
	PeImg pe;
	if (!pe_load (path, &pe)) {
		return false;
	}
	ut64 text_lo = 0, text_hi = 0;
	pe_text_range (&pe, &text_lo, &text_hi);
	R2UnityNativeView view = {
		.user = &pe,
		.ptr_at = pe_ptr_at,
		.ptr_size = pe.is64? 8: 4,
		.base_vaddr = pe.image_base,
		.text_lo = text_lo,
		.text_hi = text_hi
	};
	size_t section_count = 0;
	R2UnityNativeSection *sections = pe_sections (&pe, &section_count);
	bool ok = r2unity_native_run_view (meta, &view, sections, section_count, options, result);
	R_FREE (sections);
	pe_free (&pe);
	return ok;
}

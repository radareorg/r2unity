/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.elf"

#include "native_internal.h"
#include <string.h>

typedef struct {
	ut64 vaddr;
	ut64 memsz;
	ut64 offset;
	ut64 filesz;
	ut32 flags;
	ut32 type;
} ElfSeg;

typedef struct {
	ut8 *file;
	ut64 size;
	bool is64;
	ElfSeg segs[128];
	size_t nsegs;
} ElfImg;

static void elf_write_word(ut8 *p, ut64 v, bool is64) {
	if (is64) {
		r_write_le64 (p, v);
	} else {
		r_write_le32 (p, (ut32)v);
	}
}

static bool elf_load(const char *path, ElfImg *e) {
	memset (e, 0, sizeof (*e));
	size_t size = 0;
	e->file = (ut8 *)r_file_slurp (path, &size);
	if (!e->file || size < 0x40) {
		R_FREE (e->file);
		return false;
	}
	e->size = size;
	if (memcmp (e->file, "\x7f""ELF", 4) || e->file[5] != 1) {
		R_FREE (e->file);
		return false;
	}
	e->is64 = e->file[4] == 2;
	if (e->is64) {
		ut64 phoff = r_read_le64 (e->file + 0x20);
		ut16 phentsz = r_read_le16 (e->file + 0x36);
		ut16 phnum = r_read_le16 (e->file + 0x38);
		for (ut16 i = 0; i < phnum && e->nsegs < R_ARRAY_SIZE (e->segs); i++) {
			ut64 off = phoff + (ut64)i * phentsz;
			if (off + 56 > e->size) {
				break;
			}
			const ut8 *ph = e->file + off;
			ut32 type = r_read_le32 (ph);
			if (type != 1 && type != 2) {
				continue;
			}
			ElfSeg *s = &e->segs[e->nsegs++];
			s->type = type;
			s->flags = r_read_le32 (ph + 4);
			s->offset = r_read_le64 (ph + 8);
			s->vaddr = r_read_le64 (ph + 16);
			s->filesz = r_read_le64 (ph + 32);
			s->memsz = r_read_le64 (ph + 40);
		}
	} else {
		ut32 phoff = r_read_le32 (e->file + 0x1c);
		ut16 phentsz = r_read_le16 (e->file + 0x2a);
		ut16 phnum = r_read_le16 (e->file + 0x2c);
		for (ut16 i = 0; i < phnum && e->nsegs < R_ARRAY_SIZE (e->segs); i++) {
			ut64 off = (ut64)phoff + (ut64)i * phentsz;
			if (off + 32 > e->size) {
				break;
			}
			const ut8 *ph = e->file + off;
			ut32 type = r_read_le32 (ph);
			if (type != 1 && type != 2) {
				continue;
			}
			ElfSeg *s = &e->segs[e->nsegs++];
			s->type = type;
			s->offset = r_read_le32 (ph + 4);
			s->vaddr = r_read_le32 (ph + 8);
			s->filesz = r_read_le32 (ph + 16);
			s->memsz = r_read_le32 (ph + 20);
			s->flags = r_read_le32 (ph + 24);
		}
	}
	return true;
}

static void elf_free(ElfImg *e) {
	R_FREE (e->file);
}

static const ut8 *elf_ptr_at_size(ElfImg *e, ut64 va, ut64 size) {
	for (size_t i = 0; i < e->nsegs; i++) {
		const ElfSeg *s = &e->segs[i];
		ut64 memsz = s->memsz? s->memsz: s->filesz;
		if (va < s->vaddr || va >= s->vaddr + memsz) {
			continue;
		}
		ut64 delta = va - s->vaddr;
		if (delta + size <= s->filesz && s->offset + delta + size <= e->size) {
			return e->file + s->offset + delta;
		}
		return NULL;
	}
	return NULL;
}

static const ut8 *elf_ptr_at(void *user, ut64 va) {
	return elf_ptr_at_size ((ElfImg *)user, va, 8);
}

static void elf_ranges(ElfImg *e, ut64 *base, ut64 *text_lo, ut64 *text_hi) {
	*base = UT64_MAX;
	*text_lo = UT64_MAX;
	*text_hi = 0;
	for (size_t i = 0; i < e->nsegs; i++) {
		const ElfSeg *s = &e->segs[i];
		if (s->vaddr < *base) {
			*base = s->vaddr;
		}
		if ((s->flags & 1) && (s->flags & 4)) {
			*text_lo = R_MIN (*text_lo, s->vaddr);
			*text_hi = R_MAX (*text_hi, s->vaddr + (s->memsz? s->memsz: s->filesz));
		}
	}
	if (*base == UT64_MAX) {
		*base = 0;
	}
	if (*text_lo == UT64_MAX || *text_hi <= *text_lo) {
		*text_lo = *base;
		for (size_t i = 0; i < e->nsegs; i++) {
			ut64 memsz = e->segs[i].memsz? e->segs[i].memsz: e->segs[i].filesz;
			*text_hi = R_MAX (*text_hi, e->segs[i].vaddr + memsz);
		}
	}
}

static R2UnityNativeSection *elf_sections(ElfImg *e, size_t *count) {
	*count = e->nsegs;
	R2UnityNativeSection *out = R_NEWS0 (R2UnityNativeSection, e->nsegs);
	if (!out) {
		*count = 0;
		return NULL;
	}
	for (size_t i = 0; i < e->nsegs; i++) {
		const ElfSeg *s = &e->segs[i];
		ut64 memsz = s->memsz? s->memsz: s->filesz;
		out[i].vaddr = s->vaddr;
		out[i].vsize = memsz;
		out[i].size = R_MIN (s->filesz, memsz);
		out[i].perm = ((s->flags & 4)? R_PERM_R: 0) | ((s->flags & 2)? R_PERM_W: 0) | ((s->flags & 1)? R_PERM_X: 0);
		out[i].is_data = (s->flags & 2) && !(s->flags & 1);
	}
	return out;
}

static void elf_apply_relocs(ElfImg *e, ut64 base) {
	ut64 dyn_off = 0, dyn_sz = 0;
	for (size_t i = 0; i < e->nsegs; i++) {
		if (e->segs[i].type == 2) {
			dyn_off = e->segs[i].offset;
			dyn_sz = e->segs[i].filesz;
			break;
		}
	}
	if (!dyn_off || !dyn_sz || dyn_off + dyn_sz > e->size) {
		return;
	}
	ut64 rela_off = 0, rela_sz = 0, rela_ent = e->is64? 24: 12;
	ut64 rel_off = 0, rel_sz = 0, rel_ent = e->is64? 16: 8;
	ut64 relr_off = 0, relr_sz = 0, relr_ent = e->is64? 8: 4;
	const ut8 *d = e->file + dyn_off;
	ut64 dyn_ent = e->is64? 16: 8;
	for (ut64 off = 0; off + dyn_ent <= dyn_sz; off += dyn_ent) {
		ut64 tag = e->is64? r_read_le64 (d + off): (ut64)r_read_le32 (d + off);
		ut64 val = e->is64? r_read_le64 (d + off + 8): (ut64)r_read_le32 (d + off + 4);
		if (!tag) {
			break;
		}
		switch (tag) {
		case 7: rela_off = val; break;
		case 8: rela_sz = val; break;
		case 9: rela_ent = val; break;
		case 17: rel_off = val; break;
		case 18: rel_sz = val; break;
		case 19: rel_ent = val; break;
		case 35: relr_sz = val; break;
		case 36: relr_off = val; break;
		case 37: relr_ent = val; break;
		default: break;
		}
	}
	const ut64 type_mask = e->is64? 0xffffffffULL: 0xffULL;
	for (ut64 i = 0; rela_off && rela_ent && i + rela_ent <= rela_sz; i += rela_ent) {
		const ut8 *rp = elf_ptr_at_size (e, rela_off + i, rela_ent);
		if (!rp) {
			break;
		}
		ut64 r_offset = e->is64? r_read_le64 (rp): (ut64)r_read_le32 (rp);
		ut64 r_info = e->is64? r_read_le64 (rp + 8): (ut64)r_read_le32 (rp + 4);
		ut64 r_addend = e->is64? r_read_le64 (rp + 16): (ut64)r_read_le32 (rp + 8);
		ut64 type = r_info & type_mask;
		if (type == 8 || type == 23 || type == 1027) {
			ut8 *loc = (ut8 *)elf_ptr_at_size (e, r_offset, e->is64? 8: 4);
			if (loc) {
				elf_write_word (loc, base + r_addend, e->is64);
			}
		}
	}
	for (ut64 i = 0; rel_off && rel_ent && i + rel_ent <= rel_sz; i += rel_ent) {
		const ut8 *rp = elf_ptr_at_size (e, rel_off + i, rel_ent);
		if (!rp) {
			break;
		}
		ut64 r_offset = e->is64? r_read_le64 (rp): (ut64)r_read_le32 (rp);
		ut64 r_info = e->is64? r_read_le64 (rp + 8): (ut64)r_read_le32 (rp + 4);
		ut64 type = r_info & type_mask;
		if (type == 8 || type == 23 || type == 1027) {
			ut8 *loc = (ut8 *)elf_ptr_at_size (e, r_offset, e->is64? 8: 4);
			if (loc) {
				ut64 addend = e->is64? r_read_le64 (loc): (ut64)r_read_le32 (loc);
				elf_write_word (loc, base + addend, e->is64);
			}
		}
	}
	if (!e->is64 || !relr_off || !relr_sz || !relr_ent) {
		return;
	}
	ut64 curr = 0;
	for (ut64 i = 0; i + relr_ent <= relr_sz; i += relr_ent) {
		const ut8 *rp = elf_ptr_at_size (e, relr_off + i, relr_ent);
		if (!rp) {
			break;
		}
		ut64 r = r_read_le64 (rp);
		if (!(r & 1)) {
			ut8 *loc = (ut8 *)elf_ptr_at_size (e, r, 8);
			if (loc) {
				elf_write_word (loc, base + r_read_le64 (loc), true);
			}
			curr = r + 8;
			continue;
		}
		ut64 bitmap = r >> 1;
		for (int bit = 0; bit < 63; bit++) {
			if (bitmap & (1ULL << bit)) {
				ut8 *loc = (ut8 *)elf_ptr_at_size (e, curr + (ut64)(bit + 1) * 8, 8);
				if (loc) {
					elf_write_word (loc, base + r_read_le64 (loc), true);
				}
			}
		}
		curr += 8 * 63;
	}
}

bool r2unity_find_method_pointers_elf(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result) {
	if (!meta || !path || !result) {
		return false;
	}
	ElfImg e;
	if (!elf_load (path, &e)) {
		return false;
	}
	ut64 base = 0, text_lo = 0, text_hi = 0;
	elf_ranges (&e, &base, &text_lo, &text_hi);
	elf_apply_relocs (&e, base);
	R2UnityNativeView view = {
		.user = &e,
		.ptr_at = elf_ptr_at,
		.ptr_size = e.is64? 8: 4,
		.base_vaddr = base,
		.text_lo = text_lo,
		.text_hi = text_hi
	};
	size_t section_count = 0;
	R2UnityNativeSection *sections = elf_sections (&e, &section_count);
	view.sections = sections;
	view.section_count = section_count;
	bool ok = r2unity_native_resolve (meta, &view, NULL, options, result);
	R_FREE (sections);
	elf_free (&e);
	return ok;
}

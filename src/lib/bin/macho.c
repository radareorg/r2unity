/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.macho"

#include "native_internal.h"
#include <string.h>

typedef struct {
	char name[17];
	ut64 vmaddr;
	ut64 vmsize;
	ut64 fileoff;
	ut64 filesize;
	ut32 maxprot;
} MachSeg;

typedef struct {
	ut8 *file;
	ut64 size;
	ut64 base;
	ut64 vm_base;
	MachSeg segs[128];
	size_t nsegs;
} MachO;

static bool macho_load(const char *path, MachO *mo) {
	memset (mo, 0, sizeof (*mo));
	size_t size = 0;
	mo->file = (ut8 *)r_file_slurp (path, &size);
	if (!mo->file || size < 0x20) {
		R_FREE (mo->file);
		return false;
	}
	mo->size = size;
	ut32 magic = r_read_be32 (mo->file);
	ut64 off = 0;
	if (magic == 0xcafebabe || magic == 0xbebafeca) {
		ut32 nfat = r_read_be32 (mo->file + 4);
		ut32 best = 0;
		for (ut32 i = 0; i < nfat && 8 + (ut64)i * 20 + 20 <= mo->size; i++) {
			const ut8 *fa = mo->file + 8 + (ut64)i * 20;
			if (r_read_be32 (fa) == 0x0100000c) {
				best = i;
				break;
			}
		}
		const ut8 *fa = mo->file + 8 + (ut64)best * 20;
		off = r_read_be32 (fa + 8);
	}
	if (off + 0x20 > mo->size) {
		R_FREE (mo->file);
		return false;
	}
	mo->base = off;
	const ut8 *mh = mo->file + off;
	if (r_read_le32 (mh) != 0xfeedfacf) {
		R_FREE (mo->file);
		return false;
	}
	ut32 ncmds = r_read_le32 (mh + 0x10);
	ut64 co = off + 0x20;
	for (ut32 i = 0; i < ncmds && co + 8 <= mo->size; i++) {
		ut32 cmd = r_read_le32 (mo->file + co);
		ut32 cmdsz = r_read_le32 (mo->file + co + 4);
		if (!cmdsz || co + cmdsz > mo->size) {
			break;
		}
		if (cmd == 0x19 && cmdsz >= 72 && mo->nsegs < R_ARRAY_SIZE (mo->segs)) {
			const ut8 *sp = mo->file + co + 8;
			MachSeg *s = &mo->segs[mo->nsegs++];
			memcpy (s->name, sp, 16);
			s->name[16] = 0;
			s->vmaddr = r_read_le64 (sp + 16);
			s->vmsize = r_read_le64 (sp + 24);
			s->fileoff = r_read_le64 (sp + 32);
			s->filesize = r_read_le64 (sp + 40);
			s->maxprot = r_read_le32 (sp + 48);
			if (!mo->vm_base || s->vmaddr < mo->vm_base) {
				mo->vm_base = s->vmaddr;
			}
		}
		co += cmdsz;
	}
	return mo->nsegs > 0;
}

static void macho_free(MachO *mo) {
	R_FREE (mo->file);
}

static const ut8 *macho_ptr_at(void *user, ut64 va) {
	MachO *mo = (MachO *)user;
	for (size_t i = 0; i < mo->nsegs; i++) {
		const MachSeg *s = &mo->segs[i];
		ut64 vsize = s->vmsize? s->vmsize: s->filesize;
		if (va < s->vmaddr || va >= s->vmaddr + vsize) {
			continue;
		}
		ut64 delta = va - s->vmaddr;
		if (delta + 8 <= s->filesize && mo->base + s->fileoff + delta + 8 <= mo->size) {
			return mo->file + mo->base + s->fileoff + delta;
		}
		return NULL;
	}
	return NULL;
}

static void macho_text_range(MachO *mo, ut64 *lo, ut64 *hi) {
	*lo = UT64_MAX;
	*hi = 0;
	for (size_t i = 0; i < mo->nsegs; i++) {
		const MachSeg *s = &mo->segs[i];
		if ((s->maxprot & 4) || !strncmp (s->name, "__TEXT", 6)) {
			*lo = R_MIN (*lo, s->vmaddr);
			*hi = R_MAX (*hi, s->vmaddr + (s->vmsize? s->vmsize: s->filesize));
		}
	}
	if (*lo != UT64_MAX && *hi > *lo) {
		return;
	}
	*lo = UT64_MAX;
	*hi = 0;
	for (size_t i = 0; i < mo->nsegs; i++) {
		*lo = R_MIN (*lo, mo->segs[i].vmaddr);
		ut64 vsize = mo->segs[i].vmsize? mo->segs[i].vmsize: mo->segs[i].filesize;
		*hi = R_MAX (*hi, mo->segs[i].vmaddr + vsize);
	}
	if (*lo == UT64_MAX) {
		*lo = 0;
	}
}

static R2UnityNativeSection *macho_sections(MachO *mo, size_t *count) {
	*count = mo->nsegs;
	R2UnityNativeSection *out = R_NEWS0 (R2UnityNativeSection, mo->nsegs);
	if (!out) {
		*count = 0;
		return NULL;
	}
	for (size_t i = 0; i < mo->nsegs; i++) {
		const MachSeg *s = &mo->segs[i];
		ut64 vsize = s->vmsize? s->vmsize: s->filesize;
		out[i].name = s->name;
		out[i].vaddr = s->vmaddr;
		out[i].vsize = vsize;
		out[i].size = R_MIN (s->filesize, vsize);
		out[i].perm = ((s->maxprot & 1)? R_PERM_R: 0) | ((s->maxprot & 2)? R_PERM_W: 0) | ((s->maxprot & 4)? R_PERM_X: 0);
		out[i].is_data = (s->maxprot & 1) && !(s->maxprot & 4);
	}
	return out;
}

bool r2unity_find_method_pointers_macho(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result) {
	if (!meta || !path || !result) {
		return false;
	}
	MachO mo;
	if (!macho_load (path, &mo)) {
		return false;
	}
	ut64 text_lo = 0, text_hi = 0;
	macho_text_range (&mo, &text_lo, &text_hi);
	R2UnityNativeView view = {
		.user = &mo,
		.ptr_at = macho_ptr_at,
		.ptr_size = 8,
		.base_vaddr = mo.vm_base,
		.text_lo = text_lo,
		.text_hi = text_hi
	};
	size_t section_count = 0;
	R2UnityNativeSection *sections = macho_sections (&mo, &section_count);
	view.sections = sections;
	view.section_count = section_count;
	bool ok = r2unity_native_resolve (meta, &view, NULL, options, result);
	R_FREE (sections);
	macho_free (&mo);
	return ok;
}

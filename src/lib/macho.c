// Fast Mach-O parser for r2unity
#include "lib.h"
#include <string.h>

typedef struct {
	char segname[16];
	ut64 vmaddr;
	ut64 vmsize;
	ut64 fileoff;
	ut64 filesize;
	ut32 maxprot;
} MachSeg;

typedef struct {
	ut8 *file;
	ut64 vm_base; // lowest VM address among segments (usually __TEXT vmaddr)
	ut64 filesize;
	ut64 base;
	ut32 ncmds;
	ut64 cmd_off;
	MachSeg segs[128];
	int nsegs;
} MachO;

static bool macho_load(const char *path, MachO *mo) {
	memset (mo, 0, sizeof (*mo));
	size_t sz = 0;
	mo->file = (ut8 *)r_file_slurp (path, &sz);
	if (!mo->file || sz == 0) {
		R_FREE (mo->file);
		return false;
	}
	mo->filesize = (ut64)sz;
	ut32 magic = r_read_be32 (mo->file);
	ut64 off = 0;
	if (magic == 0xcafebabe) {
		ut32 nfat = r_read_be32 (mo->file + 4);
		ut32 best = 0;
		for (ut32 i = 0; i < nfat; i++) {
			const ut8 *fa = mo->file + 8 + i * 20;
			ut32 cputype = r_read_be32 (fa + 0);
			if (cputype == 0x0100000c) {
				best = i;
				break;
			}
		}
		const ut8 *fa = mo->file + 8 + best * 20;
		off = r_read_be32 (fa + 8);
	}
	mo->base = off;
	const ut8 *p = mo->file + off;
	ut32 mh_magic = r_read_le32 (p);
	if (mh_magic != 0xFEEDFACF) {
		R_FREE (mo->file);
		return false;
	}
	ut32 ncmds = r_read_le32 (p + 0x10);
	mo->ncmds = ncmds;
	mo->cmd_off = off + 0x20;
	ut64 co = mo->cmd_off;
	for (ut32 i = 0; i < ncmds && (co + 8 <= mo->filesize); i++) {
		ut32 cmd = r_read_le32 (mo->file + co);
		ut32 cmdsize = r_read_le32 (mo->file + co + 4);
		if (cmd == 0x19 && cmdsize >= 72) {
			if (mo->nsegs < (int) (sizeof (mo->segs) / sizeof (mo->segs[0]))) {
				MachSeg *s = &mo->segs[mo->nsegs++];
				const ut8 *sp = mo->file + co + 8;
				memcpy (s->segname, sp, 16);
				s->vmaddr = r_read_le64 (sp + 16);
				s->vmsize = r_read_le64 (sp + 24);
				s->fileoff = r_read_le64 (sp + 32);
				s->filesize = r_read_le64 (sp + 40);
				s->maxprot = r_read_le32 (sp + 48);
				if (!mo->vm_base || s->vmaddr < mo->vm_base) {
					mo->vm_base = s->vmaddr;
				}
			}
		}
		co += cmdsize? cmdsize: 8;
	}
	return true;
}

static void macho_free(MachO *mo) {
	R_FREE (mo->file);
}

static inline bool macho_vm_in_text(MachO *mo, ut64 addr, ut64 *text_lo, ut64 *text_hi) {
	ut64 lo = UT64_MAX, hi = 0;
	for (int i = 0; i < mo->nsegs; i++) {
		MachSeg *s = &mo->segs[i];
		bool is_text = (s->maxprot & 0x4) || !strncmp (s->segname, "__TEXT", 6);
		if (is_text) {
			if (s->vmaddr < lo) {
				lo = s->vmaddr;
			}
			if (s->vmaddr + s->vmsize > hi) {
				hi = s->vmaddr + s->vmsize;
			}
		}
	}
	// Fallback: if no exec segment detected, use min/max of all segments
	if (lo == UT64_MAX || hi <= lo) {
		lo = UT64_MAX;
		hi = 0;
		for (int i = 0; i < mo->nsegs; i++) {
			MachSeg *s = &mo->segs[i];
			if (s->vmaddr < lo) {
				lo = s->vmaddr;
			}
			if (s->vmaddr + s->vmsize > hi) {
				hi = s->vmaddr + s->vmsize;
			}
		}
	}
	if (text_lo) {
		*text_lo = lo;
	}
	if (text_hi) {
		*text_hi = hi;
	}
	return (addr >= lo && addr < hi);
}

static inline const ut8 *macho_vm_to_ptr(MachO *mo, ut64 vmaddr) {
	for (int i = 0; i < mo->nsegs; i++) {
		MachSeg *s = &mo->segs[i];
		if (vmaddr >= s->vmaddr && vmaddr < s->vmaddr + s->vmsize) {
			ut64 delta = vmaddr - s->vmaddr;
			if (delta < s->filesize) {
				return mo->file + mo->base + s->fileoff + delta;
			}
			return NULL;
		}
	}
	return NULL;
}

/* Evaluate the table at VA `arrptr` with `count` 8-byte pointers. Entries
 * below text_lo are retried with vm_base added (RVA→VA fallback). */
static bool macho_probe_table(MachO *mo, ut64 arrptr, ut32 count, ut64 text_lo, ut64 text_hi, ut32 min_seen, ut32 min_good, size_t method_count, ut64 *candidates) {
	ut32 sample = R_MIN ((ut32)128, count);
	ut32 good = 0, seen = 0;
	for (ut32 k = 0; k < sample; k++) {
		const ut8 *p = macho_vm_to_ptr (mo, arrptr + (ut64)k * 8);
		if (!p && mo->vm_base) {
			p = macho_vm_to_ptr (mo, mo->vm_base + arrptr + (ut64)k * 8);
		}
		if (!p) {
			return false;
		}
		ut64 val = r_read_le64 (p);
		if (val && val < text_lo && mo->vm_base && (val + mo->vm_base) >= text_lo && (val + mo->vm_base) < text_hi) {
			val += mo->vm_base;
		}
		if (val) {
			seen++;
		}
		if (val >= text_lo && val < text_hi) {
			good++;
		}
	}
	if (seen < min_seen || good < min_good) {
		return false;
	}
	memset (candidates, 0, method_count * sizeof (ut64));
	size_t tocopy = R_MIN ((size_t)count, method_count);
	for (size_t m = 0; m < tocopy; m++) {
		const ut8 *p = macho_vm_to_ptr (mo, arrptr + (ut64)m * 8);
		if (!p && mo->vm_base) {
			p = macho_vm_to_ptr (mo, mo->vm_base + arrptr + (ut64)m * 8);
		}
		if (!p) {
			break;
		}
		ut64 val = r_read_le64 (p);
		if (val && val < text_lo && mo->vm_base && (val + mo->vm_base) >= text_lo && (val + mo->vm_base) < text_hi) {
			val += mo->vm_base;
		}
		if (val >= text_lo && val < text_hi) {
			candidates[m] = val;
		}
	}
	return true;
}

R_API bool r2unity_find_method_pointers_macho(R2UnityMetadata *meta, const char *macho_path, ut64 **out_ptrs) {
	R_RETURN_VAL_IF_FAIL (meta && macho_path && out_ptrs, false);
	*out_ptrs = NULL;
	MachO mo;
	if (!macho_load (macho_path, &mo)) {
		return false;
	}
	size_t method_count = (ut64)meta->methodsSize / sizeof (Il2CppMethodDefinition);
	if (!method_count) {
		macho_free (&mo);
		return false;
	}
	ut64 text_lo = 0, text_hi = 0;
	macho_vm_in_text (&mo, 0, &text_lo, &text_hi);
	ut64 *candidates = R_NEWS (ut64, method_count);
	if (!candidates) {
		macho_free (&mo);
		return false;
	}
	bool found = false;
	int ptrsz = 8;
	ut32 expected = (ut32)R_MAX ((ut64)64, (ut64)method_count);
	(void)ptrsz; /* Mach-O fast path is 64-bit only. */
	/* Pass 0: CodeRegistration-shaped pair {count32, pad, ptr64}.
	 * Pass 1: generic {count32, ptr64} with expected-count bounds and
	 * stricter sample-fraction thresholds. */
	for (int pass = 0; pass < 2 && !found; pass++) {
		const ut32 min_count = (pass == 0)? 32: 64;
		for (int i = 0; i < mo.nsegs && !found; i++) {
			MachSeg *s = &mo.segs[i];
			bool is_data = (s->maxprot & 0x1) && ! (s->maxprot & 0x4);
			if (!is_data || s->filesize < 16) {
				continue;
			}
			const ut8 *buf = mo.file + mo.base + s->fileoff;
			for (ut64 off = 0; off + 16 <= s->filesize; off += 4) {
				ut32 cnt = r_read_le32 (buf + off);
				if (cnt < min_count) {
					continue;
				}
				if (pass == 1 && expected && (cnt > expected * 2 || cnt < expected / 2)) {
					continue;
				}
				ut64 arrptr = r_read_le64 (buf + off + 8);
				ut32 sample = R_MIN ((ut32)128, cnt);
				ut32 min_seen = (pass == 0)? 8: sample / 2;
				ut32 min_good = (pass == 0)? 8: (sample * 3) / 4;
				if (macho_probe_table (&mo, arrptr, cnt, text_lo, text_hi, min_seen, min_good, method_count, candidates)) {
					if (r2unity_is_debug ()) {
						fprintf (stderr, "[r2unity/macho] pass=%d arrptr=0x%" PFMT64x " cnt=%u\n", pass, arrptr, cnt);
					}
					found = true;
					break;
				}
			}
		}
	}
	if (!found) {
		R_FREE (candidates);
		macho_free (&mo);
		return false;
	}
	*out_ptrs = candidates;
	macho_free (&mo);
	return true;
}

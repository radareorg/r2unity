/* r2unity - MIT - Copyright 2025-2026 - pancake */

// Minimal PE parser for r2unity (GameAssembly.dll on Windows, GameAssembly.dll on UWP).
// PE .dll images are already relocated at link time (preferred image base is
// concrete), so no relocation fixup is required — pointers in .data / .rdata
// are absolute VAs relative to the ImageBase.
#define R_LOG_ORIGIN "r2unity.pe"
#include "lib.h"
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
	ut64 filesize;
	int is64;
	ut64 image_base;
	PeSec secs[128];
	int nsecs;
} PeImg;

static bool pe_load(const char *path, PeImg *pe) {
	memset (pe, 0, sizeof (*pe));
	size_t sz = 0;
	pe->file = (ut8 *)r_file_slurp (path, &sz);
	if (!pe->file || sz < 0x40) {
		R_FREE (pe->file);
		return false;
	}
	pe->filesize = (ut64)sz;
	if (pe->file[0] != 'M' || pe->file[1] != 'Z') {
		R_FREE (pe->file);
		return false;
	}
	ut32 e_lfanew = r_read_le32 (pe->file + 0x3C);
	if ((ut64)e_lfanew + 24 > pe->filesize) {
		R_FREE (pe->file);
		return false;
	}
	const ut8 *nt = pe->file + e_lfanew;
	if (r_read_le32 (nt) != 0x00004550) { /* "PE\0\0" */
		R_FREE (pe->file);
		return false;
	}
	/* COFF file header immediately follows the 4-byte PE signature */
	ut16 nsec = r_read_le16 (nt + 4 + 2);
	ut16 sizeof_opt = r_read_le16 (nt + 4 + 16);
	ut64 opt_off = (ut64)e_lfanew + 4 + 20;
	if (opt_off + sizeof_opt > pe->filesize) {
		R_FREE (pe->file);
		return false;
	}
	const ut8 *opt = pe->file + opt_off;
	ut16 mag = r_read_le16 (opt);
	if (mag == 0x20b) {
		pe->is64 = 1;
		pe->image_base = r_read_le64 (opt + 24);
	} else if (mag == 0x10b) {
		pe->is64 = 0;
		pe->image_base = (ut64)r_read_le32 (opt + 28);
	} else {
		R_FREE (pe->file);
		return false;
	}
	ut64 sh_off = opt_off + sizeof_opt;
	for (ut16 i = 0; i < nsec && pe->nsecs < (int) (sizeof (pe->secs) / sizeof (pe->secs[0])); i++) {
		ut64 off = sh_off + (ut64)i * 40;
		if (off + 40 > pe->filesize) {
			break;
		}
		const ut8 *sh = pe->file + off;
		PeSec *s = &pe->secs[pe->nsecs++];
		memcpy (s->name, sh, 8);
		s->name[8] = 0;
		s->vsize = r_read_le32 (sh + 8);
		ut32 rva = r_read_le32 (sh + 12);
		s->vaddr = pe->image_base + (ut64)rva;
		s->filesize = r_read_le32 (sh + 16);
		s->fileoff = r_read_le32 (sh + 20);
		s->chars = r_read_le32 (sh + 36);
	}
	return true;
}

static void pe_free(PeImg *pe) {
	R_FREE (pe->file);
}

static inline const ut8 *pe_vm_to_ptr(PeImg *pe, ut64 vaddr) {
	for (int i = 0; i < pe->nsecs; i++) {
		PeSec *s = &pe->secs[i];
		if (vaddr >= s->vaddr && vaddr < s->vaddr + s->vsize) {
			ut64 delta = vaddr - s->vaddr;
			if (delta < s->filesize && s->fileoff + delta < pe->filesize) {
				return pe->file + s->fileoff + delta;
			}
			return NULL;
		}
	}
	return NULL;
}

static inline void pe_text_range(PeImg *pe, ut64 *text_lo, ut64 *text_hi) {
	ut64 lo = UT64_MAX, hi = 0;
	for (int i = 0; i < pe->nsecs; i++) {
		PeSec *s = &pe->secs[i];
		/* IMAGE_SCN_MEM_EXECUTE = 0x20000000 */
		bool is_text = (s->chars & 0x20000000) || !strncmp (s->name, ".text", 5);
		if (is_text) {
			if (s->vaddr < lo) {
				lo = s->vaddr;
			}
			if (s->vaddr + s->vsize > hi) {
				hi = s->vaddr + s->vsize;
			}
		}
	}
	if (lo == UT64_MAX || hi <= lo) {
		lo = UT64_MAX;
		hi = 0;
		for (int i = 0; i < pe->nsecs; i++) {
			PeSec *s = &pe->secs[i];
			if (s->vaddr < lo) {
				lo = s->vaddr;
			}
			if (s->vaddr + s->vsize > hi) {
				hi = s->vaddr + s->vsize;
			}
		}
	}
	*text_lo = lo;
	*text_hi = hi;
}

static inline bool pe_is_data_section(const PeSec *s) {
	/* Skip executable sections; keep initialized-data / .data / .rdata */
	if (s->chars & 0x20000000) {
		return false;
	}
	if (!strncmp (s->name, ".data", 5)) {
		return true;
	}
	if (!strncmp (s->name, ".rdata", 6)) {
		return true;
	}
	/* IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040 */
	return (s->chars & 0x40) != 0;
}

/* Validate the table at VA `arrptr` (`count` pointers): require at least
 * `min_seen` non-zero entries and `min_good` entries landing in [text_lo, text_hi)
 * within a 128-entry sample. On match, copy the first method_count entries
 * into candidates (clamping to count) and return true. */
static bool pe_probe_table(PeImg *pe, ut64 arrptr, ut32 count, int ptrsz, ut64 text_lo, ut64 text_hi, ut32 min_seen, ut32 min_good, size_t method_count, ut64 *candidates) {
	ut32 sample = R_MIN ((ut32)128, count);
	ut32 good = 0, seen = 0;
	for (ut32 k = 0; k < sample; k++) {
		const ut8 *p = pe_vm_to_ptr (pe, arrptr + (ut64)k *(ut64)ptrsz);
		if (!p) {
			return false;
		}
		ut64 val = (ptrsz == 8)? r_read_le64 (p): (ut64)r_read_le32 (p);
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
	size_t in_text = 0;
	for (size_t m = 0; m < tocopy; m++) {
		const ut8 *p = pe_vm_to_ptr (pe, arrptr + (ut64)m *(ut64)ptrsz);
		if (!p) {
			break;
		}
		ut64 val = (ptrsz == 8)? r_read_le64 (p): (ut64)r_read_le32 (p);
		if (val >= text_lo && val < text_hi) {
			candidates[m] = val;
			in_text++;
		}
	}
	return in_text >= 8;
}

R_API bool r2unity_find_method_pointers_pe(R2UnityMetadata *meta, const char *pe_path, ut64 **out_ptrs) {
	R_RETURN_VAL_IF_FAIL (meta && pe_path && out_ptrs, false);
	*out_ptrs = NULL;
	PeImg pe;
	if (!pe_load (pe_path, &pe)) {
		return false;
	}
	size_t method_count = (ut64)meta->methodsSize / sizeof (Il2CppMethodDefinition);
	if (!method_count) {
		pe_free (&pe);
		return false;
	}
	ut64 text_lo = 0, text_hi = 0;
	pe_text_range (&pe, &text_lo, &text_hi);
	if (text_lo >= text_hi) {
		pe_free (&pe);
		return false;
	}
	ut64 *candidates = R_NEWS (ut64, method_count);
	if (!candidates) {
		pe_free (&pe);
		return false;
	}
	bool found = false;
	int ptrsz = pe.is64? 8: 4;
	ut32 expected = (ut32)R_MAX ((ut64)64, (ut64) ((ut64)meta->methodsSize / sizeof (Il2CppMethodDefinition)));

	/* Pass 1: CodeRegistration-shaped pair {count32, pad?, ptr}, loose
	 * thresholds (small absolute floor). Pass 2: generic {count32, ptr}
	 * fallback with stricter sample-fraction thresholds. */
	for (int pass = 0; pass < 2 && !found; pass++) {
		const ut32 min_count = (pass == 0)? 32: 64;
		for (int i = 0; i < pe.nsecs && !found; i++) {
			PeSec *s = &pe.secs[i];
			if (!pe_is_data_section (s) || s->filesize < (ut64) (8 + ptrsz)) {
				continue;
			}
			const ut8 *buf = pe.file + s->fileoff;
			for (ut64 off = 0; off + (ut64) (8 + ptrsz) <= s->filesize; off += 4) {
				ut32 cnt = r_read_le32 (buf + off);
				if (cnt < min_count) {
					continue;
				}
				if (pass == 1 && expected && (cnt > expected * 2 || cnt < expected / 2)) {
					continue;
				}
				ut64 arrptr = (ptrsz == 8)
					? r_read_le64 (buf + off + 8)
					: (ut64)r_read_le32 (buf + off + 4);
				ut32 sample = R_MIN ((ut32)128, cnt);
				ut32 min_seen = (pass == 0)? 8: sample / 2;
				ut32 min_good = (pass == 0)? 8: (sample * 3) / 4;
				if (pe_probe_table (&pe, arrptr, cnt, ptrsz, text_lo, text_hi, min_seen, min_good, method_count, candidates)) {
					R_LOG_DEBUG ("[pe] pass=%d arrptr=0x%" PFMT64x " cnt=%u", pass, arrptr, cnt);
					found = true;
					break;
				}
			}
		}
	}

	if (!found) {
		R_FREE (candidates);
		pe_free (&pe);
		return false;
	}
	*out_ptrs = candidates;
	pe_free (&pe);
	return true;
}

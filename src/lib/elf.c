// Fast ELF parser for r2unity
#include "lib.h"
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static inline ut32 RD_LE32 (const ut8 *p) {
	return ((ut32)p[0]) | ((ut32)p[1] << 8) | ((ut32)p[2] << 16) | ((ut32)p[3] << 24);
}
static inline ut64 RD_LE64 (const ut8 *p) {
	return ((ut64)p[0]) | ((ut64)p[1] << 8) | ((ut64)p[2] << 16) | ((ut64)p[3] << 24) |
		((ut64)p[4] << 32) | ((ut64)p[5] << 40) | ((ut64)p[6] << 48) | ((ut64)p[7] << 56);
}
static inline ut16 RD_LE16 (const ut8 *p) {
	return (ut16)((ut16)p[0] | ((ut16)p[1] << 8));
}

typedef struct {
	ut64 vaddr;
	ut64 memsz;
	ut64 offset;
	ut64 filesz;
	ut32 flags;
	ut32 p_type;
} ElfSeg;

typedef struct {
	ut8 *file;
	ut64 filesize;
	int is64;
	int le;
	ElfSeg segs[128];
	int nsegs;
} ElfImg;

static bool elf_load (const char *path, ElfImg *e) {
	memset (e, 0, sizeof (*e));
	int fd = open (path, O_RDONLY);
	if (fd < 0) return false;
	struct stat st;
	if (fstat (fd, &st) != 0 || st.st_size <= 0) { close (fd); return false; }
	e->filesize = (ut64) st.st_size;
	e->file = R_NEWS (ut8, e->filesize);
	if (!e->file) { close (fd); return false; }
	ssize_t rd = read (fd, e->file, (size_t) e->filesize);
	close (fd);
	if (rd < 0 || (ut64) rd != e->filesize) { R_FREE (e->file); return false; }
	const ut8 *p = e->file;
	if (!(p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')) { R_FREE (e->file); return false; }
	e->is64 = (p[4] == 2);
	e->le = (p[5] == 1);
	if (!e->le) { R_FREE (e->file); return false; }
	if (e->is64) {
		ut64 phoff = RD_LE64 (p + 0x20);
		ut16 phentsize = RD_LE16 (p + 0x36);
		ut16 phnum = RD_LE16 (p + 0x38);
		for (ut16 i = 0; i < phnum && e->nsegs < (int)(sizeof (e->segs)/sizeof (e->segs[0])); i++) {
			ut64 off = phoff + (ut64) i * phentsize;
			if (off + 56 > e->filesize) break;
			const ut8 *ph = p + off;
			ut32 p_type = RD_LE32 (ph + 0);
			ut32 p_flags = RD_LE32 (ph + 4);
			ut64 p_offset = RD_LE64 (ph + 8);
			ut64 p_vaddr = RD_LE64 (ph + 16);
			ut64 p_filesz = RD_LE64 (ph + 32);
			ut64 p_memsz = RD_LE64 (ph + 40);
			if (p_type == 1 || p_type == 2) {
				ElfSeg *s = &e->segs[e->nsegs++];
				s->vaddr = p_vaddr; s->memsz = p_memsz; s->offset = p_offset; s->filesz = p_filesz; s->flags = p_flags; s->p_type = p_type;
			}
		}
	} else {
		ut32 phoff = RD_LE32 (p + 0x1C);
		ut16 phentsize = RD_LE16 (p + 0x2A);
		ut16 phnum = RD_LE16 (p + 0x2C);
		for (ut16 i = 0; i < phnum && e->nsegs < (int)(sizeof (e->segs)/sizeof (e->segs[0])); i++) {
			ut64 off = (ut64) phoff + (ut64) i * phentsize;
			if (off + 32 > e->filesize) break;
			const ut8 *ph = p + off;
			ut32 p_type = RD_LE32 (ph + 0);
			ut32 p_offset = RD_LE32 (ph + 4);
			ut32 p_vaddr = RD_LE32 (ph + 8);
			ut32 p_filesz = RD_LE32 (ph + 16);
			ut32 p_memsz = RD_LE32 (ph + 20);
			ut32 p_flags = RD_LE32 (ph + 24);
			if (p_type == 1 || p_type == 2) {
				ElfSeg *s = &e->segs[e->nsegs++];
				s->vaddr = p_vaddr; s->memsz = p_memsz; s->offset = p_offset; s->filesz = p_filesz; s->flags = p_flags; s->p_type = p_type;
			}
		}
	}
	return true;
}

static void elf_free (ElfImg *e) { R_FREE (e->file); }

static inline const ut8 *elf_vm_to_ptr (ElfImg *e, ut64 vaddr) {
	for (int i = 0; i < e->nsegs; i++) {
		const ElfSeg *s = &e->segs[i];
		if (vaddr >= s->vaddr && vaddr < s->vaddr + s->memsz) {
			ut64 delta = vaddr - s->vaddr;
			if (delta < s->filesz && s->offset + delta < e->filesize) {
				return e->file + s->offset + delta;
			}
			return NULL;
		}
	}
	return NULL;
}

R_API bool r2unity_find_method_pointers_elf (R2UnityMetadata *meta, const char *elf_path, ut64 **out_ptrs) {
	if (!meta || !elf_path || !out_ptrs) return false;
	*out_ptrs = NULL;
	ElfImg e;
	if (!elf_load (elf_path, &e)) return false;
	size_t method_count = (ut64) meta->methodsSize / sizeof (Il2CppMethodDefinition);
	if (!method_count) { elf_free (&e); return false; }
	ut64 base_vaddr = UT64_MAX, text_lo = UT64_MAX, text_hi = 0;
	for (int i = 0; i < e.nsegs; i++) {
		const ElfSeg *s = &e.segs[i];
		if (s->vaddr < base_vaddr) base_vaddr = s->vaddr;
		if ((s->flags & 0x1) && (s->flags & 0x4)) {
			if (s->vaddr < text_lo) text_lo = s->vaddr;
			if (s->vaddr + s->memsz > text_hi) text_hi = s->vaddr + s->memsz;
		}
	}
	if (text_lo == UT64_MAX || text_hi <= text_lo) { elf_free (&e); return false; }
	ut64 *candidates = R_NEWS (ut64, method_count);
	if (!candidates) { elf_free (&e); return false; }
	bool found = false;
	int ptrsz = e.is64 ? 8 : 4;
	// Apply REL(A)/RELR (relative only)
	ut64 dyn_off = 0, dyn_sz = 0;
	for (int i = 0; i < e.nsegs; i++) { const ElfSeg *s = &e.segs[i]; if (s->p_type == 2) { dyn_off = s->offset; dyn_sz = s->filesz; break; } }
	ut64 rela_off = 0, rela_sz = 0, rela_ent = e.is64? 24: 12;
	ut64 rel_off = 0, rel_sz = 0, rel_ent = e.is64? 16: 8;
	ut64 relr_off = 0, relr_sz = 0, relr_ent = e.is64? 8: 4;
	ut64 base = base_vaddr;
	if (dyn_off && dyn_sz) {
		const ut8 *d = e.file + dyn_off;
		for (ut64 off = 0; off + (e.is64? 16: 8) <= dyn_sz; off += (e.is64? 16: 8)) {
			ut64 tag = e.is64 ? RD_LE64 (d + off) : (ut64) RD_LE32 (d + off);
			ut64 val = e.is64 ? RD_LE64 (d + off + 8) : (ut64) RD_LE32 (d + off + 4);
			if (tag == 0) break;
			if (tag == 7) rela_off = val; else if (tag == 8) rela_sz = val; else if (tag == 9) rela_ent = val;
			else if (tag == 17) rel_off = val; else if (tag == 18) rel_sz = val; else if (tag == 19) rel_ent = val;
			else if (tag == 36) relr_off = val; else if (tag == 35) relr_sz = val; else if (tag == 37) relr_ent = val;
		}
	}
	if (rela_off && rela_sz && rela_ent) {
		for (ut64 i = 0; i + rela_ent <= rela_sz; i += rela_ent) {
			const ut8 *rp = elf_vm_to_ptr (&e, rela_off + i); if (!rp) break;
			ut64 r_offset = e.is64 ? RD_LE64 (rp + 0) : (ut64) RD_LE32 (rp + 0);
			ut64 r_info = e.is64 ? RD_LE64 (rp + 8) : (ut64) RD_LE32 (rp + 4);
			ut64 r_addend = e.is64 ? RD_LE64 (rp + 16) : (ut64) RD_LE32 (rp + 8);
			ut64 type = e.is64 ? (r_info & 0xffffffffULL) : (r_info & 0xffULL);
			bool is_relative = (type == 8) || (type == 23) || (type == 1027) || (type == 8);
			if (!is_relative) continue;
			ut8 *loc = (ut8 *) elf_vm_to_ptr (&e, r_offset); if (!loc) continue;
			ut64 newv = r_addend;
			if (e.is64) { for (int b = 0; b < 8; b++) loc[b] = (ut8)((newv >> (8*b)) & 0xff); }
			else { ut32 v32 = (ut32) newv; for (int b = 0; b < 4; b++) loc[b] = (ut8)((v32 >> (8*b)) & 0xff); }
		}
	}
	if (rel_off && rel_sz && rel_ent) {
		for (ut64 i = 0; i + rel_ent <= rel_sz; i += rel_ent) {
			const ut8 *rp = elf_vm_to_ptr (&e, rel_off + i); if (!rp) break;
			ut64 r_offset = e.is64 ? RD_LE64 (rp + 0) : (ut64) RD_LE32 (rp + 0);
			ut64 r_info = e.is64 ? RD_LE64 (rp + 8) : (ut64) RD_LE32 (rp + 4);
			ut64 type = e.is64 ? (r_info & 0xffffffffULL) : (r_info & 0xffULL);
			bool is_relative = (type == 8) || (type == 23) || (type == 1027) || (type == 8);
			if (!is_relative) continue;
			ut8 *loc = (ut8 *) elf_vm_to_ptr (&e, r_offset); if (!loc) continue;
			ut64 add = e.is64 ? RD_LE64 (loc) : (ut64) RD_LE32 (loc);
			ut64 newv = add;
			if (e.is64) { for (int b = 0; b < 8; b++) loc[b] = (ut8)((newv >> (8*b)) & 0xff); }
			else { ut32 v32 = (ut32) newv; for (int b = 0; b < 4; b++) loc[b] = (ut8)((v32 >> (8*b)) & 0xff); }
		}
	}
	if (e.is64 && relr_off && relr_sz && relr_ent) {
		ut64 curr = 0;
		for (ut64 i = 0; i + relr_ent <= relr_sz; i += relr_ent) {
			const ut8 *rp = elf_vm_to_ptr (&e, relr_off + i); if (!rp) break;
			ut64 R = RD_LE64 (rp);
			if ((R & 1ULL) == 0) {
				ut64 addr = R; ut8 *loc = (ut8 *) elf_vm_to_ptr (&e, addr);
				if (loc) { ut64 add = RD_LE64 (loc); ut64 newv = add; for (int b = 0; b < 8; b++) loc[b] = (ut8)((newv >> (8*b)) & 0xff); }
				curr = addr + 8;
			} else {
				ut64 bitmap = R >> 1;
				for (int bit = 0; bit < 63; bit++) if (bitmap & (1ULL << bit)) {
					ut64 addr = curr + (ut64)(bit + 1) * 8ULL;
					ut8 *loc = (ut8 *) elf_vm_to_ptr (&e, addr); if (!loc) continue;
					ut64 add = RD_LE64 (loc); ut64 newv = add; for (int b = 0; b < 8; b++) loc[b] = (ut8)((newv >> (8*b)) & 0xff);
				}
				curr += 8ULL * 63ULL;
			}
		}
	}
	// Scan for [count][ptr]
	/* expected number of entries in a possible method pointer array
	 * use method_count computed earlier (methodsSize / sizeof(Il2CppMethodDefinition))
	 */
	ut32 expected = (ut32) R_MAX ((ut64)64, (ut64) ((ut64) meta->methodsSize / sizeof (Il2CppMethodDefinition)));
	for (int i = 0; i < e.nsegs && !found; i++) {
		const ElfSeg *s = &e.segs[i];
		bool is_data = (s->flags & 0x1) && !(s->flags & 0x4);
		if (!is_data || s->filesz < (ut64)(8 + ptrsz)) continue;
		const ut8 *buf = e.file + s->offset;
		ut64 sz = s->filesz;
		for (ut64 off = 0; off + (ut64)(8 + ptrsz) <= sz; off += 4) {
			ut32 cnt32 = RD_LE32 (buf + off);
			if (cnt32 == 0) continue;
			/* relax thresholds: accept arrays that are reasonably large compared to expected */
				if (cnt32 < 32) continue;
				if (expected && (cnt32 > expected * 10 || cnt32 < expected / 8)) continue;
			ut64 arrptr = (ptrsz == 8) ? RD_LE64 (buf + off + 8) : (ut64) RD_LE32 (buf + off + 4);
			ut32 good = 0, seen = 0;
			ut32 sample = R_MIN ((ut32)128, cnt32);
			for (ut32 k = 0; k < sample; k++) {
				const ut8 *pp = elf_vm_to_ptr (&e, arrptr + (ut64)k * (ut64)ptrsz);
				if (!pp && base_vaddr != UT64_MAX) { pp = elf_vm_to_ptr (&e, base_vaddr + arrptr + (ut64)k * (ut64)ptrsz); }
				if (!pp) break;
				ut64 val = (ptrsz == 8) ? RD_LE64 (pp) : (ut64) RD_LE32 (pp);
				if (val) seen++;
				if ((val >= text_lo && val < text_hi) || (base_vaddr != UT64_MAX && (val + base_vaddr) >= text_lo && (val + base_vaddr) < text_hi)) good++;
			}
			if (seen >= 8 && good >= 8) {
				memset (candidates, 0, method_count * sizeof (ut64));
				size_t tocopy = R_MIN ((size_t) cnt32, method_count);
				size_t in_text = 0;
				for (size_t m = 0; m < tocopy; m++) {
					const ut8 *pp = elf_vm_to_ptr (&e, arrptr + (ut64)m * (ut64)ptrsz);
					if (!pp && base_vaddr != UT64_MAX) { pp = elf_vm_to_ptr (&e, base_vaddr + arrptr + (ut64)m * (ut64)ptrsz); }
					if (!pp) break;
					ut64 val = (ptrsz == 8) ? RD_LE64 (pp) : (ut64) RD_LE32 (pp);
					ut64 abs = (val >= text_lo && val < text_hi)? val: (base_vaddr != UT64_MAX? val + base_vaddr: val);
					if (abs >= text_lo && abs < text_hi) { candidates[m] = abs; in_text++; }
				}
				if (in_text >= 8) { found = true; }
				break;
			}
		}
	}
	if (!found) {
		R_FREE (candidates); elf_free (&e); return false;
	}
	*out_ptrs = candidates; elf_free (&e); return true;
}


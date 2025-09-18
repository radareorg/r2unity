// Fast Mach-O parser for r2unity
#include "lib.h"
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static inline ut32 RD_LE32 (const ut8 *p) {
	return ((ut32)p[0]) | ((ut32)p[1] << 8) | ((ut32)p[2] << 16) | ((ut32)p[3] << 24);
}
static inline ut32 RD_BE32 (const ut8 *p) {
	return ((ut32)p[3]) | ((ut32)p[2] << 8) | ((ut32)p[1] << 16) | ((ut32)p[0] << 24);
}
static inline ut64 RD_LE64 (const ut8 *p) {
	return ((ut64)p[0]) | ((ut64)p[1] << 8) | ((ut64)p[2] << 16) | ((ut64)p[3] << 24) |
		((ut64)p[4] << 32) | ((ut64)p[5] << 40) | ((ut64)p[6] << 48) | ((ut64)p[7] << 56);
}

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
	ut64 filesize;
	ut64 base;
	ut32 ncmds;
	ut64 cmd_off;
	MachSeg segs[128];
	int nsegs;
} MachO;

static bool macho_load (const char *path, MachO *mo) {
	memset (mo, 0, sizeof (*mo));
	int fd = open (path, O_RDONLY);
	if (fd < 0) return false;
	struct stat st;
	if (fstat (fd, &st) != 0 || st.st_size <= 0) {
		close (fd);
		return false;
	}
	mo->filesize = (ut64) st.st_size;
	mo->file = R_NEWS (ut8, mo->filesize);
	if (!mo->file) {
		close (fd);
		return false;
	}
	ssize_t rd = read (fd, mo->file, (size_t) mo->filesize);
	close (fd);
	if (rd < 0 || (ut64) rd != mo->filesize) {
		R_FREE (mo->file);
		return false;
	}
	ut32 magic = RD_BE32 (mo->file);
	ut64 off = 0;
	if (magic == 0xcafebabe) {
		ut32 nfat = RD_BE32 (mo->file + 4);
		ut32 best = 0;
		for (ut32 i = 0; i < nfat; i++) {
			const ut8 *fa = mo->file + 8 + i * 20;
			ut32 cputype = RD_BE32 (fa + 0);
			if (cputype == 0x0100000c) { best = i; break; }
		}
		const ut8 *fa = mo->file + 8 + best * 20;
		off = RD_BE32 (fa + 8);
	}
	mo->base = off;
	const ut8 *p = mo->file + off;
	ut32 mh_magic = RD_LE32 (p);
	if (mh_magic != 0xFEEDFACF) {
		R_FREE (mo->file);
		return false;
	}
	ut32 ncmds = RD_LE32 (p + 0x10);
	mo->ncmds = ncmds;
	mo->cmd_off = off + 0x20;
	ut64 co = mo->cmd_off;
	for (ut32 i = 0; i < ncmds && (co + 8 <= mo->filesize); i++) {
		ut32 cmd = RD_LE32 (mo->file + co);
		ut32 cmdsize = RD_LE32 (mo->file + co + 4);
		if (cmd == 0x19 && cmdsize >= 72) {
			if (mo->nsegs < (int)(sizeof (mo->segs)/sizeof (mo->segs[0]))) {
				MachSeg *s = &mo->segs[mo->nsegs++];
				const ut8 *sp = mo->file + co + 8;
				memcpy (s->segname, sp, 16);
				s->vmaddr = RD_LE64 (sp + 16);
				s->vmsize = RD_LE64 (sp + 24);
				s->fileoff = RD_LE64 (sp + 32);
				s->filesize = RD_LE64 (sp + 40);
				s->maxprot = RD_LE32 (sp + 48);
			}
		}
		co += cmdsize ? cmdsize : 8;
	}
	return true;
}

static void macho_free (MachO *mo) {
	R_FREE (mo->file);
}

static inline bool macho_vm_in_text (MachO *mo, ut64 addr, ut64 *text_lo, ut64 *text_hi) {
	ut64 lo = UT64_MAX, hi = 0;
	for (int i = 0; i < mo->nsegs; i++) {
		MachSeg *s = &mo->segs[i];
		bool is_text = (s->maxprot & 0x4) || !strncmp (s->segname, "__TEXT", 6);
		if (is_text) {
			if (s->vmaddr < lo) lo = s->vmaddr;
			if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
		}
	}
	if (text_lo) *text_lo = lo;
	if (text_hi) *text_hi = hi;
	return (addr >= lo && addr < hi);
}

static inline const ut8 *macho_vm_to_ptr (MachO *mo, ut64 vmaddr) {
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

R_API bool r2unity_find_method_pointers_macho (R2UnityMetadata *meta, const char *macho_path, ut64 **out_ptrs) {
	if (!meta || !macho_path || !out_ptrs) return false;
	*out_ptrs = NULL;
	MachO mo;
	if (!macho_load (macho_path, &mo)) {
		return false;
	}
	size_t method_count = meta->header.v24.methodsSize / sizeof (Il2CppMethodDefinition);
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
    for (int i = 0; i < mo.nsegs && !found; i++) {
        MachSeg *s = &mo.segs[i];
		bool is_data = (s->maxprot & 0x1) && !(s->maxprot & 0x4);
		if (!is_data || s->filesize < (8 + 8)) continue;
		const ut8 *buf = mo.file + mo.base + s->fileoff;
		ut64 sz = s->filesize;
		for (ut64 off = 0; off + 16 <= sz; off += 4) {
			ut32 cnt32 = RD_LE32 (buf + off);
			if (cnt32 < 1024 || cnt32 > (ut32)(method_count * 2)) continue;
			ut64 arrptr = RD_LE64 (buf + off + 8);
			ut32 good = 0, seen = 0;
			ut32 sample = R_MIN ((ut32)128, cnt32);
			for (ut32 k = 0; k < sample; k++) {
				const ut8 *p = macho_vm_to_ptr (&mo, arrptr + (ut64)k * (ut64)ptrsz);
				if (!p) break;
				ut64 val = RD_LE64 (p);
				if (val) seen++;
				if (val >= text_lo && val < text_hi) good++;
			}
			if (seen >= 8 && good >= 8) {
				memset (candidates, 0, method_count * sizeof (ut64));
				for (size_t m = 0; m < method_count && m < cnt32; m++) {
					const ut8 *p = macho_vm_to_ptr (&mo, arrptr + (ut64)m * (ut64)ptrsz);
					if (!p) break;
					ut64 val = RD_LE64 (p);
					if (val >= text_lo && val < text_hi) candidates[m] = val;
				}
				found = true;
				break;
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

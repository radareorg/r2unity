#include "lib.h"
#include <r_util.h>
#include <r_util/r_buf.h>
#include <r_core.h>
#include <r_util/r_json.h>
#undef RD_LE32
#undef RD_LE64
static inline ut32 RD_LE32 (const ut8 *p) {
	return ((ut32)p[0]) | ((ut32)p[1] << 8) | ((ut32)p[2] << 16) | ((ut32)p[3] << 24);
}
static inline ut64 RD_LE64 (const ut8 *p) {
	return ((ut64)p[0]) | ((ut64)p[1] << 8) | ((ut64)p[2] << 16) | ((ut64)p[3] << 24) |
		((ut64)p[4] << 32) | ((ut64)p[5] << 40) | ((ut64)p[6] << 48) | ((ut64)p[7] << 56);
}
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

R_API R2UnityMetadata *r2unity_parse_metadata (RBuffer *buf) {
	if (!buf) {
		return NULL;
	}
	R2UnityMetadata *meta = R_NEW0 (R2UnityMetadata);
	if (!meta) {
		return NULL;
	}
	// read sanity and version first
	uint32_t sanity = 0;
	int32_t version = 0;
	if (r_buf_read_at (buf, 0, (ut8 *)&sanity, 4) != 4) {
		R_FREE (meta);
		return NULL;
	}
	if (sanity != IL2CPP_MAGIC) {
		R_FREE (meta);
		return NULL;
	}
	if (r_buf_read_at (buf, 4, (ut8 *)&version, 4) != 4) {
		R_FREE (meta);
		return NULL;
	}
	meta->version = version;
	size_t header_size = 0;
	if (version < 24 || version > 31) {
		R_FREE (meta);
		return NULL;
	}
	if (version < 27) {
		header_size = sizeof (Il2CppGlobalMetadataHeader_v24);
	} else if (version < 29) {
		header_size = sizeof (Il2CppGlobalMetadataHeader_v27);
	} else {
		// versions 29 and above (e.g. 30, 31) are compatible for the fields we use
		header_size = sizeof (Il2CppGlobalMetadataHeader_v29);
	}
	// read full header for selected version
	if (r_buf_read_at (buf, 0, (ut8 *)&meta->header.v24, header_size) != (st64)header_size) {
		R_FREE (meta);
		return NULL;
	}
	meta->buf = buf;
	meta->strings = r_buf_new_slice (buf, meta->header.v24.stringOffset, meta->header.v24.stringSize);
	meta->string_literals = r_buf_new_slice (buf, meta->header.v24.stringLiteralOffset, meta->header.v24.stringLiteralSize);
	return meta;
}

R_API void r2unity_free_metadata (R2UnityMetadata *meta) {
	if (!meta) {
		return;
	}
	r_buf_free (meta->strings);
	r_buf_free (meta->string_literals);
	R_FREE (meta);
}

R_API const char *r2unity_get_string (R2UnityMetadata *meta, uint32_t index) {
	if (!meta || !meta->strings || index >= r_buf_size (meta->strings)) {
		return NULL;
	}
	// Align index to the beginning of the string in case it points mid-string
	ut8 bprev = 0;
	ut64 idx = index;
	while (idx > 0) {
		if (r_buf_read_at (meta->strings, idx - 1, &bprev, 1) != 1) break;
		if (bprev == 0) break;
		idx--;
	}
	// Read until null terminator
	char *str = NULL;
	ut64 len = 0;
	ut8 byte;
	while (r_buf_read_at (meta->strings, idx + len, &byte, 1) == 1 && byte != 0) {
		len++;
	}
	if (len > 0) {
		str = R_NEWS (char, len + 1);
		if (str) {
			r_buf_read_at (meta->strings, idx, (ut8 *)str, len);
			str[len] = 0;
		}
	}
	return str;
}

R_API Il2CppTypeDefinition *r2unity_get_type_definitions (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	*count = meta->header.v24.typeDefinitionsSize / sizeof (Il2CppTypeDefinition);
	if (*count == 0) {
		return NULL;
	}
	Il2CppTypeDefinition *types = R_NEWS (Il2CppTypeDefinition, *count);
	if (!types) {
		return NULL;
	}
	if (r_buf_read_at (meta->buf, meta->header.v24.typeDefinitionsOffset, (ut8 *)types, meta->header.v24.typeDefinitionsSize) != meta->header.v24.typeDefinitionsSize) {
		R_FREE (types);
		return NULL;
	}
	return types;
}

static inline ut16 RD_LE16 (const ut8 *p) {
	return (ut16)((ut16)p[0] | ((ut16)p[1] << 8));
}

R_API Il2CppMethodDefinition *r2unity_get_method_definitions (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	// On-disk Il2CppMethodDefinition is 32 bytes (little-endian)
	const ut64 entry = 32;
	if (meta->header.v24.methodsSize < entry) {
		*count = 0;
		return NULL;
	}
	*count = meta->header.v24.methodsSize / entry;
	Il2CppMethodDefinition *methods = R_NEWS (Il2CppMethodDefinition, *count);
	if (!methods) {
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, meta->header.v24.methodsSize);
	if (!buf) {
		R_FREE (methods);
		return NULL;
	}
	if (r_buf_read_at (meta->buf, meta->header.v24.methodsOffset, buf, meta->header.v24.methodsSize) != meta->header.v24.methodsSize) {
		R_FREE (buf);
		R_FREE (methods);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		methods[i].nameIndex = RD_LE32 (p + 0);
		methods[i].declaringType = (int32_t) RD_LE32 (p + 4);
		methods[i].returnType = (int32_t) RD_LE32 (p + 8);
		methods[i].parameterStart = (int32_t) RD_LE32 (p + 12);
		methods[i].genericContainerIndex = (int32_t) RD_LE32 (p + 16);
		methods[i].token = RD_LE32 (p + 20);
		methods[i].flags = RD_LE16 (p + 24);
		methods[i].iflags = RD_LE16 (p + 26);
		methods[i].slot = RD_LE16 (p + 28);
		methods[i].parameterCount = RD_LE16 (p + 30);
	}
	R_FREE (buf);
	return methods;
}

R_API bool r2unity_find_method_pointers (R2UnityMetadata *meta, const char *exe_path, ut64 **out_ptrs) {
	if (!meta || !exe_path || !out_ptrs) {
		return false;
	}
	*out_ptrs = NULL;
	RCore *core = r_core_new ();
	if (!core) {
		return false;
	}
	// apply relocs/binds/fixups
	r_core_cmd0 (core, "e bin.cache=true");
	r_core_cmd0 (core, "e bin.relocs.apply=true");
	if (!r_core_file_open (core, exe_path, 0, 0)) {
		r_core_free (core);
		return false;
	}
	if (!r_core_bin_load (core, NULL, 0)) {
		r_core_free (core);
		return false;
	}
	// detect pointer size
	int ptrsz = 8;
	char *ij = r_core_cmd_str (core, "ij");
	if (ij) {
		RJson *pj = r_json_parse (ij);
		if (pj) {
			const RJson *pbin = r_json_get (pj, "bin");
			if (pbin) {
				st64 bits = r_json_get_num (pbin, "bits");
				ptrsz = (bits == 32)? 4: 8;
			}
			r_json_free (pj);
		}
		free (ij);
	}
	// compute text range and data ranges from io maps json
	ut64 text_lo = UT64_MAX;
	ut64 text_hi = 0;
	char *omj = r_core_cmd_str (core, "omj");
	RJson *jom = omj? r_json_parse (omj): NULL;
	#define MAX_RANGES 128
	typedef struct { ut64 from, to; } Range;
	Range data_ranges[MAX_RANGES];
	int data_n = 0;
	if (jom && jom->type == R_JSON_ARRAY) {
		for (size_t i = 0; i < jom->children.count; i++) {
			const RJson *item = r_json_item (jom, i);
			if (!item) continue;
			const char *perm = r_json_get_str (item, "perm");
			ut64 from = (ut64) r_json_get_num (item, "from");
			ut64 to = (ut64) r_json_get_num (item, "to");
			if (!perm) continue;
			if (strchr (perm, 'x')) {
				if (from < text_lo) text_lo = from;
				if (to > text_hi) text_hi = to;
			} else if (strchr (perm, 'r')) {
				if (data_n < MAX_RANGES) {
					data_ranges[data_n].from = from;
					data_ranges[data_n].to = to;
					data_n++;
				}
			}
		}
	}
	if (jom) r_json_free (jom);
	free (omj);
	if (text_lo == UT64_MAX || text_hi <= text_lo) {
		r_core_free (core);
		return false;
	}
	size_t method_count = meta->header.v24.methodsSize / sizeof (Il2CppMethodDefinition);
	if (!method_count) {
		r_core_free (core);
		return false;
	}
    // Try strict match first: full method_count array
    ut64 *candidates = R_NEWS (ut64, method_count);
    if (!candidates) {
        r_core_free (core);
        return false;
    }
    bool found = false;
    for (int r = 0; r < data_n && !found; r++) {
        ut64 start = data_ranges[r].from;
        ut64 end = data_ranges[r].to;
        ut64 rsize = end - start;
        if (rsize < (ut64)method_count * (ut64)ptrsz) {
            continue;
        }
        ut8 *rbuf = R_NEWS (ut8, rsize);
        if (!rbuf) {
            break;
        }
        r_io_read_at (core->io, start, rbuf, rsize);
        for (ut64 off = 0; off + ((ut64)method_count * (ut64)ptrsz) <= rsize; off += ptrsz) {
            ut32 good = 0, zero = 0;
            for (size_t i = 0; i < method_count; i++) {
                ut64 val = (ptrsz == 8)
                    ? RD_LE64 (rbuf + off + (ut64)i * 8)
                    : (ut64) RD_LE32 (rbuf + off + (ut64)i * 4);
                candidates[i] = val;
                if (!val) {
                    zero++;
                    continue;
                }
                if (val >= text_lo && val < text_hi) {
                    good++;
                }
            }
            if (good > 0 && good + zero >= (ut32)(method_count * 9 / 10)) {
                found = true;
                break;
            }
        }
        R_FREE (rbuf);
    }
    if (!found) {
        // Try to identify CodeRegistration-like pattern: [count (u32), padding (u32), pointer (ptrsz)]
        ut64 gmp_ptr = 0;
        size_t gmp_cnt = 0;
        for (int r = 0; r < data_n && !found; r++) {
            ut64 start = data_ranges[r].from;
            ut64 end = data_ranges[r].to;
            ut64 rsize = end - start;
            ut8 *rbuf = R_NEWS (ut8, rsize);
            if (!rbuf) break;
            r_io_read_at (core->io, start, rbuf, rsize);
            for (ut64 off = 0; off + (ut64)(ptrsz + 8) <= rsize; off += 4) {
                ut32 cnt32 = RD_LE32 (rbuf + off);
                if (cnt32 < 1024 || cnt32 > (ut32)(method_count * 2)) {
                    continue;
                }
                ut64 arrptr = (ptrsz == 8)
                    ? RD_LE64 (rbuf + off + 8)
                    : (ut64) RD_LE32 (rbuf + off + 4);
                // Check arrptr lies in a readable range
                bool in_r = false;
                for (int rr = 0; rr < data_n; rr++) {
                    if (arrptr >= data_ranges[rr].from && arrptr < data_ranges[rr].to) {
                        in_r = true;
                        break;
                    }
                }
                if (!in_r) continue;
                // Sample first up to 256 entries
                ut32 good = 0, seen = 0;
                ut32 sample = R_MIN ((ut32)256, cnt32);
                for (ut32 i = 0; i < sample; i++) {
                    ut64 val = 0;
                    if (ptrsz == 8) {
                        r_io_read_at (core->io, arrptr + (ut64)i * 8, (ut8 *)&val, 8);
                    } else {
                        ut32 v32 = 0;
                        r_io_read_at (core->io, arrptr + (ut64)i * 4, (ut8 *)&v32, 4);
                        val = v32;
                    }
                    if (val) seen++;
                    if (val >= text_lo && val < text_hi) good++;
                }
                if (good >= 32 && seen >= 32) {
                    gmp_ptr = arrptr;
                    gmp_cnt = cnt32;
                    found = true;
                    break;
                }
            }
            R_FREE (rbuf);
        }
        if (found && gmp_ptr) {
            // Read array at gmp_ptr
            size_t maxc = method_count;
            memset (candidates, 0, method_count * sizeof (ut64));
            ut64 bytes = (ut64) R_MIN (maxc, gmp_cnt) * (ut64) ptrsz;
            ut8 *tmp = R_NEWS (ut8, bytes);
            if (tmp) {
                r_io_read_at (core->io, gmp_ptr, tmp, bytes);
                for (size_t i = 0; i < R_MIN (maxc, gmp_cnt); i++) {
                    candidates[i] = (ptrsz == 8)
                        ? RD_LE64 (tmp + (ut64)i * 8)
                        : (ut64) RD_LE32 (tmp + (ut64)i * 4);
                }
                R_FREE (tmp);
            }
        }
    }
    if (!found) {
        // Fallback: find the longest run of pointers into text (or zero) – use as prefix
        ut64 best_start = 0;
        size_t best_len = 0;
        size_t max_probe = R_MIN ((size_t)65536, (size_t)method_count);
        for (int r = 0; r < data_n; r++) {
            ut64 start = data_ranges[r].from;
            ut64 end = data_ranges[r].to;
            ut64 rsize = end - start;
            ut8 *rbuf = R_NEWS (ut8, rsize);
            if (!rbuf) break;
            r_io_read_at (core->io, start, rbuf, rsize);
            for (ut64 off = 0; off + (ut64)(ptrsz * 16) <= rsize; off += ptrsz) {
                size_t run = 0;
                for (; run < max_probe; run++) {
                    ut64 val = (ptrsz == 8)
                        ? RD_LE64 (rbuf + off + (ut64)run * 8)
                        : (ut64) RD_LE32 (rbuf + off + (ut64)run * 4);
                    if (!val) {
                        // allow zeros in the run
                        continue;
                    }
                    if (!(val >= text_lo && val < text_hi)) {
                        break;
                    }
                }
                if (run > best_len) {
                    best_len = run;
                    best_start = start + off;
                }
            }
            R_FREE (rbuf);
        }
        if (best_len > 0) {
            // Read best_len pointers into prefix of candidates, rest zeros
            memset (candidates, 0, method_count * sizeof (ut64));
            for (size_t i = 0; i < best_len; i++) {
                ut64 val = 0;
                if (ptrsz == 8) {
                    r_io_read_at (core->io, best_start + i * 8, (ut8 *)&val, 8);
                } else {
                    ut32 v32 = 0;
                    r_io_read_at (core->io, best_start + i * 4, (ut8 *)&v32, 4);
                    val = v32;
                }
                candidates[i] = val;
            }
            found = true;
        }
    }
    if (!found) {
        R_FREE (candidates);
        r_core_free (core);
        return false;
    }
    // Sanitize: zero out any pointer not pointing into text
    for (size_t i = 0; i < method_count; i++) {
        ut64 v = candidates[i];
        if (!v) continue;
        if (!(v >= text_lo && v < text_hi)) {
            candidates[i] = 0;
        }
    }
    *out_ptrs = candidates;
    r_core_free (core);
    return true;
}

R_API bool r2unity_read_method_pointers_at (R2UnityMetadata *meta, const char *exe_path, ut64 addr, size_t count, ut64 **out_ptrs) {
	if (!meta || !exe_path || !out_ptrs || !addr) {
		return false;
	}
	*out_ptrs = NULL;
	RCore *core = r_core_new ();
	if (!core) {
		return false;
	}
	r_core_cmd0 (core, "e bin.cache=true");
	r_core_cmd0 (core, "e bin.relocs.apply=true");
	if (!r_core_file_open (core, exe_path, 0, 0)) {
		r_core_free (core);
		return false;
	}
	if (!r_core_bin_load (core, NULL, 0)) {
		r_core_free (core);
		return false;
	}
	int ptrsz = 8;
	char *ij = r_core_cmd_str (core, "ij");
	if (ij) {
		RJson *pj = r_json_parse (ij);
		if (pj) {
			const RJson *pbin = r_json_get (pj, "bin");
			if (pbin) {
				st64 bits = r_json_get_num (pbin, "bits");
				ptrsz = (bits == 32)? 4: 8;
			}
			r_json_free (pj);
		}
		free (ij);
	}
	size_t method_count = meta->header.v24.methodsSize / sizeof (Il2CppMethodDefinition);
	if (!method_count) {
		r_core_free (core);
		return false;
	}
	if (!count || count > method_count) {
		count = method_count;
	}
    ut64 *ptrs = R_NEWS (ut64, method_count);
	if (!ptrs) {
		r_core_free (core);
		return false;
	}
	memset (ptrs, 0, method_count * sizeof (ut64));
	ut64 bytes = (ut64) count * (ut64) ptrsz;
	ut8 *tmp = R_NEWS (ut8, bytes);
    if (tmp) {
        r_io_read_at (core->io, addr, tmp, bytes);
        for (size_t i = 0; i < count; i++) {
            ptrs[i] = (ptrsz == 8)
                ? RD_LE64 (tmp + (ut64)i * 8)
                : (ut64) RD_LE32 (tmp + (ut64)i * 4);
        }
        R_FREE (tmp);
    }
    // Sanitize out-of-text pointers
    ut64 text_lo = UT64_MAX, text_hi = 0;
    char *omj = r_core_cmd_str (core, "omj");
    RJson *jom = omj? r_json_parse (omj): NULL;
    if (jom && jom->type == R_JSON_ARRAY) {
        for (size_t i = 0; i < jom->children.count; i++) {
            const RJson *item = r_json_item (jom, i);
            if (!item) continue;
            const char *perm = r_json_get_str (item, "perm");
            ut64 from = (ut64) r_json_get_num (item, "from");
            ut64 to = (ut64) r_json_get_num (item, "to");
            if (!perm) continue;
            if (strchr (perm, 'x')) {
                if (from < text_lo) text_lo = from;
                if (to > text_hi) text_hi = to;
            }
        }
    }
    if (jom) r_json_free (jom);
    free (omj);
    if (text_lo != UT64_MAX && text_hi > text_lo) {
        for (size_t i = 0; i < count; i++) {
            ut64 v = ptrs[i];
            if (!v) continue;
            if (!(v >= text_lo && v < text_hi)) ptrs[i] = 0;
        }
    }
	*out_ptrs = ptrs;
	r_core_free (core);
	return true;
}

// Minimal Mach-O 64-bit + FAT parser to locate g_MethodPointers quickly
typedef struct {
	ut64 vmaddr;
	ut64 vmsize;
	ut64 fileoff;
	ut64 filesize;
	ut32 maxprot;
	char segname[16];
} MachSeg;

typedef struct {
	ut8 *file;
	ut64 filesize;
	ut64 base; // slice base offset
	ut32 ncmds;
	ut64 cmd_off;
	MachSeg segs[128];
	int nsegs;
} MachO;

static inline ut32 RD_BE32 (const ut8 *p) {
	return ((ut32)p[3]) | ((ut32)p[2] << 8) | ((ut32)p[1] << 16) | ((ut32)p[0] << 24);
}

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
	// Handle FAT
	ut32 magic = RD_BE32 (mo->file);
	ut64 off = 0;
	if (magic == 0xcafebabe) {
		ut32 nfat = RD_BE32 (mo->file + 4);
		// Choose first arm64 (cputype 0x100000c) or first slice
		ut32 best = 0;
		for (ut32 i = 0; i < nfat; i++) {
			const ut8 *fa = mo->file + 8 + i * 20; // fat_arch (not _64) is 20 bytes, but common on iOS
			ut32 cputype = RD_BE32 (fa + 0);
			if (cputype == 0x0100000c /* ARM64 */) { best = i; break; }
		}
		const ut8 *fa = mo->file + 8 + best * 20;
		off = RD_BE32 (fa + 8);
		// size = RD_BE32 (fa + 12);
	}
	mo->base = off;
	const ut8 *p = mo->file + off;
	ut32 mh_magic = *(const ut32 *) p;
	if (mh_magic != 0xFEEDFACF) {
		// Not 64-bit LE Mach-O
		R_FREE (mo->file);
		return false;
	}
	// mach_header_64
	ut32 ncmds = *(const ut32 *)(p + 0x10);
	ut32 sizeofcmds = *(const ut32 *)(p + 0x14);
	mo->ncmds = ncmds;
	mo->cmd_off = off + 0x20;
	ut64 co = mo->cmd_off;
	for (ut32 i = 0; i < ncmds && (co + 8 <= mo->filesize); i++) {
		ut32 cmd = *(const ut32 *)(mo->file + co);
		ut32 cmdsize = *(const ut32 *)(mo->file + co + 4);
		if (cmd == 0x19 /* LC_SEGMENT_64 */ && cmdsize >= 72) {
			if (mo->nsegs < (int)(sizeof (mo->segs)/sizeof (mo->segs[0]))) {
				MachSeg *s = &mo->segs[mo->nsegs++];
				const ut8 *sp = mo->file + co + 8;
				memcpy (s->segname, sp, 16);
				s->vmaddr = *(const ut64 *)(sp + 16);
				s->vmsize = *(const ut64 *)(sp + 24);
				s->fileoff = *(const ut64 *)(sp + 32);
				s->filesize = *(const ut64 *)(sp + 40);
				s->maxprot = *(const ut32 *)(sp + 48);
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
	int ptrsz = 8; // 64-bit
	// Fast CodeRegistration-like scan across readable non-exec segments
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
			// Sample within arrptr
			ut32 good = 0, seen = 0;
			ut32 sample = R_MIN ((ut32)256, cnt32);
			for (ut32 k = 0; k < sample; k++) {
				const ut8 *p = macho_vm_to_ptr (&mo, arrptr + (ut64)k * (ut64)ptrsz);
				if (!p) break;
				ut64 val = RD_LE64 (p);
				if (val) seen++;
				if (val >= text_lo && val < text_hi) good++;
			}
			if (seen >= 32 && good >= 32) {
				// Read full array
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

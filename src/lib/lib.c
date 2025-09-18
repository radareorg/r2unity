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
static inline ut16 RD_LE16 (const ut8 *p) {
	return (ut16)((ut16)p[0] | ((ut16)p[1] << 8));
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
	// On-disk Il2CppTypeDefinition is 88 bytes for v24+ (little-endian)
	const ut64 entry = 88;
	ut64 tsize = (ut64) meta->header.v24.typeDefinitionsSize;
	if (tsize < entry) {
		*count = 0;
		return NULL;
	}
	*count = (size_t) (tsize / entry);
	Il2CppTypeDefinition *types = R_NEWS (Il2CppTypeDefinition, *count);
	if (!types) {
		return NULL;
	}
	ut8 *buf = R_NEWS (ut8, tsize);
	if (!buf) {
		R_FREE (types);
		return NULL;
	}
	if (r_buf_read_at (meta->buf, meta->header.v24.typeDefinitionsOffset, buf, tsize) != (st64) tsize) {
		R_FREE (buf);
		R_FREE (types);
		return NULL;
	}
	for (size_t i = 0; i < *count; i++) {
		const ut8 *p = buf + i * entry;
		types[i].nameIndex = RD_LE32 (p + 0);
		types[i].namespaceIndex = RD_LE32 (p + 4);
		types[i].byvalTypeIndex = (int32_t) RD_LE32 (p + 8);
		types[i].declaringTypeIndex = (int32_t) RD_LE32 (p + 12);
		types[i].parentIndex = (int32_t) RD_LE32 (p + 16);
		types[i].elementTypeIndex = (int32_t) RD_LE32 (p + 20);
		types[i].genericContainerIndex = (int32_t) RD_LE32 (p + 24);
		types[i].flags = RD_LE32 (p + 28);
		types[i].fieldStart = (int32_t) RD_LE32 (p + 32);
		types[i].methodStart = (int32_t) RD_LE32 (p + 36);
		types[i].eventStart = (int32_t) RD_LE32 (p + 40);
		types[i].propertyStart = (int32_t) RD_LE32 (p + 44);
		types[i].nestedTypesStart = (int32_t) RD_LE32 (p + 48);
		types[i].interfacesStart = (int32_t) RD_LE32 (p + 52);
		types[i].vtableStart = (int32_t) RD_LE32 (p + 56);
		types[i].interfaceOffsetsStart = (int32_t) RD_LE32 (p + 60);
		types[i].method_count = RD_LE16 (p + 64);
		types[i].property_count = RD_LE16 (p + 66);
		types[i].field_count = RD_LE16 (p + 68);
		types[i].event_count = RD_LE16 (p + 70);
		types[i].nested_type_count = RD_LE16 (p + 72);
		types[i].vtable_count = RD_LE16 (p + 74);
		types[i].interfaces_count = RD_LE16 (p + 76);
		types[i].interface_offsets_count = RD_LE16 (p + 78);
		types[i].bitfield = RD_LE32 (p + 80);
		types[i].token = RD_LE32 (p + 84);
	}
	R_FREE (buf);
	return types;
}

R_API Il2CppMethodDefinition *r2unity_get_method_definitions (R2UnityMetadata *meta, size_t *count) {
	if (!meta || !count) {
		return NULL;
	}
	// On-disk Il2CppMethodDefinition is 32 bytes (little-endian)
	const ut64 entry = 32;
	if ((ut64) meta->header.v24.methodsSize < entry) {
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
    // Try fast sampled window scan first instead of full sliding O(N^2)
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
        if (rsize < (ut64)ptrsz * 256) {
            continue;
        }
        ut8 *rbuf = R_NEWS (ut8, rsize);
        if (!rbuf) break;
        r_io_read_at (core->io, start, rbuf, rsize);
        const ut64 sampleN = R_MIN ((ut64)512, (ut64)method_count);
        const ut64 step = (ut64)ptrsz * 4; // stride to reduce checks
        for (ut64 off = 0; off + sampleN * (ut64)ptrsz <= rsize; off += step) {
            ut32 good = 0, zero = 0;
            for (ut64 i = 0; i < sampleN; i++) {
                ut64 val = (ptrsz == 8)
                    ? RD_LE64 (rbuf + off + i * 8)
                    : (ut64) RD_LE32 (rbuf + off + i * 4);
                if (!val) { zero++; continue; }
                if (val >= text_lo && val < text_hi) { good++; }
            }
            if (good > 0 && good + zero >= (ut32)(sampleN * 9 / 10)) {
                // Likely candidate; fetch full array from IO once
                ut64 addr = start + off;
                ut64 bytes = (ut64)method_count * (ut64)ptrsz;
                ut8 *tmp = R_NEWS (ut8, bytes);
                if (!tmp) { continue; }
                st64 nr = r_io_read_at (core->io, addr, tmp, bytes);
                if (nr > 0) {
                    ut32 g2 = 0, z2 = 0;
                    for (size_t i = 0; i < method_count; i++) {
                        ut64 val = (ptrsz == 8)
                            ? RD_LE64 (tmp + (ut64)i * 8)
                            : (ut64) RD_LE32 (tmp + (ut64)i * 4);
                        candidates[i] = val;
                        if (!val) { z2++; continue; }
                        if (val >= text_lo && val < text_hi) { g2++; }
                    }
                    if (g2 > 0 && g2 + z2 >= (ut32)(method_count * 9 / 10)) {
                        found = true;
                        R_FREE (tmp);
                        break;
                    }
                }
                R_FREE (tmp);
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
                // Sample first up to 256 entries with one bulk read
                ut32 good = 0, seen = 0;
                ut32 sample = R_MIN ((ut32)256, cnt32);
                ut64 sbytes = (ut64)sample * (ut64)ptrsz;
                ut8 *sbuf = R_NEWS (ut8, sbytes);
                if (!sbuf) continue;
                st64 sread = r_io_read_at (core->io, arrptr, sbuf, sbytes);
                if (sread <= 0) { R_FREE (sbuf); continue; }
                for (ut32 i = 0; i < sample; i++) {
                    ut64 val = (ptrsz == 8)
                        ? RD_LE64 (sbuf + (ut64)i * 8)
                        : (ut64) RD_LE32 (sbuf + (ut64)i * 4);
                    if (val) seen++;
                    if (val >= text_lo && val < text_hi) good++;
                }
                R_FREE (sbuf);
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
            // Read best_len pointers in one shot into prefix of candidates, rest zeros
            memset (candidates, 0, method_count * sizeof (ut64));
            ut64 bytes = (ut64)best_len * (ut64)ptrsz;
            ut8 *tmp = R_NEWS (ut8, bytes);
            if (tmp) {
                st64 nr = r_io_read_at (core->io, best_start, tmp, bytes);
                if (nr > 0) {
                    for (size_t i = 0; i < best_len; i++) {
                        candidates[i] = (ptrsz == 8)
                            ? RD_LE64 (tmp + (ut64)i * 8)
                            : (ut64) RD_LE32 (tmp + (ut64)i * 4);
                    }
                }
                R_FREE (tmp);
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

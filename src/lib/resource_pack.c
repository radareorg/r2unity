/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.resource_pack"

#include "resource_pack.h"

#define R2U_RESOURCE_PACK_MAX_ENTRIES 65536
#define R2U_RESOURCE_PACK_MAX_NAME 65536

typedef struct {
	RBuffer *buf;
	ut64 offset;
	ut64 limit;
} R2UnityResourcePackReader;

static bool reader_read(R2UnityResourcePackReader *reader, void *out, ut64 size) {
	if (reader->offset > reader->limit || size > reader->limit - reader->offset
		|| r_buf_read_at (reader->buf, reader->offset, out, size) != (st64)size) {
		return false;
	}
	reader->offset += size;
	return true;
}

static bool reader_u32(R2UnityResourcePackReader *reader, ut32 *value) {
	ut8 bytes[4];
	if (!reader_read (reader, bytes, sizeof (bytes))) {
		return false;
	}
	*value = r_read_le32 (bytes);
	return true;
}

static bool valid_name(const ut8 *bytes, ut32 size) {
	ut32 offset = 0;
	bool visible = false;
	while (offset < size) {
		RRune rune;
		int length = r_utf8_decode (bytes + offset, size - offset, &rune);
		if (length < 1 || (ut32)length > size - offset
			|| rune == 0 || (rune < 0x20 && rune != '\t')
			|| rune == 0x7f) {
			return false;
		}
		visible |= rune != ' ' && rune != '\t';
		offset += length;
	}
	return visible;
}

static R2UnityResourcePack *parse_internal(RBuffer *buf) {
	ut64 size = r_buf_size (buf);
	if (size < 8) {
		return NULL;
	}
	R2UnityResourcePack *pack = R_NEW0 (R2UnityResourcePack);
	pack->buf = r_buf_new_slice (buf, 0, size);
	if (!pack->buf) {
		free (pack);
		return NULL;
	}
	pack->size = size;
	R2UnityResourcePackReader reader = {
		.buf = pack->buf,
		.offset = 0,
		.limit = size,
	};
	ut32 count;
	if (!reader_u32 (&reader, &pack->records_size)
		|| pack->records_size < 4
		|| pack->records_size > size - 4
		|| !reader_u32 (&reader, &count)
		|| !count || count > R2U_RESOURCE_PACK_MAX_ENTRIES) {
		goto fail;
	}
	pack->payload_offset = 4 + pack->records_size;
	reader.limit = pack->payload_offset;
	pack->entry_count = count;
	pack->entries = R_NEWS0 (R2UnityResourcePackEntry, count);
	if (!pack->entries) {
		goto fail;
	}
	for (size_t i = 0; i < pack->entry_count; i++) {
		R2UnityResourcePackEntry *entry = &pack->entries[i];
		ut32 payload_size;
		entry->descriptor_offset = reader.offset;
		if (!reader_u32 (&reader, &payload_size)) {
			goto fail;
		}
		entry->payload_size = payload_size;
		entry->name_size_offset = reader.offset;
		if (!reader_u32 (&reader, &entry->name_size)
			|| !entry->name_size
			|| entry->name_size > R2U_RESOURCE_PACK_MAX_NAME
			|| entry->name_size > reader.limit - reader.offset) {
			goto fail;
		}
		entry->name_offset = reader.offset;
		entry->name = R_NEWS (char, (ut64)entry->name_size + 1);
		if (!entry->name
			|| !reader_read (&reader, entry->name, entry->name_size)
			|| !valid_name ((const ut8 *)entry->name, entry->name_size)) {
			goto fail;
		}
		entry->name[entry->name_size] = 0;
	}
	if (reader.offset != pack->payload_offset) {
		goto fail;
	}
	ut64 payload_offset = pack->payload_offset;
	for (size_t i = 0; i < pack->entry_count; i++) {
		R2UnityResourcePackEntry *entry = &pack->entries[i];
		if (payload_offset > size || entry->payload_size > size - payload_offset) {
			goto fail;
		}
		entry->payload_offset = payload_offset;
		payload_offset += entry->payload_size;
	}
	if (payload_offset != size) {
		goto fail;
	}
	return pack;

fail:
	r2unity_resource_pack_free (pack);
	return NULL;
}

bool r2unity_resource_pack_check(RBuffer *buf) {
	R_RETURN_VAL_IF_FAIL (buf, false);
	R2UnityResourcePack *pack = parse_internal (buf);
	if (!pack) {
		return false;
	}
	r2unity_resource_pack_free (pack);
	return true;
}

R2UnityResourcePack *r2unity_resource_pack_parse(RBuffer *buf) {
	R_RETURN_VAL_IF_FAIL (buf, NULL);
	return parse_internal (buf);
}

void r2unity_resource_pack_free(R2UnityResourcePack *pack) {
	if (!pack) {
		return;
	}
	for (size_t i = 0; i < pack->entry_count; i++) {
		free (pack->entries[i].name);
	}
	free (pack->entries);
	r_unref (pack->buf);
	free (pack);
}

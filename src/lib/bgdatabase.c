/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.bgdatabase"

#include "bgdatabase.h"
#include <ctype.h>

#define R2U_BGDB_VERSION 6
#define R2U_BGDB_MAX_ADDONS 1024
#define R2U_BGDB_MAX_TABLES 65536
#define R2U_BGDB_MAX_FIELDS 65536
#define R2U_BGDB_MAX_STRING (1024 * 1024)
#define R2U_BGDB_MAX_STRINGS 1048576

typedef struct {
	RBuffer *buf;
	ut64 offset;
	ut64 limit;
} R2UnityBGDatabaseReader;

static bool reader_has(const R2UnityBGDatabaseReader *r, ut64 size) {
	return r && r->offset <= r->limit && size <= r->limit - r->offset;
}

static bool reader_read(R2UnityBGDatabaseReader *r, void *out, ut64 size) {
	if (!reader_has (r, size)
		|| r_buf_read_at (r->buf, r->offset, out, size) != (st64)size) {
		return false;
	}
	r->offset += size;
	return true;
}

static bool reader_skip(R2UnityBGDatabaseReader *r, ut64 size) {
	if (!reader_has (r, size)) {
		return false;
	}
	r->offset += size;
	return true;
}

static bool reader_u32(R2UnityBGDatabaseReader *r, ut32 *out) {
	ut8 raw[4];
	if (!reader_read (r, raw, sizeof (raw))) {
		return false;
	}
	*out = r_read_le32 (raw);
	return true;
}

static bool reader_string(R2UnityBGDatabaseReader *r, char **out,
		ut64 *data_offset, ut32 *string_size) {
	ut32 length;
	if (!reader_u32 (r, &length) || length > R2U_BGDB_MAX_STRING
		|| !reader_has (r, length)) {
		return false;
	}
	char *value = R_NEWS (char, (ut64)length + 1);
	if (!value) {
		return false;
	}
	if (length && r_buf_read_at (r->buf, r->offset,
			(ut8 *)value, length) != (st64)length) {
		free (value);
		return false;
	}
	value[length] = 0;
	if (data_offset) {
		*data_offset = r->offset;
	}
	if (string_size) {
		*string_size = length;
	}
	*out = value;
	return reader_skip (r, length);
}

static bool addon_type_valid(const char *type) {
	if (R_STR_ISEMPTY (type) || !r_str_startswith (type, "BansheeGz.BGDatabase.")) {
		return false;
	}
	for (const ut8 *p = (const ut8 *)type; *p; p++) {
		if (*p < 0x20 || *p > 0x7e) {
			return false;
		}
	}
	return strstr (type, ", BGDatabase") != NULL;
}

static bool utf8_printable(const ut8 *bytes, ut32 size) {
	ut32 i = 0;
	bool visible = false;
	while (i < size) {
		ut32 cp;
		ut8 ch = bytes[i++];
		if (ch < 0x80) {
			if (!ch || (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t')
				|| ch == 0x7f) {
				return false;
			}
			visible |= !isspace (ch);
			continue;
		}
		ut32 need;
		if (ch >= 0xc2 && ch <= 0xdf) {
			cp = ch & 0x1f;
			need = 1;
		} else if (ch >= 0xe0 && ch <= 0xef) {
			cp = ch & 0x0f;
			need = 2;
		} else if (ch >= 0xf0 && ch <= 0xf4) {
			cp = ch & 0x07;
			need = 3;
		} else {
			return false;
		}
		if (need > size - i) {
			return false;
		}
		for (ut32 j = 0; j < need; j++) {
			ut8 continuation = bytes[i++];
			if ((continuation & 0xc0) != 0x80) {
				return false;
			}
			cp = (cp << 6) | (continuation & 0x3f);
		}
		if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000)
			|| (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
			return false;
		}
		visible = true;
	}
	return visible;
}

static bool append_table(R2UnityBGDatabase *db, ut64 offset,
		ut64 name_field_offset, ut32 field_count, ut16 name_field_type) {
	if (db->table_count >= R2U_BGDB_MAX_TABLES) {
		return false;
	}
	size_t count = db->table_count + 1;
	R2UnityBGDatabaseTable *tables = realloc (db->tables,
		count * sizeof (*tables));
	if (!tables) {
		return false;
	}
	db->tables = tables;
	R2UnityBGDatabaseTable *table = &db->tables[db->table_count++];
	memset (table, 0, sizeof (*table));
	table->offset = offset;
	table->name_field_offset = name_field_offset;
	table->field_count = field_count;
	table->name_field_type = name_field_type;
	return true;
}

static bool collect_tables(R2UnityBGDatabase *db, const ut8 *data,
		ut32 declared_count) {
	ut64 start = db->body_offset + 4;
	for (ut64 at = start; at <= db->size && db->size - at >= 8; at++) {
		if (r_read_le32 (data + at) != 4) {
			continue;
		}
		if (memcmp (data + at + 4, "name", 4)) {
			continue;
		}
		/* A v6 table starts with its field count, followed by the canonical
		 * name-field descriptor: u32 kind, u16 type, 16-byte id, string. */
		if (at < start + 26) {
			continue;
		}
		ut64 name_field = at - 22;
		ut64 table_offset = name_field - 4;
		ut32 field_count = r_read_le32 (data + table_offset);
		ut32 field_kind = r_read_le32 (data + name_field);
		ut16 field_type = r_read_le16 (data + name_field + 4);
		const ut8 *id = data + name_field + 6;
		if (!field_count || field_count > R2U_BGDB_MAX_FIELDS
			|| field_kind > 0x1000 || !field_type) {
			continue;
		}
		bool nonzero = false;
		for (size_t i = 0; i < sizeof (id); i++) {
			nonzero |= id[i] != 0;
		}
		if (!nonzero || !append_table (db, table_offset, name_field,
				field_count, field_type)) {
			if (!nonzero) {
				continue;
			}
			return false;
		}
		at += 7;
	}
	if (db->table_count != declared_count) {
		return false;
	}
	for (size_t i = 0; i < db->table_count; i++) {
		ut64 end = i + 1 < db->table_count
			? db->tables[i + 1].offset: db->size;
		if (end <= db->tables[i].offset) {
			return false;
		}
		db->tables[i].size = end - db->tables[i].offset;
	}
	return true;
}

static bool append_string(R2UnityBGDatabase *db, ut64 prefix_offset,
		ut32 size, const ut8 *bytes) {
	if (db->string_count >= R2U_BGDB_MAX_STRINGS) {
		return false;
	}
	size_t count = db->string_count + 1;
	R2UnityBGDatabaseString *strings = realloc (db->strings,
		count * sizeof (*strings));
	if (!strings) {
		return false;
	}
	db->strings = strings;
	R2UnityBGDatabaseString *string = &db->strings[db->string_count++];
	memset (string, 0, sizeof (*string));
	string->value = R_NEWS (char, (ut64)size + 1);
	if (!string->value) {
		db->string_count--;
		return false;
	}
	memcpy (string->value, bytes, size);
	string->value[size] = 0;
	string->prefix_offset = prefix_offset;
	string->offset = prefix_offset + 4;
	string->size = size;
	return true;
}

static bool collect_strings(R2UnityBGDatabase *db, const ut8 *data) {
	for (ut64 at = 0; at <= db->size && db->size - at >= 6;) {
		bool opaque = false;
		for (size_t i = 0; i < db->addon_count; i++) {
			R2UnityBGDatabaseAddon *addon = &db->addons[i];
			ut64 end = addon->payload_offset + addon->payload_size;
			if (at >= addon->payload_offset && at < end) {
				at = end;
				opaque = true;
				break;
			}
		}
		if (opaque || at > db->size || db->size - at < 6) {
			continue;
		}
		ut32 length = r_read_le32 (data + at);
		if (length < 2
			|| length > R2U_BGDB_MAX_STRING
			|| length > db->size - at - 4) {
			at++;
			continue;
		}
		const ut8 *bytes = data + at + 4;
		if (!utf8_printable (bytes, length)) {
			at++;
			continue;
		}
		bool ok = append_string (db, at, length, bytes);
		if (!ok) {
			return false;
		}
		at += 4 + length;
	}
	return true;
}

static R2UnityBGDatabase *parse_internal(RBuffer *buf, bool strings) {
	ut64 size = r_buf_size (buf);
	if (size < R2UNITY_BGDB_HEADER_SIZE + 8) {
		return NULL;
	}
	R2UnityBGDatabase *db = R_NEW0 (R2UnityBGDatabase);
	db->buf = r_buf_new_slice (buf, 0, size);
	if (!db->buf) {
		free (db);
		return NULL;
	}
	db->size = size;
	R2UnityBGDatabaseReader r = {
		.buf = db->buf,
		.offset = 0,
		.limit = size,
	};
	ut32 addon_count;
	if (!reader_u32 (&r, &db->version) || db->version != R2U_BGDB_VERSION
		|| !reader_read (&r, db->repository_id, sizeof (db->repository_id))
		|| !reader_u32 (&r, &db->header_value)
		|| !reader_u32 (&r, &addon_count)
		|| !addon_count || addon_count > R2U_BGDB_MAX_ADDONS) {
		goto fail;
	}
	db->addon_count = addon_count;
	db->addons = R_NEWS0 (R2UnityBGDatabaseAddon, addon_count);
	if (!db->addons) {
		goto fail;
	}
	bool recognized = false;
	for (size_t i = 0; i < db->addon_count; i++) {
		R2UnityBGDatabaseAddon *addon = &db->addons[i];
		addon->offset = r.offset;
		if (!reader_string (&r, &addon->type, &addon->type_offset,
				&addon->type_size)) {
			goto fail;
		}
		recognized |= addon_type_valid (addon->type);
		ut32 payload_size;
		addon->payload_size_offset = r.offset;
		if (!reader_u32 (&r, &payload_size)) {
			goto fail;
		}
		addon->payload_offset = r.offset;
		addon->payload_size = payload_size;
		if (!reader_skip (&r, payload_size)) {
			goto fail;
		}
	}
	if (!recognized) {
		goto fail;
	}
	db->body_offset = r.offset;
	ut32 table_count;
	if (!reader_u32 (&r, &table_count) || !table_count
		|| table_count > R2U_BGDB_MAX_TABLES || size > INT_MAX) {
		goto fail;
	}
	int data_size = 0;
	ut8 *data = r_buf_read_all (db->buf, &data_size);
	bool valid = data && (ut64)data_size == size
		&& collect_tables (db, data, table_count)
		&& (!strings || collect_strings (db, data));
	free (data);
	if (!valid) {
		goto fail;
	}
	return db;

fail:
	r2unity_bgdatabase_free (db);
	return NULL;
}

bool r2unity_bgdatabase_check(RBuffer *buf) {
	R_RETURN_VAL_IF_FAIL (buf, false);
	R2UnityBGDatabase *db = parse_internal (buf, false);
	if (!db) {
		return false;
	}
	r2unity_bgdatabase_free (db);
	return true;
}

R2UnityBGDatabase *r2unity_bgdatabase_parse(RBuffer *buf) {
	R_RETURN_VAL_IF_FAIL (buf, NULL);
	return parse_internal (buf, true);
}

char *r2unity_bgdatabase_id_string(const ut8 id[16]) {
	R_RETURN_VAL_IF_FAIL (id, NULL);
	char *result = R_NEWS (char, 33);
	if (!result) {
		return NULL;
	}
	for (size_t i = 0; i < 16; i++) {
		snprintf (result + i * 2, 3, "%02x", id[i]);
	}
	return result;
}

void r2unity_bgdatabase_free(R2UnityBGDatabase *db) {
	if (!db) {
		return;
	}
	for (size_t i = 0; i < db->addon_count; i++) {
		free (db->addons[i].type);
	}
	for (size_t i = 0; i < db->string_count; i++) {
		free (db->strings[i].value);
	}
	free (db->addons);
	free (db->tables);
	free (db->strings);
	r_unref (db->buf);
	free (db);
}

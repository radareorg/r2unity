/* r2unity - MIT - Copyright 2025-2026 - pancake */

#define R_LOG_ORIGIN "r2unity.serialized_file"

#include "serialized_file.h"
#include <ctype.h>

#define R2U_SERIALIZED_VERSION 22
#define R2U_SERIALIZED_HEADER_SIZE 0x30
#define R2U_MAX_TYPES 65536
#define R2U_MAX_OBJECTS 1048576
#define R2U_MAX_REFERENCES 1048576
#define R2U_MAX_STRING (1024 * 1024)

typedef struct {
	RBuffer *buf;
	ut64 offset;
	ut64 limit;
	bool big_endian;
} R2UnityReader;

static bool reader_has(const R2UnityReader *r, ut64 size) {
	return r && r->offset <= r->limit && size <= r->limit - r->offset;
}

static bool reader_read(R2UnityReader *r, void *out, ut64 size) {
	if (!reader_has (r, size) || r_buf_read_at (r->buf, r->offset, out, size) != (st64)size) {
		return false;
	}
	r->offset += size;
	return true;
}

static bool reader_skip(R2UnityReader *r, ut64 size) {
	if (!reader_has (r, size)) {
		return false;
	}
	r->offset += size;
	return true;
}

static bool reader_align(R2UnityReader *r, ut64 alignment) {
	ut64 aligned = R_ROUND (r->offset, alignment);
	return aligned >= r->offset && reader_skip (r, aligned - r->offset);
}

static bool reader_u8(R2UnityReader *r, ut8 *out) {
	return reader_read (r, out, sizeof (*out));
}

static bool reader_u16(R2UnityReader *r, ut16 *out) {
	ut8 raw[2];
	if (!reader_read (r, raw, sizeof (raw))) {
		return false;
	}
	*out = r->big_endian? r_read_be16 (raw): r_read_le16 (raw);
	return true;
}

static bool reader_u32(R2UnityReader *r, ut32 *out) {
	ut8 raw[4];
	if (!reader_read (r, raw, sizeof (raw))) {
		return false;
	}
	*out = r->big_endian? r_read_be32 (raw): r_read_le32 (raw);
	return true;
}

static bool reader_u64(R2UnityReader *r, ut64 *out) {
	ut8 raw[8];
	if (!reader_read (r, raw, sizeof (raw))) {
		return false;
	}
	*out = r->big_endian? r_read_be64 (raw): r_read_le64 (raw);
	return true;
}

static bool reader_cstring(R2UnityReader *r, char **out, ut64 *string_offset) {
	ut64 start = r->offset;
	ut64 length = 0;
	ut8 ch = 0;
	while (reader_has (r, 1)) {
		if (!reader_u8 (r, &ch)) {
			return false;
		}
		if (!ch) {
			char *s = R_NEWS (char, length + 1);
			if (!s) {
				return false;
			}
			if (length && r_buf_read_at (r->buf, start, (ut8 *)s, length) != (st64)length) {
				free (s);
				return false;
			}
			s[length] = 0;
			*out = s;
			if (string_offset) {
				*string_offset = start;
			}
			return true;
		}
		if (++length > R2U_MAX_STRING) {
			return false;
		}
	}
	return false;
}

static bool buffer_range(RBuffer *buf, ut64 offset, ut64 size) {
	ut64 total = r_buf_size (buf);
	return offset <= total && size <= total - offset;
}

static ut32 buffer_u32(const R2UnitySerializedFile *sf, ut64 offset) {
	return r_buf_read_ble32_at (sf->buf, offset, sf->big_endian);
}

static ut64 buffer_u64(const R2UnitySerializedFile *sf, ut64 offset) {
	return r_buf_read_ble64_at (sf->buf, offset, sf->big_endian);
}

static bool object_string(const R2UnitySerializedFile *sf, const R2UnitySerializedObject *object, ut64 at, char **out, ut64 *string_offset, ut32 *string_size, ut64 *next) {
	ut64 end = object->offset + object->size;
	if (end < object->offset || at > end || end - at < 4) {
		return false;
	}
	ut32 length = buffer_u32 (sf, at);
	ut64 data = at + 4;
	if (length > R2U_MAX_STRING || data > end || length > end - data) {
		return false;
	}
	char *s = R_NEWS (char, (ut64)length + 1);
	if (!s) {
		return false;
	}
	if (length && r_buf_read_at (sf->buf, data, (ut8 *)s, length) != length) {
		free (s);
		return false;
	}
	s[length] = 0;
	*out = s;
	if (string_offset) {
		*string_offset = data;
	}
	if (string_size) {
		*string_size = length;
	}
	if (next) {
		ut64 aligned = R_ROUND (data + length, 4);
		if (aligned < data || aligned > end) {
			free (s);
			*out = NULL;
			return false;
		}
		*next = aligned;
	}
	return true;
}

static bool skip_type_tree(R2UnityReader *r) {
	ut32 node_count;
	ut32 string_size;
	if (!reader_u32 (r, &node_count) || !reader_u32 (r, &string_size)) {
		return false;
	}
	/* SerializedFile v19+ type-tree nodes include an eight-byte ref hash. */
	const ut64 node_size = 32;
	if (node_count > R2U_MAX_OBJECTS || node_count > UT64_MAX / node_size) {
		return false;
	}
	return reader_skip (r, (ut64)node_count * node_size)
		&& reader_skip (r, string_size);
}

static bool skip_dependencies(R2UnityReader *r) {
	ut32 count;
	if (!reader_u32 (r, &count) || count > R2U_MAX_TYPES) {
		return false;
	}
	return reader_skip (r, (ut64)count * 4);
}

static bool read_serialized_type(R2UnityReader *r, bool type_tree, bool reference_type, R2UnitySerializedType *out) {
	ut64 start = r->offset;
	ut32 class_id;
	ut8 stripped;
	ut16 script_type;
	if (!reader_u32 (r, &class_id) || !reader_u8 (r, &stripped) || !reader_u16 (r, &script_type)) {
		return false;
	}
	out->class_id = (st32)class_id;
	out->stripped = stripped != 0;
	out->script_type_index = (st16)script_type;
	if (out->class_id == 114 && !reader_skip (r, 16)) {
		return false;
	}
	if (!reader_skip (r, 16)) {
		return false;
	}
	if (type_tree && !skip_type_tree (r)) {
		return false;
	}
	if (reference_type) {
		char *class_name = NULL;
		char *name_space = NULL;
		char *assembly = NULL;
		bool ok = reader_cstring (r, &class_name, NULL)
			&& reader_cstring (r, &name_space, NULL)
			&& reader_cstring (r, &assembly, NULL);
		free (class_name);
		free (name_space);
		free (assembly);
		if (!ok) {
			return false;
		}
	} else if (type_tree && !skip_dependencies (r)) {
		return false;
	}
	out->offset = start;
	out->size = r->offset - start;
	return true;
}

const char *r2unity_serialized_class_name(st32 class_id) {
	switch (class_id) {
	case 21:
		return "Material";
	case 28:
		return "Texture2D";
	case 43:
		return "Mesh";
	case 49:
		return "TextAsset";
	case 74:
		return "AnimationClip";
	case 83:
		return "AudioClip";
	case 91:
		return "AnimatorController";
	case 114:
		return "MonoBehaviour";
	case 150:
		return "PreloadData";
	case 328:
		return "VideoClip";
	default:
		return "Object";
	}
}

static bool class_has_name(st32 class_id) {
	switch (class_id) {
	case 21:
	case 28:
	case 43:
	case 49:
	case 74:
	case 83:
	case 91:
	case 150:
	case 328:
		return true;
	default:
		return false;
	}
}

static char *read_sidecar_path(const R2UnitySerializedFile *sf, ut64 offset,
		ut32 size) {
	char *path = R_NEWS (char, (ut64)size + 1);
	if (!path || r_buf_read_at (sf->buf, offset, (ut8 *)path, size) != size) {
		free (path);
		return NULL;
	}
	path[size] = 0;
	if (memchr (path, 0, size)
		|| (!r_str_endswith (path, ".resS")
			&& !r_str_endswith (path, ".resource"))) {
		free (path);
		return NULL;
	}
	return path;
}

static void parse_stream_info(R2UnitySerializedFile *sf,
		R2UnitySerializedObject *object, ut64 after_name) {
	ut64 end = object->offset + object->size;
	/* Texture2D/Mesh StreamingInfo: u64 offset, u32 size, string path. */
	for (ut64 at = after_name; at <= end && end - at >= 16; at += 4) {
		ut64 stream_offset = buffer_u64 (sf, at);
		ut32 stream_size = buffer_u32 (sf, at + 8);
		ut32 path_size = buffer_u32 (sf, at + 12);
		ut64 path_offset = at + 16;
		if (!stream_size || !path_size || path_size > R2U_MAX_STRING
			|| path_offset > end || path_size > end - path_offset) {
			continue;
		}
		ut64 aligned_end = R_ROUND (path_offset + path_size, 4);
		if (aligned_end != end) {
			continue;
		}
		char *path = read_sidecar_path (sf, path_offset, path_size);
		if (path) {
			object->stream_path = path;
			object->stream_offset = stream_offset;
			object->stream_size = stream_size;
			return;
		}
	}

	/* AudioClip/VideoClip resource: string path, u64 offset, u64 size. */
	for (ut64 at = after_name; at <= end && end - at >= 20; at += 4) {
		ut32 path_size = buffer_u32 (sf, at);
		ut64 path_offset = at + 4;
		if (!path_size || path_size > R2U_MAX_STRING
			|| path_offset > end || path_size > end - path_offset) {
			continue;
		}
		ut64 stream_info = R_ROUND (path_offset + path_size, 4);
		if (stream_info < path_offset || stream_info > end
			|| end - stream_info < 16) {
			continue;
		}
		ut64 stream_offset = buffer_u64 (sf, stream_info);
		ut64 stream_size = buffer_u64 (sf, stream_info + 8);
		if (!stream_size) {
			continue;
		}
		char *path = read_sidecar_path (sf, path_offset, path_size);
		if (path) {
			object->stream_path = path;
			object->stream_offset = stream_offset;
			object->stream_size = stream_size;
			return;
		}
	}
}

static void parse_texture_info(R2UnitySerializedFile *sf,
		R2UnitySerializedObject *object, ut64 after_name) {
	ut64 end = object->offset + object->size;
	if (after_name <= end && end - after_name >= 28) {
		object->width = buffer_u32 (sf, after_name + 8);
		object->height = buffer_u32 (sf, after_name + 12);
		object->texture_format = (st32)buffer_u32 (sf, after_name + 24);
		object->has_texture_info = object->width > 0 && object->height > 0;
	}
	parse_stream_info (sf, object, after_name);
}

static bool parse_object_details(R2UnitySerializedFile *sf, R2UnitySerializedObject *object) {
	ut64 after_name = object->offset;
	if (class_has_name (object->class_id)) {
		if (!object_string (sf, object, object->offset, &object->name,
				&object->name_offset, &object->name_size, &after_name)) {
			return false;
		}
		if (!*object->name) {
			R_FREE (object->name);
		}
	} else if (object->class_id == 114) {
		/* v22 MonoBehaviour: GameObject PPtr, enabled+alignment, Script PPtr, name. */
		ut64 name_at = object->offset + 28;
		if (!object_string (sf, object, name_at, &object->name,
				&object->name_offset, &object->name_size, &after_name)) {
			return false;
		}
		if (!*object->name) {
			R_FREE (object->name);
		}
	}
	if (object->class_id == 49) {
		ut64 end = object->offset + object->size;
		if (after_name > end || end - after_name < 4) {
			return false;
		}
		ut32 payload_size = buffer_u32 (sf, after_name);
		ut64 payload_offset = after_name + 4;
		if (payload_offset > end || payload_size > end - payload_offset) {
			return false;
		}
		object->payload_offset = payload_offset;
		object->payload_size = payload_size;
	} else if (object->class_id == 28) {
		parse_texture_info (sf, object, after_name);
	} else if (object->class_id == 43 || object->class_id == 83
		|| object->class_id == 328) {
		parse_stream_info (sf, object, after_name);
	}
	return true;
}

bool r2unity_serialized_file_check(RBuffer *buf) {
	R_RETURN_VAL_IF_FAIL (buf, false);
	ut64 actual_size = r_buf_size (buf);
	if (actual_size < R2U_SERIALIZED_HEADER_SIZE) {
		return false;
	}
	ut8 header[R2U_SERIALIZED_HEADER_SIZE];
	if (r_buf_read_at (buf, 0, header, sizeof (header)) != sizeof (header)) {
		return false;
	}
	ut32 version = r_read_be32 (header + 8);
	if (version != R2U_SERIALIZED_VERSION || header[16] > 1) {
		return false;
	}
	ut32 metadata_size = r_read_be32 (header + 20);
	ut64 file_size = r_read_be64 (header + 24);
	ut64 data_offset = r_read_be64 (header + 32);
	if (file_size != actual_size || metadata_size < 16
		|| data_offset < R2U_SERIALIZED_HEADER_SIZE || data_offset > file_size
		|| metadata_size > data_offset - R2U_SERIALIZED_HEADER_SIZE) {
		return false;
	}
	/* The metadata begins with a NUL-terminated Unity version string. */
	ut8 first;
	if (r_buf_read_at (buf, R2U_SERIALIZED_HEADER_SIZE, &first, 1) != 1 || !isdigit (first)) {
		return false;
	}
	ut64 string_limit = R_MIN ((ut64)metadata_size, 64);
	bool nul = false;
	for (ut64 i = 0; i < string_limit; i++) {
		ut8 ch;
		if (r_buf_read_at (buf, R2U_SERIALIZED_HEADER_SIZE + i, &ch, 1) != 1) {
			return false;
		}
		if (!ch) {
			nul = i > 0;
			break;
		}
	}
	return nul;
}

R2UnitySerializedFile *r2unity_serialized_file_parse(RBuffer *buf) {
	if (!r2unity_serialized_file_check (buf)) {
		return NULL;
	}
	R2UnitySerializedFile *sf = R_NEW0 (R2UnitySerializedFile);
	sf->buf = r_buf_new_slice (buf, 0, r_buf_size (buf));
	if (!sf->buf) {
		free (sf);
		return NULL;
	}
	sf->version = r_buf_read_be32_at (sf->buf, 8);
	sf->big_endian = r_buf_read8_at (sf->buf, 16) != 0;
	sf->metadata_size = r_buf_read_be32_at (sf->buf, 20);
	sf->file_size = r_buf_read_be64_at (sf->buf, 24);
	sf->data_offset = r_buf_read_be64_at (sf->buf, 32);
	sf->header_size = R2U_SERIALIZED_HEADER_SIZE;

	R2UnityReader r = {
		.buf = sf->buf,
		.offset = sf->header_size,
		.limit = sf->header_size + sf->metadata_size,
		.big_endian = sf->big_endian,
	};
	ut32 platform;
	ut8 type_tree;
	ut32 type_count;
	if (!reader_cstring (&r, &sf->unity_version, NULL)
		|| !reader_u32 (&r, &platform)
		|| !reader_u8 (&r, &type_tree)
		|| !reader_u32 (&r, &type_count)
		|| type_count > R2U_MAX_TYPES) {
		goto fail;
	}
	sf->target_platform = (st32)platform;
	sf->enable_type_tree = type_tree != 0;
	sf->type_count = type_count;
	sf->types = R_NEWS0 (R2UnitySerializedType, type_count);
	if (type_count && !sf->types) {
		goto fail;
	}
	for (size_t i = 0; i < sf->type_count; i++) {
		if (!read_serialized_type (&r, sf->enable_type_tree, false, &sf->types[i])) {
			goto fail;
		}
	}

	ut32 object_count;
	if (!reader_u32 (&r, &object_count) || object_count > R2U_MAX_OBJECTS) {
		goto fail;
	}
	sf->object_count = object_count;
	sf->objects = R_NEWS0 (R2UnitySerializedObject, object_count);
	if (object_count && !sf->objects) {
		goto fail;
	}
	for (size_t i = 0; i < sf->object_count; i++) {
		R2UnitySerializedObject *object = &sf->objects[i];
		ut64 path_id;
		ut64 byte_start;
		ut32 byte_size;
		ut32 type_index;
		if (!reader_align (&r, 4)) {
			goto fail;
		}
		object->table_offset = r.offset;
		if (!reader_u64 (&r, &path_id) || !reader_u64 (&r, &byte_start)
			|| !reader_u32 (&r, &byte_size) || !reader_u32 (&r, &type_index)
			|| type_index >= sf->type_count || byte_start > UT64_MAX - sf->data_offset) {
			goto fail;
		}
		object->path_id = (st64)path_id;
		object->type_index = type_index;
		object->class_id = sf->types[type_index].class_id;
		object->offset = sf->data_offset + byte_start;
		object->size = byte_size;
		if (!buffer_range (sf->buf, object->offset, object->size)) {
			goto fail;
		}
	}

	ut32 script_count;
	if (!reader_u32 (&r, &script_count) || script_count > R2U_MAX_REFERENCES) {
		goto fail;
	}
	sf->script_count = script_count;
	sf->scripts = R_NEWS0 (R2UnitySerializedScript, script_count);
	if (script_count && !sf->scripts) {
		goto fail;
	}
	for (size_t i = 0; i < sf->script_count; i++) {
		ut32 file_index;
		ut64 path_id;
		if (!reader_align (&r, 4) || !reader_u32 (&r, &file_index) || !reader_u64 (&r, &path_id)) {
			goto fail;
		}
		sf->scripts[i].file_index = (st32)file_index;
		sf->scripts[i].path_id = (st64)path_id;
	}

	ut32 external_count;
	if (!reader_u32 (&r, &external_count) || external_count > R2U_MAX_REFERENCES) {
		goto fail;
	}
	sf->external_count = external_count;
	sf->externals = R_NEWS0 (R2UnitySerializedExternal, external_count);
	if (external_count && !sf->externals) {
		goto fail;
	}
	for (size_t i = 0; i < sf->external_count; i++) {
		R2UnitySerializedExternal *external = &sf->externals[i];
		char *empty = NULL;
		ut32 type;
		if (!reader_cstring (&r, &empty, NULL) || !reader_read (&r, external->guid, sizeof (external->guid))
			|| !reader_u32 (&r, &type)
			|| !reader_cstring (&r, &external->path, &external->path_offset)) {
			free (empty);
			goto fail;
		}
		free (empty);
		external->type = (st32)type;
	}

	ut32 reference_type_count;
	if (!reader_u32 (&r, &reference_type_count) || reference_type_count > R2U_MAX_TYPES) {
		goto fail;
	}
	for (ut32 i = 0; i < reference_type_count; i++) {
		R2UnitySerializedType type = {0};
		if (!read_serialized_type (&r, sf->enable_type_tree, true, &type)) {
			goto fail;
		}
	}
	if (!reader_cstring (&r, &sf->user_information, NULL)) {
		goto fail;
	}
	for (size_t i = 0; i < sf->object_count; i++) {
		if (!parse_object_details (sf, &sf->objects[i])) {
			goto fail;
		}
	}
	return sf;

fail:
	r2unity_serialized_file_free (sf);
	return NULL;
}

void r2unity_serialized_file_free(R2UnitySerializedFile *sf) {
	if (!sf) {
		return;
	}
	for (size_t i = 0; i < sf->object_count; i++) {
		free (sf->objects[i].name);
		free (sf->objects[i].stream_path);
	}
	for (size_t i = 0; i < sf->external_count; i++) {
		free (sf->externals[i].path);
	}
	free (sf->unity_version);
	free (sf->types);
	free (sf->objects);
	free (sf->scripts);
	free (sf->externals);
	free (sf->user_information);
	r_unref (sf->buf);
	free (sf);
}

#ifndef R2UNITY_SERIALIZED_FILE_H
#define R2UNITY_SERIALIZED_FILE_H

#include "lib.h"

typedef struct {
	st32 class_id;
	st16 script_type_index;
	ut64 offset;
	ut64 size;
	bool stripped;
} R2UnitySerializedType;

typedef struct {
	st64 path_id;
	st32 class_id;
	ut32 type_index;
	ut64 table_offset;
	ut64 offset;
	ut64 size;
	char *name;
	ut64 name_offset;
	ut32 name_size;
	ut64 payload_offset;
	ut64 payload_size;
	char *stream_path;
	ut64 stream_offset;
	ut64 stream_size;
	ut32 width;
	ut32 height;
	st32 texture_format;
	bool has_texture_info;
} R2UnitySerializedObject;

typedef struct {
	st32 file_index;
	st64 path_id;
} R2UnitySerializedScript;

typedef struct {
	char *path;
	ut64 path_offset;
	ut8 guid[16];
	st32 type;
} R2UnitySerializedExternal;

typedef struct {
	RBuffer *buf;
	ut32 version;
	ut32 metadata_size;
	ut64 file_size;
	ut64 data_offset;
	ut64 header_size;
	bool big_endian;
	char *unity_version;
	st32 target_platform;
	bool enable_type_tree;
	R2UnitySerializedType *types;
	size_t type_count;
	R2UnitySerializedObject *objects;
	size_t object_count;
	R2UnitySerializedScript *scripts;
	size_t script_count;
	R2UnitySerializedExternal *externals;
	size_t external_count;
	char *user_information;
} R2UnitySerializedFile;

bool r2unity_serialized_file_check(RBuffer *buf);
R2UnitySerializedFile *r2unity_serialized_file_parse(RBuffer *buf);
void r2unity_serialized_file_free(R2UnitySerializedFile *sf);
const char *r2unity_serialized_class_name(st32 class_id);

#endif

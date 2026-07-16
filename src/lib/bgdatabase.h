#ifndef R2UNITY_BGDATABASE_H
#define R2UNITY_BGDATABASE_H

#include "lib.h"

#define R2UNITY_BGDB_HEADER_SIZE 0x1c

typedef struct {
	ut64 offset;
	ut64 type_offset;
	ut32 type_size;
	char *type;
	ut64 payload_size_offset;
	ut64 payload_offset;
	ut64 payload_size;
} R2UnityBGDatabaseAddon;

typedef struct {
	ut64 offset;
	ut64 size;
	ut64 name_field_offset;
	ut32 field_count;
	ut16 name_field_type;
} R2UnityBGDatabaseTable;

typedef struct {
	ut64 prefix_offset;
	ut64 offset;
	ut32 size;
	char *value;
} R2UnityBGDatabaseString;

typedef struct {
	RBuffer *buf;
	ut64 size;
	ut32 version;
	ut8 repository_id[16];
	ut32 header_value;
	ut64 body_offset;
	R2UnityBGDatabaseAddon *addons;
	size_t addon_count;
	R2UnityBGDatabaseTable *tables;
	size_t table_count;
	R2UnityBGDatabaseString *strings;
	size_t string_count;
} R2UnityBGDatabase;

bool r2unity_bgdatabase_check(RBuffer *buf);
R2UnityBGDatabase *r2unity_bgdatabase_parse(RBuffer *buf);
void r2unity_bgdatabase_free(R2UnityBGDatabase *db);
char *r2unity_bgdatabase_id_string(const ut8 id[16]);

#endif

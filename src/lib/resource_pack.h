#ifndef R2UNITY_RESOURCE_PACK_H
#define R2UNITY_RESOURCE_PACK_H

#include "lib.h"

typedef struct {
	ut64 descriptor_offset;
	ut64 name_size_offset;
	ut64 name_offset;
	ut32 name_size;
	char *name;
	ut64 payload_offset;
	ut64 payload_size;
} R2UnityResourcePackEntry;

typedef struct {
	RBuffer *buf;
	ut64 size;
	ut32 records_size;
	ut64 payload_offset;
	R2UnityResourcePackEntry *entries;
	size_t entry_count;
} R2UnityResourcePack;

bool r2unity_resource_pack_check(RBuffer *buf);
R2UnityResourcePack *r2unity_resource_pack_parse(RBuffer *buf);
void r2unity_resource_pack_free(R2UnityResourcePack *pack);

#endif

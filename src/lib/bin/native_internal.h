/* r2unity - MIT - Copyright 2025-2026 - pancake */

#ifndef R2UNITY_NATIVE_INTERNAL_H
#define R2UNITY_NATIVE_INTERNAL_H

#include "../lib.h"

typedef const ut8 *(*R2UnityNativePtrAt)(void *user, ut64 va);

typedef struct {
	void *user;
	R2UnityNativePtrAt ptr_at;
	int ptr_size;
	ut64 base_vaddr;
	ut64 text_lo;
	ut64 text_hi;
} R2UnityNativeView;

typedef struct {
	const char *name;
	ut64 vaddr;
	ut64 vsize;
	ut64 size;
	ut32 perm;
	bool is_data;
} R2UnityNativeSection;

bool r2unity_native_run_view(R2UnityMetadata *meta, R2UnityNativeView *view, const R2UnityNativeSection *sections, size_t section_count, const R2UnityNativeOptions *options, R2UnityNativeResult *result);
bool r2unity_find_method_pointers_elf(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result);
bool r2unity_find_method_pointers_macho(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result);
bool r2unity_find_method_pointers_pe(R2UnityMetadata *meta, const char *path, const R2UnityNativeOptions *options, R2UnityNativeResult *result);

#endif

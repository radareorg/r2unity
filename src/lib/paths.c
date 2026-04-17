#define R_LOG_ORIGIN "r2unity.paths"

#include "lib.h"
#include <r_util.h>
#include <string.h>
#include <strings.h>

static char *pjoin2 (const char *a, const char *b) {
	return r_str_newf ("%s/%s", a, b);
}

static bool str_ieq (const char *a, const char *b) {
	return a && b && !strcasecmp (a, b);
}

static char *strip_ext (const char *name, const char *ext) {
	size_t nlen = strlen (name);
	size_t elen = strlen (ext);
	if (nlen > elen && !strcasecmp (name + nlen - elen, ext)) {
		return r_str_ndup (name, (int) (nlen - elen));
	}
	return strdup (name);
}

static const char *il2cpp_basename_for (const char *platform) {
	if (!platform) return NULL;
	if (!strcmp (platform, "ios")) return "UnityFramework";
	if (!strcmp (platform, "macos")) return "GameAssembly.dylib";
	if (!strcmp (platform, "windows")) return "GameAssembly.dll";
	if (!strcmp (platform, "linux")) return "GameAssembly.so";
	if (!strcmp (platform, "android")) return "libil2cpp.so";
	return NULL;
}

static bool is_il2cpp_basename (const char *base) {
	return str_ieq (base, "UnityFramework")
		|| str_ieq (base, "libil2cpp.so")
		|| str_ieq (base, "GameAssembly.dll")
		|| str_ieq (base, "GameAssembly.so")
		|| str_ieq (base, "GameAssembly.dylib");
}

static char *find_il2cpp_sibling (const char *dir) {
	static const char *const names[] = {
		"UnityFramework",
		"GameAssembly.dylib",
		"GameAssembly.dll",
		"GameAssembly.so",
		"libil2cpp.so",
		NULL
	};
	for (int i = 0; names[i]; i++) {
		char *cand = pjoin2 (dir, names[i]);
		if (r_file_exists (cand)) {
			return cand;
		}
		free (cand);
	}
	return NULL;
}

static bool try_macos (R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Layout: Game.app/Contents/MacOS/Game
	 *         Game.app/Contents/Frameworks/GameAssembly.dylib
	 *         Game.app/Contents/Resources/Data/il2cpp_data/Metadata/global-metadata.dat */
	char *contents = NULL;
	if (r_str_endswith (dir, "/Contents/MacOS")) {
		contents = r_file_dirname (dir);
	} else if (r_str_endswith (dir, "/Contents/Frameworks") && str_ieq (base, "GameAssembly.dylib")) {
		contents = r_file_dirname (dir);
	} else {
		return false;
	}
	char *resources = pjoin2 (contents, "Resources");
	char *data_dir = pjoin2 (resources, "Data");
	char *metadata = r_str_newf ("%s/il2cpp_data/Metadata/global-metadata.dat", data_dir);
	char *il2cpp = r_str_newf ("%s/Frameworks/GameAssembly.dylib", contents);
	if (r_file_exists (metadata)) {
		p->platform = strdup ("macos");
		p->metadata = metadata;
		p->data_dir = data_dir;
		if (r_file_exists (il2cpp)) {
			p->il2cpp_binary = il2cpp;
		} else if (str_ieq (base, "GameAssembly.dylib")) {
			p->il2cpp_binary = strdup (abs);
			free (il2cpp);
		} else {
			free (il2cpp);
		}
		free (resources);
		free (contents);
		return true;
	}
	free (metadata);
	free (data_dir);
	free (il2cpp);
	free (resources);
	free (contents);
	return false;
}

static bool try_ios (R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Framework layout: Game.app/Frameworks/UnityFramework.framework/UnityFramework
	 * Flat iOS layout:  Game.app/UnityFramework (older builds)
	 * Metadata:         Game.app/Data/Managed/Metadata/global-metadata.dat
	 *                   Game.app/Data/Raw/Managed/Metadata/global-metadata.dat (newer) */
	char *app_dir = NULL;
	if (r_str_endswith (dir, ".framework") && str_ieq (base, "UnityFramework")) {
		char *frameworks = r_file_dirname (dir);
		app_dir = r_file_dirname (frameworks);
		free (frameworks);
	} else if (r_str_endswith (dir, ".app")) {
		app_dir = strdup (dir);
	} else {
		return false;
	}
	const char *candidates[] = {
		"Data/Managed/Metadata/global-metadata.dat",
		"Data/Raw/Managed/Metadata/global-metadata.dat",
		NULL
	};
	for (int i = 0; candidates[i]; i++) {
		char *metadata = pjoin2 (app_dir, candidates[i]);
		if (r_file_exists (metadata)) {
			char *il2cpp_fw = r_str_newf ("%s/Frameworks/UnityFramework.framework/UnityFramework", app_dir);
			char *il2cpp_flat = pjoin2 (app_dir, "UnityFramework");
			p->platform = strdup ("ios");
			p->metadata = metadata;
			p->data_dir = pjoin2 (app_dir, "Data");
			if (str_ieq (base, "UnityFramework")) {
				p->il2cpp_binary = strdup (abs);
				free (il2cpp_fw);
				free (il2cpp_flat);
			} else if (r_file_exists (il2cpp_fw)) {
				p->il2cpp_binary = il2cpp_fw;
				free (il2cpp_flat);
			} else if (r_file_exists (il2cpp_flat)) {
				p->il2cpp_binary = il2cpp_flat;
				free (il2cpp_fw);
			} else {
				free (il2cpp_fw);
				free (il2cpp_flat);
			}
			free (app_dir);
			return true;
		}
		free (metadata);
	}
	free (app_dir);
	return false;
}

static bool try_winlin_standalone (R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Windows: Game.exe        + GameAssembly.dll + Game_Data/il2cpp_data/Metadata/global-metadata.dat
	 * Linux:   Game (ELF)      + GameAssembly.so  + Game_Data/il2cpp_data/Metadata/global-metadata.dat
	 *
	 * `base` is either the main exe name ("Game"/"Game.exe") or the IL2CPP lib itself. */
	char *stem = NULL;
	const char *il2cpp_name = NULL;
	if (str_ieq (base, "GameAssembly.dll")) {
		il2cpp_name = "GameAssembly.dll";
	} else if (str_ieq (base, "GameAssembly.so")) {
		il2cpp_name = "GameAssembly.so";
	} else {
		stem = strip_ext (base, ".exe");
	}

	/* If il2cpp was the input, try every *_Data sibling by probing common names.
	 * Otherwise derive the Data dir from the exe stem. */
	const char *stem_candidates[8] = {0};
	int n_stems = 0;
	if (stem) {
		stem_candidates[n_stems++] = stem;
	}

	bool ok = false;
	char *data_dir = NULL;
	char *metadata = NULL;
	for (int i = 0; i < n_stems; i++) {
		char *try_data = r_str_newf ("%s/%s_Data", dir, stem_candidates[i]);
		char *try_meta = r_str_newf ("%s/il2cpp_data/Metadata/global-metadata.dat", try_data);
		if (r_file_exists (try_meta)) {
			data_dir = try_data;
			metadata = try_meta;
			ok = true;
			break;
		}
		free (try_meta);
		free (try_data);
	}

	/* If input was GameAssembly.{dll,so}, scan siblings for *_Data dirs. */
	if (!ok && il2cpp_name) {
		RList *entries = r_sys_dir (dir);
		if (entries) {
			RListIter *it;
			char *name;
			r_list_foreach (entries, it, name) {
				size_t nl = strlen (name);
				if (nl < 6 || strcmp (name + nl - 5, "_Data")) {
					continue;
				}
				char *try_data = pjoin2 (dir, name);
				char *try_meta = r_str_newf ("%s/il2cpp_data/Metadata/global-metadata.dat", try_data);
				if (r_file_exists (try_meta)) {
					data_dir = try_data;
					metadata = try_meta;
					ok = true;
					break;
				}
				free (try_meta);
				free (try_data);
			}
			r_list_free (entries);
		}
	}

	if (!ok) {
		free (stem);
		return false;
	}

	char *il2cpp_dll = pjoin2 (dir, "GameAssembly.dll");
	char *il2cpp_so = pjoin2 (dir, "GameAssembly.so");
	if (il2cpp_name) {
		p->il2cpp_binary = strdup (abs);
		p->platform = strdup (str_ieq (il2cpp_name, "GameAssembly.dll") ? "windows" : "linux");
		free (il2cpp_dll);
		free (il2cpp_so);
	} else if (r_file_exists (il2cpp_dll)) {
		p->il2cpp_binary = il2cpp_dll;
		p->platform = strdup ("windows");
		free (il2cpp_so);
	} else if (r_file_exists (il2cpp_so)) {
		p->il2cpp_binary = il2cpp_so;
		p->platform = strdup ("linux");
		free (il2cpp_dll);
	} else {
		free (il2cpp_dll);
		free (il2cpp_so);
	}
	p->data_dir = data_dir;
	p->metadata = metadata;
	free (stem);
	return true;
}

static bool try_android_apk (R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Extracted APK: <root>/lib/<abi>/libil2cpp.so
	 *                <root>/assets/bin/Data/Managed/Metadata/global-metadata.dat */
	if (!str_ieq (base, "libil2cpp.so")) {
		return false;
	}
	char *abi_parent = r_file_dirname (dir);     /* <root>/lib */
	if (!abi_parent) {
		return false;
	}
	char *root = r_file_dirname (abi_parent);    /* <root> */
	const char *candidates[] = {
		"assets/bin/Data/Managed/Metadata/global-metadata.dat",
		"assets/bin/Data/Managed/Metadata/global-metadata.dat.so",
		NULL
	};
	for (int i = 0; candidates[i]; i++) {
		char *metadata = pjoin2 (root, candidates[i]);
		if (r_file_exists (metadata)) {
			p->platform = strdup ("android");
			p->il2cpp_binary = strdup (abs);
			p->metadata = metadata;
			p->data_dir = pjoin2 (root, "assets/bin/Data");
			free (abi_parent);
			free (root);
			return true;
		}
		free (metadata);
	}
	free (abi_parent);
	free (root);
	return false;
}

static bool try_fixture (R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Flat fixture: main exe + il2cpp binary + global-metadata.dat all in the
	 * same directory. Used by users dumping files
	 * manually for quick triage. */
	char *metadata = pjoin2 (dir, "global-metadata.dat");
	if (!r_file_exists (metadata)) {
		free (metadata);
		return false;
	}
	p->platform = strdup ("fixture");
	p->metadata = metadata;
	p->data_dir = strdup (dir);
	if (is_il2cpp_basename (base)) {
		p->il2cpp_binary = strdup (abs);
	} else {
		p->il2cpp_binary = find_il2cpp_sibling (dir);
	}
	return true;
}

R_API R2UnityPaths *r2unity_detect_paths (const char *input) {
	if (R_STR_ISEMPTY (input)) {
		return NULL;
	}
	char *abs = r_file_abspath (input);
	if (!abs || !r_file_exists (abs)) {
		free (abs);
		return NULL;
	}
	R2UnityPaths *p = R_NEW0 (R2UnityPaths);
	if (!p) {
		free (abs);
		return NULL;
	}
	p->main_executable = abs;

	char *dir = r_file_dirname (abs);
	const char *base = r_file_basename (abs);

	bool ok =
		try_macos (p, abs, dir, base)
		|| try_ios (p, abs, dir, base)
		|| try_android_apk (p, abs, dir, base)
		|| try_winlin_standalone (p, abs, dir, base)
		|| try_fixture (p, abs, dir, base);

	free (dir);
	if (!ok) {
		r2unity_free_paths (p);
		return NULL;
	}
	return p;
}

R_API void r2unity_free_paths (R2UnityPaths *p) {
	if (!p) {
		return;
	}
	free (p->platform);
	free (p->main_executable);
	free (p->il2cpp_binary);
	free (p->metadata);
	free (p->data_dir);
	free (p);
}

R_API const char *r2unity_platform_il2cpp_name (const char *platform) {
	return il2cpp_basename_for (platform);
}

#define R_LOG_ORIGIN "r2unity.paths"

#include "lib.h"
#include <r_util.h>

static bool str_ieq(const char *a, const char *b) {
	return a && b && !strcasecmp (a, b);
}

static char *pjoin(const char *a, const char *b) {
	return r_str_newf ("%s/%s", a, b);
}

/* Take ownership of `candidate` if it exists on disk; otherwise free it. */
static bool take_if_exists(char **out, char *candidate) {
	if (r_file_exists (candidate)) {
		*out = candidate;
		return true;
	}
	free (candidate);
	return false;
}

/* IL2CPP native-library basenames, in the order the flat/fixture detector
 * should probe for them. NULL platform entries are still valid basenames
 *(the fixture layout has no platform to report). */
static const struct {
	const char *platform;
	const char *basename;
} il2cpp_map[] = {
	{ "ios", "UnityFramework" },
	{ "macos", "GameAssembly.dylib" },
	{ "windows", "GameAssembly.dll" },
	{ "linux", "GameAssembly.so" },
	{ "android", "libil2cpp.so" },
};

static const char *il2cpp_basename_for(const char *platform) {
	for (size_t i = 0; i < sizeof (il2cpp_map) / sizeof (il2cpp_map[0]); i++) {
		if (!strcmp (platform, il2cpp_map[i].platform)) {
			return il2cpp_map[i].basename;
		}
	}
	return NULL;
}

static bool is_il2cpp_basename(const char *base) {
	for (size_t i = 0; i < sizeof (il2cpp_map) / sizeof (il2cpp_map[0]); i++) {
		if (str_ieq (base, il2cpp_map[i].basename)) {
			return true;
		}
	}
	return false;
}

static char *find_il2cpp_sibling(const char *dir) {
	char *found = NULL;
	for (size_t i = 0; i < sizeof (il2cpp_map) / sizeof (il2cpp_map[0]); i++) {
		if (take_if_exists (&found, pjoin (dir, il2cpp_map[i].basename))) {
			return found;
		}
	}
	return NULL;
}

static char *strip_ext_icase(const char *name, const char *ext) {
	size_t nlen = strlen (name);
	size_t elen = strlen (ext);
	if (nlen > elen && !strcasecmp (name + nlen - elen, ext)) {
		return r_str_ndup (name, (int) (nlen - elen));
	}
	return strdup (name);
}

/* Pick the first *_Data directory whose il2cpp_data/Metadata/global-metadata.dat
 * exists. Returns true and hands ownership via out_data/out_meta on success. */
static bool take_winlin_data_dir(const char *dir, char **out_data, char **out_meta) {
	RList *entries = r_sys_dir (dir);
	if (!entries) {
		return false;
	}
	bool ok = false;
	RListIter *it;
	char *name;
	r_list_foreach (entries, it, name) {
		size_t nl = strlen (name);
		if (nl < 6 || strcmp (name + nl - 5, "_Data")) {
			continue;
		}
		char *try_data = r_file_new (dir, name, NULL);
		char *try_meta = r_file_new (try_data, "il2cpp_data", "Metadata", "global-metadata.dat", NULL);
		if (r_file_exists (try_meta)) {
			*out_data = try_data;
			*out_meta = try_meta;
			ok = true;
			break;
		}
		free (try_meta);
		free (try_data);
	}
	r_list_free (entries);
	return ok;
}

static bool try_macos(R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
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
	char *data_dir = r_file_new (contents, "Resources", "Data", NULL);
	char *metadata = r_file_new (data_dir, "il2cpp_data", "Metadata", "global-metadata.dat", NULL);
	if (!r_file_exists (metadata)) {
		free (metadata);
		free (data_dir);
		free (contents);
		return false;
	}
	p->platform = strdup ("macos");
	p->metadata = metadata;
	p->data_dir = data_dir;
	if (str_ieq (base, "GameAssembly.dylib")) {
		p->il2cpp_binary = strdup (abs);
	} else {
		take_if_exists (&p->il2cpp_binary, r_file_new (contents, "Frameworks", "GameAssembly.dylib", NULL));
	}
	free (contents);
	return true;
}

static char *find_ios_metadata(const char *app_dir) {
	static const char *const metas[] = {
		"Data/Managed/Metadata/global-metadata.dat",
		"Data/Raw/Managed/Metadata/global-metadata.dat",
		NULL
	};
	char *metadata = NULL;
	for (int i = 0; metas[i]; i++) {
		if (take_if_exists (&metadata, pjoin (app_dir, metas[i]))) {
			return metadata;
		}
	}
	return NULL;
}

static bool try_ios(R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Framework layout: Game.app/Frameworks/UnityFramework.framework/UnityFramework
	 * Flat iOS layout:  Game.app/UnityFramework (older builds)
	 * Extracted app root: Game/Data plus the main Mach-O beside Data
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
		app_dir = strdup (dir);
	}
	char *metadata = find_ios_metadata (app_dir);
	if (!metadata) {
		free (app_dir);
		return false;
	}
	p->platform = strdup ("ios");
	p->metadata = metadata;
	p->data_dir = pjoin (app_dir, "Data");
	if (str_ieq (base, "UnityFramework")) {
		p->il2cpp_binary = strdup (abs);
	} else if (!take_if_exists (&p->il2cpp_binary,
			r_file_new (app_dir, "Frameworks", "UnityFramework.framework", "UnityFramework", NULL))) {
		if (!take_if_exists (&p->il2cpp_binary, pjoin (app_dir, "UnityFramework"))) {
			p->il2cpp_binary = strdup (abs);
		}
	}
	free (app_dir);
	return true;
}

static bool try_winlin_standalone(R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Windows: Game.exe   + GameAssembly.dll + Game_Data/il2cpp_data/Metadata/global-metadata.dat
	 * Linux:   Game (ELF) + GameAssembly.so  + Game_Data/il2cpp_data/Metadata/global-metadata.dat
	 *
	 * `base` is either the main exe name ("Game"/"Game.exe") or the IL2CPP lib itself. */
	const bool is_dll = str_ieq (base, "GameAssembly.dll");
	const bool is_so = str_ieq (base, "GameAssembly.so");
	const bool is_il2cpp = is_dll || is_so;

	char *data_dir = NULL;
	char *metadata = NULL;
	if (is_il2cpp) {
		if (!take_winlin_data_dir (dir, &data_dir, &metadata)) {
			return false;
		}
	} else {
		char *stem = strip_ext_icase (base, ".exe");
		char *data_name = r_str_newf ("%s_Data", stem);
		free (stem);
		data_dir = r_file_new (dir, data_name, NULL);
		free (data_name);
		metadata = r_file_new (data_dir, "il2cpp_data", "Metadata", "global-metadata.dat", NULL);
		if (!r_file_exists (metadata)) {
			free (metadata);
			free (data_dir);
			return false;
		}
	}

	if (is_il2cpp) {
		p->il2cpp_binary = strdup (abs);
		p->platform = strdup (is_dll? "windows": "linux");
	} else {
		char *dll = pjoin (dir, "GameAssembly.dll");
		char *so = pjoin (dir, "GameAssembly.so");
		if (take_if_exists (&p->il2cpp_binary, dll)) {
			p->platform = strdup ("windows");
			free (so);
		} else if (take_if_exists (&p->il2cpp_binary, so)) {
			p->platform = strdup ("linux");
		} else {
			p->platform = strdup (r_str_endswith (base, ".exe")? "windows": "linux");
		}
	}
	p->data_dir = data_dir;
	p->metadata = metadata;
	return true;
}

static bool try_android_apk(R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Extracted APK: <root>/lib/<abi>/libil2cpp.so
	 *                <root>/assets/bin/Data/Managed/Metadata/global-metadata.dat */
	if (!str_ieq (base, "libil2cpp.so")) {
		return false;
	}
	char *abi_parent = r_file_dirname (dir);
	if (!abi_parent) {
		return false;
	}
	char *root = r_file_dirname (abi_parent);
	free (abi_parent);
	static const char *const metas[] = {
		"assets/bin/Data/Managed/Metadata/global-metadata.dat",
		"assets/bin/Data/Managed/Metadata/global-metadata.dat.so",
		NULL
	};
	for (int i = 0; metas[i]; i++) {
		if (take_if_exists (&p->metadata, pjoin (root, metas[i]))) {
			p->platform = strdup ("android");
			p->il2cpp_binary = strdup (abs);
			p->data_dir = pjoin (root, "assets/bin/Data");
			free (root);
			return true;
		}
	}
	free (root);
	return false;
}

static bool try_fixture(R2UnityPaths *p, const char *abs, const char *dir, const char *base) {
	/* Flat fixture: main exe + il2cpp binary + global-metadata.dat all in the
	 * same directory. Used by users dumping files
	 * manually for quick triage. */
	char *metadata = pjoin (dir, "global-metadata.dat");
	if (!r_file_exists (metadata)) {
		free (metadata);
		return false;
	}
	p->platform = strdup ("fixture");
	p->metadata = metadata;
	p->data_dir = strdup (dir);
	p->il2cpp_binary = is_il2cpp_basename (base)? strdup (abs): find_il2cpp_sibling (dir);
	return true;
}

/* Drill into a macOS .app bundle's Contents/MacOS and return the first regular
 * file there; falls back to iOS framework/flat layouts. */
static char *expand_app_bundle(const char *dir) {
	char *macos_dir = pjoin (dir, "Contents/MacOS");
	if (r_file_is_directory (macos_dir)) {
		RList *entries = r_sys_dir (macos_dir);
		if (entries) {
			RListIter *it;
			char *name;
			r_list_foreach (entries, it, name) {
				if (*name == '.') {
					continue;
				}
				char *f = pjoin (macos_dir, name);
				if (r_file_is_regular (f)) {
					r_list_free (entries);
					free (macos_dir);
					return f;
				}
				free (f);
			}
			r_list_free (entries);
		}
	}
	free (macos_dir);
	char *ios_fw = r_file_new (dir, "Frameworks", "UnityFramework.framework", "UnityFramework", NULL);
	char *out = NULL;
	if (take_if_exists (&out, ios_fw)) {
		return out;
	}
	if (take_if_exists (&out, pjoin (dir, "UnityFramework"))) {
		return out;
	}
	return NULL;
}

/* Extracted iOS app root without a .app suffix: Data sits next to the main
 * Mach-O, whose basename is project-specific. */
static char *expand_ios_root_dir(const char *dir) {
	char *metadata = find_ios_metadata (dir);
	if (!metadata) {
		return NULL;
	}
	free (metadata);

	char *out = NULL;
	if (take_if_exists (&out, r_file_new (dir, "Frameworks", "UnityFramework.framework", "UnityFramework", NULL))) {
		return out;
	}
	if (take_if_exists (&out, pjoin (dir, "UnityFramework"))) {
		return out;
	}

	RList *entries = r_sys_dir (dir);
	if (!entries) {
		return NULL;
	}
	char *fallback = NULL;
	RListIter *it;
	char *name;
	r_list_foreach (entries, it, name) {
		if (*name == '.') {
			continue;
		}
		if (!strcmp (name, "Data") || !strcmp (name, "Info.plist") || !strcmp (name, "PkgInfo") || !strcmp (name, "global-metadata.dat") || !strcmp (name, "embedded.mobileprovision")) {
			continue;
		}
		char *f = pjoin (dir, name);
		if (r_file_is_regular (f)) {
			if (r_file_is_executable (f)) {
				r_list_free (entries);
				free (fallback);
				return f;
			}
			if (!fallback) {
				fallback = f;
				continue;
			}
		}
		free (f);
	}
	r_list_free (entries);
	return fallback;
}

/* Windows/Linux standalone: look for a *_Data sibling and pick its stem as the
 * main exe (or fall back to the IL2CPP binary). */
static char *expand_winlin_dir(const char *dir) {
	RList *entries = r_sys_dir (dir);
	if (!entries) {
		return NULL;
	}
	char *found = NULL;
	RListIter *it;
	char *name;
	r_list_foreach (entries, it, name) {
		size_t nl = strlen (name);
		if (nl <= 5 || strcmp (name + nl - 5, "_Data")) {
			continue;
		}
		char *stem = r_str_ndup (name, (int) (nl - 5));
		char *exe = r_str_newf ("%s/%s.exe", dir, stem);
		if (take_if_exists (&found, exe)) {
			free (stem);
			break;
		}
		if (take_if_exists (&found, pjoin (dir, stem))) {
			free (stem);
			break;
		}
		free (stem);
	}
	r_list_free (entries);
	return found;
}

/* Android extracted APK: lib/<abi>/libil2cpp.so */
static char *expand_apk_dir(const char *dir) {
	char *lib_dir = pjoin (dir, "lib");
	if (!r_file_is_directory (lib_dir)) {
		free (lib_dir);
		return NULL;
	}
	RList *abis = r_sys_dir (lib_dir);
	char *found = NULL;
	if (abis) {
		RListIter *it;
		char *name;
		r_list_foreach (abis, it, name) {
			if (*name == '.') {
				continue;
			}
			char *so = r_str_newf ("%s/%s/libil2cpp.so", lib_dir, name);
			if (take_if_exists (&found, so)) {
				break;
			}
		}
		r_list_free (abis);
	}
	free (lib_dir);
	return found;
}

/* When the caller hands us a directory, pick the most specific file inside it
 * that the per-platform detectors already understand. Probes ordered so that a
 * layout-bearing location wins over a flat fixture. Returns an absolute path or
 * NULL if nothing recognisable was found. */
static char *expand_dir_input(const char *dir) {
	if (r_str_endswith (dir, ".app")) {
		char *out = expand_app_bundle (dir);
		if (out) {
			return out;
		}
	}
	char *ios = expand_ios_root_dir (dir);
	if (ios) {
		return ios;
	}
	char *flat = find_il2cpp_sibling (dir);
	if (flat) {
		return flat;
	}
	char *winlin = expand_winlin_dir (dir);
	if (winlin) {
		return winlin;
	}
	char *apk = expand_apk_dir (dir);
	if (apk) {
		return apk;
	}
	char *out = NULL;
	take_if_exists (&out, pjoin (dir, "global-metadata.dat"));
	return out;
}

R_API R2UnityPaths *r2unity_detect_paths(const char *input) {
	R_RETURN_VAL_IF_FAIL (input, NULL);
	if (!*input) {
		return NULL;
	}
	char *abs = r_file_abspath (input);
	if (!abs) {
		return NULL;
	}
	if (r_file_is_directory (abs)) {
		char *expanded = expand_dir_input (abs);
		free (abs);
		if (!expanded) {
			return NULL;
		}
		abs = expanded;
	} else if (!r_file_exists (abs)) {
		free (abs);
		return NULL;
	}
	R2UnityPaths *p = R_NEW0 (R2UnityPaths);
	p->main_executable = abs;

	char *dir = r_file_dirname (abs);
	const char *base = r_file_basename (abs);

	bool ok = try_macos (p, abs, dir, base) || try_ios (p, abs, dir, base) || try_android_apk (p, abs, dir, base) || try_winlin_standalone (p, abs, dir, base) || try_fixture (p, abs, dir, base);

	free (dir);
	if (!ok) {
		r2unity_free_paths (p);
		return NULL;
	}
	return p;
}

R_API void r2unity_free_paths(R2UnityPaths *p) {
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

R_API const char *r2unity_platform_il2cpp_name(const char *platform) {
	R_RETURN_VAL_IF_FAIL (platform, NULL);
	return il2cpp_basename_for (platform);
}

// =================================================================================================
// blc
//
// File:   linker.c
// Author: Martin Dorazil
// Date:   09/02/2018
//
// Copyright 2017 Martin Dorazil
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// =================================================================================================

#include "builder.h"
#include "common.h"
#include "conf.h"
#include "stb_ds.h"

#define link_error(code, tok, pos, format, ...)                                             \
	{                                                                                       \
		if (tok)                                                                            \
			builder_msg(MSG_ERR, (code), &(tok)->location, (pos), (format), ##__VA_ARGS__); \
		else                                                                                \
			builder_error((format), ##__VA_ARGS__);                                         \
	}                                                                                       \
	(void)0

struct context {
	struct assembly *assembly;
};

static bool search_library(struct context *ctx,
                           str_t           lib_name,
                           str_t          *out_lib_name,
                           str_t          *out_lib_dir,
                           str_t          *out_lib_filepath) {
	bool found = false;

	str_buf_t lib_filepath      = get_tmp_str();
	str_buf_t lib_platform_name = platform_lib_name(lib_name);

	builder_log("- Looking for: '%.*s'", lib_platform_name.len, lib_platform_name.ptr);
	for (usize i = 0; i < arrlenu(ctx->assembly->lib_paths); ++i) {
		char *dir = ctx->assembly->lib_paths[i];
		builder_log("- Search in: '%s'", dir);

		str_buf_clr(&lib_filepath);
		str_buf_append_fmt(&lib_filepath, "{s}/{str}", dir, lib_platform_name);

		if (file_exists2(lib_filepath)) {
			builder_log("  Found: '%.*s'", lib_filepath.len, lib_filepath.ptr);

			if (out_lib_name) {
				(*out_lib_name) = scdup2(&ctx->assembly->string_cache, lib_platform_name);
			}
			if (out_lib_dir) {
				(*out_lib_dir) = scdup2(&ctx->assembly->string_cache, make_str_from_c(dir));
			}
			if (out_lib_filepath) {
				(*out_lib_filepath) = scdup2(&ctx->assembly->string_cache, lib_filepath);
			}
			found = true;
			goto DONE;
		}
	}

DONE:
	if (!found) builder_log("  Not found: '%.*s'", lib_filepath.len, lib_filepath.ptr);
	put_tmp_str(lib_platform_name);
	put_tmp_str(lib_filepath);

	return found;
}

static void set_lib_paths(struct context *ctx) {
	char        tmp[PATH_MAX] = {0};
	const char *lib_path =
	    read_config(builder.config, ctx->assembly->target, "linker_lib_path", "");
	if (!strlen(lib_path)) return;

	s64         len;
	const char *begin = lib_path;
	const char *c     = lib_path;
	bool        done  = false;

	while (!done) {
		done = *(c++) == '\0';
		if (done || *c == ENVPATH_SEPARATORC) {
			len = c - begin;
			if (len - 1 > 0) {
				strncpy(tmp, begin, len);
				tmp[len] = '\0';
				if (file_exists(tmp)) {
					char *dup = malloc(sizeof(char) * len + 1);
					if (!dup) babort("Bad alloc!");
					memcpy(dup, begin, len);
					dup[len] = '\0';

#if BL_PLATFORM_WIN
					win_path_to_unix(dup, len);
#endif

					arrput(ctx->assembly->lib_paths, dup);
				} else {
					builder_warning("Invalid LIB_PATH entry value '%s'.", tmp);
				}

				begin = c + 1;
			}
		}
	}
}

static bool link_lib(struct context *ctx, struct native_lib *lib) {
	if (!lib) babort("invalid lib");
	if (!lib->user_name.len) babort("invalid lib name");
	if (!search_library(ctx, lib->user_name, &lib->filename, &lib->dir, &lib->filepath)) {
		return false;
	}
	if (lib->runtime_only) {
		builder_log("- Library with 'runtime_only' flag '%.*s' skipped.",
		            lib->user_name.len,
		            lib->user_name.ptr);
		return true;
	}
	lib->handle = dlLoadLibrary(str_to_c(lib->filepath));
	return lib->handle;
}

static bool link_working_environment(struct context *ctx, const char *lib_name) {
	DLLib *handle = dlLoadLibrary(lib_name);
	if (!handle) return false;

	struct native_lib native_lib = {0};
	native_lib.handle            = handle;
	native_lib.is_internal       = true;

	arrput(ctx->assembly->libs, native_lib);
	return true;
}

void linker_run(struct assembly *assembly) {
	zone();
	struct context ctx;
	ctx.assembly = assembly;
	builder_log("Running compile-time linker...");
	set_lib_paths(&ctx);

	for (usize i = 0; i < arrlenu(assembly->libs); ++i) {
		struct native_lib *lib = &assembly->libs[i];
		if (!link_lib(&ctx, lib)) {
			char      error_buffer[256];
			const s32 error_len = get_last_error(error_buffer, static_arrlenu(error_buffer));
			link_error(ERR_LIB_NOT_FOUND,
			           lib->linked_from,
			           CARET_WORD,
			           "Cannot load library '%.*s' with error: %s",
			           lib->user_name.len,
			           lib->user_name.ptr,
			           error_len ? error_buffer : "UNKNOWN");
		}
	}

#if BL_PLATFORM_WIN
	if (!link_working_environment(&ctx, MSVC_CRT)) {
		struct token *dummy = NULL;
		link_error(ERR_LIB_NOT_FOUND, dummy, CARET_WORD, "Cannot link " MSVC_CRT);
		return_zone();
	}
	if (!link_working_environment(&ctx, KERNEL32)) {
		struct token *dummy = NULL;
		link_error(ERR_LIB_NOT_FOUND, dummy, CARET_WORD, "Cannot link " KERNEL32);
		return_zone();
	}
	if (!link_working_environment(&ctx, SHLWAPI)) {
		struct token *dummy = NULL;
		link_error(ERR_LIB_NOT_FOUND, dummy, CARET_WORD, "Cannot link " SHLWAPI);
		return_zone();
	}
#endif
	if (!link_working_environment(&ctx, NULL)) {
		struct token *dummy = NULL;
		link_error(ERR_LIB_NOT_FOUND, dummy, CARET_WORD, "Cannot link working environment.");
		return_zone();
	}
	return_zone();
}

/*
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Resolution of the stack traces that native programs built without debug
 * information print, with the map file written when they were built. It runs
 * on the build host, as part of libucode, not in native programs. */

#include <stdlib.h>
#include <string.h>

#include "ucode/internal/native.h"
#include "ucode/internal/program.h"
#include "ucode/internal/source.h"
#include "ucode/internal/lib.h"

/* Reads the program of the unit that stack traces name `path` from the map
 * file `file`. After its magic line, the map holds for every unit a line with
 * its path, a line with the length of its program and the program itself,
 * written with debug information, see uc_program_write_llvm(). A unit loaded
 * by a relative path is found by its normalised path. */
static uc_program_t *
uc_native_map_program(const char *file, const char *path, char **errp)
{
	char *line = NULL, *buf = NULL, *key = uc_path_normalize(path);
	uc_program_t *program = NULL;
	size_t cap = 0, len = 0;
	uc_source_t *input;
	bool found = false;
	ssize_t n;
	FILE *fp;

	fp = fopen(file, "rb");

	if (!fp) {
		xasprintf(errp, "Unable to open map file '%s': %m\n", file);
		free(key);

		return NULL;
	}

	if (getline(&line, &cap, fp) < 0 || strcmp(line, UC_NATIVE_MAP_MAGIC)) {
		xasprintf(errp, "Unable to load map file '%s': not a map file\n", file);
		goto out;
	}

	while ((n = getline(&line, &cap, fp)) > 0) {
		if (line[n - 1] == '\n')
			line[n - 1] = 0;

		found = !strcmp(line, path) || (key && !strcmp(line, key));

		if (getline(&line, &cap, fp) <= 0) {
			found = false;
			break;
		}

		len = strtoul(line, NULL, 10);

		if (found)
			break;

		if (fseek(fp, len, SEEK_CUR))
			break;
	}

	if (!found) {
		xasprintf(errp, "Map file '%s' has no program for '%s'\n", file, path);
		goto out;
	}

	buf = xalloc(len ? len : 1);

	if (fread(buf, 1, len, fp) != len) {
		xasprintf(errp, "Unable to load map file '%s': truncated\n", file);
		free(buf);
		goto out;
	}

	input = uc_source_new_buffer(path, buf, len);

	if (!input) {
		free(buf);
		goto out;
	}

	program = uc_program_load(input, errp);
	uc_source_put(input);

	for (size_t i = 0; program && i < program->sources.count; i++)
		uc_source_runpath_set(program->sources.entries[i],
		                      program->sources.entries[i]->filename);

out:
	free(key);
	free(line);
	fclose(fp);

	return program;
}

typedef struct {
	char *path;
	uc_program_t *program;
} uc_native_trace_map_t;

uc_declare_vector(uc_native_trace_maps_t, uc_native_trace_map_t);

static uc_program_t *
uc_native_trace_program(uc_native_trace_maps_t *maps, const char *file, const char *path)
{
	char *err = NULL;

	for (size_t i = 0; i < maps->count; i++)
		if (!strcmp(maps->entries[i].path, path))
			return maps->entries[i].program;

	uc_vector_push(maps, {
		.path = xstrdup(path),
		.program = uc_native_map_program(file, path, &err)
	});

	if (err)
		fprintf(stderr, "%s", err);

	free(err);

	return maps->entries[maps->count - 1].program;
}

typedef struct {
	uc_native_trace_maps_t maps;
	const char *file;
	uc_value_t *stacktrace;
	uc_stringbuf_t *raw;
	uc_program_t *program;
	size_t id, pc;
	bool unresolved;
} uc_native_trace_t;

/* Fills `entry` from a frame of code without debug information, given by its
 * function number and instruction offset in the program loaded as `path`. */
static bool
uc_native_trace_resolve(uc_native_trace_t *t, uc_value_t *entry, const char *path,
                        size_t id, size_t pc)
{
	uc_program_t *program = uc_native_trace_program(&t->maps, t->file, path);
	uc_function_t *fn = program ? uc_program_function_load(program, id) : NULL;
	uc_source_t *source;
	size_t pos;

	if (!fn)
		return false;

	source = uc_program_function_source(fn);
	pos = uc_program_function_srcpos(fn, pc);

	ucv_object_add(entry, "filename", ucv_string_new(source->filename));
	ucv_object_add(entry, "line", ucv_int64_new(uc_source_get_line(source, &pos)));
	ucv_object_add(entry, "byte", ucv_int64_new(pos));
	ucv_object_add(entry, "function",
		ucv_string_new(fn->name[0] ? fn->name : "[anonymous function]"));

	if (!ucv_array_length(t->stacktrace)) {
		t->program = program;
		t->id = id;
		t->pc = pc;
	}

	return true;
}

/* Adds the frame that `line` describes, in the form that the VM prints for
 * code without debug information, to the stack trace collected so far.
 * Returns false if `line` is not such a frame. */
static bool
uc_native_trace_frame(uc_native_trace_t *t, const char *line)
{
	size_t n = ucv_array_length(t->stacktrace), id, pc, lnum, byte;
	char path[4096];
	const char *p, *paren, *end;
	uc_value_t *entry;

	if (n && sscanf(line, "  (%zu tail call frames omitted)", &pc) == 1) {
		ucv_object_add(ucv_array_get(t->stacktrace, n - 1), "tco", ucv_int64_new(pc));

		return true;
	}

	if (!n && !strncmp(line, "In function ", 12))
		p = line + 12;
	else if (n && !strncmp(line, "  called from function ", 23))
		p = line + 23;
	else
		return false;

	entry = ucv_object_new(NULL);
	paren = strrchr(p, '(');
	end = strstr(p, " [C]");

	if (sscanf(p, "#%zu at offset %zu of %4095[^\n]", &id, &pc, path) == 3) {
		if (!uc_native_trace_resolve(t, entry, path, id, pc))
			t->unresolved = true;
	}
	else if (paren && paren > p &&
	         sscanf(paren, "(%4095[^:]:%zu:%zu)", path, &lnum, &byte) == 3) {
		ucv_object_add(entry, "filename", ucv_string_new(path));
		ucv_object_add(entry, "line", ucv_int64_new(lnum));
		ucv_object_add(entry, "byte", ucv_int64_new(byte));
		ucv_object_add(entry, "function", ucv_string_new_length(p, paren - p - 1));

		/* the context of the innermost frame needs its source, which the map
		 * does not hold for code of other programs */
		if (!n)
			t->unresolved = true;
	}
	else if (end) {
		ucv_object_add(entry, "function", ucv_string_new_length(p, end - p));

		if (!n)
			t->unresolved = true;
	}
	else {
		ucv_put(entry);

		return false;
	}

	ucv_array_push(t->stacktrace, entry);
	ucv_stringbuf_addstr(t->raw, line, strlen(line));

	return true;
}

/* Writes the collected stack trace in the form that a program with debug
 * information prints, or unchanged if it cannot be resolved. */
static void
uc_native_trace_flush(uc_native_trace_t *t, FILE *out)
{
	size_t n = ucv_array_length(t->stacktrace), startoff = 0, endoff = 0, srcpos;
	uc_stringbuf_t *buf;
	uc_function_t *fn;

	if (!n)
		return;

	fn = (t->program && !t->unresolved) ? uc_program_function_load(t->program, t->id) : NULL;

	if (fn) {
		/* the outermost frame is the program itself, which has no name */
		ucv_object_delete(ucv_array_get(t->stacktrace, n - 1), "function");

		srcpos = uc_program_function_srcpos(fn, t->pc);
		uc_program_function_stmt_bounds(fn, t->pc, &startoff, &endoff);

		buf = uc_stringbuf_new();
		uc_error_context_format(buf, uc_program_function_source(fn), t->stacktrace,
		                        startoff, endoff, srcpos);
		fwrite(buf->buf, 1, uc_stringbuf_length(buf), out);
		uc_stringbuf_free(buf);
	}
	else {
		fwrite(t->raw->buf, 1, uc_stringbuf_length(t->raw), out);
	}

	ucv_array_delete(t->stacktrace, 0, n);
	uc_stringbuf_reset(t->raw);
	t->program = NULL;
	t->unresolved = false;
}

int
uc_native_trace_decode(const char *file, FILE *in, FILE *out)
{
	uc_native_trace_t t = {
		.file = file,
		.stacktrace = ucv_array_new(NULL),
		.raw = uc_stringbuf_new()
	};
	size_t cap = 0;
	char *line = NULL;
	ssize_t n;

	while ((n = getline(&line, &cap, in)) > 0) {
		if (uc_native_trace_frame(&t, line))
			continue;

		uc_native_trace_flush(&t, out);
		fwrite(line, 1, n, out);
	}

	uc_native_trace_flush(&t, out);

	for (size_t i = 0; i < t.maps.count; i++) {
		free(t.maps.entries[i].path);

		if (t.maps.entries[i].program)
			uc_program_put(t.maps.entries[i].program);
	}

	uc_vector_clear(&t.maps);
	ucv_put(t.stacktrace);
	uc_stringbuf_free(t.raw);
	free(line);

	return 0;
}

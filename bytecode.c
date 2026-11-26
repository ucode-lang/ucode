/*
 * Copyright (C) 2022 Jo-Philipp Wich <jo@mein.io>
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

/* Reading and writing of programs as bytecode. A runtime without bytecode
 * leaves this file out. */

#include <assert.h>
#include <errno.h>
#include <limits.h>

#include "ucode/internal/program.h"
#include "ucode/internal/source.h"
#include "ucode/internal/chunk.h"
#include "ucode/internal/platform.h"


static void
write_u16(size_t value, FILE *file)
{
	uint16_t n;

	if (sizeof(value) > sizeof(n))
		assert(value <= UINT16_MAX);

	n = htobe16((uint16_t)value);

	fwrite(&n, 1, sizeof(n), file);
}

static void
write_u32(size_t value, FILE *file)
{
	uint32_t n;

	if (sizeof(value) > sizeof(n))
		assert(value <= UINT32_MAX);

	n = htobe32((uint32_t)value);

	fwrite(&n, 1, sizeof(n), file);
}

static void
write_u64(uint64_t value, FILE *file)
{
	uint64_t n;

	if (sizeof(value) > sizeof(n))
		assert(value <= UINT64_MAX);

	n = htobe64((uint64_t)value);

	fwrite(&n, 1, sizeof(n), file);
}

static void
_write_vector(size_t count, size_t itemsize, void *data, FILE *file)
{
	size_t pad = (~(count * itemsize) + 1) & (sizeof(uint32_t) - 1);
	char z[sizeof(uint32_t) - 1] = { 0 };

	write_u32(count, file);
	fwrite(data, itemsize, count, file);
	fwrite(z, 1, pad, file);
}

#define write_vector(vec, file) \
	_write_vector((vec)->count, sizeof((vec)->entries[0]), (vec)->entries, file)

#define write_string(str, file) \
	_write_vector(strlen(str) + 1, 1, str, file)

static void
write_vallist(uc_value_list_t *vallist, FILE *file)
{
	size_t i;

	/* write index */
	write_u32(vallist->isize, file);

	for (i = 0; i < vallist->isize; i++)
		write_u64(vallist->index[i], file);

	/* write data */
	write_u32(vallist->dsize, file);

	if (vallist->dsize)
		fwrite(vallist->data, 1, vallist->dsize, file);
}

enum {
	UC_PROGRAM_F_DEBUG      = (1 << 0),
	UC_PROGRAM_F_SOURCEINFO = (1 << 1),
	UC_PROGRAM_F_SOURCEBUF  = (1 << 2),
	UC_PROGRAM_F_EXPORTS    = (1 << 3),
};

enum {
	UC_FUNCTION_F_IS_ARROW       = (1 << 0),
	UC_FUNCTION_F_IS_VARARG      = (1 << 1),
	UC_FUNCTION_F_IS_STRICT      = (1 << 2),
	UC_FUNCTION_F_HAS_EXCEPTIONS = (1 << 3),
	UC_FUNCTION_F_HAS_NAME       = (1 << 4),
	UC_FUNCTION_F_HAS_VARDBG     = (1 << 5),
	UC_FUNCTION_F_HAS_OFFSETDBG  = (1 << 6),
	UC_FUNCTION_F_IS_MODULE      = (1 << 7),
};

static void
write_chunk(uc_chunk_t *chunk, FILE *file, uint32_t flags)
{
	size_t i, slot;

	/* write bytecode data */
	write_vector(chunk, file);

	/* write exception ranges */
	if (flags & UC_FUNCTION_F_HAS_EXCEPTIONS) {
		write_u32(chunk->ehranges.count, file);

		for (i = 0; i < chunk->ehranges.count; i++) {
			write_u32(chunk->ehranges.entries[i].from,   file);
			write_u32(chunk->ehranges.entries[i].to,     file);
			write_u32(chunk->ehranges.entries[i].target, file);
			write_u32(chunk->ehranges.entries[i].slot,   file);
		}
	}

	/* write variable info */
	if (flags & UC_FUNCTION_F_HAS_VARDBG) {
		write_u32(chunk->debuginfo.variables.count, file);

		for (i = 0; i < chunk->debuginfo.variables.count; i++) {
			slot = chunk->debuginfo.variables.entries[i].slot;

			if (slot >= ((size_t)-1 / 2))
				slot = ((uint32_t)-1 / 2) + (slot - ((size_t)-1 / 2));

			write_u32(chunk->debuginfo.variables.entries[i].from,    file);
			write_u32(chunk->debuginfo.variables.entries[i].to,      file);
			write_u32(slot,                                          file);
			write_u32(chunk->debuginfo.variables.entries[i].nameidx, file);
		}

		write_vallist(&chunk->debuginfo.varnames, file);
	}

	/* write offset info */
	if (flags & UC_FUNCTION_F_HAS_OFFSETDBG)
		write_vector(&chunk->debuginfo.offsets, file);
}

static void
write_function(uc_function_t *func, FILE *file, bool debug)
{
	uint32_t flags = 0;

	if (func->arrow)
		flags |= UC_FUNCTION_F_IS_ARROW;

	if (func->vararg)
		flags |= UC_FUNCTION_F_IS_VARARG;

	if (func->strict)
		flags |= UC_FUNCTION_F_IS_STRICT;

	if (func->module)
		flags |= UC_FUNCTION_F_IS_MODULE;

	if (func->chunk.ehranges.count)
		flags |= UC_FUNCTION_F_HAS_EXCEPTIONS;

	if (debug && func->name[0])
		flags |= UC_FUNCTION_F_HAS_NAME;

	if (debug && func->chunk.debuginfo.variables.count)
		flags |= UC_FUNCTION_F_HAS_VARDBG;

	if (debug && func->chunk.debuginfo.offsets.count)
		flags |= UC_FUNCTION_F_HAS_OFFSETDBG;

	write_u32(flags, file);

	if (flags & UC_FUNCTION_F_HAS_NAME)
		write_string(func->name, file);

	write_u16(func->nargs, file);
	write_u16(func->nupvals, file);
	write_u32(func->srcidx, file);
	write_u32(func->srcpos, file);

	write_chunk(&func->chunk, file, flags);
}

static bool
write_exports(FILE *file, uc_source_t *source, uint32_t flags, const char *subj)
{
	size_t i, num_exports = source->exports.count - 1;
	uc_value_t *sym;

	if (flags & UC_PROGRAM_F_EXPORTS)
	{
		/* write export count */
		write_u32(num_exports, file);

		/* write each export name */
		for (i = 1; i <= num_exports; i++)
		{
			sym = source->exports.entries[i];

			if (ucv_type(sym) == UC_STRING)
				_write_vector(ucv_string_length(sym) + 1, 1,
				              ucv_string_get(sym), file);
			else if (ucv_type(sym) == UC_NULL)
				write_u32(0xffffffff, file);
			else
				assert(0);
		}
	}

	return true;
}

void
uc_program_write(uc_program_t *prog, FILE *file, bool debug)
{
	/* the version this program requires, the oldest one which can run it unless
	 * the compiler recorded a newer one, see uc_program_require_version() */
	uint32_t flags = (prog->version & 0xff) << 24;
	size_t i = 0;

	if (debug)
		flags |= UC_PROGRAM_F_DEBUG;

	if (debug && prog->sources.count)
		flags |= UC_PROGRAM_F_SOURCEINFO;

	if (prog->sources.count && prog->sources.entries[0]->exports.count)
		flags |= UC_PROGRAM_F_EXPORTS;

	/* magic word + flags */
	write_u32(UC_PRECOMPILED_BYTECODE_MAGIC, file);
	write_u32(flags, file);

	/* write source information */
	if (flags & UC_PROGRAM_F_SOURCEINFO) {
		write_u32(prog->sources.count, file);

		for (i = 0; i < prog->sources.count; i++) {
			/* write source file name */
			write_string(prog->sources.entries[i]->filename, file);

			/* include source buffer if program was compiled from stdin */
			if (prog->sources.entries[i]->buffer)
				write_string(prog->sources.entries[i]->buffer, file);
			else
				//write_string("", file);
				write_u32(0, file);

			/* write lineinfo data */
			write_vector(&prog->sources.entries[i]->lineinfo, file);
		}
	}

	/* write constants */
	write_vallist(&prog->constants, file);

	/* write export symbols */
	write_exports(file, prog->sources.entries[0], flags, "exports");

	/* write program sections */
	i = 0;
	uc_program_function_foreach(prog, fn1) {
		(void)fn1;
		i++;
	}

	write_u32(i, file);

	uc_program_function_foreach(prog, fn2)
		write_function(fn2, file, debug);
}

static bool
read_error(uc_source_t *in, char **errp, const char *subject, size_t rlen, size_t len)
{
	const char *reason;

	if (!errno)
		reason = "Premature EOF";
	else
		reason = strerror(errno);

	if (errp)
		xasprintf(errp,
		          "%s while reading %s at offset %zu, got %zu of %zu bytes\n",
		          reason, subject, uc_source_offset(in) - rlen, rlen, len);

	return false;
}

static bool
skip_padding(uc_source_t *in, size_t len, char **errp)
{
	size_t pad = (~len + 1) & (sizeof(uint32_t) - 1), rlen;
	char buf[sizeof(uint32_t) - 1];

	if (pad != 0) {
		rlen = uc_source_read(in, buf, pad);

		if (rlen != pad)
			return read_error(in, errp, "padding", rlen, pad);
	}

	return true;
}

static bool
read_u32(uc_source_t *in, uint32_t *n, const char *subj, char **errp)
{
	size_t rlen = uc_source_read(in, n, sizeof(*n));

	if (rlen != sizeof(*n)) {
		*n = 0;

		return read_error(in, errp, subj ? subj : "uint32_t", rlen, sizeof(*n));
	}

	*n = be32toh(*n);

	return true;
}

static bool
read_u64(uc_source_t *in, uint64_t *n, const char *subj, char **errp)
{
	size_t rlen = uc_source_read(in, n, sizeof(*n));

	if (rlen != sizeof(*n)) {
		*n = 0;

		return read_error(in, errp, subj ? subj : "uint64_t", rlen, sizeof(*n));
	}

	*n = be64toh(*n);

	return true;
}

static bool
read_size_t(uc_source_t *in, size_t *n, size_t size, const char *subj, char **errp);

/* A count or length taken from the file sizes an allocation, and every item
 * it counts is then read from that same file, so a value larger than the
 * bytes remaining cannot be honest. Refusing it here keeps a crafted 32-bit
 * field from reaching xalloc(), which aborts the process rather than
 * returning. */
static bool
length_fits(uc_source_t *in, size_t n, const char *subj, char **errp)
{
	if (n > uc_source_remaining(in)) {
		if (errp)
			xasprintf(errp, "Invalid length %zu for %s at offset %zu\n",
			          n, subj, uc_source_offset(in));

		return false;
	}

	return true;
}

static bool
read_count(uc_source_t *in, size_t *n, size_t itemsize, const char *subj, char **errp)
{
	if (!read_size_t(in, n, sizeof(uint32_t), subj, errp))
		return false;

	if (*n > uc_source_remaining(in) / (itemsize ? itemsize : 1)) {
		if (errp)
			xasprintf(errp, "Invalid count %zu for %s at offset %zu\n",
			          *n, subj, uc_source_offset(in));

		return false;
	}

	return true;
}

#define read_length(in, n, subj, errp) read_count(in, n, 1, subj, errp)

static bool
read_size_t(uc_source_t *in, size_t *n, size_t size, const char *subj, char **errp)
{
	union { uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64; } nval;
	size_t rlen;

	rlen = uc_source_read(in, &nval.u64, size);

	if (rlen != size) {
		*n = 0;

		if (!subj) {
			switch (size) {
			case 1: subj = "uint8_t";  break;
			case 2: subj = "uint16_t"; break;
			case 4: subj = "uint32_t"; break;
			case 8: subj = "uint64_t"; break;
			}
		}

		return read_error(in, errp, subj, rlen, sizeof(nval));
	}

	switch (size) {
	case 1: *n = (size_t)        nval.u8;   break;
	case 2: *n = (size_t)be16toh(nval.u16); break;
	case 4: *n = (size_t)be32toh(nval.u32); break;
	case 8: *n = (size_t)be64toh(nval.u64); break;
	}

	return true;
}

static bool
_read_vector(uc_source_t *in, void *ptr, size_t itemsize, const char *subj, char **errp)
{
	struct { size_t count; void *data; } *vec = ptr;
	size_t rlen, len;
	char subjbuf[64];

	snprintf(subjbuf, sizeof(subjbuf), "%s vector size", subj);

	if (!read_count(in, &vec->count, itemsize, subjbuf, errp))
		return false;

	vec->data = xcalloc(vec->count, itemsize);

	len = vec->count;
	rlen = uc_source_read(in, vec->data, len * itemsize);

	if (rlen != len * itemsize) {
		free(vec->data);

		vec->count = 0;
		vec->data = NULL;

		snprintf(subjbuf, sizeof(subjbuf), "%s vector data", subj);

		return read_error(in, errp, subjbuf, rlen, len * itemsize);
	}

	return skip_padding(in, vec->count * itemsize, errp);
}

#define read_vector(in, vec, subj, errp) \
	_read_vector(in, vec, sizeof((vec)->entries[0]), subj, errp)

static bool
read_string(uc_source_t *in, char *dst, size_t len, const char *subj, char **errp)
{
	size_t rlen;

	rlen = uc_source_read(in, dst, len);

	if (rlen != len)
		return read_error(in, errp, subj, rlen, len);

	return skip_padding(in, len, errp);
}

static bool
read_vallist(uc_source_t *in, uc_value_list_t *vallist, const char *subj, char **errp)
{
	char subjbuf[64];
	size_t i;

	/* read index */
	snprintf(subjbuf, sizeof(subjbuf), "%s index size", subj);

	if (!read_count(in, &vallist->isize, sizeof(uint64_t), subjbuf, errp))
		goto out;

	vallist->index = xcalloc(sizeof(vallist->index[0]), vallist->isize);

	for (i = 0; i < vallist->isize; i++) {
		snprintf(subjbuf, sizeof(subjbuf), "%s index entry %zu of %zu", subj, i, vallist->isize);

		if (!read_u64(in, &vallist->index[i], subjbuf, errp))
			goto out;
	}

	/* read data */
	snprintf(subjbuf, sizeof(subjbuf), "%s data size", subj);

	if (!read_length(in, &vallist->dsize, subjbuf, errp))
		goto out;

	vallist->data = xalloc(vallist->dsize);

	snprintf(subjbuf, sizeof(subjbuf), "%s data", subj);

	if (!read_string(in, vallist->data, vallist->dsize, subj, errp))
		goto out;

	return true;

out:
	free(vallist->index);
	free(vallist->data);

	vallist->isize = 0;
	vallist->index = NULL;

	vallist->dsize = 0;
	vallist->data = NULL;

	return false;
}

static bool
read_exports(uc_source_t *in, uc_source_t *source, uint32_t flags, const char *subj, char **errp)
{
	size_t i, num_exports, len;
	uc_stringbuf_t *buf;
	uc_value_t *symname;
	char subjbuf[64];

	if (flags & UC_PROGRAM_F_EXPORTS)
	{
		/* read export count */
		snprintf(subjbuf, sizeof(subjbuf), "%s count", subj);

		if (!read_count(in, &num_exports, sizeof(uint32_t), subjbuf, errp))
			return false;

		/* read export symbol names */
		for (i = 0; i < num_exports; i++)
		{
			snprintf(subjbuf, sizeof(subjbuf), "%s entry %zu of %zu size",
					 subj, i, num_exports);

			if (!read_size_t(in, &len, sizeof(uint32_t), subjbuf, errp))
				return false;

			if (len < 0xffffffff) {
				snprintf(subjbuf, sizeof(subjbuf), "%s entry %zu of %zu name",
						subj, i, num_exports);

				if (!length_fits(in, len, subjbuf, errp))
					return false;

				buf = ucv_stringbuf_new();

				if (uc_stringbuf_memset(buf, -1, 0, len + 1)) {
					uc_stringbuf_free(buf);

					if (errp)
						xasprintf(errp, "Out of memory reading %s\n", subjbuf);

					return false;
				}

				if (!read_string(in, buf->buf + buf->bpos - len - 1, len, subjbuf, errp))
				{
					uc_stringbuf_free(buf);
					return false;
				}

				symname = ucv_stringbuf_finish(buf);
			}
			else {
				symname = NULL;
			}

			uc_source_export_add(source, symname);
			ucv_put(symname);
		}
	}

	return true;
}

static uc_source_t *
read_sourceinfo(uc_source_t *input, uint32_t flags, char **errp, uc_program_t *program)
{
	char *path = NULL, *code = NULL;
	uc_source_t *source = NULL;
	size_t len, count;

	if (flags & UC_PROGRAM_F_SOURCEINFO) {
		if (!read_count(input, &count, sizeof(uint32_t) * 2,
		                "amount of source entries", errp))
			return NULL;

		while (count > 0) {
			if (!read_length(input, &len, "sourceinfo filename length", errp))
				return NULL;

			path = xalloc(len + 1);

			if (!read_string(input, path, len, "sourceinfo filename", errp)) {
				free(path);

				return NULL;
			}

			if (!read_length(input, &len, "sourceinfo code buffer length", errp)) {
				free(path);

				return NULL;
			}

			if (len > 0) {
				code = xalloc(len);

				if (!read_string(input, code, len, "sourceinfo code buffer data", errp)) {
					free(code);
					free(path);

					return NULL;
				}

				source = uc_source_new_buffer(path, code, len);
			}
			else {
				source = uc_source_new_path(path);
			}

			if (!read_vector(input, &source->lineinfo, "sourceinfo lineinfo", errp)) {
				uc_source_put(source);
				free(path);

				return NULL;
			}

			uc_source_runpath_set(source, input->runpath);
			uc_vector_push(&program->sources, source);

			free(path);

			count--;
		}
	}
	else {
		source = uc_source_new_buffer("[no source]", xstrdup(""), 0);

		uc_source_runpath_set(source, input->runpath);
		uc_vector_push(&program->sources, source);
	}

	return source;
}

static bool
read_chunk(uc_source_t *in, uc_chunk_t *chunk, uint32_t flags, const char *subj, char **errp)
{
	uc_varrange_t *varrange;
	uc_ehrange_t *ehrange;
	char subjbuf[192];
	size_t i;

	/* read bytecode data */
	snprintf(subjbuf, sizeof(subjbuf), "%s byte code", subj);

	if (!read_vector(in, chunk, subjbuf, errp))
		goto out;

	/* read exception ranges */
	if (flags & UC_FUNCTION_F_HAS_EXCEPTIONS) {
		snprintf(subjbuf, sizeof(subjbuf), "%s exception ranges count", subj);

		if (!read_count(in, &chunk->ehranges.count,
		                sizeof(uint32_t) * 4, subjbuf, errp))
			goto out;

		chunk->ehranges.entries = xcalloc(
			sizeof(chunk->ehranges.entries[0]),
			chunk->ehranges.count);

		for (i = 0; i < chunk->ehranges.count; i++) {
			snprintf(subjbuf, sizeof(subjbuf), "%s exception range %zu of %zu offset",
				subj, i, chunk->ehranges.count);

			ehrange = &chunk->ehranges.entries[i];

			if (!read_size_t(in, &ehrange->from,   sizeof(uint32_t), subjbuf, errp) ||
			    !read_size_t(in, &ehrange->to,     sizeof(uint32_t), subjbuf, errp) ||
			    !read_size_t(in, &ehrange->target, sizeof(uint32_t), subjbuf, errp) ||
			    !read_size_t(in, &ehrange->slot,   sizeof(uint32_t), subjbuf, errp))
				goto out;
		}
	}

	/* read variable info */
	if (flags & UC_FUNCTION_F_HAS_VARDBG) {
		snprintf(subjbuf, sizeof(subjbuf), "%s variable scopes count", subj);

		if (!read_count(in, &chunk->debuginfo.variables.count,
		                sizeof(uint32_t) * 4, subjbuf, errp))
			goto out;

		chunk->debuginfo.variables.entries = xcalloc(
			sizeof(chunk->debuginfo.variables.entries[0]),
			chunk->debuginfo.variables.count);

		for (i = 0; i < chunk->debuginfo.variables.count; i++) {
			snprintf(subjbuf, sizeof(subjbuf), "%s variable scope %zu of %zu offset",
				subj, i, chunk->debuginfo.variables.count);

			varrange = &chunk->debuginfo.variables.entries[i];

			if (!read_size_t(in, &varrange->from,    sizeof(uint32_t), subjbuf, errp) ||
			    !read_size_t(in, &varrange->to,      sizeof(uint32_t), subjbuf, errp) ||
			    !read_size_t(in, &varrange->slot,    sizeof(uint32_t), subjbuf, errp) ||
			    !read_size_t(in, &varrange->nameidx, sizeof(uint32_t), subjbuf, errp))
			    goto out;

			if (varrange->slot >= ((uint32_t)-1 / 2))
				varrange->slot = ((size_t)-1 / 2) + (varrange->slot - ((uint32_t)-1 / 2));
		}

		snprintf(subjbuf, sizeof(subjbuf), "%s variable names", subj);

		if (!read_vallist(in, &chunk->debuginfo.varnames, subjbuf, errp))
			goto out;
	}

	/* read offset info */
	if (flags & UC_FUNCTION_F_HAS_OFFSETDBG) {
		snprintf(subjbuf, sizeof(subjbuf), "%s source offsets", subj);

		if (!read_vector(in, &chunk->debuginfo.offsets, subjbuf, errp))
			goto out;
	}

	return true;

out:
	uc_vallist_free(&chunk->debuginfo.varnames);

	free(chunk->entries);
	free(chunk->ehranges.entries);
	free(chunk->debuginfo.variables.entries);

	chunk->count = 0;
	chunk->entries = NULL;

	chunk->ehranges.count = 0;
	chunk->ehranges.entries = NULL;

	chunk->debuginfo.variables.count = 0;
	chunk->debuginfo.variables.entries = NULL;

	return false;
}

static bool
read_function(uc_source_t *in, uc_program_t *program, size_t idx, char **errp)
{
	size_t nargs, nupvals, srcidx, srcpos, namelen = 0;
	char subjbuf[64], *name = NULL;
	uc_function_t *func = NULL;
	uc_source_t *source;
	uint32_t flags;

	snprintf(subjbuf, sizeof(subjbuf), "function #%zu flags", idx);

	if (!read_u32(in, &flags, subjbuf, errp))
		goto out;

	if (flags & UC_FUNCTION_F_HAS_NAME) {
		snprintf(subjbuf, sizeof(subjbuf), "function #%zu name length", idx);

		if (!read_length(in, &namelen, subjbuf, errp))
			goto out;

		name = xalloc(namelen + 1);

		snprintf(subjbuf, sizeof(subjbuf), "function #%zu name", idx);

		if (!read_string(in, name, namelen, subjbuf, errp))
			goto out;
	}

	snprintf(subjbuf, sizeof(subjbuf), "function #%zu (%s) arg count and offset", idx, name ? name : "-");

	if (!read_size_t(in, &nargs,   sizeof(uint16_t), subjbuf, errp) ||
	    !read_size_t(in, &nupvals, sizeof(uint16_t), subjbuf, errp) ||
	    !read_size_t(in, &srcidx,  sizeof(uint32_t), subjbuf, errp) ||
	    !read_size_t(in, &srcpos,  sizeof(uint32_t), subjbuf, errp)) {
		goto out;
	}

	// FIXME
	if (srcidx < program->sources.count)
		source = program->sources.entries[srcidx];
	else
		source = program->sources.entries[0];

	func = (uc_function_t *)uc_program_function_new(program, name, source, srcpos);
	func->arrow   = (flags & UC_FUNCTION_F_IS_ARROW);
	func->vararg  = (flags & UC_FUNCTION_F_IS_VARARG);
	func->strict  = (flags & UC_FUNCTION_F_IS_STRICT);
	func->module  = (flags & UC_FUNCTION_F_IS_MODULE);
	func->nargs   = nargs;
	func->nupvals = nupvals;

	snprintf(subjbuf, sizeof(subjbuf), "function #%zu (%s) body", idx, name ? name : "-");

	if (!read_chunk(in, &func->chunk, flags, subjbuf, errp))
		goto out;

	free(name);

	return true;

out:
	free(name);

	return false;
}

uc_program_t *
uc_program_load(uc_source_t *input, char **errp)
{
	uc_program_t *program = NULL;
	uint32_t flags, i;
	size_t nfuncs;

	if (!read_u32(input, &i, "file magic", errp))
		goto out;

	if (i != UC_PRECOMPILED_BYTECODE_MAGIC) {
		xasprintf(errp, "Invalid file magic\n");
		goto out;
	}

	if (!read_u32(input, &flags, "program flags", errp))
		goto out;

	if (!uc_bytecode_version_supported(flags >> 24)) {
		xasprintf(errp,
			"Bytecode version mismatch, got 0x%02x, expected 0x%02x to 0x%02x\n",
			flags >> 24, UCODE_BYTECODE_VERSION_MIN, UCODE_BYTECODE_VERSION);
		goto out;
	}

	program = uc_program_new();

	/* keep the version the bytecode came with, so that writing it out again
	 * does not raise it beyond what the program actually uses */
	uc_program_require_version(program, flags >> 24);

	if (!read_sourceinfo(input, flags, errp, program))
		goto out;

	if (!read_vallist(input, &program->constants, "constants", errp))
		goto out;

	if (!read_exports(input, program->sources.entries[0], flags,
					  "exports", errp))
		goto out;

	if (!read_count(input, &nfuncs, sizeof(uint32_t), "function count", errp))
		goto out;

	for (i = 0; i < nfuncs; i++)
		if (!read_function(input, program, i, errp))
			goto out;

	return program;

out:
	uc_program_put(program);

	return NULL;
}

uc_program_t *
uc_program_load_entry(uc_source_t *source, char **errp)
{
	uc_program_t *prog;

	prog = uc_program_load(source, errp);

	if (prog && !uc_program_entry(prog)) {
		if (errp)
			xasprintf(errp, "Program file contains no entry function\n");

		ucv_put(&prog->header);
		prog = NULL;
	}

	return prog;
}

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

#include <assert.h>
#include <errno.h>
#include <limits.h>

#include "ucode/internal/program.h"
#include "ucode/internal/source.h"
#include "ucode/internal/chunk.h"
#include "ucode/internal/platform.h"


uc_program_t *
uc_program_new(void)
{
	uc_program_t *prog;

	prog = xalloc(sizeof(*prog));

	prog->header.type = UC_PROGRAM;
	prog->header.refcount = 1;
	prog->version = UCODE_BYTECODE_VERSION_MIN;

	prog->functions.next = &prog->functions;
	prog->functions.prev = &prog->functions;

	uc_vallist_init(&prog->constants);

	return prog;
}

uc_program_t *
uc_program_get(uc_program_t *prog)
{
	return (uc_program_t *)ucv_get(prog ? &prog->header : NULL);
}

void
uc_program_put(uc_program_t *prog)
{
	ucv_put(prog ? &prog->header : NULL);
}

uc_function_t *
uc_program_function_new(uc_program_t *prog, const char *name, uc_source_t *source, size_t srcpos)
{
	uc_function_t *func;
	size_t namelen = 0;

	if (name)
		namelen = strlen(name);

	func = xalloc(sizeof(*func) + namelen + 1);

	if (name)
		strcpy(func->name, name);

	for (func->srcidx = 0; func->srcidx < prog->sources.count; func->srcidx++)
		if (prog->sources.entries[func->srcidx] == source)
			break;

	if (func->srcidx >= prog->sources.count)
		uc_vector_push(&prog->sources, uc_source_get(source));

	func->nargs = 0;
	func->nupvals = 0;
	func->srcpos = srcpos;
	func->program = prog;
	func->vararg = false;

	uc_chunk_init(&func->chunk);
	ucv_ref(&prog->functions, &func->progref);

	return func;
}

size_t
uc_program_function_id(uc_program_t *prog, uc_function_t *func)
{
	size_t i = 1;

	uc_program_function_foreach(prog, fn) {
		if (fn == func)
			return i;

		i++;
	}

	return 0;
}

uc_function_t *
uc_program_function_load(uc_program_t *prog, size_t id)
{
	size_t i = 1;

	uc_program_function_foreach(prog, fn)
		if (i++ == id)
			return fn;

	return NULL;
}

uc_source_t *
uc_program_function_source(uc_function_t *fn)
{
	assert(fn->srcidx < fn->program->sources.count);

	return fn->program->sources.entries[fn->srcidx];
}

size_t
uc_program_function_srcpos(uc_function_t *fn, size_t off)
{
	if (!fn)
		return 0;

	return fn->srcpos + uc_chunk_debug_get_srcpos(&fn->chunk, off);
}

/* Resolve the innermost statement enclosing instruction `off` of `fn`, storing
 * the source offsets delimiting its extent, absolute to the function's source
 * buffer, into `startoff` and `endoff`. Returns false if the instruction
 * belongs to no statement, e.g. to implicitly emitted code. Passing SIZE_MAX
 * for `off` resolves the innermost statement still open instead, which is how a
 * syntax error finds the statement it aborted. As with
 * uc_chunk_debug_stmt_bounds(), the out parameters are left untouched on
 * failure. */
bool
uc_program_function_stmt_bounds(uc_function_t *fn, size_t off,
                                size_t *startoff, size_t *endoff)
{
	size_t srcpos;

	if (!fn || !uc_chunk_debug_stmt_bounds(&fn->chunk, off, startoff, endoff))
		return false;

	srcpos = fn->srcpos;

	*startoff += srcpos;
	*endoff += srcpos;

	return true;
}

void
uc_program_function_free(uc_function_t *func)
{
	if (!func)
		return;

	ucv_unref(&func->progref);
	uc_chunk_free(&func->chunk);
	free(func->native);
	free(func);
}

/* Constants are immutable, so each one is created once and shared by all its
 * uses, instead of once per evaluation. The program keeps the reference. */
uc_value_t *
uc_program_constant_load(uc_program_t *prog, size_t idx)
{
	uc_constant_values_t *values = &prog->constant_values;

	if (idx >= prog->constants.isize)
		return NULL;

	if (idx >= values->count) {
		uc_vector_extend(values, prog->constants.isize - values->count);
		values->count = prog->constants.isize;
	}

	if (!values->entries[idx])
		values->entries[idx] = uc_vallist_get(&prog->constants, idx);

	return values->entries[idx];
}

uc_value_t *
uc_program_get_constant(uc_program_t *prog, size_t idx)
{
	return ucv_get(uc_program_peek_constant(prog, idx));
}

/* The hash of the string constant `idx` as an object key, computed once */
uint64_t
uc_program_constant_hash_load(uc_program_t *prog, size_t idx)
{
	uc_constant_hashes_t *hashes = &prog->constant_hashes;

	if (idx >= prog->constants.isize)
		return 0;

	if (idx >= hashes->count) {
		uc_vector_extend(hashes, prog->constants.isize - hashes->count);
		hashes->count = prog->constants.isize;
	}

	if (!hashes->entries[idx])
		hashes->entries[idx] = ucv_object_key_hash(uc_program_peek_constant(prog, idx));

	return hashes->entries[idx];
}

ssize_t
uc_program_add_constant(uc_program_t *prog, uc_value_t *val)
{
	return uc_vallist_add(&prog->constants, val);
}

uc_function_t *
uc_program_entry(uc_program_t *program)
{
	if (program->functions.prev == &program->functions)
		return NULL;

	return (uc_function_t *)program->functions.prev;
}

uc_export_instance_t *
uc_program_instance_get(uc_program_t *program, uc_closure_t *closure, bool create)
{
	uc_vector_foreach(&program->instances, instance)
		if (instance->closure == closure)
			return instance;

	if (!create)
		return NULL;

	return uc_vector_push(&program->instances, { .closure = closure });
}

uc_value_t *
uc_program_main(uc_vm_t *vm, uc_program_t *prog)
{
	uc_function_t *fn = uc_program_entry(prog);

	return fn ? ucv_closure_new(vm, fn, false) : NULL;
}

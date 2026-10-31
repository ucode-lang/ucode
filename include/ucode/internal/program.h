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

/* Internal program API. */

#ifndef UCODE_INTERNAL_PROGRAM_H
#define UCODE_INTERNAL_PROGRAM_H

#include "ucode/program.h"
#include "ucode/internal/types.h"
#include "ucode/internal/util.h"

#define uc_program_function_foreach(prog, fn)			\
	uc_function_t *fn;									\
	for (fn = (uc_function_t *)prog->functions.prev;	\
	     fn != (uc_function_t *)&prog->functions; 		\
	     fn = (uc_function_t *)fn->progref.prev)

#define uc_program_function_foreach_safe(prog, fn)		\
	uc_function_t *fn, *fn##_tmp;						\
	for (fn = (uc_function_t *)prog->functions.prev, 	\
	     fn##_tmp = (uc_function_t *)fn->progref.prev;	\
	     fn != (uc_function_t *)&prog->functions; 		\
	     fn = fn##_tmp, 								\
	     fn##_tmp = (uc_function_t *)fn##_tmp->progref.prev)

#define uc_program_function_last(prog) (uc_function_t *)prog->functions.next

/* Record that the program uses features of the given bytecode version; the
 * newest version recorded this way is what uc_program_write() stores, so a
 * program which uses none of them stays loadable by older VMs.
 *
 * uc_program_load() raises it to the version of the bytecode it read, which
 * keeps that version if the program is written back out. */
static inline void
uc_program_require_version(uc_program_t *prog, uint8_t version)
{
	if (version > prog->version)
		prog->version = version;
}

__hidden uc_function_t *uc_program_entry(uc_program_t *);

/* uc_program_load() of a program, which fails if it has no entry function */
__hidden uc_program_t *uc_program_load_entry(uc_source_t *source, char **errp);

/* prefix of the registry keys under which a VM holds the closures that
 * uc_vm_execute() runs; the address of the program follows */
#define UC_PROGRAM_MAIN_KEY "vm.main."

__hidden uc_export_instance_t *uc_program_instance_get(uc_program_t *, uc_closure_t *, bool);
__hidden void uc_program_instance_free(uc_program_t *, uc_closure_t *, bool);

__hidden uc_function_t *uc_program_function_new(uc_program_t *, const char *, uc_source_t *, size_t);
__hidden size_t uc_program_function_id(uc_program_t *, uc_function_t *);
__hidden uc_function_t *uc_program_function_load(uc_program_t *, size_t);
uc_source_t *uc_program_function_source(uc_function_t *);
size_t uc_program_function_srcpos(uc_function_t *, size_t);
__hidden bool uc_program_function_stmt_bounds(uc_function_t *, size_t, size_t *, size_t *);
__hidden void uc_program_function_free(uc_function_t *);

__hidden uc_value_t *uc_program_constant_load(uc_program_t *, size_t);
__hidden uint64_t uc_program_constant_hash_load(uc_program_t *, size_t);
__hidden uc_value_t *uc_program_get_constant(uc_program_t *, size_t);

/* The value of constant `idx`, which the program keeps the reference to */
static inline uc_value_t *
uc_program_peek_constant(uc_program_t *prog, size_t idx)
{
	if (idx < prog->constant_values.count && prog->constant_values.entries[idx])
		return prog->constant_values.entries[idx];

	return uc_program_constant_load(prog, idx);
}

/* The hash of the string constant `idx` as an object key */
static inline uint64_t
uc_program_get_constant_hash(uc_program_t *prog, size_t idx)
{
	if (idx < prog->constant_hashes.count && prog->constant_hashes.entries[idx])
		return prog->constant_hashes.entries[idx];

	return uc_program_constant_hash_load(prog, idx);
}
__hidden ssize_t uc_program_add_constant(uc_program_t *, uc_value_t *);

#endif /* UCODE_INTERNAL_PROGRAM_H */

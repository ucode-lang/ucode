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

__hidden uc_function_t *uc_program_entry(uc_program_t *);

__hidden uc_function_t *uc_program_function_new(uc_program_t *, const char *, uc_source_t *, size_t);
__hidden size_t uc_program_function_id(uc_program_t *, uc_function_t *);
__hidden uc_function_t *uc_program_function_load(uc_program_t *, size_t);
uc_source_t *uc_program_function_source(uc_function_t *);
size_t uc_program_function_srcpos(uc_function_t *, size_t);
__hidden bool uc_program_function_stmt_bounds(uc_function_t *, size_t, size_t *, size_t *);
__hidden void uc_program_function_free(uc_function_t *);

__hidden uc_value_t *uc_program_get_constant(uc_program_t *, size_t);
__hidden ssize_t uc_program_add_constant(uc_program_t *, uc_value_t *);

#endif /* UCODE_INTERNAL_PROGRAM_H */

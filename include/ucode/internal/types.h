/*
 * Copyright (C) 2020-2021 Jo-Philipp Wich <jo@mein.io>
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

/* Internal type definitions: source, program, function and chunk layouts,
 * thread-context and object-iterator state, and the internal value entry
 * points. Included by ucode's implementation files. */

#ifndef UCODE_INTERNAL_TYPES_H
#define UCODE_INTERNAL_TYPES_H

#include <signal.h>

#include "ucode/types.h"
#include "ucode/internal/util.h"


/* Constant list definitions (internal: backs uc_program_t) */

typedef struct {
	size_t isize;
	size_t dsize;
	uint64_t *index;
	char *data;
} uc_value_list_t;


/* Source buffer */

uc_declare_vector(uc_lineinfo_t, uint8_t);

struct uc_source {
	uc_value_t header;
	char *filename, *runpath, *buffer;
	FILE *fp;
	size_t off;
	uc_lineinfo_t lineinfo;
	struct {
		size_t count, offset;
		uc_value_t **entries;
	} exports;
};


/* Bytecode chunk definitions (internal) */

typedef struct {
	size_t from, to, target, slot;
} uc_ehrange_t;

typedef struct {
	size_t from, to, slot, nameidx;
} uc_varrange_t;

typedef struct {
	uint8_t bytes;
	uint8_t insns;
} uc_offset_t;

uc_declare_vector(uc_ehranges_t, uc_ehrange_t);
uc_declare_vector(uc_variables_t, uc_varrange_t);
uc_declare_vector(uc_offsetinfo_t, uc_offset_t);

typedef struct {
	size_t count;
	uint8_t *entries;
	uc_ehranges_t ehranges;
	struct {
		uc_variables_t variables;
		uc_value_list_t varnames;
		uc_offsetinfo_t offsets;
	} debuginfo;
} uc_chunk_t;


/* Function value */

struct uc_function {
	uc_weakref_t progref;
	bool arrow, vararg, strict, module;
	size_t nargs;
	size_t nupvals;
	size_t srcidx;
	size_t srcpos;
	uc_chunk_t chunk;
	struct uc_program *program;
	char name[];
};


/* Object iterator state (internal) */

typedef struct {
	uc_list_t list;
	struct lh_table *table;
	union {
		struct lh_entry *pos;
		struct {
			const void *k;
			unsigned long hash;
		} kh;
	} u;
} uc_object_iterator_t;


/* Stringification state (internal) */

typedef struct uc_stringify_frame {
	struct uc_stringify_frame *prev;
	uc_value_t *value;
} uc_stringify_frame_t;


/* Program structure */

uc_declare_vector(uc_sources_t, uc_source_t *);

struct uc_program {
	uc_value_t header;
	uc_value_list_t constants;
	uc_weakref_t functions;
	uc_sources_t sources;
	uc_modexports_t exports;
};


/* TLS data (internal) */

typedef struct {
	/* VM owning installed signal handlers */
	uc_vm_t *signal_handler_vm;

	/* Reference counter of this thread context for deallocation purposes */
	size_t refcount;

	/* Object iteration */
	uc_list_t object_iterators;

	/* Innermost container under stringification */
	uc_stringify_frame_t *stringify_frame;
} uc_thread_context_t;

__hidden uc_thread_context_t *uc_thread_context_get(void);

__hidden void uc_thread_context_free(void);


/* Internal value entry points */

__hidden bool ucv_compare(int, uc_value_t *, uc_value_t *, int *);

__hidden uc_value_t *ucv_closure_new(uc_vm_t *, uc_function_t *, bool);

__hidden void ucv_free(uc_value_t *, bool);
__hidden void ucv_unref(uc_weakref_t *);
__hidden void ucv_ref(uc_weakref_t *, uc_weakref_t *);
__hidden void ucv_freeall(uc_vm_t *);

/* Type-cast helpers */
#define ucv_as_integer(v)   ((uc_integer_t *)   (v))
#define ucv_as_double(v)    ((uc_double_t *)    (v))
#define ucv_as_string(v)    ((uc_string_t *)    (v))
#define ucv_as_array(v)     ((uc_array_t *)     (v))
#define ucv_as_object(v)    ((uc_object_t *)    (v))
#define ucv_as_regexp(v)    ((uc_regexp_t *)    (v))
#define ucv_as_upvalue(v)   ((uc_upvalref_t *)  (v))
#define ucv_as_closure(v)   ((uc_closure_t *)   (v))
#define ucv_as_cfunction(v) ((uc_cfunction_t *) (v))
#define ucv_as_resource(v)  ((uc_resource_t *)  (v))
#define ucv_as_program(v)   ((uc_program_t *)   (v))
#define ucv_as_source(v)    ((uc_source_t *)    (v))

#endif /* UCODE_INTERNAL_TYPES_H */

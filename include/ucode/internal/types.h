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
	char *filename, *runpath, *buffer, *path;
	FILE *fp;
	uint8_t *rbuf;
	size_t off, size;
	uint8_t *ahead;
	size_t ahead_off, ahead_len;
	bool done, sized, moved;
	uc_lineinfo_t lineinfo;
	struct {
		size_t count, offset;
		uc_value_t **entries;
		size_t capacity;
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
	size_t capacity;
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
	/* the native code of a function of a native program, which has no
	 * bytecode */
	struct uc_native_function_info *native;
	char name[];
};


/* Object iterator state (internal) */

typedef struct {
	uc_list_t list;
	uc_object_t *object;
	size_t pos;
} uc_object_iterator_t;


/* Program structure */

uc_declare_vector(uc_sources_t, uc_source_t *);

typedef struct {
	size_t from, count;
	uc_value_t *object;
} uc_namespace_t;

uc_declare_vector(uc_namespaces_t, uc_namespace_t);

/* The exports of the modules that a program links statically, as created by
 * the runs of one closure of its entry function, and the namespace objects
 * of the wildcard imports of these modules, one per module. */
typedef struct {
	uc_closure_t *closure;
	uc_modexports_t exports;
	uc_namespaces_t namespaces;
} uc_export_instance_t;

typedef struct {
	uc_closure_t *closure;
	size_t cursor;
	bool keep;
} uc_program_run_t;

uc_declare_vector(uc_export_instances_t, uc_export_instance_t);
uc_declare_vector(uc_program_runs_t, uc_program_run_t);
uc_declare_vector(uc_constant_values_t, uc_value_t *);
uc_declare_vector(uc_constant_hashes_t, uint64_t);

struct uc_program {
	uc_value_t header;
	uc_value_list_t constants;
	/* the values of the constants, created on their first use */
	uc_constant_values_t constant_values;
	/* their hashes as object keys, 0 until first use */
	uc_constant_hashes_t constant_hashes;
	uc_weakref_t functions;
	uc_sources_t sources;
	uc_export_instances_t instances;
	uc_program_runs_t runs;
	uint32_t closure_refs : 28;
	uint32_t version : 4;     /* bytecode version required, see uc_program_require_version() */
};


/* TLS data (internal) */

typedef struct {
	/* VM owning installed signal handlers */
	uc_vm_t *signal_handler_vm;

	/* Reference counter of this thread context for deallocation purposes */
	size_t refcount;

	/* Object iteration */
	uc_list_t object_iterators;

	/* Marks the values that a runtime holds outside of the VM state, such
	 * as the locals of native code */
	void (*gc_roots_mark)(uc_vm_t *vm, void (*mark)(uc_value_t *));
} uc_thread_context_t;

__hidden uc_thread_context_t *uc_thread_context_get(void);

__hidden void uc_thread_context_free(void);


/* Internal value entry points */

/* Whether the value is an integer that the pointer itself holds, which needs
 * no reference counting and fits an int64_t with room for one addition */
static inline bool
ucv_is_small_integer(uc_value_t *uv)
{
	return ((uintptr_t)uv & 3) == UC_INTEGER;
}

/* The value of an integer that the pointer holds: the bits above the type and
 * the sign bit hold its magnitude */
static inline int64_t
ucv_small_integer_get(uc_value_t *uv)
{
	uintptr_t pv = (uintptr_t)uv;

	if (((pv >> 2) & 1) == 0)
		return (int64_t)(pv >> 3);

	return -(int64_t)(pv >> 3);
}

/* Whether the value has a reference count, unlike null and the values that
 * the pointer itself holds */
static inline bool
ucv_is_counted(uc_value_t *uv)
{
	return uv != NULL && ((uintptr_t)uv & 3) == 0;
}

__hidden bool ucv_compare(int, uc_value_t *, uc_value_t *, int *);

__hidden uc_value_t *ucv_closure_new(uc_vm_t *, uc_function_t *, bool);

__hidden void ucv_free(uc_value_t *, bool);
__hidden void ucv_unref(uc_weakref_t *);
__hidden void ucv_ref(uc_weakref_t *, uc_weakref_t *);
__hidden void ucv_freeall(uc_vm_t *);

/* ucv_object_get(), ucv_object_add() and ucv_object_delete() for keys of a
 * given length, which may contain NUL bytes */
__hidden uc_value_t *ucv_object_lookup(uc_value_t *, const char *, size_t, bool *);
__hidden bool ucv_object_insert(uc_value_t *, const char *, size_t, uc_value_t *);
__hidden bool ucv_object_remove(uc_value_t *, const char *, size_t);

/* ucv_object_get() and ucv_object_add() with the key as a string value,
 * which the object shares */
__hidden uc_value_t *ucv_object_lookup_string(uc_value_t *, uc_value_t *, bool *);
__hidden bool ucv_object_insert_string(uc_value_t *, uc_value_t *, uc_value_t *);

/* The hash of the string `key` as an object key, ucv_object_lookup_string()
 * with that hash, and ucv_key_get() of an own key of the object `obj` with
 * that hash, which returns false if `obj` is no object or lacks the key */
__hidden uint64_t ucv_object_key_hash(uc_value_t *key);
__hidden uc_value_t *ucv_object_lookup_hashed(uc_value_t *, uc_value_t *, uint64_t, bool *);
__hidden bool ucv_key_get_hashed(uc_vm_t *vm, uc_value_t *obj, uc_value_t *key, uint64_t hash,
                                 uc_value_t **res);

__hidden void ucv_object_sort_entries(uc_value_t *,
	int (*)(const uc_object_entry_t *, const uc_object_entry_t *, void *), void *);

/* Returns the entry after `e`, or the first one if `e` is NULL, skipping
 * deleted entries; NULL at the end or if `uv` is no object. */
static inline uc_object_entry_t *
ucv_object_entry_next(uc_value_t *uv, uc_object_entry_t *e)
{
	uc_object_t *object = (uc_object_t *)uv;

	if (ucv_type(uv) != UC_OBJECT)
		return NULL;

	for (e = e ? e + 1 : object->entries; e && e < object->entries + object->used; e++)
		if (e->key)
			return e;

	return NULL;
}

#define ucv_object_foreach_entry(obj, e) \
	for (uc_object_entry_t *e = ucv_object_entry_next((obj), NULL); e; \
	     e = ucv_object_entry_next((obj), e))

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

/*
 * Copyright (C) 2021 Jo-Philipp Wich <jo@mein.io>
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

#ifndef UCODE_TYPES_H
#define UCODE_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <regex.h>
#include <signal.h>

#include <ucode/util.h>
#include <ucode/platform.h>


/* Value types and generic value header.
 *
 * `mark` is the cycle-detection mark used by stringification and by ubus
 * blobmsg conversion (and as a reentrancy guard in ucv_free()); `gc_mark`
 * is the collector's reachability mark.  The two cannot share a bit: a
 * tostring callback may run the collector while the formatter has marked
 * the active traversal path, and the mark phase must then descend into
 * those containers instead of stopping at their marks.
 *
 * `gc_mark` reuses the top bit of the former 26 bit refcount field,
 * reducing the refcount to 25 bits; a refcount high enough to have set that
 * bit would exceed 33 million, so the change is invisible in practice and
 * `type`, `mark` and `ext_flag` keep their positions.  The field is
 * declared so that this top bit is chosen on both endiannesses: little
 * endian packs bitfields low to high, so gc_mark is declared last, while
 * big endian packs them high to low, so it is declared first. */

typedef enum uc_type {
	UC_NULL,
	UC_INTEGER,
	UC_BOOLEAN,
	UC_STRING,
	UC_DOUBLE,
	UC_ARRAY,
	UC_OBJECT,
	UC_REGEXP,
	UC_CFUNCTION,
	UC_CLOSURE,
	UC_UPVALUE,
	UC_RESOURCE,
	UC_PROGRAM,
	UC_SOURCE
} uc_type_t;

#if __BYTE_ORDER == __LITTLE_ENDIAN
typedef struct uc_value {
	uint32_t type:4;
	uint32_t mark:1;
	uint32_t ext_flag:1;
	uint32_t refcount:25;
	uint32_t gc_mark:1;
} uc_value_t;
#else
typedef struct uc_value {
	uint32_t type:4;
	uint32_t mark:1;
	uint32_t ext_flag:1;
	uint32_t gc_mark:1;
	uint32_t refcount:25;
} uc_value_t;
#endif


struct uc_vm;
struct uc_source;
struct uc_program;
struct uc_function;

struct uc_parse_config;
typedef struct uc_parse_config uc_parse_config_t;

typedef struct uc_vm uc_vm_t;
typedef struct uc_source uc_source_t;
typedef struct uc_program uc_program_t;
typedef struct uc_function uc_function_t;


/* Value type structures. */

typedef struct uc_weakref {
	struct uc_weakref *prev;
	struct uc_weakref *next;
} uc_weakref_t;

typedef struct {
	uc_value_t header;
	double dbl;
} uc_double_t;

typedef struct {
	uc_value_t header;
	union {
		int64_t s64;
		uint64_t u64;
	} i;
} uc_integer_t;

typedef struct {
	uc_value_t header;
	size_t length;
	char str[];
} uc_string_t;

typedef struct {
	uc_value_t header;
	uc_weakref_t ref;
	size_t count;
	uc_value_t *proto;
	uc_value_t **entries;
	size_t capacity;
} uc_array_t;

/* `key` is a string value, NULL in place of a deleted entry */
typedef struct {
	uc_value_t *key;
	uc_value_t *value;
} uc_object_entry_t;

/* The entries are in insertion order; `used` counts them with the deleted
 * ones, `count` without. Objects with many keys also have a hash index of
 * `mask` + 1 slots that hold entry positions plus one. */
typedef struct {
	uc_value_t header;
	uc_weakref_t ref;
	uc_value_t *proto;
	uc_object_entry_t *entries;
	uint32_t *index;
	uint32_t count;
	uint32_t used;
	uint32_t size;
	uint32_t mask;
} uc_object_t;

typedef struct {
	uc_value_t header;
	regex_t regexp;
	bool icase, newline, global;
	char source[];
} uc_regexp_t;

typedef struct uc_upval_tref {
	uc_value_t header;
	size_t slot;
	bool closed;
	uc_value_t *value;
	struct uc_upval_tref *next;
} uc_upvalref_t;

typedef struct {
	uc_value_t header;
	uc_weakref_t ref;
	bool is_arrow;
	uc_function_t *function;
	uc_upvalref_t **upvals;
} uc_closure_t;

typedef uc_value_t *(*uc_cfn_ptr_t)(uc_vm_t *, size_t);

typedef struct {
	uc_value_t header;
	uc_cfn_ptr_t cfn;
	char name[];
} uc_cfunction_t;

typedef struct {
	const char *name;
	uc_value_t *proto;
	void (*free)(void *);
} uc_resource_type_t;

typedef struct {
	uc_value_t header;
	uc_resource_type_t *type;
	void *data;
} uc_resource_t;

typedef struct {
	uc_value_t header;
	uc_weakref_t ref;
	uc_resource_type_t *type;

	uint32_t reserved:2;
	uint32_t hasproto:1;
	uint32_t persistent:1;
	uint32_t uvcount:8;
	uint32_t datasize:20;

	uint32_t _pad;
} uc_resource_ext_t;

uc_declare_vector(uc_resource_types_t, uc_resource_type_t *);


/* VM exception, call-frame and stack state */

typedef enum {
	EXCEPTION_NONE,
	EXCEPTION_SYNTAX,
	EXCEPTION_RUNTIME,
	EXCEPTION_TYPE,
	EXCEPTION_REFERENCE,
	EXCEPTION_USER,
	EXCEPTION_EXIT,
	EXCEPTION_MAX
} uc_exception_type_t;

extern const char *exception_type_strings[];

typedef struct {
	uc_exception_type_t type;
	uc_value_t *stacktrace;
	char *message;
} uc_exception_t;

typedef struct {
	uint8_t *ip;
	uc_closure_t *closure;
	uc_cfunction_t *cfunction;
	size_t stackframe;
	uc_value_t *ctx;
	bool mcall, strict;
	/* number of tail calls that reused this frame, i.e. the number of call
	 * frames collapsed into it by tail call optimization. stored as a uint16_t
	 * so it fits in the struct's trailing padding and does not change its size,
	 * preserving the ABI; the count saturates at 0xffff rather than wrapping */
	uint16_t tco;
} uc_callframe_t;

typedef struct uc_breakpoint {
	uint8_t *ip;
	void (*cb)(uc_vm_t *, struct uc_breakpoint *);
} uc_breakpoint_t;

uc_declare_vector(uc_callframes_t, uc_callframe_t);
uc_declare_vector(uc_stack_t, uc_value_t *);
uc_declare_vector(uc_breakpoints_t, uc_breakpoint_t *);

typedef void (uc_exception_handler_t)(uc_vm_t *, uc_exception_t *);

uc_declare_vector(uc_modexports_t, uc_upvalref_t *);

struct uc_vm {
	uc_stack_t stack;
	uc_exception_t exception;
	uc_callframes_t callframes;
	uc_upvalref_t *open_upvals;
	uc_parse_config_t *config;
	uc_value_t *globals;
	uc_value_t *registry;
	void *reserved;
	uc_weakref_t values;
	uc_resource_types_t restypes;
	uc_breakpoints_t breakpoints;
	union {
		uint32_t u32;
		int32_t s32;
		uint16_t u16;
		int16_t s16;
		uint8_t u8;
		int8_t s8;
	} arg;
	size_t alloc_refs;
	uint8_t trace;
	uint8_t gc_flags;
	uint16_t gc_interval;
	uc_stringbuf_t *strbuf;
	uc_exception_handler_t *exhandler;
	FILE *output;
	struct {
		uint64_t raised[((UC_SYSTEM_SIGNAL_COUNT + 63) & ~63) / 64];
		uc_value_t *handler;
		struct sigaction sa;
		int sigpipe[2];
	} signal;
	bool break_requested;
	int break_notifyfd[2];
};


/* Value API */

uc_value_t *ucv_get(uc_value_t *uv);
void ucv_put(uc_value_t *);

uc_type_t ucv_type(uc_value_t *);
const char *ucv_typename(uc_value_t *);

uc_value_t *ucv_boolean_new(bool);
bool ucv_boolean_get(uc_value_t *);

uc_value_t *ucv_string_alloc(char **, size_t);
uc_value_t *ucv_string_new(const char *);
uc_value_t *ucv_string_new_length(const char *, size_t);
size_t ucv_string_length(uc_value_t *);

char *_ucv_string_get(uc_value_t **);
#define ucv_string_get(uv) _ucv_string_get((uc_value_t **)&uv)

uc_stringbuf_t *ucv_stringbuf_new(void);
uc_value_t *ucv_stringbuf_finish(uc_stringbuf_t *);

#define ucv_stringbuf_append(buf, str) uc_stringbuf_append(buf, str)
#define ucv_stringbuf_addstr(buf, str, len) uc_stringbuf_add(buf, str, len)
#define ucv_stringbuf_printf(buf, fmt, ...) uc_stringbuf_printf(buf, fmt, __VA_ARGS__)

uc_value_t *ucv_int64_new(int64_t);
uc_value_t *ucv_uint64_new(uint64_t);
int64_t ucv_int64_get(uc_value_t *);
uint64_t ucv_uint64_get(uc_value_t *);

uc_value_t *ucv_double_new(double);
double ucv_double_get(uc_value_t *);

uc_value_t *ucv_array_new(uc_vm_t *);
uc_value_t *ucv_array_new_length(uc_vm_t *, size_t);
uc_value_t *ucv_array_get(uc_value_t *, size_t);
uc_value_t *ucv_array_pop(uc_value_t *);
uc_value_t *ucv_array_push(uc_value_t *, uc_value_t *);
uc_value_t *ucv_array_shift(uc_value_t *);
uc_value_t *ucv_array_unshift(uc_value_t *, uc_value_t *);
void ucv_array_sort(uc_value_t *, int (*)(const void *, const void *));
void ucv_array_sort_r(uc_value_t *, int (*)(uc_value_t *, uc_value_t *, void *), void *);
bool ucv_array_delete(uc_value_t *, size_t, size_t);
bool ucv_array_set(uc_value_t *, size_t, uc_value_t *);
size_t ucv_array_length(uc_value_t *);

uc_value_t *ucv_object_new(uc_vm_t *);
uc_value_t *ucv_object_new_size(uc_vm_t *, size_t);
uc_value_t *ucv_object_get(uc_value_t *, const char *, bool *);
bool ucv_object_add(uc_value_t *, const char *, uc_value_t *);
void ucv_object_sort(uc_value_t *, int (*)(const void *, const void *));
void ucv_object_sort_r(uc_value_t *, int (*)(const char *, uc_value_t *, const char *, uc_value_t *, void *), void *);
bool ucv_object_delete(uc_value_t *, const char *);
size_t ucv_object_length(uc_value_t *);
bool ucv_object_next(uc_value_t *, size_t *, char **, uc_value_t **);

#define ucv_object_foreach(obj, key, val)							\
	char *key = NULL;												\
	uc_value_t *val = NULL;											\
	for (size_t pos##key = 0; ucv_object_next((obj), &pos##key, &key, &val); )

uc_value_t *ucv_cfunction_new(const char *, uc_cfn_ptr_t);

uc_resource_type_t *ucv_resource_type_add(uc_vm_t *, const char *, uc_value_t *, void (*)(void *));
uc_resource_type_t *ucv_resource_type_lookup(uc_vm_t *, const char *);

uc_value_t *ucv_resource_new(uc_resource_type_t *, void *);
uc_value_t *ucv_resource_new_ex(uc_vm_t *, uc_resource_type_t *, void **, size_t, size_t);
uc_value_t *ucv_resource_new_with_proto(uc_vm_t *, uc_resource_type_t *, void **, size_t, size_t, uc_value_t *);
void *ucv_resource_data(uc_value_t *uv, const char *);
void **ucv_resource_dataptr(uc_value_t *, const char *);
uc_value_t *ucv_resource_value_get(uc_value_t *, size_t);
bool ucv_resource_value_set(uc_value_t *, size_t, uc_value_t *);

static inline uc_resource_type_t *
ucv_resource_type(uc_value_t *uv)
{
		if (uv->ext_flag) {
			uc_resource_ext_t *res = (uc_resource_ext_t *)uv;

			return res->type;
		}
		else {
			uc_resource_t *res = (uc_resource_t *)uv;

			return res->type;
		}
}

static inline uc_value_t *
ucv_resource_create(uc_vm_t *vm, const char *type, void *value)
{
    uc_resource_type_t *t = NULL;

    if (type && (t = ucv_resource_type_lookup(vm, type)) == NULL)
        return NULL;

    return ucv_resource_new(t, value);
}

static inline uc_value_t *
ucv_resource_create_ex(uc_vm_t *vm, const char *type, void **data, size_t uvcount, size_t datasize)
{
    uc_resource_type_t *t = NULL;

    if (type && (t = ucv_resource_type_lookup(vm, type)) == NULL)
        return NULL;

    return ucv_resource_new_ex(vm, t, data, uvcount, datasize);
}

static inline uc_value_t *
ucv_resource_create_with_proto(uc_vm_t *vm, const char *type, void **data, size_t uvcount, size_t datasize, uc_value_t *proto)
{
    uc_resource_type_t *t = NULL;

    if (type && (t = ucv_resource_type_lookup(vm, type)) == NULL)
        return NULL;

    return ucv_resource_new_with_proto(vm, t, data, uvcount, datasize, proto);
}

static inline bool
ucv_resource_is_extended(uc_value_t *uv)
{
	return (((uintptr_t)uv & 3) == 0 && uv != NULL &&
	        uv->ext_flag == true && uv->type == UC_RESOURCE);
}

static inline bool
ucv_resource_is_persistent(uc_value_t *uv)
{
	uc_resource_ext_t *res = (uc_resource_ext_t *)uv;

	return ucv_resource_is_extended(uv) && res->persistent;
}

static inline bool
ucv_resource_persistent_set(uc_value_t *uv, bool persistent)
{
	uc_resource_ext_t *res = (uc_resource_ext_t *)uv;

	if (!ucv_resource_is_extended(uv) || res->persistent == persistent)
		return false;

	res->persistent = persistent;

	return true;
}

static inline bool
ucv_resource_hasproto(uc_value_t *uv)
{
	uc_resource_ext_t *res = (uc_resource_ext_t *)uv;

	return ucv_resource_is_extended(uv) && res->hasproto;
}

#define UCV_RESOURCE_PROTO_IDX ((size_t)-1)

static inline uc_value_t *
ucv_resource_proto_get(uc_value_t *uv)
{
	return ucv_resource_value_get(uv, UCV_RESOURCE_PROTO_IDX);
}

static inline bool
ucv_resource_proto_set(uc_value_t *uv, uc_value_t *proto)
{
	return ucv_resource_value_set(uv, UCV_RESOURCE_PROTO_IDX, proto);
}

uc_value_t *ucv_regexp_new(const char *, bool, bool, bool, char **);

uc_value_t *ucv_upvalref_new(size_t);

uc_value_t *ucv_prototype_get(uc_value_t *);
bool ucv_prototype_set(uc_value_t *, uc_value_t *);

uc_value_t *ucv_property_get(uc_value_t *, const char *);

char *ucv_to_string(uc_vm_t *, uc_value_t *);
char *ucv_to_jsonstring_formatted(uc_vm_t *, uc_value_t *, char, size_t);
void ucv_to_stringbuf_formatted(uc_vm_t *, uc_stringbuf_t *, uc_value_t *, size_t, char, size_t);

#define ucv_to_jsonstring(vm, val) ucv_to_jsonstring_formatted(vm, val, '\1', 0)
#define ucv_to_stringbuf(vm, buf, val, json) ucv_to_stringbuf_formatted(vm, buf, val, 0, json ? '\1' : '\0', 0)

uc_type_t ucv_cast_number(uc_value_t *, int64_t *, double *);

uc_value_t *ucv_to_number(uc_value_t *);

static inline double
ucv_to_double(uc_value_t *v)
{
	uc_value_t *nv;
	double d;

	nv = ucv_to_number(v);
	d = ucv_double_get(nv);
	ucv_put(nv);

	return d;
}

static inline int64_t
ucv_to_integer(uc_value_t *v)
{
	uc_value_t *nv;
	int64_t n;

	nv = ucv_to_number(v);
	n = ucv_int64_get(nv);
	ucv_put(nv);

	return n;
}

static inline uint64_t
ucv_to_unsigned(uc_value_t *v)
{
	uc_value_t *nv;
	uint64_t u;

	nv = ucv_to_number(v);
	u = ucv_uint64_get(nv);
	ucv_put(nv);

	return u;
}

/* Walk the prototype chain of the given value (own keys of the value itself
 * do not count) and return the first non-null value stored under the given
 * name, or NULL. The result may be a function (for invocation) or an object
 * (for delegation); callers must check ucv_is_callable() before invoking. */
uc_value_t *ucv_metamethod_lookup(uc_value_t *, const char *);

static inline bool
ucv_is_callable(uc_value_t *uv)
{
	uc_value_t *m;

	switch (ucv_type(uv)) {
	case UC_CLOSURE:
	case UC_CFUNCTION:
		return true;

	case UC_OBJECT:
	case UC_ARRAY:
	case UC_RESOURCE:
		/* a value whose prototype chain provides a callable `__call__` method
		 * can be invoked like a function itself */
		m = ucv_metamethod_lookup(uv, "__call__");

		return ucv_type(m) == UC_CLOSURE || ucv_type(m) == UC_CFUNCTION;

	default:
		return false;
	}
}

static inline bool
ucv_is_arrowfn(uc_value_t *uv)
{
	uc_closure_t *closure = (uc_closure_t *)uv;

	return (ucv_type(uv) == UC_CLOSURE && closure->is_arrow);
}

static inline bool
ucv_is_u64(uc_value_t *uv)
{
	return (((uintptr_t)uv & 3) == 0 && uv != NULL && uv->ext_flag == true &&
		    uv->type == UC_INTEGER);
}

static inline bool
ucv_is_constant(uc_value_t *uv)
{
	return (((uintptr_t)uv & 3) == 0 && uv != NULL && uv->ext_flag == true &&
	        (uv->type == UC_ARRAY || uv->type == UC_OBJECT));
}

static inline bool
ucv_set_constant(uc_value_t *uv, bool constant)
{
	if (((uintptr_t)uv & 3) == 0 && uv != NULL && uv->ext_flag != constant &&
	    (uv->type == UC_ARRAY || uv->type == UC_OBJECT)) {
		uv->ext_flag = constant;

		return true;
	}

	return false;
}

static inline bool
ucv_is_scalar(uc_value_t *uv)
{
	switch (ucv_type(uv)) {
	case UC_NULL:
	case UC_BOOLEAN:
	case UC_DOUBLE:
	case UC_INTEGER:
	case UC_STRING:
		return true;

	default:
		return false;
	}
}

bool ucv_is_equal(uc_value_t *, uc_value_t *);
bool ucv_is_truish(uc_value_t *);

/* Property accessors behind `foo.bar`, `foo.bar = x` and `delete foo.bar`.
 * Own keys and numeric array indices take precedence, the __get__, __set__ and
 * __delete__ metamethods of the accessed value serve as fallback; with a null
 * vm nothing is dispatched. ucv_key_set() retains the value stored and returns
 * a reference to it, or NULL if the store failed. Setting a non-index property
 * on an array is guaranteed to fail (arrays have no own-key storage); in
 * strict mode it raises a type error instead of being silently dropped,
 * unless the array has a __set__ metamethod which handles the store.
 * ucv_key_delete() returns whether the key was removed; for values without
 * own-key storage (arrays, resources) a __delete__ metamethod is the only way
 * to handle a key, and a value which has neither the storage nor the metamethod
 * raises a reference error, as does deleting from a value which can carry no
 * prototype at all. */
uc_value_t *ucv_key_get(uc_vm_t *, uc_value_t *, uc_value_t *);
uc_value_t *ucv_key_set(uc_vm_t *, uc_value_t *, uc_value_t *, uc_value_t *);
bool ucv_key_delete(uc_vm_t *, uc_value_t *, uc_value_t *);

/* Raw counterparts, exposed to scripts as rawget(), rawset() and rawdelete():
 * the escape hatch for a metamethod which needs to reach the storage below
 * itself, since a __set__ doing this[key] = val would dispatch itself again. */
uc_value_t *ucv_key_rawget(uc_vm_t *, uc_value_t *, uc_value_t *);
uc_value_t *ucv_key_rawset(uc_vm_t *, uc_value_t *, uc_value_t *, uc_value_t *);
bool ucv_key_rawdelete(uc_vm_t *, uc_value_t *, uc_value_t *);


static inline bool
ucv_is_marked(uc_value_t *uv)
{
	return (((uintptr_t)uv & 3) == 0 && uv != NULL && uv->mark == true);
}

static inline void
ucv_set_mark(uc_value_t *uv)
{
	if (((uintptr_t)uv & 3) == 0 && uv != NULL)
		uv->mark = true;
}

static inline void
ucv_clear_mark(uc_value_t *uv)
{
	if (((uintptr_t)uv & 3) == 0 && uv != NULL)
		uv->mark = false;
}

void ucv_gc(uc_vm_t *);

#endif /* UCODE_TYPES_H */

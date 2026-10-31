/*
 * FFI module for ucode - LibFFI-based Foreign Function Interface
 *
 * Copyright (C) 2005-2025 Mike Pall (LuaJIT FFI implementation)
 * Copyright (C) 2023-2026 Jo-Philipp Wich <jo@mein.io> (ucode integration)
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
 *
 * This module contains derived work from LuaJIT's FFI implementation.
 *
 * Modifications from LuaJIT:
 * - Replaced LuaJIT's own call infrastructure (lj_ccall.c, lj_ccallback.c) with libffi
 * - Adapted VM interactions to use ucode's API (uc_vm_t, uc_value_t, etc.)
 * - Removed JIT-specific code and dependencies
 * - Consolidated library loading logic into this module
 *
 * See NOTICE and ATTRIBUTION.md for complete attribution details.
 */

/**
 * # Foreign Function Interface (FFI)
 *
 * The `ffi` module provides a foreign function interface for ucode, allowing
 * direct interaction with C libraries. It combines a C declaration parser with
 * libffi-based function calling to enable seamless interop between ucode and C.
 *
 * The module can be imported using the wildcard import syntax:
 *
 *   ```
 *   import * as ffi from 'ffi';
 *   ```
 *
 * ## Synopsis
 *
 * ```javascript
 * import * as ffi from 'ffi';
 *
 * // 1. Declare C types and functions
 * ffi.cdef(`
 *     struct point { int x; int y; };
 *     extern char **environ;
 * `);
 *
 * // 2. Call C functions via the global C namespace
 * // Primitive return values are auto-converted to ucode types
 * let strcmp = ffi.C.wrap('int strcmp(const char *, const char *)');
 * print(strcmp("hello", "world"), "\n");  // => non-zero (number)
 *
 * // 3. String return values remain as cdata - use ffi.string() to convert
 * let getenv = ffi.C.wrap('char *getenv(char *)');
 * let path_ptr = getenv('PATH');      // Returns char* cdata
 * let path_str = ffi.string(path_ptr); // Convert to ucode string
 *
 * // 4. Create C data instances
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point', 10, 20);
 * print(p.get('x'), p.get('y'), "\n");  // => 10 20
 *
 * // 5. Access global variables
 * print(ffi.C.dlsym('environ').get(0), "\n");
 *
 * // 6. Query type information
 * print(ffi.sizeof('int'), "\n");        // => 4
 * print(ffi.alignof('double'), "\n");    // => 8
 * print(ffi.offsetof('struct point', 'y'), "\n");  // => 4
 *
 * // 7. Load external libraries
 * let libz = ffi.dlopen('z');
 * let zlibVersion = libz.wrap('const char *zlibVersion(void)');
 * print(zlibVersion().slice(), "\n");  // => "1.2.11" (or similar)
 *
 * // Use in callbacks (primitives auto-converted)
 * let qsort = ffi.C.wrap('void qsort(void *, size_t, size_t, int (*)(const void *, const void *))');
 * let cmp = ffi.C.wrap('int strcmp(const char *, const char *)');
 * let arr = ffi.ctype('char *[5]', ["zebra", "apple", "banana", "cherry", "date"]);
 * // cmp() returns ucode number directly (primitives auto-converted)
 * qsort(arr.ptr(), arr.length(), arr.itemsize(),
 *       (a, b) => cmp(a.deref('const char *'), b.deref('const char *')));
 * ```
 *
 * ## Passing ucode Functions to C (Closures)
 *
 * When a C function expects a callback, the ucode function can be passed
 * directly as the corresponding function-pointer argument: the FFI layer
 * transparently creates a C closure for the call's duration and invokes the
 * ucode function for each C call.
 *
 * A closure can also be created explicitly with `closure()`, the counterpart
 * of `wrap()`, and stored in a variable or passed to other ucode functions
 * that accept function pointers:
 *
 * ```javascript
 * let qsort = ffi.C.wrap('void qsort(void *, size_t, size_t, int (*)(const void *, const void *))');
 * let arr = ffi.ctype('int[5]', [56, 4, 12, 1, 5]);
 *
 * // Explicit closure: a function-pointer cdata ("ffi.closure") bound to a ucode function
 * let cmp = ffi.closure('int (*)(const void *, const void *)',
 *                     (a, b) => a.deref('int') - b.deref('int'));
 * qsort(arr.ptr(), arr.length(), arr.itemsize(), cmp);
 *
 * // Or simply pass the ucode function directly:
 * qsort(arr.ptr(), arr.length(), arr.itemsize(),
 *       (a, b) => a.deref('int') - b.deref('int'));
 * ```
 *
 * The returned cdata keeps the ucode function alive for as long as it is
 * reachable; dropping all references to it releases the closure. A function
 * may be re-bound to a new function-pointer type with `cast()`.
 *
 * ## Memory Management for char* Return Values
 *
 * When a wrapped C function returns `char*`, the return value is a **cdata pointer
 * object**, not an auto-converted ucode string. This design prevents memory leaks
 * and gives you explicit control over memory management.
 *
 * ### Converting char* to ucode Strings
 *
 * Use `ffi.string()` or `slice()` to convert a char* cdata to a ucode string:
 *
 * ```javascript
 * let getenv = ffi.C.wrap('char *getenv(char *)');
 *
 * let path_ptr = getenv('PATH');    // Returns char* cdata
 * let path = ffi.string(path_ptr);  // Convert to ucode string
 * // or equivalently:
 * let path = path_ptr.slice();      // slice() without args = string()
 * ```
 *
 * **Note**: Both `ffi.string()` and `slice()` create a **copy** of the C string.
 * The original C memory remains untouched.
 *
 * ### Memory Ownership Patterns
 *
 * #### Pattern 1: C Manages Memory (No Free Required)
 *
 * Functions like `getenv()`, `strerror()` return pointers to **static/internal
 * memory** managed by the C library. Do NOT free these.
 *
 * ```javascript
 * let getenv = ffi.C.wrap('char *getenv(char *)');
 *
 * let path_ptr = getenv('PATH');
 * let path = ffi.string(path_ptr);  // Copies to ucode string
 *
 * // path_ptr points to C internal memory - DO NOT free
 * // path is a ucode string - managed by ucode GC
 * ```
 *
 * #### Pattern 2: Caller Must Free (malloc'd Memory)
 *
 * Functions like `strdup()`, `asprintf()`, `getline()` return **malloc'd memory**
 * that you must free to avoid leaks.
 *
 * ```javascript
 * let strdup = ffi.C.wrap('char *strdup(const char *)');
 * let free = ffi.C.wrap('void free(void *)');
 *
 * let ptr = strdup("hello");      // malloc'd by strdup
 * let str = ffi.string(ptr);      // Copies to ucode string
 * free(ptr);                       // NOW you can safely free
 *
 * // str is safe - it's a ucode string copy
 * // ptr memory is freed - no leak
 * ```
 *
 * **Key**: Keep the cdata pointer until you're done copying, then free it.
 *
 * #### Pattern 3: Stack-Allocated Buffers
 *
 * When C writes into a buffer you provide (e.g., `sprintf`), the buffer is
 * managed by ucode.
 *
 * ```javascript
 * let sprintf = ffi.C.wrap('int sprintf(char *, const char *, ...)');
 *
 * let buf = ffi.ctype('char[256]');  // ucode-managed array
 * sprintf(buf, "Hello %s", "World");
 *
 * let msg = ffi.string(buf);  // Copies to ucode string
 *
 * // buf is managed by ucode GC - no manual free needed
 * ```
 *
 * ### Substring Operations with slice()
 *
 * For char* pointers, `slice()` supports substring extraction:
 *
 * ```javascript
 * let getenv = ffi.C.wrap('char *getenv(char *)');
 * let ptr = getenv('PATH');
 *
 * // From start to end (same as ffi.string())
 * let full = ptr.slice();
 *
 * // From start index to end
 * let rest = ptr.slice(5);
 *
 * // Specific range
 * let part = ptr.slice(0, 10);
 *
 * // Negative indices (from end)
 * let last = ptr.slice(-5);
 * ```
 *
 * ### Common Functions Reference
 *
 * | Function | Memory Owner | Pattern |
 * |----------|--------------|---------|
 * | `getenv()` | C (static) | No free needed |
 * | `strerror()` | C (static) | No free needed |
 * | `strdup()` | Caller | Must `free()` |
 * | `asprintf()` | Caller | Must `free()` |
 * | `getline()` | Caller | Must `free()` |
 * | `sprintf()` | Caller (buffer) | Buffer managed by you |
 * | `strtok()` | C (static) | No free needed |
 *
 * ### Best Practices
 *
 * 1. **Always use `ffi.string()` or `slice()`** when you need a ucode string from `char*`
 * 2. **Track ownership**: Does C manage the memory or do you?
 * 3. **Free after copying**: Call `free(ptr)` only after `ffi.string(ptr)` or `ptr.slice()`
 * 4. **Never free static memory**: `getenv()`, `strerror()` return static pointers
 *
 * ## Limitations
 *
 * - **No vararg closures**: closures (automatic or via `closure()`) cannot be
 *   created for function types with variable arguments
 * - **Fixed ABI**: Calling convention determined at closure creation time
 * - **Platform constraints**: Some architectures have limited support for certain type combinations
 *
 * ## The `ffi.C` Namespace
 *
 * `ffi.C` is a special CLib instance representing the process's global symbol table.
 * It provides access to standard C library functions without explicit `dlopen()`:
 *
 * ```javascript
 * // These are equivalent:
 * let strlen1 = ffi.C.wrap('size_t strlen(const char *)');
 *
 * ffi.cdef('size_t strlen(const char *);');
 * let strlen2 = ffi.C.wrap('strlen');
 * ```
 *
 * Functions declared via `cdef()` are automatically registered in `ffi.C`'s symbol table.
 *
 * ## Pointer Arithmetic and Memory Access
 *
 * C data objects (cdata) provide methods for pointer arithmetic and memory access:
 *
 * ### Creating Pointers with ptr()
 *
 * Use `ptr()` to get a pointer to a cdata value:
 *
 * ```javascript
 * let x = ffi.ctype('int', 42);
 * let px = x.ptr();  // int* pointer to x
 *
 * // Pass to C functions expecting pointers
 * ffi.cdef('int atoi(const char *)');
 * let num = ffi.ctype('char[4]', "123");
 * let result = atoi(num.ptr());  // => 123
 * ```
 *
 * ### Array Indexing with get() and set()
 *
 * Access array elements using `get(index)` and `set(index, value)`:
 *
 * ```javascript
 * let arr = ffi.ctype('int[5]', [10, 20, 30, 40, 50]);
 *
 * // Read elements
 * let first = arr.get(0);  // => 10 (ucode number)
 * let third = arr.get(2);  // => 30 (ucode number)
 *
 * // Modify elements
 * arr.set(0, 100);
 * arr.set(4, 200);
 *
 * // Negative indices work too
 * let last = arr.get(-1);  // => 200 (ucode number)
 * ```
 *
 * ### Understanding get() vs index()
 *
 * **`get()` returns converted ucode values**, while **`index()` returns
 * raw cdata references**. This is the key distinction between the two methods.
 *
 * #### get() - Converted Values
 *
 * The `get()` method immediately converts C values to ucode types:
 *
 * ```javascript
 * let arr = ffi.ctype('int[5]', [10, 20, 30, 40, 50]);
 *
 * // Returns ucode number directly
 * let val1 = arr.get(0);      // => 10 (number)
 * let val2 = arr.get(2);      // => 30 (number)
 *
 * // Struct field access - returns converted value
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point', 10, 20);
 * p.get('x');      // => 10 (number)
 * p.get('y');      // => 20 (number)
 * ```
 *
 * #### index() - Raw cdata References
 *
 * The `index()` method returns a cdata reference for further manipulation:
 *
 * ```javascript
 * let arr = ffi.ctype('int[5]', [10, 20, 30, 40, 50]);
 *
 * // Returns cdata reference (unconverted)
 * let ref1 = arr.index(0);    // => cdata (int)
 * let ref2 = arr.index(2);    // => cdata (int)
 *
 * // Convert to ucode value explicitly
 * ref1.get();     // => 10 (number)
 *
 * // Or modify through the reference
 * arr.index(0).set(100);  // Set arr[0] = 100
 * ```
 *
 * #### Pointer Arithmetic
 *
 * Both methods work with pointers, but return different types:
 *
 * ```javascript
 * let ptr = ffi.ctype('int *', arr.ptr());
 *
 * // index() returns cdata reference
 * ptr.index(0);   // => cdata at ptr[0]
 * ptr.index(1);   // => cdata at ptr[1]
 * ptr.index(0).get();  // => 10 (number)
 *
 * // get() returns converted value
 * ptr.get(0);     // => 10 (number)
 * ptr.get(1);     // => 20 (number)
 * ```
 *
 * #### Path Syntax Support
 *
 * Both methods support path notation for nested access:
 *
 * ```javascript
 * ffi.cdef('struct rect { struct point min; struct point max; };');
 * let r = ffi.ctype('struct rect', {
 *     min: {x: 0, y: 0},
 *     max: {x: 100, y: 100}
 * });
 *
 * // get() returns converted value
 * r.get('min.x');       // => 0 (number)
 *
 * // index() returns cdata reference
 * r.index('min.x');     // => cdata (int)
 * r.index('min.x').get() // => 0 (number)
 * ```
 *
 * #### Practical Guidance
 *
 * **Use `get()` when:**
 * - You need the value immediately as a ucode type
 * - Reading values for computation: `let x = arr.get(i)`
 * - Accessing struct fields: `let y = struct.get('field')`
 * - Most common use cases
 *
 * **Use `index()` when:**
 * - You need a reference for further manipulation
 * - Chaining operations: `arr.index(i).set(val)`
 * - Pointer arithmetic with cdata: `ptr.index(n).deref()`
 * - Passing references to other C functions
 *
 * **For writing values:**
 * - Use `set()` for both arrays and structs: `arr.set(i, val)`, `struct.set('f', val)`
 *
 * **For getting pointers (not values):**
 * - Use `ptr()` on scalars: `x.ptr()` gives you `int*`
 * - Arrays are already pointers: `arr` can be passed to C functions
 *
 * ### Pointer Arithmetic via get() and index()
 *
 * Both `get(n)` and `index(n)` work for pointer arithmetic on pointer types:
 *
 * ```javascript
 * ffi.cdef('char *strdup(const char *)');
 * let strdup = ffi.C.wrap('char *strdup(const char *)');
 *
 * let ptr = strdup("hello world");
 *
 * // get() returns converted value (number for char)
 * let first_char = ptr.get(0);    // 'h' (number 104)
 * let sixth_char = ptr.get(6);    // 'w' (number 119)
 *
 * // index() returns cdata reference
 * ptr.index(6);       // => cdata (char)
 * ptr.index(6).get()  // => 119 (number)
 *
 * // Get substring from offset
 * let substring = ffi.string(ptr.get(6));  // "world"
 *
 * free(ptr);
 * ```
 *
 * ### Path-Based Access for Nested Structures
 *
 * Use dot notation and array indexing in paths for complex access:
 *
 * ```javascript
 * ffi.cdef(`
 *     struct point { int x; int y; };
 *     struct rect { struct point min; struct point max; };
 * `);
 *
 * let r = ffi.ctype('struct rect', {
 *     min: {x: 0, y: 0},
 *     max: {x: 100, y: 100}
 * });
 *
 * // Nested field access
 * r.get('min.x');    // => 0
 * r.set('max.y', 50);
 *
 * // Array of structs
 * ffi.cdef('struct point points[3];');
 * let arr = ffi.ctype('struct point[3]', [
 *     {x: 1, y: 2},
 *     {x: 3, y: 4},
 *     {x: 5, y: 6}
 * ]);
 *
 * arr.get('[1].x');  // => 3
 * arr.set('[2].y', 10);
 * ```
 *
 * ### Dereferencing Pointers with deref()
 *
 * Use `deref(type)` to read the value pointed to:
 *
 * ```javascript
 * let x = ffi.ctype('int', 42);
 * let px = x.ptr();
 *
 * let value = px.deref('int');  // => 42
 *
 * // With char* pointers
 * ffi.cdef('char *strdup(const char *)');
 * let strdup = ffi.C.wrap('char *strdup(const char *)');
 *
 * let ptr = strdup("hello");
 * let first_byte = ptr.deref('char');  // => 'h' (as number 104)
 *
 * free(ptr);
 * ```
 *
 * ### Querying Array Properties
 *
 * Use `length()` and `itemsize()` for array information:
 *
 * ```javascript
 * let arr = ffi.ctype('int[10]');
 *
 * arr.length();   // => 10 (number of elements)
 * arr.itemsize(); // => 4 (size of each element in bytes)
 *
 * // Calculate total size
 * let total = arr.length() * arr.itemsize();  // => 40 bytes
 * ```
 *
 * ### Working with Byte Arrays
 *
 * For `char[]` or `uint8_t[]`, use `slice()` to extract strings:
 *
 * ```javascript
 * let buf = ffi.ctype('char[10]', "hello");
 *
 * // Extract as ucode string
 * let str = buf.slice();        // => "hello"
 * let part = buf.slice(0, 3);   // => "hel"
 *
 * // Or use ffi.string()
 * let str2 = ffi.string(buf);   // => "hello"
 * ```
 *
 * ### Complete Example: String Manipulation
 *
 * ```javascript
 * ffi.cdef(`
 *     char *strdup(const char *);
 *     void free(void *);
 *     size_t strlen(const char *);
 * `);
 *
 * let strdup = ffi.C.wrap('char *strdup(const char *)');
 * let free = ffi.C.wrap('void free(void *)');
 * let strlen = ffi.C.wrap('size_t strlen(const char *)');
 *
 * // Create a duplicatable string
 * let ptr = strdup("hello world");
 *
 * // Get length
 * let len = strlen(ptr).get();  // => 11
 *
 * // Access individual characters via indexing
 * let first = ptr.get(0);       // 'h'
 * let sixth = ptr.get(6);       // 'w'
 *
 * // Extract substrings
 * let hello = ptr.slice(0, 5);  // "hello"
 * let world = ptr.slice(6);     // "world"
 *
 * // Modify in place
 * ptr.set(5, 0);  // Null-terminate at space
 *
 * let str = ffi.string(ptr);  // => "hello"
 *
 * // Clean up
 * free(ptr);
 * ```
 *
 * @module ffi
 */

#include <syslog.h>
#include <errno.h>
#include <dlfcn.h>

#include <assert.h>
#include <stdio.h>
#include <ffi.h>
#include "ucode/module.h"
#include "ucode/util.h"
#include "ucode/internal/types.h"

#ifdef HAVE_ULOG
#include <libubox/ulog.h>
#endif

#include "uc_cdata.h"
#include "uc_ctype.h"
#include "uc_cparse.h"
#include "uc_cconv.h"

#define CLNS_INDEX ((1u<<CT_FUNC)|(1u<<CT_EXTERN)|(1u<<CT_CONSTVAL))

typedef struct {
	void *dlh;
	char *name;
	uc_value_t *cache;
} uc_ffi_clib_t;

/* A wrapped C function. The value is an "ffi.cfn" resource whose prototype
 * carries a __call__ metamethod; the C symbol pointer and its C type id live
 * in the resource data area. */
typedef struct {
	CTypeID cid;
	void *fp;
} uc_ffi_cfn_t;

typedef struct {
	ffi_cif cif;
	ffi_abi abi;
	size_t nargs;
	ffi_type *rtype;
	ffi_type **atypes;
} uc_ffi_cc_t;




/* Returns the GCcdata for a value that is either an "ffi.ctype" cdata or
 * an "ffi.closure" (function-pointer cdata). NULL if neither. */
static GCcdata *
ucv_cdata_data(uc_value_t *uv)
{
	GCcdata *cd = ucv_resource_data(uv, "ffi.ctype");

	if (!cd)
		cd = ucv_resource_data(uv, "ffi.closure");

	return cd;
}

/* Check first argument for a C type and returns its ID. */
static CTypeID ffi_checkctype(uc_vm_t *vm, size_t nargs, size_t narg, CTState *cts, uc_value_t **param)
{
	uc_value_t *arg = uc_fn_arg(narg);

	if (narg >= nargs) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"C type expected, got no value");

		return 0;
	}

	if (ucv_type(arg) == UC_STRING)
	{ /* Parse an abstract C type declaration. */
		CPState cp = {
			.uv_vm = vm,
			.cts = cts,
			.srcname = ucv_string_get(arg),
			.p = ucv_string_get(arg),
			.uv_param = param,
			.mode = CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT
		};

		if (!uc_cparse(&cp))
			return 0;

		return cp.val.id;
	}
	else
	{
		GCcdata *cd = ucv_resource_data(arg, "ffi.ctype");

		if (!cd) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"C type expected, got %s",
				(narg < nargs) ? ucv_typename(arg) : "no value");

			return 0;
		}

		if (param && param < uc_vector_last(&vm->stack)) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"wrong number of type parameters");

			return 0;
		}
		//cd = cdataV(o);
		return cd->ctypeid == CTID_CTYPEID ? *(CTypeID *)cdataptr(cd) : cd->ctypeid;
	}
}

/* Convert given value to C type. */
static CTypeID
uv_to_ct(uc_vm_t *vm, uint32_t mode, uc_value_t *uv, GCcdata **cdp)
{
	GCcdata *cd = ucv_resource_data(uv, "ffi.ctype");

	if (cd && cd->ctypeid == CTID_CTYPEID) {
		return *(CTypeID *)cdataptr(cd);
	}
	else if (cd) {
		if (cdp)
			*cdp = cd;

		return cd->ctypeid;
	}
	else if (ucv_type(uv) == UC_STRING) {
		/* Parse an abstract C type declaration. */
		CPState cp = {
			.uv_vm = vm,
			.cts = ctype_cts(vm),
			.srcname = ucv_string_get(uv),
			.p = ucv_string_get(uv),
			.mode = mode
		};

		if (!uc_cparse(&cp))
			return 0;

		return cp.val.id;
	}
	else {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"C type or string expected, got %s",
			ucv_typename(uv));

		return 0;
	}
}

/* Convert argument to C pointer. */
static void *ffi_checkptr(uc_vm_t *vm, size_t nargs, size_t narg, CTypeID id)
{
	uc_value_t *arg = uc_fn_arg(narg);
	CTState *cts = ctype_cts(vm);
	void *p;

	if (narg >= nargs) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "value expected");

		return NULL;
	}

	uc_cconv_ct_tv(cts, ctype_get(cts, id), (uint8_t *)&p,
		arg, CCF_ARG(narg), NULL);

	return p;
}

/* Get buffer size from cdata object. Returns SIZE_MAX for non-array types. */
static size_t
ffi_cdata_bufsize(CTState *cts, uc_value_t *uv)
{
	GCcdata *cd = ucv_resource_data(uv, "ffi.ctype");
	CType *ct;

	if (!cd)
		return SIZE_MAX;

	ct = ctype_get(cts, cd->ctypeid);

	if (ctype_isptr(ct->info))
		ct = ctype_rawchild(cts, ct);

	if (ctype_isrefarray(ct->info))
		return ct->size;

	return SIZE_MAX;
}

/* Get redirected or mangled external symbol. */
static uc_value_t *
clib_extsym(CTState *cts, CType *ct, uc_value_t *name)
{
	if (ct->sib) {
		CType *ctf = ctype_get(cts, ct->sib);

		if (ctype_isxattrib(ctf->info, CTA_REDIR))
			return ctf->uv_name;
	}

	return name;
}


/* Number of members a struct declares, including bitfields. */
static size_t
uc_ctype_member_count(CTState *cts, CType *ct)
{
	size_t count = 0;
	CTypeID id;

	for (id = ct->sib; id; id = ctype_get(cts, id)->sib)
		if (ctype_isfield(ctype_get(cts, id)->info) ||
		    ctype_isbitfield(ctype_get(cts, id)->info))
			count++;

	return count;
}

/*
 * A type descriptor either denotes one of the predefined types, which live in
 * libffi itself, or it got synthesized for a struct or union. Only the latter
 * belong to whoever requested them, hence only those may be released.
 *
 * Inspecting the descriptor cannot tell the two apart: FFI_TYPE_VOID has the
 * value 0, so the kind of a descriptor says nothing, and neither does its size,
 * which a structure of two 64 bit integers shares with ffi_type_longdouble on
 * x86-64. Its identity does, and the predefined ones form a closed set.
 *
 * The addresses are not known until link time, so iterating over an array is
 * both smaller and takes fewer relocations than comparing against each entry,
 * which emits one load per entry.
 */
static bool
uc_ffi_type_is_predefined(const ffi_type *st)
{
	static const ffi_type *const predefined[] = {
		&ffi_type_void,
		&ffi_type_uint8,   &ffi_type_sint8,
		&ffi_type_uint16,  &ffi_type_sint16,
		&ffi_type_uint32,  &ffi_type_sint32,
		&ffi_type_uint64,  &ffi_type_sint64,
		&ffi_type_uint,    &ffi_type_sint,
		&ffi_type_float,   &ffi_type_double,
		&ffi_type_longdouble,
		&ffi_type_pointer,

#ifdef HAVE_FFI_COMPLEX_TYPES
		/* guarded by FFI_TARGET_HAS_COMPLEX_TYPE, hence probed for */
		&ffi_type_complex_float,
		&ffi_type_complex_double,
		&ffi_type_complex_longdouble,
#endif
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(predefined); i++)
		if (st == predefined[i])
			return true;

	return false;
}

/* Release a type descriptor synthesized by uc_ctype_to_ffi_type(), including
 * the element vector and the descriptors of nested structures. */
static void
uc_ctype_free_ffi_type(ffi_type *st)
{
	ffi_type **element;

	if (!st || uc_ffi_type_is_predefined(st))
		return;

	for (element = st->elements; *element; element++)
		uc_ctype_free_ffi_type(*element);

	free(st->elements);
	free(st);
}

static ffi_type *
uc_ctype_to_ffi_type(CTState *cts, CTypeID cid, ffi_type *st, ffi_type **stmem)
{
	CType *ct = ctype_get(cts, cid);
	CTInfo info = ct->info;
	CTSize size = ct->size;

	if (stmem)
		*stmem = NULL;

	switch (ctype_type(info)) {
	case CT_NUM:
		if (info & CTF_BOOL)
			return &ffi_type_uint8;

		if (info & CTF_FP) {
			if (size == sizeof(double))
				return &ffi_type_double;

			if (size == sizeof(float))
				return &ffi_type_float;

			return &ffi_type_longdouble;
		}

		switch (size) {
		case 1:
			return (info & CTF_UNSIGNED) ? &ffi_type_uchar : &ffi_type_schar;

		case 2:
			return (info & CTF_UNSIGNED) ? &ffi_type_uint16 : &ffi_type_sint16;

		case 4:
			return (info & CTF_UNSIGNED) ? &ffi_type_uint32 : &ffi_type_sint32;

		case 8:
			return (info & CTF_UNSIGNED) ? &ffi_type_uint64 : &ffi_type_sint64;
		}

		assert(0);
		return NULL;

	case CT_VOID:
		return &ffi_type_void;

	case CT_ENUM:
		switch (ctype_cid(info)) {
		case CTID_INT32:
			return &ffi_type_sint32;

		case CTID_UINT32:
			return &ffi_type_uint32;
		}

		assert(0);
		return NULL;

	case CT_PTR:
		return &ffi_type_pointer;

	case CT_ARRAY:
#ifdef HAVE_FFI_COMPLEX_TYPES
		switch (cid) {
		case CTID_COMPLEX_FLOAT:
			return &ffi_type_complex_float;

		case CTID_COMPLEX_DOUBLE:
			return &ffi_type_complex_double;
		}
#else
		/* The predefined complex number types are unavailable: a backend provides
		 * them only when it defines FFI_TARGET_HAS_COMPLEX_TYPE. They cannot be
		 * described as a struct of two reals instead, since libffi passes such a
		 * description differently than the predefined type, so reject such
		 * declarations rather than pass bogus values. Plain arrays go on being
		 * described by the structure case below. */
		if (ctype_iscomplex(info)) {
			uc_vm_raise_exception(cts->vm, EXCEPTION_TYPE,
				"complex number types are not supported by this libffi build");

			return NULL;
		}
#endif

		/* fall through */

	case CT_STRUCT:
		if (!st) {
			size_t count = uc_ctype_member_count(cts, ct), num = 0, id;
			ffi_type **elements;

			/* A union shares its storage while a libffi element vector describes
			 * consecutive members whose sizes add up, so no descriptor exists
			 * which passes a union the way the ABI passes one. */
			if (!count || (info & CTF_UNION))
				return NULL;

			/* libffi requires a NULL terminated member vector: without one,
			 * initialize_aggregate() cannot derive size and alignment and
			 * ffi_prep_cif() dereferences the missing members. */
			elements = xalloc((count + 1) * sizeof(ffi_type *));

			for (id = ct->sib; id && num < count; id = ctype_get(cts, id)->sib) {
				CType *ctf = ctype_get(cts, id);
				CType *field;

				if (!ctype_isfield(ctf->info) && !ctype_isbitfield(ctf->info))
					continue;

				if (ctype_isbitfield(ctf->info)) {
					/* describe a bitfield by its storage class, so that the
					 * descriptor goes on matching the plain memory layout */
					elements[num++] = (ctype_bitbsz(ctf->info) <= 32)
						? &ffi_type_uint32
						: &ffi_type_uint64;

					continue;
				}

				field = ctype_rawchild(cts, ctf);

				/* a function member is stored as a pointer */
				elements[num++] = ctype_isfunc(field->info)
					? &ffi_type_pointer
					: uc_ctype_to_ffi_type(cts, ctype_typeid(cts, field),
					                       NULL, NULL);

				if (!elements[num - 1]) {
					free(elements);

					return NULL;
				}
			}

			elements[num] = NULL;

			st = xalloc(sizeof(ffi_type));
			st->elements = elements;
		}

		st->type = FFI_TYPE_STRUCT;
		st->size = size;

		/* the alignment stored in a CType is a base 2 logarithm, libffi expects
		 * the plain byte value */
		st->alignment = (uint16_t)(1u << ctype_align(info));

		if (stmem)
			*stmem = st;

		return st;
	}

	return NULL;
}

static uc_value_t *
clib_dlsym(uc_vm_t *vm, uc_ffi_clib_t *lib, uc_value_t *name)
{
	uc_value_t *sym;
	bool exists;

	if (!lib || ucv_type(name) != UC_STRING)
		return NULL;

	sym = ucv_object_get(lib->cache, ucv_string_get(name), &exists);

	if (!exists) {
		CTState *cts = ctype_cts(vm);
		CType *ct;
		CTypeID id = uc_ctype_getname(cts, &ct, name, CLNS_INDEX);

		if (!id) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"missing declaration for symbol '%s'",
				ucv_string_get(name));

			return NULL;
		}

		if (ctype_isconstval(ct->info)) {
			sym = ucv_uint64_new(ct->size);
		}
		else {
			uc_value_t *extname = clib_extsym(cts, ct, name);

			if (!ctype_isfunc(ct->info) && !ctype_isextern(ct->info)) {
				uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					"unexpected ctype %08x for symbol '%s' in clib",
					ct->info, ucv_string_get(name));

				return NULL;
			}

#if UC_TARGET_WINDOWS
			DWORD oldwerr = GetLastError();
#endif
			void *p = dlsym(lib->dlh, ucv_string_get(extname));

#if UC_TARGET_WINDOWS
			SetLastError(oldwerr);
#endif

			if (!p) {
				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
					"cannot resolve symbol '%s': %s",
					ucv_string_get(name),
					dlerror());

				return NULL;
			}

			/* dlsym returns a pointer to the symbol (not the value).
			 * Wrap the symbol's type in a pointer for correct semantics. */
			CTypeID ptr_id = uc_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR) + id, CTSIZE_PTR);

			sym = uc_cdata_new(vm, ptr_id, CTSIZE_PTR);
			*(void **)uc_cdata_dataptr(sym) = p;
		}

		ucv_object_add(lib->cache, ucv_string_get(name), sym);
	}

	return ucv_get(sym);
}

static uc_value_t *
uc_ctype_call(uc_vm_t *vm, size_t nargs);
static uc_value_t *uc_ctype_get(uc_vm_t *vm, size_t nargs);
static uc_value_t *uc_ctype_set(uc_vm_t *vm, size_t nargs);
static uc_value_t *
ct_to_uv(uc_vm_t *vm, CTState *cts, CTypeID cid, void *cdata, size_t size,
         uc_value_t *refs);

/* __get__ / __set__ metamethods for cdata, so that a struct field or array
 * element can be read/written with plain property syntax (p.x, arr[1])
 * instead of the explicit get() / set() methods. The dispatch hands the
 * cdata as `this` and the key (plus value for __set__) as arguments, which
 * is exactly the calling convention get() / set() already use. */

/* __get__: resolve the key like get() does, but yield a live cdata reference
 * for a struct field instead of a snapshot table. A table result would make
 * the __get__ dispatch delegate the lookup into the copy (and lose it),
 * whereas a cdata reference supports further property access (r.min.x) and
 * writes through it (r.min.x = 1) via its own __get__ / __set__. Scalars and
 * array elements yield their converted value, as get() does. */
static uc_value_t *
uc_ctype_meta_get(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");
	CTState *cts = ctype_cts(vm);
	uc_value_t *key = uc_fn_arg(0);
	CTInfo qual = 0;
	uint8_t *p;
	CType *ct;

	if (!cd || !key)
		return NULL;

	ct = uc_cdata_index(cts, cd, key, &p, &qual);

	if (!ct || (qual & 1))
		return NULL;

	ct = ctype_child(cts, ct);

	/* struct and pointer fields yield a live cdata reference so that further
	 * property access (r.min.x, w.p.x) works through the reference's own
	 * __get__ / __set__; scalars and array elements yield their converted
	 * value, as get() does */
	if (ctype_isstruct(ct->info) || ctype_isptr(ct->info))
		return uc_cdata_newref(vm, p, ctype_typeid(cts, ct));

	return ct_to_uv(vm, cts, ctype_typeid(cts, ct), p, ct->size, cd->refs);
}

/* __set__: store the value at the key, exactly as set() does. */
static uc_value_t *
uc_ctype_meta_set(uc_vm_t *vm, size_t nargs)
{
	return uc_ctype_set(vm, nargs);
}

/* Path token types */
typedef enum {
	PATH_TOKEN_FIELD,	/* Field name: "foo" */
	PATH_TOKEN_INDEX	/* Array index: "[0]" */
} path_token_type;

typedef struct {
	path_token_type type;
	union {
		char *field;		/* Allocated field name (for PATH_TOKEN_FIELD) */
		size_t index;		/* For PATH_TOKEN_INDEX */
	};
} path_token;

typedef struct {
	path_token *entries;
	size_t count;
	size_t capacity;
} path_tokens;

/* Free path tokens */
static void
path_tokens_free(path_tokens *tokens)
{
	uc_vector_foreach(tokens, tok)
		if (tok->type == PATH_TOKEN_FIELD)
			free(tok->field);

	uc_vector_clear(tokens);
}

/* Tokenize a path string like "foo.bar[0].baz" or "foo.0.bar" */
static bool
path_tokenize(uc_vm_t *vm, uc_value_t *key_uv, path_tokens *tokens)
{
	if (ucv_type(key_uv) != UC_STRING)
		return false;

	const char *path = ucv_string_get(key_uv);
	size_t len = ucv_string_length(key_uv);

	size_t i = 0;
	while (i < len) {
		/* Skip dots */
		if (path[i] == '.') {
			i++;
			continue;
		}

		/* Check for array index [n] */
		if (path[i] == '[') {
			/* Find closing bracket */
			size_t j = i + 1;
			while (j < len && path[j] != ']')
				j++;

			if (j >= len) {
				uc_vm_raise_exception(vm, EXCEPTION_SYNTAX,
					"Invalid path syntax: missing closing bracket");
				path_tokens_free(tokens);
				return false;
			}

			/* Parse index */
			char *endptr;
			size_t idx = strtoul(path + i + 1, &endptr, 10);
			if (endptr != path + j) {
				uc_vm_raise_exception(vm, EXCEPTION_SYNTAX,
					"Invalid path syntax: invalid array index");
				path_tokens_free(tokens);
				return false;
			}

			/* Add index token */
			uc_vector_push(tokens, (path_token){ .type = PATH_TOKEN_INDEX, .index = idx });

			i = j + 1;
			continue;
		}

		/* Field name */
		size_t j = i;
		while (j < len && path[j] != '.' && path[j] != '[')
			j++;

		if (j > i) {
			/* Add field token */
			size_t field_len = j - i;
			char *field = malloc(field_len + 1);
			if (!field) {
				uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
					"Out of memory");
				path_tokens_free(tokens);
				return false;
			}
			memcpy(field, path + i, field_len);
			field[field_len] = '\0';

			uc_vector_push(tokens, (path_token){ .type = PATH_TOKEN_FIELD, .field = field });
		}

		i = j;
	}

	return tokens->count > 0;
}

/* Navigate through a cdata using path tokens.
 * Returns final CType, updates pointer to final location.
 * Sets *error to true on failure.
 */
static CType *
path_navigate(CTState *cts, GCcdata *start_cd, path_tokens *tokens,
              uint8_t **pptr, CType **pct, bool *error)
{
	*error = false;
	uint8_t *p = cdataptr(start_cd);
	CType *ct = ctype_get(cts, start_cd->ctypeid);

	/* Skip extern and attribute wrappers */
	while (ctype_isextern(ct->info) || ctype_isattrib(ct->info))
		ct = ctype_child(cts, ct);

	/* Handle reference indirection */
	if (ctype_isref(ct->info)) {
		p = *(uint8_t **)p;
		ct = ctype_child(cts, ct);
	}

	uc_vector_foreach(tokens, tok) {
		if (tok->type == PATH_TOKEN_FIELD) {
			/* String key - struct field access */
			if (!ctype_isstruct(ct->info)) {
				*error = true;
				return NULL;
			}

			CTSize ofs;
			CTInfo fqual = 0;
			uc_value_t *field_key = ucv_string_new(tok->field);

			CType *fct = uc_ctype_getfieldq(cts, ct, field_key, &ofs, &fqual);
			ucv_put(field_key);

			if (!fct) {
				*error = true;
				return NULL;
			}

			p += ofs;
			ct = fct;

			/* Get the actual field type */
			ct = ctype_child(cts, ct);

			/* Skip attributes on field */
			while (ctype_isattrib(ct->info))
				ct = ctype_child(cts, ct);
		}
		else {
			/* Integer key - array/pointer access */
			if (!ctype_ispointer(ct->info) && !ctype_isarray(ct->info)) {
				*error = true;
				return NULL;
			}

			CTSize sz = uc_ctype_size(cts, ctype_cid(ct->info));
			if (sz == CTSIZE_INVALID) {
				*error = true;
				return NULL;
			}

			if (ctype_isptr(ct->info))
				p = (uint8_t *)cdata_getptr(p, ct->size);

			/* Check bounds for arrays */
			if (ctype_isarray(ct->info)) {
				CTSize arr_len = ct->size / sz;
				if (tok->index >= arr_len) {
					*error = true;
					return NULL;
				}
			}

			p += tok->index * sz;
			ct = ctype_rawchild(cts, ct);
		}
	}

	*pct = ct;
	*pptr = p;
	return ct;
}

/* __call__ metamethod for wrapped C functions. The instance is an "ffi.cfn"
 * resource whose data area holds the C type id and symbol pointer; the body
 * is identical to the former clib_wrapped_call() cfunction, except the
 * context is read from the resource data instead of being smuggled after the
 * cfunction's name[] flexible array. */
static uc_value_t *
clib_wrapped_call(uc_vm_t *vm, size_t nargs)
{
	uc_ffi_cfn_t *cfn = uc_fn_thisval("ffi.cfn");
	uc_callframe_t *call = uc_vector_last(&vm->callframes);

	uc_value_t *sym = uc_cdata_new(vm, cfn->cid, CTSIZE_PTR);
	*(void **)uc_cdata_dataptr(sym) = cfn->fp;

	uc_value_t *ctx = call->ctx;
	call->ctx = sym;

	uc_value_t *ret = uc_ctype_call(vm, nargs);

		/* Auto-convert primitive return values for convenience */
		if (ret) {
			GCcdata *cd = ucv_resource_data(ret, "ffi.ctype");
			if (cd) {
				CTState *cts = ctype_cts(vm);
				CType *ct = ctype_get(cts, cd->ctypeid);

				if (ct && !ctype_isfunc(ct->info) && !ctype_isptr(ct->info)) {
					/* Primitives: convert to ucode values */
					uc_value_t *converted = ct_to_uv(vm, cts, cd->ctypeid, cdataptr(cd), ct->size, NULL);
					ucv_put(ret);
					ret = converted;
				}
				/* Pointers remain as cdata for explicit control:
				 * - Avoid memory leaks from auto-copying char*
				 * - Allow explicit ffi.string() conversion when needed
				 * - Enable pointer arithmetic and dereferencing
				 */
			}
		}

	ucv_put(call->ctx);
	call->ctx = ctx;

	return ret;
}

/* Create a wrapped C function value: an "ffi.cfn" resource whose data area
 * holds the C type id and symbol pointer. All such values share the type's
 * prototype, which provides the __call__ metamethod, so the value is
 * invocable like a function. */
static uc_value_t *
clib_wrap_new(uc_vm_t *vm, CTypeID cid, void *fp)
{
	uc_resource_type_t *type = ucv_resource_type_lookup(vm, "ffi.cfn");
	uc_value_t *res;
	uc_ffi_cfn_t *cfn;

	if (!type)
		return NULL;

	/* the data area (sized to hold uc_ffi_cfn_t) is filled in via the
	 * out-pointer ucv_resource_new_ex() points at res + 1 */
	res = ucv_resource_new_ex(vm, type, (void **)&cfn, 0, sizeof(*cfn));

	cfn->cid = cid;
	cfn->fp = fp;

	return res;
}


/**
 * Represents a handle to a loaded shared library.
 *
 * @class module:ffi.CLib
 * @hideconstructor
 *
 * @see {@link module:ffi#dlopen|dlopen()}
 *
 * @example
 *
 * const lib = dlopen(…);
 *
 * lib.wrap(…);
 * lib.dlsym(…);
 */

/**
 * Look up a symbol in the loaded library.
 *
 * The `dlsym()` method retrieves a symbol (function, variable, or constant)
 * from the loaded shared library or global symbol table.
 *
 * **Input patterns:**
 *
 * 1. **Bare symbol name**: Look up by symbol name directly. Returns a cdata
 *    pointer for functions/variables, or a number for constants.
 *
 * 2. **Full declaration**: Provide a complete declaration string. The symbol
 *    name is extracted and used for lookup.
 *
 * @function module:ffi.CLib#dlsym
 *
 * @param {string} name
 * The symbol name or full declaration string.
 *
 * @returns {?module:ffi.CData|number}
 * A cdata pointer for functions/variables, a number for constants,
 * or `null` if the symbol cannot be resolved.
 *
 * @throws {Error}
 * Throws an exception if the symbol cannot be found or the declaration
 * syntax is invalid.
 *
 * @example
 * // Pattern 1: Bare symbol name
 * ffi.cdef('extern char **environ;');
 * let env = ffi.C.dlsym('environ');
 * print(env.get(0), "\n");
 *
 * @example
 * // Pattern 2: Full declaration
 * let getenv = ffi.C.dlsym('char *getenv(char *)');
 * print(ffi.string(getenv.deref('char *')));
 *
 * @example
 * // Access constant value (returns number)
 * ffi.cdef('const int INT_MAX;');
 * let max = ffi.C.dlsym('INT_MAX');  // => number
 */
static uc_value_t *
uc_clib_dlsym(uc_vm_t *vm, size_t nargs)
{
	uc_ffi_clib_t *lib = uc_fn_thisval("ffi.clib");
	uc_value_t *arg = uc_fn_arg(0);
	CTState *cts = ctype_cts(vm);

	if (ucv_type(arg) == UC_STRING) {
		const char *s = ucv_string_get(arg);
		if (strpbrk(s, " \t\n\r")) {
			/* Parse the declaration */
			CPState cp = {
				.uv_vm = vm,
				.cts = cts,
				.srcname = s,
				.p = s,
				.uv_param = NULL,
				.mode = CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT
			};

			if (!uc_cparse(&cp)) {
				uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					"failed to parse C declaration: '%s'", s);
				return NULL;
			}

			/* Get the symbol name from the parsed type */
			CType *ct = ctype_raw(cts, cp.val.id);
			if (!ct || !ct->uv_name) {
				uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					"declaration does not define a named symbol");
				return NULL;
			}

			/* Use the symbol name for lookup */
			arg = ct->uv_name;
		} else {
			/* Bare symbol name: first check if there's a declaration */
			CType *ct;
			CTypeID id = uc_ctype_getname(cts, &ct, arg, CLNS_INDEX);
			if (id) {
				/* Declaration exists, use normal clib_dlsym */
				return clib_dlsym(vm, lib, arg);
			} else {
				/* No declaration: direct dlsym returning void* cdata */
				void *p = dlsym(lib->dlh, s);
				if (!p) {
					/* Symbol not found */
					return NULL;
				}
				/* Create a void* cdata */
				CTypeID voidp = CTID_P_VOID;
				uc_value_t *cd = uc_cdata_new(vm, voidp, sizeof(void*));
				void **ptr = (void**)uc_cdata_dataptr(cd);
				*ptr = p;
				return cd;
			}
		}
	}

	return clib_dlsym(vm, lib, arg);
}

static uc_value_t *
uc_clib_resolve_common(CTState *cts, uc_ffi_clib_t *lib, uc_value_t *cdef,
                       CType **ctp, GCcdata **cdp)
{
	const char *spec;
	size_t spec_len, pos;
	GCcdata *cd;
	CType *ct;
	void *fp;

	if (!lib)
		return NULL;

	if (ucv_type(cdef) == UC_STRING) {
		spec = ucv_string_get(cdef);
		spec_len = ucv_string_length(cdef);

		pos = strcspn(spec, " \t\r\n*[{(");

		if (pos != spec_len) {
			CTypeID cid = uv_to_ct(cts->vm, CPARSE_MODE_DIRECT, cdef, NULL);

			if (!cid)
				return NULL;

			CType *ct = ctype_raw(cts, cid);

			uc_value_t *sym_name = ct->uv_name;
			uc_value_t *sym = clib_dlsym(cts->vm, lib, sym_name);

			if (!sym) {
				uc_value_t *repr = uc_ctype_repr(cts->vm, cid, ct->uv_name);

				uc_vm_raise_exception(cts->vm, EXCEPTION_TYPE,
					"unable to resolve symbol '%s' for declaration '%s'",
					ucv_string_get(sym_name), ucv_string_get(repr));

				ucv_put(repr);

				return NULL;
			}

			GCcdata *cd = ucv_resource_data(sym, "ffi.ctype");
			assert(cd);

			/* dlsym returns a pointer to the symbol. Unwrap pointer to get actual type.
			 * For function pointers, this gives us the function type which should match the declaration. */
			CType *sym_ct = ctype_get(cts, cd->ctypeid);
			if (ctype_isptr(sym_ct->info))
				sym_ct = ctype_rawchild(cts, sym_ct);

			CType *decl_ct = ctype_get(cts, cid);

			if (sym_ct != decl_ct) {
				uc_value_t *repr_decl = uc_ctype_repr(cts->vm, cid, ct->uv_name);
				uc_value_t *repr_sym = uc_ctype_repr(cts->vm, cd->ctypeid, NULL);

				uc_vm_raise_exception(cts->vm, EXCEPTION_TYPE,
					"type mismatch between declaration (%s) and resolved symbol (%s)",
					ucv_string_get(repr_decl),
					ucv_string_get(repr_sym));

				ucv_put(repr_decl);
				ucv_put(repr_sym);
				ucv_put(sym);

				return NULL;
			}

			if (ctp)
				*ctp = ct;

			if (cdp)
				*cdp = cd;

			return sym;
		}
		else {
			CType *ct;
			uc_value_t *sym_uv = ucv_string_new(spec);
			CTypeID id = uc_ctype_getname(cts, &ct, sym_uv, CLNS_INDEX);
			ucv_put(sym_uv);

			if (!id) {
				uc_vm_raise_exception(cts->vm, EXCEPTION_TYPE,
					"unknown symbol '%s'", spec);

				return NULL;
			}

			uc_value_t *sym = clib_dlsym(cts->vm, lib, cdef);

			if (!sym) {
				uc_vm_raise_exception(cts->vm, EXCEPTION_TYPE,
					"unable to resolve symbol '%s'", spec);

				return NULL;
			}

			GCcdata *cd = ucv_resource_data(sym, "ffi.ctype");
			assert(cd);

			/* dlsym returns a pointer to the symbol. Unwrap pointer to get actual type. */
			CType *sym_ct = ctype_get(cts, cd->ctypeid);
			if (ctype_isptr(sym_ct->info))
				sym_ct = ctype_rawchild(cts, sym_ct);

			CType *decl_ct = ctype_get(cts, id);

			if (sym_ct != decl_ct) {
				uc_value_t *repr_decl = uc_ctype_repr(cts->vm, id, ct->uv_name);
				uc_value_t *repr_sym = uc_ctype_repr(cts->vm, cd->ctypeid, NULL);

				uc_vm_raise_exception(cts->vm, EXCEPTION_TYPE,
					"type mismatch between declared type '%s' and resolved symbol type '%s'",
					ucv_string_get(repr_decl),
					ucv_string_get(repr_sym));

				ucv_put(repr_decl);
				ucv_put(repr_sym);
				ucv_put(sym);

				return NULL;
			}

			if (ctp)
				*ctp = ct;

			if (cdp)
				*cdp = cd;

			return sym;
		}
	}

	cd = ucv_resource_data(cdef, "ffi.ctype");
	if (!cd)
		return NULL;

	/* Handle ctype resources containing function pointers */
	uc_vm_t *vm = cts->vm;

	ct = ctype_get(cts, cd->ctypeid);

	if (!ct)
		return NULL;

	/* Unwrap pointer types to get to the actual function type */
	if (ctype_isptr(ct->info)) {
		ct = ctype_rawchild(cts, ct);
	}

	if (!ct || !ctype_isfunc(ct->info)) {
		uc_value_t *repr = uc_ctype_repr(vm, cd->ctypeid, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"attempt to wrap non-function cdata type '%s'",
			ucv_string_get(repr));

		ucv_put(repr);

		return NULL;
	}

	/* Extract the function pointer from the cdata */
	fp = *(void **)cdataptr(cd);

	if (!fp) {
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
			"attempt to wrap NULL function pointer");

		return NULL;
	}

	if (ctp)
		*ctp = ct;

	if (cdp)
		*cdp = cd;

	/* Create a temporary cdata to hold the function pointer for the caller */
	uc_value_t *sym = uc_cdata_new(vm, ctype_typeid(cts, ct), CTSIZE_PTR);
	*(void **)uc_cdata_dataptr(sym) = fp;

	return sym;
}

/**
 * Resolve a symbol to a cdata pointer.
 *
 * The `resolve()` method retrieves a symbol from the loaded library and
 * returns a cdata pointer. Unlike `wrap()`, it does not create a callable
 * wrapper - it returns the raw pointer for manual handling.
 *
 * @function module:ffi.CLib#resolve
 *
 * @param {string|module:ffi.CData} decl
 * The function declaration or cdata function pointer.
 *
 * @returns {?module:ffi.CData}
 * A cdata pointer to the symbol, or `null` if resolution fails.
 *
 * @throws {Error}
 * Throws an exception if the symbol cannot be resolved.
 *
 * @example
 * // Resolve function pointer
 * ffi.cdef('int strcmp(const char *, const char *)');
 * let ptr = ffi.C.resolve('strcmp');
 * // ptr is a cdata, not callable directly
 */
static uc_value_t *
uc_clib_resolve(uc_vm_t *vm, size_t nargs)
{
	uc_ffi_clib_t *this = uc_fn_thisval("ffi.clib");
	uc_value_t *cdef = uc_fn_arg(0);
	CTState *cts = ctype_cts(vm);

	return uc_clib_resolve_common(cts, this, cdef, NULL, NULL);
}

/**
 * Wrap a C function symbol into a callable ucode function.
 *
 * The `wrap()` method retrieves a function symbol from the library and returns
 * a callable wrapper that handles argument marshaling and function invocation
 * via libffi.
 *
 * **Input patterns:**
 *
 * 1. **Full declaration**: Provide a complete function declaration string.
 *    The symbol name is extracted automatically.
 *
 * 2. **Bare symbol**: Provide just the symbol name. Requires that the type
 *    was previously declared via `cdef()`.
 *
 * 3. **cdata pointer**: Provide a cdata containing a function pointer
 *    (e.g., from `dlsym()`). The type must match the cdata's type.
 *
 * @function module:ffi.CLib#wrap
 *
 * @param {string|module:ffi.CData} decl
 * The function declaration string, bare symbol name, or cdata function pointer.
 *
 * @returns {?function}
 * A callable function wrapper, or `null` if resolution fails.
 *
 * @throws {Error}
 * Throws an exception if the symbol cannot be resolved or is not a function.
 *
 * @example
 * // Pattern 1: Full declaration (no cdef needed)
 * let strcmp = ffi.C.wrap('int strcmp(const char *, const char *)');
 * print(strcmp("hello", "world"));  // => number (auto-converted)
 *
 * @example
 * // Pattern 2: Bare symbol (requires cdef)
 * ffi.cdef('int strcmp(const char *, const char *)');
 * let strcmp = ffi.C.wrap('strcmp');
 * print(strcmp("hello", "world"));  // => number (auto-converted)
 *
 * @example
 * // Pattern 3: cdata function pointer
 * ffi.cdef('size_t strlen(const char *)');
 * let strlen_sym = ffi.C.dlsym('strlen');
 * let strlen_fn = ffi.C.wrap(strlen_sym);
 * print(strlen_fn("hello"));  // => number (auto-converted)
 *
 * @example
 * // Pointer returns remain as cdata for explicit control
 * let getenv = ffi.C.wrap('char *getenv(char *)');
 * let path_ptr = getenv('PATH');  // => cdata (char*)
 * let path = ffi.string(path_ptr);  // Convert to ucode string
 */
static uc_value_t *
uc_clib_wrap(uc_vm_t *vm, size_t nargs)
{
	uc_ffi_clib_t *this = uc_fn_thisval("ffi.clib");
	uc_value_t *cdef = uc_fn_arg(0);
	CTState *cts = ctype_cts(vm);
	GCcdata *cd;
	CType *ct;

	uc_value_t *sym = uc_clib_resolve_common(cts, this, cdef, &ct, &cd);

	if (!sym)
		return NULL;

	CTypeID cid = ctype_typeid(cts, ct);

	if (ctype_isptr(ct->info))
		ct = ctype_rawchild(cts, ct);

	if (!ct || !ctype_isfunc(ct->info)) {
		uc_value_t *repr_sym = uc_ctype_repr(vm, cd->ctypeid, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"attempt to wrap non-function value type '%s'",
			ucv_string_get(repr_sym));

		ucv_put(repr_sym);
		ucv_put(sym);

		return NULL;
	}

	void *fp = *(void **)uc_cdata_dataptr(sym);

	ucv_put(sym);

	return clib_wrap_new(vm, cid, fp);
}


typedef struct {
	ffi_closure closure;
	ffi_cif cif;
	void *codeloc;
	uc_vm_t *vm;
	uc_value_t *func;
	CType *ct;
	/* Return type descriptor, when one had to be synthesized for it. Nothing
	 * else refers to it, so the context retains it, along the descriptors it
	 * synthesized for its arguments. Those denoting a predefined descriptor are
	 * left alone, see uc_ffi_type_is_predefined(). */
	ffi_type *rtype;
	ffi_type *argtypes[];
} uc_closure_context_t;

/* Releases a closure context along the type descriptors it retained. */
static void
uc_closure_context_free(uc_closure_context_t *context)
{
	size_t i;

	uc_ctype_free_ffi_type(context->rtype);
	context->rtype = NULL;

	/* A closure is invoked long after the call which created it, so the
	 * descriptors synthesized for its arguments cannot be released with that
	 * call and are retained here instead. */
	for (i = 0; i < context->cif.nargs; i++)
		uc_ctype_free_ffi_type(context->argtypes[i]);

	ffi_closure_free(context);
}

static void
uc_ctype_closure_cb(ffi_cif *cif, void *ret, void *args[], void *ud)
{
	uc_value_t *uv_arg, *uv_ret = ucv_uint64_new(0);
	uc_closure_context_t *context = ud;
	uc_exception_type_t ex;

	CTState *cts = ctype_cts(context->vm);
	CType *ct_arg, *ct_ret;
	CTypeID id_arg;

	uc_vm_stack_push(context->vm, ucv_get(context->func));

	/* skip attribute entries */
	for (id_arg = context->ct->sib;
	     id_arg && ctype_isattrib(ctype_get(cts, id_arg)->info);
	     id_arg = ctype_get(cts, id_arg)->sib)
		;

	for (size_t i = 0; i < cif->nargs; i++) {
		uv_arg = NULL;

		assert(id_arg);
		ct_arg = ctype_get(cts, id_arg);

		assert(ctype_isfield(ct_arg->info));
		id_arg = ct_arg->sib;

		uc_cconv_tv_ct(cts, ctype_raw(cts, ctype_cid(ct_arg->info)),
			ctype_cid(ct_arg->info), &uv_arg, args[i]);

		uc_vm_stack_push(context->vm, uv_arg);
	}

	ex = uc_vm_call(context->vm, false, cif->nargs);

	if (ex == EXCEPTION_NONE)
		uv_ret = uc_vm_stack_pop(context->vm);

	ct_ret = ctype_get(cts, ctype_cid(context->ct->info));

	// FIXME: ret value ref
	uc_cconv_ct_init(cts, ct_ret, ct_ret->size, ret, &uv_ret, 1, NULL);
	ucv_put(uv_ret);
}

static uc_closure_context_t *
ct_to_closure(uc_vm_t *vm, CTState *cts, CType *ct, uc_value_t *func)
{
	ffi_type **argument_type, *atype, *rtype;
	uc_closure_context_t *context;
	ffi_abi abi = FFI_DEFAULT_ABI;
	size_t context_size;
	CTypeID cid_arg;
	ffi_status st;
	void *codeloc;

	if (!ucv_is_callable(func)) {
		char *repr = ucv_to_string(vm, func);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"attempt to bind non-function value '%s'",
			repr ? repr : "null");

		free(repr);

		return NULL;
	}

	/* resolve function type */
	if (ct && ctype_isptr(ct->info))
		ct = ctype_rawchild(cts, ct);

	if (!ct || !ctype_isfunc(ct->info)) {
		uc_value_t *repr = uc_ctype_repr(vm, ctype_typeid(cts, ct), NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"attempt to wrap non-function C type '%s'",
			repr ? ucv_string_get(repr) : "NULL");

		ucv_put(repr);

		return NULL;
	}

	/* can't wrap variadic functions */
	if (ct->info & CTF_VARARG) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"wrapping variadic C function types is not supported");

		return NULL;
	}

	/* skip attribute entries */
	for (cid_arg = ct->sib;
	     cid_arg && ctype_isattrib(ctype_get(cts, cid_arg)->info);
	     cid_arg = ctype_get(cts, cid_arg)->sib)
		;

	/* Compute required size & allocate storage for closure context. Only the
	 * vector of argument type pointers is stored inline; the descriptors belong
	 * to uc_ctype_to_ffi_type(), which allocates them. Letting it fill in place
	 * storage handed to it would have a structure describe itself with the size
	 * and alignment a previously converted type left behind in that slot. */
	context_size = sizeof(*context) + (ct->size ? ct->size : 1) * sizeof(ffi_type *);

	context = ffi_closure_alloc(context_size, &codeloc);

	if (!context) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
			"unable to allocate FFI closure context");

		return NULL;
	}

	context->codeloc = codeloc;
	context->func = ucv_get(func);
	context->vm = vm;
	context->ct = ct;

	argument_type = (ffi_type **)context->argtypes;
	context->rtype = NULL;

	/* select ABI */
#ifdef X86
	switch (ctype_cconv(ct->info)) {
	case CTCC_FASTCALL: abi = FFI_FASTCALL; break;
	case CTCC_THISCALL: abi = FFI_THISCALL; break;
	case CTCC_STDCALL:  abi = FFI_STDCALL;  break;
	case CTCC_CDECL:    abi = FFI_MS_CDECL; break;
	}
#endif

	if (ctype_isvector(ct->info)) {
#if defined(X86) || defined(X86_WIN32) || defined(X86_WIN64)
		if (ct->size != 8 && ct->size != 16) {
			uc_value_t *repr = uc_ctype_repr(vm, ctype_cid(ct->info), NULL);

			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
				"vector return type '%s' is not supported",
				ucv_string_get(repr));

			ucv_put(repr);

			return NULL;
		}
#endif
	}

	rtype = uc_ctype_to_ffi_type(cts, ctype_cid(ct->info), NULL, &context->rtype);

	if (!rtype) {
		uc_value_t *repr = uc_ctype_repr(vm, ctype_cid(ct->info), NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"don't know how to handle return type '%s'",
			ucv_string_get(repr));

		ucv_put(repr);

		goto out;
	}

	for (size_t i = 0; i < ct->size; i++) {
		assert(cid_arg);

		CType *ct_arg = ctype_get(cts, cid_arg);

		assert(ctype_isfield(ct_arg->info));

		cid_arg = ct_arg->sib;
		atype = uc_ctype_to_ffi_type(cts, ctype_cid(ct_arg->info), NULL, NULL);

		if (!atype) {
			uc_value_t *repr = uc_ctype_repr(vm, ctype_cid(ct->info), NULL);

			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"don't know how to handle argument type '%s'",
				ucv_string_get(repr));

			ucv_put(repr);

			goto out;
		}

		*(argument_type++) = atype;
	}

	st = ffi_prep_cif(&context->cif, abi, ct->size, rtype, context->argtypes);

	if (st == FFI_OK) {
		st = ffi_prep_closure_loc(&context->closure, &context->cif,
		                          uc_ctype_closure_cb, context,
		                          context->codeloc);
	}

	switch (st) {
	case FFI_BAD_TYPEDEF:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "invalid FFI type");
		goto out;

	case FFI_BAD_ABI:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "invalid FFI ABI");
		goto out;

#ifdef HAVE_FFI_BAD_ARGTYPE
	case FFI_BAD_ARGTYPE:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "invalid variadic argument type");
		goto out;
#endif

	case FFI_OK:
		return context;
	}

out:
	uc_closure_context_free(context);

	return NULL;
}

/* Destructor for "ffi.closure" resources (C closures bound to a ucode
 * function). The data area holds the cdata header (function pointer type)
 * followed by the libffi closure context pointer; value slot 0 retains
 * the bound ucode function as long as the resource is reachable.
 * The destructor frees the closure context. */
static void
close_closure(void *ud)
{
	GCcdata *cd = (GCcdata *)ud;
	uc_closure_context_t *ctx = *(uc_closure_context_t **)(cd + 1);

	if (ctx)
		uc_closure_context_free(ctx);
}

/* --- ffi.closure methods ------------------------------------------------ */
/*
 * The closure resource has the same GCcdata header as a plain cdata, but
 * with a trailing uc_closure_context_t * in the data area. These methods
 * extract the GCcdata and perform the same operations as their ffi.ctype
 * counterparts, minus the ones that don't make sense on a function pointer
 * (deref, get, set, index, slice, copy, fill, etc.).
 */

static uc_value_t *
uc_closure_ptr(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this_arg = _uc_fn_this_res(vm);
	GCcdata *cd = ucv_resource_data(this_arg, "ffi.closure");

	if (!cd)
		return NULL;

	uc_value_t *pres = uc_cdata_new(vm, CTID_P_VOID, CTSIZE_PTR);
	*(void **)uc_cdata_dataptr(pres) = cdataptr(cd);

	return pres;
}

static uc_value_t *
uc_closure_tostring(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this_arg = _uc_fn_this_res(vm);
	GCcdata *cd = ucv_resource_data(this_arg, "ffi.closure");

	if (!cd)
		return NULL;

	uc_value_t *type_repr = uc_ctype_repr(vm, cd->ctypeid, NULL);
	uc_stringbuf_t *sb = ucv_stringbuf_new();

	ucv_stringbuf_printf(sb, "%s %p",
		ucv_string_get(type_repr), *(void **)cdataptr(cd));

	ucv_put(type_repr);

	return ucv_stringbuf_finish(sb);
}

static uc_value_t *
uc_closure_cast(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this_arg = _uc_fn_this_res(vm);
	GCcdata *cd = ucv_resource_data(this_arg, "ffi.closure");

	if (!cd)
		return NULL;

	CTState *cts = ctype_cts(vm);
	CTypeID id = ffi_checkctype(vm, nargs, 0, cts, NULL);
	CType *d = ctype_raw(cts, id);

	if (!ctype_isptr(d->info)) {
		uc_value_t *repr = uc_ctype_repr(vm, id, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
		                      "invalid cast to type '%s', can only cast to pointer types",
		                      repr ? ucv_string_get(repr) : "NULL");

		ucv_put(repr);

		return NULL;
	}

	if (cd->ctypeid == id)
		return ucv_get(this_arg);

	/* create a new plain cdata with the target type, copying the pointer value */
	uc_value_t *res = uc_cdata_new(vm, id, d->size);
	*(void **)uc_cdata_dataptr(res) = *(void **)cdataptr(cd);

	return res;
}

/* Create a C closure bound to a ucode function: allocate a libffi closure
 * for the given function type and return a function-pointer cdata pointing
 * at the closure code.
 *
 * The result is an "ffi.closure" resource (a cdata-like value): its data
 * area holds the closure code address and the closure context pointer, and
 * a value slot retains the bound ucode function. The C callback is valid
 * for as long as the resource is reachable; dropping all references
 * releases the closure via its destructor.
 *
 * @function module:ffi#closure
 *
 * @param {string|module:ffi.CData} type
 * A C function type or function pointer type declaration, e.g.
 * `'int (*)(char *, void *)'`. The type must not be variadic.
 *
 * @param {function} func
 * The ucode function to bind to the C closure.
 *
 * @returns {?module:ffi.CData}
 * A function-pointer cdata bound to the ucode function, or `null` if the
 * type or function is invalid.
 *
 * @throws {Error}
 * Throws an exception if the type is not a function type or the value is
 * not callable.
 *
 * @example
 * // Create a closure for a comparison function and pass it to qsort
 * let qsort = ffi.C.wrap('void qsort(void *, size_t, size_t, int (*)(const void *, const void *))');
 * let arr = ffi.ctype('int[5]', [56, 4, 12, 1, 5]);
 * let cmp = ffi.closure('int (*)(const void *, const void *)',
 *                     (a, b) => a.deref('int') - b.deref('int'));
 * qsort(arr.ptr(), arr.length(), arr.itemsize(), cmp);
 */
static uc_value_t *
uc_ffi_closure(uc_vm_t *vm, size_t nargs)
{
	CTState *cts = ctype_cts(vm);
	uc_value_t *func = uc_fn_arg(1), *res;
	CTypeID cid;
	CType *ct;
	uc_closure_context_t *ctx;

	cid = ffi_checkctype(vm, nargs, 0, cts, NULL);

	if (!cid || !func)
		return NULL;

	ct = ctype_raw(cts, cid);

	/* accept pointers to function types too, e.g. 'int (*)(int, char *)' */
	if (ctype_isptr(ct->info))
		ct = ctype_rawchild(cts, ct);

	if (!ct || !ctype_isfunc(ct->info)) {
		uc_value_t *repr = uc_ctype_repr(vm, cid, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"attempt to wrap non-function C type '%s'",
			repr ? ucv_string_get(repr) : "NULL");

		ucv_put(repr);

		return NULL;
	}

	ctx = ct_to_closure(vm, cts, ct, func);

	if (!ctx)
		return NULL;

	/* create the "ffi.closure" resource: the data area holds the cdata
	 * context (pointer type) followed by a pointer to the libffi closure
	 * context, and a value slot retains the bound ucode function. The
	 * destructor (close_closure) frees the libffi closure. */
	GCcdata *cd;

	res = ucv_resource_create_ex(vm, "ffi.closure", (void **)&cd, 1,
	                             sizeof(GCcdata) + sizeof(void *));

	if (!res)
		return NULL;

	/* use the original type id (which is the function pointer type,
	 * e.g. 'int (*)(...)') — not a new pointer-to-pointer */
	cd->ctypeid = ctype_check(cts, cid);
	cd->isvla = 0;
	cd->refs = NULL;

	*(void **)(cd + 1) = ctx;

	/* retain the bound ucode function for the lifetime of the resource */
	ucv_resource_value_set(res, 0, ucv_get(func));

	return res;
}

static uc_value_t *
uc_ctype_call(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");
	CTState *cts = ctype_cts(vm);
	CType *ct = cd ? ctype_get(cts, cd->ctypeid) : NULL;
	CTSize sz = CTSIZE_PTR;

	if (ct && ctype_isptr(ct->info)) {
		sz = ct->size;
		ct = ctype_rawchild(cts, ct);
	}

	if (!ct || !ctype_isfunc(ct->info)) {
		uc_value_t *repr = cd ? uc_ctype_repr(vm, cd->ctypeid, NULL) : NULL;

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"attempt to call non-function value type '%s'",
			repr ? ucv_string_get(repr) : "NULL");

		ucv_put(repr);

		return NULL;
	}

	ffi_cif cif;
	ffi_abi abi = FFI_DEFAULT_ABI;
	ffi_type *rtype = &ffi_type_void, *stype = NULL;

	/* select ABI */
#ifdef X86
	switch (ctype_cconv(ct->info)) {
	case CTCC_FASTCALL: abi = FFI_FASTCALL; break;
	case CTCC_THISCALL: abi = FFI_THISCALL; break;
	case CTCC_STDCALL:  abi = FFI_STDCALL;  break;
	case CTCC_CDECL:    abi = FFI_MS_CDECL; break;
	}
#endif

	CType *ct_ret = ct; //ctype_child(cts, ct);

	if (ctype_isvector(ct_ret->info)) {
#if defined(X86) || defined(X86_WIN32) || defined(X86_WIN64)
		if (ct_ret->size != 8 && ct_ret->size != 16) {
			uc_value_t *repr = uc_ctype_repr(vm, ctype_cid(ct_ret->info), NULL);

			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
				"vector return type '%s' is not supported",
				ucv_string_get(repr));

			ucv_put(repr);

			return NULL;
		}
#endif
	}

	rtype = uc_ctype_to_ffi_type(cts, ctype_cid(ct_ret->info), NULL, &stype);

	if (!rtype) {
		uc_value_t *repr = uc_ctype_repr(vm, ctype_cid(ct_ret->info), NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"don't know how to handle return type '%s'",
			ucv_string_get(repr));

		ucv_put(repr);

		return NULL;
	}

	/* skip attribute entries */
	CTypeID fid = ct->sib;

	while (fid) {
		CType *ctf = ctype_get(cts, fid);

		if (!ctype_isattrib(ctf->info))
			break;

		fid = ctf->sib;
	}

	struct {
		size_t count;
		ffi_type **entries;
		size_t capacity;
	} argtypes = { 0 };

	struct {
		size_t count;
		void **entries;
		size_t capacity;
	} argvalues = { 0 };

	struct {
		size_t count;
		void **entries;
		size_t capacity;
	} argmem = { 0 };

	uc_value_t *rv = NULL;
	size_t nfixedargs = 0;

	/* Count fixed arguments from declaration (before ...) */
	CTypeID temp_fid = ct->sib;
	while (temp_fid) {
		CType *ctf = ctype_get(cts, temp_fid);
		if (!ctype_isattrib(ctf->info))
			nfixedargs++;
		temp_fid = ctf->sib;
	}

	for (size_t i = 0; i < nargs; i++) {
		CTypeID did;
		bool is_vararg = false;

		if (fid) {
			CType *ctf = ctype_get(cts, fid);

			assert(ctype_isfield(ctf->info));

			fid = ctf->sib;
			did = ctype_cid(ctf->info);
		}
		else if (ct->info & CTF_VARARG) {
			is_vararg = true;
			/* For variadic args, infer type from ucode value */
			uc_value_t **argp = &vm->stack.entries[vm->stack.count - nargs + i];
			GCcdata *arg_cd = ucv_cdata_data(*argp);

			if (arg_cd && arg_cd->ctypeid != CTID_CTYPEID) {
				/* cdata argument: use its type directly */
				did = arg_cd->ctypeid;
			}
			else if (ucv_type(*argp) == UC_STRING) {
				/* string -> char* */
				did = CTID_P_CCHAR;
			}
			else if (ucv_type(*argp) == UC_INTEGER) {
				/* integer -> int (promoted from smaller types) */
				did = CTID_INT32;
			}
			else if (ucv_type(*argp) == UC_DOUBLE) {
				/* double stays double (float would be promoted) */
				did = CTID_DOUBLE;
			}
			else if (ucv_is_callable(*argp)) {
				/* A function passed to a variadic argument cannot be bound to a
				 * C function pointer: the C signature is unknown, so no FFI
				 * closure can be generated. (Inferred as void* below it would
				 * fail in ct_to_closure() and crash on a NULL context.) */
				uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					"cannot pass a function as a variadic argument: "
					"the C function pointer type cannot be inferred");
				goto out;
			}
			else {
				uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					"unsupported variadic argument type %s",
					ucv_typename(*argp));
				goto out;
			}
		}
		else {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"too many arguments for called function");

			goto out;
		}

		CType *d = ctype_raw(cts, did);
		CTSize sz = d->size;
		ffi_type *atype = uc_ctype_to_ffi_type(cts, did, NULL, NULL);

		/* Apply default argument promotions for variadic arguments */
		if (is_vararg && atype) {
			/* Float promotes to double */
			if (atype == &ffi_type_float) {
				atype = &ffi_type_double;
				sz = sizeof(double);
				did = CTID_DOUBLE;
				d = ctype_get(cts, did);
			}
			/* Small integers promote to int */
			else if (atype == &ffi_type_schar || atype == &ffi_type_uchar ||
			         atype == &ffi_type_sint16 || atype == &ffi_type_uint16) {
#if UC_SIZEOF_INT == 4
				atype = (atype == &ffi_type_uchar || atype == &ffi_type_uint16)
				        ? &ffi_type_uint : &ffi_type_sint;
				sz = sizeof(int);
				did = (atype == &ffi_type_uint) ? CTID_UINT32 : CTID_INT32;
#else
				atype = (atype == &ffi_type_uchar || atype == &ffi_type_uint16)
				        ? &ffi_type_uint64 : &ffi_type_sint64;
				sz = sizeof(int64_t);
				did = (atype == &ffi_type_uint64) ? CTID_UINT64 : CTID_INT64;
#endif
				d = ctype_get(cts, did);
			}
		}

		if (!atype) {
			uc_value_t *repr = uc_ctype_repr(vm, ctype_cid(ct_ret->info), NULL);

			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"don't know how to handle argument type '%s'",
				ucv_string_get(repr));

			ucv_put(repr);

			goto out;
		}

		uc_value_t **argp = &vm->stack.entries[vm->stack.count - nargs + i];
		GCcdata *arg_cd = ucv_resource_data(*argp, "ffi.ctype");

		if (arg_cd && arg_cd->ctypeid != CTID_CTYPEID) {
			/* Check if this is an array cdata - if so, wrap pointer in pointer-sized slot */
			CType *arg_ct = ctype_get(cts, arg_cd->ctypeid);
			if (ctype_isarray(arg_ct->info)) {
				void *memp, *valp;
				memp = valp = xalloc(sizeof(void *));
				*(void **)valp = cdataptr(arg_cd);
				uc_vector_push(&argmem, memp);
				uc_vector_push(&argvalues, valp);
			}
			else {
				uc_vector_push(&argvalues, cdataptr(arg_cd));
			}
		}
		else if (ctype_isptr(d->info) && ucv_type(*argp) == UC_OBJECT) {
			CType *child = ctype_rawchild(cts, d);
			if (ctype_isstruct(child->info)) {
				void *struct_mem = xalloc(child->size);
				uc_cconv_ct_init(cts, child, child->size, struct_mem, argp, 1, NULL);
				void *ptr_mem = xalloc(sizeof(void*));
				*(void**)ptr_mem = struct_mem;
				uc_vector_push(&argmem, struct_mem);
				uc_vector_push(&argmem, ptr_mem);
				uc_vector_push(&argvalues, ptr_mem);
			}
			else {
				void *memp, *valp;
				memp = valp = xalloc(sz);
				uc_cconv_ct_tv(cts, d, valp, *argp, CCF_ARG(i), NULL);
				uc_vector_push(&argmem, memp);
				uc_vector_push(&argvalues, valp);
			}
		}
		else {
			void *memp, *valp;
			GCcdata *cd;

			if (ucv_type(*argp) == UC_STRING) {
				memp = valp = xalloc(sz);
				*(char **)valp = ucv_string_get(*argp);
			}
			else if (ucv_is_callable(*argp)) {
				uc_closure_context_t *cc = ct_to_closure(vm, cts, d, *argp);

				/* ct_to_closure() raises the specific error and returns NULL on
				 * failure; abort the call rather than dereference a NULL context.
				 *
				 * The check is hidden from the static analyzer: it cannot track the
				 * heap-allocated `atype` through the argtypes vector to the `out:`
				 * cleanup, and reports a false positive leak on this early exit path. */
				#ifndef __clang_analyzer__
				if (!cc)
					goto out;
				#endif

				memp = (void *)((uintptr_t)cc | 1u);
				valp = &cc->codeloc;
			}
			else if ((cd = ucv_resource_data(*argp, "ffi.closure")) != NULL) {
				/* pre-wrapped closure: extract the function pointer from the
				 * closure context; the resource owns the closure */
				uc_closure_context_t *cc = *(uc_closure_context_t **)(cd + 1);

				valp = &cc->codeloc;
				memp = NULL;
			}
			else {
				memp = valp = xalloc(sz);
				uc_cconv_ct_tv(cts, d, valp, *argp, CCF_ARG(i), NULL);
			}

			uc_vector_push(&argmem, memp);
			uc_vector_push(&argvalues, valp);
		}

		uc_vector_push(&argtypes, atype);
	}

	if (fid) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"too few arguments for called function");

		goto out;
	}

	ffi_status st;

	if (ct->info & CTF_VARARG)
		st = ffi_prep_cif_var(&cif, abi, nfixedargs, argtypes.count, rtype, argtypes.entries);
	else
		st = ffi_prep_cif(&cif, abi, argtypes.count, rtype, argtypes.entries);

	switch (st) {
	case FFI_BAD_TYPEDEF:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "invalid FFI type");
		goto out;

	case FFI_BAD_ABI:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "invalid FFI ABI");
		goto out;

#ifdef HAVE_FFI_BAD_ARGTYPE
	case FFI_BAD_ARGTYPE:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "invalid variadic argument type");
		goto out;
#endif

	case FFI_OK:
		if (rtype != &ffi_type_void) {
			CTSize rsz = ctype_get(cts, ctype_cid(ct_ret->info))->size;

			if (rsz < sizeof(ffi_arg) || rsz == CTSIZE_INVALID)
				rsz = sizeof(ffi_arg);

			rv = uc_cdata_new(vm, ctype_cid(ct_ret->info), rsz);
		}

		ffi_call(&cif,
			(void (*)(void))cdata_getptr(cdataptr(cd), sz),
			uc_cdata_dataptr(rv),
			argvalues.entries);
	}

out:
	/* The cif is done with the descriptors once ffi_prep_cif() returned, so the
	 * ones it synthesized are released here. The predefined descriptors belong
	 * to libffi and are left alone. */
	uc_ctype_free_ffi_type(stype);

	for (size_t i = 0; i < argtypes.count; i++)
		if (!uc_ffi_type_is_predefined(argtypes.entries[i]))
			uc_ctype_free_ffi_type(argtypes.entries[i]);

	while (argmem.count) {
		void *ptr = argmem.entries[--argmem.count];

		if ((uintptr_t)ptr & 1u) {
			uc_closure_context_t *cc =
				(uc_closure_context_t *)((uintptr_t)ptr & ~(uintptr_t)1u);

			ucv_put(cc->func);
			uc_closure_context_free(cc);
		}
		else {
			free(ptr);
		}
	}

	uc_vector_clear(&argvalues);
	uc_vector_clear(&argtypes);
	uc_vector_clear(&argmem);

	return rv;
}

static uc_value_t *
uc_ctype_free(uc_vm_t *vm, size_t nargs)
{
	GCcdata **cd = uc_fn_this("ffi.ctype");

	if (cd) {
		if (UC_UNLIKELY(*cd && cdataisv(*cd)))
			free(memcdatav(*cd));
		else
			free(*cd);

		*cd = NULL;
	}

	return NULL;
}

static uc_value_t *
ct_to_uv(uc_vm_t *vm, CTState *cts, CTypeID cid, void *cdata, size_t size,
         uc_value_t *refs);

static uc_value_t *
ct_to_uv(uc_vm_t *vm, CTState *cts, CTypeID cid, void *cdata, size_t size,
         uc_value_t *refs)
{
	CType *ct = ctype_get(cts, cid);
	CTInfo info = ct->info;
	uc_value_t *s;

	switch (ctype_type(info)) {
	case CT_PTR:
		switch (ctype_cid(info)) {
		case CTID_INT8:
		case CTID_UINT8:
			if ((info ^ CTF_UCHAR) & CTF_UNSIGNED)
				goto generic_ptr;

			/* fall through */

		case CTID_CCHAR:
			/* special optimization case: when retrieving the ucode equivalent value of
			a `const char *` pointer, attempt to return a reference to the original
			uv string (if any) nstead of constructing a new heap string */
			if (ctype_cid(info) == CTID_CCHAR) {
				for (size_t i = 0; i < ucv_array_length(refs); i++) {
					uc_string_t *us = (uc_string_t *)ucv_array_get(refs, i);

					if (us->str == *(char **)cdata) {
						return ucv_get(&us->header);
					}
				}
			}

			return *(char **)cdata ? ucv_string_new(*(char **)cdata) : NULL;

		default:
		generic_ptr:
			/* Return pointer value as integer for all pointer types */
			return ucv_uint64_new((uintptr_t)*(void **)cdata);
		}

		break;

	case CT_NUM:
		if (info & CTF_BOOL) {
			return ucv_boolean_new(*(bool *)cdata);
		}
		else if ((info & CTF_FP)) {
			if (size == sizeof(double))
				return ucv_double_new(*(double *)cdata);
			else if (size == sizeof(float))
				return ucv_double_new(*(float *)cdata);
		}
		else if (size == 1) {
			if (info & CTF_UNSIGNED)
				return ucv_uint64_new(*(uint8_t *)cdata);
			else
				return ucv_int64_new(*(int8_t *)cdata);
		}
		else if (size == 2) {
			if (info & CTF_UNSIGNED)
				return ucv_uint64_new(*(uint16_t *)cdata);
			else
				return ucv_int64_new(*(int16_t *)cdata);
		}
		else if (size == 4) {
			if (info & CTF_UNSIGNED)
				return ucv_uint64_new(*(uint32_t *)cdata);
			else
				return ucv_int64_new(*(int32_t *)cdata);
		}
		else if (size == 8) {
			if (info & CTF_UNSIGNED)
				return ucv_uint64_new(*(uint64_t *)cdata);
			else
				return ucv_int64_new(*(int64_t *)cdata);
		}

		break;

	case CT_ENUM:
		/* attempt to return named enum choice name */
		for (CTypeID choice_id = ct->sib; choice_id; ) {
			CType *choice_type = ctype_get(cts, choice_id);

			choice_id = choice_type->sib;

			if (!ctype_isconstval(choice_type->info) || !choice_type->uv_name)
				continue;

			if (choice_type->size != *(CTSize *)cdata)
				continue;

			return ucv_get(choice_type->uv_name);
		}

		/* no matching constant name found, return numeric value */
		if (ctype_cid(info) == CTID_UINT32)
			return ucv_uint64_new(*(uint32_t *)cdata);
		else
			return ucv_int64_new(*(int32_t *)cdata);

		break;

	case CT_ARRAY:
		if (info & CTF_COMPLEX) {
			if (size == 2 * sizeof(float)) {
				uc_value_t *a = ucv_array_new_length(vm, 2);
				float *f = (float *)cdata;

				ucv_array_set(a, 0, ucv_double_new((double)f[0]));
				ucv_array_set(a, 1, ucv_double_new((double)f[1]));

				return a;
			}
			else if (size == 2 * sizeof(double)) {
				uc_value_t *a = ucv_array_new_length(vm, 2);
				double *d = (double *)cdata;

				ucv_array_set(a, 0, ucv_double_new(d[0]));
				ucv_array_set(a, 1, ucv_double_new(d[1]));

				return a;
			}
		}
		else {
			CType *elem_type = ctype_rawchild(cts, ct);
			CTSize elem_size = elem_type->size;
			uc_value_t *a = ucv_array_new_length(vm, size / elem_size);

			for (size_t off = 0; off < size; off += elem_size)
				ucv_array_push(a,
					ct_to_uv(vm, cts, ctype_typeid(cts, elem_type),
					         (char *)cdata + off, elem_size, refs));

			return a;
		}

		break;

	case CT_STRUCT:
		s = ucv_object_new(vm);

		for (CTypeID field_id = ct->sib; field_id; ) {
			CType *field_type = ctype_get(cts, field_id);

			field_id = field_type->sib;

			if (ctype_isfield(field_type->info) || ctype_isbitfield(field_type->info)) {
				if (!field_type->uv_name)
					continue;

				ucv_object_add(s, ucv_string_get(field_type->uv_name),
					ct_to_uv(vm, cts, ctype_cid(field_type->info),
					         (char *)cdata + field_type->size,
					         ctype_rawchild(cts, field_type)->size, refs));
			}
		}

		return s;
	}

	return NULL;
}


/**
 * Read a value from a C data object.
 *
 * The `get()` method reads values from cdata objects and returns them
 * converted to ucode types. It supports:
 *
 * - **Scalar values**: `int.get()` returns the scalar value directly
 * - **Array indexing**: `arr.get(n)` returns element at position n
 * - **Struct fields**: `struct.get('field')` returns field value
 * - **Path notation**: `struct.get('nested.field[0]')` for deep access
 *
 * For arrays and struct fields, `get()` behaves identically to `index()`.
 * Use `get()` as the primary method for reading values due to its
 * descriptive name.
 *
 * @function module:ffi.CData#get
 *
 * @param {string|number} [key]
 * The field name, array index, or path to read. Omit for scalar types
 * to get the value directly.
 *
 * @returns {*}
 * The value at the specified location, converted to a ucode type.
 * For structs without a key, returns an object with all field values.
 *
 * @throws {Error}
 * Throws an exception if the key is invalid for the type.
 *
 * @example
 * // Read scalar value (no key needed)
 * let x = ffi.ctype('int', 42);
 * x.get();    // => 42 (number)
 *
 * @example
 * // Read struct field
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point', 10, 20);
 * p.get('x');    // => 10 (number)
 * p.get('y');    // => 20 (number)
 *
 * @example
 * // Read entire struct as object
 * p.get();    // => {x: 10, y: 20} (ucode object)
 *
 * @example
 * // Read array element
 * let arr = ffi.ctype('int[5]', [1, 2, 3, 4, 5]);
 * arr.get(0);    // => 1 (number)
 * arr.get(4);    // => 5 (number)
 *
 * @example
 * // Path notation for nested access
 * ffi.cdef('struct rect { struct point min; struct point max; };');
 * let r = ffi.ctype('struct rect', {
 *     min: {x: 0, y: 0},
 *     max: {x: 100, y: 100}
 * });
 * r.get('min.x');        // => 0
 * r.get('max.y');        // => 100
 *
 * @see {@link module:ffi.CData#index|index()} - Equivalent for array/field access
 * @see {@link module:ffi.CData#set|set()} - Write values to cdata
 */
static uc_value_t *
uc_ctype_get(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");
	CTState *cts = ctype_cts(vm);
	CTInfo qual = 0;
	uint8_t *p;
	CType *ct;
	CTSize sz;

	if (!cd)
		return NULL;

	ct = ctype_get(cts, cd->ctypeid);
	sz = cdataisv(cd) ? cdatavlen(cd) : ct->size;
	p = cdataptr(cd);

	/* Handle reference types: dereference to get actual data pointer */
	if (ctype_isref(ct->info)) {
		p = *(uint8_t **)p;
		ct = ctype_get(cts, ctype_cid(ct->info));
		if (!ct)
			return NULL;
		sz = ct->size;
		if (sz == CTSIZE_INVALID)
			return NULL;
	}

	if (nargs == 0 && ctype_isstruct(ct->info)) {
		return ct_to_uv(vm, cts, ctype_typeid(cts, ct), p, sz,
		                cd->refs);
	}

	if (nargs) {
		uc_value_t *key = uc_fn_arg(0);

		/* Check for path syntax (contains '.' or '[') */
		bool is_path = false;
		if (ucv_type(key) == UC_STRING) {
			const char *s = ucv_string_get(key);
			if (strpbrk(s, ".["))
				is_path = true;
		}

		if (is_path) {
			/* Use path parsing for nested access */
			path_tokens tokens = {0};
			bool error = false;

			if (!path_tokenize(vm, key, &tokens))
				return NULL;

			ct = path_navigate(cts, cd, &tokens, &p, &ct, &error);
			path_tokens_free(&tokens);

			if (error || !ct) {
				uc_value_t *repr = uc_ctype_repr(vm, cd->ctypeid, NULL);
				char *keystr = ucv_to_string(vm, key);

				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "Invalid path '%s' for type '%s'",
				                      keystr, ucv_string_get(repr));

				ucv_put(repr);
				free(keystr);

				return NULL;
			}

			/* path_navigate already returns the final type */
			sz = ct->size;
		}
		else {
			/* Use original uc_cdata_index for single-level access */
			ct = uc_cdata_index(cts, cd, key, &p, &qual);

			if (!ct)
				return NULL;

			if (qual & 1) {
				uc_value_t *repr = uc_ctype_repr(vm, cd->ctypeid, NULL);
				char *keystr = ucv_to_string(vm, key);

				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "Invalid index '%s' for type '%s' given",
				                      keystr, ucv_string_get(repr));

				ucv_put(repr);
				free(keystr);

				return NULL;
			}

			ct = ctype_child(cts, ct);
			sz = ct->size;
		}
	}

	return ct_to_uv(vm, cts, ctype_typeid(cts, ct), p, sz,
	                cd->refs);
}

/**
 * Write a value to a C data object.
 *
 * The `set()` method writes a value to a cdata. For structs, it can write
 * individual fields by name. For arrays, it can write elements by index.
 *
 * @function module:ffi.CData#set
 *
 * @param {string|number} key
 * The field name or array index to write.
 *
 * @param {*} value
 * The value to write. Will be converted to the appropriate C type.
 *
 * @returns {undefined}
 * Returns `undefined`.
 *
 * @throws {Error}
 * Throws an exception if the key is invalid or the value cannot be converted.
 *
 * @example
 * // Write scalar value
 * let x = ffi.ctype('int');
 * x.set(42);
 *
 * @example
 * // Write struct field
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point');
 * p.set('x', 10);
 * p.set('y', 20);
 *
 * @example
 * // Write array element
 * let arr = ffi.ctype('int[5]');
 * arr.set(0, 100);
 * arr.set(4, 200);
 */
static uc_value_t *
uc_ctype_set(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");
	CTState *cts = ctype_cts(vm);
	CTInfo qual = 0;
	uint8_t *p;
	CType *ct;

	if (!cd)
		return NULL;

	ct = ctype_get(cts, cd->ctypeid);
	p = cdataptr(cd);

	/* Handle reference types: dereference to get actual data pointer */
	if (ctype_isref(ct->info)) {
		p = *(uint8_t **)p;
		ct = ctype_get(cts, ctype_cid(ct->info));
		if (!ct)
			return NULL;
	}

	if (nargs > 1) {
		uc_value_t *key = uc_fn_arg(0);
		uc_value_t *val = uc_fn_arg(1);

		/* Check for path syntax (contains '.' or '[') */
		bool is_path = false;
		if (ucv_type(key) == UC_STRING) {
			const char *s = ucv_string_get(key);
			if (strpbrk(s, ".["))
				is_path = true;
		}

		if (is_path) {
			/* Use path parsing for nested access */
			path_tokens tokens = {0};
			bool error = false;

			if (!path_tokenize(vm, key, &tokens))
				return NULL;

			ct = path_navigate(cts, cd, &tokens, &p, &ct, &error);
			path_tokens_free(&tokens);

			if (error || !ct) {
				uc_value_t *repr = uc_ctype_repr(vm, cd->ctypeid, NULL);
				char *keystr = ucv_to_string(vm, key);

				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "Invalid path '%s' for type '%s'",
				                      keystr, ucv_string_get(repr));

				ucv_put(repr);
				free(keystr);

				return NULL;
			}

			/* path_navigate already returns the final type, no need to call ctype_child */
		}
		else {
			/* Use original uc_cdata_index for single-level access */
			ct = uc_cdata_index(cts, cd, key, &p, &qual);

			if (!ct)
				return NULL;

			if (qual & 1) {
				uc_value_t *repr = uc_ctype_repr(vm, cd->ctypeid, NULL);
				char *keystr = ucv_to_string(vm, key);

				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "Invalid index '%s' for type '%s' given",
				                      keystr, ucv_string_get(repr));

				ucv_put(repr);
				free(keystr);

				return NULL;
			}

			ct = ctype_child(cts, ct);
		}

		/* Convert and store the value */
		uc_cconv_ct_tv(cts, ct, p, val, CCF_ARG(0), NULL);
	}
	else if (nargs == 1) {
		/* No key: set the value directly (for reference types or scalar cdata) */
		uc_value_t *val = uc_fn_arg(0);
		uc_cconv_ct_tv(cts, ct, p, val, CCF_ARG(0), NULL);
	}

	return NULL;
}

/**
 * Get a pointer to a C data object.
 *
 * The `ptr()` method returns a pointer cdata pointing to the memory of the
 * current cdata. This is useful for passing to C functions that expect
 * pointers.
 *
 * @function module:ffi.CData#ptr
 *
 * @returns {module:ffi.CData}
 * A pointer cdata pointing to this cdata's memory.
 *
 * @example
 * // Get pointer to scalar
 * let x = ffi.ctype('int', 42);
 * let px = x.ptr();  // int* pointer
 *
 * @example
 * // Pass to C function expecting pointer
 * ffi.cdef('void memset(void *, int, size_t)');
 * let buf = ffi.ctype('char[10]');
 * ffi.C.wrap('void memset(void *, int, size_t)')(buf.ptr(), 0, 10);
 */
static uc_value_t *
uc_ctype_ptr(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");

	if (!cd)
		return NULL;

	uc_value_t *pres = uc_cdata_new(vm, CTID_P_VOID, CTSIZE_PTR);
	*(void **)uc_cdata_dataptr(pres) = cdataptr(cd);

	return pres;
}

/**
 * Access array elements, struct fields, or perform pointer arithmetic.
 *
 * The `index()` method returns **raw cdata references** to the accessed
 * location, without converting to ucode types. This allows further
 * manipulation, pointer arithmetic, or explicit conversion.
 *
 * Supports:
 *
 * - **Array indexing**: `arr.index(n)` returns cdata reference to element
 * - **Struct fields**: `struct.index('field')` returns cdata reference
 * - **Pointer arithmetic**: `ptr.index(n)` returns cdata at *(ptr + n)
 * - **Path notation**: `struct.index('nested.field[0]')` for deep access
 *
 * **Key difference from `get()`**: `index()` returns raw cdata (unconverted),
 * while `get()` returns converted ucode values.
 *
 * @function module:ffi.CData#index
 *
 * @param {string|number} key
 * The array index, field name, or path to access.
 *
 * @returns {module:ffi.CData}
 * A cdata reference to the value at the specified location (unconverted).
 * Call `.get()` on the result to convert to a ucode value.
 *
 * @throws {Error}
 * Throws an exception if the key is invalid for the type.
 *
 * @example
 * // Array indexing - returns cdata, not number
 * let arr = ffi.ctype('int[5]', [10, 20, 30, 40, 50]);
 * arr.index(0);      // => cdata (int)
 * arr.index(0).get() // => 10 (number)
 *
 * @example
 * // Struct field access - returns cdata reference
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point', 10, 20);
 * p.index('x');      // => cdata (int)
 * p.index('x').get() // => 10 (number)
 *
 * @example
 * // Pointer arithmetic - returns cdata at offset
 * let ptr = ffi.ctype('int *', arr.ptr());
 * ptr.index(0);      // => cdata (int) at ptr[0]
 * ptr.index(2);      // => cdata (int) at ptr[2]
 * ptr.index(2).get() // => 30 (number)
 *
 * @example
 * // Chaining - modify through index()
 * arr.index(0).set(100);  // Set arr[0] = 100
 *
 * @see {@link module:ffi.CData#get|get()} - Returns converted ucode values
 * @see {@link module:ffi.CData#ptr|ptr()} - Get a pointer, not a value
 */
static uc_value_t *
uc_ctype_index(uc_vm_t *vm, size_t nargs)
{
	CTState *cts = ctype_cts(vm);
	CTInfo qual = 0;
	uint8_t *p;
	GCcdata *cd = uc_fn_thisval("ffi.ctype");
	uc_value_t *key = uc_fn_arg(0);

	if (!cd)
		return NULL;

	/* Check for path syntax (contains '.' or '[') */
	bool is_path = false;
	if (ucv_type(key) == UC_STRING) {
		const char *s = ucv_string_get(key);
		if (strpbrk(s, ".["))
			is_path = true;
	}

	CType *ct = ctype_get(cts, cd->ctypeid);

	/* Handle reference types: dereference to get actual type */
	if (ctype_isref(ct->info)) {
		p = *(uint8_t **)cdataptr(cd);
		ct = ctype_get(cts, ctype_cid(ct->info));
		if (!ct)
			return NULL;
	}
	else {
		p = cdataptr(cd);
	}

	if (is_path) {
		/* Use path parsing for nested access */
		path_tokens tokens = {0};
		bool error = false;

		if (!path_tokenize(vm, key, &tokens)) {
			path_tokens_free(&tokens);
			return NULL;
		}

		ct = path_navigate(cts, cd, &tokens, &p, &ct, &error);
		path_tokens_free(&tokens);

		if (error || !ct)
			return NULL;
	}
	else {
		/* Handle integer key for pointer/array indexing */
		uc_type_t ut = ucv_type(key);
		bool is_integer_key = (ut == UC_INTEGER || ut == UC_DOUBLE);

		if (is_integer_key && (ctype_ispointer(ct->info) || ctype_isarray(ct->info))) {
			/* Pointer/array indexing: ptr[index] or arr[index] */
			ptrdiff_t idx;
			if (ut == UC_INTEGER)
				idx = (ptrdiff_t)ucv_int64_get(key);
			else
				idx = (ptrdiff_t)ucv_double_get(key);

			CTSize sz = uc_ctype_size(cts, ctype_cid(ct->info));
			if (sz == CTSIZE_INVALID) {
				uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					"size of C type is unknown or too large");
				return NULL;
			}

			/* Get the pointer value (for ptr types) or use data directly (for arrays) */
			if (ctype_isptr(ct->info))
				p = (uint8_t *)cdata_getptr(p, ct->size);

			/* Get element type */
			CType *elt = ctype_rawchild(cts, ct);

			/* Calculate offset */
			p = p + idx * (int32_t)sz;

			/* Return raw cdata reference (unconverted) */
			CTypeID elt_id = ctype_typeid(cts, elt);
			CType *elt_ct = ctype_get(cts, elt_id);

			/* For pointer element types, we need to store the pointer VALUE, not the address */
			if (ctype_isptr(elt_ct->info)) {
				/* Read the pointer value from p and create a cdata containing it */
				void *ptrval = cdata_getptr(p, elt_ct->size);
				uc_value_t *res = uc_cdata_new(vm, elt_id, elt_ct->size);
				*(void **)uc_cdata_dataptr(res) = ptrval;
				return res;
			}

			return uc_cdata_newref(vm, p, elt_id);
		}
		else {
			/* Use original uc_cdata_index for single-level access */
			ct = uc_cdata_index(cts, cd, key, &p, &qual);

			if (!ct)
				return NULL;

			if (qual & 1)
				return NULL;

			/* Get the raw child type to avoid qualifier issues,
			   but preserve pointer types */
			if (!ctype_ispointer(ct->info))
				ct = ctype_rawchild(cts, ct);

			/* For pointer fields, create a cdata containing the pointer value */
			if (ctype_ispointer(ct->info)) {
				void *ptrval = cdata_getptr(p, ct->size);
				uc_value_t *res = uc_cdata_new(vm, ctype_typeid(cts, ct), CTSIZE_PTR);
				*(void **)uc_cdata_dataptr(res) = ptrval;
				return res;
			}
		}
	}

	/* Return raw cdata reference (unconverted) */
	return uc_cdata_newref(vm, p, ctype_typeid(cts, ct));
}

/**
 * Read the value pointed to by a pointer cdata.
 *
 * The `deref()` method dereferences a pointer cdata and reads the value
 * at the pointed-to address. The target type can be specified explicitly
 * or inferred from the pointer type.
 *
 * @function module:ffi.CData#deref
 *
 * @param {string} [type]
 * The C type to read. If omitted, the pointer's element type is used.
 *
 * @returns {*}
 * The value at the pointer address, converted to a ucode type.
 *
 * @throws {Error}
 * Throws an exception if the pointer is NULL or the type is invalid.
 *
 * @example
 * // Dereference int pointer
 * let x = ffi.ctype('int', 42);
 * let px = x.ptr();
 * print(px.deref('int'));  // => 42
 *
 * @example
 * // Read first byte of char*
 * ffi.cdef('char *strdup(const char *)');
 * let ptr = ffi.C.wrap('char *strdup(const char *)')("hello");
 * print(ptr.deref('char'));  // => 104 (ASCII for 'h')
 * ptr.deref();  // Also works, uses pointer's element type
 */
static uc_value_t *
uc_ctype_deref(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");

	if (!cd)
		return NULL;

	CTState *cts = ctype_cts(vm);
	CType *ct = ctype_get(cts, cd->ctypeid);

	CTypeID ctid;
	uint8_t *p = NULL;

	/* Skip extern and attribute wrappers to get the actual type */
	while (ctype_isextern(ct->info) || ctype_isattrib(ct->info))
		ct = ctype_child(cts, ct);

	if (ctype_isptr(ct->info)) {
		ctid = nargs
			? uv_to_ct(vm, CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT,
				uc_fn_arg(0), NULL)
			: ctype_cid(ct->info);

		if (!ctid)
			return NULL;

		p = *(uint8_t **)cdataptr(cd);

		if (!p) {
			uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				"Attempt to dereference a NULL pointer");

			return NULL;
		}
	}
	else if (ctype_isref(ct->info)) {
		/* Reference: dereference to get actual data pointer */
		ctid = nargs
			? uv_to_ct(vm, CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT,
				uc_fn_arg(0), NULL)
			: ctype_cid(ct->info);

		if (!ctid)
			return NULL;

		p = *(uint8_t **)cdataptr(cd);

		if (!p) {
			uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				"Attempt to dereference a NULL reference");

			return NULL;
		}
	}
	else if (ctype_isrefarray(ct->info)) {
		/* Array: dereference returns first element */
		ctid = nargs
			? uv_to_ct(vm, CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT,
				uc_fn_arg(0), NULL)
			: ctype_cid(ct->info);

		if (!ctid)
			return NULL;

		p = (uint8_t *)cdataptr(cd);

		if (!p) {
			uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				"Attempt to dereference empty array");

			return NULL;
		}
	}
	else {
		uc_value_t *repr = uc_ctype_repr(vm, cd->ctypeid, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"Attempt to dereference non-pointer type %s",
			ucv_string_get(repr));

		ucv_put(repr);

		return NULL;
	}

	CType *ctt = ctype_raw(cts, ctid);

	if (ctt->size == CTSIZE_INVALID) {
		uc_value_t *repr = uc_ctype_repr(vm, ctid, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"C type '%s' has unknown storage size",
			ucv_string_get(repr));

		ucv_put(repr);

		return NULL;
	}

	uc_value_t *rv = NULL;

	uc_cconv_tv_ct(cts, ctt, ctid, &rv, p);

	return rv;
}

static CTSize
uc_ctype_sizeof_common(CTState *cts, uc_value_t *uv, uc_value_t *nelem)
{
	GCcdata *cd = NULL;
	CTypeID id;
	CTSize sz;
	CType *ct;

	id = uv_to_ct(cts->vm, CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT,
		uv, &cd);

	if (!id)
		return CTSIZE_INVALID;

	if (UC_UNLIKELY(cd && cdataisv(cd))) {
		ct = uc_ctype_rawref(cts, id);
		if (ctype_isarray(ct->info)) {
			CType *child = ctype_rawchild(cts, ct);
			return cdatavlen(cd) * child->size;
		}
		return cdatavlen(cd) * ct->size;
	}

	ct = uc_ctype_rawref(cts, id);

	if (ctype_isvltype(ct->info)) {
		// FIXME: transaprently handle cdata (ffi_checkint())
		if (ucv_type(nelem) != UC_INTEGER) {
			uc_vm_raise_exception(cts->vm, EXCEPTION_TYPE,
				"integer argument expected, got %s",
				ucv_typename(nelem));

			return CTSIZE_INVALID;
		}

		sz = uc_ctype_vlsize(cts, ct, (CTSize)ucv_int64_get(nelem));
	}
	else {
		sz = ctype_hassize(ct->info) ? ct->size : CTSIZE_INVALID;
	}

	return sz;
}

/**
 * Get the size of a C data object in bytes.
 *
 * The `size()` method returns the total size in bytes of the cdata. For
 * arrays, this is the total size including all elements.
 *
 * @function module:ffi.CData#size
 *
 * @returns {number}
 * The size of the cdata in bytes.
 *
 * @example
 * // Get size of struct
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point');
 * print(p.size());  // => 8 (on typical systems)
 *
 * @example
 * // Get size of array
 * let arr = ffi.ctype('int[10]');
 * print(arr.size());  // => 40 (10 * sizeof(int))
 */
static uc_value_t *
uc_ctype_sizeof(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this = _uc_fn_this_res(vm);
	CTSize sz = uc_ctype_sizeof_common(ctype_cts(vm), this, uc_fn_arg(0));

	return (sz != CTSIZE_INVALID) ? ucv_uint64_new(sz) : NULL;
}

/**
 * Get the number of elements in an array cdata.
 *
 * The `length()` method returns the number of elements in an array.
 * For non-array types, returns `null`.
 *
 * @function module:ffi.CData#length
 *
 * @returns {?number}
 * The number of elements in the array, or `null` if not an array.
 *
 * @example
 * // Get array length
 * let arr = ffi.ctype('int[10]');
 * print(arr.length());  // => 10
 *
 * @example
 * // Works with initialized arrays
 * let arr2 = ffi.ctype('char[5]', "hello");
 * print(arr2.length());  // => 5
 */
static uc_value_t *
uc_ctype_length(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this = _uc_fn_this_res(vm);
	CTState *cts = ctype_cts(vm);
	CTSize sz = uc_ctype_sizeof_common(cts, this, uc_fn_arg(0));

	if (sz == CTSIZE_INVALID)
		return NULL;

	GCcdata *cd = ucv_resource_data(this, "ffi.ctype");
	CType *ct = ctype_raw(cts, cd->ctypeid);

	if (!ctype_isarray(ct->info))
		return NULL;

	CTSize item_sz = ctype_rawchild(cts, ct)->size;

	return (item_sz != CTSIZE_INVALID) ? ucv_uint64_new(sz / item_sz) : NULL;
}

/**
 * Get the size of an array element or struct field in bytes.
 *
 * The `itemsize()` method returns the size in bytes of each element in an
 * array, or the size of a specified struct field.
 *
 * @function module:ffi.CData#itemsize
 *
 * @param {string} [fieldname]
 * For struct types, the field name to get the size of.
 *
 * @returns {number}
 * The size of each array element or the struct field in bytes.
 *
 * @example
 * // Get array element size
 * let arr = ffi.ctype('int[10]');
 * print(arr.itemsize());  // => 4 (sizeof(int))
 *
 * @example
 * // Get struct field size
 * ffi.cdef('struct foo { char a; int b; double c; };');
 * let f = ffi.ctype('struct foo');
 * print(f.itemsize('b'));  // => 4 (size of int field)
 */
static uc_value_t *
uc_ctype_itemsize(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this = _uc_fn_this_res(vm);
	GCcdata *cd = NULL;
	CTypeID id = uv_to_ct(vm, CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT,
		this, &cd);

	if (!id)
		return NULL;

	CTState *cts = ctype_cts(vm);
	CTInfo info = ctype_raw(cts, id)->info;
	CTSize item_sz = CTSIZE_INVALID;

	if (ctype_isstruct(info)) {
		uc_value_t *key = uc_fn_arg(0);

		if (ucv_type(key) != UC_STRING) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"Expecting field name for struct type, got %s",
				nargs ? ucv_typename(key) : "no argument");

			return NULL;
		}

		CTSize ofs;
		CType *fct;

		fct = uc_ctype_getfieldq(cts, ctype_raw(cts, id), key, &ofs, NULL);

		if (fct)
			item_sz = ctype_rawchild(cts, fct)->size;
	}
	else if (ctype_isarray(info)) {
		item_sz = ctype_rawchild(cts, ctype_get(cts, id))->size;
	}

	return (item_sz != CTSIZE_INVALID) ? ucv_uint64_new(item_sz) : NULL;
}

/**
 * Extract a substring from a char* or char[] cdata.
 *
 * The `slice()` method extracts a substring from a character pointer or
 * array. For char* pointers, it reads until the null terminator by default.
 * For char[] arrays, it uses the array length.
 *
 * @function module:ffi.CData#slice
 *
 * @param {number} [start=0]
 * The starting index (0-based). Negative values count from the end.
 *
 * @param {number} [end]
 * The ending index (exclusive). If omitted, uses the end of the string/array.
 *
 * @returns {string}
 * The extracted substring.
 *
 * @throws {Error}
 * Throws an exception if called without arguments on non-char* pointer types.
 *
 * @example
 * // Extract from char* pointer
 * ffi.cdef('char *strdup(const char *)');
 * let ptr = ffi.C.wrap('char *strdup(const char *)')("hello world");
 * print(ptr.slice());      // => "hello world"
 * print(ptr.slice(6));     // => "world"
 * print(ptr.slice(0, 5));  // => "hello"
 *
 * @example
 * // Extract from char[] array
 * let buf = ffi.ctype('char[10]', "hello");
 * print(buf.slice());      // => "hello"
 * print(buf.slice(0, 3));  // => "hel"
 */
static uc_value_t *
uc_ctype_slice(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");

	if (!cd)
		return NULL;

	CTState *cts = ctype_cts(vm);
	CType *ct = ctype_get(cts, cd->ctypeid);
	CTSize sz = cdataisv(cd) ? cdatavlen(cd) : ct->size;
	uint8_t *p = cdataptr(cd);

	/* Check if this is a char* pointer type */
	bool is_charptr = false;
	uint8_t *charptr_data = p;
	size_t charptr_len = sz;

	if (ctype_isptr(ct->info)) {
		CType *child = ctype_rawchild(cts, ct);
		/* Unwrap REF pointers to get the actual pointed-to type */
		if (ctype_isptr(child->info)) {
			/* This is a pointer to pointer - check if inner points to char */
			CType *inner = ctype_rawchild(cts, child);
			if (ctype_type(inner->info) == CT_NUM && inner->size == 1) {
				is_charptr = true;
				charptr_data = *(uint8_t **)p;
				if (charptr_data)
					charptr_len = strlen((char *)charptr_data);
				else
					charptr_len = 0;
			}
		}
		else if (ctype_type(child->info) == CT_NUM && child->size == 1) {
			is_charptr = true;
			charptr_data = *(uint8_t **)p;
			if (charptr_data)
				charptr_len = strlen((char *)charptr_data);
			else
				charptr_len = 0;
		}
	}

	/* No arguments: treat as string() for char* pointers */
	if (nargs == 0) {
		if (is_charptr) {
			/* char* pointer: read null-terminated string */
			if (!charptr_data)
				return ucv_string_new("");
			return ucv_string_new((char *)charptr_data);
		}
		/* For non-char* pointers, require explicit indices */
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"slice() without arguments only supported for char* pointers");
		return NULL;
	}

	int64_t start_i = ucv_int64_get(uc_fn_arg(0));
	size_t start;
	size_t end;

	if (start_i < 0)
		start = (is_charptr ? charptr_len : sz) + start_i;
	else
		start = (size_t)start_i;

	if (nargs >= 2) {
		int64_t end_i = ucv_int64_get(uc_fn_arg(1));
		if (end_i < 0)
			end = (is_charptr ? charptr_len : sz) + end_i;
		else
			end = (size_t)end_i;
	}
	else {
		end = is_charptr ? charptr_len : sz;
	}

	/* Clamp to valid range */
	size_t max_len = is_charptr ? charptr_len : sz;
	if (start > max_len)
		start = max_len;
	if (end > max_len)
		end = max_len;
	if (start > end)
		start = end;

	size_t len = end - start;

	if (len == 0)
		return ucv_string_new_length("", 0);

	return ucv_string_new_length((char *)charptr_data + start, len);
}

static uc_value_t *
uc_ctype_tostring(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");

	if (!cd)
		return NULL;

	CTState *cts = ctype_cts(vm);
	CType *ct = ctype_get(cts, cd->ctypeid);
	uc_value_t *type_repr = uc_ctype_repr(vm, cd->ctypeid, NULL);
	uc_stringbuf_t *sb = ucv_stringbuf_new();

	ucv_stringbuf_addstr(sb, ucv_string_get(type_repr), ucv_string_length(type_repr));
	ucv_put(type_repr);

	/* Skip qualifiers and attributes to get the actual type */
	while (ctype_isattrib(ct->info) || ctype_isref(ct->info))
		ct = ctype_child(cts, ct);

	/* Format value based on type */
	switch (ctype_type(ct->info)) {
	case CT_NUM:
	case CT_ENUM:
		{
			uc_value_t *val = ct_to_uv(vm, cts, cd->ctypeid, cdataptr(cd),
			                           cdataisv(cd) ? cdatavlen(cd) : ct->size,
			                           cd->refs);
			if (val) {
				char *str = ucv_to_string(vm, val);
				if (str) {
					ucv_stringbuf_addstr(sb, ": ", 2);
					ucv_stringbuf_addstr(sb, str, strlen(str));
					free(str);
				}
				ucv_put(val);
			}
		}
		break;

	case CT_ARRAY:
		{
			CTSize clen = ct->size;
			CType *ctt = ctype_rawchild(cts, ct);

			/* Complex number: show as re+imI */
			if (ct->info & CTF_COMPLEX)
			{
				uc_value_t *val = uc_ctype_repr_complex(cdataptr(cd),
					cdataisv(cd) ? cdatavlen(cd) : ct->size);
				if (val) {
					ucv_stringbuf_addstr(sb, ": ", 2);
					ucv_stringbuf_addstr(sb, ucv_string_get(val), ucv_string_length(val));
					ucv_put(val);
				}
			}
			/* String array: show contents */
			else if (ctt->size == 1 && (ctt->info & CTF_UNSIGNED) == 0)
			{
				char *str = (char *)cdataptr(cd);
				ucv_stringbuf_addstr(sb, ": \"", 3);
				for (char *p = str; *p && (p - str) < 128; p++) {
					if (*p == '"')
						ucv_stringbuf_addstr(sb, "\\\"", 2);
					else if (*p == '\\')
						ucv_stringbuf_addstr(sb, "\\\\", 2);
					else if (*p == '\n')
						ucv_stringbuf_addstr(sb, "\\n", 2);
					else if (*p == '\r')
						ucv_stringbuf_addstr(sb, "\\r", 2);
					else if (*p == '\t')
						ucv_stringbuf_addstr(sb, "\\t", 2);
					else if (*p >= 32 && *p < 127)
						ucv_stringbuf_addstr(sb, p, 1);
					else
						ucv_stringbuf_printf(sb, "\\x%02x", (unsigned char)*p);
				}
				ucv_stringbuf_addstr(sb, "\"", 1);
			}
			else if (clen != CTSIZE_INVALID && ctt->size > 0)
			{
				ucv_stringbuf_printf(sb, " (len=%zu)", clen / ctt->size);
			}
		}
		break;

	case CT_PTR:
		{
			void *ptr = *(void **)cdataptr(cd);
			if (ptr)
				ucv_stringbuf_printf(sb, " @ %p", ptr);
			else
				ucv_stringbuf_addstr(sb, ": NULL", 6);
		}
		break;

	case CT_STRUCT:
		{
			CTSize sz = cdataisv(cd) ? cdatavlen(cd) : ct->size;
			uc_value_t *val = ct_to_uv(vm, cts, cd->ctypeid, cdataptr(cd), sz, cd->refs);
			if (val) {
				char *str = ucv_to_string(vm, val);
				if (str) {
					ucv_stringbuf_addstr(sb, ": ", 2);
					ucv_stringbuf_addstr(sb, str, strlen(str));
					free(str);
				}
				ucv_put(val);
			}
		}
		break;

	case CT_VOID:
		ucv_stringbuf_addstr(sb, ": void", 6);
		break;
	}

	return ucv_stringbuf_finish(sb);
}


/**
 * Represents a C data object holding a value of a C type.
 *
 * @class module:ffi.CData
 * @hideconstructor
 *
 * @see {@link module:ffi#ctype|ctype()}
 *
 * @example
 *
 * const val = ctype(…);
 *
 * val.get();
 * val.set(…);
 * val.ptr();
 * val.index(…);
 * val.deref(…);
 * val.size();
 * val.length();
 * val.itemsize(…);
 * val.slice(…);
 */

/**
 * Create a C data instance.
 *
 * The `ctype()` function creates a new C data object (cdata) of the specified
 * type. It can be called with optional initializer values that will be used
 * to initialize the object.
 *
 * **Usage patterns:**
 *
 * 1. **Without initializer**: Creates an uninitialized cdata of the given type.
 *    For pointer types, the pointer is set to NULL.
 *
 * 2. **With initializer**: Creates and initializes a cdata. The initializer
 *    values depend on the type:
 *    - Scalar types: single value (number, boolean)
 *    - Structs: positional arguments for each field or a ucode object
 *    - Arrays: individual element values or a string for char arrays
 *
 *   ```javascript
 *   // Primitive type
 *   let x = ffi.ctype('int', 42);
 *   print(x.get());  // => 42
 *
 *   // Struct type with positional arguments
 *   ffi.cdef('struct point { int x; int y; };');
 *   let p1 = ffi.ctype('struct point', 10, 20);
 *   print(p1.get('x'), p1.get('y'));  // => 10 20
 *
 *   // Struct type with object initializer
 *   let p2 = ffi.ctype('struct point', { x: 30, y: 40 });
 *   print(p2.get('x'), p2.get('y'));  // => 30 40
 *
 *   // Nested struct with object initializer
 *   ffi.cdef('struct rect { struct point tl; struct point br; };');
 *   let r = ffi.ctype('struct rect', {
 *       tl: { x: 0, y: 0 },
 *       br: { x: 100, y: 200 }
 *   });
 *   let tl = r.get('tl');
 *   print(tl.get('x'), tl.get('y'));  // => 0 0
 *
 *   // Array type
 *   let arr = ffi.ctype('int[3]', 1, 2, 3);
 *   print(arr.get(0), arr.get(1), arr.get(2));  // => 1 2 3
 *
 *   // Char array from string
 *   let buf = ffi.ctype('char[10]', 'hello');
 *   print(buf.deref());  // => "hello"
 *
 *   // Pointer type (uninitialized)
 *   let ptr = ffi.ctype('void *');
 *   ```
 *
 * @function module:ffi#ctype
 *
 * @param {string} type
 * A C type declaration string. Can be a basic type, struct name, array type,
 * pointer type, etc. The type must have been declared via `cdef()` first.
 *
 * @param {...*} [init]
 * Optional initializer values.
 *
 * @returns {?module:ffi.CData}
 * A cdata of the specified type, or `null` if the type cannot be
 * parsed or has invalid size. For `typeof()` without initializer,
 * returns a CTypeID handle cdata.
 *
 * @throws {Error}
 * Throws an exception if the type declaration is invalid or wrong number
 * of initializers provided.
 *
 * @example
 * // Create integer
 * let x = ffi.ctype('int', 42);
 * print(x.get());
 *
 * @example
 * // Create struct
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point', 10, 20);
 * print(p.get('x'));
 *
 * @example
 * // Create array
 * let arr = ffi.ctype('double[5]', 1.1, 2.2, 3.3, 4.4, 5.5);
 * print(arr.length());
 */
static uc_value_t *
uc_ffi_ctype(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *spec = uc_fn_arg(0);
	CTState *cts = ctype_cts(vm);
	uc_value_t *res;

	if (ucv_type(spec) != UC_STRING)
		return NULL;

	CPState cp = {
		.uv_vm = vm,
		.cts = cts,
		.srcname = ucv_string_get(spec),
		.p = ucv_string_get(spec),
		.uv_param = NULL,
		.mode = CPARSE_MODE_ABSTRACT | CPARSE_MODE_NOIMPLICIT
	};

	if (!uc_cparse(&cp))
		return NULL;

	/* initializer values provided... */
	if (nargs > 1) {
		size_t init_arg_off = 1;
		CTSize sz;
		CType *ct = ctype_raw(cts, cp.val.id);
		CTInfo info = uc_ctype_info(cts, cp.val.id, &sz);
		uc_value_t *refs = NULL;

		if (info & CTF_VLA)	{
			CTSize vla_sz = ucv_uint64_get(uc_fn_arg(1));

			if (errno) {
				uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					"invalid size argument provided");

				return NULL;
			}

			init_arg_off++;
			sz = uc_ctype_vlsize(cts, ct, vla_sz);
		}

		if (sz == CTSIZE_INVALID) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"C type has invalid size");

			return NULL;
		}

		res = uc_cdata_newx(vm, cp.val.id, sz, info);

		/* Special handling: char array initialized with a string */
		if (ctype_isarray(info) && ctype_isinteger(ctype_child(cts, ct)->info) &&
		    nargs - init_arg_off == 1 && ucv_type(vm->stack.entries[vm->stack.count - nargs + init_arg_off]) == UC_STRING) {
			/* Convert string to array of char values */
			uc_value_t *str = vm->stack.entries[vm->stack.count - nargs + init_arg_off];
			const char *s = ucv_string_get(str);
			size_t len = strlen(s);
			CType *child = ctype_child(cts, ct);
			CTSize elem_sz = child->size;
			GCcdata *cd_tmp = ucv_resource_data(res, "ffi.ctype");
			uint8_t *data = (uint8_t *)cdataptr(cd_tmp);

			/* Copy string including null terminator if array is large enough */
			for (size_t i = 0; i < len && i * elem_sz < sz; i++) {
				if (elem_sz == 1) {
					data[i] = s[i];
				} else {
					/* For wider character types (e.g., char16_t) - truncate for now */
					data[i * elem_sz] = s[i];
				}
			}
			/* Null-terminate if there's space */
			if (len < sz / elem_sz) {
				if (elem_sz == 1) {
					data[len] = '\0';
				} else {
					data[len * elem_sz] = '\0';
				}
			}
		} else {
			if (nargs - init_arg_off > 0) {
				GCcdata *cd_tmp = ucv_resource_data(res, "ffi.ctype");
				uint8_t *data = (uint8_t *)cdataptr(cd_tmp);
				uc_cconv_ct_init(cts, ct, sz, data,
					&vm->stack.entries[vm->stack.count - nargs + init_arg_off],
					nargs - init_arg_off, &refs);
			}
		}

		GCcdata *cd = ucv_resource_data(res, "ffi.ctype");
		cd->refs = refs;
	}
	else {
		CTSize sz;
		CTInfo info = uc_ctype_info(cts, cp.val.id, &sz);

		if (sz == CTSIZE_INVALID) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"C type has invalid size");

			return NULL;
		}

		res = uc_cdata_newx(vm, cp.val.id, sz, info);
	}

	return res;
}

/**
 * Declare C types and functions.
 *
 * The `cdef()` function parses C declaration strings and registers the types
 * with the FFI system. This is required before using types with `ctype()`,
 * wrapping functions with `wrap()`, or resolving symbols with `dlsym()`.
 *
 * Multiple declarations can be provided in a single call, separated by
 * semicolons. The parser supports most C declaration syntax including:
 *
 * - Basic types (`int`, `char`, `float`, `double`, etc.)
 * - Type modifiers (`const`, `volatile`, `unsigned`, `signed`)
 * - Pointers and arrays (`int *`, `char **`, `int[10]`)
 * - Structs and unions (`struct foo { ... }`, `union bar { ... }`)
 * - Enums (`enum baz { ... }`)
 * - Function declarations (`int foo(int, char *)`)
 * - Typedefs (`typedef ...`)
 * - Extern declarations (`extern int var;`)
 *
 *   ```javascript
 *   // Declare a struct type
 *   ffi.cdef('struct point { int x; int y; };');
 *
 *   // Declare a function
 *   ffi.cdef('int strcmp(const char *, const char *);');
 *
 *   // Declare multiple items
 *   ffi.cdef(`
 *       typedef unsigned int uint32_t;
 *       struct sockaddr {
 *           sa_family_t sa_family;
 *           char sa_data[14];
 *       };
 *       extern char **environ;
 *   `);
 *   ```
 *
 * After declaring types, you can create instances with `ctype()`, wrap
 * functions with `wrap()`, or access global variables with `dlsym()`.
 *
 * @function module:ffi#cdef
 *
 * @param {string} spec
 * A C declaration string or multiple declarations separated by semicolons.
 *
 * @returns {module:ffi.CData}
 * A cdata holding the CTypeID handle for the last declared type.
 *
 * @throws {Error}
 * Throws an exception if the declaration syntax is invalid.
 *
 * @example
 * // Declare struct and create instance
 * ffi.cdef('struct point { int x; int y; };');
 * let p = ffi.ctype('struct point', 10, 20);
 * print(p.get('x'), p.get('y'));
 *
 * @example
 * // Declare function and wrap it
 * ffi.cdef('size_t strlen(const char *);');
 * let strlen = ffi.C.wrap('size_t strlen(const char *)');
 * print(strlen("hello").get());
 */
static uc_value_t *
uc_ffi_cdef(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *spec = uc_fn_arg(0);
	CTState *cts = ctype_cts(vm);

	if (ucv_type(spec) != UC_STRING)
		return NULL;

	if (!vm->callframes.count)
		return NULL;

	CPState cp = {
		.uv_vm = vm,
		.cts = cts,
		.srcname = ucv_string_get(spec),
		.p = ucv_string_get(spec),
		.uv_param = &vm->stack.entries[uc_vector_last(&vm->callframes)->stackframe + 2],
		.mode = CPARSE_MODE_MULTI | CPARSE_MODE_DIRECT
	};

	if (!uc_cparse(&cp))
		return NULL;

	uc_value_t *res = uc_cdata_new(vm, CTID_CTYPEID, 4);
	*(CTypeID *)uc_cdata_dataptr(res) = cp.val.id;

	return res;
}

/**
 * Get the CTypeID for a C type.
 *
 * The `typeof()` function returns a CTypeID handle for the specified type.
 * This is useful for storing type references or passing to other FFI functions.
 *
 * @function module:ffi#typeof
 *
 * @param {string} type
 * The C type declaration.
 *
 * @returns {module:ffi.CData}
 * A cdata holding the CTypeID handle (an integer type ID).
 *
 * @throws {Error}
 * Throws an exception if the type declaration is invalid.
 *
 * @example
 * // Get type ID for struct
 * ffi.cdef('struct point { int x; int y; };');
 * let point_type = ffi.typeof('struct point');
 *
 * @example
 * // Get type ID for function pointer
 * ffi.cdef('int callback(int, char *);');
 * let cb_type = ffi.typeof('int (*)(int, char *)');
 */
static uc_value_t *
uc_ffi_typeof(uc_vm_t *vm, size_t nargs)
{
	CTState *cts = ctype_cts(vm);
	CTypeID id = ffi_checkctype(vm, nargs, 0, cts, NULL);

	uc_value_t *res = uc_cdata_new(vm, CTID_CTYPEID, 4);

	*(CTypeID *)uc_cdata_dataptr(res) = id;

	return res;
}

/**
 * Get the size of a C type in bytes.
 *
 * The `sizeof()` function returns the size in bytes of a C type or cdata
 * expression. For variable-length arrays, an element count can be provided.
 *
 * @function module:ffi#sizeof
 *
 * @param {string|module:ffi.CData} type
 * The C type declaration or cdata expression to measure.
 *
 * @param {number} [nelem]
 * For variable-length arrays, the number of elements.
 *
 * @returns {?number}
 * The size in bytes, or `null` if the size is unknown.
 *
 * @throws {Error}
 * Throws an exception if the type is invalid or nelem is required but missing.
 *
 * @example
 * // Get size of primitive types
 * print(ffi.sizeof('int'));     // => 4
 * print(ffi.sizeof('double'));  // => 8
 *
 * @example
 * // Get size of struct
 * ffi.cdef('struct point { int x; int y; };');
 * print(ffi.sizeof('struct point'));  // => 8
 *
 * @example
 * // Get size of VLA with element count
 * ffi.cdef('int vla[];');
 * print(ffi.sizeof('int[]', 10));  // => 40 (10 * sizeof(int))
 */
static uc_value_t *
uc_ffi_sizeof(uc_vm_t *vm, size_t nargs)
{
	CTSize sz;

	sz = uc_ctype_sizeof_common(ctype_cts(vm), uc_fn_arg(0), uc_fn_arg(1));

	return (sz != CTSIZE_INVALID) ? ucv_uint64_new(sz) : NULL;
}

/**
 * Get the alignment requirement of a C type in bytes.
 *
 * The `alignof()` function returns the minimum alignment requirement in bytes
 * for a C type. This is useful for understanding structure padding and memory
 * layout.
 *
 * @function module:ffi#alignof
 *
 * @param {string} type
 * The C type declaration.
 *
 * @returns {number}
 * The alignment requirement in bytes (typically a power of 2).
 *
 * @throws {Error}
 * Throws an exception if the type is invalid.
 *
 * @example
 * // Get alignment of primitive types
 * print(ffi.alignof('int'));     // => 4
 * print(ffi.alignof('double'));  // => 8
 *
 * @example
 * // Get alignment of struct
 * ffi.cdef('struct foo { char a; int b; };');
 * print(ffi.alignof('struct foo'));  // => 4 (alignment of int member)
 */
static uc_value_t *
uc_ffi_alignof(uc_vm_t *vm, size_t nargs)
{
	CTState *cts = ctype_cts(vm);
	CTypeID id = ffi_checkctype(vm, nargs, 0, cts, NULL);

	CTSize sz;
	CTInfo info = uc_ctype_info_raw(cts, id, &sz);

	return ucv_uint64_new(1 << ctype_align(info));
}

/**
 * Get the offset of a struct field in bytes.
 *
 * The `offsetof()` function returns the byte offset of a field within a struct.
 * For bitfields, the bit position and bit size are returned in an array passed
 * as the third argument.
 *
 * @function module:ffi#offsetof
 *
 * @param {string} type
 * The struct type declaration.
 *
 * @param {string} field
 * The field name to get the offset of.
 *
 * @param {array} [bitpos]
 * Optional array to receive [bit_position, bit_size] for bitfield members.
 *
 * @returns {?number}
 * The byte offset of the field, or `null` if the field doesn't exist.
 *
 * @throws {Error}
 * Throws an exception if the type is not a struct or the field is invalid.
 *
 * @example
 * // Get field offset
 * ffi.cdef('struct point { int x; int y; };');
 * print(ffi.offsetof('struct point', 'x'));  // => 0
 * print(ffi.offsetof('struct point', 'y'));  // => 4
 *
 * @example
 * // Get bitfield info
 * ffi.cdef('struct flags { unsigned int a:4; unsigned int b:4; };');
 * let bitpos = [];
 * let offset = ffi.offsetof('struct flags', 'b', bitpos);
 * print(offset, bitpos[0], bitpos[1]);  // => 0 4 4
 */
static uc_value_t *
uc_ffi_offsetof(uc_vm_t *vm, size_t nargs)
{
	CTState *cts = ctype_cts(vm);
	CTypeID id = ffi_checkctype(vm, nargs, 0, cts, NULL);
	uc_value_t *name = uc_fn_arg(1);
	uc_value_t *bitpos = uc_fn_arg(2);
	CType *ct = uc_ctype_rawref(cts, id);
	CTSize ofs;

	if (!ctype_isstruct(ct->info) || ct->size == CTSIZE_INVALID)
		return NULL;

	if (ucv_type(name) != UC_STRING)
		return NULL;

	CType *fct = uc_ctype_getfield(cts, ct, name, &ofs);

	if (ctype_isfield(fct->info))
		return ucv_uint64_new(ofs);

	if (ctype_isbitfield(fct->info)) {
		ucv_array_set(bitpos, 0, ucv_uint64_new(ctype_bitpos(fct->info)));
		ucv_array_set(bitpos, 1, ucv_uint64_new(ctype_bitbsz(fct->info)));

		return ucv_uint64_new(ofs);
	}

	return NULL;
}

/**
 * Get or set the C `errno` value.
 *
 * The `errno()` function retrieves the current value of the C `errno`
 * variable, or sets it to a new value if an argument is provided.
 *
 * @function module:ffi#errno
 *
 * @param {number} [value]
 * Optional value to set errno to.
 *
 * @returns {number}
 * The current errno value (before any set operation).
 *
 * @example
 * // Get current errno
 * let err = ffi.errno();
 *
 * @example
 * // Set errno
 * ffi.errno(0);  // Clear errno
 */
static uc_value_t *
uc_ffi_errno(uc_vm_t *vm, size_t nargs)
{
	int err = errno;

	if (nargs)
		errno = ucv_int64_get(uc_fn_arg(0));

	return ucv_int64_new(err);
}

/**
 * Preloaded C types and variables.
 *
 * The FFI module automatically preloads certain C types and global variables
 * that are commonly needed. These are available without explicit `cdef()` declarations.
 *
 * ### Preloaded Global Variables
 *
 * The following global variables are automatically available through `ffi.C`:
 *
 * | Variable | Type | Description |
 * |----------|------|-------------|
 * | `errno` | `int *` | Thread-local error code pointer |
 * | `environ` | `char ***` | Process environment variables |
 *
 * Access these via `ffi.C.dlsym()`:
 *
 * ```javascript
 * // Get errno pointer
 * let errno_ptr = ffi.C.dlsym('errno');
 * let err = errno_ptr.deref('int');
 *
 * // Get environment variables
 * let env = ffi.C.dlsym('environ');
 * for (let i = 0; i < 10; i++) {
 *     let var = env.get(i);
 *     if (!var) break;
 *     print(ffi.string(var), "\n");
 * }
 * ```
 *
 * ### Builtin Type Definitions
 *
 * The following types are pre-declared and available without `cdef()`:
 *
 * | Type | Description | Typical Size |
 * |------|-------------|--------------|
 * | `size_t` | Unsigned pointer-sized integer | 4 or 8 bytes |
 * | `ssize_t` | Signed pointer-sized integer | 4 or 8 bytes |
 * | `intptr_t` | Signed integer with same size as pointer | 4 or 8 bytes |
 * | `uintptr_t` | Unsigned integer with same size as pointer | 4 or 8 bytes |
 * | `ptrdiff_t` | Signed difference type (pointer subtraction) | 4 or 8 bytes |
 * | `wchar_t` | Wide character type | 2 or 4 bytes |
 * | `va_list` | Variable argument list (for vararg functions) | Implementation-dependent |
 *
 * ### Fixed-Width Integer Types
 *
 * The following types from `<stdint.h>` are pre-declared:
 *
 * | Type | Description | Size |
 * |------|-------------|------|
 * | `int8_t` | Signed 8-bit integer | 1 byte |
 * | `int16_t` | Signed 16-bit integer | 2 bytes |
 * | `int32_t` | Signed 32-bit integer | 4 bytes |
 * | `int64_t` | Signed 64-bit integer | 8 bytes |
 * | `uint8_t` | Unsigned 8-bit integer | 1 byte |
 * | `uint16_t` | Unsigned 16-bit integer | 2 bytes |
 * | `uint32_t` | Unsigned 32-bit integer | 4 bytes |
 * | `uint64_t` | Unsigned 64-bit integer | 8 bytes |
 *
 * These types can be used directly without prior declaration:
 *
 * ```javascript
 * // Use builtin types directly
 * let sz = ffi.sizeof('size_t');        // => 8 (on 64-bit systems)
 * let ptr = ffi.ctype('uintptr_t', 0);
 *
 * // Use fixed-width types
 * let i32 = ffi.ctype('int32_t', 42);
 * let u64 = ffi.ctype('uint64_t', 0xFFFFFFFFFFFFFFFF);
 *
 * // Create arrays of builtin types
 * let buf = ffi.ctype('uint8_t[256]');
 * let indices = ffi.ctype('size_t[10]');
 * ```
 *
 * Note: When wrapping functions that use these types, you still need to
 * declare the function prototype via `cdef()` or provide a full declaration
 * to `wrap()`:
 *
 * ```javascript
 * // Declare function using builtin types
 * ffi.cdef('size_t strlen(const char *);');
 * let strlen = ffi.C.wrap('strlen');
 *
 * // Or provide full declaration to wrap()
 * let strlen = ffi.C.wrap('size_t strlen(const char *)');
 * ```
 *
 * @section Preloaded Types
 */

/**
 * Convert between ucode strings and C char arrays/pointers.
 *
 * The `string()` function has two modes:
 *
 * 1. **String to buffer**: Given a ucode string, creates a C char[] buffer
 *    containing the string plus null terminator. Returns a cdata that can be
 *    passed to C functions expecting `char*`.
 *
 * 2. **Pointer to string**: Given a char* cdata pointer, reads the C string
 *    and returns a ucode string. An optional length parameter can be provided
 *    to limit the maximum bytes read (reads up to `len` bytes or until null
 *    terminator, whichever comes first).
 *
 * @function module:ffi#string
 *
 * @param {string|module:ffi.CData} arg
 * A ucode string to convert to char[], or a char* cdata pointer to read.
 *
 * @param {number} [len]
 * Optional maximum length for reading C strings (reads up to `len` bytes
 * or until null terminator).
 *
 * @returns {string|module:ffi.CData}
 * When given a char* pointer: returns a ucode string.
 * When given a ucode string: returns a char[] cdata buffer.
 *
 * @throws {Error}
 * Throws an exception if the argument type is invalid.
 *
 * @example
 * // Convert ucode string to char[] buffer
 * let buf = ffi.string("hello");
 * // buf is now char[6] cdata (including null terminator)
 * // Can be passed to C functions expecting char*
 *
 * @example
 * // Read C string from char* pointer
 * ffi.cdef('char *getenv(char *);');
 * let ptr = ffi.C.wrap('char *getenv(char *)')("PATH");
 * let path = ffi.string(ptr);
 * print(path);  // => "/usr/bin:..."
 *
 * @example
 * // Read fixed-length string (no null terminator)
 * ffi.cdef('char *strncpy(char *, const char *, size_t);');
 * let src = ffi.string("hello world");
 * let dst = ffi.ctype('char[5]');
 * ffi.C.wrap('char *strncpy(char *, const char *, size_t)')(dst, src, 5);
 * let short_str = ffi.string(dst, 5);  // => "hello" (no null terminator)
 */
static uc_value_t *
uc_ffi_string(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *arg = uc_fn_arg(0);
	uc_value_t *len_arg = uc_fn_arg(1);
	CTState *cts = ctype_cts(vm);

	/* If argument is ucode string, create C char[] buffer */
	if (ucv_type(arg) == UC_STRING) {
		size_t len = ucv_string_length(arg) + 1;

		/* Create char[N] array type directly without parser invocation */
		CTypeID elem_type = CTID_CCHAR;  /* char element type */
		CTInfo array_info = CTINFO(CT_ARRAY, CTALIGN(0)) + elem_type;
		CTSize array_size = len;  /* Total size in bytes */

		/* Intern the array type */
		CTypeID array_typeid = uc_ctype_intern(cts, array_info, array_size);

		/* Create cdata instance */
		uc_value_t *arr = uc_cdata_new(vm, array_typeid, array_size);

		/* Copy string including null terminator */
		const char *src = ucv_string_get(arg);
		uint8_t *dst = (uint8_t *)cdataptr((GCcdata *)ucv_as_resource(arr)->data);
		memcpy(dst, src, len);

		return arr;
	}

	/* Otherwise, argument is a C pointer - extract address and read as string */
	void *p = NULL;
	size_t sz;

	if (nargs > 1) {
		/* With explicit max-length: accept any pointer type */
		uc_cconv_ct_tv(cts, ctype_get(cts, CTID_P_VOID), (uint8_t *)&p, arg,
			CCF_ARG(1), NULL);
		size_t max_len = ucv_uint64_get(len_arg);
		/* Read up to max_len bytes or until null terminator (like strncpy) */
		sz = strnlen((const char *)p, max_len);
	}
	else {
		/* Without length: extract pointer address and treat as char* */
		GCcdata *cd = ucv_resource_data(arg, "ffi.ctype");
		if (!cd) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"string or cdata pointer expected, got %s",
				ucv_typename(arg));

			return NULL;
		}

		/* Get pointer: arrays contain data directly, pointers contain address */
		CType *cd_ct = ctype_get(cts, cd->ctypeid);
		if (ctype_isrefarray(cd_ct->info)) {
			/* Array: data is directly in cdata, use array size as limit */
			p = cdataptr(cd);
			sz = strnlen((const char *)p, cd_ct->size);
		}
		else if (ctype_isptr(cd_ct->info)) {
			/* Pointer: dereference and read null-terminated string */
			p = *(void **)cdataptr(cd);
			if (!p)
				return ucv_string_new("");
			sz = strlen((const char *)p);
		}
		else {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"string or cdata pointer expected, got %s",
				ucv_typename(arg));

			return NULL;
		}
	}

	return ucv_string_new_length((const char *)p, sz);
}

/**
 * Copy memory between pointers.
 *
 * The `copy()` function copies memory from a source pointer to a destination
 * pointer. If the source is a ucode string, it copies the string including
 * its null terminator. Otherwise, an explicit length must be provided.
 *
 * @function module:ffi#copy
 *
 * @param {module:ffi.CData} dest
 * Destination pointer.
 *
 * @param {string|module:ffi.CData} src
 * Source string or pointer.
 *
 * @param {number} [len]
 * Number of bytes to copy. Required if src is not a string.
 *
 * @returns {undefined}
 * Returns `undefined`.
 *
 * @example
 * // Copy string (includes null terminator)
 * let buf = ffi.ctype('char[10]');
 * ffi.copy(buf, "hello");
 *
 * @example
 * // Copy memory with explicit length
 * let src = ffi.ctype('char[5]', [1, 2, 3, 4, 5]);
 * let dst = ffi.ctype('char[5]');
 * ffi.copy(dst, src, 5);
 */
static uc_value_t *
uc_ffi_copy(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *dp_arg = uc_fn_arg(0);
	void *dp = ffi_checkptr(vm, nargs, 0, CTID_P_VOID);
	uc_value_t *sp_arg = uc_fn_arg(1);
	void *sp = NULL;
	size_t len, dp_size, sp_size = SIZE_MAX;
	CTState *cts = ctype_cts(vm);

	/* Get destination buffer size for bounds checking */
	dp_size = ffi_cdata_bufsize(cts, dp_arg);

	/* Handle string source: copy directly from the string buffer */
	if (ucv_type(sp_arg) == UC_STRING) {
		const char *src = ucv_string_get(sp_arg);
		size_t src_len = ucv_string_length(sp_arg);

		/* Determine length: use explicit len if provided, else string + null */
		if (nargs > 2) {
			len = ucv_uint64_get(uc_fn_arg(2));
		} else {
			len = src_len + 1;
		}

		sp = (void *)src;
		sp_size = src_len + 1;
	} else {
		sp = ffi_checkptr(vm, nargs, 1, CTID_P_CVOID);
		if (!sp)
			return NULL;

		/* Get source buffer size for bounds checking */
		sp_size = ffi_cdata_bufsize(cts, sp_arg);

		if (nargs > 2) {
			len = ucv_uint64_get(uc_fn_arg(2));
		} else {
			len = strnlen((const char *)sp, sp_size);
		}
	}

	/* Cap length to destination buffer size */
	if (len > dp_size)
		len = dp_size;

	/* Cap length to source buffer size */
	if (len > sp_size)
		len = sp_size;

	memcpy(dp, sp, len);

	return NULL;
}

/**
 * Fill memory with a byte value.
 *
 * The `fill()` function sets `len` bytes at the destination pointer to
 * the specified fill value. The fill value can be a number, boolean,
 * or string (first character used).
 *
 * @function module:ffi#fill
 *
 * @param {module:ffi.CData} dest
 * Destination pointer.
 *
 * @param {number} len
 * Number of bytes to fill.
 *
 * @param {number|boolean|string} [value=0]
 * Fill value. Numbers/booleans use the value directly; strings use
 * the first character's ASCII code.
 *
 * @returns {undefined}
 * Returns `undefined`.
 *
 * @example
 * // Zero-fill a buffer
 * let buf = ffi.ctype('char[10]');
 * ffi.fill(buf, 10, 0);
 *
 * // Fill with specific byte
 * ffi.fill(buf, 10, 0xFF);
 *
 * // Fill with character
 * ffi.fill(buf, 10, 'A');  // Fills with 65 (ASCII for 'A')
 */
static uc_value_t *
uc_ffi_fill(uc_vm_t *vm, size_t nargs)
{
	void *dp = ffi_checkptr(vm, nargs, 0, CTID_P_VOID);
	size_t len = ucv_int64_get(uc_fn_arg(1));
	uc_value_t *fill = uc_fn_arg(2);
	int chr = 0;

	switch (ucv_type(fill))
	{
	case UC_INTEGER:
	case UC_DOUBLE:
		chr = ucv_int64_get(fill);
		break;

	case UC_BOOLEAN:
		chr = ucv_boolean_get(fill) ? 1 : 0;
		break;

	case UC_STRING:
		chr = ucv_string_get(fill)[0];
		break;

	default:
		chr = 0;
		break;
	}

	memset(dp, chr, len);

	return NULL;
}

/**
 * Cast a value to a different C type.
 *
 * The `cast()` function converts a value to a specified C type. It supports
 * casts to numbers, enums, and pointers. The cast is performed without
 * intermediate ucode type conversions.
 *
 * @function module:ffi#cast
 *
 * @param {string} type
 * The target C type declaration.
 *
 * @param {*} value
 * The value to cast. Can be a ucode value or cdata.
 *
 * @returns {module:ffi.CData}
 * A cdata of the target type holding the cast value.
 *
 * @throws {Error}
 * Throws an exception if the cast is invalid (e.g., casting to a struct).
 *
 * @example
 * // Cast number to pointer
 * let ptr = ffi.cast('void *', 0x1000);
 * print(ptr.get());  // => 4096
 *
 * @example
 * // Cast between pointer types
 * ffi.cdef('int x;');
 * let px = ffi.ctype('int *', ffi.ctype('int', 42).ptr());
 * let pv = ffi.cast('void *', px);
 *
 * @example
 * // Cast pointer to integer
 * let str = ffi.string("hello");
 * let addr = ffi.cast('uintptr_t', str.ptr());
 * print(addr.get());  // => address as number
 *
 * @example
 * // Cast integer to enum
 * ffi.cdef('enum color { RED, GREEN, BLUE };');
 * let c = ffi.cast('enum color', 2);  // => BLUE
 */
static uc_value_t *
uc_ffi_cast(uc_vm_t *vm, size_t nargs)
{
	CTState *cts = ctype_cts(vm);
	CTypeID id = ffi_checkctype(vm, nargs, 0, cts, NULL);
	CType *d = ctype_raw(cts, id);
	uc_value_t *init = uc_fn_arg(1);
	GCcdata *cd = ucv_cdata_data(init);

	if (!ctype_isnum(d->info) && !ctype_isptr(d->info) && !ctype_isenum(d->info)) {
		uc_value_t *repr = uc_ctype_repr(vm, id, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
		                      "invalid cast to type '%s', only casts to "
							  "numbers, enums or pointers are allowed",
							  ucv_string_get(repr));

		ucv_put(repr);

		return NULL;
	}

	if (cd && cd->ctypeid == id)
		return ucv_get(init);

	uc_value_t *res = uc_cdata_new(vm, id, d->size);
	uc_value_t *refs = NULL;

	/* when we're casting to pointer, keep references to original memory */
	if (cd && ctype_isptr(d->info)) {
		refs = ucv_array_new(vm);

		/* keep reference to original value itself */
		ucv_array_push(refs, ucv_get(init));

		/* merge original values references */
		uc_value_t *src_refs = cd->refs;

		for (size_t i = 0; i < ucv_array_length(src_refs); i++)
			ucv_array_push(refs, ucv_get(ucv_array_get(src_refs, i)));
	}

	uc_cconv_ct_tv(cts, d, uc_cdata_dataptr(res), init, CCF_CAST, &refs);

	cd = ucv_resource_data(res, "ffi.ctype");
	cd->refs = refs;

	return res;
}

/**
 * Cast a cdata to a different C type.
 *
 * The `.cast()` method converts a cdata to a specified C type. This is
 * equivalent to calling `ffi.cast(type, cdata)`. It supports casts to
 * numbers, enums, and pointers.
 *
 * @function module:ffi.CData#cast
 *
 * @param {string} type
 * The target C type declaration.
 *
 * @returns {module:ffi.CData}
 * A cdata of the target type holding the cast value.
 *
 * @throws {Error}
 * Throws an exception if the cast is invalid.
 *
 * @example
 * // Cast pointer to void*
 * let px = ffi.ctype('int *', ffi.ctype('int', 42).ptr());
 * let pv = px.cast('void *');
 *
 * @example
 * // Cast pointer to integer
 * let str = ffi.string("hello");
 * let addr = str.ptr().cast('uintptr_t');
 * print(addr.get());
 *
 * @see {@link module:ffi#cast|ffi.cast()}
 */
static uc_value_t *
uc_ctype_cast(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this_arg = _uc_fn_this_res(vm);
	GCcdata *cd = ucv_resource_data(this_arg, "ffi.ctype");

	if (!cd)
		return NULL;

	CTState *cts = ctype_cts(vm);
	CTypeID id = ffi_checkctype(vm, nargs, 0, cts, NULL);
	CType *d = ctype_raw(cts, id);

	if (!ctype_isnum(d->info) && !ctype_isptr(d->info) && !ctype_isenum(d->info)) {
		uc_value_t *repr = uc_ctype_repr(vm, id, NULL);

		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
		                      "invalid cast to type '%s', only casts to "
							  "numbers, enums or pointers are allowed",
							  ucv_string_get(repr));

		ucv_put(repr);

		return NULL;
	}

	if (cd->ctypeid == id)
		return ucv_get(this_arg);

	uc_value_t *res = uc_cdata_new(vm, id, d->size);
	uc_value_t *refs = NULL;

	/* when we're casting to pointer, keep references to original memory */
	if (ctype_isptr(d->info)) {
		refs = ucv_array_new(vm);

		/* keep reference to original value itself */
		ucv_array_push(refs, ucv_get(this_arg));

		/* merge original values references */
		uc_value_t *src_refs = cd->refs;

		for (size_t i = 0; i < ucv_array_length(src_refs); i++)
			ucv_array_push(refs, ucv_get(ucv_array_get(src_refs, i)));
	}

	uc_cconv_ct_tv(cts, d, uc_cdata_dataptr(res), this_arg, CCF_CAST, &refs);

	GCcdata *res_cd = ucv_resource_data(res, "ffi.ctype");
	res_cd->refs = refs;

	return res;
}

/**
 * Copy memory to a cdata from a source.
 *
 * The `.copy()` method copies memory from a source to this cdata. If the
 * source is a ucode string, it copies the string including its null
 * terminator. Otherwise, an explicit length can be provided.
 *
 * @function module:ffi.CData#copy
 *
 * @param {string|module:ffi.CData} src
 * Source string or pointer.
 *
 * @param {number} [len]
 * Number of bytes to copy. Required if src is not a string.
 *
 * @returns {undefined}
 * Returns `undefined`.
 *
 * @example
 * // Copy string into buffer
 * let buf = ffi.ctype('char[10]');
 * buf.copy("hello");
 *
 * @example
 * // Copy with explicit length
 * let src = ffi.ctype('char[5]', [1, 2, 3, 4, 5]);
 * let dst = ffi.ctype('char[5]');
 * dst.copy(src, 5);
 *
 * @see {@link module:ffi#copy|ffi.copy()}
 */
static uc_value_t *
uc_ctype_copy(uc_vm_t *vm, size_t nargs)
{
	GCcdata *cd = uc_fn_thisval("ffi.ctype");

	if (!cd)
		return NULL;

	uc_value_t *sp_arg = uc_fn_arg(0);
	uc_value_t *len_arg = uc_fn_arg(1);
	void *dp = cdataptr(cd);
	void *sp = NULL;
	size_t len, dp_size, sp_size = SIZE_MAX;
	CTState *cts = ctype_cts(vm);

	/* Get destination buffer size for bounds checking */
	dp_size = ffi_cdata_bufsize(cts, (uc_value_t *)cd);

	/* Handle string source: copy directly from the string buffer */
	if (ucv_type(sp_arg) == UC_STRING) {
		const char *src = ucv_string_get(sp_arg);
		size_t src_len = ucv_string_length(sp_arg);

		/* Determine length: use explicit len if provided, else string + null */
		if (nargs > 1) {
			len = ucv_uint64_get(len_arg);
		} else {
			len = src_len + 1;
		}

		sp = (void *)src;
		sp_size = src_len + 1;
	} else {
		sp = ffi_checkptr(vm, nargs, 0, CTID_P_CVOID);
		if (!sp)
			return NULL;

		/* Get source buffer size for bounds checking */
		sp_size = ffi_cdata_bufsize(cts, sp_arg);

		if (nargs > 1) {
			len = ucv_uint64_get(len_arg);
		} else {
			len = strnlen((const char *)sp, sp_size);
		}
	}

	/* Cap length to destination buffer size */
	if (len > dp_size)
		len = dp_size;

	/* Cap length to source buffer size */
	if (len > sp_size)
		len = sp_size;

	memcpy(dp, sp, len);

	return NULL;
}

/**
 * Convert a cdata to a ucode string.
 *
 * The `.string()` method reads a C string from a char* pointer or char[]
 * array and returns a ucode string. For char* pointers, it reads until the
 * null terminator. For char[] arrays, it reads up to the array length.
 * An optional length parameter can limit the bytes read.
 *
 * @function module:ffi.CData#string
 *
 * @param {number} [len]
 * Optional maximum length for reading C strings (reads up to `len` bytes
 * or until null terminator).
 *
 * @returns {string}
 * The extracted ucode string.
 *
 * @throws {Error}
 * Throws an exception if the cdata is not a char* or char[] type.
 *
 * @example
 * // Read from char* pointer
 * ffi.cdef('char *getenv(char *);');
 * let ptr = ffi.C.wrap('char *getenv(char *)')("PATH");
 * let path = ptr.string();
 * print(path);
 *
 * @example
 * // Read from char[] array
 * let buf = ffi.ctype('char[10]', "hello");
 * print(buf.string());  // => "hello"
 *
 * @example
 * // Read fixed-length string
 * let buf = ffi.ctype('char[10]', "hello world");
 * print(buf.string(5));  // => "hello"
 *
 * @see {@link module:ffi#string|ffi.string()}
 */
static uc_value_t *
uc_ctype_string(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *this_arg = _uc_fn_this_res(vm);
	GCcdata *cd = ucv_resource_data(this_arg, "ffi.ctype");

	if (!cd)
		return NULL;

	uc_value_t *len_arg = uc_fn_arg(0);
	CTState *cts = ctype_cts(vm);
	CType *cd_ct = ctype_get(cts, cd->ctypeid);
	void *p = NULL;
	size_t sz;

	/* Get pointer: arrays contain data directly, pointers contain address */
	if (ctype_isarray(cd_ct->info)) {
		/* Array: data is directly in cdata, use array size as limit */
		p = cdataptr(cd);
		if (nargs > 0) {
			size_t max_len = ucv_uint64_get(len_arg);
			sz = strnlen((const char *)p, max_len);
		} else {
			sz = strnlen((const char *)p, cd_ct->size);
		}
	} else if (ctype_isptr(cd_ct->info)) {
		/* Pointer: dereference and read null-terminated string */
		p = *(void **)cdataptr(cd);
		if (!p)
			return ucv_string_new("");

		if (nargs > 0) {
			size_t max_len = ucv_uint64_get(len_arg);
			sz = strnlen((const char *)p, max_len);
		} else {
			sz = strlen((const char *)p);
		}
	} else {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"string() requires char* or char[] type, got %s",
			ucv_typename(this_arg));

		return NULL;
	}

	return ucv_string_new_length((const char *)p, sz);
}

#if UC_TARGET_CYGWIN
#define CLIB_SOPREFIX "cyg"
#else
#define CLIB_SOPREFIX "lib"
#endif

#if defined(__APPLE__)
#define CLIB_SOEXT "%s.dylib"
#elif UC_TARGET_CYGWIN
#define CLIB_SOEXT "%s.dll"
#else
#define CLIB_SOEXT "%s.so"
#endif

/**
 * Load a shared library.
 *
 * The `dlopen()` function loads a shared library into the process address
 * space and returns a CLib object that can be used to access symbols via
 * `dlsym()` or `wrap()`.
 *
 *   ```javascript
 *   // Load zlib compression library
 *   let libz = ffi.dlopen('z');
 *
 *   // Load OpenSSL crypto library
 *   let libcrypto = ffi.dlopen('crypto');
 *
 *   // Load absolute path
 *   let custom = ffi.dlopen('/usr/local/lib/mylib.so');
 *
 *   // Use wrap() to get function pointers
 *   let zlibVersion = libz.wrap('const char *zlibVersion(void)');
 *   print(zlibVersion().slice(), "\n");  // => "1.2.11"
 *   ```
 *
 * On Unix-like systems, the `.so` extension is automatically appended if
 * omitted. On macOS, `.dylib` is used. On Windows, `.dll` is used.
 *
 * When the optional third argument is provided, `dlopen()` will:
 * - Parse the C definitions to register types and function prototypes
 * - Resolve and wrap all declared functions
 * - Attach the wrapped functions as methods on the library object
 *
 * @function module:ffi#dlopen
 *
 * @param {string} name
 * The library name or path.
 *
 * @param {boolean} [global=false]
 * If `true`, make symbols available to subsequently loaded libraries.
 *
 * @param {string} [cdefs]
 * Optional C declaration string containing types and function prototypes.
 * Function declarations will be automatically wrapped and attached to the
 * library object as methods.
 *
 * @returns {?module:ffi.CLib}
 * A CLib object representing the loaded library, or `null` on error.
 * When `cdefs` is provided, the returned CLib will have wrapped functions
 * attached as methods.
 *
 * @throws {Error}
 * Throws an exception if the library cannot be loaded or if C definitions
 * cannot be parsed.
 *
 * @example
 * // Load zlib and call functions
 * let libz = ffi.dlopen('z');
 * let zlibVersion = libz.wrap('const char *zlibVersion(void)');
 * print(zlibVersion().slice());
 *
 * @example
 * // Load OpenSSL crypto library
 * let libcrypto = ffi.dlopen('crypto');
 * let OpenSSL_version = libcrypto.wrap('const char *OpenSSL_version(int)');
 * print(OpenSSL_version(0).slice());  // => "OpenSSL 3.0.0..."
 *
 * @example
 * // Load library with automatic wrapping
 * let libssl = ffi.dlopen('ssl', false, `
 *     typedef void SSL_METHOD;
 *     const SSL_METHOD *TLS_method(void);
 * `);
 * // TLS_method is now directly callable
 * let method = libssl.TLS_method();
 *
 * @example
 * // Load zlib with pre-wrapped functions
 * let libz = ffi.dlopen('z', false, `
 *     const char *zlibVersion(void);
 *     uLong compressBound(uLong sourceLen);
 * `);
 * print(libz.zlibVersion().slice());
 * print(libz.compressBound(1024));
 */
static uc_value_t *
uc_ffi_dlopen(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *name = uc_fn_arg(0);
	uc_value_t *global = uc_fn_arg(1);
	uc_value_t *cdefs = uc_fn_arg(2);
	uc_value_t *clibs = uc_vm_registry_get(vm, "ffi.clibs");

	/* Handle dlopen(null) or dlopen("") case - returns global C library */
	if (!name || (ucv_type(name) == UC_STRING && !ucv_string_length(name))) {
		uc_value_t *global_lib = ucv_object_get(clibs, "", NULL);

		/* If cdefs provided, parse them and add wrapped functions to ffi.C prototype */
		if (cdefs && ucv_type(cdefs) == UC_STRING && ucv_string_length(cdefs)) {
			uc_ffi_clib_t *lib = ucv_resource_data(global_lib, "ffi.clib");
			uc_value_t *methods = ucv_prototype_get(global_lib);

			if (lib) {
				CTState *cts = ctype_cts(vm);
				CPState cp = {
					.uv_vm = vm,
					.cts = cts,
					.srcname = ucv_string_get(cdefs),
					.p = ucv_string_get(cdefs),
					.uv_param = NULL,
					.mode = CPARSE_MODE_MULTI | CPARSE_MODE_DIRECT,
					.func_ids = &cp.func_ids_buf,
					.func_ids_buf = { .count = 0, .entries = NULL },
					.error = NULL
				};

				if (!uc_cparse(&cp))
					return NULL;

				for (size_t i = 0; i < cp.func_ids_buf.count; i++) {
					CTypeID id = cp.func_ids_buf.entries[i];
					CType *ct = ctype_get(cts, id);
					if (!ct || !ct->uv_name || ucv_type(ct->uv_name) != UC_STRING)
						continue;

					const char *symname = ucv_string_get(ct->uv_name);
					void *fp = dlsym(RTLD_DEFAULT, symname);
					if (!fp)
						continue;

					CTypeID cid = ctype_typeid(cts, ct);

					uc_value_t *wrapped = clib_wrap_new(vm, cid, fp);

					if (wrapped)
						ucv_object_add(methods, symname, wrapped);
				}
				uc_vector_clear(&cp.func_ids_buf);
			}
		}

		return ucv_get(global_lib);
	}

	if (ucv_type(name) != UC_STRING)
		return NULL;

	char *path = ucv_string_get(name);
	char *s = path;

	/* relative name provided */
	if (!strchr(path, '/') && !strchr(path, '\\') && !strchr(path, '.'))
		xasprintf(&s, CLIB_SOPREFIX CLIB_SOEXT, path);

	int mode = RTLD_LAZY | (ucv_is_truish(global) ? RTLD_GLOBAL : RTLD_LOCAL);
	void *dlh = dlopen(s, mode);

	if (!dlh) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
			"unable to load library '%s' (%s): %s",
			path, s, dlerror());

		if (s != path)
			free(s);

		return NULL;
	}

	/* Per-name instance caching: check if this library name already exists */
	uc_value_t *cached = ucv_object_get(clibs, s, NULL);

	if (cached) {
		dlclose(dlh);

		if (s != path)
			free(s);

		return ucv_get(cached);
	}

	uc_ffi_clib_t *lib;

	/* Create instance prototype - wrapped functions live here as instance methods */
	uc_value_t *methods = ucv_object_new(vm);

	size_t libnamesize = strlen(s) + 1;
	size_t datasize = ((sizeof(uc_ffi_clib_t) + libnamesize + 7) / 8) * 8;

	uc_value_t *lib_obj = ucv_resource_new_with_proto(
		vm, ucv_resource_type_lookup(vm, "ffi.clib"),
		(void **)&lib, 0, datasize, methods);

	lib->cache = ucv_object_new(vm);
	lib->dlh = dlh;

	/* Copy library name into the allocated block right after the struct */
	lib->name = memcpy((char *)lib + sizeof(uc_ffi_clib_t), s, libnamesize);

	if (s != path)
		free(s);

	/* If cdefs provided, parse them and wrap functions */
	if (cdefs && ucv_type(cdefs) == UC_STRING && ucv_string_length(cdefs)) {
		CTState *cts = ctype_cts(vm);
		CPState cp = {
			.uv_vm = vm,
			.cts = cts,
			.srcname = ucv_string_get(cdefs),
			.p = ucv_string_get(cdefs),
			.uv_param = NULL,
			.mode = CPARSE_MODE_MULTI | CPARSE_MODE_DIRECT,
			.func_ids = &cp.func_ids_buf,
			.func_ids_buf = { .count = 0, .entries = NULL },
			.error = NULL
		};

		if (!uc_cparse(&cp)) {
			if (lib->dlh != RTLD_DEFAULT)
				dlclose(lib->dlh);

			ucv_put(lib_obj);

			return NULL;
		}

		for (size_t i = 0; i < cp.func_ids_buf.count; i++) {
			CTypeID id = cp.func_ids_buf.entries[i];
			CType *ct = ctype_get(cts, id);
			if (!ct)
				continue;

			if (!ct->uv_name || ucv_type(ct->uv_name) != UC_STRING)
				continue;

			const char *symname = ucv_string_get(ct->uv_name);

			void *fp = dlsym(dlh, symname);
			if (!fp) {
				uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
					"unable to resolve symbol '%s' in library '%s'",
					symname, lib->name);

				if (lib->dlh != RTLD_DEFAULT)
					dlclose(lib->dlh);

				ucv_put(lib_obj);

				return NULL;
			}

			CTypeID cid = ctype_typeid(cts, ct);

			uc_value_t *wrapped = clib_wrap_new(vm, cid, fp);

			if (wrapped)
				ucv_object_add(methods, symname, wrapped);
		}

		uc_vector_clear(&cp.func_ids_buf);
	}

	ucv_object_add(clibs, lib->name, ucv_get(lib_obj));

	return lib_obj;
}

/**
 * Import a C library with automatic function wrapping.
 *
 * This is a convenience function that combines library loading, type
 * declaration, and function wrapping into a single call. It loads the
 * specified library, parses the C definitions, and returns an object
 * with all functions pre-wrapped and ready to call.
 *
 * @function module:ffi#import
 *
 * @param {string} libname
 * The library name or path to load. Can be a bare name (e.g., 'z'),
 * a filename (e.g., 'libcrypto.so.3'), or an absolute path.
 *
 * @param {string} cdefs
 * A C declaration string containing function prototypes to import.
 * Only function declarations are wrapped; types, structs, and other
 * declarations are registered but not added to the result object.
 *
 * @returns {object|null}
 * An object containing wrapped functions keyed by their symbol names.
 * Returns null if the library cannot be loaded or if parsing fails.
 *
 * @throws {Error}
 * Throws an exception if:
 * - The library cannot be loaded
 * - The C declarations are syntactically invalid
 * - A declared function cannot be resolved in the library
 *
 * @example
 * // Import sqlite3 with all functions
 * let sqlite3 = ffi.import('sqlite3', `
 *     const char *sqlite3_libversion(void);
 *     int sqlite3_libversion_number(void);
 *     int sqlite3_open(const char *, void **);
 *     int sqlite3_close(void *);
 * `);
 *
 * print("Version: ", sqlite3.sqlite3_libversion(), "\n");
 *
 * @example
 * // Import zlib functions
 * let zlib = ffi.import('z', `
 *     const char *zlibVersion(void);
 *     uLong compressBound(uLong sourceLen);
 * `);
 *
 * print(zlib.zlibVersion());
 * print(zlib.compressBound(1024));
 */
static uc_value_t *
uc_ffi_import(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *libname = uc_fn_arg(0);
	uc_value_t *cdefs = uc_fn_arg(1);
	CTState *cts = ctype_cts(vm);

	if (!libname || ucv_type(libname) != UC_STRING)
		return NULL;

	if (!cdefs || ucv_type(cdefs) != UC_STRING)
		return NULL;

	/* Load the library */
	char *path = ucv_string_get(libname);
	void *dlh = dlopen(path, RTLD_LAZY | RTLD_LOCAL);

	if (!dlh) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
			"unable to load library '%s' (%s): %s",
			ucv_string_get(libname), path, dlerror());

		return NULL;
	}

	/* Parse C definitions to register types */
	CPState cp = {
		.uv_vm = vm,
		.cts = cts,
		.srcname = ucv_string_get(cdefs),
		.p = ucv_string_get(cdefs),
		.uv_param = NULL,
		.mode = CPARSE_MODE_MULTI | CPARSE_MODE_DIRECT,
		.func_ids = &cp.func_ids_buf
	};

	if (!uc_cparse(&cp)) {
		dlclose(dlh);
		return NULL;
	}

	/* Create result object */
	uc_value_t *result = ucv_object_new(vm);

	/* Iterate over recorded function type IDs */
	for (size_t i = 0; i < cp.func_ids_buf.count; i++) {
		CTypeID id = cp.func_ids_buf.entries[i];
		CType *ct = ctype_get(cts, id);
		if (!ct)
			continue;

		/* Get the function name */
		if (!ct->uv_name || ucv_type(ct->uv_name) != UC_STRING)
			continue;

		const char *symname = ucv_string_get(ct->uv_name);

		/* Resolve the symbol from the library */
		void *fp = dlsym(dlh, symname);
		if (!fp) {
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
				"unable to resolve symbol '%s' in library", symname);

			ucv_put(result);
			dlclose(dlh);
			return NULL;
		}

		/* Wrap the function as an "ffi.cfn" resource */
		CTypeID cid = ctype_typeid(cts, ct);

		uc_value_t *wrapped = clib_wrap_new(vm, cid, fp);

		if (wrapped)
			ucv_object_add(result, symname, wrapped);
	}

	uc_vector_clear(&cp.func_ids_buf);
	dlclose(dlh);

	return result;
}


static const uc_function_list_t clib_fns[] = {
	{ "dlsym",		uc_clib_dlsym },
	{ "resolve",		uc_clib_resolve },
	{ "wrap",		uc_clib_wrap },
};

static const uc_function_list_t ctype_fns[] = {
	{ "call",		uc_ctype_call },
	{ "free",		uc_ctype_free },
	{ "get",		uc_ctype_get },
	{ "set",		uc_ctype_set },
	{ "ptr",		uc_ctype_ptr },
	{ "index",		uc_ctype_index },
	{ "deref",		uc_ctype_deref },
	{ "size",		uc_ctype_sizeof },
	{ "length",		uc_ctype_length },
	{ "itemsize",		uc_ctype_itemsize },
	{ "slice",		uc_ctype_slice },
	{ "tostring",		uc_ctype_tostring },
	{ "cast",		uc_ctype_cast },
	{ "copy",		uc_ctype_copy },
	{ "string",		uc_ctype_string },
	{ "__get__",		uc_ctype_meta_get },
	{ "__set__",		uc_ctype_meta_set },
};

static const uc_function_list_t closure_fns[] = {
	{ "ptr",		uc_closure_ptr },
	{ "tostring",		uc_closure_tostring },
	{ "cast",		uc_closure_cast },
};

static const uc_function_list_t global_fns[] = {
	{ "ctype",		uc_ffi_ctype },
	{ "cdef",		uc_ffi_cdef },
	{ "typeof",		uc_ffi_typeof },
	{ "sizeof",		uc_ffi_sizeof },
	{ "alignof",		uc_ffi_alignof },
	{ "offsetof",		uc_ffi_offsetof },
	{ "errno",		uc_ffi_errno },
	{ "string",		uc_ffi_string },
	{ "copy",		uc_ffi_copy },
	{ "fill",		uc_ffi_fill },
	{ "cast",		uc_ffi_cast },
	{ "dlopen",		uc_ffi_dlopen },
	{ "import",		uc_ffi_import },
	{ "closure",		uc_ffi_closure },
};


static void
close_clib(void *ud)
{
	uc_ffi_clib_t *clib = ud;

	ucv_put(clib->cache);

	if (clib->dlh != RTLD_DEFAULT)
		dlclose(clib->dlh);
}

static void
close_ctype(void *ud)
{
	GCcdata *cd = ud;

	/* ucode does not create libffi closure cdata objects;
	 * closures are created transiently for callback arguments. */

	if (cd->refs)
		ucv_put(cd->refs);
}


extern char **environ;

static void
preload_type(uc_vm_t *vm, uc_ffi_clib_t *lib, const char *cdef, void *val)
{
	CTState *cts = ctype_cts(vm);
	uc_value_t *def = ucv_string_new(cdef);
	CTypeID cid = uv_to_ct(vm, CPARSE_MODE_DIRECT | CPARSE_MODE_NOIMPLICIT | CPARSE_MODE_MULTI, def, NULL);

	ucv_put(def);

	if (!cid)
		return;

	CType *ct = ctype_get(cts, cid);

	if (!ct || ucv_type(ct->uv_name) != UC_STRING)
		return;

	uc_value_t *sym = uc_cdata_new(vm, cid, CTSIZE_PTR);

	*(void **)uc_cdata_dataptr(sym) = val;

	ucv_object_add(lib->cache, ucv_string_get(ct->uv_name), sym);
}

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_ctype_init(vm);

	uc_type_declare(vm, "ffi.clib", clib_fns, close_clib);
	uc_type_declare(vm, "ffi.ctype", ctype_fns, close_ctype);
	uc_type_declare(vm, "ffi.closure", closure_fns, close_closure);

	/* Wrapped C functions are "ffi.cfn" resources; the shared prototype
	 * carries a __call__ metamethod so the value is invocable like a
	 * function. The context (C type id + symbol pointer) lives in the
	 * resource data area, not smuggled after a cfunction's name[]. */
	{
		uc_value_t *cfn_proto = ucv_object_new(vm);

		ucv_object_add(cfn_proto, "__call__",
			ucv_cfunction_new("ffi.cfn.__call__", clib_wrapped_call));

		ucv_resource_type_add(vm, "ffi.cfn", cfn_proto, NULL);
	}

	uc_function_list_register(scope, global_fns);

	uc_value_t *clibs = ucv_object_new(vm);

	uc_vm_registry_set(vm, "ffi.clibs", clibs);

	uc_ffi_clib_t *C;
	uc_value_t *global_proto = ucv_object_new(vm);

	uc_value_t *stdlib = ucv_resource_new_with_proto(
		vm, ucv_resource_type_lookup(vm, "ffi.clib"),
		(void **)&C, 0, sizeof(*C), global_proto);

	C->dlh = RTLD_DEFAULT;
	C->name = NULL;
	C->cache = ucv_object_new(vm);

	ucv_object_add(scope, "C", stdlib);
	ucv_object_add(clibs, "", ucv_get(stdlib));

	/* preload global variables */
	preload_type(vm, C, "int *errno", &errno);
	preload_type(vm, C, "char **environ", environ);
}

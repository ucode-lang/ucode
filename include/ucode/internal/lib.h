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

/* Internal library / stdlib entry points. */

#ifndef UCODE_INTERNAL_LIB_H
#define UCODE_INTERNAL_LIB_H

#include "ucode/lib.h"
#include "ucode/internal/util.h"

__hidden extern const uc_function_list_t uc_stdlib_functions[];

/* The slots of the standard library functions.
 *
 * I_STCALL encodes the slot of its callee, so these values are part of the
 * bytecode ABI: new functions are appended below UC_FN_COUNT, existing ones
 * are never renumbered, and retiring a function means dropping its
 * initializer from uc_stdlib_functions[], leaving the slot reserved and empty.
 *
 * Each slot is pinned to its function there by a designated initializer, so
 * the order the table is written in does not move any slot, and a slot can
 * never silently name another function. */
typedef enum {
	UC_FN_CHR        = 0,
	UC_FN_DIE        = 1,
	UC_FN_EXISTS     = 2,
	UC_FN_RAWGET     = 3,
	UC_FN_RAWSET     = 4,
	UC_FN_RAWDELETE  = 5,
	UC_FN_EXIT       = 6,
	UC_FN_FILTER     = 7,
	UC_FN_GETENV     = 8,
	UC_FN_HEX        = 9,
	UC_FN_INDEX      = 10,
	UC_FN_INT        = 11,
	UC_FN_JOIN       = 12,
	UC_FN_KEYS       = 13,
	UC_FN_LC         = 14,
	UC_FN_LENGTH     = 15,
	UC_FN_LTRIM      = 16,
	UC_FN_MAP        = 17,
	UC_FN_ORD        = 18,
	UC_FN_POP        = 19,
	UC_FN_PRINT      = 20,
	UC_FN_PUSH       = 21,
	UC_FN_REVERSE    = 22,
	UC_FN_RINDEX     = 23,
	UC_FN_RTRIM      = 24,
	UC_FN_SHIFT      = 25,
	UC_FN_SORT       = 26,
	UC_FN_SPLICE     = 27,
	UC_FN_SLICE      = 28,
	UC_FN_SPLIT      = 29,
	UC_FN_SUBSTR     = 30,
	UC_FN_TIME       = 31,
	UC_FN_TRIM       = 32,
	UC_FN_TYPE       = 33,
	UC_FN_UCHR       = 34,
	UC_FN_UC         = 35,
	UC_FN_UNSHIFT    = 36,
	UC_FN_VALUES     = 37,
	UC_FN_SPRINTF    = 38,
	UC_FN_PRINTF     = 39,
	UC_FN_REQUIRE    = 40,
	UC_FN_IPTOARR    = 41,
	UC_FN_ARRTOIP    = 42,
	UC_FN_MATCH      = 43,
	UC_FN_REPLACE    = 44,
	UC_FN_JSON       = 45,
	UC_FN_INCLUDE    = 46,
	UC_FN_WARN       = 47,
	UC_FN_SYSTEM     = 48,
	UC_FN_TRACE      = 49,
	UC_FN_PROTO      = 50,
	UC_FN_SLEEP      = 51,
	UC_FN_ASSERT     = 52,
	UC_FN_RENDER     = 53,
	UC_FN_REGEXP     = 54,
	UC_FN_WILDCARD   = 55,
	UC_FN_SOURCEPATH = 56,
	UC_FN_MIN        = 57,
	UC_FN_MAX        = 58,
	UC_FN_B64DEC     = 59,
	UC_FN_B64ENC     = 60,
	UC_FN_UNIQ       = 61,
	UC_FN_LOCALTIME  = 62,
	UC_FN_GMTIME     = 63,
	UC_FN_TIMELOCAL  = 64,
	UC_FN_TIMEGM     = 65,
	UC_FN_CLOCK      = 66,
	UC_FN_HEXDEC     = 67,
	UC_FN_HEXENC     = 68,
	UC_FN_GC         = 69,
	UC_FN_LOADSTRING = 70,
	UC_FN_LOADFILE   = 71,
	UC_FN_CALL       = 72,
	UC_FN_SIGNAL     = 73,
	UC_FN_STRFTIME   = 74,
	UC_FN_STRPTIME   = 75,

	/* Number of slots; keeps the table and this enum in step, see lib.c. */
	UC_FN_COUNT       = 76
} uc_stdlib_fn_t;

__hidden extern const uc_function_list_t uc_stdlib_functions[];

/* Resolve a bytecode slot to its function, for the VM; returns NULL for a slot
 * beyond the table - bytecode built against an older one - and for an empty
 * slot, i.e. a function retired since that bytecode was compiled. */
__hidden uc_cfn_ptr_t uc_stdlib_function_by_slot(size_t slot);

/* Resolve a bytecode slot to its function's name, for the trace output and the
 * disassembly listing; NULL as above, and also for an empty slot.
 * Cross-library: called by modules (e.g. debug) from libucode, so it must stay exported. */
const char *uc_stdlib_function_name_by_slot(size_t slot);

/* Cross-library: called by modules (e.g. debug) from libucode, so it must stay exported. */
bool uc_source_context_format(uc_stringbuf_t *buf, uc_source_t *src, size_t off, bool compact);

__hidden bool uc_error_context_format(uc_stringbuf_t *buf, uc_source_t *src,
	uc_value_t *stacktrace, size_t startoff, size_t endoff, size_t off);
__hidden void uc_error_message_indent(char **msg);

__hidden uc_value_t *uc_require_library(uc_vm_t *vm, uc_value_t *nameval, bool module_mode);

#endif /* UCODE_INTERNAL_LIB_H */

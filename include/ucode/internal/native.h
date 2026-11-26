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

#ifndef UCODE_INTERNAL_NATIVE_H
#define UCODE_INTERNAL_NATIVE_H

#include <stdint.h>
#include <stdio.h>

#include "ucode/compiler.h"
#include "ucode/vm.h"
#include "ucode/internal/types.h"
#include "ucode/internal/util.h"

/* The tables below are emitted by the LLVM backend as IR constants and read
 * by the runtime, so their layout is part of the ABI between both. They hold
 * only pointers and fixed-width integers, in an order without padding
 * differences between targets. Strings are offsets into the string block of
 * the program, and the other arrays of a function or source are indexes into
 * the arrays of its unit, so that only the unit and program tables hold
 * pointers that the dynamic linker relocates at startup. */

#define UC_NATIVE_ABI_VERSION 1

#define UC_NATIVE_NONE			UINT32_MAX

#define UC_NATIVE_CONST_STRING	0
#define UC_NATIVE_CONST_INT		1
#define UC_NATIVE_CONST_DOUBLE	2
#define UC_NATIVE_CONST_NULL	3

#define UC_NATIVE_FN_ARROW		(1u << 0)
#define UC_NATIVE_FN_VARARG		(1u << 1)
#define UC_NATIVE_FN_STRICT		(1u << 2)
#define UC_NATIVE_FN_MODULE		(1u << 3)

#define UC_NATIVE_VAR_UPVAL		(1u << 31)

#define UC_NATIVE_UNIT_TEMPLATE	(1u << 0)
#define UC_NATIVE_UNIT_MODULE	(1u << 1)

typedef struct uc_native_frame uc_native_frame_t;

typedef void (*uc_native_entry_t)(uc_vm_t *, uc_native_frame_t *);

/* `length` is the length of a string, and 1 for a negative integer */
typedef struct {
	uint64_t bits;
	uint32_t str;
	uint32_t type;
	uint32_t length;
	uint32_t reserved;
} uc_native_constant_t;

typedef struct {
	uint32_t name;
	/* debuginfo.offsets as pairs of byte and instruction counts, in the
	 * bytes of the unit */
	uint32_t offsets;
	/* debuginfo.variables as from, to, slot and name index, and
	 * debuginfo.varnames as strings, in the words of the unit */
	uint32_t variables;
	uint32_t varnames;
	uint32_t noffsets;
	uint32_t nvariables;
	uint32_t nvarnames;
	uint32_t nargs;
	uint32_t nupvals;
	uint32_t flags;
	uint32_t srcidx;
	uint32_t srcpos;
} uc_native_function_t;

typedef struct {
	uint32_t filename;
	uint32_t runpath;
	uint32_t text;
	/* in the bytes of the unit */
	uint32_t lineinfo;
	/* the entries of uc_source_t.exports, in the exports of the unit */
	uint32_t exports;
	uint32_t textlen;
	uint32_t nlineinfo;
	uint32_t nexports;
	uint32_t flags;
} uc_native_source_t;

typedef struct {
	const uc_native_source_t *sources;
	const uc_native_constant_t *constants;
	const uc_native_function_t *functions;
	/* the native function of each entry of `functions` */
	const uc_native_entry_t *entries;
	const uc_native_constant_t *exports;
	const uint32_t *words;
	const uint8_t *bytes;
	/* normalised path that loaders find the unit by; none for the entry */
	uint32_t path;
	/* the compile error of a script that failed to compile */
	uint32_t error;
	uint32_t nsources;
	uint32_t nconstants;
	uint32_t nfunctions;
	uint32_t flags;
} uc_native_unit_t;

typedef struct {
	const char *strings;
	const uc_native_unit_t *units;
	uint32_t script_name;
	uint32_t version;
	uint32_t nunits;
	uint32_t flags;
} uc_native_program_t;

typedef struct {
	const char *path;
	const char *logical;
	bool template;
} uc_llvm_embed_t;

uc_declare_vector(uc_llvm_embeds_t, uc_llvm_embed_t);

typedef struct {
	const char *triple;
	const char *script_name;
	uc_parse_config_t *config;
	uc_llvm_embeds_t *embeds;
	bool debug;
	const char *map;
} uc_llvm_options_t;

bool uc_program_write_llvm(uc_program_t *program, FILE *out,
                           const uc_llvm_options_t *opts, char **errp);

#define UC_NATIVE_MAP_MAGIC "ucode-map 1\n"

/* Copies `in` to `out`, with the stack traces of a native program built
 * without debug information resolved through the map file `map`. */
int uc_native_trace_decode(const char *map, FILE *in, FILE *out);

/* Runtime state; the generated code passes these structures on by pointer
 * and never reads them. */

/* Allocated with malloc() and released with the function. The functions of
 * the program by id - 1 are part of the allocation for the entry function,
 * and the other functions of the program point to them. */
typedef struct uc_native_function_info {
	uc_native_entry_t entry;
	uc_function_t **functions;
} uc_native_function_info_t;

struct uc_native_frame {
	uc_native_frame_t *prev;
	uc_vm_t *vm;
	uc_value_t **slots;
	uc_upvalref_t **cells;
	size_t nslots;
	uc_closure_t *closure;
	/* the closure of the caller, which only the caller references once a
	 * tail call replaced it in `closure` and in the call frame */
	uc_closure_t *kept;
	uc_value_t *ctx;
	uc_value_t **args;
	size_t nargs;
	bool mcall;
	/* whether a slot has a cell */
	bool captured;
	uc_value_t *result;
	struct {
		uc_value_t *fn;
		uc_value_t *ctx;
		uc_value_t **args;
		size_t nargs;
		bool mcall;
		bool pending;
	} tail;
};

__hidden uc_value_t **uc_native_slot(uc_native_frame_t *frame, size_t slot);
__hidden void uc_native_slot_set(uc_native_frame_t *frame, size_t slot, uc_value_t *value);
__hidden uc_value_t *uc_native_slot_take(uc_native_frame_t *frame, size_t slot);
__hidden void uc_native_slot_clear(uc_native_frame_t *frame, size_t slot);
__hidden uc_upvalref_t *uc_native_slot_capture(uc_native_frame_t *frame, size_t slot);
__hidden void uc_native_pc_set(uc_vm_t *vm, uint32_t pc);

__hidden uc_value_t *uc_native_invoke(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx,
                                      bool mcall, uc_value_t **args, size_t nargs);

__hidden int uc_native_main(int argc, char **argv, const uc_native_program_t *program);

/* uc_vm_signal_dispatch(), without a call while no signal is pending */
static inline bool
uc_native_signals_dispatch(uc_vm_t *vm)
{
	for (size_t i = 0; i < ARRAY_SIZE(vm->signal.raised); i++)
		if (vm->signal.raised[i])
			return uc_vm_signal_dispatch(vm) == EXCEPTION_NONE;

	return true;
}

/* Small helpers that LLVM inlines into the generated code when the runtime is
 * linked with the program as bitcode; the cost model of LLVM would keep them
 * out of line because of their many call sites. */
#ifdef __clang__
#define UC_NATIVE_INLINE __attribute__((always_inline))
#else
#define UC_NATIVE_INLINE
#endif

/* Helpers whose body at each call site costs more space than it saves time;
 * in ucode-mdns, inlining them grew the code by 25 kB at -Os */
#define UC_NATIVE_NOINLINE __attribute__((noinline))

/* ucv_put() without a call for the values that have no reference count */
UC_NATIVE_INLINE static inline void
uc_native_unref(uc_value_t *v)
{
	if (ucv_is_counted(v))
		ucv_put(v);
}

/* The functions that generated code calls, see native.c and native_insn.c.
 * They take the VM from the frame, which saves an argument at each call. */

__hidden void uc_native_enter(uc_native_frame_t *frame, uc_value_t **slots,
                              uc_upvalref_t **cells, uint32_t nslots);
/* the cells of the functions that no closure or export captures from */
#define UC_NATIVE_NOCELLS	64

__hidden extern uc_upvalref_t *const uc_native_nocells[UC_NATIVE_NOCELLS];

__hidden void uc_native_leave(uc_native_frame_t *frame);
__hidden void uc_native_frame_release(uc_native_frame_t *frame, size_t nused);
__hidden bool uc_native_catch(uc_native_frame_t *frame, uint32_t slot);
__hidden bool uc_native_safepoint(uc_native_frame_t *frame, uint32_t pc);

#define UC_NATIVE_INSN_ARGS \
	uc_native_frame_t *f, uint32_t pc, int32_t d, uint32_t arg, const int32_t *x

__hidden void uc_native_load(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_load8(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_load16(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_load32(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_lthis(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_lrexp(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_lnull(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_ltrue(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_lfalse(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_lloc(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_lupv(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_lvar(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_lval(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_pval(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_clfn(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_arfn(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_sloc(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_supv(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_svar(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_sval(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_uloc(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_uupv(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_uvar(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_uval(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_narr(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_parr(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_marr(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_nobj(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_sobj(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_mobj(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_bor(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_bxor(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_band(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_eqs(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_nes(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_eq(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_ne(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_lt(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_le(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_gt(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_ge(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_in(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_lshift(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_rshift(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_add(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_sub(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_mul(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_div(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_mod(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_exp(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_not(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_compl(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_plus(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_minus(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_jmpz(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_jmpnt(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_copy(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_pop(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_cupv(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_return(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_call(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_tailcall(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_print(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_nextk(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_nextkv(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_delete(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_import(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_export(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_dynload(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_stcall(UC_NATIVE_INSN_ARGS);

/* Helpers of instruction sequences, see native_insn.c */

#define UC_NATIVE_GETK_KEEP		(1u << 31)
#define UC_NATIVE_GETK_UPVAL	(1u << 31)
#define UC_NATIVE_GETK_STACK	UINT32_MAX

__hidden bool uc_native_getk(uc_native_frame_t *f, uint32_t pc, int32_t d,
                             uint32_t arg, uint32_t src);
__hidden void uc_native_popn(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_callpop(UC_NATIVE_INSN_ARGS);
__hidden void uc_native_slocpop(UC_NATIVE_INSN_ARGS);

#define UC_NATIVE_LCALL_SLOT	0x7fff0000u
#define UC_NATIVE_COPYJMPZ_POP	(1u << 31)

__hidden bool uc_native_lcall(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_lcallpop(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_copyjmpz(UC_NATIVE_INSN_ARGS);

/* each half of the argument of uc_native_load2() is a kind and an index */
#define UC_NATIVE_LOAD2_KIND_SHIFT	14
#define UC_NATIVE_LOAD2_INDEX		0x3fff
#define UC_NATIVE_LOAD2_LLOC		0
#define UC_NATIVE_LOAD2_LUPV		1
#define UC_NATIVE_LOAD2_CONST		2

__hidden void uc_native_load2(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_jnull(UC_NATIVE_INSN_ARGS);
__hidden bool uc_native_notjmpz(UC_NATIVE_INSN_ARGS);

#endif /* UCODE_INTERNAL_NATIVE_H */

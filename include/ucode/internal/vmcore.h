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

/* VM operations that do not depend on bytecode. Operands that the interpreter
 * decodes from the instruction stream are passed as parameters. */

#ifndef UCODE_INTERNAL_VMCORE_H
#define UCODE_INTERNAL_VMCORE_H

#include "ucode/internal/vm.h"
#include "ucode/internal/program.h"
#include "ucode/internal/util.h"

extern const char *exception_type_strings[];

typedef char uc_vm_main_key_t[sizeof(UC_PROGRAM_MAIN_KEY) + sizeof("0x") + 2 * sizeof(void *)];

typedef struct {
	void (*frame_push)(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx,
	                   bool mcall, size_t stackoff);
	uc_vm_status_t (*execute)(uc_vm_t *vm);
} uc_vm_interpreter_t;

/* Runs a closure of native code that uc_vm_call() found on the stack with its
 * arguments, and leaves its result there as the interpreter does. */
typedef void (*uc_vm_native_call_t)(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx,
                                    bool mcall, size_t nargs);

static inline uc_chunk_t *
uc_vm_frame_chunk(uc_callframe_t *frame)
{
	return frame->closure ? &frame->closure->function->chunk : NULL;
}

static inline uc_program_t *
uc_vm_frame_program(uc_callframe_t *frame)
{
	return frame->closure ? frame->closure->function->program : NULL;
}

static inline uc_source_t *
uc_vm_frame_source(uc_callframe_t *frame)
{
	return frame->closure ? uc_program_function_source(frame->closure->function) : NULL;
}

static inline uc_callframe_t *
uc_vm_current_frame(uc_vm_t *vm)
{
	return uc_vector_last(&vm->callframes);
}

static inline uc_program_t *
uc_vm_current_program(uc_vm_t *vm)
{
	return uc_vm_frame_program(uc_vm_current_frame(vm));
}

__hidden const char *uc_vm_insn_to_name(uc_vm_insn_t insn);

__hidden void uc_vm_reset_callframes(uc_vm_t *vm, size_t depth);

__hidden char *uc_vm_format_val(uc_vm_t *vm, uc_value_t *val);
__hidden void uc_vm_frame_dump(uc_vm_t *vm, uc_callframe_t *frame);

__hidden void uc_vm_stack_set(uc_vm_t *vm, size_t offset, uc_value_t *value);

__hidden void uc_vm_call_native(uc_vm_t *vm, uc_value_t *ctx, uc_cfunction_t *fptr, bool mcall, size_t nargs);
__hidden uc_value_t *uc_vm_callframe_pop(uc_vm_t *vm);

/* The exports of the modules that a program links statically belong to the
 * closure of its entry function that runs them, and live as long as that
 * closure. An exported function holds the program, so exports that the program
 * owned itself would keep the program alive. Inline, as each call of a
 * native function begins and ends a run. */
static inline bool
uc_vm_closure_is_entry(uc_closure_t *closure)
{
	uc_function_t *function = closure->function;

	return function->program->functions.prev == &function->progref;
}

static inline void
uc_vm_run_begin(uc_closure_t *closure, bool keep)
{
	if (uc_vm_closure_is_entry(closure))
		uc_vector_push(&closure->function->program->runs, { .closure = closure, .keep = keep });
}

static inline void
uc_vm_run_end(uc_closure_t *closure)
{
	if (uc_vm_closure_is_entry(closure))
		uc_vector_pop(&closure->function->program->runs);
}

__hidden char *uc_vm_callee_expr(uc_vm_t *vm);
__hidden void uc_vm_clear_exception(uc_vm_t *vm);

__hidden void uc_vm_close_upvals(uc_vm_t *vm, size_t slot);

__hidden void uc_vm_gc_step(uc_vm_t *vm);

__hidden uc_value_t *uc_vm_value_arith(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *value, uc_value_t *operand);
__hidden uc_value_t *uc_vm_value_bitop(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *value, uc_value_t *operand);
__hidden uc_value_t *uc_vm_value_unary(uc_vm_insn_t operation, uc_value_t *value);
__hidden uc_value_t *uc_vm_value_complement(uc_value_t *value);
__hidden bool uc_vm_value_compare(uc_vm_insn_t operation, uc_value_t *v1, uc_value_t *v2);
__hidden bool uc_vm_value_equality(uc_vm_insn_t operation, uc_value_t *v1, uc_value_t *v2);
__hidden bool uc_vm_value_in(uc_value_t *r1, uc_value_t *r2);
__hidden bool uc_vm_value_not(uc_value_t *value);

__hidden bool uc_vm_iterator_next(uc_vm_t *vm, uc_vm_insn_t insn, uc_value_t *v,
                                  uc_value_t **state, uc_value_t **key, uc_value_t **val);

__hidden bool uc_vm_property_get(uc_vm_t *vm, uc_value_t *v, uc_value_t *k, uc_value_t **res);
__hidden bool uc_vm_property_set(uc_vm_t *vm, uc_value_t *o, uc_value_t *k, uc_value_t *v, uc_value_t **res);
__hidden bool uc_vm_property_update(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *v,
                                    uc_value_t *k, uc_value_t *inc, uc_value_t **res);
__hidden bool uc_vm_property_delete(uc_vm_t *vm, uc_value_t *v, uc_value_t *k, uc_value_t **res);

__hidden void uc_vm_array_append(uc_value_t *arr, uc_value_t **values, size_t count);
__hidden bool uc_vm_array_spread(uc_vm_t *vm, uc_value_t *dst, uc_value_t *src);
__hidden uc_value_t *uc_vm_object_new(uc_vm_t *vm, size_t nkeys);
__hidden void uc_vm_object_init_key(uc_vm_t *vm, uc_value_t *obj, uc_value_t *k, uc_value_t *v);
__hidden bool uc_vm_object_spread(uc_vm_t *vm, uc_value_t *dst, uc_value_t *src);

__hidden bool uc_vm_spread_push(uc_vm_t *vm, size_t base, size_t nargs, size_t from, size_t spread);
__hidden void uc_vm_spread_finish(uc_vm_t *vm, size_t base, size_t nargs, size_t from);

/* `hash` is the object key hash of `name`, see uc_program_get_constant_hash() */
__hidden uc_value_t *uc_vm_global_get(uc_vm_t *vm, uc_value_t *name, uint64_t hash, bool strict);
__hidden bool uc_vm_global_set(uc_vm_t *vm, uc_value_t *name, uint64_t hash, uc_value_t *v,
                               bool strict);
__hidden bool uc_vm_global_update(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *name,
                                  uint64_t hash, uc_value_t *inc, bool strict, uc_value_t **res);

__hidden const char *uc_vm_main_key(uc_program_t *program, uc_vm_main_key_t key);

__hidden uc_upvalref_t *uc_vm_capture_upval(uc_vm_t *vm, size_t slot);

__hidden bool uc_vm_regexp_load(uc_vm_t *vm, uc_value_t *jstr, uc_value_t **re);
__hidden void uc_vm_closure_upval_set(uc_vm_t *vm, uc_closure_t *closure, size_t idx, int32_t uv);
__hidden void uc_vm_stcall(uc_vm_t *vm, size_t slot, size_t nargs);
__hidden void uc_vm_value_print(uc_vm_t *vm, uc_value_t *v);

__hidden void uc_vm_import(uc_vm_t *vm, size_t from, size_t to);
__hidden uc_value_t *uc_vm_import_namespace(uc_vm_t *vm, size_t from, const uint32_t *names, size_t nnames);
__hidden uc_modexports_t *uc_vm_export_target(uc_vm_t *vm);
__hidden void uc_vm_export_add(uc_modexports_t *exports, uc_upvalref_t *ref);
__hidden uc_value_t *uc_vm_dynload_module(uc_vm_t *vm, uc_value_t *name);
__hidden uc_value_t *uc_vm_dynload_namespace(uc_vm_t *vm, uc_value_t *modscope);
__hidden bool uc_vm_dynload_bind(uc_vm_t *vm, uc_value_t *modscope, uint32_t cidx, size_t to);

__hidden void uc_vm_interpreter_set(const uc_vm_interpreter_t *interpreter);
__hidden void uc_vm_native_call_set(uc_vm_native_call_t call);
__hidden uc_value_t *uc_vm_resolve_upval(uc_vm_t *vm, uc_value_t *value);
__hidden bool uc_vm_call_depth_check(uc_vm_t *vm, bool tail);
__hidden bool uc_vm_call_dispatch(uc_vm_t *vm, uc_value_t **ctx, uc_value_t **fno, bool mcall, size_t nargs);
__hidden void uc_vm_call_args_adjust(uc_vm_t *vm, uc_function_t *function, size_t nargs);

/* Runs the closure that uc_vm_call_dispatch() returned, with its arguments
 * on the stack above `stackoff`; takes the references to `ctx` and to the
 * closure */
__hidden void uc_vm_closure_call(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx,
                                 bool mcall, size_t nargs, size_t stackoff);

#endif /* UCODE_INTERNAL_VMCORE_H */

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

#include <assert.h>
#include <math.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "ucode/internal/vmcore.h"
#include "ucode/internal/chunk.h"
#include "ucode/internal/lib.h" /* uc_error_context_format() */
#include "ucode/internal/source.h" /* uc_source_callee_expr() */
#include "ucode/internal/platform.h"

static uc_vm_insn_t
uc_vm_decode_insn(uc_vm_t *vm, uc_callframe_t *frame, uc_chunk_t *chunk)
{
	uc_vm_insn_t insn;
	int8_t argtype;

#ifndef NDEBUG
	uint8_t *end = chunk->entries + chunk->count;
#endif

	assert(frame->ip < end);

	for (size_t i = 0; i < vm->breakpoints.count; i++) {
		uc_breakpoint_t *bk = vm->breakpoints.entries[i];

		if (bk != NULL && (bk->ip == NULL || bk->ip == frame->ip))
			bk->cb(vm, bk);
	}

	insn = frame->ip[0];
	frame->ip++;

	argtype = uc_vm_insn_to_argtype(insn);

	assert(frame->ip + abs(argtype) <= end);

	switch (argtype) {
	case 0:
		break;

	case -4:
		vm->arg.s32 = (
			frame->ip[0] * 0x1000000UL +
			frame->ip[1] * 0x10000UL +
			frame->ip[2] * 0x100UL +
			frame->ip[3]
		) - 0x7fffffff;
		frame->ip += 4;
		break;

	case 1:
		vm->arg.u8 = frame->ip[0];
		frame->ip++;
		break;

	case 2:
		vm->arg.u16 = (
			frame->ip[0] * 0x100 +
			frame->ip[1]
		);
		frame->ip += 2;
		break;

	case 4:
		vm->arg.u32 = (
			frame->ip[0] * 0x1000000UL +
			frame->ip[1] * 0x10000UL +
			frame->ip[2] * 0x100UL +
			frame->ip[3]
		);
		frame->ip += 4;
		break;

	default:
		fprintf(stderr, "Unhandled operand format: %" PRId8 "\n", argtype);
		abort();
	}

	return insn;
}


/* Replaces the current call frame by an invocation frame for the given closure,
 * reusing the stack slots formerly occupied by the current frame.
 *
 * Since neither the call frame vector nor the value stack grow when invoking the
 * callee, this allows unbounded tail recursion.
 *
 * On entry, the stack holds the invoked function value and its arguments at the
 * top, optionally preceded by the receiver value of a method invocation. */
static void
uc_vm_frame_reinit(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx,
                   bool mcall, size_t nargs)
{
	size_t i, blocksrc, blockdst, blocklen, blockofs;
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_value_t *oldclosure, *oldctx, *val;

	/* the slot the invoking code expects the eventual return value at, which is
	 * the slot of the current frame's function value, or - for method calls - of
	 * the receiver value immediately below it */
	blockdst = frame->stackframe - (frame->mcall ? 1 : 0);

	/* number of stack slots occupied by the receiver, function and arguments */
	blocklen = nargs + 1 + (mcall ? 1 : 0);

	/* slot of the invoked function value, or of the receiver value preceding it */
	blocksrc = vm->stack.count - blocklen;

	/* the value block is relocated towards the start of the stack, so the slots
	 * it occupies are always read before being overwritten */
	blockofs = blocksrc - blockdst;

	/* close upvalues referring to the current frame's stack slots before overwriting them */
	uc_vm_close_upvals(vm, frame->stackframe);

	/* remember the value references owned by the current frame */
	oldclosure = frame->closure ? &frame->closure->header : NULL;
	oldctx = frame->ctx;

	/* move receiver, function value and arguments down into the slots of the frame
	 * being replaced, releasing the local variable, temporary and receiver values
	 * formerly held by them and clearing stale slots left above the new frame */
	for (i = blockdst; i < vm->stack.count; i++) {
		val = (i - blockdst < blocklen) ? vm->stack.entries[i + blockofs] : NULL;

		/* the slot holds a value owned by the current frame unless it is part of
		 * the value block being relocated */
		if (i < blocksrc)
			ucv_put(vm->stack.entries[i]);

		vm->stack.entries[i] = val;
	}

	vm->stack.count = blockdst + blocklen;

	/* reinitialize the call frame in place; the receiver value of a method call
	 * occupies the slot below the invoked function value. the tco counter records
	 * that this frame replaced the tail calling one, so stack traces can indicate
	 * the collapsed frames */
	if (frame->closure)
		uc_vm_run_end(frame->closure);

	uc_vm_run_begin(closure, false);

	frame->stackframe = blockdst + (mcall ? 1 : 0);
	frame->cfunction = NULL;
	frame->closure = closure;
	frame->ctx = ctx;
	frame->ip = closure->function->chunk.entries;
	frame->mcall = mcall;
	frame->strict = closure->function->strict;

	/* saturate rather than wrap, so the count stays a valid (if capped) number
	 * of omitted frames */
	if (frame->tco < 0xffff)
		frame->tco++;

	if (vm->trace)
		uc_vm_frame_dump(vm, frame);

	/* release the references formerly owned by the current frame */
	ucv_put(oldclosure);
	ucv_put(oldctx);
}

static void
uc_vm_frame_push(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx,
                 bool mcall, size_t stackoff)
{
	uc_function_t *function = closure->function;
	uc_callframe_t *frame;

	frame = uc_vector_push(&vm->callframes, {
		.stackframe = stackoff,
		.cfunction = NULL,
		.closure = closure,
		.ctx = ctx,
		.ip = function->chunk.entries,
		.mcall = mcall,
		.strict = function->strict
	});

	uc_vm_run_begin(closure, false);

	if (vm->trace)
		uc_vm_frame_dump(vm, frame);
}

static bool
uc_vm_call_function(uc_vm_t *vm, uc_value_t *ctx, uc_value_t *fno, bool mcall,
                   size_t argspec, bool tail)
{
	size_t i, stackoff, nargs = argspec & 0xffff;
	size_t nspreads = (argspec >> 16) & 0x7fff;
	uc_callframe_t *frame = NULL;
	uc_closure_t *closure;
	uint16_t slot, tmp;

	if (!uc_vm_call_depth_check(vm, tail)) {
		ucv_put(ctx);
		ucv_put(fno);

		return false;
	}

	stackoff = vm->stack.count - nargs - 1;

	/* argument list contains spread operations, we need to reshuffle the stack */
	if (nspreads > 0) {
		frame = uc_vm_current_frame(vm);

		/* for each spread value index ... */
		for (i = 0, slot = nargs; i < nspreads; i++) {
			/* decode stack depth value */
			tmp = frame->ip[0] * 0x100 + frame->ip[1];
			frame->ip += 2;

			if (!uc_vm_spread_push(vm, stackoff + 1, nargs, slot, tmp)) {
				ucv_put(ctx);
				ucv_put(fno);

				return false;
			}

			slot = tmp;
		}

		uc_vm_spread_finish(vm, stackoff + 1, nargs, slot);

		/* update arg count */
		nargs = vm->stack.count - stackoff - 1;
	}

	if (!uc_vm_call_dispatch(vm, &ctx, &fno, mcall, nargs))
		return false;

	if (!fno)
		return true;

	closure = ucv_as_closure(fno);

	/* a native closure runs at once and leaves its result on the stack, as a
	 * C function does, also in a tail call, which a RETURN follows */
	if (closure->function->native) {
		uc_vm_closure_call(vm, closure, ctx, mcall, nargs, stackoff);

		return true;
	}

	uc_vm_call_args_adjust(vm, closure->function, nargs);

	/* tail call: reuse the current call frame instead of pushing a new one. The
	 * number of values above the invoked function may differ from the encoded
	 * argument count due to spread operations or excess argument handling */
	if (tail) {
		uc_vm_frame_reinit(vm, closure, ctx, mcall, vm->stack.count - stackoff - 1);

		return true;
	}

	uc_vm_frame_push(vm, closure, ctx, mcall, stackoff);

	return true;
}

static uc_source_t *last_source = NULL;
static size_t last_srcpos = 0;

static void
uc_dump_insn(uc_vm_t *vm, uint8_t *pos, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_chunk_t *chunk = uc_vm_frame_chunk(frame);
	uc_stringbuf_t *buf = NULL;
	uc_value_t *cnst = NULL;
	uc_source_t *source;
	int8_t argtype;
	size_t srcpos;

	srcpos = uc_program_function_srcpos(frame->closure->function, pos - chunk->entries);
	source = uc_vm_frame_source(frame);

	if (last_srcpos == 0 || last_source != source || srcpos != last_srcpos) {
		buf = uc_stringbuf_new();

		uc_source_context_format(buf, source, srcpos, true);
		fwrite(buf->buf, 1, uc_stringbuf_length(buf), stderr);
		uc_stringbuf_free(buf);

		last_source = source;
		last_srcpos = srcpos;
	}

	fprintf(stderr, "%08zx  %s", pos - chunk->entries,
		uc_vm_insn_to_name(insn));

	argtype = uc_vm_insn_to_argtype(insn);

	switch (argtype) {
	case 0:
		break;

	case -1:
		fprintf(stderr, " {%s%hhd}", vm->arg.s8 < 0 ? "" : "+", vm->arg.s8);
		break;

	case -2:
		fprintf(stderr, " {%c0x%hx}",
			vm->arg.s16 < 0 ? '-' : '+',
			(uint16_t)(vm->arg.s16 < 0 ? -vm->arg.s16 : vm->arg.s16));
		break;

	case -4:
		fprintf(stderr, " {%c0x%x}",
			vm->arg.s32 < 0 ? '-' : '+',
			(uint32_t)(vm->arg.s32 < 0 ? -vm->arg.s32 : vm->arg.s32));
		break;

	case 1:
		fprintf(stderr, " {%hhu}", vm->arg.u8);
		break;

	case 2:
		fprintf(stderr, " {0x%hx}", vm->arg.u16);
		break;

	case 4:
		fprintf(stderr, " {0x%x}", vm->arg.u32);
		break;

	default:
		fprintf(stderr, " (unknown operand format: %" PRId8 ")", argtype);
		break;
	}

	switch (insn) {
	case I_LOAD:
	case I_LVAR:
	case I_SVAR:
		cnst = uc_program_get_constant(uc_vm_frame_program(uc_vector_last(&vm->callframes)), vm->arg.u32);

		fprintf(stderr, "\t; %s",
			cnst ? uc_vm_format_val(vm, cnst) : "(?)");

		ucv_put(cnst);
		break;

	case I_LLOC:
	case I_LUPV:
	case I_SLOC:
	case I_SUPV:
		cnst = uc_chunk_debug_get_variable(chunk, pos - chunk->entries, vm->arg.u32, (insn == I_LUPV || insn == I_SUPV));

		fprintf(stderr, "\t; %s",
			cnst ? uc_vm_format_val(vm, cnst) : "(?)");

		ucv_put(cnst);
		break;

	case I_ULOC:
	case I_UUPV:
		cnst = uc_chunk_debug_get_variable(chunk, pos - chunk->entries, vm->arg.u32 & 0x00ffffff, (insn == I_UUPV));
		/* fall through */

	case I_UVAR:
		if (!cnst)
			cnst = uc_program_get_constant(uc_vm_frame_program(uc_vector_last(&vm->callframes)), vm->arg.u32 & 0x00ffffff);

		fprintf(stderr, "\t; %s (%s)",
			cnst ? uc_vm_format_val(vm, cnst) : "(?)",
			uc_vm_insn_to_name(vm->arg.u32 >> 24));

		ucv_put(cnst);
		break;

	case I_UVAL:
		fprintf(stderr, "\t; (%s)", uc_vm_insn_to_name(vm->arg.u8));
		break;

	case I_STCALL: {
		/* the operand packs the callee's uc_stdlib_functions[] slot in its
		 * upper half, the argument count in the lower one */
		const char *name = uc_stdlib_function_name_by_slot(vm->arg.u32 >> 16);

		fprintf(stderr, "\t; %s", name ? name : "(?)");
		break;
	}

	default:
		break;
	}

	fprintf(stderr, "\n");
}

/* Well-known sentinel `uc_breakpoint_t.ip` value identifying the dedicated
 * "break on uncaught exception" system breakpoint (see debug.c's BK_UNCAUGHT).
 * It deliberately isn't a real bytecode address, so the ordinary
 * ip-matching breakpoint dispatch in uc_vm_decode_insn() - which walks
 * vm->breakpoints on every single instruction - never fires it by
 * accident; it is only ever invoked explicitly, from the exception label in
 * uc_vm_execute_chunk() below, at the one moment it actually applies. */
static uint8_t uc_breakpoint_uncaught_exception_storage;
uint8_t *const UC_BREAKPOINT_UNCAUGHT_EXCEPTION =
	&uc_breakpoint_uncaught_exception_storage;

/* Non-destructively predict whether uc_vm_handle_exception()'s real unwind
 * loop (below) would find a handler for the currently raised exception
 * anywhere between the current callframe and `caller` (the frame depth this
 * uc_vm_execute_chunk() invocation was entered at - the same boundary its
 * own unwind loop stops at). Mirrors that loop's exact stopping conditions
 * (a native callframe, or reaching `caller`) but only inspects state; nops
 * of the stack/exception state, jumping ip. Used to decide whether to break
 * into the debugger *before* unwinding starts, while the original throwing
 * frame - locals, exact position - is still fully intact, since once
 * uc_vm_handle_exception() starts really popping frames that's gone. */
static bool
uc_vm_exception_would_be_caught(uc_vm_t *vm, size_t caller)
{
	for (size_t i = vm->callframes.count; i > caller; i--) {
		uc_callframe_t *frame = &vm->callframes.entries[i - 1];

		if (!frame->closure)
			return false;

		uc_chunk_t *chunk = &frame->closure->function->chunk;
		size_t pos = frame->ip - chunk->entries;

		for (size_t j = 0; j < chunk->ehranges.count; j++) {
			if (pos >= chunk->ehranges.entries[j].from &&
			    pos < chunk->ehranges.entries[j].to)
				return true;
		}
	}

	return false;
}

static bool
uc_vm_handle_exception(uc_vm_t *vm)
{
	uc_callframe_t *frame = NULL;
	uc_chunk_t *chunk = NULL;
	uc_value_t *exo;
	size_t i, pos;

	if (vm->callframes.count)
		frame = uc_vm_current_frame(vm);

	if (!frame || !frame->closure)
		return false;

	chunk = uc_vm_frame_chunk(frame);
	pos = frame->ip - chunk->entries;

	/* iterate the known exception ranges, see if the current ip falls into any of them */
	for (i = 0; i < chunk->ehranges.count; i++) {
		/* skip nonmatching ranges */
		if (pos < chunk->ehranges.entries[i].from ||
		    pos >= chunk->ehranges.entries[i].to)
			continue;

		/* we found a matching range... first unwind stack */
		while (vm->stack.count > frame->stackframe + chunk->ehranges.entries[i].slot)
			ucv_put(uc_vm_stack_pop(vm));

		/* prepare exception object and expose it to user handler code */
		exo = uc_vm_exception_object(vm);

		uc_vm_stack_push(vm, exo);

		/* reset exception information */
		uc_vm_clear_exception(vm);

		/* jump to exception handler */
		if (chunk->ehranges.entries[i].target >= chunk->count) {
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "jump target out of range");
			return false;
		}

#if 0
		if (vm->trace && chunk->entries + chunk->ehranges.entries[i].target > frame->ip) {
			while (frame->ip < chunk->entries + chunk->ehranges.entries[i].target) {
				fprintf(stderr, "(eh:skip) [%p:%zu] ", chunk, frame->ip - chunk->entries);
				uc_dump_insn(vm, frame->ip, uc_vm_decode_insn(vm, frame, chunk));
			}
		}
#endif

		frame->ip = chunk->entries + chunk->ehranges.entries[i].target;

		return true;
	}

	return false;
}

static void
uc_vm_insn_load(uc_vm_t *vm, uc_vm_insn_t insn)
{
	switch (insn) {
	case I_LOAD:
		uc_vm_stack_push(vm, uc_program_get_constant(uc_vm_current_program(vm), vm->arg.u32));
		break;

	case I_LOAD8:
		uc_vm_stack_push(vm, ucv_uint64_new(vm->arg.u8));
		break;

	case I_LOAD16:
		uc_vm_stack_push(vm, ucv_uint64_new(vm->arg.u16));
		break;

	case I_LOAD32:
		uc_vm_stack_push(vm, ucv_uint64_new(vm->arg.u32));
		break;

	default:
		break;
	}
}

static void
uc_vm_insn_load_regexp(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *re, *jstr = uc_program_get_constant(uc_vm_current_program(vm), vm->arg.u32);
	bool loaded = uc_vm_regexp_load(vm, jstr, &re);

	ucv_put(jstr);

	if (loaded)
		uc_vm_stack_push(vm, re);
}

static void
uc_vm_insn_load_null(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_vm_stack_push(vm, NULL);
}

static void
uc_vm_insn_load_bool(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_vm_stack_push(vm, ucv_boolean_new(insn == I_LTRUE));
}

static void
uc_vm_insn_load_var(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_program_t *prog = uc_vm_current_program(vm);
	uc_value_t *name = uc_program_get_constant(prog, vm->arg.u32);
	uc_value_t *val = uc_vm_global_get(vm, name, uc_program_get_constant_hash(prog, vm->arg.u32),
	                                   uc_vm_is_strict(vm));

	ucv_put(name);

	uc_vm_stack_push(vm, val);
}

static void
uc_vm_insn_load_val(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_pop(vm);
	uc_value_t *res;

	if (uc_vm_property_get(vm, v, k, &res))
		uc_vm_stack_push(vm, res);

	ucv_put(k);
	ucv_put(v);
}

static void
uc_vm_insn_peek_val(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_peek(vm, 0);
	uc_value_t *res;

	if (uc_vm_property_get(vm, v, k, &res))
		uc_vm_stack_push(vm, res);

	ucv_put(k);
}

static void
uc_vm_insn_load_upval(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_upvalref_t *ref;

	if (vm->arg.u32 >= frame->closure->function->nupvals) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "upvalue index out of range");

		return;
	}

	ref = frame->closure->upvals[vm->arg.u32];

	if (ref->closed)
		uc_vm_stack_push(vm, ucv_get(ref->value));
	else
		uc_vm_stack_push(vm, ucv_get(vm->stack.entries[ref->slot]));
}

static void
uc_vm_insn_load_local(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);

	if (frame->stackframe + vm->arg.u32 >= vm->stack.count) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "local slot out of range");

		return;
	}

	uc_vm_stack_push(vm, ucv_get(vm->stack.entries[frame->stackframe + vm->arg.u32]));
}

static void
uc_vm_insn_load_closure(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_function_t *function = uc_program_function_load(uc_vm_current_program(vm), vm->arg.u32);
	uc_closure_t *closure = (uc_closure_t *)ucv_closure_new(vm, function, insn == I_ARFN);
	volatile int32_t uv;
	size_t i;

	uc_vm_stack_push(vm, &closure->header);

	if (function->module)
		return;

	for (i = 0; i < function->nupvals; i++) {
		uv = (
			frame->ip[0] * 0x1000000 +
			frame->ip[1] * 0x10000 +
			frame->ip[2] * 0x100 +
			frame->ip[3]
		) - 0x7fffffff;

		uc_vm_closure_upval_set(vm, closure, i, uv);

		frame->ip += 4;
	}
}

static void
uc_vm_insn_store_var(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *v = uc_vm_stack_pop(vm);
	uc_program_t *prog = uc_vm_current_program(vm);
	uc_value_t *name = uc_program_get_constant(prog, vm->arg.u32);
	bool stored = uc_vm_global_set(vm, name, uc_program_get_constant_hash(prog, vm->arg.u32), v,
	                               uc_vm_is_strict(vm));

	ucv_put(name);

	if (stored)
		uc_vm_stack_push(vm, v);
	else
		ucv_put(v);
}

static void
uc_vm_insn_store_val(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *v = uc_vm_stack_pop(vm);
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *o = uc_vm_stack_pop(vm);
	uc_value_t *res;

	if (uc_vm_property_set(vm, o, k, v, &res))
		uc_vm_stack_push(vm, res);

	ucv_put(v);
	ucv_put(o);
	ucv_put(k);
}

static void
uc_vm_insn_store_upval(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_upvalref_t *ref;
	uc_value_t *val;

	if (vm->arg.u32 >= frame->closure->function->nupvals) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "upvalue index out of range");

		return;
	}

	ref = frame->closure->upvals[vm->arg.u32];
	val = ucv_get(uc_vm_stack_peek(vm, 0));

	if (ref->closed) {
		ucv_put(ref->value);
		ref->value = val;
	}
	else {
		uc_vm_stack_set(vm, ref->slot, val);
	}
}

static void
uc_vm_insn_store_local(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_value_t *val;

	if (frame->stackframe + vm->arg.u32 >= vm->stack.count) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "local slot out of range");

		return;
	}

	val = ucv_get(uc_vm_stack_peek(vm, 0));

	uc_vm_stack_set(vm, frame->stackframe + vm->arg.u32, val);
}

static void
uc_vm_insn_update_var(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *inc = uc_vm_stack_pop(vm);
	uc_program_t *prog = uc_vm_current_program(vm);
	size_t idx = vm->arg.u32 & 0x00FFFFFF;
	uc_value_t *name = uc_program_get_constant(prog, idx);
	uc_value_t *val;

	if (uc_vm_global_update(vm, vm->arg.u32 >> 24, name, uc_program_get_constant_hash(prog, idx),
	                        inc, uc_vm_is_strict(vm), &val))
		uc_vm_stack_push(vm, val);

	ucv_put(name);
	ucv_put(inc);
}

static void
uc_vm_insn_update_val(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *inc = uc_vm_stack_pop(vm);
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_pop(vm);
	uc_value_t *res;

	if (uc_vm_property_update(vm, vm->arg.u8, v, k, inc, &res))
		uc_vm_stack_push(vm, res);

	ucv_put(inc);
	ucv_put(v);
	ucv_put(k);
}

static void
uc_vm_insn_update_upval(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	size_t slot = vm->arg.u32 & 0x00FFFFFF;
	uc_upvalref_t *ref;
	uc_value_t *inc;
	uc_value_t *val;

	if (slot >= frame->closure->function->nupvals) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "upvalue index out of range");

		return;
	}

	ref = frame->closure->upvals[slot];
	inc = uc_vm_stack_pop(vm);

	if (ref->closed)
		val = ref->value;
	else
		val = vm->stack.entries[ref->slot];

	val = uc_vm_value_arith(vm, vm->arg.u32 >> 24, val, inc);

	uc_vm_stack_push(vm, val);

	ucv_put(inc);

	if (ref->closed) {
		ucv_put(ref->value);
		ref->value = ucv_get(uc_vm_stack_peek(vm, 0));
	}
	else {
		uc_vm_stack_set(vm, ref->slot, ucv_get(uc_vm_stack_peek(vm, 0)));
	}
}

static void
uc_vm_insn_update_local(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	size_t slot = vm->arg.u32 & 0x00FFFFFF;
	uc_value_t *inc;
	uc_value_t *val;

	if (frame->stackframe + slot >= vm->stack.count) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "local slot out of range");

		return;
	}

	inc = uc_vm_stack_pop(vm);

	val = uc_vm_value_arith(vm, vm->arg.u32 >> 24,
	                        vm->stack.entries[frame->stackframe + slot], inc);

	uc_vm_stack_push(vm, val);

	ucv_put(inc);
	uc_vm_stack_set(vm, frame->stackframe + slot, ucv_get(uc_vm_stack_peek(vm, 0)));
}

static void
uc_vm_insn_narr(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *arr = ucv_array_new_length(vm, vm->arg.u32);

	uc_vm_stack_push(vm, arr);
}

static void
uc_vm_insn_parr(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *arr = uc_vm_stack_peek(vm, vm->arg.u32);
	size_t idx;

	uc_vm_array_append(arr, &vm->stack.entries[vm->stack.count - vm->arg.u32], vm->arg.u32);

	for (idx = 0; idx < vm->arg.u32; idx++)
		uc_vm_stack_pop(vm);

	//uc_vm_shrink(state, vm->arg.u32);
}

static void
uc_vm_insn_marr(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *src = uc_vm_stack_pop(vm);
	uc_value_t *dst = uc_vm_stack_peek(vm, 0);

	uc_vm_array_spread(vm, dst, src);
}

static void
uc_vm_insn_nobj(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_vm_stack_push(vm, uc_vm_object_new(vm, vm->arg.u32 / 2));
}

static void
uc_vm_insn_stcall(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_vm_stcall(vm, vm->arg.u32 >> 16, vm->arg.u32 & 0xffff);
}

static void
uc_vm_insn_sobj(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *obj = uc_vm_stack_peek(vm, vm->arg.u32);
	size_t idx;

	for (idx = 0; idx < vm->arg.u32; idx += 2)
		uc_vm_object_init_key(vm, obj,
			uc_vm_stack_peek(vm, vm->arg.u32 - idx - 1),
			uc_vm_stack_peek(vm, vm->arg.u32 - idx - 2));

	for (idx = 0; idx < vm->arg.u32; idx++)
		ucv_put(uc_vm_stack_pop(vm));
}

static void
uc_vm_insn_mobj(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *src = uc_vm_stack_pop(vm);
	uc_value_t *dst = uc_vm_stack_peek(vm, 0);

	uc_vm_object_spread(vm, dst, src);
}

static void
uc_vm_insn_arith(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);
	uc_value_t *rv;

	rv = uc_vm_value_arith(vm, insn, r1, r2);

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, rv);
}

static void
uc_vm_insn_plus_minus(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *v = uc_vm_stack_pop(vm);

	uc_vm_stack_push(vm, uc_vm_value_unary(insn, v));
	ucv_put(v);
}

static void
uc_vm_insn_bitop(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);
	uc_value_t *rv;

	rv = uc_vm_value_bitop(vm, insn, r1, r2);

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, rv);
}

static void
uc_vm_insn_complement(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *v = uc_vm_stack_pop(vm);

	uc_vm_stack_push(vm, uc_vm_value_complement(v));
	ucv_put(v);
}

static void
uc_vm_insn_rel(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);

	bool res = uc_vm_value_compare(insn, r1, r2);

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, ucv_boolean_new(res));
}

static void
uc_vm_insn_in(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);
	bool found = uc_vm_value_in(r1, r2);

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, ucv_boolean_new(found));
}

static void
uc_vm_insn_equality(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);
	bool equal = uc_vm_value_equality(insn, r1, r2);

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, ucv_boolean_new(equal));
}

static void
uc_vm_insn_not(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r1 = uc_vm_stack_pop(vm);

	uc_vm_stack_push(vm, ucv_boolean_new(uc_vm_value_not(r1)));
	ucv_put(r1);
}

static void
uc_vm_insn_jmp(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_chunk_t *chunk = uc_vm_frame_chunk(frame);
	int32_t addr = vm->arg.s32;

	/* ip already has been incremented */
	addr -= 5;

	if (frame->ip + addr < chunk->entries ||
	    frame->ip + addr >= chunk->entries + chunk->count) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "jump target out of range");
		return;
	}

	frame->ip += addr;
}

static void
uc_vm_insn_jmpz(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_chunk_t *chunk = uc_vm_frame_chunk(frame);
	uc_value_t *v = uc_vm_stack_pop(vm);
	int32_t addr = vm->arg.s32;

	/* ip already has been incremented */
	addr -= 5;

	if (frame->ip + addr < chunk->entries ||
	    frame->ip + addr >= chunk->entries + chunk->count) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "jump target out of range");
		return;
	}

	if (!ucv_is_truish(v))
		frame->ip += addr;

	ucv_put(v);
}

static void
uc_vm_insn_jmpnt(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_chunk_t *chunk = uc_vm_frame_chunk(frame);
	int16_t addr = (vm->arg.u32 & 0xffff) - 0x7fff;
	uint16_t types = (vm->arg.u32 >> 16) & 0x1fff;
	uint8_t depth = (vm->arg.u32 >> 29) & 0x7;
	uc_value_t *v = uc_vm_stack_peek(vm, 0);
	size_t i;

	/* ip already has been incremented */
	addr -= 5;

	if (frame->ip + addr < chunk->entries ||
	    frame->ip + addr >= chunk->entries + chunk->count) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "jump target out of range");
		return;
	}

	if (!(types & (1u << ucv_type(v)))) {
		for (i = 0; i <= depth; i++)
			ucv_put(uc_vm_stack_pop(vm));

		uc_vm_stack_push(vm, NULL);
		frame->ip += addr;
	}
}


static void
uc_vm_insn_next(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_pop(vm);
	uc_value_t *key = NULL, *val = NULL;

	if (uc_vm_iterator_next(vm, insn, v, &k, &key, &val)) {
		uc_vm_stack_push(vm, key);

		if (insn == I_NEXTKV)
			uc_vm_stack_push(vm, val);

		uc_vm_stack_push(vm, k);
		ucv_put(v);

		return;
	}

	uc_vm_stack_push(vm, NULL);
	uc_vm_stack_push(vm, NULL);

	if (insn == I_NEXTKV)
		uc_vm_stack_push(vm, NULL);

	ucv_put(k);
	ucv_put(v);
}

static void
uc_vm_insn_close_upval(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_vm_close_upvals(vm, vm->stack.count - 1);
	ucv_put(uc_vm_stack_pop(vm));
}

static void
uc_vm_insn_call(uc_vm_t *vm)
{
	bool mcall = (vm->arg.u32 & 0x80000000);
	size_t nargs = (vm->arg.u32 & 0xffff);
	size_t nspreads = (vm->arg.u32 >> 16) & 0x7fff;
	uint8_t *ip = uc_vm_current_frame(vm)->ip + 2 * nspreads;
	uc_value_t *fno = uc_vm_stack_peek(vm, nargs);
	uc_value_t *ctx = NULL;
	bool tail;

	if (!ucv_is_arrowfn(fno))
		ctx = mcall ? uc_vm_stack_peek(vm, nargs + 1) : NULL;
	else if (vm->callframes.count > 0)
		ctx = uc_vm_current_frame(vm)->ctx;

	/* a tail call is indicated by a 0x00 marker that the compiler emits
	 * immediately after the enclosing I_RETURN. ip points just past the call's
	 * operand span, so ip[0] is the I_RETURN and ip[1] the marker. Neither VM
	 * executes the marker: older VMs return before reaching it, and this VM
	 * jumps to the callee at the call site. */
	tail = (ip[0] == I_RETURN) && (ip[1] == 0x00);

	uc_vm_call_function(vm, ucv_get(ctx), ucv_get(fno), mcall, vm->arg.u32, tail);
}

static void
uc_vm_insn_print(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *v = uc_vm_stack_pop(vm);

	uc_vm_value_print(vm, v);
	ucv_put(v);
}

static void
uc_vm_insn_delete(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_pop(vm);
	uc_value_t *res;

	if (uc_vm_property_delete(vm, v, k, &res))
		uc_vm_stack_push(vm, res);

	ucv_put(k);
	ucv_put(v);
}

static void
uc_vm_insn_import(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	size_t from = vm->arg.u32 & 0xffff;
	uint16_t to = vm->arg.u32 >> 16;
	uint32_t *names = NULL;
	size_t count, i;

	if (to != 0xffff) {
		uc_vm_import(vm, from, to);

		return;
	}

	/* is a wildcard import * from ... */
	count = from;

	if (count)
		names = xalloc(count * sizeof(*names));

	/* instruction is followed by u16 containing the offset of the
	 * first module export and `count` times u32 values containing
	 * the constant indexes of the names */
	from = frame->ip[0] * 0x100 + frame->ip[1];

	/* read all name operands, also those of a missing export */
	for (frame->ip += 2, i = 0; i < count; i++) {
		names[i] = (
			frame->ip[0] * 0x1000000UL +
			frame->ip[1] * 0x10000UL +
			frame->ip[2] * 0x100UL +
			frame->ip[3]
		);

		frame->ip += 4;
	}

	uc_vm_stack_push(vm, uc_vm_import_namespace(vm, from, names, count));

	free(names);
}

static void
uc_vm_insn_export(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_modexports_t *exports = uc_vm_export_target(vm);

	if (!exports)
		return;

	uc_vm_export_add(exports, uc_vm_capture_upval(vm, frame->stackframe + vm->arg.u32));
}

static void
uc_vm_insn_dynload(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uint16_t count = vm->arg.u32 & 0xffff;
	uint16_t to = vm->arg.u32 >> 16;
	uc_value_t *name, *modscope;
	uc_callframe_t *frame;
	uint32_t cidx;

	name = uc_vm_stack_pop(vm);
	modscope = uc_vm_dynload_module(vm, name);
	ucv_put(name);

	if (!modscope)
		return;

	/* If count is zero, we're doing a wildcard import. Shallow copy module
	 * object, mark it constant and patch into the target upvalue. */
	if (count == 0) {
		uc_vm_stack_push(vm, uc_vm_dynload_namespace(vm, modscope));
		ucv_put(modscope);

		return;
	}

	/* ... otherwise we're importing a specific list of names */
	frame = uc_vm_current_frame(vm);

	/* Instruction is followed by `count` times u32 values containing
	 * the import name constant indexes. The error context of a missing
	 * name derives from the instruction pointer, so decode each name only
	 * when it is bound. */
	for (; count > 0; count--, to++) {
		cidx = (
			frame->ip[0] * 0x1000000UL +
			frame->ip[1] * 0x10000UL +
			frame->ip[2] * 0x100UL +
			frame->ip[3]
		);

		frame->ip += 4;

		if (!uc_vm_dynload_bind(vm, modscope, cidx, to))
			break;
	}

	ucv_put(modscope);
}

static uc_vm_status_t
uc_vm_exception_type_to_status(uc_vm_t *vm)
{
	switch (vm->exception.type) {
	case EXCEPTION_NONE:   return STATUS_OK;
	case EXCEPTION_EXIT:   return STATUS_EXIT;
	case EXCEPTION_SYNTAX: return ERROR_COMPILE;
	default:               return ERROR_RUNTIME;
	}
}

static uc_vm_status_t
uc_vm_execute_chunk(uc_vm_t *vm)
{
	uc_callframe_t *frame = NULL;
	uc_chunk_t *chunk = NULL;
	size_t caller = vm->callframes.count - 1;
	uc_value_t *retval;
	uc_vm_insn_t insn;
	uint8_t *ip;

	while (vm->callframes.count > caller) {
		frame = &vm->callframes.entries[vm->callframes.count - 1];
		chunk = uc_vm_frame_chunk(frame);

		if (!chunk)
			break;

		if (vm->trace) {
			ip = frame->ip;
			insn = uc_vm_decode_insn(vm, frame, chunk);
			uc_dump_insn(vm, ip, insn);
		}
		else {
			insn = uc_vm_decode_insn(vm, frame, chunk);
		}

		switch (insn) {
		case I_LOAD:
		case I_LOAD8:
		case I_LOAD16:
		case I_LOAD32:
			uc_vm_insn_load(vm, insn);
			break;

		case I_LREXP:
			uc_vm_insn_load_regexp(vm, insn);
			break;

		case I_LNULL:
			uc_vm_insn_load_null(vm, insn);
			break;

		case I_LTRUE:
		case I_LFALSE:
			uc_vm_insn_load_bool(vm, insn);
			break;

		case I_LTHIS:
			uc_vm_stack_push(vm, ucv_get(frame->ctx));
			break;

		case I_LVAR:
			uc_vm_insn_load_var(vm, insn);
			break;

		case I_LVAL:
			uc_vm_insn_load_val(vm, insn);
			break;

		case I_PVAL:
			uc_vm_insn_peek_val(vm, insn);
			break;

		case I_LUPV:
			uc_vm_insn_load_upval(vm, insn);
			break;

		case I_LLOC:
			uc_vm_insn_load_local(vm, insn);
			break;

		case I_CLFN:
		case I_ARFN:
			uc_vm_insn_load_closure(vm, insn);
			break;

		case I_NARR:
			uc_vm_insn_narr(vm, insn);
			break;

		case I_PARR:
			uc_vm_insn_parr(vm, insn);
			break;

		case I_MARR:
			uc_vm_insn_marr(vm, insn);
			break;

		case I_NOBJ:
			uc_vm_insn_nobj(vm, insn);
			break;

		case I_SOBJ:
			uc_vm_insn_sobj(vm, insn);
			break;

		case I_MOBJ:
			uc_vm_insn_mobj(vm, insn);
			break;

		case I_STCALL:
			uc_vm_insn_stcall(vm, insn);
			break;

		case I_SVAR:
			uc_vm_insn_store_var(vm, insn);
			break;

		case I_SVAL:
			uc_vm_insn_store_val(vm, insn);
			break;

		case I_SUPV:
			uc_vm_insn_store_upval(vm, insn);
			break;

		case I_SLOC:
			uc_vm_insn_store_local(vm, insn);
			break;

		case I_UVAR:
			uc_vm_insn_update_var(vm, insn);
			break;

		case I_UVAL:
			uc_vm_insn_update_val(vm, insn);
			break;

		case I_UUPV:
			uc_vm_insn_update_upval(vm, insn);
			break;

		case I_ULOC:
			uc_vm_insn_update_local(vm, insn);
			break;

		case I_ADD:
		case I_SUB:
		case I_MUL:
		case I_DIV:
		case I_MOD:
		case I_EXP:
			uc_vm_insn_arith(vm, insn);
			break;

		case I_PLUS:
		case I_MINUS:
			uc_vm_insn_plus_minus(vm, insn);
			break;

		case I_LSHIFT:
		case I_RSHIFT:
		case I_BAND:
		case I_BXOR:
		case I_BOR:
			uc_vm_insn_bitop(vm, insn);
			break;

		case I_COMPL:
			uc_vm_insn_complement(vm, insn);
			break;

		case I_EQS:
		case I_NES:
			uc_vm_insn_equality(vm, insn);
			break;

		case I_EQ:
		case I_NE:
		case I_LT:
		case I_LE:
		case I_GT:
		case I_GE:
			uc_vm_insn_rel(vm, insn);
			break;

		case I_IN:
			uc_vm_insn_in(vm, insn);
			break;

		case I_NOT:
			uc_vm_insn_not(vm, insn);
			break;

		case I_JMP:
			uc_vm_insn_jmp(vm, insn);
			break;

		case I_JMPZ:
			uc_vm_insn_jmpz(vm, insn);
			break;

		case I_JMPNT:
			uc_vm_insn_jmpnt(vm, insn);
			break;

		case I_NEXTK:
		case I_NEXTKV:
			uc_vm_insn_next(vm, insn);
			break;

		case I_COPY:
			uc_vm_stack_push(vm, ucv_get(uc_vm_stack_peek(vm, vm->arg.u8)));
			break;

		case I_POP:
			ucv_put(uc_vm_stack_pop(vm));
			uc_vm_gc_step(vm);
			break;

		case I_CUPV:
			uc_vm_insn_close_upval(vm, insn);
			break;

		case I_CALL:
			uc_vm_insn_call(vm);

			if (vm->callframes.count == 0)
				return uc_vm_exception_type_to_status(vm);

			/* frame and chunk are re-derived from the (now callee) top callframe
			 * at the top of the loop; nothing between here and there reads them. */
			break;

		case I_RETURN:
			retval = uc_vm_callframe_pop(vm);

			uc_vm_stack_push(vm, retval);

			if (vm->callframes.count == 0)
				return STATUS_OK;
			break;

		case I_PRINT:
			uc_vm_insn_print(vm, insn);
			break;

		case I_DELETE:
			uc_vm_insn_delete(vm, insn);
			break;

		case I_IMPORT:
			uc_vm_insn_import(vm, insn);
			break;

		case I_EXPORT:
			uc_vm_insn_export(vm, insn);
			break;

		case I_DYNLOAD:
			uc_vm_insn_dynload(vm, insn);
			break;

		default:
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "unknown opcode %d", insn);
			break;
		}

exception:
		/* previous instruction raised exception */
		if (vm->exception.type != EXCEPTION_NONE) {
			/* VM termination was requested */
			if (vm->exception.type == EXCEPTION_EXIT) {
				uc_vm_reset_callframes(vm, caller);

				return STATUS_EXIT;
			}

			/* If a debugger has armed the dedicated "break on uncaught
			 * exception" system breakpoint and nothing between here and
			 * this invocation's original call depth would actually handle
			 * this exception, give it a chance to inspect the fully intact
			 * stack *before* uc_vm_handle_exception()'s loop below starts
			 * popping frames - once that happens, the original throwing
			 * frame's locals and exact position are gone for good. */
			if (!uc_vm_exception_would_be_caught(vm, caller)) {
				for (size_t i = 0; i < vm->breakpoints.count; i++) {
					uc_breakpoint_t *bk = vm->breakpoints.entries[i];

					if (bk != NULL && bk->ip == UC_BREAKPOINT_UNCAUGHT_EXCEPTION) {
						bk->cb(vm, bk);

						/* "quit" was issued from within the breakpoint's
						 * CLI session */
						if (vm->exception.type == EXCEPTION_EXIT) {
							uc_vm_reset_callframes(vm, caller);

							return STATUS_EXIT;
						}

						break;
					}
				}
			}

			/* walk up callframes until something handles the exception or the original caller is reached */
			while (!uc_vm_handle_exception(vm)) {
				/* no further callframe, report unhandled exception and terminate */
				if (vm->callframes.count == 0)
					return ERROR_RUNTIME;

				/* if VM returned into native function, don't bubble up */
				if (!vm->callframes.entries[vm->callframes.count - 1].closure)
					return ERROR_RUNTIME;

				/* no exception handler in current function, pop callframe */
				ucv_put(uc_vm_callframe_pop(vm));

				/* do not bubble past original call depth */
				if (vm->callframes.count <= caller)
					return ERROR_RUNTIME;
			}
		}

		/* the compiler emits the exports of a module as one block, do not run
		 * signal handlers inside of it, as an exception raised by one would
		 * leave a part of the module instance behind */
		if (insn == I_EXPORT && frame->ip[0] == I_EXPORT)
			continue;

		/* run handler for signal(s) delivered during previous instruction */
		if (uc_vm_signal_dispatch(vm) != EXCEPTION_NONE)
			goto exception;

		/* check for break request */
		if (vm->break_requested) {
			vm->break_requested = false;
			return STATUS_BREAK;
		}
	}

	return STATUS_OK;
}

uc_vm_status_t
uc_vm_execute(uc_vm_t *vm, uc_program_t *program, uc_value_t **retval)
{
	uc_vm_main_key_t key;
	uc_value_t *kept = uc_vm_registry_get(vm, uc_vm_main_key(program, key));
	uc_closure_t *closure = ucv_as_closure(kept ? ucv_get(kept) : uc_program_main(vm, program));
	uc_function_t *fn = closure->function;
	uc_vm_status_t status;
	uc_callframe_t *frame;
	uc_stringbuf_t *buf;
	uc_value_t *val;

	uc_vm_clear_exception(vm);

	frame = uc_vector_push(&vm->callframes, {
		.closure = closure,
		.stackframe = 0,
		.ip = fn->chunk.entries,
		.strict = fn->strict
	});

	uc_vm_run_begin(closure, !kept);

	if (vm->trace) {
		buf = uc_stringbuf_new();

		uc_source_context_format(buf, uc_vm_frame_source(frame), 0, true);

		fwrite(buf->buf, 1, uc_stringbuf_length(buf), stderr);
		uc_stringbuf_free(buf);

		uc_vm_frame_dump(vm, frame);
	}

	//uc_vm_stack_push(vm, closure->header.jso);
	uc_vm_stack_push(vm, NULL);

	status = uc_vm_execute_chunk(vm);

	switch (status) {
	case STATUS_OK:
		val = uc_vm_stack_pop(vm);

		if (retval)
			*retval = val;
		else
			ucv_put(val);

		break;

	case STATUS_EXIT:
		if (retval)
			*retval = ucv_int64_new(vm->arg.s32);

		break;

	case STATUS_BREAK:
		/* Break requested - exit gracefully without error */
		if (retval)
			*retval = NULL;

		break;

	default:
		if (vm->exhandler)
			vm->exhandler(vm, &vm->exception);

		if (retval)
			*retval = NULL;

		break;
	}

	return status;
}

static const uc_vm_interpreter_t uc_vm_bytecode_interpreter = {
	.frame_push = uc_vm_frame_push,
	.execute = uc_vm_execute_chunk
};

/* uc_vm_call() in vmcore.c enters bytecode closures through this hook, so
 * that a runtime which links vmcore.c without this file has no interpreter */
static void __attribute__((constructor))
uc_vm_bytecode_interpreter_register(void)
{
	uc_vm_interpreter_set(&uc_vm_bytecode_interpreter);
}

uc_vm_status_t
uc_vm_resume(uc_vm_t *vm)
{
	uc_vm_status_t status = uc_vm_execute_chunk(vm);

	switch (status) {
	case STATUS_OK:
	case STATUS_EXIT:
	case STATUS_BREAK:
		break;

	default:
		if (vm->exhandler)
			vm->exhandler(vm, &vm->exception);

		break;
	}

	return status;
}

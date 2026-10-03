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
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "ucode/internal/vm.h" /* ISA: I_*, uc_vm_insn_t, __insns */
#include "ucode/internal/program.h"
#include "ucode/internal/chunk.h"
#include "ucode/internal/lib.h" /* uc_error_context_format() */
#include "ucode/internal/source.h" /* uc_source_callee_expr() */
#include "ucode/internal/platform.h"

/* maximum length of the value rendered into a "not a function" error message */
#define UC_VALUE_HINT_MAXLEN 64

#undef __insn
#define __insn(_name) #_name,

static const char *insn_names[__I_MAX] = {
	__insns
};

const int8_t uc_vm_insn_format[__I_MAX] = {
	[I_LOAD] = 4,
	[I_LOAD8] = 1,
	[I_LOAD16] = 2,
	[I_LOAD32] = 4,

	[I_LREXP] = 4,

	[I_LLOC] = 4,
	[I_LVAR] = 4,
	[I_LUPV] = 4,

	[I_CLFN] = 4,
	[I_ARFN] = 4,

	[I_SLOC] = 4,
	[I_SUPV] = 4,
	[I_SVAR] = 4,

	[I_ULOC] = 4,
	[I_UUPV] = 4,
	[I_UVAR] = 4,
	[I_UVAL] = 1,

	[I_NARR] = 4,
	[I_PARR] = 4,

	[I_NOBJ] = 4,
	[I_SOBJ] = 4,

	[I_JMP] = -4,
	[I_JMPZ] = -4,
	[I_JMPNT] = 4,

	[I_COPY] = 1,

	[I_CALL] = 4,

	[I_IMPORT] = 4,
	[I_EXPORT] = 4,
	[I_DYNLOAD] = 4
};

const char *exception_type_strings[] = {
	[EXCEPTION_SYNTAX] = "Syntax error",
	[EXCEPTION_RUNTIME] = "Runtime error",
	[EXCEPTION_TYPE] = "Type error",
	[EXCEPTION_REFERENCE] = "Reference error",
	[EXCEPTION_USER] = "Error",
	[EXCEPTION_EXIT] = "Exit"
};


static const char *
uc_vm_insn_to_name(uc_vm_insn_t insn)
{
	if (insn < 0 || insn >= __I_MAX)
		return "(unknown)";

	return insn_names[insn];
}

int8_t
uc_vm_insn_to_argtype(uc_vm_insn_t insn)
{
	if (insn < 0 || insn >= __I_MAX)
		return 0;

	return uc_vm_insn_format[insn];
}

static void
uc_vm_reset_stack(uc_vm_t *vm)
{
	while (vm->stack.count > 0) {
		vm->stack.count--;
		ucv_put(vm->stack.entries[vm->stack.count]);
		vm->stack.entries[vm->stack.count] = NULL;
	}
}

static uc_value_t *
uc_vm_callframe_pop(uc_vm_t *vm);

static void
uc_vm_close_upvals(uc_vm_t *vm, size_t slot);

static void
uc_vm_reset_callframes(uc_vm_t *vm)
{
	while (vm->callframes.count > 0)
		ucv_put(uc_vm_callframe_pop(vm));
}

static uc_value_t *
uc_vm_alloc_global_scope(uc_vm_t *vm)
{
	uc_value_t *scope, *arr;
	size_t i;

	scope = ucv_object_new(vm);

	/* build default require() search path */
	arr = ucv_array_new(vm);

	for (i = 0; i < vm->config->module_search_path.count; i++)
		ucv_array_push(arr, ucv_string_new(vm->config->module_search_path.entries[i]));

	/* register module related constants */
	ucv_object_add(scope, "REQUIRE_SEARCH_PATH", arr);
	ucv_object_add(scope, "modules", ucv_object_new(vm));

	/* register scope math constants */
	ucv_object_add(scope, "NaN", ucv_double_new(NAN));
	ucv_object_add(scope, "Infinity", ucv_double_new(INFINITY));

	/* register global property */
	ucv_object_add(scope, "global", ucv_get(scope));

	uc_vm_scope_set(vm, scope);

	return scope;
}

static void
uc_vm_output_exception(uc_vm_t *vm, uc_exception_t *ex);

static void
uc_vm_signal_handler(int sig)
{
	uc_vm_t *vm = uc_thread_context_get()->signal_handler_vm;

	assert(vm);

	uc_vm_signal_raise(vm, sig);
}

/* Actually wire up the self-pipe/handler array/sigaction template needed
 * for ucode-level signal() callbacks to work, independent of whether the
 * embedding host opted into this via config->setup_signal_handlers. Safe
 * to call more than once (a no-op once already set up for this thread). */
void
uc_vm_signal_handlers_ensure(uc_vm_t *vm)
{
	uc_thread_context_t *tctx = uc_thread_context_get();

	if (tctx->signal_handler_vm)
		return;

	if (pipe2(vm->signal.sigpipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return;

	vm->signal.handler = ucv_array_new_length(vm, UC_SYSTEM_SIGNAL_COUNT);

	vm->signal.sa.sa_handler = uc_vm_signal_handler;
	vm->signal.sa.sa_flags = SA_RESTART | SA_ONSTACK;
	sigemptyset(&vm->signal.sa.sa_mask);

	tctx->signal_handler_vm = vm;
}

static void
uc_vm_signal_handlers_setup(uc_vm_t *vm)
{
	memset(&vm->signal, 0, sizeof(vm->signal));

	vm->signal.sigpipe[0] = -1;
	vm->signal.sigpipe[1] = -1;

	if (!vm->config->setup_signal_handlers)
		return;

	uc_vm_signal_handlers_ensure(vm);
}

static void
uc_vm_signal_handlers_reset(uc_vm_t *vm)
{
	uc_thread_context_t *tctx = uc_thread_context_get();
	struct sigaction sa = { 0 };
	size_t i, signo;

	if (vm != tctx->signal_handler_vm)
		return;

	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);

	for (signo = 0; signo < ucv_array_length(vm->signal.handler); signo++)
		if (ucv_is_callable(ucv_array_get(vm->signal.handler, signo)))
			sigaction(signo, &sa, NULL);

	for (i = 0; i < ARRAY_SIZE(vm->signal.sigpipe); i++) {
		if (vm->signal.sigpipe[i] > STDERR_FILENO)
			close(vm->signal.sigpipe[i]);

		vm->signal.sigpipe[i] = -1;
	}

		tctx->signal_handler_vm = NULL;
}

void uc_vm_break_init(uc_vm_t *vm)
{
	vm->break_requested = false;
	vm->break_notifyfd[0] = -1;
	vm->break_notifyfd[1] = -1;

	if (pipe2(vm->break_notifyfd, O_CLOEXEC | O_NONBLOCK) == 0) {
		/* pipe created successfully */
	}
}

void uc_vm_break_cleanup(uc_vm_t *vm)
{
	for (size_t i = 0; i < ARRAY_SIZE(vm->break_notifyfd); i++) {
		if (vm->break_notifyfd[i] > STDERR_FILENO)
			close(vm->break_notifyfd[i]);
		vm->break_notifyfd[i] = -1;
	}
}

void uc_vm_init(uc_vm_t *vm, uc_parse_config_t *config)
{
	vm->exception.type = EXCEPTION_NONE;
	vm->exception.message = NULL;

	vm->config = config ? config : &uc_default_parse_config;

	vm->open_upvals = NULL;

	vm->sources = lh_kchar_table_new(16, NULL);

	vm->values.prev = &vm->values;
	vm->values.next = &vm->values;

	vm->strbuf = NULL;

	vm->output = stdout;

	uc_vm_reset_stack(vm);

	uc_vm_alloc_global_scope(vm);

	uc_vm_exception_handler_set(vm, uc_vm_output_exception);

	uc_vm_trace_set(vm, 0);

	uc_vm_signal_handlers_setup(vm);

	uc_vm_break_init(vm);

	uc_thread_context_get()->refcount++;
}

void uc_vm_free(uc_vm_t *vm)
{
	uc_thread_context_t *ctx;
	uc_upvalref_t *ref;
	size_t i;

	uc_vm_signal_handlers_reset(vm);

	uc_vm_break_cleanup(vm);

	ucv_put(vm->exception.stacktrace);
	free(vm->exception.message);

	while (vm->open_upvals) {
		ref = vm->open_upvals->next;
		ucv_put(&vm->open_upvals->header);
		vm->open_upvals = ref;
	}

	/* Resource type prototypes are part of the VM value pool, so release the
	 * restype refs before the final GC; any references still held by
	 * GC-managed values are dropped by ucv_freeall() itself. */
	for (i = 0; i < vm->restypes.count; i++)
		ucv_put(vm->restypes.entries[i]->proto);

	uc_vm_reset_callframes(vm);
	uc_vm_reset_stack(vm);
	uc_vector_clear(&vm->stack);
	uc_vector_clear(&vm->callframes);

	printbuf_free(vm->strbuf);

	ucv_freeall(vm);

	/* Release source cache entries after freeall so that all program refs
	 * to sources are already gone and the cache refs are the last ones. */
	if (vm->sources) {
		struct lh_entry *entry, *next;

		entry = vm->sources->head;

		while (entry) {
			next = entry->next;
			ucv_put((uc_value_t *)lh_entry_v(entry));
			free((char *)lh_entry_k(entry));
			entry = next;
		}

		lh_table_free(vm->sources);
	}

	for (i = 0; i < vm->restypes.count; i++)
		free(vm->restypes.entries[i]);

	uc_vector_clear(&vm->restypes);

	for (i = 0; i < vm->breakpoints.count; i++)
		free(vm->breakpoints.entries[i]);

	uc_vector_clear(&vm->breakpoints);

	ctx = uc_thread_context_get();

	assert(ctx->refcount > 0);

	if (--ctx->refcount == 0)
		uc_thread_context_free();
}

static uc_chunk_t *
uc_vm_frame_chunk(uc_callframe_t *frame)
{
	return frame->closure ? &frame->closure->function->chunk : NULL;
}

static uc_program_t *
uc_vm_frame_program(uc_callframe_t *frame)
{
	return frame->closure ? frame->closure->function->program : NULL;
}

static uc_source_t *
uc_vm_frame_source(uc_callframe_t *frame)
{
	return frame->closure ? uc_program_function_source(frame->closure->function) : NULL;
}

static uc_callframe_t *
uc_vm_current_frame(uc_vm_t *vm)
{
	return uc_vector_last(&vm->callframes);
}

static uc_program_t *
uc_vm_current_program(uc_vm_t *vm)
{
	return uc_vm_frame_program(uc_vm_current_frame(vm));
}

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


static char *
uc_vm_format_val(uc_vm_t *vm, uc_value_t *val)
{
	if (!vm->strbuf)
		vm->strbuf = xprintbuf_new();
	else
		printbuf_reset(vm->strbuf);

	ucv_to_stringbuf(NULL, vm->strbuf, val, true);

	if (printbuf_length(vm->strbuf) >= 64) {
		printbuf_memset(vm->strbuf, 60, '.', 3);
		printbuf_memset(vm->strbuf, 63, 0, 1);
	}

	return vm->strbuf->buf;
}

static void
uc_vm_frame_dump(uc_vm_t *vm, uc_callframe_t *frame)
{
	uc_chunk_t *chunk = uc_vm_frame_chunk(frame);
	uc_function_t *function;
	uc_closure_t *closure;
	uc_upvalref_t *ref;
	uc_value_t *v;
	size_t i;

	fprintf(stderr, "  [*] CALLFRAME[%zx]\n",
		frame - vm->callframes.entries);

	fprintf(stderr, "   |- stackframe %zu/%zu\n",
		frame->stackframe, vm->stack.count);

	fprintf(stderr, "   |- ctx %s\n",
		uc_vm_format_val(vm, frame->ctx));

	if (frame->tco)
		fprintf(stderr, "   |- tailcall (%u callframes omitted)\n",
			frame->tco);

	if (chunk) {
		closure = frame->closure;
		function = closure->function;

		fprintf(stderr, "   `- %zu upvalues\n",
			function->nupvals);

		for (i = 0; i < function->nupvals; i++) {
			ref = closure->upvals[i];
			v = uc_chunk_debug_get_variable(chunk, 0, i, true);

			fprintf(stderr, "     [%zu] <%p> %s ",
				i, (void *)ref, uc_vm_format_val(vm, v));

			if (!ref) {
				fprintf(stderr, "{unresolved}\n");
			}
			else if (ref->closed) {
				fprintf(stderr, "{closed} %s\n",
					uc_vm_format_val(vm, ref->value));
			}
			else {
				fprintf(stderr, "{open[%zu]} %s\n",
					ref->slot,
					uc_vm_format_val(vm, vm->stack.entries[ref->slot]));
			}

			ucv_put(v);
		}
	}
}

static uc_value_t *
uc_vm_resolve_upval(uc_vm_t *vm, uc_value_t *value)
{
	uc_upvalref_t *ref;
	uc_value_t *rv;

#ifdef __clang_analyzer__
	/* Clang static analyzer does not understand that ucv_type(NULL) can't
	 * possibly yield UC_UPVALUE. Nudge it. */
	if (value != NULL && ucv_type(value) == UC_UPVALUE)
#else
	if (ucv_type(value) == UC_UPVALUE)
#endif
	{
		ref = ucv_as_upvalue(value);

		if (ref->closed)
			rv = ucv_get(ref->value);
		else
			rv = ucv_get(vm->stack.entries[ref->slot]);

		ucv_put(value);

		return rv;
	}

	return value;
}

void
uc_vm_stack_push(uc_vm_t *vm, uc_value_t *value)
{
	uc_vector_push(&vm->stack, uc_vm_resolve_upval(vm, value));

	if (vm->trace) {
		fprintf(stderr, "  [+%zd] %s\n",
			vm->stack.count - 1,
			uc_vm_format_val(vm, vm->stack.entries[vm->stack.count - 1]));
	}
}

uc_value_t *
uc_vm_stack_pop(uc_vm_t *vm)
{
	uc_value_t *rv;

	vm->stack.count--;
	rv = vm->stack.entries[vm->stack.count];
	vm->stack.entries[vm->stack.count] = NULL;

	if (vm->trace) {
		fprintf(stderr, "  [-%zd] %s\n",
			vm->stack.count,
			uc_vm_format_val(vm, rv));
	}

	return rv;
}

uc_value_t *
uc_vm_stack_peek(uc_vm_t *vm, size_t offset)
{
	return vm->stack.entries[vm->stack.count + (-1 - offset)];
}

static void
uc_vm_stack_set(uc_vm_t *vm, size_t offset, uc_value_t *value)
{
	if (vm->trace) {
		fprintf(stderr, "  [!%zu] %s\n",
			offset,
			uc_vm_format_val(vm, value));
	}

	ucv_put(vm->stack.entries[offset]);
	vm->stack.entries[offset] = value;
}

static void
uc_vm_call_native(uc_vm_t *vm, uc_value_t *ctx, uc_cfunction_t *fptr, bool mcall, size_t nargs)
{
	uc_value_t *res = NULL;
	uc_callframe_t *frame;

	/* add new callframe */
	frame = uc_vector_push(&vm->callframes, {
		.stackframe = vm->stack.count - nargs - 1,
		.cfunction = fptr,
		.closure = NULL,
		.ctx = ctx,
		.mcall = mcall
	});

	if (vm->trace)
		uc_vm_frame_dump(vm, frame);

	res = fptr->cfn(vm, nargs);

	/* Reset stack, check for callframe depth since an uncatched exception in managed
	 * code executed by fptr->cfn() could've reset the callframe stack already. */
	if (vm->callframes.count > 0)
		ucv_put(uc_vm_callframe_pop(vm));

	/* push return value */
	if (!vm->exception.type)
		uc_vm_stack_push(vm, res);
	else
		ucv_put(res);
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

/* Returns the expression invoked by the call that the instruction the VM is
 * about to execute belongs to, or NULL if it cannot be determined. The result
 * is meant for error messages describing a failed call, and stays valid until
 * the next call. */
static const char *
uc_vm_callee_expr(uc_vm_t *vm)
{
	uc_function_t *function;
	uc_callframe_t *frame;
	size_t insn;

	/* the topmost frame is the one performing the failing call; if it belongs
	 * to a native function, the call did not originate from source code */
	if (vm->callframes.count == 0)
		return NULL;

	frame = &vm->callframes.entries[vm->callframes.count - 1];

	if (!frame->closure)
		return NULL;

	function = frame->closure->function;
	insn = (frame->ip - uc_vm_frame_chunk(frame)->entries) - 1;

	/* a call instruction refers to the closing parenthesis of the arguments
	 * passed to it */
	return uc_source_callee_expr(uc_program_function_source(function),
	                             uc_program_function_srcpos(function, insn));
}

static bool
uc_vm_call_function(uc_vm_t *vm, uc_value_t *ctx, uc_value_t *fno, bool mcall,
                   size_t argspec, bool tail)
{
	size_t i, j, stackoff, nargs = argspec & 0xffff;
	size_t nspreads = (argspec >> 16) & 0x7fff;
	uc_callframe_t *frame = NULL;
	uc_value_t *ellip, *arg;
	uc_function_t *function;
	uc_closure_t *closure;
	uint16_t slot, tmp;
	char *s;

	/* a tail invocation reuses the current frame, so it never grows the stack */
	if (!tail && vm->callframes.count >= 1000) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Too much recursion");
		ucv_put(ctx);
		ucv_put(fno);

		return false;
	}

	stackoff = vm->stack.count - nargs - 1;

	/* argument list contains spread operations, we need to reshuffle the stack */
	if (nspreads > 0) {
		frame = uc_vm_current_frame(vm);

		/* create temporary array */
		ellip = ucv_array_new_length(vm, nargs);

		/* pop original stack values and push to temp array in reverse order */
		for (i = 0; i < nargs; i++)
			ucv_array_push(ellip, uc_vm_stack_pop(vm));

		/* for each spread value index ... */
		for (i = 0, slot = nargs; i < nspreads; i++) {
			/* decode stack depth value */
			tmp = frame->ip[0] * 0x100 + frame->ip[1];
			frame->ip += 2;

			/* push each preceeding non-spread value to the stack */
			for (j = slot; j > tmp + 1UL; j--)
				uc_vm_stack_push(vm, ucv_get(ucv_array_get(ellip, j - 1)));

			/* read spread value at index... */
			slot = tmp;
			arg = ucv_get(ucv_array_get(ellip, slot));

			/* ... ensure that it is an array type ... */
			if (ucv_type(arg) != UC_ARRAY) {
				s = ucv_to_string(vm, arg);
				uc_vm_raise_exception(vm, EXCEPTION_TYPE, "(%s) is not iterable", s);
				free(s);
				ucv_put(ctx);
				ucv_put(fno);
				ucv_put(ellip);

				return false;
			}

			/* ... and push each spread array value as argument to the stack */
			for (j = 0; j < ucv_array_length(arg); j++)
				uc_vm_stack_push(vm, ucv_get(ucv_array_get(arg, j)));

			ucv_put(arg);
		}

		/* push remaining non-spread arguments to the stack */
		for (i = slot; i > 0; i--)
			uc_vm_stack_push(vm, ucv_get(ucv_array_get(ellip, i - 1)));

		/* free temp array */
		ucv_put(ellip);

		/* update arg count */
		nargs = vm->stack.count - stackoff - 1;
	}

	/* __call__ dispatch: if fno is not itself callable (not a closure or
	 * cfunction) but carries a __call__ metamethod somewhere in its prototype
	 * chain, invoke that method with the instance as `this`. The instance
	 * becomes the new ctx; the original ctx (if any) is discarded. The tail
	 * flag is preserved so `return foo()` still TCOs through the dispatch. */
	if (ucv_type(fno) != UC_CFUNCTION && ucv_type(fno) != UC_CLOSURE) {
		uc_value_t *meta = ucv_metamethod_lookup(fno, "__call__");

		if (ucv_is_callable(meta)) {
			ucv_put(ctx);

			/* the instance becomes `this` and is invoked in place of the
			 * __call__ method, so hand the reference to ctx over */
			ctx = fno;
			fno = ucv_get(meta);
		}
	}

	/* is a native function; such functions are always invoked in a fresh frame,
	 * even when requested as tail call, since the foreign code may rely on the
	 * stack layout of an ordinary invocation */
	if (ucv_type(fno) == UC_CFUNCTION) {
		uc_vm_call_native(vm, ctx, ucv_as_cfunction(fno), mcall, nargs);

		return true;
	}

	if (ucv_type(fno) != UC_CLOSURE) {
		/* name the invoked expression where it can be recovered from the source,
		 * and render the value it held as JSON, so that the message identifies
		 * both the offending expression and the value that was not a function;
		 * a NULL vm skips __tostring__ dispatch, so describing the value has no
		 * side effects */
		const char *callee = uc_vm_callee_expr(vm);
		char *hint = ucv_to_jsonstring(NULL, fno);
		size_t hlen = strlen(hint);

		if (hlen > UC_VALUE_HINT_MAXLEN)
			memcpy(hint + UC_VALUE_HINT_MAXLEN - 3, "...", 4);

		if (callee)
			uc_vm_raise_exception(vm, EXCEPTION_TYPE, "`%s` is not a function, got %s instead", callee, hint);
		else
			uc_vm_raise_exception(vm, EXCEPTION_TYPE, "left-hand side is not a function, got %s instead", hint);

		free(hint);
		ucv_put(ctx);
		ucv_put(fno);

		return false;
	}

	closure = ucv_as_closure(fno);
	function = closure->function;

	/* fewer arguments on stack than function expects => pad */
	if (nargs < function->nargs) {
		for (i = nargs; i < function->nargs; i++) {
			if (function->vararg && (i + 1) == function->nargs)
				uc_vm_stack_push(vm, ucv_array_new_length(vm, 0));
			else
				uc_vm_stack_push(vm, NULL);
		}
	}

	/* more arguments on stack than function expects... */
	else if (nargs > function->nargs - function->vararg) {
		/* is a vararg function => pass excess args as array */
		if (function->vararg) {
			ellip = ucv_array_new_length(vm, nargs - (function->nargs - 1));

			for (i = function->nargs; i <= nargs; i++)
				ucv_array_push(ellip, uc_vm_stack_peek(vm, nargs - i));

			for (i = function->nargs; i <= nargs; i++)
				uc_vm_stack_pop(vm);

			uc_vm_stack_push(vm, ellip);
		}

		/* static amount of args => drop excess values */
		else {
			for (i = function->nargs; i < nargs; i++)
				ucv_put(uc_vm_stack_pop(vm));
		}
	}

	/* tail call: reuse the current call frame instead of pushing a new one. The
	 * number of values above the invoked function may differ from the encoded
	 * argument count due to spread operations or excess argument handling */
	if (tail) {
		uc_vm_frame_reinit(vm, closure, ctx, mcall, vm->stack.count - stackoff - 1);

		return true;
	}

	frame = uc_vector_push(&vm->callframes, {
		.stackframe = stackoff,
		.cfunction = NULL,
		.closure = closure,
		.ctx = ctx,
		.ip = function->chunk.entries,
		.mcall = mcall,
		.strict = function->strict
	});

	if (vm->trace)
		uc_vm_frame_dump(vm, frame);

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
		buf = xprintbuf_new();

		uc_source_context_format(buf, source, srcpos, true);
		fwrite(buf->buf, 1, printbuf_length(buf), stderr);
		printbuf_free(buf);

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

	default:
		break;
	}

	fprintf(stderr, "\n");
}

static uc_value_t *
uc_vm_exception_tostring(uc_vm_t *vm, size_t nargs)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_value_t *message = ucv_object_get(frame->ctx, "message", NULL);

	return message ? ucv_get(message) : ucv_string_new("Exception");
}

uc_value_t *
uc_vm_exception_object(uc_vm_t *vm)
{
	uc_exception_type_t type = vm->exception.type;
	const char *message = vm->exception.message;
	uc_value_t *stacktrace = vm->exception.stacktrace;
	uc_value_t *exception_prototype = uc_vm_registry_get(vm, "vm.exception.proto");
	uc_value_t *exo;

	if (exception_prototype == NULL) {
		exception_prototype = ucv_object_new(vm);

		ucv_object_add(exception_prototype, "tostring",
			ucv_cfunction_new("tostring", uc_vm_exception_tostring));

		uc_vm_registry_set(vm, "vm.exception.proto", exception_prototype);
	}

	exo = ucv_object_new(vm);

	ucv_object_add(exo, "type", ucv_string_new(exception_type_strings[type]));
	ucv_object_add(exo, "message", ucv_string_new(message));
	ucv_object_add(exo, "stacktrace", ucv_get(stacktrace));

	ucv_prototype_set(exo, ucv_get(exception_prototype));

	return exo;
}

static void
uc_vm_clear_exception(uc_vm_t *vm)
{
	vm->exception.type = EXCEPTION_NONE;

	ucv_put(vm->exception.stacktrace);
	vm->exception.stacktrace = NULL;

	free(vm->exception.message);
	vm->exception.message = NULL;
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

/* Adds the line and byte column of the source position `off` to `object`, under
 * the keys `linekey` and `bytekey`. */
static void
uc_vm_stacktrace_add_position(uc_value_t *object, uc_source_t *source, size_t off,
                             const char *linekey, const char *bytekey)
{
	size_t pos = off;

	ucv_object_add(object, linekey, ucv_int64_new(uc_source_get_line(source, &pos)));
	ucv_object_add(object, bytekey, ucv_int64_new(pos));
}

/* Adds the bounds of the statement enclosing instruction `off` of `function`,
 * so that consumers rendering a stacktrace can highlight the whole statement
 * rather than only the failing position. */
static void
uc_vm_stacktrace_add_statement(uc_vm_t *vm, uc_value_t *entry, uc_function_t *function, size_t off)
{
	uc_source_t *source = uc_program_function_source(function);
	size_t startoff, endoff;
	uc_value_t *statement;

	if (!uc_program_function_stmt_bounds(function, off, &startoff, &endoff))
		return;

	statement = ucv_object_new(vm);

	uc_vm_stacktrace_add_position(statement, source, startoff, "line", "byte");
	uc_vm_stacktrace_add_position(statement, source, endoff, "endline", "endbyte");

	ucv_object_add(entry, "statement", statement);
}

static uc_value_t *
uc_vm_capture_stacktrace(uc_vm_t *vm, size_t i)
{
	uc_value_t *stacktrace, *entry, *last = NULL;
	uc_function_t *function;
	uc_callframe_t *frame;
	uc_source_t *source;
	size_t off, srcpos;
	char *name;

	stacktrace = ucv_array_new(vm);

	for (; i > 0; i--) {
		frame = &vm->callframes.entries[i - 1];
		entry = ucv_object_new(vm);

		if (frame->closure) {
			function = frame->closure->function;
			source = uc_program_function_source(function);

			off = (frame->ip - uc_vm_frame_chunk(frame)->entries) - 1;
			srcpos = uc_program_function_srcpos(function, off);

			ucv_object_add(entry, "filename", ucv_string_new(source->filename));
			uc_vm_stacktrace_add_position(entry, source, srcpos, "line", "byte");

			uc_vm_stacktrace_add_statement(vm, entry, function, off);
		}

		if (i > 1) {
			if (frame->closure) {
				if (frame->closure->function->name[0])
					name = frame->closure->function->name;
				else if (frame->closure->is_arrow)
					name = "[arrow function]";
				else
					name = "[anonymous function]";
			}
			else {
				name = frame->cfunction->name;
			}

			ucv_object_add(entry, "function", ucv_string_new(name));
		}

		/* record the number of call frames collapsed into this one by tail call
		 * optimization, so consumers can indicate the missing frames */
		if (frame->tco)
			ucv_object_add(entry, "tco", ucv_int64_new(frame->tco));

		if (!ucv_is_equal(last, entry)) {
			ucv_array_push(stacktrace, entry);
			last = entry;
		}
		else {
			ucv_put(entry);
		}
	}

	return stacktrace;
}

static uc_value_t *
uc_vm_get_error_context(uc_vm_t *vm)
{
	size_t offset, insn, i, byte, line, startoff = 0, endoff = 0;
	uc_value_t *stacktrace;
	uc_callframe_t *frame;
	uc_stringbuf_t *buf;
	uc_chunk_t *chunk;

	if (vm->callframes.count == 0)
		return NULL;

	/* skip to first non-native function call frame */
	for (i = vm->callframes.count; i > 1; i--)
		if (vm->callframes.entries[i - 1].closure)
			break;

	frame = &vm->callframes.entries[i - 1];

	if (!frame->closure)
		return NULL;

	chunk = uc_vm_frame_chunk(frame);
	insn = (frame->ip - chunk->entries) - 1;
	offset = uc_program_function_srcpos(frame->closure->function, insn);
	stacktrace = uc_vm_capture_stacktrace(vm, i);

	buf = ucv_stringbuf_new();

	byte = offset;
	line = uc_source_get_line(uc_program_function_source(frame->closure->function), &byte);

	if (line) {
		/* underline the statement the failing instruction belongs to, where one
		 * can be resolved, rather than only marking that instruction */
		uc_program_function_stmt_bounds(frame->closure->function, insn, &startoff, &endoff);

		uc_error_context_format(buf, uc_vm_frame_source(frame), stacktrace,
			startoff, endoff, offset);
	}
	else if (frame->ip != chunk->entries)
		ucv_stringbuf_printf(buf, "At instruction %zu", (frame->ip - chunk->entries) - 1);
	else
		ucv_stringbuf_append(buf, "At start of program");

	ucv_object_add(ucv_array_get(stacktrace, 0), "context", ucv_stringbuf_finish(buf));

	return stacktrace;
}

void __attribute__((format(printf, 3, 0)))
uc_vm_raise_exception(uc_vm_t *vm, uc_exception_type_t type, const char *fmt, ...)
{
	va_list ap;

	vm->exception.type = type;

	free(vm->exception.message);

	va_start(ap, fmt);
	xvasprintf(&vm->exception.message, fmt, ap);
	va_end(ap);

	ucv_put(vm->exception.stacktrace);
	vm->exception.stacktrace = uc_vm_get_error_context(vm);
}

static bool
uc_vm_test_strict_equality(uc_value_t *v1, uc_value_t *v2, bool nan_equal)
{
	uc_type_t t1 = ucv_type(v1);
	uc_type_t t2 = ucv_type(v2);
	double d1, d2;

	if (t1 != t2)
		return false;

	switch (t1) {
	case UC_DOUBLE:
		d1 = ucv_as_double(v1)->dbl;
		d2 = ucv_as_double(v2)->dbl;

		if (isnan(d1) && isnan(d2))
			return nan_equal;

		return (d1 == d2);

	case UC_NULL:
	case UC_BOOLEAN:
	case UC_INTEGER:
	case UC_STRING:
		return ucv_is_equal(v1, v2);

	default:
		return (v1 == v2);
	}
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
	bool icase = false, newline = false, global = false;
	char *str, *err = NULL;

	if (ucv_type(jstr) != UC_STRING || ucv_string_length(jstr) < 2) {
		uc_vm_stack_push(vm, NULL);
		ucv_put(jstr);

		return;
	}

	str = ucv_string_get(jstr);

	global  = (*str & (1 << 0));
	icase   = (*str & (1 << 1));
	newline = (*str & (1 << 2));

	re = ucv_regexp_new(++str, icase, newline, global, &err);

	ucv_put(jstr);

	if (re)
		uc_vm_stack_push(vm, re);
	else
		uc_vm_raise_exception(vm, EXCEPTION_SYNTAX, "%s", err);

	free(err);
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
	uc_value_t *name, *val = NULL;
	uc_value_t *scope, *next;
	bool found;

	scope = vm->globals;
	name = uc_program_get_constant(uc_vm_current_program(vm), vm->arg.u32);

	while (ucv_type(name) == UC_STRING) {
		val = ucv_object_get(scope, ucv_string_get(name), &found);

		if (found)
			break;

		next = ucv_prototype_get(scope);

		if (!next) {
			if (uc_vm_is_strict(vm)) {
				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "access to undeclared variable %s",
				                      ucv_string_get(name));
			}

			break;
		}

		scope = next;
	}

	ucv_put(name);

	uc_vm_stack_push(vm, ucv_get(val));
}

static void
uc_vm_insn_load_val(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_pop(vm);

	switch (ucv_type(v)) {
	case UC_RESOURCE:
	case UC_OBJECT:
	case UC_ARRAY:
		/* aware read: dispatches __get__ on a missing key */
		uc_vm_stack_push(vm, ucv_key_get(vm, v, k));
		break;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
		                      "left-hand side expression is %s",
		                      v ? "not an array or object" : "null");

		break;
	}

	ucv_put(k);
	ucv_put(v);
}

static void
uc_vm_insn_peek_val(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_peek(vm, 0);

	switch (ucv_type(v)) {
	case UC_RESOURCE:
	case UC_OBJECT:
	case UC_ARRAY:
		/* aware read: dispatches __get__ on a missing key */
		uc_vm_stack_push(vm, ucv_key_get(vm, v, k));
		break;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
								"left-hand side expression is %s",
								v ? "not an array or object" : "null");

		break;
	}

	ucv_put(k);
}

static void
uc_vm_insn_load_upval(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_upvalref_t *ref = frame->closure->upvals[vm->arg.u32];

	if (ref->closed)
		uc_vm_stack_push(vm, ucv_get(ref->value));
	else
		uc_vm_stack_push(vm, ucv_get(vm->stack.entries[ref->slot]));
}

static void
uc_vm_insn_load_local(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);

	uc_vm_stack_push(vm, ucv_get(vm->stack.entries[frame->stackframe + vm->arg.u32]));
}

static uc_upvalref_t *
uc_vm_capture_upval(uc_vm_t *vm, size_t slot)
{
	uc_upvalref_t *curr = vm->open_upvals;
	uc_upvalref_t *prev = NULL;
	uc_upvalref_t *created;
	char *s;

	while (curr && curr->slot > slot) {
		prev = curr;
		curr = curr->next;
	}

	if (curr && curr->slot == slot) {
		if (vm->trace) {
			s = ucv_to_string(NULL, vm->stack.entries[slot]);
			fprintf(stderr, "  {+%zu} <%p> %s\n", slot, (void *)curr, s);
			free(s);
		}

		return curr;
	}

	created = (uc_upvalref_t *)ucv_upvalref_new(slot);
	created->next = curr;

	if (vm->trace) {
		s = ucv_to_string(NULL, vm->stack.entries[slot]);
		fprintf(stderr, "  {*%zu} <%p> %s\n", slot, (void *)created, s);
		free(s);
	}

	if (prev)
		prev->next = created;
	else
		vm->open_upvals = created;

	return created;
}

static void
uc_vm_close_upvals(uc_vm_t *vm, size_t slot)
{
	uc_upvalref_t *ref;

	while (vm->open_upvals && vm->open_upvals->slot >= slot) {
		ref = vm->open_upvals;
		ref->value = ucv_get(vm->stack.entries[ref->slot]);
		ref->closed = true;

		if (vm->trace) {
			fprintf(stderr, "  {!%zu} <%p> %s\n", ref->slot,
				(void *)ref,
				uc_vm_format_val(vm, ref->value));
		}

		vm->open_upvals = ref->next;
		ucv_put(&ref->header);
	}
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

		if (uv < 0)
			closure->upvals[i] = uc_vm_capture_upval(vm, frame->stackframe - (uv + 1));
		else
			closure->upvals[i] = frame->closure->upvals[uv];

		ucv_get(&closure->upvals[i]->header);

		frame->ip += 4;
	}
}

static void
uc_vm_insn_store_var(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *name, *v = uc_vm_stack_pop(vm);
	uc_value_t *scope, *next;
	bool found;

	scope = vm->globals;
	name = uc_program_get_constant(uc_vm_current_program(vm), vm->arg.u32);

	while (ucv_type(name) == UC_STRING) {
		ucv_object_get(scope, ucv_string_get(name), &found);

		if (found)
			break;

		next = ucv_prototype_get(scope);

		if (!next) {
			if (uc_vm_is_strict(vm)) {
				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "access to undeclared variable %s",
				                      ucv_string_get(name));
				ucv_put(name);
				ucv_put(v);

				return;
			}

			break;
		}

		scope = next;
	}

	if (scope && ucv_type(name) == UC_STRING)
		ucv_object_add(scope, ucv_string_get(name), ucv_get(v));

	ucv_put(name);
	uc_vm_stack_push(vm, v);
}

static bool
assert_mutable_value(uc_vm_t *vm, uc_value_t *val)
{
	if (ucv_is_constant(val)) {
		uc_vm_stack_push(vm, NULL);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
		                      "%s value is immutable",
		                      ucv_typename(val));

		return false;
	}

	return true;
}

static void
uc_vm_insn_store_val(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *v = uc_vm_stack_pop(vm);
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *o = uc_vm_stack_pop(vm);

	switch (ucv_type(o)) {
	case UC_OBJECT:
	case UC_ARRAY:
	case UC_RESOURCE:
		if (assert_mutable_value(vm, o)) {
			/* ucv_key_set() retains the stored value on its own and returns a
			 * reference to it, which becomes the value of the assignment; for
			 * resources it dispatches a __set__ metamethod when present. In
			 * strict mode it raises a type error when a non-index property
			 * store on an array would otherwise be silently dropped. */
			uc_vm_stack_push(vm, ucv_key_set(vm, o, k, v));
		}

		break;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
		                      "attempt to set property on %s value",
		                      ucv_typename(o));
	}

	ucv_put(v);
	ucv_put(o);
	ucv_put(k);
}

static void
uc_vm_insn_store_upval(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_upvalref_t *ref = frame->closure->upvals[vm->arg.u32];
	uc_value_t *val = ucv_get(uc_vm_stack_peek(vm, 0));

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
	uc_value_t *val = ucv_get(uc_vm_stack_peek(vm, 0));

	uc_vm_stack_set(vm, frame->stackframe + vm->arg.u32, val);
}

static int64_t
int64(uc_value_t *nv, uint64_t *u64)
{
	int64_t n;

	n = ucv_int64_get(nv);
	*u64 = 0;

	if (errno == ERANGE) {
		n = INT64_MAX;
		*u64 = ucv_uint64_get(nv);
	}

	return n;
}

static uint64_t
abs64(int64_t n)
{
	if (n == INT64_MIN)
		return 0x8000000000000000ULL;

	if (n < 0)
		return -n;

	return n;
}


static uc_value_t *
uc_vm_value_bitop(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *value, uc_value_t *operand)
{
	uc_value_t *nv1, *nv2, *rv = NULL;
	uint64_t u1, u2;
	int64_t n1, n2;

	nv1 = ucv_to_number(value);
	nv2 = ucv_to_number(operand);

	n1 = int64(nv1, &u1);
	n2 = int64(nv2, &u2);

	if (n1 < 0 || n2 < 0) {
		switch (operation) {
		case I_LSHIFT:
			rv = ucv_int64_new(n1 << n2);
			break;

		case I_RSHIFT:
			rv = ucv_int64_new(n1 >> n2);
			break;

		case I_BAND:
			rv = ucv_int64_new(n1 & n2);
			break;

		case I_BXOR:
			rv = ucv_int64_new(n1 ^ n2);
			break;

		case I_BOR:
			rv = ucv_int64_new(n1 | n2);
			break;

		default:
			break;
		}
	}
	else {
		if (!u1) u1 = (uint64_t)n1;
		if (!u2) u2 = (uint64_t)n2;

		switch (operation) {
		case I_LSHIFT:
			rv = ucv_uint64_new(u1 << (u2 % (sizeof(uint64_t) * CHAR_BIT)));
			break;

		case I_RSHIFT:
			rv = ucv_uint64_new(u1 >> (u2 % (sizeof(uint64_t) * CHAR_BIT)));
			break;

		case I_BAND:
			rv = ucv_uint64_new(u1 & u2);
			break;

		case I_BXOR:
			rv = ucv_uint64_new(u1 ^ u2);
			break;

		case I_BOR:
			rv = ucv_uint64_new(u1 | u2);
			break;

		default:
			break;
		}
	}

	ucv_put(nv1);
	ucv_put(nv2);

	return rv;
}

static uc_value_t *
uc_vm_string_concat(uc_vm_t *vm, uc_value_t *v1, uc_value_t *v2)
{
	char buf[sizeof(void *)], *s1, *s2, *str;
	uc_value_t *ustr;
	uc_stringbuf_t *sbuf;
	size_t l1, l2;

	/* optimize cases for string+string concat... */
	if (ucv_type(v1) == UC_STRING && ucv_type(v2) == UC_STRING) {
		s1 = ucv_string_get(v1);
		s2 = ucv_string_get(v2);
		l1 = ucv_string_length(v1);
		l2 = ucv_string_length(v2);

		/* ... result fits into a tagged pointer */
		if (l1 + l2 + 1 < sizeof(buf)) {
			memcpy(&buf[0], s1, l1);
			memcpy(&buf[l1], s2, l2);

			return ucv_string_new_length(buf, l1 + l2);
		}
		else {
			ustr = ucv_string_alloc(&str, l1 + l2);
			memcpy(&str[0], s1, l1);
			memcpy(&str[l1], s2, l2);

			return ustr;
		}
	}

	sbuf = ucv_stringbuf_new();

	ucv_to_stringbuf(vm, sbuf, v1, false);
	ucv_to_stringbuf(vm, sbuf, v2, false);

	return ucv_stringbuf_finish(sbuf);
}

static uint64_t
upow64(uint64_t base, uint64_t exponent)
{
	uint64_t result = 1;

	while (exponent) {
		if (exponent & 1)
			result *= base;

		exponent >>= 1;
		base *= base;
	}

	return result;
}

static uc_value_t *
uc_vm_value_arith(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *value, uc_value_t *operand)
{
	uc_value_t *nv1, *nv2, *rv = NULL;
	uint64_t u1, u2;
	int64_t n1, n2;
	double d1, d2;

	if (operation == I_LSHIFT || operation == I_RSHIFT ||
	    operation == I_BAND || operation == I_BXOR || operation == I_BOR)
		return uc_vm_value_bitop(vm, operation, value, operand);

	if (operation == I_ADD && (ucv_type(value) == UC_STRING || ucv_type(operand) == UC_STRING))
		return uc_vm_string_concat(vm, value, operand);

	nv1 = ucv_to_number(value);
	nv2 = ucv_to_number(operand);

	/* any operation involving NaN results in NaN */
	if (!nv1 || !nv2) {
		ucv_put(nv1);
		ucv_put(nv2);

		return ucv_double_new(NAN);
	}
	if (ucv_type(nv1) == UC_DOUBLE || ucv_type(nv2) == UC_DOUBLE) {
		d1 = ucv_double_get(nv1);
		d2 = ucv_double_get(nv2);

		switch (operation) {
		case I_ADD:
		case I_PLUS:
			rv = ucv_double_new(d1 + d2);
			break;

		case I_SUB:
		case I_MINUS:
			rv = ucv_double_new(d1 - d2);
			break;

		case I_MUL:
			rv = ucv_double_new(d1 * d2);
			break;

		case I_DIV:
			if (d2 == 0.0)
				rv = ucv_double_new(INFINITY);
			else if (isnan(d2))
				rv = ucv_double_new(NAN);
			else if (!isfinite(d2))
				rv = ucv_double_new(isfinite(d1) ? 0.0 : NAN);
			else
				rv = ucv_double_new(d1 / d2);

			break;

		case I_MOD:
			rv = ucv_double_new(fmod(d1, d2));
			break;

		case I_EXP:
			rv = ucv_double_new(pow(d1, d2));
			break;

		default:
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
			                      "undefined arithmetic operation %d",
			                      operation);
			break;
		}
	}
	else {
		n1 = int64(nv1, &u1);
		n2 = int64(nv2, &u2);

		switch (operation) {
		case I_ADD:
		case I_PLUS:
			if (n1 < 0 || n2 < 0) {
				if (u1)
					rv = ucv_uint64_new(u1 - abs64(n2));
				else if (u2)
					rv = ucv_uint64_new(u2 - abs64(n1));
				else
					rv = ucv_int64_new(n1 + n2);
			}
			else {
				if (!u1) u1 = (uint64_t)n1;
				if (!u2) u2 = (uint64_t)n2;

				rv = ucv_uint64_new(u1 + u2);
			}

			break;

		case I_SUB:
		case I_MINUS:
			if (n1 < 0 && n2 < 0) {
				if (n1 > n2)
					rv = ucv_uint64_new(abs64(n2) - abs64(n1));
				else
					rv = ucv_int64_new(n1 - n2);
			}
			else if (n1 >= 0 && n2 >= 0) {
				if (!u1) u1 = (uint64_t)n1;
				if (!u2) u2 = (uint64_t)n2;

				if (u2 > u1)
					rv = ucv_int64_new(-(u2 - u1));
				else
					rv = ucv_uint64_new(u1 - u2);
			}
			else if (n1 >= 0) {
				if (!u1) u1 = (uint64_t)n1;

				rv = ucv_uint64_new(u1 + abs64(n2));
			}
			else {
				rv = ucv_int64_new(n1 - n2);
			}

			break;

		case I_MUL:
			if (n1 < 0 && n2 < 0) {
				rv = ucv_uint64_new(abs64(n1) * abs64(n2));
			}
			else if (n1 >= 0 && n2 >= 0) {
				if (!u1) u1 = (uint64_t)n1;
				if (!u2) u2 = (uint64_t)n2;

				rv = ucv_uint64_new(u1 * u2);
			}
			else {
				rv = ucv_int64_new(n1 * n2);
			}

			break;

		case I_DIV:
			if (n2 == 0) {
				rv = ucv_double_new(INFINITY);
			}
			else if (n1 == INT64_MIN && n2 == -1) {
				/* the mathematical result 2^63 only fits into uint64_t;
				 * the signed division would trap with SIGFPE */
				rv = ucv_uint64_new((uint64_t)INT64_MAX + 1);
			}
			else if (n1 < 0 || n2 < 0) {
				rv = ucv_int64_new(n1 / n2);
			}
			else {
				if (!u1) u1 = (uint64_t)n1;
				if (!u2) u2 = (uint64_t)n2;

				rv = ucv_uint64_new(u1 / u2);
			}

			break;

		case I_MOD:
			if (n2 == 0) {
				rv = ucv_double_new(NAN);
			}
			else if (n1 == INT64_MIN && n2 == -1) {
				/* the signed modulo would trap with SIGFPE */
				rv = ucv_int64_new(0);
			}
			else if (n1 < 0 || n2 < 0) {
				rv = ucv_int64_new(n1 % n2);
			}
			else {
				if (!u1) u1 = (uint64_t)n1;
				if (!u2) u2 = (uint64_t)n2;

				rv = ucv_uint64_new(u1 % u2);
			}

			break;

		case I_EXP:
			if (n1 < 0 || n2 < 0) {
				/* the result is negative iff the base is negative and the
				 * exponent is odd */
				if (n2 < 0)
					rv = ucv_double_new(((n1 < 0 && (n2 & 1)) ? -1.0 : 1.0) /
						(double)upow64(abs64(n1), abs64(n2)));
				else if (n2 & 1)
					rv = ucv_int64_new((int64_t)-upow64(abs64(n1), abs64(n2)));
				else
					rv = ucv_uint64_new(upow64(abs64(n1), abs64(n2)));
			}
			else {
				if (!u1) u1 = (uint64_t)n1;
				if (!u2) u2 = (uint64_t)n2;

				rv = ucv_uint64_new(upow64(u1, u2));
			}

			break;

		default:
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
			                      "undefined arithmetic operation %d",
			                      operation);
			break;
		}
	}

	ucv_put(nv1);
	ucv_put(nv2);

	return rv;
}

static void
uc_vm_insn_update_var(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *name, *val = NULL, *inc = uc_vm_stack_pop(vm);
	uc_value_t *scope, *next;
	bool found;

	scope = vm->globals;
	name = uc_program_get_constant(uc_vm_current_program(vm), vm->arg.u32 & 0x00FFFFFF);

	assert(ucv_type(name) == UC_STRING);

	while (true) {
		val = ucv_object_get(scope, ucv_string_get(name), &found);

		if (found)
			break;

		next = ucv_prototype_get(scope);

		if (!next) {
			if (uc_vm_is_strict(vm)) {
				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "access to undeclared variable %s",
				                      ucv_string_get(name));
				ucv_put(name);
				ucv_put(inc);

				return;
			}

			break;
		}

		scope = next;
	}

	val = uc_vm_value_arith(vm, vm->arg.u32 >> 24, val, inc);

	ucv_object_add(scope, ucv_string_get(name), ucv_get(val));
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
	uc_value_t *val = NULL;
	uc_vm_insn_t op;

	switch (ucv_type(v)) {
	case UC_OBJECT:
	case UC_ARRAY:
	case UC_RESOURCE:
		if (assert_mutable_value(vm, v)) {
			uc_value_t *nv, *rv;

			/* reading the property may dispatch a __get__ metamethod which runs
			 * on the current stack, clobbers vm->arg and may collect garbage, so
			 * remember the operation and keep the operands reachable */
			op = vm->arg.u8;
			uc_vm_stack_push(vm, ucv_get(inc));

			val = ucv_key_get(vm, v, k);
			uc_vm_stack_push(vm, ucv_get(val));

			nv = uc_vm_value_arith(vm, op, val, inc);

			ucv_put(uc_vm_stack_pop(vm));
			ucv_put(uc_vm_stack_pop(vm));

			/* an aborted read leaves nothing to store; the exception it raised is
			 * left to propagate instead of overwriting the property */
			rv = vm->exception.type != EXCEPTION_NONE ? NULL : ucv_key_set(vm, v, k, nv);

			/* the store keeps a reference of its own; rv is the value of the
			 * update expression, null if storing failed */
			ucv_put(nv);

			uc_vm_stack_push(vm, rv);
		}

		break;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
		                      "left-hand side expression is %s",
		                      v ? "not an array or object" : "null");

		break;
	}

	ucv_put(val);
	ucv_put(inc);
	ucv_put(v);
	ucv_put(k);
}

static void
uc_vm_insn_update_upval(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	size_t slot = vm->arg.u32 & 0x00FFFFFF;
	uc_upvalref_t *ref = frame->closure->upvals[slot];
	uc_value_t *inc = uc_vm_stack_pop(vm);
	uc_value_t *val;

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
	uc_value_t *inc = uc_vm_stack_pop(vm);
	uc_value_t *val;

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

	for (idx = 0; idx < vm->arg.u32; idx++)
		ucv_array_push(arr, uc_vm_stack_peek(vm, vm->arg.u32 - idx - 1));

	for (idx = 0; idx < vm->arg.u32; idx++)
		uc_vm_stack_pop(vm);

	//uc_vm_shrink(state, vm->arg.u32);
}

static void
uc_vm_insn_marr(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *src = uc_vm_stack_pop(vm);
	uc_value_t *dst = uc_vm_stack_peek(vm, 0);
	size_t i;
	char *s;

	if (ucv_type(src) != UC_ARRAY) {
		s = ucv_to_string(vm, src);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "(%s) is not iterable", s);
		ucv_put(src);
		free(s);

		return;
	}

	for (i = 0; i < ucv_array_length(src); i++)
		ucv_array_push(dst, ucv_get(ucv_array_get(src, i)));

	ucv_put(src);
}

static void
uc_vm_insn_nobj(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *obj = ucv_object_new(vm);

	uc_vm_stack_push(vm, obj);
}

static void
uc_vm_insn_sobj(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *obj = uc_vm_stack_peek(vm, vm->arg.u32);
	size_t idx;

	/* object literals create own keys, a __set__ metamethod of the object under
	 * construction must not intercept the keys it is built from */
	for (idx = 0; idx < vm->arg.u32; idx += 2)
		ucv_put(ucv_key_rawset(vm, obj,
			uc_vm_stack_peek(vm, vm->arg.u32 - idx - 1),
			uc_vm_stack_peek(vm, vm->arg.u32 - idx - 2)));

	for (idx = 0; idx < vm->arg.u32; idx++)
		ucv_put(uc_vm_stack_pop(vm));
}

static void
uc_vm_insn_mobj(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *src = uc_vm_stack_pop(vm);
	uc_value_t *dst = uc_vm_stack_peek(vm, 0);
	size_t i;
	char *s;

	switch (ucv_type(src)) {
	case UC_OBJECT:
		; /* a label can only be part of a statement and a declaration is not a statement */
		ucv_object_foreach(src, k, v)
			ucv_object_add(dst, k, ucv_get(v));

		ucv_put(src);
		break;

	case UC_ARRAY:
		for (i = 0; i < ucv_array_length(src); i++) {
			xasprintf(&s, "%zu", i);
			ucv_object_add(dst, s, ucv_get(ucv_array_get(src, i)));
			free(s);
		}

		ucv_put(src);
		break;

	default:
		s = ucv_to_string(vm, src);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Value (%s) is not iterable", s);
		free(s);

		break;
	}
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
	uc_value_t *v = uc_vm_stack_pop(vm), *nv;
	bool is_sub = (insn == I_MINUS);
	int64_t n;
	double d;

	if (ucv_type(v) == UC_STRING) {
		nv = uc_number_parse(ucv_string_get(v), NULL);

		if (nv) {
			ucv_put(v);
			v = nv;
		}
	}

	switch (ucv_type(v)) {
	case UC_INTEGER:
		n = ucv_int64_get(v);

		/* numeric value is in range 9223372036854775808..18446744073709551615 */
		if (errno == ERANGE) {
			if (is_sub)
				/* make negation of large numeric value result in smallest negative value */
				uc_vm_stack_push(vm, ucv_int64_new(INT64_MIN));
			else
				/* for positive number coercion return value as-is */
				uc_vm_stack_push(vm, ucv_get(v));
		}

		/* numeric value is in range -9223372036854775808..9223372036854775807 */
		else {
			if (is_sub) {
				if (n == INT64_MIN)
					/* make negation of minimum value result in maximum signed positive value */
					uc_vm_stack_push(vm, ucv_int64_new(INT64_MAX));
				else
					/* for all other values flip the sign */
					uc_vm_stack_push(vm, ucv_int64_new(-n));
			}
			else {
				/* for positive number coercion return value as-is */
				uc_vm_stack_push(vm, ucv_get(v));
			}
		}

		break;

	case UC_DOUBLE:
		d = ucv_double_get(v);
		uc_vm_stack_push(vm, ucv_double_new(is_sub ? -d : d));
		break;

	case UC_BOOLEAN:
		n = (int64_t)ucv_boolean_get(v);
		uc_vm_stack_push(vm, ucv_int64_new(is_sub ? -n : n));
		break;

	case UC_NULL:
		uc_vm_stack_push(vm, ucv_int64_new(0));
		break;

	default:
		uc_vm_stack_push(vm, ucv_double_new(NAN));
	}

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
	uc_value_t *nv;
	uint64_t u;
	int64_t n;

	nv = ucv_to_number(v);
	n = int64(nv, &u);

	if (n < 0) {
		uc_vm_stack_push(vm, ucv_int64_new(~n));
	}
	else {
		if (!u) u = (uint64_t)n;

		uc_vm_stack_push(vm, ucv_uint64_new(~u));
	}

	ucv_put(nv);
	ucv_put(v);
}

static void
uc_vm_insn_rel(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);

	bool res = ucv_compare(insn, r1, r2, NULL);

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, ucv_boolean_new(res));
}

/* Test whether `key` names a property of `scope` or of anything in its
 * prototype chain, the way a property read resolves it - but without
 * dispatching __get__, so a purely virtual property stays hidden. */
static bool
uc_vm_key_exists(uc_value_t *scope, uc_value_t *key)
{
	uc_value_t *o;
	bool found = false;

	if (ucv_type(key) != UC_STRING)
		return false;

	for (o = scope; o != NULL && !found; o = ucv_prototype_get(o))
		ucv_object_get(o, ucv_string_get(key), &found);

	return found;
}

static void
uc_vm_insn_in(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);
	uc_value_t *item;
	size_t arrlen, arridx;
	bool found = false;

	switch (ucv_type(r2)) {
	case UC_ARRAY:
		/* value membership: is r1 one of the array elements? */
		for (arridx = 0, arrlen = ucv_array_length(r2);
		     arridx < arrlen; arridx++) {
			item = ucv_array_get(r2, arridx);

			if (uc_vm_test_strict_equality(r1, item, true)) {
				found = true;
				break;
			}
		}

		/* arrays can carry a prototype providing the tested key as well */
		if (!found)
			found = uc_vm_key_exists(r2, r1);

		break;

	case UC_OBJECT:
		found = uc_vm_key_exists(r2, r1);

		break;

	default:
		found = false;
	}

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, ucv_boolean_new(found));
}

static void
uc_vm_insn_equality(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r2 = uc_vm_stack_pop(vm);
	uc_value_t *r1 = uc_vm_stack_pop(vm);
	bool equal = uc_vm_test_strict_equality(r1, r2, false);

	ucv_put(r1);
	ucv_put(r2);

	uc_vm_stack_push(vm, ucv_boolean_new((insn == I_EQS) ? equal : !equal));
}

static void
uc_vm_insn_not(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *r1 = uc_vm_stack_pop(vm);

	uc_vm_stack_push(vm, ucv_boolean_new(!ucv_is_truish(r1)));
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
uc_vm_object_iterator_free(void *ud)
{
	uc_object_iterator_t *iter = ud;

	uc_list_remove(&iter->list);
}

static uc_resource_type_t uc_vm_object_iterator_type = {
	.name = "object iterator",
	.free = uc_vm_object_iterator_free
};

static bool
uc_vm_object_iterator_next(uc_vm_t *vm, uc_vm_insn_t insn,
                           uc_value_t *k, uc_value_t *v)
{
	uc_resource_t *res = ucv_as_resource(k);
	uc_object_t *obj = ucv_as_object(v);
	uc_object_iterator_t *iter;

	if (!res) {
		/* object is empty */
		if (!obj->table->head)
			return false;

		res = xalloc(sizeof(*res) + sizeof(uc_object_iterator_t));
		res->header.type = UC_RESOURCE;
		res->header.refcount = 1;
		res->type = &uc_vm_object_iterator_type;

		iter = res->data = (char *)res + sizeof(*res);
		iter->table = obj->table;
		iter->u.pos = obj->table->head;

		uc_list_insert(&uc_thread_context_get()->object_iterators, &iter->list);
	}
	else if (ucv_type(k) == UC_RESOURCE &&
	         res->type == &uc_vm_object_iterator_type && res->data != NULL) {

		iter = res->data;
	}
	else {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid object iterator");

		return false;
	}

	/* no next key */
	if (!iter->u.pos) {
		uc_list_remove(&iter->list);

		return false;
	}

	uc_vm_stack_push(vm, ucv_string_new(iter->u.pos->k));

	if (insn == I_NEXTKV)
		uc_vm_stack_push(vm, ucv_get((uc_value_t *)iter->u.pos->v));

	uc_vm_stack_push(vm, &res->header);
	ucv_put(v);

	iter->u.pos = iter->u.pos->next;

	return true;
}

static bool
uc_vm_array_iterator_next(uc_vm_t *vm, uc_vm_insn_t insn,
                          uc_value_t *k, uc_value_t *v)
{
	uint64_t n;

	if (!k) {
		/* array is empty */
		if (!ucv_array_length(v))
			return false;

		k = ucv_resource_new(NULL, NULL);
		n = 0;
	}
	else if (ucv_type(k) == UC_RESOURCE) {
		n = (uintptr_t)ucv_resource_data(k, NULL);
	}
	else {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid array iterator");

		return false;
	}

	/* no next index */
	if (n >= ucv_array_length(v))
		return false;

	if (insn == I_NEXTKV)
		uc_vm_stack_push(vm, ucv_uint64_new(n));

	uc_vm_stack_push(vm, ucv_get(ucv_array_get(v, n)));

	uc_vm_stack_push(vm, k);
	ucv_put(v);

	ucv_as_resource(k)->data = (void *)(uintptr_t)(n + 1);

	return true;
}

static void
uc_vm_insn_next(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_pop(vm);

	switch (ucv_type(v)) {
	case UC_OBJECT:
		if (uc_vm_object_iterator_next(vm, insn, k, v))
			return;

		break;

	case UC_ARRAY:
		if (uc_vm_array_iterator_next(vm, insn, k, v))
			return;

		break;

	default:
		break;
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
	char *p;

	switch (ucv_type(v)) {
	case UC_OBJECT:
	case UC_ARRAY:
		p = ucv_to_jsonstring(vm, v);
		fwrite(p, 1, strlen(p), vm->output);
		free(p);
		break;

	case UC_STRING:
		fwrite(ucv_string_get(v), 1, ucv_string_length(v), vm->output);
		break;

	case UC_NULL:
		break;

	default:
		p = ucv_to_string(vm, v);
		fwrite(p, 1, strlen(p), vm->output);
		free(p);
	}

	ucv_put(v);
}

static void
uc_vm_insn_delete(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_value_t *k = uc_vm_stack_pop(vm);
	uc_value_t *v = uc_vm_stack_pop(vm);
	bool rv;

	switch (ucv_type(v)) {
	case UC_OBJECT:
		if (assert_mutable_value(vm, v)) {
			rv = ucv_key_delete(vm, v, k);
			uc_vm_stack_push(vm, ucv_boolean_new(rv));
		}

		break;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
		                      "left-hand side expression is %s",
		                      v ? "not an object" : "null");

		break;
	}

	ucv_put(k);
	ucv_put(v);
}

static void
uc_vm_insn_import(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_program_t *prog = uc_vm_current_program(vm);
	uint16_t from = vm->arg.u32 & 0xffff;
	uint16_t to = vm->arg.u32 >> 16;
	uc_value_t *name, *modobj;
	uint32_t cidx;

	/* is a wildcard import * from ... */
	if (to == 0xffff) {
		to = from;
		modobj = ucv_object_new(vm);

		/* instruction is followed by u16 containing the offset of the
		 * first module export and `from` times u32 values containing
		 * the constant indexes of the names */
		for (from = frame->ip[0] * 0x100 + frame->ip[1], frame->ip += 2;
		     from < prog->exports.count && to > 0;
		     from++, to--) {

			cidx = (
				frame->ip[0] * 0x1000000UL +
				frame->ip[1] * 0x10000UL +
				frame->ip[2] * 0x100UL +
				frame->ip[3]
			);

			frame->ip += 4;

			name = uc_program_get_constant(uc_vm_current_program(vm), cidx);

			if (ucv_type(name) == UC_STRING && prog->exports.entries[from])
				ucv_object_add(modobj, ucv_string_get(name),
					ucv_get(&prog->exports.entries[from]->header));

			ucv_put(name);
		}

		ucv_set_constant(modobj, true);

		uc_vm_stack_push(vm, modobj);
	}

	/* module export available, patch into upvalue */
	else if (from < prog->exports.count && prog->exports.entries[from]) {
		frame->closure->upvals[to] = prog->exports.entries[from];
		ucv_get(&prog->exports.entries[from]->header);
	}

	/* module export missing, e.g. due to premature return in module,
	 * patch up dummy upvalue ref with `null` value */
	else {
		frame->closure->upvals[to] = (uc_upvalref_t *)ucv_upvalref_new(0);
		frame->closure->upvals[to]->closed = true;
	}
}

static void
uc_vm_insn_export(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_program_t *prog = uc_vm_current_program(vm);
	uc_upvalref_t *ref = uc_vm_capture_upval(vm, frame->stackframe + vm->arg.u32);

	uc_vector_push(&prog->exports, ref);
	ucv_get(&ref->header);
}

static void
uc_vm_insn_dynload(uc_vm_t *vm, uc_vm_insn_t insn)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_value_t *name, *export, *modscope, *modobj;
	uint16_t count = vm->arg.u32 & 0xffff;
	uint16_t to = vm->arg.u32 >> 16;
	uint32_t cidx;
	bool found;

	/* Attempt to load module. Will raise exception on error */
	name = uc_vm_stack_pop(vm);
	modscope = uc_require_library(vm, name, true);
	ucv_put(name);

	if (!modscope)
		return;

	/* If count is zero, we're doing a wildcard import. Shallow copy module
	 * object, mark it constant and patch into the target upvalue. */
	if (count == 0) {
		modobj = ucv_object_new(vm);

		ucv_object_foreach(modscope, k, v)
			ucv_object_add(modobj, k, ucv_get(v));

		ucv_set_constant(modobj, true);

		uc_vm_stack_push(vm, modobj);
	}

	/* ... otherwise we're importing a specific list of names */
	else {
		/* Instruction is followed by `count` times u32 values containing
		 * the import name constant indexes */
		while (count > 0) {
			cidx = (
				frame->ip[0] * 0x1000000UL +
				frame->ip[1] * 0x10000UL +
				frame->ip[2] * 0x100UL +
				frame->ip[3]
			);

			frame->ip += 4;

			name = uc_program_get_constant(uc_vm_current_program(vm), cidx);
			export = ucv_object_get(modscope, ucv_string_get(name), &found);

			if (!found) {
				uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
				                      "Module does not export %s",
				                      ucv_string_get(name));

				ucv_put(name);
				ucv_put(modscope);

				return;
			}

			ucv_put(name);

			frame->closure->upvals[to] = (uc_upvalref_t *)ucv_upvalref_new(0);
			frame->closure->upvals[to]->closed = true;
			frame->closure->upvals[to]->value = ucv_get(export);

			count--;
			to++;
		}
	}

	ucv_put(modscope);
}

static void
uc_vm_gc_step(uc_vm_t *vm)
{
	size_t curr_count = 0, prev_count = 0;
	uc_weakref_t *ref;

	if (!(vm->gc_flags & UC_GC_ENABLED))
		return;

	if (vm->alloc_refs >= vm->gc_interval) {
		if (vm->trace) {
			for (ref = vm->values.next; ref != &vm->values; ref = ref->next)
				prev_count++;

			ucv_gc(vm);

			for (ref = vm->values.next; ref != &vm->values; ref = ref->next)
				curr_count++;

			fprintf(stderr, "! GC reclaimed %zu object(s)\n", prev_count - curr_count);
		}
		else {
			ucv_gc(vm);
		}
	}
}

static uc_value_t *
uc_vm_callframe_pop(uc_vm_t *vm)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_value_t *retval;

	/* close upvalues */
	uc_vm_close_upvals(vm, frame->stackframe);

	if (vm->stack.count > frame->stackframe)
		retval = uc_vm_stack_pop(vm);
	else
		retval = NULL;

	/* reset function stack frame */
	while (vm->stack.count > frame->stackframe)
		ucv_put(uc_vm_stack_pop(vm));

	/* for method calls, release context as well */
	if (frame->mcall)
		ucv_put(uc_vm_stack_pop(vm));

	/* release function */
	if (frame->closure)
		ucv_put(&frame->closure->header);

	if (frame->cfunction)
		ucv_put(&frame->cfunction->header);

	/* release context */
	ucv_put(frame->ctx);

	vm->callframes.count--;

	return retval;
}

static void
uc_vm_output_exception(uc_vm_t *vm, uc_exception_t *ex)
{
	uc_value_t *ctx;

	if (ex->type == EXCEPTION_USER)
		fprintf(stderr, "%s\n", ex->message);
	else
		fprintf(stderr, "%s: %s\n",
			    exception_type_strings[ex->type] ? exception_type_strings[ex->type] : "Error",
			    ex->message);

	ctx = ucv_object_get(ucv_array_get(ex->stacktrace, 0), "context", NULL);

	if (ctx)
		fprintf(stderr, "%s\n", ucv_string_get(ctx));

	fprintf(stderr, "\n");
}

uc_exception_type_t
uc_vm_signal_dispatch(uc_vm_t *vm)
{
	uc_exception_type_t ex;
	uc_value_t *handler;
	uint64_t mask;
	size_t i, j;
	int sig, rv;

	/* Check whether the signal self-pipe was actually set up, rather than
	 * re-checking config->setup_signal_handlers directly: the pipe may
	 * have been lazily initialized on demand via
	 * uc_vm_signal_handlers_ensure() after the fact (see lib/debug.c),
	 * independent of what the original config requested. */
	if (vm->signal.sigpipe[0] < 0)
		return EXCEPTION_NONE;

	for (i = 0; i < ARRAY_SIZE(vm->signal.raised); i++) {
		if (!vm->signal.raised[i])
			continue;

		do {
			rv = read(vm->signal.sigpipe[0], &sig, sizeof(sig));
		} while (rv > 0 || (rv == -1 && errno == EINTR));

		for (j = 0; j < 64; j++) {
			mask = 1ull << j;

			if (vm->signal.raised[i] & mask) {
				vm->signal.raised[i] &= ~mask;

				sig = i * 64 + j;
				handler = ucv_array_get(vm->signal.handler, sig);

				if (ucv_is_callable(handler)) {
					uc_vm_stack_push(vm, ucv_get(handler));
					uc_vm_stack_push(vm, ucv_int64_new(sig));

					ex = uc_vm_call(vm, false, 1);

					if (ex != EXCEPTION_NONE)
						return ex;

					ucv_put(uc_vm_stack_pop(vm));
				}
			}
		}
	}

	return EXCEPTION_NONE;
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
				uc_vm_reset_callframes(vm);

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
							uc_vm_reset_callframes(vm);

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
	uc_closure_t *closure = ucv_as_closure(uc_program_main(vm, program));
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

	if (vm->trace) {
		buf = xprintbuf_new();

		uc_source_context_format(buf, uc_vm_frame_source(frame), 0, true);

		fwrite(buf->buf, 1, printbuf_length(buf), stderr);
		printbuf_free(buf);

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

uc_exception_type_t
uc_vm_call(uc_vm_t *vm, bool mcall, size_t nargs)
{
	uc_value_t *ctx = mcall ? ucv_get(uc_vm_stack_peek(vm, nargs + 1)) : NULL;
	uc_value_t *fno = ucv_get(uc_vm_stack_peek(vm, nargs));

	uc_vm_clear_exception(vm);

	if (uc_vm_call_function(vm, ctx, fno, mcall, nargs & 0xffff, false)) {
		if (ucv_type(fno) != UC_CFUNCTION)
			uc_vm_execute_chunk(vm);
	}

	return vm->exception.type;
}

uc_value_t *
uc_vm_scope_get(uc_vm_t *vm)
{
	return vm->globals;
}

void
uc_vm_scope_set(uc_vm_t *vm, uc_value_t *ctx)
{
	ucv_put(vm->globals);
	vm->globals = ctx;
}

uc_value_t *
uc_vm_invoke(uc_vm_t *vm, const char *fname, size_t nargs, ...)
{
	uc_exception_type_t ex;
	uc_value_t *fno, *arg;
	va_list ap;
	size_t i;

	fno = ucv_property_get(vm->globals, fname);

	if (!ucv_is_callable(fno))
		return NULL;

	uc_vm_stack_push(vm, ucv_get(fno));

	va_start(ap, nargs);

	for (i = 0; i < nargs; i++) {
		arg = va_arg(ap, uc_value_t *);
		uc_vm_stack_push(vm, ucv_get(arg));
	}

	va_end(ap);

	ex = uc_vm_call(vm, false, nargs);

	if (ex) {
		if (vm->exhandler)
			vm->exhandler(vm, &vm->exception);

		return NULL;
	}

	return uc_vm_stack_pop(vm);
}

uc_exception_handler_t *
uc_vm_exception_handler_get(uc_vm_t *vm)
{
	return vm->exhandler;
}

void
uc_vm_exception_handler_set(uc_vm_t *vm, uc_exception_handler_t *exhandler)
{
	vm->exhandler = exhandler;
}

uint32_t
uc_vm_trace_get(uc_vm_t *vm)
{
	return vm->trace;
}

void
uc_vm_trace_set(uc_vm_t *vm, uint32_t level)
{
	vm->trace = level;
}

bool
uc_vm_registry_exists(uc_vm_t *vm, const char *key)
{
	bool exists;

	ucv_object_get(vm->registry, key, &exists);

	return exists;
}

uc_value_t *
uc_vm_registry_get(uc_vm_t *vm, const char *key)
{
	return ucv_object_get(vm->registry, key, NULL);
}

void
uc_vm_registry_set(uc_vm_t *vm, const char *key, uc_value_t *value)
{
	if (!vm->registry)
		vm->registry = ucv_object_new(vm);

	ucv_object_add(vm->registry, key, value);
}

bool
uc_vm_registry_delete(uc_vm_t *vm, const char *key)
{
	return ucv_object_delete(vm->registry, key);
}

bool
uc_vm_gc_start(uc_vm_t *vm, uint16_t interval)
{
	bool changed = false;

	if (vm->gc_interval != interval) {
		vm->gc_interval = interval;
		changed = true;
	}

	if (!(vm->gc_flags & UC_GC_ENABLED)) {
		vm->gc_flags |= UC_GC_ENABLED;
		changed = true;
	}

	return changed;
}

bool
uc_vm_gc_stop(uc_vm_t *vm)
{
	if (!(vm->gc_flags & UC_GC_ENABLED))
		return false;

	vm->gc_flags &= ~UC_GC_ENABLED;

	return true;
}

void
uc_vm_signal_raise(uc_vm_t *vm, int signo)
{
	uint8_t signum = signo;

	if (signo <= 0 || signo >= UC_SYSTEM_SIGNAL_COUNT)
		return;

	vm->signal.raised[signo / 64] |= (1ull << (signo % 64));

	if (write(vm->signal.sigpipe[1], &signum, sizeof(signum)) == -1) {}
}

int
uc_vm_signal_notifyfd(uc_vm_t *vm)
{
	return vm->signal.sigpipe[0];
}

bool
uc_vm_break_requested(uc_vm_t *vm)
{
	return vm->break_requested;
}

void
uc_vm_break_request(uc_vm_t *vm)
{
	vm->break_requested = true;

	if (vm->break_notifyfd[1] >= 0) {
		char c = 'B';
		if (write(vm->break_notifyfd[1], &c, 1) == -1) {}
	}
}

int
uc_vm_break_notifyfd(uc_vm_t *vm)
{
	return vm->break_notifyfd[0];
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

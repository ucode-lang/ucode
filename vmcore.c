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
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>

#include "ucode/internal/vmcore.h"
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
	[I_DYNLOAD] = 4,

	[I_STCALL] = 4
};

const char *exception_type_strings[] = {
	[EXCEPTION_SYNTAX] = "Syntax error",
	[EXCEPTION_RUNTIME] = "Runtime error",
	[EXCEPTION_TYPE] = "Type error",
	[EXCEPTION_REFERENCE] = "Reference error",
	[EXCEPTION_USER] = "Error",
	[EXCEPTION_EXIT] = "Exit"
};

const char *
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

void
uc_vm_reset_callframes(uc_vm_t *vm, size_t depth)
{
	while (vm->callframes.count > depth)
		ucv_put(uc_vm_callframe_pop(vm));
}

static void
uc_vm_output_exception(uc_vm_t *vm, uc_exception_t *ex);

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

	uc_vm_reset_callframes(vm, 0);
	uc_vm_reset_stack(vm);
	uc_vector_clear(&vm->stack);
	uc_vector_clear(&vm->callframes);

	uc_stringbuf_free(vm->strbuf);

	ucv_freeall(vm);

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

char *
uc_vm_format_val(uc_vm_t *vm, uc_value_t *val)
{
	if (!vm->strbuf)
		vm->strbuf = uc_stringbuf_new();
	else
		uc_stringbuf_reset(vm->strbuf);

	ucv_to_stringbuf(NULL, vm->strbuf, val, true);

	if (uc_stringbuf_length(vm->strbuf) >= 64) {
		uc_stringbuf_memset(vm->strbuf, 60, '.', 3);
		uc_stringbuf_memset(vm->strbuf, 63, 0, 1);
	}

	return vm->strbuf->buf;
}

void
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

uc_value_t *
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
	if (offset >= vm->stack.count)
		return NULL;

	return vm->stack.entries[vm->stack.count + (-1 - offset)];
}

void
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

void
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

/* Returns the expression invoked by the call that the instruction the VM is
 * about to execute belongs to, or NULL if it cannot be determined. The result
 * is meant for error messages describing a failed call, and must be
 * released using free(). */
char *
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

void
uc_vm_clear_exception(uc_vm_t *vm)
{
	vm->exception.type = EXCEPTION_NONE;

	ucv_put(vm->exception.stacktrace);
	vm->exception.stacktrace = NULL;

	free(vm->exception.message);
	vm->exception.message = NULL;
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

			/* without debug information the position is that of the function
			 * only; record the function and instruction offset, which the debug
			 * information of a separate copy of the program can resolve */
			if (!function->chunk.debuginfo.offsets.count) {
				ucv_object_add(entry, "path", ucv_string_new(source->runpath));
				ucv_object_add(entry, "id",
					ucv_uint64_new(uc_program_function_id(function->program, function)));
				ucv_object_add(entry, "pc", ucv_uint64_new(off));
			}
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

/* Describes the frames of a stack trace of code without debug information by
 * their function and instruction offset, in the form that the debug
 * information of the program can resolve later, see ucode-trace. */
static void
uc_vm_stacktrace_format_positions(uc_stringbuf_t *buf, uc_value_t *stacktrace)
{
	uc_value_t *entry, *path, *file, *name, *tco;
	const char *prefix, *fn;

	for (size_t i = 0; i < ucv_array_length(stacktrace); i++) {
		entry = ucv_array_get(stacktrace, i);
		path = ucv_object_get(entry, "path", NULL);
		file = ucv_object_get(entry, "filename", NULL);
		name = ucv_object_get(entry, "function", NULL);
		tco = ucv_object_get(entry, "tco", NULL);
		prefix = i ? "  called from " : "In ";
		fn = name ? ucv_string_get(name) : "[anonymous function]";

		/* code with debug information, e.g. a closure of another program, is
		 * described by its source position */
		if (path)
			ucv_stringbuf_printf(buf, "%sfunction #%" PRIu64 " at offset %" PRIu64 " of %s\n",
				prefix,
				ucv_uint64_get(ucv_object_get(entry, "id", NULL)),
				ucv_uint64_get(ucv_object_get(entry, "pc", NULL)),
				ucv_string_get(path));
		else if (file)
			ucv_stringbuf_printf(buf, "%sfunction %s (%s:%" PRId64 ":%" PRId64 ")\n",
				prefix, fn, ucv_string_get(file),
				ucv_int64_get(ucv_object_get(entry, "line", NULL)),
				ucv_int64_get(ucv_object_get(entry, "byte", NULL)));
		else
			ucv_stringbuf_printf(buf, "%sfunction %s [C]\n", prefix, fn);

		if (tco)
			ucv_stringbuf_printf(buf, "  (%" PRId64 " tail call frames omitted)\n",
				ucv_int64_get(tco));
	}
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

	if (!chunk->debuginfo.offsets.count && frame->ip != chunk->entries) {
		uc_vm_stacktrace_format_positions(buf, stacktrace);
	}
	else if (line) {
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

void
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

void
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

uc_value_t *
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
	if (frame->closure) {
		uc_vm_run_end(frame->closure);
		ucv_put(&frame->closure->header);
	}

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

uc_value_t *
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

uc_value_t *
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
				rv = ucv_double_new(d1 / d2);
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
				if (n1 == 0 && !u1)
					rv = ucv_double_new(NAN);
				else
					rv = ucv_double_new((n1 < 0) ? -INFINITY : INFINITY);
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
		ucv_object_lookup(o, ucv_string_get(key), ucv_string_length(key), &found);

	return found;
}

uc_value_t *
uc_vm_value_unary(uc_vm_insn_t operation, uc_value_t *value)
{
	bool is_sub = (operation == I_MINUS);
	uc_value_t *nv = NULL, *rv;
	int64_t n;
	double d;

	if (ucv_type(value) == UC_STRING) {
		nv = uc_number_parse(ucv_string_get(value), NULL);

		if (nv)
			value = nv;
	}

	switch (ucv_type(value)) {
	case UC_INTEGER:
		n = ucv_int64_get(value);

		/* numeric value is in range 9223372036854775808..18446744073709551615 */
		if (errno == ERANGE) {
			if (is_sub)
				/* make negation of large numeric value result in smallest negative value */
				rv = ucv_int64_new(INT64_MIN);
			else
				/* for positive number coercion return value as-is */
				rv = ucv_get(value);
		}

		/* numeric value is in range -9223372036854775808..9223372036854775807 */
		else {
			if (is_sub) {
				if (n == INT64_MIN)
					/* make negation of minimum value result in maximum signed positive value */
					rv = ucv_int64_new(INT64_MAX);
				else
					/* for all other values flip the sign */
					rv = ucv_int64_new(-n);
			}
			else {
				/* for positive number coercion return value as-is */
				rv = ucv_get(value);
			}
		}

		break;

	case UC_DOUBLE:
		d = ucv_double_get(value);
		rv = ucv_double_new(is_sub ? -d : d);
		break;

	case UC_BOOLEAN:
		n = (int64_t)ucv_boolean_get(value);
		rv = ucv_int64_new(is_sub ? -n : n);
		break;

	case UC_NULL:
		rv = ucv_int64_new(0);
		break;

	default:
		rv = ucv_double_new(NAN);
	}

	ucv_put(nv);

	return rv;
}

uc_value_t *
uc_vm_value_complement(uc_value_t *value)
{
	uc_value_t *nv, *rv;
	uint64_t u;
	int64_t n;

	nv = ucv_to_number(value);
	n = int64(nv, &u);

	if (n < 0) {
		rv = ucv_int64_new(~n);
	}
	else {
		if (!u) u = (uint64_t)n;

		rv = ucv_uint64_new(~u);
	}

	ucv_put(nv);

	return rv;
}

bool
uc_vm_value_compare(uc_vm_insn_t operation, uc_value_t *v1, uc_value_t *v2)
{
	return ucv_compare(operation, v1, v2, NULL);
}

bool
uc_vm_value_equality(uc_vm_insn_t operation, uc_value_t *v1, uc_value_t *v2)
{
	bool equal = uc_vm_test_strict_equality(v1, v2, false);

	return (operation == I_EQS) ? equal : !equal;
}

bool
uc_vm_value_in(uc_value_t *r1, uc_value_t *r2)
{
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

	return found;
}

bool
uc_vm_value_not(uc_value_t *value)
{
	return !ucv_is_truish(value);
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
uc_vm_object_iterator_next(uc_vm_t *vm, uc_vm_insn_t insn, uc_value_t *v,
                           uc_value_t **state, uc_value_t **key, uc_value_t **val)
{
	uc_value_t *k = *state;
	uc_resource_t *res = ucv_as_resource(k);
	uc_object_t *obj = ucv_as_object(v);
	uc_object_iterator_t *iter;
	uc_object_entry_t *e;

	if (!res) {
		/* object is empty */
		if (!obj->count)
			return false;

		res = xalloc(sizeof(*res) + sizeof(uc_object_iterator_t));
		res->header.type = UC_RESOURCE;
		res->header.refcount = 1;
		res->type = &uc_vm_object_iterator_type;

		iter = res->data = (char *)res + sizeof(*res);
		iter->object = obj;
		iter->pos = 0;

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

	while (iter->pos < obj->used && !obj->entries[iter->pos].key)
		iter->pos++;

	/* no next key */
	if (iter->pos >= obj->used) {
		uc_list_remove(&iter->list);

		return false;
	}

	e = &obj->entries[iter->pos++];
	*key = ucv_get(e->key);

	if (insn == I_NEXTKV)
		*val = ucv_get(e->value);

	*state = &res->header;

	return true;
}

static bool
uc_vm_array_iterator_next(uc_vm_t *vm, uc_vm_insn_t insn, uc_value_t *v,
                          uc_value_t **state, uc_value_t **key, uc_value_t **val)
{
	uc_value_t *k = *state;
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

	if (insn == I_NEXTKV) {
		*key = ucv_uint64_new(n);
		*val = ucv_get(ucv_array_get(v, n));
	}
	else {
		*key = ucv_get(ucv_array_get(v, n));
	}

	*state = k;

	ucv_as_resource(k)->data = (void *)(uintptr_t)(n + 1);

	return true;
}

bool
uc_vm_iterator_next(uc_vm_t *vm, uc_vm_insn_t insn, uc_value_t *v,
                    uc_value_t **state, uc_value_t **key, uc_value_t **val)
{
	switch (ucv_type(v)) {
	case UC_OBJECT:
		return uc_vm_object_iterator_next(vm, insn, v, state, key, val);

	case UC_ARRAY:
		return uc_vm_array_iterator_next(vm, insn, v, state, key, val);

	default:
		return false;
	}
}

static bool
assert_mutable_value(uc_vm_t *vm, uc_value_t *val)
{
	if (ucv_is_constant(val)) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
		                      "%s value is immutable",
		                      ucv_typename(val));

		return false;
	}

	return true;
}

bool
uc_vm_property_get(uc_vm_t *vm, uc_value_t *v, uc_value_t *k, uc_value_t **res)
{
	switch (ucv_type(v)) {
	case UC_RESOURCE:
	case UC_OBJECT:
	case UC_ARRAY:
		/* aware read: dispatches __get__ on a missing key */
		*res = ucv_key_get(vm, v, k);

		return true;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
		                      "left-hand side expression is %s",
		                      v ? "not an array or object" : "null");

		return false;
	}
}

bool
uc_vm_property_set(uc_vm_t *vm, uc_value_t *o, uc_value_t *k, uc_value_t *v, uc_value_t **res)
{
	switch (ucv_type(o)) {
	case UC_OBJECT:
	case UC_ARRAY:
	case UC_RESOURCE:
		*res = NULL;

		if (!assert_mutable_value(vm, o))
			return true;

		/* ucv_key_set() retains the stored value on its own and returns a
		 * reference to it, which becomes the value of the assignment; for
		 * resources it dispatches a __set__ metamethod when present. In
		 * strict mode it raises a type error when a non-index property
		 * store on an array would otherwise be silently dropped. */
		*res = ucv_key_set(vm, o, k, v);

		return true;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
		                      "attempt to set property on %s value",
		                      ucv_typename(o));

		return false;
	}
}

bool
uc_vm_property_update(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *v,
                      uc_value_t *k, uc_value_t *inc, uc_value_t **res)
{
	uc_value_t *val, *nv;

	switch (ucv_type(v)) {
	case UC_OBJECT:
	case UC_ARRAY:
	case UC_RESOURCE:
		*res = NULL;

		if (!assert_mutable_value(vm, v))
			return true;

		/* reading the property may dispatch a __get__ metamethod which runs
		 * on the current stack and may collect garbage, so keep the operands
		 * reachable */
		uc_vm_stack_push(vm, ucv_get(inc));

		val = ucv_key_get(vm, v, k);
		uc_vm_stack_push(vm, ucv_get(val));

		nv = uc_vm_value_arith(vm, operation, val, inc);

		ucv_put(uc_vm_stack_pop(vm));
		ucv_put(uc_vm_stack_pop(vm));

		/* an aborted read leaves nothing to store; the exception it raised is
		 * left to propagate instead of overwriting the property */
		*res = vm->exception.type != EXCEPTION_NONE ? NULL : ucv_key_set(vm, v, k, nv);

		/* the store keeps a reference of its own; the result is the value of
		 * the update expression, null if storing failed */
		ucv_put(nv);
		ucv_put(val);

		return true;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
		                      "left-hand side expression is %s",
		                      v ? "not an array or object" : "null");

		return false;
	}
}

bool
uc_vm_property_delete(uc_vm_t *vm, uc_value_t *v, uc_value_t *k, uc_value_t **res)
{
	switch (ucv_type(v)) {
	case UC_OBJECT:
	case UC_ARRAY:
	case UC_RESOURCE:
		*res = NULL;

		if (!assert_mutable_value(vm, v))
			return true;

		/* ucv_key_delete() removes own object keys directly and consults the
		 * __delete__ metamethod for keys the value has no own storage for.
		 * Arrays and resources have no own-key storage at all, so there the
		 * metamethod is the only way to handle a key; without one it raises
		 * the same reference error the default case below reports */
		*res = ucv_boolean_new(ucv_key_delete(vm, v, k));

		return true;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
		                      "left-hand side expression is %s",
		                      v ? "not an object" : "null");

		return false;
	}
}

void
uc_vm_array_append(uc_value_t *arr, uc_value_t **values, size_t count)
{
	size_t idx;

	for (idx = 0; idx < count; idx++)
		ucv_array_push(arr, values[idx]);
}

bool
uc_vm_array_spread(uc_vm_t *vm, uc_value_t *dst, uc_value_t *src)
{
	size_t i;
	char *s;

	if (ucv_type(src) != UC_ARRAY) {
		s = ucv_to_string(vm, src);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "(%s) is not iterable", s);
		ucv_put(src);
		free(s);

		return false;
	}

	for (i = 0; i < ucv_array_length(src); i++)
		ucv_array_push(dst, ucv_get(ucv_array_get(src, i)));

	ucv_put(src);

	return true;
}

uc_value_t *
uc_vm_object_new(uc_vm_t *vm, size_t nkeys)
{
	return ucv_object_new_size(vm, nkeys);
}

void
uc_vm_object_init_key(uc_vm_t *vm, uc_value_t *obj, uc_value_t *k, uc_value_t *v)
{
	/* object literals create own keys, a __set__ metamethod of the object under
	 * construction must not intercept the keys it is built from */
	ucv_put(ucv_key_rawset(vm, obj, k, v));
}

bool
uc_vm_object_spread(uc_vm_t *vm, uc_value_t *dst, uc_value_t *src)
{
	size_t i;
	char *s;

	switch (ucv_type(src)) {
	case UC_OBJECT:
		; /* a label can only be part of a statement and a declaration is not a statement */
		ucv_object_foreach_entry(src, e)
			ucv_object_insert_string(dst, e->key, ucv_get(e->value));

		ucv_put(src);

		return true;

	case UC_ARRAY:
		for (i = 0; i < ucv_array_length(src); i++) {
			xasprintf(&s, "%zu", i);
			ucv_object_add(dst, s, ucv_get(ucv_array_get(src, i)));
			free(s);
		}

		ucv_put(src);

		return true;

	default:
		s = ucv_to_string(vm, src);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Value (%s) is not iterable", s);
		free(s);

		return false;
	}
}

/* The `nargs` arguments of a call stay at `base` of the stack while their
 * expansion is pushed above them, so that the GC sees them when a tostring
 * method runs. `from` and `spread` count arguments from the last one, as the
 * bytecode encodes the spread positions. */
static uc_value_t *
uc_vm_spread_arg(uc_vm_t *vm, size_t base, size_t nargs, size_t idx)
{
	return vm->stack.entries[base + nargs - 1 - idx];
}

bool
uc_vm_spread_push(uc_vm_t *vm, size_t base, size_t nargs, size_t from, size_t spread)
{
	uc_value_t *arg;
	size_t j;
	char *s;

	/* push each preceeding non-spread value to the stack */
	for (j = from; j > spread + 1UL; j--)
		uc_vm_stack_push(vm, ucv_get(uc_vm_spread_arg(vm, base, nargs, j - 1)));

	/* read spread value at index... */
	arg = uc_vm_spread_arg(vm, base, nargs, spread);

	/* ... ensure that it is an array type ... */
	if (ucv_type(arg) != UC_ARRAY) {
		s = ucv_to_string(vm, arg);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "(%s) is not iterable", s);
		free(s);

		return false;
	}

	/* ... and push each spread array value as argument to the stack */
	for (j = 0; j < ucv_array_length(arg); j++)
		uc_vm_stack_push(vm, ucv_get(ucv_array_get(arg, j)));

	return true;
}

/* Pushes the arguments after the last spread one, then replaces the original
 * arguments with the expanded ones */
void
uc_vm_spread_finish(uc_vm_t *vm, size_t base, size_t nargs, size_t from)
{
	size_t i, count;

	for (i = from; i > 0; i--)
		uc_vm_stack_push(vm, ucv_get(uc_vm_spread_arg(vm, base, nargs, i - 1)));

	for (i = 0; i < nargs; i++)
		ucv_put(vm->stack.entries[base + i]);

	count = vm->stack.count - base - nargs;
	memmove(&vm->stack.entries[base], &vm->stack.entries[base + nargs],
	        count * sizeof(vm->stack.entries[0]));
	vm->stack.count = base + count;
}

/* Finds the scope in the chain of the globals that has the string `name`
 * with the object key hash `hash`, or the last scope if none has it, which
 * fails in strict mode */
static bool
uc_vm_global_find(uc_vm_t *vm, uc_value_t *name, uint64_t hash, bool strict,
                  uc_value_t **scope, uc_value_t **val)
{
	uc_value_t *next;
	bool found;

	for (*scope = vm->globals; ; *scope = next) {
		*val = ucv_object_lookup_hashed(*scope, name, hash, &found);

		if (found)
			return true;

		next = ucv_prototype_get(*scope);

		if (!next)
			break;
	}

	if (!strict)
		return true;

	uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
	                      "access to undeclared variable %s",
	                      ucv_string_get(name));

	return false;
}

uc_value_t *
uc_vm_global_get(uc_vm_t *vm, uc_value_t *name, uint64_t hash, bool strict)
{
	uc_value_t *scope, *val;

	if (ucv_type(name) != UC_STRING ||
	    !uc_vm_global_find(vm, name, hash, strict, &scope, &val))
		return NULL;

	return ucv_get(val);
}

bool
uc_vm_global_set(uc_vm_t *vm, uc_value_t *name, uint64_t hash, uc_value_t *v, bool strict)
{
	uc_value_t *scope, *val;

	if (ucv_type(name) != UC_STRING)
		return true;

	if (!uc_vm_global_find(vm, name, hash, strict, &scope, &val))
		return false;

	if (scope)
		ucv_object_insert_string(scope, name, ucv_get(v));

	return true;
}

bool
uc_vm_global_update(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *name, uint64_t hash,
                    uc_value_t *inc, bool strict, uc_value_t **res)
{
	uc_value_t *scope, *val;

	assert(ucv_type(name) == UC_STRING);

	if (!uc_vm_global_find(vm, name, hash, strict, &scope, &val))
		return false;

	*res = uc_vm_value_arith(vm, operation, val, inc);

	ucv_object_insert_string(scope, name, ucv_get(*res));

	return true;
}

const char *
uc_vm_main_key(uc_program_t *program, uc_vm_main_key_t key)
{
	snprintf(key, sizeof(uc_vm_main_key_t), UC_PROGRAM_MAIN_KEY "%p", (void *)program);

	return key;
}

/* The caller of uc_vm_execute() holds the program only. Keep the closure that
 * runs in the registry once it has module instances, so that a later execution
 * in this VM runs the same closure and finds them. */
static void
uc_vm_main_keep(uc_vm_t *vm, uc_program_t *program, uc_program_run_t *run)
{
	uc_vm_main_key_t key;

	if (!run->keep)
		return;

	run->keep = false;
	uc_vm_registry_set(vm, uc_vm_main_key(program, key), ucv_get(&run->closure->header));
}

static uc_modexports_t *
uc_vm_run_exports(uc_program_t *program, uc_program_run_t *run, bool create)
{
	uc_export_instance_t *instance;

	if (!run)
		return NULL;

	instance = uc_program_instance_get(program, run->closure, create);

	return instance ? &instance->exports : NULL;
}

uc_upvalref_t *
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

/* a closure that runs again holds the binding of its previous run */
static void
uc_vm_import_bind(uc_closure_t *closure, size_t slot, uc_upvalref_t *ref)
{
	if (closure->upvals[slot])
		ucv_put(&closure->upvals[slot]->header);

	closure->upvals[slot] = ref;
}

bool
uc_vm_regexp_load(uc_vm_t *vm, uc_value_t *jstr, uc_value_t **re)
{
	bool icase = false, newline = false, global = false;
	char *str, *err = NULL;

	*re = NULL;

	if (ucv_type(jstr) != UC_STRING || ucv_string_length(jstr) < 2)
		return true;

	str = ucv_string_get(jstr);

	global  = (*str & (1 << 0));
	icase   = (*str & (1 << 1));
	newline = (*str & (1 << 2));

	*re = ucv_regexp_new(++str, icase, newline, global, &err);

	if (!*re)
		uc_vm_raise_exception(vm, EXCEPTION_SYNTAX, "%s", err);

	free(err);

	return (*re != NULL);
}

void
uc_vm_closure_upval_set(uc_vm_t *vm, uc_closure_t *closure, size_t idx, int32_t uv)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);

	if (uv < 0)
		closure->upvals[idx] = uc_vm_capture_upval(vm, frame->stackframe - (uv + 1));
	else
		closure->upvals[idx] = frame->closure->upvals[uv];

	ucv_get(&closure->upvals[idx]->header);
}

/* Invoke a standard library function without pushing a call frame.
 *
 * The arguments occupy the top `nargs` stack entries, which is exactly the
 * layout uc_fn_arg() expects, so the callee can be entered as an ordinary C
 * function; it restores the stack to the argument boundary itself when it
 * calls back into managed code.
 *
 * The callee is addressed by its slot in the order-frozen
 * uc_stdlib_functions[] table rather than by name, so a shadowing `slice`,
 * `type` or `die` in the runtime namespace cannot affect the call.
 *
 * Since no frame is involved, an exception raised by the callee reports the
 * position of this instruction in the calling chunk, not of a synthetic one. */
void
uc_vm_stcall(uc_vm_t *vm, size_t slot, size_t nargs)
{
	uc_cfn_ptr_t fptr = uc_stdlib_function_by_slot(slot);
	size_t keep = vm->stack.count - nargs;
	uc_value_t *res = NULL;

	if (!fptr) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
		                      "unknown standard library slot %zu", slot);
	}
	else {
		res = fptr(vm, nargs);
	}

	/* a callee reads the arguments with uc_fn_arg() but does not consume them -
	 * ordinary calls consume them through the callframe pop - so drop them here,
	 * along with anything the callee left above them */
	while (vm->stack.count > keep)
		ucv_put(uc_vm_stack_pop(vm));

	/* as in uc_vm_call_native(), a result is only kept when no exception fired */
	if (vm->exception.type)
		ucv_put(res);
	else
		uc_vm_stack_push(vm, res);
}

void
uc_vm_value_print(uc_vm_t *vm, uc_value_t *v)
{
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
}

void
uc_vm_import(uc_vm_t *vm, size_t from, size_t to)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_program_t *prog = uc_vm_current_program(vm);
	uc_modexports_t *exports = uc_vm_run_exports(prog, uc_vector_last(&prog->runs), false);
	size_t count = exports ? exports->count : 0;

	/* module export available, patch into upvalue */
	if (from < count && exports->entries[from]) {
		ucv_get(&exports->entries[from]->header);
		uc_vm_import_bind(frame->closure, to, exports->entries[from]);

		return;
	}

	/* module export missing, e.g. due to premature return in module,
	 * patch up dummy upvalue ref with `null` value */
	uc_vm_import_bind(frame->closure, to, (uc_upvalref_t *)ucv_upvalref_new(0));
	frame->closure->upvals[to]->closed = true;
}

uc_value_t *
uc_vm_import_namespace(uc_vm_t *vm, size_t from, const uint32_t *names, size_t nnames)
{
	uc_program_t *prog = uc_vm_current_program(vm);
	uc_program_run_t *run = uc_vector_last(&prog->runs);
	uc_export_instance_t *instance =
		run ? uc_program_instance_get(prog, run->closure, false) : NULL;
	uc_modexports_t *exports = instance ? &instance->exports : NULL;
	size_t count = exports ? exports->count : 0, first = from, i;
	uc_value_t *name, *modobj;
	bool complete = true;

	if (instance)
		uc_vector_foreach(&instance->namespaces, ns)
			if (ns->from == first && ns->count == nnames)
				return ucv_get(ns->object);

	modobj = ucv_object_new(vm);

	for (i = 0; i < nnames; i++, from++) {
		if (from >= count || !exports->entries[from]) {
			complete = false;
			continue;
		}

		name = uc_program_get_constant(prog, names[i]);

		if (ucv_type(name) == UC_STRING)
			ucv_object_add(modobj, ucv_string_get(name),
				ucv_get(&exports->entries[from]->header));

		ucv_put(name);
	}

	ucv_set_constant(modobj, true);

	/* The exports are references to the variables of the module, so all
	 * wildcard imports of the module can share one object. An export that is
	 * missing yet, e.g. in a module still in progress, can appear later. */
	if (instance && complete)
		uc_vector_push(&instance->namespaces, {
			.from = first, .count = nnames, .object = ucv_get(modobj)
		});

	return modobj;
}

uc_modexports_t *
uc_vm_export_target(uc_vm_t *vm)
{
	uc_program_t *prog = uc_vm_current_program(vm);
	uc_program_run_t *run = uc_vector_last(&prog->runs);
	uc_modexports_t *exports = uc_vm_run_exports(prog, run, true);

	if (!exports)
		return NULL;

	uc_vm_main_keep(vm, prog, run);

	/* A later run of the closure runs the module bodies again. Its imports
	 * resolve to the instances of the first run, which stay in place. */
	if (run->cursor++ < exports->count)
		return NULL;

	return exports;
}

void
uc_vm_export_add(uc_modexports_t *exports, uc_upvalref_t *ref)
{
	uc_vector_push(exports, ref);
	ucv_get(&ref->header);
}

uc_value_t *
uc_vm_dynload_module(uc_vm_t *vm, uc_value_t *name)
{
	/* Attempt to load module. Will raise exception on error */
	uc_value_t *modscope = uc_require_library(vm, name, true);

	if (!modscope || vm->exception.type != EXCEPTION_NONE) {
		ucv_put(modscope);

		return NULL;
	}

	return modscope;
}

/* All wildcard imports of a module scope share one copy. The registry entry
 * holds the scope beside the copy, so that the address in the key stays in
 * use while the entry exists. A change to the scope after the first import
 * does not show in the copy. */
uc_value_t *
uc_vm_dynload_namespace(uc_vm_t *vm, uc_value_t *modscope)
{
	uc_value_t *modobj, *entry;
	char key[sizeof("namespace.") + 2 * sizeof(void *) + 2];

	snprintf(key, sizeof(key), "namespace.%p", (void *)modscope);
	entry = uc_vm_registry_get(vm, key);

	if (entry)
		return ucv_get(ucv_array_get(entry, 1));

	modobj = ucv_object_new(vm);

	ucv_object_foreach_entry(modscope, e)
		ucv_object_insert_string(modobj, e->key, ucv_get(e->value));

	ucv_set_constant(modobj, true);

	entry = ucv_array_new_length(vm, 2);
	ucv_array_push(entry, ucv_get(modscope));
	ucv_array_push(entry, ucv_get(modobj));
	uc_vm_registry_set(vm, key, entry);

	return modobj;
}

bool
uc_vm_dynload_bind(uc_vm_t *vm, uc_value_t *modscope, uint32_t cidx, size_t to)
{
	uc_callframe_t *frame = uc_vm_current_frame(vm);
	uc_value_t *name, *export;
	bool found;

	name = uc_program_get_constant(uc_vm_current_program(vm), cidx);
	export = ucv_object_get(modscope, ucv_string_get(name), &found);

	if (!found) {
		uc_vm_raise_exception(vm, EXCEPTION_REFERENCE,
		                      "Module does not export %s",
		                      ucv_string_get(name));

		ucv_put(name);

		return false;
	}

	ucv_put(name);

	uc_vm_import_bind(frame->closure, to, (uc_upvalref_t *)ucv_upvalref_new(0));
	frame->closure->upvals[to]->closed = true;
	frame->closure->upvals[to]->value = ucv_get(export);

	return true;
}

static const uc_vm_interpreter_t *uc_vm_interpreter;
static uc_vm_native_call_t uc_vm_native_call;

void
uc_vm_interpreter_set(const uc_vm_interpreter_t *interpreter)
{
	uc_vm_interpreter = interpreter;
}

void
uc_vm_native_call_set(uc_vm_native_call_t call)
{
	uc_vm_native_call = call;
}

bool
uc_vm_call_depth_check(uc_vm_t *vm, bool tail)
{
	/* a tail invocation reuses the current frame, so it never grows the stack */
	if (tail || vm->callframes.count < 1000)
		return true;

	uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Too much recursion");

	return false;
}

bool
uc_vm_call_dispatch(uc_vm_t *vm, uc_value_t **ctx, uc_value_t **fno, bool mcall, size_t nargs)
{
	char *callee, *hint;
	size_t hlen;

	/* __call__ dispatch: if fno is not itself callable (not a closure or
	 * cfunction) but carries a __call__ metamethod somewhere in its prototype
	 * chain, invoke that method with the instance as `this`. The instance
	 * becomes the new ctx; the original ctx (if any) is discarded. The tail
	 * flag is preserved so `return foo()` still TCOs through the dispatch. */
	if (ucv_type(*fno) != UC_CFUNCTION && ucv_type(*fno) != UC_CLOSURE) {
		uc_value_t *meta = ucv_metamethod_lookup(*fno, "__call__");

		if (ucv_is_callable(meta)) {
			ucv_put(*ctx);

			/* the instance becomes `this` and is invoked in place of the
			 * __call__ method, so hand the reference to ctx over */
			*ctx = *fno;
			*fno = ucv_get(meta);
		}
	}

	/* is a native function; such functions are always invoked in a fresh frame,
	 * even when requested as tail call, since the foreign code may rely on the
	 * stack layout of an ordinary invocation */
	if (ucv_type(*fno) == UC_CFUNCTION) {
		uc_vm_call_native(vm, *ctx, ucv_as_cfunction(*fno), mcall, nargs);
		*fno = NULL;

		return true;
	}

	if (ucv_type(*fno) == UC_CLOSURE)
		return true;

	/* name the invoked expression where it can be recovered from the source,
	 * and render the value it held as JSON, so that the message identifies
	 * both the offending expression and the value that was not a function;
	 * a NULL vm skips __tostring__ dispatch, so describing the value has no
	 * side effects */
	callee = uc_vm_callee_expr(vm);
	hint = ucv_to_jsonstring(NULL, *fno);
	hlen = strlen(hint);

	if (hlen > UC_VALUE_HINT_MAXLEN)
		memcpy(hint + UC_VALUE_HINT_MAXLEN - 3, "...", 4);

	if (callee)
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "`%s` is not a function, got %s instead", callee, hint);
	else
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "left-hand side is not a function, got %s instead", hint);

	free(callee);
	free(hint);
	ucv_put(*ctx);
	ucv_put(*fno);

	return false;
}

void
uc_vm_call_args_adjust(uc_vm_t *vm, uc_function_t *function, size_t nargs)
{
	uc_value_t *ellip;
	size_t i;

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
}

void
uc_vm_closure_call(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx,
                   bool mcall, size_t nargs, size_t stackoff)
{
	if (closure->function->native && uc_vm_native_call) {
		uc_vm_native_call(vm, closure, ctx, mcall, nargs);

		return;
	}

	if (!uc_vm_interpreter) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME,
		                      "bytecode closures cannot execute in the native runtime");
		ucv_put(ctx);
		ucv_put(&closure->header);

		return;
	}

	uc_vm_call_args_adjust(vm, closure->function, nargs);
	uc_vm_interpreter->frame_push(vm, closure, ctx, mcall, stackoff);
	uc_vm_interpreter->execute(vm);
}

uc_exception_type_t
uc_vm_call(uc_vm_t *vm, bool mcall, size_t nargs)
{
	uc_value_t *ctx = mcall ? ucv_get(uc_vm_stack_peek(vm, nargs + 1)) : NULL;
	uc_value_t *fno = ucv_get(uc_vm_stack_peek(vm, nargs));
	size_t argc = nargs & 0xffff;
	size_t stackoff;

	uc_vm_clear_exception(vm);

	if (!uc_vm_call_depth_check(vm, false)) {
		ucv_put(ctx);
		ucv_put(fno);

		return vm->exception.type;
	}

	stackoff = vm->stack.count - argc - 1;

	if (!uc_vm_call_dispatch(vm, &ctx, &fno, mcall, argc))
		return vm->exception.type;

	if (fno)
		uc_vm_closure_call(vm, ucv_as_closure(fno), ctx, mcall, argc, stackoff);

	return vm->exception.type;
}

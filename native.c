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

/* Runtime of native programs: frames, calls, exceptions and the setup of the
 * program from the tables that the LLVM backend emits. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "ucode/internal/native.h"
#include "ucode/internal/program.h"
#include "ucode/internal/vallist.h"
#include "ucode/internal/chunk.h"
#include "ucode/internal/source.h"
#include "ucode/internal/vmcore.h"
#include "ucode/internal/lib.h"
#include "ucode/internal/cli.h"
#include "ucode/source.h"
#include "ucode/vm.h"
#include "ucode/lib.h"

uc_upvalref_t *const uc_native_nocells[UC_NATIVE_NOCELLS];

static const uc_native_program_t *uc_native_running;
static uc_native_frame_t *uc_native_frames;

UC_NATIVE_INLINE uc_value_t **
uc_native_slot(uc_native_frame_t *frame, size_t slot)
{
	if (frame->cells[slot])
		return &frame->cells[slot]->value;

	return &frame->slots[slot];
}

UC_NATIVE_INLINE void
uc_native_slot_set(uc_native_frame_t *frame, size_t slot, uc_value_t *value)
{
	uc_value_t **p = uc_native_slot(frame, slot);
	uc_value_t *old = *p;

	*p = value;
	uc_native_unref(old);
}

UC_NATIVE_INLINE uc_value_t *
uc_native_slot_take(uc_native_frame_t *frame, size_t slot)
{
	uc_value_t **p = uc_native_slot(frame, slot);
	uc_value_t *v = *p;

	*p = NULL;

	return v;
}

UC_NATIVE_INLINE void
uc_native_slot_clear(uc_native_frame_t *frame, size_t slot)
{
	if (frame->cells[slot]) {
		ucv_put(&frame->cells[slot]->header);
		frame->cells[slot] = NULL;
	}

	uc_native_unref(frame->slots[slot]);
	frame->slots[slot] = NULL;
}

/* Returns the cell that a closure shares with the frame for the slot. The
 * first capture moves the value of the slot into a closed cell; later
 * captures of the same slot get the same cell until the slot is cleared. */
uc_upvalref_t *
uc_native_slot_capture(uc_native_frame_t *frame, size_t slot)
{
	uc_upvalref_t *cell = frame->cells[slot];

	if (!cell) {
		cell = (uc_upvalref_t *)ucv_upvalref_new(slot);
		cell->closed = true;
		cell->value = frame->slots[slot];
		frame->slots[slot] = NULL;
		frame->cells[slot] = cell;
		frame->captured = true;
	}

	return cell;
}

UC_NATIVE_INLINE void
uc_native_pc_set(uc_vm_t *vm, uint32_t pc)
{
	uc_vector_last(&vm->callframes)->ip = (uint8_t *)(uintptr_t)pc;
}

static void
uc_native_gc_mark(uc_vm_t *vm, void (*mark)(uc_value_t *))
{
	uc_native_frame_t *frame;
	size_t i;

	for (frame = uc_native_frames; frame; frame = frame->prev) {
		if (frame->vm != vm)
			continue;

		for (i = 0; frame->slots && i < frame->nslots; i++) {
			mark(frame->slots[i]);

			if (frame->cells[i])
				mark(&frame->cells[i]->header);
		}

		for (i = 0; i < frame->nargs; i++)
			mark(frame->args[i]);

		mark(&frame->kept->header);
		mark(frame->result);
		mark(frame->tail.fn);
		mark(frame->tail.ctx);

		for (i = 0; i < frame->tail.nargs; i++)
			mark(frame->tail.args[i]);
	}
}

static void
uc_native_args_put(uc_value_t **args, size_t nargs)
{
	for (size_t i = 0; i < nargs; i++) {
		uc_native_unref(args[i]);
		args[i] = NULL;
	}
}

static void
uc_native_args_free(uc_value_t **args, size_t nargs)
{
	uc_native_args_put(args, nargs);
	free(args);
}

/* Binds the arguments to the slots of the function as uc_vm_call_function()
 * binds them to the stack: missing arguments are null, a vararg function
 * gets the excess arguments as an array in its last parameter, excess
 * arguments of other functions are dropped. The entries of the argument
 * array are cleared, as they can be slots of the calling frame. */
void
uc_native_enter(uc_native_frame_t *frame, uc_value_t **slots, uc_upvalref_t **cells,
                uint32_t nslots)
{
	uc_vm_t *vm = frame->vm;
	uc_function_t *function = frame->closure->function;
	size_t nparams = function->nargs, i, fixed;
	uc_value_t *rest;

	frame->slots = slots;
	frame->cells = cells;
	frame->nslots = nslots;
	slots[0] = ucv_get(&frame->closure->header);

	fixed = function->vararg ? nparams - 1 : nparams;

	for (i = 0; i < fixed && i < frame->nargs; i++)
		slots[1 + i] = frame->args[i];

	if (function->vararg) {
		rest = ucv_array_new_length(vm, frame->nargs > fixed ? frame->nargs - fixed : 0);

		for (i = fixed; i < frame->nargs; i++)
			ucv_array_push(rest, frame->args[i]);

		slots[nparams] = rest;
	}
	else {
		for (i = fixed; i < frame->nargs; i++)
			uc_native_unref(frame->args[i]);
	}

	for (i = 0; i < frame->nargs; i++)
		frame->args[i] = NULL;

	frame->nargs = 0;
}

/* Releases the slots of the frame, of which only the first `nused` hold
 * values unless a closure captured one of them */
void
uc_native_frame_release(uc_native_frame_t *frame, size_t nused)
{
	uc_value_t **slots = frame->slots;

	for (size_t i = 0; frame->captured && i < frame->nslots; i++)
		uc_native_slot_clear(frame, i);

	for (size_t i = 0; !frame->captured && i < nused; i++)
		uc_native_unref(slots[i]);

	frame->slots = NULL;
	frame->nslots = 0;
}

void
uc_native_leave(uc_native_frame_t *frame)
{
	uc_native_frame_release(frame, frame->nslots);
}

/* Unwinds the operand stack of the frame to the slot of the exception
 * range and puts the exception object there, as uc_vm_handle_exception()
 * does. An exit is never caught. */
bool
uc_native_catch(uc_native_frame_t *frame, uint32_t slot)
{
	uc_vm_t *vm = frame->vm;

	if (vm->exception.type == EXCEPTION_EXIT)
		return false;

	for (size_t i = slot; i < frame->nslots; i++)
		uc_native_slot_clear(frame, i);

	frame->slots[slot] = uc_vm_exception_object(vm);
	uc_vm_clear_exception(vm);

	return true;
}

bool
uc_native_safepoint(uc_native_frame_t *frame, uint32_t pc)
{
	uc_native_pc_set(frame->vm, pc);

	return uc_native_signals_dispatch(frame->vm);
}

static uc_native_function_info_t *
uc_native_function_info(uc_function_t *function)
{
	return function->native;
}

/* Runs a native closure, and the closures it tail calls, in one call frame
 * that it pushes as uc_vm_call_function() does, so that the stack trace,
 * `this` lookups of callbacks and the recursion limit see it. Takes the
 * references to `ctx` and to the arguments, but not the array that holds the
 * arguments, nor the closure, which the caller keeps during the call. */
static uc_value_t *
uc_native_invoke_run(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx, bool mcall,
                     uc_value_t **args, size_t nargs, bool keep)
{
	uc_native_frame_t frame = {
		.prev = uc_native_frames,
		.vm = vm,
		.closure = closure,
		.kept = closure,
		.ctx = ctx,
		.mcall = mcall,
		.args = args,
		.nargs = nargs
	};

	uc_native_function_info_t *info;
	uc_callframe_t *cf;
	uc_value_t *fn;
	bool owned = false;

	if (!uc_vm_call_depth_check(vm, false)) {
		ucv_put(ctx);
		uc_native_args_put(args, nargs);

		return NULL;
	}

	uc_native_frames = &frame;

	uc_vector_push(&vm->callframes, {
		.stackframe = vm->stack.count,
		.closure = closure,
		.ctx = ctx,
		.ip = closure->function->chunk.entries,
		.mcall = mcall,
		.strict = closure->function->strict
	});

	uc_vm_run_begin(closure, keep);

	while (true) {
		info = uc_native_function_info(frame.closure->function);
		info->entry(vm, &frame);

		/* the entry took the arguments; those of a tail call are in an
		 * array of the frame, and the frame holds the tail called closure */
		if (owned)
			free(frame.args);

		frame.args = NULL;

		if (!frame.tail.pending || vm->exception.type != EXCEPTION_NONE)
			break;

		/* the request holds a native closure; C functions run at once */
		fn = frame.tail.fn;
		frame.tail.pending = false;
		frame.tail.fn = NULL;

		uc_vm_run_end(frame.closure);

		if (owned)
			ucv_put(&frame.closure->header);

		frame.closure = ucv_as_closure(fn);
		frame.ctx = frame.tail.ctx;
		frame.mcall = frame.tail.mcall;
		frame.args = frame.tail.args;
		frame.nargs = frame.tail.nargs;
		owned = true;
		frame.tail.ctx = NULL;
		frame.tail.args = NULL;
		frame.tail.nargs = 0;

		cf = uc_vector_last(&vm->callframes);
		ucv_put(cf->ctx);
		cf->closure = frame.closure;
		cf->ctx = frame.ctx;
		cf->mcall = frame.mcall;
		cf->strict = frame.closure->function->strict;
		cf->ip = frame.closure->function->chunk.entries;

		if (cf->tco < 0xffff)
			cf->tco++;

		uc_vm_run_begin(frame.closure, false);
	}

	/* a tail call request that an exception left */
	if (frame.tail.fn || frame.tail.ctx || frame.tail.args) {
		ucv_put(frame.tail.fn);
		ucv_put(frame.tail.ctx);
		uc_native_args_free(frame.tail.args, frame.tail.nargs);
	}

	uc_vm_run_end(frame.closure);
	cf = uc_vector_pop(&vm->callframes);
	uc_native_unref(cf->ctx);

	if (owned)
		ucv_put(&frame.closure->header);

	uc_native_frames = frame.prev;

	if (vm->exception.type != EXCEPTION_NONE) {
		ucv_put(frame.result);

		return NULL;
	}

	return frame.result;
}

uc_value_t *
uc_native_invoke(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx, bool mcall,
                 uc_value_t **args, size_t nargs)
{
	return uc_native_invoke_run(vm, closure, ctx, mcall, args, nargs, false);
}

/* Called by uc_vm_call() for a native closure, with the callee and its
 * arguments on the VM stack and the receiver below them for a method call. */
static void
uc_native_vm_call(uc_vm_t *vm, uc_closure_t *closure, uc_value_t *ctx, bool mcall,
                  size_t nargs)
{
	uc_value_t **args = nargs ? xalloc(nargs * sizeof(*args)) : NULL;
	uc_value_t *res;
	size_t i;

	for (i = nargs; i > 0; i--)
		args[i - 1] = uc_vm_stack_pop(vm);

	ucv_put(uc_vm_stack_pop(vm));

	if (mcall)
		ucv_put(uc_vm_stack_pop(vm));

	res = uc_native_invoke(vm, closure, ctx, mcall, args, nargs);
	ucv_put(&closure->header);
	free(args);

	if (vm->exception.type == EXCEPTION_NONE)
		uc_vm_stack_push(vm, res);
	else
		ucv_put(res);
}

static const char *
uc_native_string(uint32_t off)
{
	return (off == UC_NATIVE_NONE) ? NULL : uc_native_running->strings + off;
}

static uc_value_t *
uc_native_constant_value(const uc_native_constant_t *c)
{
	union { uint64_t u; double d; } bits = { .u = c->bits };

	switch (c->type) {
	case UC_NATIVE_CONST_STRING:
		return ucv_string_new_length(uc_native_string(c->str), c->length);

	case UC_NATIVE_CONST_DOUBLE:
		return ucv_double_new(bits.d);

	case UC_NATIVE_CONST_INT:
		return c->length ? ucv_int64_new((int64_t)c->bits) : ucv_uint64_new(c->bits);

	default:
		return NULL;
	}
}

/* Creates the source `s`, named `path` if given, as a loader names the file
 * it loads by its path. */
static uc_source_t *
uc_native_source_new(const uc_native_unit_t *unit, const uc_native_source_t *s,
                     const char *path)
{
	const char *filename = path ? path : uc_native_string(s->filename);
	uc_source_t *src;
	char *text;

	if (s->text != UC_NATIVE_NONE) {
		text = xalloc(s->textlen + 1);
		memcpy(text, uc_native_string(s->text), s->textlen);
		src = uc_source_new_buffer(filename, text, s->textlen);
	}
	else {
		src = uc_source_new_path(filename);
	}

	uc_source_runpath_set(src, path ? path : uc_native_string(s->runpath));

	for (uint32_t i = 0; i < s->nlineinfo; i++)
		uc_vector_push(&src->lineinfo, unit->bytes[s->lineinfo + i]);

	for (uint32_t i = 0; i < s->nexports; i++)
		uc_vector_push(&src->exports, uc_native_constant_value(&unit->exports[s->exports + i]));

	return src;
}

static void
uc_native_function_setup(uc_function_t *fn, const uc_native_unit_t *unit,
                         const uc_native_function_t *fd)
{
	const uint32_t *variables = unit->words + fd->variables;
	const uint8_t *offsets = unit->bytes + fd->offsets;
	uc_chunk_t *chunk = &fn->chunk;
	uc_value_t *name;
	uint32_t i, slot;
	const char *s;

	fn->nargs = fd->nargs;
	fn->nupvals = fd->nupvals;
	fn->arrow = fd->flags & UC_NATIVE_FN_ARROW;
	fn->vararg = fd->flags & UC_NATIVE_FN_VARARG;
	fn->strict = fd->flags & UC_NATIVE_FN_STRICT;
	fn->module = fd->flags & UC_NATIVE_FN_MODULE;

	for (i = 0; i < fd->noffsets; i++)
		uc_vector_push(&chunk->debuginfo.offsets, {
			.bytes = offsets[i * 2],
			.insns = offsets[i * 2 + 1]
		});

	for (i = 0; i < fd->nvarnames; i++) {
		s = uc_native_string(unit->words[fd->varnames + i]);
		name = s ? ucv_string_new(s) : NULL;
		uc_vallist_add(&chunk->debuginfo.varnames, name);
		ucv_put(name);
	}

	if (!fd->nvariables)
		return;

	/* allocated to size and not as a vector, which would reserve at least
	 * eight entries per function; nothing adds variables to a function that
	 * the runtime did not compile */
	chunk->debuginfo.variables.entries =
		xcalloc(fd->nvariables, sizeof(chunk->debuginfo.variables.entries[0]));
	chunk->debuginfo.variables.count = fd->nvariables;

	for (i = 0; i < fd->nvariables; i++) {
		slot = variables[i * 4 + 2];

		chunk->debuginfo.variables.entries[i] = (uc_varrange_t){
			.from = variables[i * 4],
			.to = variables[i * 4 + 1],
			.slot = (slot & UC_NATIVE_VAR_UPVAL)
				? (slot & ~UC_NATIVE_VAR_UPVAL) + (size_t)-1 / 2 : slot,
			.nameidx = variables[i * 4 + 3]
		};
	}
}

/* Creates a program of the unit, as the interpreter compiles one each time it
 * loads a script; `path` names its main source as the loader found it. */
static uc_program_t *
uc_native_program_new(const uc_native_unit_t *desc, const char *path)
{
	uc_program_t *prog = uc_program_new();
	uc_native_function_info_t *info, *entry = NULL;
	uc_function_t *fn;
	uc_value_t *v;
	uint32_t i;

	for (i = 0; i < desc->nsources; i++)
		uc_vector_push(&prog->sources,
		               uc_native_source_new(desc, &desc->sources[i], i ? NULL : path));

	for (i = 0; i < desc->nconstants; i++) {
		v = uc_native_constant_value(&desc->constants[i]);
		uc_vallist_add(&prog->constants, v);
		ucv_put(v);
	}

	for (i = 0; i < desc->nfunctions; i++) {
		fn = uc_program_function_new(prog, uc_native_string(desc->functions[i].name),
			prog->sources.entries[desc->functions[i].srcidx],
			desc->functions[i].srcpos);

		uc_native_function_setup(fn, desc, &desc->functions[i]);

		if (!entry) {
			entry = xalloc(sizeof(*entry) + desc->nfunctions * sizeof(uc_function_t *));
			entry->functions = (uc_function_t **)(entry + 1);
			info = entry;
		}
		else {
			info = xalloc(sizeof(*info));
			info->functions = entry->functions;
		}

		info->entry = desc->entries[i];
		entry->functions[i] = fn;
		fn->native = info;
	}

	return prog;
}

static const uc_native_unit_t *
uc_native_unit_find(const char *path, bool any_mode, bool raw_mode, bool module_mode)
{
	uint32_t flags = (raw_mode ? 0 : UC_NATIVE_UNIT_TEMPLATE) |
	                 (module_mode ? UC_NATIVE_UNIT_MODULE : 0);
	const uc_native_unit_t *unit, *found = NULL;
	char *key;

	if (!uc_native_running)
		return NULL;

	key = uc_path_normalize(path);

	for (uint32_t i = 1; key && !found && i < uc_native_running->nunits; i++) {
		unit = &uc_native_running->units[i];

		if (strcmp(uc_native_string(unit->path), key))
			continue;

		if (any_mode || unit->flags == flags)
			found = unit;
	}

	free(key);

	return found;
}

static uc_value_t *
uc_native_unit_closure(uc_vm_t *vm, const char *path, bool raw_mode, bool module_mode,
                       char **errp)
{
	const uc_native_unit_t *unit = uc_native_unit_find(path, false, raw_mode, module_mode);
	uc_program_t *prog;
	uc_value_t *closure;

	if (!unit)
		return NULL;

	if (unit->error != UC_NATIVE_NONE) {
		*errp = xstrdup(uc_native_string(unit->error));

		return NULL;
	}

	prog = uc_native_program_new(unit, path);
	closure = uc_program_main(vm, prog);
	uc_program_put(prog);

	return closure;
}

static char *
uc_native_unit_path(const char *path)
{
	return uc_native_unit_find(path, true, false, false) ? uc_path_normalize(path) : NULL;
}

/* The sources of a native program are known by their logical path, which
 * stays valid when the files are absent. Sources that are not files, such as
 * "[stdin]", have bracketed names. */
static char *
uc_native_source_path(const char *runpath)
{
	if (!runpath || *runpath == '[')
		return NULL;

	return uc_path_normalize(runpath);
}

static const uc_loader_units_t uc_native_loader_units = {
	.unit_closure = uc_native_unit_closure,
	.unit_path = uc_native_unit_path,
	.source_path = uc_native_source_path
};

/* Returns the logical paths of the embedded scripts for include() (the
 * default), render() ("template") or module imports ("module"). */
static uc_value_t *
uc_native_bundle_files(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *kind = uc_fn_arg(0), *list;
	const char *k = ucv_string_get(kind);
	uint32_t flags = 0;

	if (kind && ucv_type(kind) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Passed kind is not a string");

		return NULL;
	}

	if (k && !strcmp(k, "template"))
		flags = UC_NATIVE_UNIT_TEMPLATE;
	else if (k && !strcmp(k, "module"))
		flags = UC_NATIVE_UNIT_MODULE;
	else if (k && strcmp(k, "include")) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Unknown kind '%s'", k);

		return NULL;
	}

	list = ucv_array_new(vm);

	for (uint32_t i = 1; i < uc_native_running->nunits; i++)
		if (uc_native_running->units[i].flags == flags)
			ucv_array_push(list, ucv_string_new(uc_native_string(uc_native_running->units[i].path)));

	return list;
}

uc_declare_vector(uc_native_argv_t, char *);

static const char *uc_native_compiler = "ucode";

/* Reads the output and the errors of the compiler until it closes both, as
 * it can fill either pipe while the other one is read */
static void
uc_native_compiler_read(int fds[2], uc_stringbuf_t *bufs[2])
{
	struct pollfd p[2] = { { .fd = fds[0], .events = POLLIN }, { .fd = fds[1], .events = POLLIN } };
	char chunk[4096];
	ssize_t n;

	while (p[0].fd >= 0 || p[1].fd >= 0) {
		n = poll(p, 2, -1);

		if (n < 0 && errno == EINTR)
			continue;

		if (n < 0)
			return;

		for (size_t i = 0; i < 2; i++) {
			if (p[i].fd < 0 || !p[i].revents)
				continue;

			n = read(p[i].fd, chunk, sizeof(chunk));

			if (n > 0)
				uc_stringbuf_add(bufs[i], chunk, n);
			else if (n == 0 || errno != EINTR)
				p[i].fd = -1;
		}
	}
}

/* Runs the compiler with `argv` and `in`, if not -1, as its standard input,
 * and returns its exit status, or -1 with errno set if it did not start */
static int
uc_native_compiler_run(char **argv, int in, uc_stringbuf_t *out, uc_stringbuf_t *err)
{
	uc_stringbuf_t *bufs[2] = { out, err };
	posix_spawn_file_actions_t fa;
	int po[2], pe[2], status = -1, rv;
	pid_t pid;

	if (pipe2(po, O_CLOEXEC))
		return -1;

	if (pipe2(pe, O_CLOEXEC)) {
		close(po[0]);
		close(po[1]);

		return -1;
	}

	posix_spawn_file_actions_init(&fa);

	if (in >= 0)
		posix_spawn_file_actions_adddup2(&fa, in, STDIN_FILENO);

	posix_spawn_file_actions_adddup2(&fa, po[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, pe[1], STDERR_FILENO);
	rv = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(po[1]);
	close(pe[1]);

	if (rv == 0) {
		uc_native_compiler_read((int[]){ po[0], pe[0] }, bufs);

		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	}

	close(po[0]);
	close(pe[0]);

	if (rv) {
		errno = rv;

		return -1;
	}

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The ucode CLI arguments that compile `input` as uc_compile() would with
 * the settings of `config` */
static void
uc_native_compiler_args(uc_native_argv_t *args, uc_parse_config_t *config, const char *input)
{
	uc_stringbuf_t flags = { 0 };

	uc_vector_push(args, xstrdup(uc_native_compiler));

	uc_stringbuf_append(&flags, "-cno-interp");

	if (config->compile_module)
		uc_stringbuf_append(&flags, ",module");

	uc_vector_foreach(&config->force_dynlink_list, name)
		uc_stringbuf_printf(&flags, ",dynlink=%s", *name);

	uc_vector_push(args, xstrdup(flags.buf));
	uc_stringbuf_reset(&flags);

	if (config->raw_mode) {
		uc_vector_push(args, xstrdup("-R"));
	}
	else {
		uc_stringbuf_printf(&flags, "-T%s%s%s", config->lstrip_blocks ? "" : "no-lstrip",
		                    (!config->lstrip_blocks && !config->trim_blocks) ? "," : "",
		                    config->trim_blocks ? "" : "no-rtrim");
		uc_vector_push(args, xstrdup(flags.buf));
	}

	free(flags.buf);

	if (config->strict_declarations)
		uc_vector_push(args, xstrdup("-S"));

	uc_vector_foreach(&config->module_search_path, pattern) {
		uc_vector_push(args, xstrdup("-L"));
		uc_vector_push(args, xstrdup(*pattern));
	}

	uc_vector_push(args, xstrdup("-o"));
	uc_vector_push(args, xstrdup("-"));
	uc_vector_push(args, xstrdup(input));
	uc_vector_push(args, NULL);
}

/* A source string reaches the compiler as its standard input, which it names
 * [stdin]; the program gets the name of the string back, so that its errors
 * and traces read as in the interpreter. */
static void
uc_native_compiled_rename(uc_program_t *prog, uc_source_t *source, size_t len)
{
	uc_source_t *old, *new;

	for (size_t i = 0; i < prog->sources.count; i++) {
		old = prog->sources.entries[i];

		if (strcmp(old->filename, "[stdin]"))
			continue;

		new = uc_source_new_buffer(source->filename, xalloc(len), len);
		memcpy(new->buffer, source->buffer, len);
		uc_source_runpath_set(new, old->runpath);
		new->lineinfo = old->lineinfo;
		memset(&old->lineinfo, 0, sizeof(old->lineinfo));

		uc_source_put(old);
		prog->sources.entries[i] = new;
	}
}

/* Compiles a script that the program does not contain with the ucode CLI,
 * which runs in its own process, and loads the bytecode that it writes, which
 * the interpreter runs. */
static uc_program_t *
uc_native_compile(uc_parse_config_t *config, uc_source_t *source, char **errp)
{
	uc_stringbuf_t out = { 0 }, err = { 0 };
	uc_native_argv_t args = { 0 };
	uc_program_t *prog = NULL;
	uc_source_t *bytecode;
	size_t len = 0;
	int in = -1, rv;

	switch (uc_source_begin(source)) {
	case UC_SOURCE_TYPE_PRECOMPILED:
		return uc_program_load_entry(source, errp);

	case UC_SOURCE_TYPE_UNAVAILABLE:
		xasprintf(errp, "Unable to open source file %s: %s\n", source->filename, strerror(errno));

		return NULL;

	default:
		break;
	}

	if (source->buffer) {
		fseek(source->fp, 0, SEEK_END);
		len = ftell(source->fp);
		in = memfd_create("ucode-source", MFD_CLOEXEC);

		if (in < 0 || write(in, source->buffer, len) != (ssize_t)len || lseek(in, 0, SEEK_SET)) {
			xasprintf(errp, "Unable to pass the source to %s: %s\n", uc_native_compiler,
			          strerror(errno));

			if (in >= 0)
				close(in);

			return NULL;
		}
	}

	uc_native_compiler_args(&args, config, source->buffer ? "-" : source->filename);
	rv = uc_native_compiler_run(args.entries, in, &out, &err);

	if (in >= 0)
		close(in);

	for (size_t i = 0; i < args.count; i++)
		free(args.entries[i]);

	uc_vector_clear(&args);

	if (rv < 0) {
		xasprintf(errp, "Unable to run %s: %s\n", uc_native_compiler, strerror(errno));
	}
	else if (rv > 0 || !out.buf) {
		xasprintf(errp, "%s", err.buf ? err.buf : "Compilation failed\n");
	}
	else {
		bytecode = uc_source_new_buffer(source->filename, out.buf, uc_stringbuf_length(&out));
		out.buf = NULL;
		uc_source_runpath_set(bytecode, source->runpath);
		prog = uc_program_load_entry(bytecode, errp);
		uc_source_put(bytecode);
	}

	if (prog && source->buffer)
		uc_native_compiled_rename(prog, source, len);

	free(out.buf);
	free(err.buf);

	return prog;
}

/* Native program options, accepted before the first program argument. They
 * use long names, so that the short options of the program itself pass
 * through to ARGV unchanged. */
static const struct {
	const char *name;
	char opt;
} uc_native_options[] = {
	{ "--ucode-define",      'D' },
	{ "--ucode-define-file", 'F' },
	{ "--ucode-undefine",    'U' },
	{ "--ucode-library",     'l' },
	{ "--ucode-search-path", 'L' },
	{ "--ucode-gc",          'g' },
	{ "--ucode-compiler",    'C' },
};

static char
uc_native_option(int argc, char **argv, int i)
{
	if (i + 1 >= argc)
		return 0;

	for (size_t j = 0; j < ARRAY_SIZE(uc_native_options); j++)
		if (!strcmp(argv[i], uc_native_options[j].name))
			return uc_native_options[j].opt;

	return 0;
}

static int
uc_native_options_apply(uc_vm_t *vm, int argc, char **argv, uc_value_t *args)
{
	FILE *stdin_unused = stdin;
	int i;
	char opt;

	for (i = 1; i < argc && (opt = uc_native_option(argc, argv, i)) != 0; i += 2) {
		switch (opt) {
		case 'D':
			if (!uc_cli_define_string(argv[i + 1], uc_vm_scope_get(vm)))
				return -1;
			break;

		case 'F':
			if (!uc_cli_define_file(argv[i + 1], uc_vm_scope_get(vm), &stdin_unused))
				return -1;
			break;

		case 'U':
			ucv_object_delete(uc_vm_scope_get(vm), argv[i + 1]);
			break;

		case 'l':
			if (!uc_cli_library_load(argv[i + 1], vm))
				return -1;
			break;

		case 'g':
			vm->gc_interval = atoi(argv[i + 1]);
			break;

		case 'C':
			uc_native_compiler = argv[i + 1];
			break;
		}
	}

	if (i < argc && !strcmp(argv[i], "--"))
		i++;

	for (; i < argc; i++)
		ucv_array_push(args, ucv_string_new(argv[i]));

	return 0;
}

/* Runs the entry of the program and returns the exit status as the ucode CLI
 * does: 0, the code of exit(), or -2 after an uncaught exception, which the
 * exception handler of the VM reports. */
static int
uc_native_run(uc_vm_t *vm, uc_program_t *program)
{
	uc_value_t *closure = uc_program_main(vm, program);
	uc_value_t *res;
	int rv = 0;

	/* the entry closure keeps the exports of the modules that the program
	 * links, as uc_vm_execute() does */
	res = uc_native_invoke_run(vm, ucv_as_closure(closure), NULL, false, NULL, 0, true);
	ucv_put(closure);
	ucv_put(res);

	switch (vm->exception.type) {
	case EXCEPTION_NONE:
		break;

	case EXCEPTION_EXIT:
		rv = vm->arg.s32;
		break;

	default:
		if (vm->exhandler)
			vm->exhandler(vm, &vm->exception);

		rv = -2;
		break;
	}

	return rv;
}

/* Loads the library that stands in for libucode, see native_abi.c, before any
 * binding, so that the dependency of a binding on libucode resolves to it by
 * its SONAME; the RUNPATH of the executable leads to it. A libucode that is
 * already loaded is the interpreter library and must not serve bindings. */
static bool
uc_native_abi_load(void)
{
	int saved_errno = errno;
	void *h = dlopen(UC_NATIVE_ABI_SONAME, RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);

	if (!h)
		h = dlopen(UC_NATIVE_ABI_SONAME, RTLD_NOW | RTLD_GLOBAL);

	/* failed lookups must not leave an error for the dlerror() of a later
	 * dlopen() to report, nor change the errno that the program starts with */
	dlerror();
	errno = saved_errno;

	/* without it, a binding that depends on libucode loads the interpreter
	 * library, if one is installed, beside the runtime of the program */
	if (!h) {
		fprintf(stderr, "%s not found, bindings that depend on it load the "
		        "interpreter library\n", UC_NATIVE_ABI_SONAME);

		return true;
	}

	if (dlsym(h, "uc_native_abi_provider"))
		return true;

	fprintf(stderr, "%s is loaded, but it is not the library of native programs\n",
	        UC_NATIVE_ABI_SONAME);

	return false;
}

int
uc_native_main(int argc, char **argv, const uc_native_program_t *np)
{
	/* the settings of the ucode CLI, which apply to the scripts that the
	 * program compiles at runtime */
	uc_parse_config_t config = {
		.lstrip_blocks = true,
		.trim_blocks = true,
		.raw_mode = !(np->units[0].flags & UC_NATIVE_UNIT_TEMPLATE),
		.setup_signal_handlers = true
	};

	uc_program_t *program;
	uc_vm_t vm = { 0 };
	uc_value_t *args;
	char opt;
	int i, rv;

	if (np->version != UC_NATIVE_ABI_VERSION || np->nunits == 0) {
		fprintf(stderr, "Native program ABI version %u is not supported, expected %u\n",
		        np->version, UC_NATIVE_ABI_VERSION);

		return 1;
	}

	if (!uc_native_abi_load())
		return 1;

	uc_native_running = np;
	uc_vm_native_call_set(uc_native_vm_call);
	uc_loader_units_set(&uc_native_loader_units);
	uc_loader_compile_set(uc_native_compile);

	/* the search path is part of the configuration the VM starts with; as in
	 * the CLI, the given entries come before the default ones */
	for (i = 1; i < argc && (opt = uc_native_option(argc, argv, i)) != 0; i += 2)
		if (opt == 'L')
			uc_cli_search_path(argv[i + 1], &config);

	uc_search_path_init(&config.module_search_path);

	uc_vm_init(&vm, &config);
	uc_thread_context_get()->gc_roots_mark = uc_native_gc_mark;
	uc_stdlib_load(uc_vm_scope_get(&vm));
	ucv_object_add(uc_vm_scope_get(&vm), "bundle_files",
	               ucv_cfunction_new("bundle_files", uc_native_bundle_files));

	program = uc_native_program_new(&np->units[0], NULL);
	args = ucv_array_new(&vm);
	ucv_object_add(uc_vm_scope_get(&vm), "ARGV", ucv_get(args));

	if (np->script_name != UC_NATIVE_NONE)
		ucv_object_add(uc_vm_scope_get(&vm), "SCRIPT_NAME",
		               ucv_string_new(uc_native_string(np->script_name)));

	rv = uc_native_options_apply(&vm, argc, argv, args);

	if (rv == 0) {
		if (vm.gc_interval)
			uc_vm_gc_start(&vm, vm.gc_interval);

		rv = uc_native_run(&vm, program);
	}
	else {
		rv = 1;
	}

	ucv_put(args);
	uc_program_put(program);
	uc_vm_free(&vm);
	uc_search_path_free(&config.module_search_path);

	return rv;
}

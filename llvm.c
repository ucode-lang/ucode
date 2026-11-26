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

/* LLVM IR backend.
 *
 * Each function becomes a native function that keeps the values of the
 * operand stack in an array of slots, one per stack depth, as found by a
 * depth analysis of the bytecode. Every instruction becomes a call to a
 * runtime helper with the depth and the decoded operands as arguments, and
 * the branches and exception handlers become native control flow. The
 * bytecode itself is not part of the output. */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "ucode/internal/native.h"
#include "ucode/internal/program.h"
#include "ucode/internal/vm.h"
#include "ucode/internal/lib.h"
#include "ucode/internal/source.h"
#include "ucode/internal/vallist.h"
#include "ucode/compiler.h"

#define LLVM_INSN_FALLIBLE	(1u << 0)

typedef struct {
	size_t off, next;
	uint32_t arg;
	uc_vm_insn_t insn;
	bool tail;
	/* reached by a branch or an exception handler */
	bool target;
	ssize_t depth;
	size_t extra, nextra;
} llvm_insn_t;

uc_declare_vector(llvm_insns_t, llvm_insn_t);
uc_declare_vector(llvm_words_t, uint32_t);
uc_declare_vector(llvm_bytes_t, uint8_t);
uc_declare_vector(llvm_queue_t, size_t);

typedef struct {
	uc_program_t *program;
	uc_function_t *fn;
	size_t id;
	llvm_insns_t insns;
	llvm_words_t extra;
	size_t nslots;
} llvm_func_t;

typedef struct {
	uc_stringbuf_t *data;
	/* offsets by string, of the strings without NUL bytes */
	uc_value_t *index;
} llvm_strings_t;

/* The arrays of a unit that its function and source tables index */
typedef struct {
	llvm_strings_t *strings;
	size_t unit;
	bool debug;
	llvm_words_t words;
	llvm_bytes_t bytes;
} llvm_tables_t;

typedef struct {
	char *path;
	uint32_t flags;
	bool relocated;
	uc_program_t *program;
	char *error;
	size_t nexports, nwords, nbytes;
} llvm_unit_t;

uc_declare_vector(llvm_units_t, llvm_unit_t);

static const char *llvm_insn_names[] = {
#undef __insn
#define __insn(_name) #_name,
	__insns
#undef __insn
};

static uint32_t
llvm_read_u32(const uint8_t *p)
{
	return p[0] * 0x1000000UL + p[1] * 0x10000UL + p[2] * 0x100UL + p[3];
}

static uint32_t
llvm_operand(const uint8_t *p, int8_t argtype)
{
	switch (argtype) {
	case 1:  return p[0];
	case 2:  return p[0] * 0x100 + p[1];
	case 4:  return llvm_read_u32(p);
	case -4: return llvm_read_u32(p) - 0x7fffffff;
	default: return 0;
	}
}

static bool
llvm_fail(char **errp, llvm_func_t *f, llvm_insn_t *in, const char *msg)
{
	if (in)
		xasprintf(errp, "Function '%s' at offset %zu (%s): %s\n",
		          f->fn->name, in->off, llvm_insn_names[in->insn], msg);
	else
		xasprintf(errp, "Function '%s': %s\n", f->fn->name, msg);

	return false;
}

/* Reads the operands that the handler of an instruction reads beyond its
 * fixed operand, as uc_vm_insn_call(), uc_vm_insn_load_closure(),
 * uc_vm_insn_import() and uc_vm_insn_dynload() do, into the extra words of
 * the function. */
static bool
llvm_decode_extra(llvm_func_t *f, llvm_insn_t *in, const uint8_t *p, const uint8_t *end,
                  char **errp)
{
	uc_function_t *target;
	size_t n, i, width = 4;
	uint32_t nargs;

	switch (in->insn) {
	case I_CALL:
		n = (in->arg >> 16) & 0x7fff;
		width = 2;
		break;

	case I_CLFN:
	case I_ARFN:
		target = uc_program_function_load(f->program, in->arg);

		if (!target)
			return llvm_fail(errp, f, in, "invalid function index");

		n = target->module ? 0 : target->nupvals;
		break;

	case I_IMPORT:
		n = ((in->arg >> 16) == 0xffff) ? 1 + (in->arg & 0xffff) : 0;
		break;

	case I_DYNLOAD:
		n = in->arg & 0xffff;
		break;

	default:
		n = 0;
		break;
	}

	in->extra = f->extra.count;
	in->nextra = n;

	for (i = 0; i < n; i++) {
		width = (in->insn == I_CALL || (in->insn == I_IMPORT && i == 0)) ? 2 : 4;

		if (p + width > end)
			return llvm_fail(errp, f, in, "truncated operands");

		uc_vector_push(&f->extra, (width == 2) ? p[0] * 0x100U + p[1] : llvm_read_u32(p));
		p += width;
	}

	in->next = p - f->fn->chunk.entries;

	if (in->insn == I_CALL) {
		nargs = in->arg & 0xffff;

		for (i = 0; i < n; i++) {
			uint32_t *v = &f->extra.entries[in->extra + i];

			if (*v >= nargs || (i > 0 && *v >= v[-1]))
				return llvm_fail(errp, f, in, "invalid argument spread positions");
		}

		in->tail = (in->next + 2 <= f->fn->chunk.count &&
		            f->fn->chunk.entries[in->next] == I_RETURN &&
		            f->fn->chunk.entries[in->next + 1] == 0);
	}

	if (in->insn == I_CLFN || in->insn == I_ARFN)
		for (i = 0; i < n; i++)
			f->extra.entries[in->extra + i] -= 0x7fffffff;

	return true;
}

static bool
llvm_decode(llvm_func_t *f, char **errp)
{
	uc_chunk_t *chunk = &f->fn->chunk;
	const uint8_t *end = chunk->entries + chunk->count;
	size_t off = 0, width;
	int8_t argtype;
	llvm_insn_t *in, *prev = NULL;

	if (!chunk->count)
		return llvm_fail(errp, f, NULL, "empty function");

	while (off < chunk->count) {
		if (chunk->entries[off] >= __I_MAX) {
			xasprintf(errp, "Function '%s': invalid opcode %u at offset %zu\n",
			          f->fn->name, chunk->entries[off], off);

			return false;
		}

		in = uc_vector_push(&f->insns, {
			.off = off,
			.insn = chunk->entries[off],
			.depth = -1
		});

		argtype = uc_vm_insn_to_argtype(in->insn);
		width = abs(argtype);

		if (off + 1 + width > chunk->count)
			return llvm_fail(errp, f, in, "truncated operand");

		in->arg = llvm_operand(chunk->entries + off + 1, argtype);

		if (!llvm_decode_extra(f, in, chunk->entries + off + 1 + width, end, errp))
			return false;

		if (in->insn == I_DYNLOAD && (!prev || prev->insn != I_LOAD))
			return llvm_fail(errp, f, in, "module name is not a constant load");

		off = in->next;

		/* the tail call marker after RETURN is data, not an instruction */
		if (in->insn == I_RETURN && off < chunk->count && chunk->entries[off] == 0)
			off++;

		prev = in;
	}

	return true;
}

static ssize_t
llvm_insn_index(llvm_func_t *f, size_t off)
{
	size_t lo = 0, hi = f->insns.count, mid;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;

		if (f->insns.entries[mid].off == off)
			return mid;

		if (f->insns.entries[mid].off < off)
			lo = mid + 1;
		else
			hi = mid;
	}

	return -1;
}

static size_t
llvm_jump_target(llvm_insn_t *in)
{
	if (in->insn == I_JMPNT)
		return in->off + (in->arg & 0xffff) - 0x7fff;

	return in->off + (int32_t)in->arg;
}

/* Returns the offset that the instruction pointer of the interpreter holds
 * when the instruction raises: the target of a backward jump, whose signal
 * handlers run after the jump, and the offset after any other instruction. */
static size_t
llvm_raise_offset(llvm_insn_t *in)
{
	switch (in->insn) {
	case I_JMP:
	case I_JMPZ:
	case I_JMPNT:
		return llvm_jump_target(in);

	default:
		return in->next;
	}
}

/* Returns the exception range that covers the instruction, the first one in
 * which its raise offset lies, as uc_vm_handle_exception() selects it. */
static uc_ehrange_t *
llvm_handler(llvm_func_t *f, llvm_insn_t *in)
{
	uc_ehranges_t *ranges = &f->fn->chunk.ehranges;
	size_t off = llvm_raise_offset(in);

	for (size_t i = 0; i < ranges->count; i++)
		if (ranges->entries[i].from <= off && off < ranges->entries[i].to)
			return &ranges->entries[i];

	return NULL;
}

/* Returns the number of values the instruction needs on the stack, and in
 * `delta` the change of the depth at the instruction that follows it. */
static size_t
llvm_stack_effect(llvm_insn_t *in, ssize_t *delta)
{
	uint32_t n;

	switch (in->insn) {
	case I_LOAD: case I_LOAD8: case I_LOAD16: case I_LOAD32:
	case I_LREXP: case I_LTHIS: case I_LNULL: case I_LTRUE: case I_LFALSE:
	case I_LVAR: case I_LLOC: case I_LUPV: case I_COPY:
	case I_CLFN: case I_ARFN: case I_NARR: case I_NOBJ:
		*delta = 1;
		return 0;

	case I_SLOC: case I_SUPV: case I_SVAR:
	case I_ULOC: case I_UUPV: case I_UVAR:
	case I_NOT: case I_COMPL: case I_PLUS: case I_MINUS:
	case I_RETURN:
		*delta = 0;
		return 1;

	case I_BOR: case I_BXOR: case I_BAND: case I_EQS: case I_NES:
	case I_EQ: case I_NE: case I_LT: case I_LE: case I_GT: case I_GE:
	case I_IN: case I_LSHIFT: case I_RSHIFT: case I_ADD: case I_SUB:
	case I_MUL: case I_DIV: case I_MOD: case I_EXP:
	case I_LVAL: case I_DELETE: case I_MARR: case I_MOBJ:
		*delta = -1;
		return 2;

	case I_PVAL: case I_NEXTK:
		*delta = 0;
		return 2;

	case I_NEXTKV:
		*delta = 1;
		return 2;

	case I_SVAL: case I_UVAL:
		*delta = -2;
		return 3;

	case I_POP: case I_CUPV: case I_PRINT: case I_JMPZ:
		*delta = -1;
		return 1;

	case I_JMPNT:
		*delta = 0;
		return ((in->arg >> 29) & 7) + 1;

	case I_PARR: case I_SOBJ:
		*delta = -(ssize_t)in->arg;
		return in->arg + 1;

	case I_CALL:
		n = (in->arg & 0xffff) + (in->arg >> 31);
		*delta = -(ssize_t)n;
		return n + 1;

	case I_STCALL:
		n = in->arg & 0xffff;
		*delta = 1 - (ssize_t)n;
		return n;

	case I_IMPORT:
		*delta = ((in->arg >> 16) == 0xffff) ? 1 : 0;
		return 0;

	case I_DYNLOAD:
		*delta = (in->arg & 0xffff) ? -1 : 0;
		return 1;

	default:
		*delta = 0;
		return 0;
	}
}

static bool
llvm_check_operands(llvm_func_t *f, llvm_insn_t *in, size_t depth, char **errp)
{
	uc_function_t *fn = f->fn;
	size_t nconst = f->program->constants.isize;
	uint32_t idx = in->arg & 0xffffff;
	int32_t cap;

	switch (in->insn) {
	case I_LLOC: case I_SLOC:
		if (in->arg >= depth)
			return llvm_fail(errp, f, in, "local index outside live values");
		break;

	case I_ULOC:
		if (idx >= depth)
			return llvm_fail(errp, f, in, "local index outside live values");
		break;

	case I_LUPV: case I_SUPV:
		if (in->arg >= fn->nupvals)
			return llvm_fail(errp, f, in, "invalid upvalue index");
		break;

	case I_UUPV:
		if (idx >= fn->nupvals)
			return llvm_fail(errp, f, in, "invalid upvalue index");
		break;

	case I_LOAD: case I_LREXP: case I_LVAR: case I_SVAR:
		if (in->arg >= nconst)
			return llvm_fail(errp, f, in, "invalid constant index");
		break;

	case I_UVAR:
		if (idx >= nconst)
			return llvm_fail(errp, f, in, "invalid constant index");
		break;

	case I_COPY:
		if (in->arg >= depth)
			return llvm_fail(errp, f, in, "copy outside live values");
		break;

	case I_EXPORT:
		if (in->arg >= depth)
			return llvm_fail(errp, f, in, "export outside live values");
		break;

	case I_IMPORT:
		if ((in->arg >> 16) != 0xffff && (in->arg >> 16) >= fn->nupvals)
			return llvm_fail(errp, f, in, "invalid import upvalue");
		break;

	case I_DYNLOAD:
		if ((in->arg & 0xffff) && (in->arg >> 16) + (in->arg & 0xffff) > fn->nupvals)
			return llvm_fail(errp, f, in, "invalid import upvalues");
		break;

	case I_CLFN: case I_ARFN:
		for (size_t i = 0; i < in->nextra; i++) {
			cap = (int32_t)f->extra.entries[in->extra + i];

			/* a capture of the slot at the current depth is the slot that
			 * receives the new closure, for recursive named functions */
			if (cap < 0 && (size_t)(-cap - 1) > depth)
				return llvm_fail(errp, f, in, "capture outside live values");

			if (cap >= 0 && (size_t)cap >= fn->nupvals)
				return llvm_fail(errp, f, in, "invalid captured upvalue");
		}
		break;

	default:
		break;
	}

	return true;
}

/* Queues the instruction at `off` for the analysis; `branch` tells that it
 * is reached by other means than from the instruction before it. */
static bool
llvm_enqueue(llvm_func_t *f, llvm_queue_t *queue, size_t off, ssize_t depth, bool branch,
             char **errp)
{
	ssize_t idx = llvm_insn_index(f, off);
	llvm_insn_t *in;

	if (idx < 0) {
		xasprintf(errp, "Function '%s': control flow into offset %zu, which is no instruction\n",
		          f->fn->name, off);

		return false;
	}

	in = &f->insns.entries[idx];
	in->target |= branch;

	if (in->depth >= 0) {
		if (in->depth != depth) {
			xasprintf(errp, "Function '%s': inconsistent stack depth at offset %zu\n",
			          f->fn->name, off);

			return false;
		}

		return true;
	}

	in->depth = depth;
	uc_vector_push(queue, idx);

	return true;
}

static uint32_t llvm_insn_flags(llvm_insn_t *in);

/* Returns whether the instruction can raise an exception, and so branch to
 * the handler of an exception range that covers it: a backward jump can, as
 * it dispatches signals to handlers in ucode. */
static bool
llvm_insn_raises(llvm_insn_t *in)
{
	switch (in->insn) {
	case I_JMP:
	case I_JMPZ:
	case I_JMPNT:
		return llvm_jump_target(in) <= in->off;

	case I_RETURN:
		return false;

	default:
		return llvm_insn_flags(in) & LLVM_INSN_FALLIBLE;
	}
}

/* Assigns each reachable instruction the depth of the operand stack before
 * it, which is the slot its first result goes to. */
static bool
llvm_analyze(llvm_func_t *f, char **errp)
{
	llvm_queue_t queue = { 0 };
	uc_ehranges_t *ranges = &f->fn->chunk.ehranges;
	size_t maxdepth = f->fn->nargs + 1, need, i;
	llvm_insn_t *in;
	uc_ehrange_t *h;
	ssize_t delta;
	bool ok = true;

	if (f->fn->vararg && !f->fn->nargs)
		return llvm_fail(errp, f, NULL, "invalid vararg function");

	for (i = 0; i < ranges->count; i++)
		if (ranges->entries[i].from > ranges->entries[i].to ||
		    ranges->entries[i].to > f->fn->chunk.count ||
		    llvm_insn_index(f, ranges->entries[i].target) < 0)
			return llvm_fail(errp, f, NULL, "invalid exception range");

	ok = llvm_enqueue(f, &queue, 0, f->fn->nargs + 1, true, errp);

	for (i = 0; ok && i < queue.count; i++) {
		in = &f->insns.entries[queue.entries[i]];
		need = llvm_stack_effect(in, &delta);

		if ((size_t)in->depth < need) {
			ok = llvm_fail(errp, f, in, "operand stack underflow");
			break;
		}

		if (in->insn == I_SOBJ && (in->arg & 1)) {
			ok = llvm_fail(errp, f, in, "odd object initializer count");
			break;
		}

		ok = llvm_check_operands(f, in, in->depth, errp);

		if (in->depth + 1 > (ssize_t)maxdepth)
			maxdepth = in->depth + 1;

		if (in->depth + delta > (ssize_t)maxdepth)
			maxdepth = in->depth + delta;

		h = llvm_insn_raises(in) ? llvm_handler(f, in) : NULL;

		/* a tail call leaves the frame, so no handler of it can catch */
		if (ok && h && in->insn == I_CALL && in->tail) {
			ok = llvm_fail(errp, f, in, "tail call inside an exception range");
			break;
		}

		if (ok && h) {
			if ((ssize_t)h->slot > in->depth && (ssize_t)h->slot > in->depth + delta)
				ok = llvm_fail(errp, f, in, "handler slot outside live values");
			else
				ok = llvm_enqueue(f, &queue, h->target, h->slot + 1, true, errp);
		}

		if (!ok || in->insn == I_RETURN || (in->insn == I_CALL && in->tail))
			continue;

		if (in->insn == I_JMP || in->insn == I_JMPZ) {
			ok = llvm_enqueue(f, &queue, llvm_jump_target(in), in->depth + delta, true, errp);

			if (in->insn == I_JMP)
				continue;
		}

		if (ok && in->insn == I_JMPNT)
			ok = llvm_enqueue(f, &queue, llvm_jump_target(in),
			                  in->depth - ((in->arg >> 29) & 7), true, errp);

		if (ok)
			ok = llvm_enqueue(f, &queue, in->next, in->depth + delta, false, errp);
	}

	uc_vector_clear(&queue);
	f->nslots = maxdepth;

	return ok;
}

static void
llvm_emit_bytes(FILE *out, const void *data, size_t len)
{
	const unsigned char *p = data;

	fputs("c\"", out);

	for (size_t i = 0; i < len; i++) {
		if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '"' && p[i] != '\\')
			fputc(p[i], out);
		else
			fprintf(out, "\\%02X", p[i]);
	}

	fputs("\\00\"", out);
}

/* Adds a string, terminated by NUL, to the string block and returns its
 * offset; strings without NUL bytes are stored once. */
static uint32_t
llvm_string_add(llvm_strings_t *strings, const char *s, size_t len)
{
	bool plain = !memchr(s, 0, len);
	uc_value_t *known = plain ? ucv_object_get(strings->index, s, NULL) : NULL;
	uint32_t off;

	if (known)
		return ucv_uint64_get(known);

	off = uc_stringbuf_length(strings->data);
	ucv_stringbuf_addstr(strings->data, s, len);
	ucv_stringbuf_addstr(strings->data, "", 1);

	if (plain)
		ucv_object_add(strings->index, s, ucv_uint64_new(off));

	return off;
}

static uint32_t
llvm_cstring_add(llvm_strings_t *strings, const char *s)
{
	return s ? llvm_string_add(strings, s, strlen(s)) : UC_NATIVE_NONE;
}

static uint32_t
llvm_value_string_add(llvm_strings_t *strings, uc_value_t *v)
{
	if (ucv_type(v) != UC_STRING)
		return UC_NATIVE_NONE;

	return llvm_string_add(strings, ucv_string_get(v), ucv_string_length(v));
}

static void
llvm_emit_strings(FILE *out, llvm_strings_t *strings)
{
	fprintf(out, "@uc_strings = private unnamed_addr constant [%d x i8] ",
	        uc_stringbuf_length(strings->data) + 1);
	llvm_emit_bytes(out, strings->data->buf, uc_stringbuf_length(strings->data));
	fputs("\n", out);
}

static uint32_t
llvm_insn_flags(llvm_insn_t *in)
{
	switch (in->insn) {
	case I_NOOP: case I_LOAD: case I_LOAD8: case I_LOAD16: case I_LOAD32:
	case I_LNULL: case I_LTRUE: case I_LFALSE: case I_LTHIS:
	case I_LLOC: case I_LUPV: case I_COPY: case I_SLOC: case I_SUPV:
	case I_POP: case I_CUPV: case I_NARR: case I_PARR: case I_NOBJ:
	case I_CLFN: case I_ARFN: case I_EXPORT:
	case I_NOT: case I_COMPL: case I_PLUS: case I_MINUS: case I_IN:
		return 0;

	default:
		return LLVM_INSN_FALLIBLE;
	}
}

static void
llvm_lower_name(char *buf, size_t size, const char *name)
{
	size_t i;

	for (i = 0; name[i] && i + 1 < size; i++)
		buf[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];

	buf[i] = 0;
}

static void
llvm_emit_exception_target(FILE *out, llvm_func_t *f, llvm_insn_t *in)
{
	uc_ehrange_t *h = llvm_handler(f, in);

	if (h)
		fprintf(out, "%%H%zu", (size_t)(h - f->fn->chunk.ehranges.entries));
	else
		fputs("%leave", out);
}

static void
llvm_emit_branch_to(FILE *out, llvm_func_t *f, llvm_insn_t *in, size_t target)
{
	if (target <= in->off)
		fprintf(out, "%%S%zu_%zu", in->off, target);
	else
		fprintf(out, "%%L%zu", target);
}

static void
llvm_emit_args(FILE *out, llvm_func_t *f, llvm_insn_t *in, const char *prefix)
{
	fprintf(out, "(ptr %%frame, i32 %zu, i32 %zd, i32 %u, ",
	        in->next, in->depth, in->arg);

	if (in->nextra)
		fprintf(out, "ptr @%s_x%zu)", prefix, in->off);
	else
		fputs("ptr null)", out);
}

/* Emits the safepoint block of a backward jump, which dispatches signals
 * before the loop runs again, as the interpreter does after every
 * instruction. */
static void
llvm_emit_safepoint(FILE *out, llvm_func_t *f, llvm_insn_t *in, size_t target)
{
	if (target > in->off)
		return;

	fprintf(out, "S%zu_%zu:\n", in->off, target);
	fprintf(out, "  %%sp%zu_%zu = call i1 @uc_native_safepoint(ptr %%frame, i32 %zu)\n",
	        in->off, target, target);
	fprintf(out, "  br i1 %%sp%zu_%zu, label %%L%zu, label ", in->off, target, target);
	llvm_emit_exception_target(out, f, in);
	fputc('\n', out);
}

static void
llvm_emit_insn(FILE *out, llvm_func_t *f, llvm_insn_t *in, const char *prefix)
{
	char name[16];
	size_t target;

	llvm_lower_name(name, sizeof(name), llvm_insn_names[in->insn]);
	fprintf(out, "L%zu:\n", in->off);

	switch (in->insn) {
	/* A frame starts with empty slots, and each helper empties the slots of
	 * the values it consumes, so the slot that LNULL pushes is empty. */
	case I_NOOP:
	case I_LNULL:
		fprintf(out, "  br label %%L%zu\n", in->next);
		return;

	case I_JMP:
		target = llvm_jump_target(in);
		fputs("  br label ", out);
		llvm_emit_branch_to(out, f, in, target);
		fputc('\n', out);
		llvm_emit_safepoint(out, f, in, target);
		return;

	case I_JMPZ:
	case I_JMPNT:
		target = llvm_jump_target(in);
		fprintf(out, "  %%t%zu = call i1 @uc_native_%s", in->off, name);
		llvm_emit_args(out, f, in, prefix);
		fprintf(out, "\n  br i1 %%t%zu, label ", in->off);
		llvm_emit_branch_to(out, f, in, target);
		fprintf(out, ", label %%L%zu\n", in->next);
		llvm_emit_safepoint(out, f, in, target);
		return;

	case I_RETURN:
		fputs("  call void @uc_native_return", out);
		llvm_emit_args(out, f, in, prefix);
		fputs("\n  ret void\n", out);
		return;

	case I_CALL:
		if (in->tail) {
			fputs("  call i1 @uc_native_tailcall", out);
			llvm_emit_args(out, f, in, prefix);
			fputs("\n  br label %leave\n", out);
			return;
		}

		break;

	default:
		break;
	}

	if (!(llvm_insn_flags(in) & LLVM_INSN_FALLIBLE)) {
		fprintf(out, "  call void @uc_native_%s", name);
		llvm_emit_args(out, f, in, prefix);
		fprintf(out, "\n  br label %%L%zu\n", in->next);

		return;
	}

	fprintf(out, "  %%r%zu = call i1 @uc_native_%s", in->off, name);
	llvm_emit_args(out, f, in, prefix);
	fprintf(out, "\n  br i1 %%r%zu, label %%L%zu, label ", in->off, in->next);
	llvm_emit_exception_target(out, f, in);
	fputc('\n', out);
}

/* Returns the instruction `n` places after the one at index `i` if the ones
 * up to it can run as one helper: none of them after the first is reached
 * by a branch. */
static llvm_insn_t *
llvm_fusable(llvm_func_t *f, size_t i, size_t n)
{
	llvm_insn_t *in;

	if (i + n >= f->insns.count)
		return NULL;

	for (size_t j = 1; j <= n; j++) {
		in = &f->insns.entries[i + j];

		if (in->target || in->depth < 0)
			return NULL;
	}

	return &f->insns.entries[i + n];
}

static bool
llvm_insn_is(llvm_insn_t *in, uc_vm_insn_t a, uc_vm_insn_t b)
{
	return in && (in->insn == a || in->insn == b);
}

/* LOAD of a constant key, then LVAL or PVAL, after an LLOC or LUPV or not */
static size_t
llvm_emit_getk(FILE *out, llvm_func_t *f, size_t i)
{
	llvm_insn_t *first = &f->insns.entries[i], *load = first, *get;
	uint32_t src = UC_NATIVE_GETK_STACK;
	size_t n = 1;

	if (llvm_insn_is(first, I_LLOC, I_LUPV)) {
		if (first->arg >= UC_NATIVE_GETK_UPVAL)
			return 0;

		load = llvm_fusable(f, i, 1);
		src = first->arg | ((first->insn == I_LUPV) ? UC_NATIVE_GETK_UPVAL : 0);
		n = 2;
	}

	get = llvm_fusable(f, i, n);

	if (!load || load->insn != I_LOAD || load->arg > 0x00ffffff ||
	    !llvm_insn_is(get, I_LVAL, I_PVAL))
		return 0;

	fprintf(out, "L%zu:\n", first->off);
	fprintf(out, "  %%r%zu = call i1 @uc_native_getk(ptr %%frame, i32 %zu, i32 %zd, "
	        "i32 %u, i32 %u)\n", first->off, get->next, first->depth,
	        load->arg | ((get->insn == I_PVAL) ? UC_NATIVE_GETK_KEEP : 0), src);
	fprintf(out, "  br i1 %%r%zu, label %%L%zu, label ", first->off, get->next);
	llvm_emit_exception_target(out, f, get);
	fputc('\n', out);

	return n + 1;
}

/* A helper `name` for the instruction at `i` up to `last`, called with the
 * operands of the instruction `op`, which raises if `fallible` is set, and
 * the depth before the first one */
static void
llvm_emit_fused_call(FILE *out, llvm_func_t *f, size_t i, llvm_insn_t *op, llvm_insn_t *last,
                     uint32_t arg, const char *name, bool fallible, const char *prefix)
{
	llvm_insn_t *first = &f->insns.entries[i];
	llvm_insn_t in = *op;

	in.depth = first->depth;
	in.arg = arg;

	fprintf(out, "L%zu:\n", first->off);

	if (!fallible) {
		fprintf(out, "  call void @uc_native_%s", name);
		llvm_emit_args(out, f, &in, prefix);
		fprintf(out, "\n  br label %%L%zu\n", last->next);

		return;
	}

	fprintf(out, "  %%r%zu = call i1 @uc_native_%s", first->off, name);
	llvm_emit_args(out, f, &in, prefix);
	fprintf(out, "\n  br i1 %%r%zu, label %%L%zu, label ", first->off, last->next);
	llvm_emit_exception_target(out, f, op);
	fputc('\n', out);
}

/* A helper `name` that runs the instruction at `i` up to the JMPZ `jump`,
 * and up to `last` if the jump is not taken, and returns whether the jump
 * is taken */
static void
llvm_emit_fused_jump(FILE *out, llvm_func_t *f, size_t i, llvm_insn_t *jump, llvm_insn_t *last,
                     uint32_t arg, const char *name)
{
	llvm_insn_t *first = &f->insns.entries[i];
	size_t target = llvm_jump_target(jump);

	fprintf(out, "L%zu:\n", first->off);
	fprintf(out, "  %%t%zu = call i1 @uc_native_%s(ptr %%frame, i32 %zu, i32 %zd, "
	        "i32 %u, ptr null)\n", first->off, name, jump->next, first->depth, arg);
	fprintf(out, "  br i1 %%t%zu, label ", first->off);
	llvm_emit_branch_to(out, f, jump, target);
	fprintf(out, ", label %%L%zu\n", last->next);
	llvm_emit_safepoint(out, f, jump, target);
}

/* The half of the argument of uc_native_load2() for an LLOC, LUPV or LOAD
 * whose index fits */
static bool
llvm_load2_op(llvm_insn_t *in, uint32_t *op)
{
	uint32_t kind;

	if (!in || in->arg > UC_NATIVE_LOAD2_INDEX)
		return false;

	switch (in->insn) {
	case I_LLOC:
		kind = UC_NATIVE_LOAD2_LLOC;
		break;

	case I_LUPV:
		kind = UC_NATIVE_LOAD2_LUPV;
		break;

	case I_LOAD:
		kind = UC_NATIVE_LOAD2_CONST;
		break;

	default:
		return false;
	}

	*op = in->arg | (kind << UC_NATIVE_LOAD2_KIND_SHIFT);

	return true;
}

/* LLOC, then CALL without spread arguments, then POP or not */
static size_t
llvm_emit_lcall(FILE *out, llvm_func_t *f, size_t i, const char *prefix)
{
	llvm_insn_t *in = &f->insns.entries[i], *call = llvm_fusable(f, i, 1), *pop;
	uint32_t arg;

	if (!llvm_insn_is(call, I_CALL, I_CALL) || call->tail ||
	    (call->arg & UC_NATIVE_LCALL_SLOT) || in->arg > (UC_NATIVE_LCALL_SLOT >> 16))
		return 0;

	arg = call->arg | (in->arg << 16);
	pop = llvm_fusable(f, i, 2);

	if (!llvm_insn_is(pop, I_POP, I_POP)) {
		llvm_emit_fused_call(out, f, i, call, call, arg, "lcall", true, prefix);

		return 2;
	}

	llvm_emit_fused_call(out, f, i, call, pop, arg, "lcallpop", true, prefix);

	return 3;
}

/* Emits the instruction at `i` together with the ones after it as one
 * helper call where a helper for the sequence exists, and returns the
 * number of instructions it covers, or 0. */
static size_t
llvm_emit_fused(FILE *out, llvm_func_t *f, size_t i, const char *prefix)
{
	llvm_insn_t *in = &f->insns.entries[i], *next = llvm_fusable(f, i, 1), *jump, *pop;
	uint32_t a, b;
	size_t n;

	if ((n = llvm_emit_getk(out, f, i)) != 0)
		return n;

	switch (in->insn) {
	case I_POP:
		for (n = 1; llvm_insn_is(llvm_fusable(f, i, n), I_POP, I_POP); n++)
			;

		if (n < 2)
			return 0;

		llvm_emit_fused_call(out, f, i, in, &f->insns.entries[i + n - 1], n, "popn", false, prefix);

		return n;

	case I_CALL:
		if (in->tail || !llvm_insn_is(next, I_POP, I_POP))
			return 0;

		llvm_emit_fused_call(out, f, i, in, next, in->arg, "callpop", true, prefix);

		return 2;

	case I_SLOC:
		if (!llvm_insn_is(next, I_POP, I_POP))
			return 0;

		llvm_emit_fused_call(out, f, i, in, next, in->arg, "slocpop", false, prefix);

		return 2;

	case I_LLOC:
		if ((n = llvm_emit_lcall(out, f, i, prefix)) != 0)
			return n;

		/* fall through */

	case I_LUPV:
	case I_LOAD:
		if (!llvm_load2_op(in, &a) || !llvm_load2_op(next, &b))
			return 0;

		llvm_emit_fused_call(out, f, i, in, next, a | (b << 16), "load2", false, prefix);

		return 2;

	case I_LNULL:
		jump = llvm_fusable(f, i, 2);

		if (!llvm_insn_is(next, I_EQS, I_NES) || !llvm_insn_is(jump, I_JMPZ, I_JMPZ))
			return 0;

		llvm_emit_fused_jump(out, f, i, jump, jump, next->insn == I_NES, "jnull");

		return 3;

	case I_NOT:
		if (!llvm_insn_is(next, I_JMPZ, I_JMPZ))
			return 0;

		llvm_emit_fused_jump(out, f, i, next, next, 0, "notjmpz");

		return 2;

	case I_COPY:
		pop = llvm_fusable(f, i, 2);

		if (!llvm_insn_is(next, I_JMPZ, I_JMPZ) || in->arg >= UC_NATIVE_COPYJMPZ_POP)
			return 0;

		if (!llvm_insn_is(pop, I_POP, I_POP)) {
			llvm_emit_fused_jump(out, f, i, next, next, in->arg, "copyjmpz");

			return 2;
		}

		llvm_emit_fused_jump(out, f, i, next, pop, in->arg | UC_NATIVE_COPYJMPZ_POP, "copyjmpz");

		return 3;

	default:
		return 0;
	}
}

/* Whether a closure or an export can capture a slot of the function */
static bool
llvm_func_captures(llvm_func_t *f)
{
	uc_vector_foreach(&f->insns, in)
		if (in->insn == I_CLFN || in->insn == I_ARFN || in->insn == I_EXPORT)
			return true;

	return false;
}

static void
llvm_emit_function(FILE *out, llvm_func_t *f, size_t unit)
{
	uc_ehranges_t *ranges = &f->fn->chunk.ehranges;
	const char *cells = "@uc_native_nocells";
	char prefix[48];
	llvm_insn_t *in;

	snprintf(prefix, sizeof(prefix), "uc_u%zu_f%zu", unit, f->id);

	uc_vector_foreach(&f->insns, x) {
		if (!x->nextra || x->depth < 0)
			continue;

		fprintf(out, "@%s_x%zu = private unnamed_addr constant [%zu x i32] [", prefix, x->off, x->nextra);

		for (size_t i = 0; i < x->nextra; i++)
			fprintf(out, "%si32 %d", i ? ", " : "", (int32_t)f->extra.entries[x->extra + i]);

		fputs("]\n", out);
	}

	fprintf(out, "define internal void @%s(ptr %%vm, ptr %%frame) {\n", prefix);
	fprintf(out, "entry:\n");
	fprintf(out, "  %%slots = alloca [%zu x ptr]\n", f->nslots);
	fprintf(out, "  store [%zu x ptr] zeroinitializer, ptr %%slots\n", f->nslots);

	if (llvm_func_captures(f) || f->nslots > UC_NATIVE_NOCELLS) {
		fprintf(out, "  %%cells = alloca [%zu x ptr]\n", f->nslots);
		fprintf(out, "  store [%zu x ptr] zeroinitializer, ptr %%cells\n", f->nslots);
		cells = "%cells";
	}

	fprintf(out, "  call void @uc_native_enter(ptr %%frame, ptr %%slots, ptr %s, i32 %zu)\n",
	        cells, f->nslots);
	fprintf(out, "  br label %%L0\n");

	for (size_t i = 0, n; i < f->insns.count; i += n) {
		in = &f->insns.entries[i];
		n = 1;

		if (in->depth < 0)
			continue;

		n = llvm_emit_fused(out, f, i, prefix);

		if (!n) {
			llvm_emit_insn(out, f, in, prefix);
			n = 1;
		}
	}

	for (size_t i = 0; i < ranges->count; i++) {
		if (llvm_insn_index(f, ranges->entries[i].target) < 0 ||
		    f->insns.entries[llvm_insn_index(f, ranges->entries[i].target)].depth < 0)
			continue;

		fprintf(out, "H%zu:\n", i);
		fprintf(out, "  %%h%zu = call i1 @uc_native_catch(ptr %%frame, i32 %zu)\n",
		        i, ranges->entries[i].slot);
		fprintf(out, "  br i1 %%h%zu, label %%L%zu, label %%leave\n", i, ranges->entries[i].target);
	}

	fprintf(out, "leave:\n");
	fprintf(out, "  call void @uc_native_leave(ptr %%frame)\n");
	fprintf(out, "  ret void\n}\n\n");
}

static void
llvm_emit_declarations(FILE *out)
{
	char name[16];

	fprintf(out, "@uc_native_nocells = external constant [%d x ptr]\n", UC_NATIVE_NOCELLS);
	fputs("declare void @uc_native_enter(ptr, ptr, ptr, i32)\n"
	      "declare void @uc_native_leave(ptr)\n"
	      "declare zeroext i1 @uc_native_catch(ptr, i32)\n"
	      "declare zeroext i1 @uc_native_safepoint(ptr, i32)\n"
	      "declare zeroext i1 @uc_native_tailcall(ptr, i32, i32, i32, ptr)\n"
	      "declare i32 @uc_native_main(i32, ptr, ptr)\n"
	      "declare zeroext i1 @uc_native_getk(ptr, i32, i32, i32, i32)\n"
	      "declare void @uc_native_popn(ptr, i32, i32, i32, ptr)\n"
	      "declare zeroext i1 @uc_native_callpop(ptr, i32, i32, i32, ptr)\n"
	      "declare void @uc_native_slocpop(ptr, i32, i32, i32, ptr)\n"
	      "declare zeroext i1 @uc_native_lcall(ptr, i32, i32, i32, ptr)\n"
	      "declare zeroext i1 @uc_native_lcallpop(ptr, i32, i32, i32, ptr)\n"
	      "declare zeroext i1 @uc_native_copyjmpz(ptr, i32, i32, i32, ptr)\n"
	      "declare void @uc_native_load2(ptr, i32, i32, i32, ptr)\n"
	      "declare zeroext i1 @uc_native_jnull(ptr, i32, i32, i32, ptr)\n"
	      "declare zeroext i1 @uc_native_notjmpz(ptr, i32, i32, i32, ptr)\n", out);

	for (size_t i = 0; i < __I_MAX; i++) {
		llvm_insn_t in = { .insn = i };

		if (i == I_NOOP || i == I_JMP)
			continue;

		llvm_lower_name(name, sizeof(name), llvm_insn_names[i]);

		fprintf(out, "declare %s @uc_native_%s(ptr, i32, i32, i32, ptr)\n",
		        (i == I_RETURN || !(llvm_insn_flags(&in) & LLVM_INSN_FALLIBLE)) ? "void" : "zeroext i1",
		        name);
	}

	fputs("\n%uc_const = type { i64, i32, i32, i32, i32 }\n"
	      "%uc_fn = type { i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32 }\n"
	      "%uc_src = type { i32, i32, i32, i32, i32, i32, i32, i32, i32 }\n"
	      "%uc_unit = type { ptr, ptr, ptr, ptr, ptr, ptr, ptr, i32, i32, i32, i32, i32, i32 }\n"
	      "%uc_prog = type { ptr, ptr, i32, i32, i32, i32 }\n\n", out);
}

static void
llvm_emit_value(FILE *out, llvm_strings_t *strings, uc_value_t *v)
{
	union { double d; uint64_t u; } bits;

	switch (ucv_type(v)) {
	case UC_STRING:
		fprintf(out, "%%uc_const { i64 0, i32 %u, i32 %d, i32 %zu, i32 0 }",
		        llvm_value_string_add(strings, v), UC_NATIVE_CONST_STRING,
		        ucv_string_length(v));
		break;

	case UC_DOUBLE:
		bits.d = ucv_double_get(v);
		fprintf(out, "%%uc_const { i64 %" PRIu64 ", i32 %u, i32 %d, i32 0, i32 0 }",
		        bits.u, UC_NATIVE_NONE, UC_NATIVE_CONST_DOUBLE);
		break;

	case UC_INTEGER:
		if (ucv_int64_get(v) < 0)
			fprintf(out, "%%uc_const { i64 %" PRId64 ", i32 %u, i32 %d, i32 1, i32 0 }",
			        ucv_int64_get(v), UC_NATIVE_NONE, UC_NATIVE_CONST_INT);
		else
			fprintf(out, "%%uc_const { i64 %" PRIu64 ", i32 %u, i32 %d, i32 0, i32 0 }",
			        ucv_uint64_get(v), UC_NATIVE_NONE, UC_NATIVE_CONST_INT);
		break;

	default:
		fprintf(out, "%%uc_const { i64 0, i32 %u, i32 %d, i32 0, i32 0 }",
		        UC_NATIVE_NONE, UC_NATIVE_CONST_NULL);
		break;
	}
}

static void
llvm_emit_words(FILE *out, const char *name, size_t unit, llvm_words_t *words)
{
	if (!words->count)
		return;

	fprintf(out, "@uc_u%zu_%s = private unnamed_addr constant [%zu x i32] [",
	        unit, name, words->count);

	for (size_t i = 0; i < words->count; i++)
		fprintf(out, "%si32 %u", i ? ", " : "", words->entries[i]);

	fputs("]\n", out);
}

static void
llvm_emit_bytes_array(FILE *out, const char *name, size_t unit, llvm_bytes_t *bytes)
{
	if (!bytes->count)
		return;

	fprintf(out, "@uc_u%zu_%s = private unnamed_addr constant [%zu x i8] [",
	        unit, name, bytes->count);

	for (size_t i = 0; i < bytes->count; i++)
		fprintf(out, "%si8 %u", i ? ", " : "", bytes->entries[i]);

	fputs("]\n", out);
}

static void
llvm_emit_array_ref(FILE *out, const char *name, size_t unit, size_t count)
{
	if (count)
		fprintf(out, "ptr @uc_u%zu_%s, ", unit, name);
	else
		fputs("ptr null, ", out);
}

static void
llvm_emit_constants(FILE *out, uc_program_t *prog, llvm_tables_t *t)
{
	size_t n = prog->constants.isize;
	uc_value_t *v;

	fprintf(out, "@uc_u%zu_consts = private constant [%zu x %%uc_const] [", t->unit, n);

	for (size_t i = 0; i < n; i++) {
		v = uc_program_get_constant(prog, i);
		fputs(i ? ", " : "", out);
		llvm_emit_value(out, t->strings, v);
		ucv_put(v);
	}

	fputs("]\n", out);
}

static bool
llvm_variable_wanted(uc_function_t *fn, uc_varrange_t *var, bool debug)
{
	return debug || (var->slot >= 1 && var->slot <= fn->nargs);
}

static uint32_t
llvm_function_flags(uc_function_t *fn)
{
	return (fn->arrow ? UC_NATIVE_FN_ARROW : 0) |
	       (fn->vararg ? UC_NATIVE_FN_VARARG : 0) |
	       (fn->strict ? UC_NATIVE_FN_STRICT : 0) |
	       (fn->module ? UC_NATIVE_FN_MODULE : 0);
}

static void
llvm_function_offsets(llvm_tables_t *t, uc_chunk_t *chunk, uc_native_function_t *fd)
{
	fd->offsets = t->bytes.count;
	fd->noffsets = chunk->debuginfo.offsets.count;

	uc_vector_foreach(&chunk->debuginfo.offsets, o) {
		uc_vector_push(&t->bytes, o->bytes);
		uc_vector_push(&t->bytes, o->insns);
	}
}

/* Adds the variables that the function keeps and the names they refer to,
 * renumbered in the order of their first use. */
static void
llvm_function_variables(llvm_tables_t *t, uc_function_t *fn, uc_native_function_t *fd)
{
	uc_chunk_t *chunk = &fn->chunk;
	size_t nnames = chunk->debuginfo.varnames.isize, i;
	uint32_t *map = xalloc((nnames ? nnames : 1) * sizeof(*map));
	uint32_t *names;
	uc_value_t *v;

	for (i = 0; i < nnames; i++)
		map[i] = UC_NATIVE_NONE;

	fd->variables = t->words.count;

	uc_vector_foreach(&chunk->debuginfo.variables, var) {
		if (!llvm_variable_wanted(fn, var, t->debug))
			continue;

		if (var->nameidx < nnames && map[var->nameidx] == UC_NATIVE_NONE)
			map[var->nameidx] = fd->nvarnames++;

		uc_vector_push(&t->words, var->from);
		uc_vector_push(&t->words, var->to);
		uc_vector_push(&t->words, (var->slot >= (size_t)-1 / 2)
			? (uint32_t)(var->slot - (size_t)-1 / 2) | UC_NATIVE_VAR_UPVAL
			: (uint32_t)var->slot);
		uc_vector_push(&t->words, var->nameidx < nnames ? map[var->nameidx] : UC_NATIVE_NONE);
		fd->nvariables++;
	}

	fd->varnames = t->words.count;
	names = uc_vector_extend(&t->words, fd->nvarnames);
	t->words.count += fd->nvarnames;

	for (i = 0; i < nnames; i++) {
		if (map[i] == UC_NATIVE_NONE)
			continue;

		v = uc_vallist_get(&chunk->debuginfo.varnames, i);
		names[map[i]] = llvm_value_string_add(t->strings, v);
		ucv_put(v);
	}

	free(map);
}

static void
llvm_emit_function_table(FILE *out, llvm_func_t *funcs, size_t count, llvm_tables_t *t)
{
	uc_native_function_t fd;
	uc_function_t *fn;
	size_t i;

	fprintf(out, "@uc_u%zu_fns = private constant [%zu x %%uc_fn] [", t->unit, count);

	for (i = 0; i < count; i++) {
		fn = funcs[i].fn;
		fd = (uc_native_function_t){
			.name = llvm_cstring_add(t->strings, fn->name),
			.nargs = fn->nargs,
			.nupvals = fn->nupvals,
			.flags = llvm_function_flags(fn),
			.srcidx = fn->srcidx,
			.srcpos = t->debug ? fn->srcpos : 0
		};

		if (t->debug)
			llvm_function_offsets(t, &fn->chunk, &fd);

		llvm_function_variables(t, fn, &fd);

		fprintf(out, "%s%%uc_fn { i32 %u, i32 %u, i32 %u, i32 %u, i32 %u, i32 %u, "
		        "i32 %u, i32 %u, i32 %u, i32 %u, i32 %u, i32 %u }",
		        i ? ",\n  " : "\n  ", fd.name, fd.offsets, fd.variables, fd.varnames,
		        fd.noffsets, fd.nvariables, fd.nvarnames, fd.nargs, fd.nupvals,
		        fd.flags, fd.srcidx, fd.srcpos);
	}

	fputs("\n]\n", out);
	fprintf(out, "@uc_u%zu_entries = private constant [%zu x ptr] [", t->unit, count);

	for (i = 0; i < count; i++)
		fprintf(out, "%sptr @uc_u%zu_f%zu", i ? ", " : "", t->unit, funcs[i].id);

	fputs("]\n", out);
}

/* Returns the number of exports of all sources. */
static size_t
llvm_emit_source_table(FILE *out, uc_program_t *prog, llvm_tables_t *t)
{
	size_t n = prog->sources.count, nexports = 0, len, i, j;
	uc_native_source_t *descs = xcalloc(n ? n : 1, sizeof(*descs));
	uc_source_t *src;
	char *content;

	for (i = 0; i < n; i++) {
		src = prog->sources.entries[i];
		content = t->debug ? uc_source_read_content(src, &len) : NULL;

		descs[i] = (uc_native_source_t){
			.filename = llvm_cstring_add(t->strings, src->filename),
			.runpath = llvm_cstring_add(t->strings, src->runpath),
			.text = content ? llvm_string_add(t->strings, content, len) : UC_NATIVE_NONE,
			.textlen = content ? len : 0,
			.exports = nexports,
			.nexports = src->exports.count
		};

		free(content);
		nexports += src->exports.count;

		if (!t->debug)
			continue;

		descs[i].lineinfo = t->bytes.count;
		descs[i].nlineinfo = src->lineinfo.count;

		for (j = 0; j < src->lineinfo.count; j++)
			uc_vector_push(&t->bytes, src->lineinfo.entries[j]);
	}

	if (nexports) {
		fprintf(out, "@uc_u%zu_exports = private constant [%zu x %%uc_const] [", t->unit, nexports);

		for (i = 0, nexports = 0; i < n; i++) {
			src = prog->sources.entries[i];

			for (j = 0; j < src->exports.count; j++, nexports++) {
				fputs(nexports ? ", " : "", out);
				llvm_emit_value(out, t->strings, src->exports.entries[j]);
			}
		}

		fputs("]\n", out);
	}

	fprintf(out, "@uc_u%zu_srcs = private constant [%zu x %%uc_src] [", t->unit, n);

	for (i = 0; i < n; i++)
		fprintf(out, "%s%%uc_src { i32 %u, i32 %u, i32 %u, i32 %u, i32 %u, i32 %u, i32 %u, "
		        "i32 %u, i32 0 }", i ? ",\n  " : "\n  ",
		        descs[i].filename, descs[i].runpath, descs[i].text, descs[i].lineinfo,
		        descs[i].exports, descs[i].textlen, descs[i].nlineinfo, descs[i].nexports);

	fputs("\n]\n", out);
	free(descs);

	return nexports;
}

static size_t
llvm_function_count(uc_program_t *prog)
{
	size_t count = 0;

	uc_program_function_foreach(prog, fn)
		count++;

	return count;
}

static bool
llvm_emit_unit(FILE *out, llvm_unit_t *u, llvm_tables_t *t, char **errp)
{
	uc_program_t *prog = u->program;
	size_t count = llvm_function_count(prog), i = 0;
	llvm_func_t *funcs;
	bool ok = true;

	funcs = xcalloc(count ? count : 1, sizeof(*funcs));

	uc_program_function_foreach(prog, fn) {
		funcs[i].program = prog;
		funcs[i].fn = fn;
		funcs[i].id = i + 1;
		i++;
	}

	for (i = 0; ok && i < count; i++)
		ok = llvm_decode(&funcs[i], errp) && llvm_analyze(&funcs[i], errp);

	for (i = 0; ok && i < count; i++)
		llvm_emit_function(out, &funcs[i], t->unit);

	if (ok) {
		llvm_emit_constants(out, prog, t);
		llvm_emit_function_table(out, funcs, count, t);
		u->nexports = llvm_emit_source_table(out, prog, t);
		u->nwords = t->words.count;
		u->nbytes = t->bytes.count;
		llvm_emit_words(out, "words", t->unit, &t->words);
		llvm_emit_bytes_array(out, "bytes", t->unit, &t->bytes);
	}

	for (i = 0; i < count; i++) {
		uc_vector_clear(&funcs[i].insns);
		uc_vector_clear(&funcs[i].extra);
	}

	uc_vector_clear(&t->words);
	uc_vector_clear(&t->bytes);
	free(funcs);

	return ok;
}

static char *
llvm_constant_string(uc_program_t *prog, uint32_t idx)
{
	uc_value_t *v = uc_program_get_constant(prog, idx);
	char *s = (ucv_type(v) == UC_STRING) ? xstrdup(ucv_string_get(v)) : NULL;

	ucv_put(v);

	return s;
}

/* Returns the script that require() finds for module `name` through the
 * search path, or NULL if it finds a shared extension or nothing. */
static char *
llvm_resolve_module(uc_parse_config_t *config, const char *name)
{
	uc_search_path_t *search = &config->module_search_path;
	uc_stringbuf_t *buf;
	char *path = NULL;
	const char *ext;
	bool found;

	for (size_t i = 0; i < search->count; i++) {
		buf = uc_stringbuf_new();
		ext = uc_require_path_expand(search->entries[i], name, buf);
		found = ext && access(buf->buf, F_OK) == 0;

		if (found && !strcmp(ext, ".uc"))
			path = xstrdup(buf->buf);

		uc_stringbuf_free(buf);

		if (found)
			break;
	}

	return path;
}

/* Compiles the script `path` in the mode its loader uses and adds it to the
 * units of the native program. A script that fails to compile is left out, so
 * that loading it fails at runtime, as it would in the interpreter. */
static void
llvm_unit_add(llvm_units_t *units, uc_parse_config_t *config, char *path,
              const char *logical, uint32_t flags)
{
	uc_parse_config_t conf = *config;
	uc_program_t *program;
	uc_source_t *source;
	char *key, *err = NULL;

	if (!path)
		return;

	key = uc_path_normalize(logical ? logical : path);

	for (size_t i = 1; key && i < units->count; i++) {
		if (!strcmp(units->entries[i].path, key) && units->entries[i].flags == flags) {
			free(key);
			key = NULL;
		}
	}

	source = key ? uc_source_new_file(path) : NULL;
	free(path);

	if (!source) {
		free(key);

		return;
	}

	conf.raw_mode = !(flags & UC_NATIVE_UNIT_TEMPLATE);
	conf.compile_module = flags & UC_NATIVE_UNIT_MODULE;
	program = uc_compile(&conf, source, &err);

	uc_source_put(source);

	/* a script that fails to compile is kept with its error, which loading it
	 * raises at runtime, as the interpreter does */
	uc_vector_push(units, {
		.path = key,
		.flags = flags,
		.relocated = logical != NULL,
		.program = program,
		.error = program ? NULL : err
	});

	if (program)
		free(err);
}

/* Adds the file `path`, or the scripts below the directory `path`, under the
 * logical path `logical` if given. */
static void
llvm_unit_embed(llvm_units_t *units, uc_parse_config_t *config,
                const char *path, const char *logical, bool template)
{
	uint32_t flags = template ? UC_NATIVE_UNIT_TEMPLATE : 0;
	char *child, *child_logical = NULL;
	struct dirent *e;
	struct stat st;
	const char *ext;
	DIR *d;

	if (stat(path, &st))
		return;

	if (!S_ISDIR(st.st_mode)) {
		llvm_unit_add(units, config, xstrdup(path), logical, flags);

		return;
	}

	d = opendir(path);

	while (d && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;

		xasprintf(&child, "%s/%s", path, e->d_name);

		if (logical)
			xasprintf(&child_logical, "%s/%s", logical, e->d_name);

		ext = strrchr(e->d_name, '.');

		if (!stat(child, &st) && S_ISDIR(st.st_mode))
			llvm_unit_embed(units, config, child, child_logical, template);
		else if (ext && (!strcmp(ext, ".uc") || (template && !strcmp(ext, ".ut"))))
			llvm_unit_add(units, config, xstrdup(child), child_logical, flags);

		free(child);
		free(child_logical);
		child_logical = NULL;
	}

	if (d)
		closedir(d);
}

/* Returns the file that include() resolves `literal` to from function `fn` of
 * the unit, and in `logical` the path it resolves to at runtime if the unit is
 * embedded under a logical path. */
static char *
llvm_unit_include(llvm_unit_t *unit, uc_function_t *fn, const char *literal, char **logical)
{
	uc_source_t *source = uc_program_function_source(fn);
	const char *slash;

	*logical = NULL;

	if (unit->relocated && *literal != '/' &&
	    source == unit->program->sources.entries[0] &&
	    (slash = strrchr(unit->path, '/')) != NULL)
		xasprintf(logical, "%.*s/%s", (int)(slash - unit->path), unit->path, literal);

	return uc_include_path(source->runpath, literal);
}

/* Adds the scripts that the unit loads with literal arguments: a require(),
 * include() or render() call whose first argument is a string constant, and
 * a dynamically linked import. */
static void
llvm_unit_discover(llvm_units_t *units, size_t idx, uc_parse_config_t *config)
{
	uc_program_t *prog = units->entries[idx].program;
	uint32_t flags = units->entries[idx].flags;
	char *loader, *literal, *path, *logical, *err;
	llvm_insn_t *a, *b;
	llvm_func_t f;

	if (!prog)
		return;

	uc_program_function_foreach(prog, fn) {
		memset(&f, 0, sizeof(f));
		f.program = prog;
		f.fn = fn;
		err = NULL;

		if (!llvm_decode(&f, &err)) {
			free(err);
			uc_vector_clear(&f.insns);
			uc_vector_clear(&f.extra);
			continue;
		}

		for (size_t i = 0; i + 1 < f.insns.count; i++) {
			a = &f.insns.entries[i];
			b = &f.insns.entries[i + 1];

			if (a->insn == I_LOAD && b->insn == I_DYNLOAD) {
				literal = llvm_constant_string(prog, a->arg);

				if (literal)
					llvm_unit_add(units, config, llvm_resolve_module(config, literal),
					              NULL, UC_NATIVE_UNIT_MODULE);

				free(literal);
				continue;
			}

			if (a->insn != I_LVAR || b->insn != I_LOAD)
				continue;

			loader = llvm_constant_string(prog, a->arg);
			literal = llvm_constant_string(prog, b->arg);

			if (loader && literal && !strcmp(loader, "require")) {
				llvm_unit_add(units, config, llvm_resolve_module(config, literal), NULL, 0);
			}
			else if (loader && literal &&
			         (!strcmp(loader, "include") || !strcmp(loader, "render"))) {
				/* the unit list may have grown, so look the unit up again */
				path = llvm_unit_include(&units->entries[idx], fn, literal, &logical);
				llvm_unit_add(units, config, path, logical,
				              !strcmp(loader, "render") ? UC_NATIVE_UNIT_TEMPLATE
				                                        : (flags & UC_NATIVE_UNIT_TEMPLATE));
				free(logical);
			}

			free(loader);
			free(literal);
		}

		uc_vector_clear(&f.insns);
		uc_vector_clear(&f.extra);
	}
}

/* Writes the map file: the programs of all units with debug information, each
 * under the path that stack traces of the native program name it by. */
static bool
llvm_write_map(llvm_units_t *units, const uc_llvm_options_t *opts, char **errp)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *map, *mem;

	map = fopen(opts->map, "wb");

	if (!map) {
		xasprintf(errp, "Unable to open map file '%s': %m\n", opts->map);

		return false;
	}

	fputs(UC_NATIVE_MAP_MAGIC, map);

	for (size_t i = 0; i < units->count; i++) {
		if (!units->entries[i].program)
			continue;

		mem = open_memstream(&buf, &len);

		if (!mem)
			break;

		uc_program_write(units->entries[i].program, mem, true);
		fclose(mem);

		fprintf(map, "%s\n%zu\n",
		        units->entries[i].path ? units->entries[i].path
		                               : (opts->script_name ? opts->script_name : ""),
		        len);
		fwrite(buf, 1, len, map);
		free(buf);
		buf = NULL;
	}

	if (fclose(map)) {
		xasprintf(errp, "Unable to write map file '%s': %m\n", opts->map);

		return false;
	}

	return true;
}

bool
uc_program_write_llvm(uc_program_t *program, FILE *out,
                      const uc_llvm_options_t *opts, char **errp)
{
	llvm_strings_t strings = { .data = uc_stringbuf_new(), .index = ucv_object_new(NULL) };
	llvm_units_t units = { 0 };
	llvm_tables_t tables;
	llvm_unit_t *u;
	bool ok = true;
	size_t i;

	uc_vector_push(&units, {
		.program = uc_program_get(program),
		.flags = (opts->config && !opts->config->raw_mode) ? UC_NATIVE_UNIT_TEMPLATE : 0
	});

	for (i = 0; opts->embeds && i < opts->embeds->count; i++)
		llvm_unit_embed(&units, opts->config, opts->embeds->entries[i].path,
		                opts->embeds->entries[i].logical, opts->embeds->entries[i].template);

	for (i = 0; opts->config && i < units.count; i++)
		llvm_unit_discover(&units, i, opts->config);

	if (opts->triple)
		fprintf(out, "target triple = \"%s\"\n\n", opts->triple);

	llvm_emit_declarations(out);

	for (i = 0; ok && i < units.count; i++) {
		u = &units.entries[i];
		tables = (llvm_tables_t){ .strings = &strings, .unit = i, .debug = opts->debug };

		if (u->program)
			ok = llvm_emit_unit(out, u, &tables, errp);
	}

	if (ok) {
		fprintf(out, "@uc_units = private constant [%zu x %%uc_unit] [", units.count);

		for (i = 0; i < units.count; i++) {
			u = &units.entries[i];
			fputs(i ? ",\n  %uc_unit { " : "\n  %uc_unit { ", out);

			if (u->program) {
				fprintf(out, "ptr @uc_u%zu_srcs, ptr @uc_u%zu_consts, ptr @uc_u%zu_fns, "
				        "ptr @uc_u%zu_entries, ", i, i, i, i);
				llvm_emit_array_ref(out, "exports", i, u->nexports);
				llvm_emit_array_ref(out, "words", i, u->nwords);
				llvm_emit_array_ref(out, "bytes", i, u->nbytes);
			}
			else {
				fputs("ptr null, ptr null, ptr null, ptr null, ptr null, ptr null, ptr null, ", out);
			}

			fprintf(out, "i32 %u, i32 %u, i32 %zu, i32 %zu, i32 %zu, i32 %u }",
			        llvm_cstring_add(&strings, u->path), llvm_cstring_add(&strings, u->error),
			        u->program ? u->program->sources.count : 0,
			        u->program ? u->program->constants.isize : 0,
			        u->program ? llvm_function_count(u->program) : 0, u->flags);
		}

		fputs("\n]\n", out);
		fprintf(out, "@uc_program = private constant %%uc_prog { ptr @uc_strings, ptr @uc_units, "
		        "i32 %u, i32 %d, i32 %zu, i32 0 }\n\n",
		        llvm_cstring_add(&strings, opts->script_name), UC_NATIVE_ABI_VERSION, units.count);
		llvm_emit_strings(out, &strings);
		fputs("define i32 @main(i32 %argc, ptr %argv) {\n"
		      "  %r = call i32 @uc_native_main(i32 %argc, ptr %argv, ptr @uc_program)\n"
		      "  ret i32 %r\n}\n", out);
	}

	if (ok && opts->map)
		ok = llvm_write_map(&units, opts, errp);

	for (i = 0; i < units.count; i++) {
		free(units.entries[i].path);
		free(units.entries[i].error);

		if (units.entries[i].program)
			uc_program_put(units.entries[i].program);
	}

	uc_vector_clear(&units);
	uc_stringbuf_free(strings.data);
	ucv_put(strings.index);

	return ok;
}

/*
 * Copyright (C) 2026 Jo-Philipp Wich <jo@mein.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ANY IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * ---
 *
 * ucode/utpl syntax highlighting and ANSI source rendering, ported from the
 * pre-protocol interactive debugger (formerly lib/debug.c's
 * highlight_rules[]/compile_patterns()/print_source_location()) so it can
 * be reused by any client speaking the line-based debug protocol - or
 * anything else that wants to print ucode source with the same styling.
 *
 * This module is intentionally standalone: no ucode headers, no protocol
 * knowledge, just POSIX regex + stdio. A caller supplies already-split
 * source lines and, optionally, a single line/column range to shade as the
 * "current statement" - the multi-range/ellipsis-gap layout the original
 * server-side renderer supported for very large statements is not ported,
 * since every client of the debug protocol only ever receives one
 * contiguous range at a time (see SOURCE_RANGE in lib/debug_proto.h).
 */

#ifndef _DEBUG_HIGHLIGHT_H
#define _DEBUG_HIGHLIGHT_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* A statement span to shade, in 1-based line numbers and 0-based byte
 * columns within those lines (matching the debug protocol's "col" fields).
 * Set from_line to 0 for "no highlight". `have_ip` additionally underlines
 * the single character at {ip_line, ip_col} - the exact current
 * instruction position, as opposed to {from,to} which mark the enclosing
 * statement's extent. */
typedef struct {
	size_t from_line, from_col;
	size_t to_line, to_col;
	bool have_ip;
	size_t ip_line, ip_col;
} debug_highlight_span_t;

/* Compile the highlight regexes once; safe to call repeatedly. Returns
 * false (and prints a diagnostic to stderr) on a regex compile error, in
 * which case debug_highlight_print_source() below still works, just
 * without coloring. */
bool debug_highlight_init(void);

/* Print source lines [from, to] (1-based, inclusive, clamped to
 * [1, nlines]) from the `nlines`-element `lines` array (as produced by
 * splitting raw source text on '\n', with no trailing newlines) to `out`,
 * applying ucode/utpl syntax highlighting and, if `hl` is non-NULL, shading
 * the statement range it describes. Every printed line is prefixed with
 * `left_pad` blank columns plus a right-aligned line number gutter.
 * `columns` is the terminal width to wrap/pad to (pass 0 for "don't know",
 * which disables truncation and trailing-space padding). */
void debug_highlight_print_source(FILE *out, char **lines, size_t nlines,
                                   size_t from, size_t to,
                                   const debug_highlight_span_t *hl,
                                   size_t left_pad, size_t columns);

/* A single [from, to] (1-based, inclusive) line range, for the multi-range
 * form below. */
typedef struct {
	size_t from, to;
} debug_highlight_range_t;

/* Like debug_highlight_print_source(), but for up to `nranges` disjoint
 * ranges at once - lines that fall in a gap between two ranges are skipped
 * with a single "   … " ellipsis marker rather than printed, matching the
 * original format_context_statement()'s handling of a statement too long
 * to show in full: a window of context at its start, a gap, and a window
 * around the current instruction/its end. Ranges need not be sorted; a
 * {0, 0} entry is ignored (so callers can pass a fixed-size array without
 * always filling every slot). */
void debug_highlight_print_source_ranges(FILE *out, char **lines, size_t nlines,
                                          size_t nranges,
                                          const debug_highlight_range_t *ranges,
                                          const debug_highlight_span_t *hl,
                                          size_t left_pad, size_t columns);

/* Print a full-width "[bracket] rest " status bar to `out` on a solid
 * background, ported from the original format_context_header_backtrace()/
 * format_context_header_callframe() (the bar shown above a paused
 * location's or a backtrace frame's source snippet) - `bracket` is the
 * source file (or "C" for a native frame), `rest` the call breadcrumb or
 * frame signature. Long `rest` values are elided from the front (ellipsis
 * first, keeping the tail - the original's choice, since the innermost/
 * current part of a chain matters more than the outermost when both don't
 * fit) if `columns` is nonzero; pass 0 to disable width awareness (no
 * truncation, no trailing padding). */
void debug_highlight_print_header_bar(FILE *out, const char *bracket,
                                       const char *rest,
                                       size_t left_pad, size_t columns);

/* A single decoded bytecode instruction, as reported by the DISASSEMBLE
 * protocol response's "instructions" array, for
 * debug_highlight_print_disassembly() below. Which of the optional fields
 * are populated selects what annotation (if any) is shown after the raw
 * operand - the renderer itself has no notion of opcode names or their
 * meaning, it only reacts to which fields the caller filled in. */
typedef struct {
	size_t offset;
	const char *mnemonic;
	int format;                 /* uc_vm_insn_format[] value: 0, 1, 2, 4 or -4 */
	const unsigned char *bytes; size_t nbytes;   /* raw instruction bytes */
	int64_t operand;             /* decoded operand; sign only meaningful for format -4;
	                              * unused when format == 0 */

	bool have_constant;
	const char *constant_repr;   /* JSON text of the constant value */
	bool constant_is_string;

	const char *variable_kind;   /* "local", "upval", "global", or NULL */
	const char *variable_name;

	bool have_closure;
	const char *closure_kind;    /* "closure" or "arrow" */
	uint32_t closure_index;

	bool have_call;
	bool call_mcall;             /* method call: an implicit `this` arg follows */
	uint32_t call_nargs;
	bool call_tail;              /* tail call: I_RETURN + 0x00 marker follows the
	                              * call's operand span, the VM reuses the current
	                              * frame for the callee */

	const char *stdlib_name;     /* STCALL: the standard library function it
	                              * invokes, called without a call frame */

	bool return_tailcall;       /* this I_RETURN terminates a tail call: the 0x00
	                              * marker byte the compiler emits right after it is
	                              * consumed as part of this instruction (pure data,
	                              * never executed) */

	struct {
		int64_t slot;
		bool upval;
		const char *name;
		unsigned char bytes[4];
	} *captures; size_t ncaptures;

	struct {
		uint16_t slot;
		unsigned char bytes[2];
	} *unpacks; size_t nunpacks;
} debug_disasm_insn_t;

/* Print a disassembly listing exactly as the pre-protocol interactive
 * debugger's `disassemble` command did: address, a color-coded raw byte
 * dump (opcode byte plain, operand bytes bright magenta), the mnemonic,
 * the decoded operand, and - when the caller supplied it - a semantic
 * annotation (constant value, local/upval/global name, closure/arrow
 * index) plus extra indented lines for closure upvalue captures or call
 * argument unpacks. `columns` is the terminal width to wrap to (pass 0
 * for "don't know", which disables truncation). */
void debug_highlight_print_disassembly(FILE *out, const char *function,
                                        const debug_disasm_insn_t *insns,
                                        size_t ninsns, size_t columns);

/* One entry of a VARIABLES (or a BACKTRACE frame's inline "variables")
 * protocol response, for debug_highlight_print_variables() below. `kind`
 * is one of "this", "local", "internal" (a synthetic, parenthesized slot
 * name such as a `for`-loop's hidden iterator) or "upvalue". `shadowed`
 * marks a same-named, less-nested declaration that a more-nested one
 * currently hides - still a real, live slot, just not what plain script
 * code resolves this name to right now. */
typedef struct {
	const char *name;
	const char *kind;
	const char *value_repr;
	bool shadowed;
} debug_variable_t;

/* Print a "name : value" variable listing exactly as the pre-protocol
 * interactive debugger's print_variables() did: the name in a fixed
 * 16-column field (tail-truncated with an ellipsis if longer), styled
 * bold cyan for an upvalue or faint white for "this"/an internal slot
 * (plain otherwise), a faint " : " separator, then the value - styled
 * bold red instead of truncated when it is the literal sentinel
 * "<out of range>". A shadowed entry (see above - not part of the
 * original pre-protocol listing, which never showed more than one
 * variable per name to begin with) is rendered faint throughout with a
 * trailing "(shadowed)" marker. Every line is prefixed with `indent`.
 * `columns` is the terminal width the value is truncated to fit (pass 0
 * for "don't know", which disables value truncation only - the name
 * field is always truncated to 16 regardless). */
void debug_highlight_print_variables(FILE *out, const debug_variable_t *vars,
                                      size_t nvars, const char *indent,
                                      size_t columns);

#endif

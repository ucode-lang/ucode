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
#include <errno.h>

#include "ucode/internal/compiler.h"
#include "ucode/internal/chunk.h"
#include "ucode/internal/vm.h" /* ISA: I_*, uc_vm_insn_t */
#include "ucode/internal/program.h"
#include "ucode/internal/lib.h" /* uc_error_context_format() */

#ifndef NO_COMPILE

static void uc_compiler_compile_unary(uc_compiler_t *compiler);
static void uc_compiler_compile_binary(uc_compiler_t *compiler);
static void uc_compiler_compile_delete(uc_compiler_t *compiler);
static void uc_compiler_compile_importcall(uc_compiler_t *compiler);
static void uc_compiler_compile_paren(uc_compiler_t *compiler);
static void uc_compiler_compile_call(uc_compiler_t *compiler);
static void uc_compiler_compile_post_inc(uc_compiler_t *compiler);
static void uc_compiler_compile_constant(uc_compiler_t *compiler);
static void uc_compiler_compile_template(uc_compiler_t *compiler);
static void uc_compiler_compile_comma(uc_compiler_t *compiler);
static void uc_compiler_compile_labelexpr(uc_compiler_t *compiler);
static void uc_compiler_compile_funcexpr(uc_compiler_t *compiler);
static void uc_compiler_compile_and(uc_compiler_t *compiler);
static void uc_compiler_compile_or(uc_compiler_t *compiler);
static void uc_compiler_compile_nullish(uc_compiler_t *compiler);
static void uc_compiler_compile_dot(uc_compiler_t *compiler);
static void uc_compiler_compile_subscript(uc_compiler_t *compiler);
static void uc_compiler_compile_ternary(uc_compiler_t *compiler);
static void uc_compiler_compile_array(uc_compiler_t *compiler);
static void uc_compiler_compile_object(uc_compiler_t *compiler);

static uc_tokentype_t uc_compiler_compile_declaration(uc_compiler_t *compiler);
static uc_tokentype_t uc_compiler_compile_statement(uc_compiler_t *compiler);
static uc_tokentype_t uc_compiler_compile_expstmt(uc_compiler_t *compiler);

static uc_parse_rule_t
uc_compiler_parse_rules[TK_ERROR + 1] = {
	[TK_LPAREN]		= { uc_compiler_compile_paren, uc_compiler_compile_call, P_CALL },
	[TK_QLPAREN]	= { NULL, uc_compiler_compile_call, P_CALL },
	[TK_SUB]		= { uc_compiler_compile_unary, uc_compiler_compile_binary, P_ADD },
	[TK_ADD]		= { uc_compiler_compile_unary, uc_compiler_compile_binary, P_ADD },
	[TK_COMPL]		= { uc_compiler_compile_unary, NULL, P_UNARY },
	[TK_NOT]		= { uc_compiler_compile_unary, NULL, P_UNARY },
	[TK_DELETE]		= { uc_compiler_compile_delete, NULL, P_UNARY },
	[TK_IMPORT]		= { uc_compiler_compile_importcall, NULL, P_UNARY },
	[TK_INC]		= { uc_compiler_compile_unary, uc_compiler_compile_post_inc, P_INC },
	[TK_DEC]		= { uc_compiler_compile_unary, uc_compiler_compile_post_inc, P_INC },
	[TK_DIV]		= { NULL, uc_compiler_compile_binary, P_MUL },
	[TK_MUL]		= { NULL, uc_compiler_compile_binary, P_MUL },
	[TK_MOD]		= { NULL, uc_compiler_compile_binary, P_MUL },
	[TK_EXP]		= { NULL, uc_compiler_compile_binary, P_EXP },
	[TK_NUMBER]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_DOUBLE]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_STRING]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_TRUE]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_FALSE]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_NULL]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_THIS]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_REGEXP]		= { uc_compiler_compile_constant, NULL, P_NONE },
	[TK_TEMPLATE]	= { uc_compiler_compile_template, NULL, P_NONE },
	[TK_COMMA]		= { NULL, uc_compiler_compile_comma, P_COMMA },
	[TK_LABEL]		= { uc_compiler_compile_labelexpr, NULL, P_NONE },
	[TK_FUNC]		= { uc_compiler_compile_funcexpr, NULL, P_NONE },
	[TK_AND]		= { NULL, uc_compiler_compile_and, P_AND },
	[TK_OR]			= { NULL, uc_compiler_compile_or, P_OR },
	[TK_NULLISH]	= { NULL, uc_compiler_compile_nullish, P_OR },
	[TK_BOR]		= { NULL, uc_compiler_compile_binary, P_BOR },
	[TK_BXOR]		= { NULL, uc_compiler_compile_binary, P_BXOR },
	[TK_BAND]		= { NULL, uc_compiler_compile_binary, P_BAND },
	[TK_EQ]			= { NULL, uc_compiler_compile_binary, P_EQUAL },
	[TK_EQS]		= { NULL, uc_compiler_compile_binary, P_EQUAL },
	[TK_NE]			= { NULL, uc_compiler_compile_binary, P_EQUAL },
	[TK_NES]		= { NULL, uc_compiler_compile_binary, P_EQUAL },
	[TK_LT]			= { NULL, uc_compiler_compile_binary, P_COMPARE },
	[TK_LE]			= { NULL, uc_compiler_compile_binary, P_COMPARE },
	[TK_GT]			= { NULL, uc_compiler_compile_binary, P_COMPARE },
	[TK_GE]			= { NULL, uc_compiler_compile_binary, P_COMPARE },
	[TK_IN]			= { NULL, uc_compiler_compile_binary, P_COMPARE },
	[TK_LSHIFT]		= { NULL, uc_compiler_compile_binary, P_SHIFT },
	[TK_RSHIFT]		= { NULL, uc_compiler_compile_binary, P_SHIFT },
	[TK_DOT]		= { NULL, uc_compiler_compile_dot, P_CALL },
	[TK_QDOT]		= { NULL, uc_compiler_compile_dot, P_CALL },
	[TK_LBRACK]		= { uc_compiler_compile_array, uc_compiler_compile_subscript, P_CALL },
	[TK_QLBRACK]	= { NULL, uc_compiler_compile_subscript, P_CALL },
	[TK_QMARK]		= { NULL, uc_compiler_compile_ternary, P_TERNARY },
	[TK_LBRACE]		= { uc_compiler_compile_object, NULL, P_NONE },
};

static ssize_t
uc_compiler_declare_local(uc_compiler_t *compiler, uc_value_t *name, bool constant);

static ssize_t
uc_compiler_initialize_local(uc_compiler_t *compiler);

static bool
uc_compiler_exprstack_is(uc_compiler_t *compiler, uc_exprflag_t flag)
{
	uc_exprstack_t *expr;

	for (expr = compiler->exprstack; expr; expr = expr->parent)
		if (expr->flags & flag)
			return true;

	return false;
}

static void
uc_compiler_init(uc_compiler_t *compiler, const char *name, uc_source_t *source, size_t srcpos, uc_program_t *program, bool strict)
{
	uc_value_t *varname = ucv_string_new("(callee)");
	uc_function_t *fn;

	compiler->scope_depth = 0;
	compiler->try_depth = 0;

	compiler->program = program;
	compiler->function = uc_program_function_new(program, name, source, srcpos);

	compiler->locals.count = 0;
	compiler->locals.entries = NULL;

	compiler->upvals.count = 0;
	compiler->upvals.entries = NULL;

	compiler->patchlist = NULL;

	compiler->parent = NULL;

	compiler->current_srcpos = srcpos;
	compiler->tailcall_off = SIZE_MAX;

	fn = (uc_function_t *)compiler->function;
	fn->strict = strict;

	/* reserve stack slot 0 */
	uc_compiler_declare_local(compiler, varname, false);
	uc_compiler_initialize_local(compiler);
	ucv_put(varname);
}

static uc_chunk_t *
uc_compiler_current_chunk(uc_compiler_t *compiler)
{
	uc_function_t *fn = (uc_function_t *)compiler->function;

	return &fn->chunk;
}

static uc_source_t *
uc_compiler_current_source(uc_compiler_t *compiler)
{
	return uc_program_function_source(compiler->function);
}

__attribute__((format(printf, 3, 0))) static void
uc_compiler_syntax_error(uc_compiler_t *compiler, size_t off, const char *fmt, ...)
{
	uc_source_t *source = uc_compiler_current_source(compiler);
	uc_stringbuf_t *buf = compiler->parser->error;
	size_t line = 0, byte = 0, len = 0, startoff = 0, endoff = 0;
	va_list ap;
	char *s;

	if (compiler->parser->synchronizing)
		return;

	compiler->parser->synchronizing = true;

	if (!buf)
		buf = compiler->parser->error = xprintbuf_new();

	if (!off)
		off = uc_program_function_srcpos(compiler->function,
			uc_compiler_current_chunk(compiler)->count);

	byte = off;
	line = uc_source_get_line(source, &byte);

	va_start(ap, fmt);
	len = xvasprintf(&s, fmt, ap);
	va_end(ap);

	ucv_stringbuf_append(buf, "Syntax error: ");
	ucv_stringbuf_addstr(buf, s, len);
	ucv_stringbuf_append(buf, "\n");

	free(s);

	if (line) {
		ucv_stringbuf_append(buf, "In ");

		if (compiler->program->sources.count > 1) {
			len = strlen(source->filename);

			if (len > 48)
				ucv_stringbuf_printf(buf, "...%s", source->filename + len - 45);
			else
				ucv_stringbuf_addstr(buf, source->filename, len);

			ucv_stringbuf_append(buf, ", ");
		}

		ucv_stringbuf_printf(buf, "line %zu, byte %zu:\n", line, byte);
	}

	/* underline the innermost statement being compiled rather than pointing at
	 * the offending token alone; SIZE_MAX lies beyond the last instruction, so
	 * no closed statement can cover it and the lookup comes down to the one
	 * still open, i.e. the statement which the error aborted. Left at zero, and
	 * so ignored, when there is no such statement. */
	uc_program_function_stmt_bounds(compiler->function, SIZE_MAX, &startoff, &endoff);

	if (uc_error_context_format(buf, source, NULL, startoff, endoff, off))
		ucv_stringbuf_append(buf, "\n\n");
}

static size_t
uc_compiler_set_srcpos(uc_compiler_t *compiler, size_t srcpos)
{
	size_t delta;

	/* ensure that lines counts are strictly increasing */
	assert(srcpos == 0 || srcpos >= compiler->current_srcpos);

	delta = srcpos ? srcpos - compiler->current_srcpos : 0;
	compiler->current_srcpos += delta;

	return delta;
}

static void
uc_compiler_parse_advance(uc_compiler_t *compiler)
{
	ucv_put(compiler->parser->prev.uv);
	compiler->parser->prev = compiler->parser->curr;

	while (true) {
		uc_token_t *tok = uc_lexer_next_token(&compiler->parser->lex);

		if (tok->type == TK_COMMENT || tok->type == TK_LSTM) {
			ucv_put(tok->uv);
			continue;
		}
		else if (tok->type == TK_RSTM) {
			tok->type = TK_SCOL;
		}

		compiler->parser->curr = *tok;

		if (compiler->parser->curr.type != TK_ERROR)
			break;

		uc_compiler_syntax_error(compiler, compiler->parser->curr.pos, "%s",
			ucv_string_get(compiler->parser->curr.uv));

		ucv_put(compiler->parser->curr.uv);
		compiler->parser->curr.uv = NULL;
	}
}

static void
uc_compiler_parse_consume(uc_compiler_t *compiler, uc_tokentype_t type)
{
	if (compiler->parser->curr.type == type) {
		uc_compiler_parse_advance(compiler);

		return;
	}

	uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
		"Unexpected token\nExpecting %s", uc_tokenname(type));
}

static bool
uc_compiler_parse_check(uc_compiler_t *compiler, uc_tokentype_t type)
{
	return (compiler->parser->curr.type == type);
}

static bool
uc_compiler_parse_match(uc_compiler_t *compiler, uc_tokentype_t type)
{
	if (!uc_compiler_parse_check(compiler, type))
		return false;

	uc_compiler_parse_advance(compiler);

	return true;
}

static bool
uc_compiler_keyword_check(uc_compiler_t *compiler, const char *keyword)
{
	size_t keywordlen = strlen(keyword);

	return (compiler->parser->curr.type == TK_LABEL &&
	        ucv_string_length(compiler->parser->curr.uv) == keywordlen &&
	        strcmp(ucv_string_get(compiler->parser->curr.uv), keyword) == 0);
}

static bool
uc_compiler_keyword_match(uc_compiler_t *compiler, const char *keyword)
{
	if (!uc_compiler_keyword_check(compiler, keyword))
		return false;

	uc_compiler_parse_advance(compiler);

	return true;
}

static void
uc_compiler_keyword_consume(uc_compiler_t *compiler, const char *keyword)
{
	if (uc_compiler_keyword_check(compiler, keyword)) {
		uc_compiler_parse_advance(compiler);

		return;
	}

	uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
		"Unexpected token\nExpecting '%s'", keyword);
}

static void
uc_compiler_parse_synchronize(uc_compiler_t *compiler)
{
	compiler->parser->synchronizing = false;

	while (compiler->parser->curr.type != TK_EOF) {
		if (compiler->parser->prev.type == TK_SCOL)
			return;

		switch (compiler->parser->curr.type) {
		case TK_IF:
		case TK_FOR:
		case TK_WHILE:
		case TK_SWITCH:
		case TK_FUNC:
		case TK_TRY:
		case TK_RETURN:
		case TK_BREAK:
		case TK_CONTINUE:
		case TK_LOCAL:
			return;

		default:
			break;
		}

		uc_compiler_parse_advance(compiler);
	}
}

static uc_parse_rule_t *
uc_compiler_parse_rule(uc_tokentype_t type)
{
	return &uc_compiler_parse_rules[type];
}

static bool
uc_compiler_parse_at_assignment_op(uc_compiler_t *compiler)
{
	switch (compiler->parser->curr.type) {
	case TK_ASBAND:
	case TK_ASBXOR:
	case TK_ASBOR:
	case TK_ASLEFT:
	case TK_ASRIGHT:
	case TK_ASMUL:
	case TK_ASDIV:
	case TK_ASMOD:
	case TK_ASADD:
	case TK_ASSUB:
	case TK_ASAND:
	case TK_ASOR:
	case TK_ASEXP:
	case TK_ASNULLISH:
	case TK_ASSIGN:
		return true;

	default:
		return false;
	}
}

static void
uc_compiler_backpatch(uc_compiler_t *compiler, size_t break_addr, size_t next_addr);

static void
uc_compiler_parse_precedence_for_token(uc_compiler_t *compiler, uc_precedence_t precedence, uc_token_t *token)
{
	uc_parse_rule_t *rule;

	rule = uc_compiler_parse_rule(token->type);

	if (!rule->prefix) {
		uc_compiler_syntax_error(compiler, token->pos, "Expecting expression");

		return;
	}

	uc_compiler_exprstack_push(compiler, token->type,
		(precedence <= P_ASSIGN) ? F_ASSIGNABLE : 0);

	compiler->patchlist = &(uc_patchlist_t){
		.parent = compiler->patchlist,
		.token = TK_EXP,
	};

	rule->prefix(compiler);

	while (precedence <= uc_compiler_parse_rule(compiler->parser->curr.type)->precedence) {
		compiler->exprstack->token = compiler->parser->curr.type;

		rule = uc_compiler_parse_rule(compiler->exprstack->token);

		if (!rule->infix) {
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos, "Expecting ';' or binary operator");
			uc_compiler_parse_advance(compiler);

			uc_compiler_backpatch(compiler, compiler->patchlist->depth, 0);
			uc_compiler_exprstack_pop(compiler);

			return;
		}

		/* allow reserved words in property accessors */
		if (rule->infix == uc_compiler_compile_dot)
			compiler->parser->lex.no_keyword = true;

		uc_compiler_parse_advance(compiler);

		rule->infix(compiler);
	}

	uc_compiler_backpatch(compiler, compiler->patchlist->depth, 0);

	uc_compiler_exprstack_pop(compiler);
}

static void
uc_compiler_parse_precedence(uc_compiler_t *compiler, uc_precedence_t precedence)
{
	uc_token_t token = compiler->parser->curr;
	uc_parse_rule_t *rule = uc_compiler_parse_rule(token.type);

	/* allow reserved words as property names in object literals */
	if (rule->prefix == uc_compiler_compile_object)
		compiler->parser->lex.no_keyword = true;

	/* unless a sub-expression follows, treat subsequent slash as division
	 * operator and not as beginning of regexp literal */
	if (rule->prefix != uc_compiler_compile_paren &&
	    rule->prefix != uc_compiler_compile_unary &&
	    rule->prefix != uc_compiler_compile_array)
		compiler->parser->lex.no_regexp = true;

	uc_compiler_parse_advance(compiler);
	uc_compiler_parse_precedence_for_token(compiler, precedence, &token);
}

static size_t
uc_compiler_reladdr32(uc_compiler_t *compiler, size_t from, size_t to)
{
	ssize_t delta = to - from;

	if (delta < -0x7fffffff || delta > 0x7fffffff) {
		uc_compiler_syntax_error(compiler, 0, "Jump address too far");

		return 0;
	}

	return (size_t)(delta + 0x7fffffff);
}

static void
uc_compiler_emit_stmt_start(uc_compiler_t *compiler, uc_token_t *tok)
{
	uc_chunk_stmt_start(
		uc_compiler_current_chunk(compiler),
		uc_compiler_set_srcpos(compiler, tok->pos));
}

static void
uc_compiler_emit_stmt_end(uc_compiler_t *compiler)
{
	uc_chunk_stmt_end(
		uc_compiler_current_chunk(compiler),
		uc_compiler_set_srcpos(compiler, compiler->parser->prev.end));
}

static size_t
uc_compiler_reladdr16(uc_compiler_t *compiler, size_t from, size_t to)
{
	ssize_t delta = to - from;

	if (delta < -0x7fff || delta > 0x7fff) {
		uc_compiler_syntax_error(compiler, 0, "Jump address too far");

		return 0;
	}

	return (size_t)(delta + 0x7fff);
}

static size_t
uc_compiler_emit_insn(uc_compiler_t *compiler, size_t srcpos, uc_vm_insn_t insn)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t lineoff = uc_compiler_set_srcpos(compiler, srcpos);

	compiler->last_insn = uc_chunk_add(chunk, (uint8_t)insn, lineoff);

	return compiler->last_insn;
}

static size_t
uc_compiler_emit_u8(uc_compiler_t *compiler, size_t srcpos, uint8_t n)
{
	return uc_chunk_add(
		uc_compiler_current_chunk(compiler),
		n,
		uc_compiler_set_srcpos(compiler, srcpos));
}

static size_t
uc_compiler_emit_u16(uc_compiler_t *compiler, size_t srcpos, uint16_t n)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t lineoff = uc_compiler_set_srcpos(compiler, srcpos);

	uc_chunk_add(chunk, n / 0x100, lineoff);
	uc_chunk_add(chunk, n % 0x100, 0);

	return chunk->count - 2;
}

static size_t
uc_compiler_emit_u32(uc_compiler_t *compiler, size_t srcpos, uint32_t n)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t lineoff = uc_compiler_set_srcpos(compiler, srcpos);

	uc_chunk_add(chunk, n / 0x1000000, lineoff);
	uc_chunk_add(chunk, (n / 0x10000) % 0x100, 0);
	uc_chunk_add(chunk, (n / 0x100) % 0x100, 0);
	uc_chunk_add(chunk, n % 0x100, 0);

	return chunk->count - 4;
}

static size_t
uc_compiler_emit_s32(uc_compiler_t *compiler, size_t srcpos, int32_t n)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t lineoff = uc_compiler_set_srcpos(compiler, srcpos);
	uint32_t v;

	if (n <= 0)
		v = n + 0x7fffffff;
	else
		v = (uint32_t)n + 0x7fffffff;

	uc_chunk_add(chunk, v / 0x1000000, lineoff);
	uc_chunk_add(chunk, (v / 0x10000) % 0x100, 0);
	uc_chunk_add(chunk, (v / 0x100) % 0x100, 0);
	uc_chunk_add(chunk, v % 0x100, 0);

	return chunk->count - 4;
}

static uint32_t
uc_compiler_get_u32(uc_compiler_t *compiler, size_t off)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	return chunk->entries[off + 0] * 0x1000000 +
	       chunk->entries[off + 1] * 0x10000 +
	       chunk->entries[off + 2] * 0x100 +
	       chunk->entries[off + 3];
}

static void
uc_compiler_set_u32(uc_compiler_t *compiler, size_t off, uint32_t n)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	chunk->entries[off + 0] = n / 0x1000000;
	chunk->entries[off + 1] = (n / 0x10000) % 0x100;
	chunk->entries[off + 2] = (n / 0x100) % 0x100;
	chunk->entries[off + 3] = n % 0x100;
}

/* Returns true if the most recently emitted instruction sequence is a function
 * call whose operands end exactly at the current end of the chunk, i.e. the call
 * is the last thing compiled before the upcoming return. */
static bool
uc_compiler_last_insn_is_tailcall(uc_compiler_t *compiler)
{
	/* tailcall_off records the chunk end right after the last emitted call's
	 * operands; it is a tail call candidate only if nothing has been compiled
	 * since, so it must equal the current chunk end */
	return compiler->tailcall_off ==
		uc_compiler_current_chunk(compiler)->count;
}

/* Invoking a function in tail position allows the VM to reuse the current call
 * frame for the callee, which permits unbounded tail recursion.
 *
 * A function call in return position is marked by emitting a 0x00 marker byte
 * immediately after the enclosing I_RETURN. The VM recognizes a call whose
 * operands are followed by I_RETURN + marker as a tail call and reuses the
 * current frame for the callee.
 *
 * The marker is placed after the return so that neither VM ever executes it:
 * older VMs return (and unwind) before reaching it, and the optimized VM jumps
 * to the callee at the call site. It is therefore pure data, and the emitted
 * bytecode remains compatible with interpreters predating tail call
 * optimization.
 *
 * uc_compiler_tailcall_pending() must be called before the I_RETURN is emitted
 * (while the call is still the last instruction); if it returns true, the
 * caller must emit the I_RETURN and then uc_compiler_emit_tailcall_marker(). */

static bool
uc_compiler_tailcall_pending(uc_compiler_t *compiler)
{
	/* an enclosing try block might need to catch exceptions raised by the
	 * invoked function, which requires the current frame to stay in place */
	if (compiler->try_depth > 0)
		return false;

	return uc_compiler_last_insn_is_tailcall(compiler);
}

static void
uc_compiler_emit_tailcall_marker(uc_compiler_t *compiler)
{
	uc_chunk_add(uc_compiler_current_chunk(compiler), 0x00, 0);
}

static size_t
uc_compiler_emit_constant_index(uc_compiler_t *compiler, size_t srcpos, uc_value_t *val)
{
	size_t cidx = uc_program_add_constant(compiler->program, val);

	uc_compiler_emit_u32(compiler, srcpos, cidx);

	return cidx;
}

static size_t
uc_compiler_emit_constant(uc_compiler_t *compiler, size_t srcpos, uc_value_t *val)
{
	size_t cidx;

	uc_compiler_emit_insn(compiler, srcpos, I_LOAD);

	cidx = uc_compiler_emit_constant_index(compiler, srcpos, val);

	return cidx;
}

static size_t
uc_compiler_emit_regexp(uc_compiler_t *compiler, size_t srcpos, uc_value_t *val)
{
	size_t cidx = uc_program_add_constant(compiler->program, val);

	uc_compiler_emit_insn(compiler, srcpos, I_LREXP);
	uc_compiler_emit_u32(compiler, 0, cidx);

	return cidx;
}

static size_t
uc_compiler_emit_jmp(uc_compiler_t *compiler, size_t srcpos)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	uc_compiler_emit_insn(compiler, srcpos, I_JMP);
	uc_compiler_emit_u32(compiler, 0, 0);

	return chunk->count - 5;
}

static size_t
uc_compiler_emit_jmpz(uc_compiler_t *compiler, size_t srcpos)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	uc_compiler_emit_insn(compiler, srcpos, I_JMPZ);
	uc_compiler_emit_u32(compiler, 0, 0);

	return chunk->count - 5;
}

static size_t
uc_compiler_emit_jmp_dest(uc_compiler_t *compiler, size_t srcpos, uint32_t dest)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	uc_compiler_emit_insn(compiler, srcpos, I_JMP);
	uc_compiler_emit_u32(compiler, 0, uc_compiler_reladdr32(compiler, chunk->count - 1, dest));

	return chunk->count - 5;
}

static size_t
uc_compiler_emit_jmpnt(uc_compiler_t *compiler, size_t srcpos, uint16_t types, uint8_t depth)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	uc_vector_push(compiler->patchlist,
		uc_compiler_emit_insn(compiler, srcpos, I_JMPNT));

	uc_compiler_emit_u32(compiler, 0,
		uc_compiler_reladdr16(compiler, 0, TK_BREAK)
			| ((types & 0x1fff) << 16)
			| ((depth & 0x7) << 29));

	return chunk->count - 5;
}

static size_t
uc_compiler_emit_copy(uc_compiler_t *compiler, size_t srcpos, uint8_t depth)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	uc_compiler_emit_insn(compiler, srcpos, I_COPY);
	uc_compiler_emit_u8(compiler, 0, depth);

	return chunk->count - 2;
}

static ssize_t
uc_compiler_get_jmpaddr(uc_compiler_t *compiler, size_t off)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t width = (chunk->entries[off] == I_JMPNT) ? 2 : 4;

	assert(chunk->entries[off] == I_JMP || chunk->entries[off] == I_JMPZ || chunk->entries[off] == I_JMPNT);
	assert(off + 4 < chunk->count);

	if (width == 2)
		return (
			chunk->entries[off + 3] * 0x100UL +
			chunk->entries[off + 4]
		) - 0x7fff;
	else
		return (
			chunk->entries[off + 1] * 0x1000000UL +
			chunk->entries[off + 2] * 0x10000UL +
			chunk->entries[off + 3] * 0x100UL +
			chunk->entries[off + 4]
		) - 0x7fffffff;
}

static void
uc_compiler_set_jmpaddr(uc_compiler_t *compiler, size_t off, uint32_t dest)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t width = (chunk->entries[off] == I_JMPNT) ? 2 : 4;
	size_t addr;

	assert(chunk->entries[off] == I_JMP || chunk->entries[off] == I_JMPZ || chunk->entries[off] == I_JMPNT);
	assert(off + 4 < chunk->count);

	if (width == 2) {
		addr = uc_compiler_reladdr16(compiler, off, dest);
		chunk->entries[off + 3] = addr / 0x100;
		chunk->entries[off + 4] = addr % 0x100;
	}
	else {
		addr = uc_compiler_reladdr32(compiler, off, dest);
		chunk->entries[off + 1] = addr / 0x1000000;
		chunk->entries[off + 2] = (addr / 0x10000) % 0x100;
		chunk->entries[off + 3] = (addr / 0x100) % 0x100;
		chunk->entries[off + 4] = addr % 0x100;
	}
}

static void
uc_compiler_inc_exportnum(uc_compiler_t *compiler)
{
	uc_source_t *root = uc_program_function_source(uc_program_entry(compiler->program));
	uint64_t u;

	if (root->exports.count == 0) {
		uc_vector_push(&root->exports, ucv_uint64_new(1));
	}
	else {
		u = ucv_uint64_get(root->exports.entries[0]);

		ucv_put(root->exports.entries[0]);

		root->exports.entries[0] = ucv_uint64_new(u + 1);
	}
}

static size_t
uc_compiler_get_exportnum(uc_compiler_t *compiler)
{
	uc_source_t *root = uc_program_function_source(uc_program_entry(compiler->program));

	return root->exports.count ? ucv_uint64_get(root->exports.entries[0]) : 0;
}

static void
uc_compiler_emit_exports(uc_compiler_t *compiler) {
	size_t i;

	if (!compiler->patchlist || compiler->patchlist->token != TK_EXPORT)
		return;

	for (i = 0; i < compiler->patchlist->count; i++) {
		uc_compiler_emit_insn(compiler, 0, I_EXPORT);
		uc_compiler_emit_u32(compiler, 0, compiler->patchlist->entries[i]);
	}
}

static uc_function_t *
uc_compiler_finish(uc_compiler_t *compiler, uc_tokentype_t last_statement_type)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_locals_t *locals = &compiler->locals;
	uc_upvals_t *upvals = &compiler->upvals;
	size_t i;

	if (compiler->function->module)
		uc_compiler_emit_exports(compiler);

	uc_compiler_emit_insn(compiler, 0, I_LNULL);
	uc_compiler_emit_insn(compiler, 0, I_RETURN);

	for (i = 0; i < locals->count; i++) {
		uc_chunk_debug_add_variable(chunk,
			locals->entries[i].from,
			chunk->count,
			i,
			false,
			locals->entries[i].name);

		ucv_put(locals->entries[i].name);
	}

	for (i = 0; i < upvals->count; i++) {
		uc_chunk_debug_add_variable(chunk,
			0,
			chunk->count,
			i,
			true,
			upvals->entries[i].name);

		ucv_put(upvals->entries[i].name);
	}

	uc_vector_clear(locals);
	uc_vector_clear(upvals);

	if (compiler->parser->error) {
		uc_program_function_free(compiler->function);

		return NULL;
	}

	return compiler->function;
}

static void
uc_compiler_enter_scope(uc_compiler_t *compiler)
{
	compiler->scope_depth++;
}

static void
uc_compiler_leave_scope(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_locals_t *locals = &compiler->locals;

	compiler->scope_depth--;

	while (locals->count > 0 && locals->entries[locals->count - 1].depth > (ssize_t)compiler->scope_depth) {
		locals->count--;

		uc_chunk_debug_add_variable(chunk,
			locals->entries[locals->count].from,
			chunk->count,
			locals->count,
			false,
			locals->entries[locals->count].name);

		ucv_put(locals->entries[locals->count].name);
		locals->entries[locals->count].name = NULL;

		uc_compiler_emit_insn(compiler, 0,
			locals->entries[locals->count].captured ? I_CUPV : I_POP);
	}
}

static bool
uc_compiler_is_strict(uc_compiler_t *compiler)
{
	uc_function_t *fn = (uc_function_t *)compiler->function;

	return fn->strict;
}

static ssize_t
uc_compiler_declare_local(uc_compiler_t *compiler, uc_value_t *name, bool constant)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_locals_t *locals = &compiler->locals;
	const char *str1, *str2;
	size_t i, len1, len2;

	if (locals->count >= 0x00FFFFFF) {
		uc_compiler_syntax_error(compiler, 0, "Too many local variables");

		return -1;
	}

	str1 = ucv_string_get(name);
	len1 = ucv_string_length(name);

	for (i = locals->count; i > 0; i--) {
		if (locals->entries[i - 1].depth != -1 && locals->entries[i - 1].depth < (ssize_t)compiler->scope_depth)
			break;

		str2 = ucv_string_get(locals->entries[i - 1].name);
		len2 = ucv_string_length(locals->entries[i - 1].name);

		if (str1 && str2 && len1 == len2 && !memcmp(str1, str2, len1)) {
			if (locals->entries[i - 1].funcstub) {
				uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
					"Variable '%s' redeclared", str2);

				return -1;
			}

			if (uc_compiler_is_strict(compiler)) {
				uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
					"Variable '%s' redeclared", str2);

				return -1;
			}

			return i - 1;
		}
	}

	uc_vector_push(locals, {
		.name = ucv_get(name),
		.depth = -1,
		.captured = false,
		.from = chunk->count,
		.constant = constant
	});

	return -1;
}

static ssize_t
uc_compiler_initialize_local(uc_compiler_t *compiler)
{
	uc_locals_t *locals = &compiler->locals;

	locals->entries[locals->count - 1].depth = compiler->scope_depth;

	return locals->count - 1;
}

static ssize_t
uc_compiler_resolve_local(uc_compiler_t *compiler, uc_value_t *name, bool *constant)
{
	uc_locals_t *locals = &compiler->locals;
	const char *str1, *str2;
	size_t i, len1, len2;

	str1 = ucv_string_get(name);
	len1 = ucv_string_length(name);

	for (i = locals->count; i > 0; i--) {
		str2 = ucv_string_get(locals->entries[i - 1].name);
		len2 = ucv_string_length(locals->entries[i - 1].name);

		if (!str1 || !str2 || len1 != len2 || memcmp(str1, str2, len1))
			continue;

		if (locals->entries[i - 1].depth == -1) {
			uc_compiler_syntax_error(compiler, 0,
				"Can't access lexical declaration '%s' before initialization", str2);

			return -1;
		}

		*constant = locals->entries[i - 1].constant;

		return i - 1;
	}

	return -1;
}

static ssize_t
uc_compiler_resolve_funcstub(uc_compiler_t *compiler, uc_value_t *name)
{
	uc_locals_t *locals = &compiler->locals;
	const char *str1, *str2;
	size_t i, len1, len2;

	str1 = ucv_string_get(name);
	len1 = ucv_string_length(name);

	for (i = locals->count; i > 0; i--) {
		if (locals->entries[i - 1].depth != -1 &&
		    locals->entries[i - 1].depth < (ssize_t)compiler->scope_depth)
			break;

		str2 = ucv_string_get(locals->entries[i - 1].name);
		len2 = ucv_string_length(locals->entries[i - 1].name);

		if (str1 && str2 && len1 == len2 && !memcmp(str1, str2, len1))
			return locals->entries[i - 1].funcstub ? (ssize_t)(i - 1) : -1;
	}

	return -1;
}

static ssize_t
uc_compiler_add_upval(uc_compiler_t *compiler, size_t idx, bool local, uc_value_t *name, bool constant)
{
	uc_function_t *function = (uc_function_t *)compiler->function;
	uc_upvals_t *upvals = &compiler->upvals;
	uc_upval_t *uv;
	size_t i;

	for (i = 0, uv = upvals->entries; i < upvals->count; i++, uv = upvals->entries + i)
		if (uv->index == idx && uv->local == local)
			return i;

	/* XXX: encoding... */
	if (upvals->count >= (2 << 14)) {
		uc_compiler_syntax_error(compiler, 0, "Too many upvalues");

		return -1;
	}

	uc_vector_push(upvals, {
		.local = local,
		.index = idx,
		.name  = ucv_get(name),
		.constant = constant
	});

	function->nupvals++;

	return upvals->count - 1;
}

static ssize_t
uc_compiler_resolve_upval(uc_compiler_t *compiler, uc_value_t *name, bool *constant)
{
	uc_upvals_t *upvals = &compiler->upvals;
	uc_upval_t *uv;
	ssize_t idx;
	size_t i;

	if (!compiler->parent) {
		for (i = 0, uv = upvals->entries; i < upvals->count; i++, uv = upvals->entries + i) {
			if (ucv_is_equal(uv->name, name) && uv->local == false) {
				*constant = uv->constant;

				return i;
			}
		}

		return -1;
	}

	idx = uc_compiler_resolve_local(compiler->parent, name, constant);

	if (idx > -1) {
		compiler->parent->locals.entries[idx].captured = true;

		return uc_compiler_add_upval(compiler, idx, true, name, *constant);
	}

	idx = uc_compiler_resolve_upval(compiler->parent, name, constant);

	if (idx > -1)
		return uc_compiler_add_upval(compiler, idx, false, name, *constant);

	return -1;
}

static void
uc_compiler_backpatch(uc_compiler_t *compiler, size_t break_addr, size_t next_addr)
{
	uc_patchlist_t *pl = compiler->patchlist;
	volatile ssize_t jmpaddr;
	size_t i;

	for (i = 0; i < pl->count; i++) {
		jmpaddr = uc_compiler_get_jmpaddr(compiler, pl->entries[i]);

		switch (jmpaddr) {
		case TK_BREAK:
			/* if we have a break addr, patch instruction */
			if (break_addr) {
				uc_compiler_set_jmpaddr(compiler, pl->entries[i], break_addr);
				continue;
			}

			break;

		case TK_CONTINUE:
			/* if we have a continue addr, patch instruction */
			if (next_addr) {
				uc_compiler_set_jmpaddr(compiler, pl->entries[i], next_addr);
				continue;
			}

			break;
		}

		/* there should be no unhandled instructions */
		assert(0);
	}

	free(pl->entries);

	compiler->patchlist = pl->parent;
}

static bool
uc_compiler_check_assignment_lhs(uc_compiler_t *compiler)
{
	return (
		compiler->patchlist == NULL ||
		compiler->patchlist->count == 0 ||
		compiler->patchlist->token != TK_EXP
	);
}

static void
uc_compiler_emit_inc_dec(uc_compiler_t *compiler, uc_tokentype_t toktype, bool is_postfix)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_value_t *varname = NULL;
	uc_vm_insn_t type;
	uint32_t cidx = 0;
	int insn;

	/* determine kind of emitted load instruction and operand value (if any) */
	type = chunk->entries ? chunk->entries[compiler->last_insn] : 0;

	if (type == I_LVAR || type == I_LLOC || type == I_LUPV) {
		cidx = uc_compiler_get_u32(compiler, compiler->last_insn + 1);

		if (type == I_LLOC && compiler->locals.entries[cidx].constant)
			varname = compiler->locals.entries[cidx].name;
		else if (type == I_LUPV && compiler->upvals.entries[cidx].constant)
			varname = compiler->upvals.entries[cidx].name;

		if (varname)
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Invalid increment/decrement of constant '%s'",
				ucv_string_get(varname));

		uc_chunk_pop(chunk);
		uc_chunk_pop(chunk);
		uc_chunk_pop(chunk);
		uc_chunk_pop(chunk);
		uc_chunk_pop(chunk);
	}

	/* if we're mutating an object or array field, pop the last lval instruction
	 * to leave object + last field name value on stack */
	else if (type == I_LVAL) {
		if (!uc_compiler_check_assignment_lhs(compiler)) {
			uc_compiler_syntax_error(compiler, 0,
				"Invalid left-hand side expression for increment/decrement");

			return;
		}

		uc_chunk_pop(chunk);
	}
	else {
		uc_compiler_syntax_error(compiler, 0, "Invalid increment/decrement operand");

		return;
	}

	insn = (toktype == TK_INC) ? I_PLUS : I_MINUS;

	/* add / subtract 1 */
	uc_compiler_emit_insn(compiler, 0, I_LOAD8);
	uc_compiler_emit_u8(compiler, 0, 1);

	/* depending on variable type, emit corresponding increment instruction */
	switch (type) {
	case I_LVAR:
		uc_compiler_emit_insn(compiler, 0, I_UVAR);
		uc_compiler_emit_u32(compiler, 0, (insn << 24) | cidx);
		break;

	case I_LLOC:
		uc_compiler_emit_insn(compiler, 0, I_ULOC);
		uc_compiler_emit_u32(compiler, 0, (insn << 24) | cidx);
		break;

	case I_LUPV:
		uc_compiler_emit_insn(compiler, 0, I_UUPV);
		uc_compiler_emit_u32(compiler, 0, (insn << 24) | cidx);
		break;

	case I_LVAL:
		uc_compiler_emit_insn(compiler, 0, I_UVAL);
		uc_compiler_emit_u8(compiler, 0, insn);
		break;

	default:
		break;
	}

	/* for post increment or decrement, add/subtract 1 to yield final value */
	if (is_postfix) {
		uc_compiler_emit_insn(compiler, 0, I_LOAD8);
		uc_compiler_emit_u8(compiler, 0, 1);

		uc_compiler_emit_insn(compiler, 0, (toktype == TK_INC) ? I_SUB : I_ADD);
	}
}


static void
uc_compiler_compile_unary(uc_compiler_t *compiler)
{
	uc_tokentype_t type = compiler->parser->prev.type;

	uc_compiler_parse_precedence(compiler, P_UNARY);

	switch (type) {
	case TK_SUB:
		uc_compiler_emit_insn(compiler, 0, I_MINUS);
		break;

	case TK_ADD:
		uc_compiler_emit_insn(compiler, 0, I_PLUS);
		break;

	case TK_NOT:
		uc_compiler_emit_insn(compiler, 0, I_NOT);
		break;

	case TK_COMPL:
		uc_compiler_emit_insn(compiler, 0, I_COMPL);
		break;

	case TK_INC:
	case TK_DEC:
		uc_compiler_emit_inc_dec(compiler, type, false);
		break;

	default:
		return;
	}
}

static void
uc_compiler_compile_binary(uc_compiler_t *compiler)
{
	uc_tokentype_t type = compiler->parser->prev.type;

	uc_compiler_parse_precedence(compiler, uc_compiler_parse_rule(type)->precedence + 1);
	uc_compiler_emit_insn(compiler, 0, I_BOR + (type - TK_BOR));
}

static void
uc_compiler_compile_delete(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_vm_insn_t type;

	uc_compiler_parse_precedence(compiler, P_UNARY);

	type = (compiler->last_insn < chunk->count)
		? chunk->entries[compiler->last_insn] : 0;

	if (type != I_LVAL) {
		uc_compiler_syntax_error(compiler, 0,
			"expecting a property access expression");

		return;
	}

	chunk->entries[compiler->last_insn] = I_DELETE;
}

static uc_vm_insn_t
uc_compiler_emit_variable_rw(uc_compiler_t *compiler, uc_value_t *varname, uc_tokentype_t type)
{
	uc_vm_insn_t insn;
	uint32_t sub_insn;
	bool constant;
	ssize_t idx;

	switch (type) {
	case TK_ASADD:     sub_insn = I_ADD;     break;
	case TK_ASSUB:     sub_insn = I_SUB;     break;
	case TK_ASMUL:     sub_insn = I_MUL;     break;
	case TK_ASDIV:     sub_insn = I_DIV;     break;
	case TK_ASMOD:     sub_insn = I_MOD;     break;
	case TK_ASBAND:    sub_insn = I_BAND;    break;
	case TK_ASBXOR:    sub_insn = I_BXOR;    break;
	case TK_ASBOR:     sub_insn = I_BOR;     break;
	case TK_ASLEFT:    sub_insn = I_LSHIFT;  break;
	case TK_ASRIGHT:   sub_insn = I_RSHIFT;  break;
	case TK_ASEXP:     sub_insn = I_EXP;     break;
	default:           sub_insn = 0;         break;
	}

	if (!varname) {
		if (sub_insn != 0)
			insn = I_UVAL;
		else if (type != 0 && type != TK_QDOT && type != TK_QLBRACK)
			insn = I_SVAL;
		else
			insn = I_LVAL;

		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, insn);

		if (sub_insn)
			uc_compiler_emit_u8(compiler, compiler->parser->prev.pos, sub_insn);
	}
	else if ((idx = uc_compiler_resolve_local(compiler, varname, &constant)) > -1) {
		insn = sub_insn ? I_ULOC : (type ? I_SLOC : I_LLOC);

		if (insn != I_LLOC && constant)
			uc_compiler_syntax_error(compiler, 0,
				"Invalid assignment to constant '%s'", ucv_string_get(varname));

		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, insn);
		uc_compiler_emit_u32(compiler, compiler->parser->prev.pos,
			((sub_insn & 0xff) << 24) | idx);
	}
	else if ((idx = uc_compiler_resolve_upval(compiler, varname, &constant)) > -1) {
		insn = sub_insn ? I_UUPV : (type ? I_SUPV : I_LUPV);

		if (insn != I_LUPV && constant)
			uc_compiler_syntax_error(compiler, 0,
				"Invalid assignment to constant '%s'", ucv_string_get(varname));

		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, insn);
		uc_compiler_emit_u32(compiler, compiler->parser->prev.pos,
			((sub_insn & 0xff) << 24) | idx);
	}
	else {
		idx = uc_program_add_constant(compiler->program, varname);
		insn = sub_insn ? I_UVAR : (type ? I_SVAR : I_LVAR);

		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, insn);
		uc_compiler_emit_u32(compiler, compiler->parser->prev.pos,
			((sub_insn & 0xff) << 24) | idx);
	}

	return insn;
}

static void
uc_compiler_emit_variable_copy(uc_compiler_t *compiler, uc_value_t *var)
{
	if (!var) {
		uc_compiler_emit_copy(compiler, 0, 1);
		uc_compiler_emit_copy(compiler, 0, 1);
	}

	uc_compiler_emit_variable_rw(compiler, var, 0);
}

static void
uc_compiler_compile_and(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off;

	uc_compiler_emit_copy(compiler, 0, 0);
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);
	uc_compiler_emit_insn(compiler, 0, I_POP);
	uc_compiler_parse_precedence(compiler, P_AND);
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
}

static void
uc_compiler_compile_and_assignment(uc_compiler_t *compiler, uc_value_t *var)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off;

	uc_compiler_emit_variable_copy(compiler, var);
	uc_compiler_emit_copy(compiler, 0, 0);
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);
	uc_compiler_emit_insn(compiler, 0, I_POP);
	uc_compiler_parse_precedence(compiler, P_ASSIGN);
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
	uc_compiler_emit_variable_rw(compiler, var, TK_ASSIGN);
}

static void
uc_compiler_compile_or(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off, jmp_off;

	uc_compiler_emit_copy(compiler, 0, 0);
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);
	jmp_off = uc_compiler_emit_jmp(compiler, 0);
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
	uc_compiler_emit_insn(compiler, 0, I_POP);
	uc_compiler_parse_precedence(compiler, P_OR);
	uc_compiler_set_jmpaddr(compiler, jmp_off, chunk->count);
}

static void
uc_compiler_compile_or_assignment(uc_compiler_t *compiler, uc_value_t *var)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off, jmp_off;

	uc_compiler_emit_variable_copy(compiler, var);
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);
	uc_compiler_emit_variable_rw(compiler, var, 0);
	jmp_off = uc_compiler_emit_jmp(compiler, 0);
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
	uc_compiler_parse_precedence(compiler, P_ASSIGN);
	uc_compiler_emit_variable_rw(compiler, var, TK_ASSIGN);
	uc_compiler_set_jmpaddr(compiler, jmp_off, chunk->count);
}

static void
uc_compiler_compile_nullish(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off, jmp_off;

	uc_compiler_emit_copy(compiler, 0, 0);
	uc_compiler_emit_insn(compiler, 0, I_LNULL);
	uc_compiler_emit_insn(compiler, 0, I_NES);
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);
	jmp_off = uc_compiler_emit_jmp(compiler, 0);
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
	uc_compiler_emit_insn(compiler, 0, I_POP);
	uc_compiler_parse_precedence(compiler, P_OR);
	uc_compiler_set_jmpaddr(compiler, jmp_off, chunk->count);
}

static void
uc_compiler_compile_nullish_assignment(uc_compiler_t *compiler, uc_value_t *var)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off, jmp_off;

	uc_compiler_emit_variable_copy(compiler, var);
	uc_compiler_emit_insn(compiler, 0, I_LNULL);
	uc_compiler_emit_insn(compiler, 0, I_NES);
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);
	uc_compiler_emit_variable_rw(compiler, var, 0);
	jmp_off = uc_compiler_emit_jmp(compiler, 0);
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
	uc_compiler_parse_precedence(compiler, P_ASSIGN);
	uc_compiler_emit_variable_rw(compiler, var, TK_ASSIGN);
	uc_compiler_set_jmpaddr(compiler, jmp_off, chunk->count);
}

static void
uc_compiler_compile_expression(uc_compiler_t *compiler, bool tag_stmt)
{
	if (tag_stmt)
		uc_compiler_emit_stmt_start(compiler, &compiler->parser->curr);

	uc_compiler_parse_precedence(compiler, P_COMMA);

	if (tag_stmt)
		uc_compiler_emit_stmt_end(compiler);
}

static bool
uc_compiler_compile_assignment(uc_compiler_t *compiler, uc_value_t *var)
{
	uc_tokentype_t type = compiler->parser->curr.type;

	if (!uc_compiler_parse_at_assignment_op(compiler))
		return false;

	if (!uc_compiler_check_assignment_lhs(compiler)) {
		uc_compiler_syntax_error(compiler, 0,
			"Invalid left-hand side expression for assignment");

		return false;
	}

	switch (type)
	{
	case TK_ASNULLISH:
		uc_compiler_parse_advance(compiler);
		uc_compiler_compile_nullish_assignment(compiler, var);
		break;

	case TK_ASOR:
		uc_compiler_parse_advance(compiler);
		uc_compiler_compile_or_assignment(compiler, var);
		break;

	case TK_ASAND:
		uc_compiler_parse_advance(compiler);
		uc_compiler_compile_and_assignment(compiler, var);
		break;

	default:
		uc_compiler_parse_advance(compiler);
		uc_compiler_parse_precedence(compiler, P_ASSIGN);
		uc_compiler_emit_variable_rw(compiler, var, type);
		break;
	}

	return true;
}

static bool
uc_compiler_compile_arrowfn(uc_compiler_t *compiler, uc_value_t *args, bool restarg)
{
	uc_tokentype_t last_statement_type = TK_NULL;
	bool array = (ucv_type(args) == UC_ARRAY);
	uc_compiler_t fncompiler = { 0 };
	size_t i, pos, load_off;
	uc_function_t *fn;
	ssize_t slot;

	if (!uc_compiler_parse_match(compiler, TK_ARROW))
		return false;

	pos = compiler->parser->prev.pos;

	uc_compiler_init(&fncompiler, NULL, uc_compiler_current_source(compiler),
		compiler->parser->prev.pos,
		compiler->program,
		uc_compiler_is_strict(compiler));

	fncompiler.parent = compiler;
	fncompiler.parser = compiler->parser;
	fncompiler.exprstack = compiler->exprstack;

	fn = (uc_function_t *)fncompiler.function;
	fn->arrow = true;
	fn->vararg = args ? restarg : false;
	fn->nargs = array ? ucv_array_length(args) : !!args;

	uc_compiler_enter_scope(&fncompiler);

	/* declare local variables for arguments */
	for (i = 0; i < fn->nargs; i++) {
		slot = uc_compiler_declare_local(&fncompiler,
			array ? ucv_array_get(args, i) : args, false);

		if (slot != -1)
			uc_compiler_syntax_error(&fncompiler, pos,
				"Duplicate argument names are not allowed in this context");

		uc_compiler_initialize_local(&fncompiler);
	}

	/* parse and compile body */
	if (uc_compiler_parse_match(&fncompiler, TK_LBRACE)) {
		while (!uc_compiler_parse_check(&fncompiler, TK_RBRACE) &&
		       !uc_compiler_parse_check(&fncompiler, TK_EOF))
			uc_compiler_compile_declaration(&fncompiler);

		uc_compiler_parse_consume(&fncompiler, TK_RBRACE);

		/* emit final return */
		if (last_statement_type != TK_RETURN) {
			uc_compiler_emit_insn(&fncompiler, 0, I_LNULL);
			uc_compiler_emit_insn(&fncompiler, 0, I_RETURN);
		}
	}
	else {
		uc_compiler_emit_stmt_start(&fncompiler, &compiler->parser->curr);
		uc_compiler_parse_precedence(&fncompiler, P_ASSIGN);

		/* invoke a function call in tail position to avoid growing the stack */
		bool tailcall = uc_compiler_tailcall_pending(&fncompiler);

		uc_compiler_emit_insn(&fncompiler, 0, I_RETURN);

		if (tailcall)
			uc_compiler_emit_tailcall_marker(&fncompiler);

		uc_compiler_emit_stmt_end(&fncompiler);
	}

	/* emit load instruction for function value */
	uc_compiler_emit_insn(compiler, pos, I_ARFN);
	load_off = uc_compiler_emit_u32(compiler, 0, 0);

	/* encode upvalue information */
	for (i = 0; i < fn->nupvals; i++)
		uc_compiler_emit_s32(compiler, 0,
			fncompiler.upvals.entries[i].local
				? -(fncompiler.upvals.entries[i].index + 1)
				: fncompiler.upvals.entries[i].index);

	/* finalize function compiler */
	fn = uc_compiler_finish(&fncompiler, TK_RETURN);

	if (fn)
		uc_compiler_set_u32(compiler, load_off,
			uc_program_function_id(compiler->program, fn));

	return true;
}

static uc_tokentype_t
uc_compiler_compile_var_or_arrowfn(uc_compiler_t *compiler, uc_value_t *name)
{
	uc_tokentype_t rv;

	if (uc_compiler_exprstack_is(compiler, F_ASSIGNABLE) && uc_compiler_compile_assignment(compiler, name)) {
		rv = TK_ASSIGN;
	}
	else if (uc_compiler_compile_arrowfn(compiler, name, false)) {
		rv = TK_ARROW;
	}
	else {
		uc_compiler_emit_variable_rw(compiler, name, 0);
		rv = TK_LABEL;
	}

	return rv;
}

static void
uc_compiler_compile_paren(uc_compiler_t *compiler)
{
	uc_value_t *varnames = NULL, *varname;
	bool maybe_arrowfn = false;
	bool restarg = false;

	/* First try to parse a complete parameter expression and remember the
	 * consumed label tokens as we go. */
	while (true) {
		if (uc_compiler_parse_check(compiler, TK_LABEL)) {
			if (!varnames)
				varnames = ucv_array_new(NULL);

			ucv_array_push(varnames, ucv_get(compiler->parser->curr.uv));

			/* A subsequent slash cannot be a regular expression literal */
			compiler->parser->lex.no_regexp = true;
			uc_compiler_parse_advance(compiler);
		}
		else if (uc_compiler_parse_match(compiler, TK_ELLIP)) {
			uc_compiler_parse_consume(compiler, TK_LABEL);

			if (!varnames)
				varnames = ucv_array_new(NULL);

			ucv_array_push(varnames, ucv_get(compiler->parser->prev.uv));

			/* A subsequent slash cannot be a regular expression literal */
			compiler->parser->lex.no_regexp = true;
			uc_compiler_parse_consume(compiler, TK_RPAREN);

			maybe_arrowfn = true;
			restarg = true;

			break;
		}
		else if (uc_compiler_parse_check(compiler, TK_COMMA)) {
			/* Reject consecutive commas */
			if (compiler->parser->prev.type == TK_COMMA)
				uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
					"Expecting expression");

			uc_compiler_parse_advance(compiler);

			continue;
		}
		else {
			maybe_arrowfn = uc_compiler_parse_check(compiler, TK_RPAREN);

			if (maybe_arrowfn) {
				/* A subsequent slash cannot be a regular expression literal */
				compiler->parser->lex.no_regexp = true;
				uc_compiler_parse_advance(compiler);
			}

			/* If we encounter a dot, treat potential subsequent keyword as label */
			if (uc_compiler_parse_check(compiler, TK_DOT) ||
			    uc_compiler_parse_check(compiler, TK_QDOT))
				compiler->parser->lex.no_keyword = true;

			break;
		}
	}

	/* The lhs we parsed so far is eligible for an arrow function arg list,
	 * try to continue compiling into arrow function... */
	if (maybe_arrowfn) {
		/* If we can parse the remainder as arrow function, we're done */
		if (uc_compiler_compile_arrowfn(compiler, varnames, restarg))
			goto out;

		/* ... otherwise disallow the `...` spread operator and empty
		 * parenthesized expressions */
		if (restarg || !varnames) {
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Expecting '=>' after parameter list");

			goto out;
		}
	}

	/* If we reach this, the expression we parsed so far cannot be a parameter
	 * list for an arrow function and we might have consumed one or multiple
	 * consecutive labels. */
	if (varnames) {
		/* Get last variable name */
		varname = ucv_array_get(varnames,
			ucv_array_length(varnames) - 1);

		/* If we consumed the right paren, the expression is complete and we
		 * only need to emit a variable read operation for the last parsed
		 * label since previous read operations are shadowed by subsequent ones
		 * in comma expressions and since pure variable reads are without
		 * side effects. */
		if (maybe_arrowfn) {
			uc_compiler_emit_variable_rw(compiler, varname, 0);

			goto out;
		}

		/* ... otherwise if the last token was a label, try continue parsing as
		 * assignment or arrow function expression and if that fails, as
		 * relational one */
		if (compiler->parser->prev.type == TK_LABEL) {
			uc_compiler_exprstack_push(compiler, TK_LABEL, F_ASSIGNABLE);

			if (uc_compiler_compile_var_or_arrowfn(compiler, varname) == TK_LABEL) {
				/* parse operand and rhs */
				while (P_TERNARY <= uc_compiler_parse_rule(compiler->parser->curr.type)->precedence) {
					uc_parse_rule_t *rule =
						uc_compiler_parse_rule(compiler->parser->curr.type);

					if (!rule->infix)
						break;

					uc_compiler_parse_advance(compiler);
					rule->infix(compiler);
				}
			}

			/* If we're not at the end of the expression, we require a comma.
			 * Also pop intermediate result in this case. */
			if (!uc_compiler_parse_check(compiler, TK_RPAREN)) {
				uc_compiler_emit_insn(compiler, 0, I_POP);
				uc_compiler_parse_consume(compiler, TK_COMMA);
			}

			uc_compiler_exprstack_pop(compiler);
		}
	}

	/* When we reach this point, all already complete expression possibilities
	 * have been eliminated and we either need to compile the next, non-label
	 * expression or reached the closing paren. If neither applies, we have a
	 * syntax error. */
	if (!uc_compiler_parse_check(compiler, TK_RPAREN))
		uc_compiler_compile_expression(compiler, false);

	/* A subsequent slash cannot be a regular expression literal */
	compiler->parser->lex.no_regexp = true;

	/* At this point we expect the end of the parenthesized expression, anything
	 * else is a syntax error */
	uc_compiler_parse_consume(compiler, TK_RPAREN);

out:
	ucv_put(varnames);
}

static void
uc_compiler_compile_call(uc_compiler_t *compiler)
{
	bool optional_chaining = (compiler->parser->prev.type == TK_QLPAREN);
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_jmplist_t spreads = { 0 };
	uc_vm_insn_t type;
	size_t i, nargs = 0;
	bool mcall;

	/* determine the kind of the lhs */
	type = (compiler->last_insn < chunk->count)
		? chunk->entries[compiler->last_insn] : 0;
	mcall = (type == I_LVAL);

	if (mcall) {
		uc_chunk_pop(chunk);
		uc_compiler_emit_insn(compiler, 0, I_PVAL);
	}

	if (optional_chaining)
		uc_compiler_emit_jmpnt(compiler, compiler->parser->prev.pos,
			(1u << UC_CFUNCTION) | (1u << UC_CLOSURE), mcall);

	/* compile arguments */
	if (!uc_compiler_parse_check(compiler, TK_RPAREN)) {
		do {
			/* if this is a spread arg, remember the argument index */
			if (uc_compiler_parse_match(compiler, TK_ELLIP))
				uc_vector_push(&spreads, nargs);

			/* compile argument expression */
			uc_compiler_parse_precedence(compiler, P_ASSIGN);
			nargs++;
		}
		while (uc_compiler_parse_match(compiler, TK_COMMA));
	}

	/* after a function call expression, no regexp literal can follow */
	compiler->parser->lex.no_regexp = true;
	uc_compiler_parse_consume(compiler, TK_RPAREN);

	/* if lhs is a dot or bracket expression, emit a method call */
	uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_CALL);

	if (nargs > 0xffff || spreads.count > 0x7fff)
		uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
			"Too many function call arguments");

	/* encode ordinary (bits 0..15) and spread argument (bits 16..30) count
	   as well as method call indication (bit 31) */
	uc_compiler_emit_u32(compiler, 0,
		(mcall ? 0x80000000 : 0) | ((spreads.count & 0x7fff) << 16) | nargs);

	/* encode spread arg positions */
	for (i = 0; i < spreads.count; i++)
		uc_compiler_emit_u16(compiler, 0, nargs - spreads.entries[i] - 1);

	uc_vector_clear(&spreads);

	/* remember the chunk end right after the invocation's operands for potential
	 * tail call optimization */
	compiler->tailcall_off = uc_compiler_current_chunk(compiler)->count;

	compiler->patchlist->depth = uc_compiler_current_chunk(compiler)->count;
}

static void
uc_compiler_compile_post_inc(uc_compiler_t *compiler)
{
	uc_compiler_emit_inc_dec(compiler, compiler->parser->prev.type, true);
}

static bool
uc_compiler_is_use_strict_pragma(uc_compiler_t *compiler)
{
	uc_value_t *v;

	if (uc_compiler_current_chunk(compiler)->count > 0)
		return false;

	if (compiler->parser->lex.block != STATEMENTS)
		return false;

	v = compiler->parser->prev.uv;

	return (strcmp(ucv_string_get(v), "use strict") == 0);
}

static void
uc_compiler_compile_constant(uc_compiler_t *compiler)
{
	uc_function_t *fn;
	uint64_t u;

	switch (compiler->parser->prev.type) {
	case TK_THIS:
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LTHIS);
		break;

	case TK_NULL:
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LNULL);
		break;

	case TK_TRUE:
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LTRUE);
		break;

	case TK_FALSE:
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LFALSE);
		break;

	case TK_STRING:
		if (uc_compiler_is_use_strict_pragma(compiler)) {
			fn = (uc_function_t *)compiler->function;
			fn->strict = true;
		}

		/* fall through */

	case TK_DOUBLE:
		uc_compiler_emit_constant(compiler, compiler->parser->prev.pos, compiler->parser->prev.uv);
		break;

	case TK_REGEXP:
		uc_compiler_emit_regexp(compiler, compiler->parser->prev.pos, compiler->parser->prev.uv);
		break;

	case TK_NUMBER:
		u = ucv_uint64_get(compiler->parser->prev.uv);
		assert(errno == 0);

		if (u <= 0xff) {
			uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LOAD8);
			uc_compiler_emit_u8(compiler, compiler->parser->prev.pos, u);
		}
		else if (u <= 0xffff) {
			uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LOAD16);
			uc_compiler_emit_u16(compiler, compiler->parser->prev.pos, u);
		}
		else if (u <= 0xffffffff) {
			uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LOAD32);
			uc_compiler_emit_u32(compiler, compiler->parser->prev.pos, u);
		}
		else {
			uc_compiler_emit_constant(compiler, compiler->parser->prev.pos, compiler->parser->prev.uv);
		}

		break;

	default:
		break;
	}
}

static void
uc_compiler_compile_template(uc_compiler_t *compiler)
{
	uc_compiler_emit_constant(compiler, compiler->parser->prev.pos, compiler->parser->prev.uv);

	while (true) {
		if (uc_compiler_parse_check(compiler, TK_TEMPLATE)) {
			/* a TK_TEMPLATE token only continues the current literal when the
			 * previous one was left open by a `${...}` placeholder, in which
			 * case the lexer emits a TK_PLACEH token in between; two adjacent
			 * TK_TEMPLATE tokens are separate literals and must be joined
			 * with an explicit operator
			 */
			if (compiler->parser->prev.type == TK_TEMPLATE) {
				uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
					"Adjacent template literals are not implicitly concatenated");

				return;
			}

			uc_compiler_parse_advance(compiler);
			uc_compiler_emit_constant(compiler, compiler->parser->prev.pos, compiler->parser->prev.uv);
			uc_compiler_emit_insn(compiler, 0, I_ADD);
		}
		else if (uc_compiler_parse_match(compiler, TK_PLACEH)) {
			uc_compiler_compile_expression(compiler, true);
			uc_compiler_emit_insn(compiler, 0, I_ADD);
			uc_compiler_parse_consume(compiler, TK_RBRACE);
		}
		else {
			break;
		}
	}
}

static void
uc_compiler_compile_comma(uc_compiler_t *compiler)
{
	uc_compiler_emit_insn(compiler, compiler->parser->curr.pos, I_POP);
	uc_compiler_parse_precedence(compiler, P_ASSIGN);
}

static void
uc_compiler_compile_labelexpr(uc_compiler_t *compiler)
{
	uc_value_t *label = ucv_get(compiler->parser->prev.uv);

	uc_compiler_compile_var_or_arrowfn(compiler, label);
	ucv_put(label);
}

static uc_tokentype_t
uc_compiler_compile_delimitted_block(uc_compiler_t *compiler, uc_tokentype_t endtype)
{
	uc_tokentype_t last_statement_type = TK_NULL;

	while (!uc_compiler_parse_check(compiler, endtype) &&
	       !uc_compiler_parse_check(compiler, TK_EOF))
		last_statement_type = uc_compiler_compile_declaration(compiler);

	return uc_compiler_parse_check(compiler, endtype) ? last_statement_type : TK_EOF;
}

static void
uc_compiler_compile_funcexpr_common(uc_compiler_t *compiler, bool require_name)
{
	uc_tokentype_t last_statement_type = TK_NULL;
	uc_compiler_t fncompiler = { 0 };
	uc_value_t *name = NULL;
	ssize_t slot = -1, pos;
	uc_tokentype_t type;
	size_t i, load_off;
	uc_function_t *fn;

	pos = compiler->parser->prev.pos;
	type = compiler->parser->prev.type;

	if (uc_compiler_parse_match(compiler, TK_LABEL)) {
		name = compiler->parser->prev.uv;

		if (require_name && uc_compiler_parse_check(compiler, TK_SCOL)) {
			slot = uc_compiler_resolve_funcstub(compiler, name);

			if (slot > -1) {
				uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
					"Function '%s' redeclared", ucv_string_get(name));

				return;
			}

			slot = uc_compiler_declare_local(compiler, name, true);

			if (slot > -1) {
				uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
					"Variable '%s' redeclared", ucv_string_get(name));

				return;
			}

			compiler->locals.entries[compiler->locals.count - 1].funcstub = true;
			uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LNULL);
			uc_compiler_initialize_local(compiler);
			uc_compiler_parse_consume(compiler, TK_SCOL);

			return;
		}

		/* A function declaration binds its name in the enclosing scope. A named
		 * function expression must not: its name is only visible inside its own
		 * body (for self-reference), and declaring it in the enclosing scope
		 * would both leak it and shift the local slots, corrupting the
		 * initialisation state of a `let`/`const` the expression initialises. */
		if (require_name) {
			slot = uc_compiler_resolve_funcstub(compiler, name);

			if (slot > -1) {
				compiler->locals.entries[slot].funcstub = false;
			}
			else {
				slot = uc_compiler_declare_local(compiler, name, false);

				if (slot == -1)
					uc_compiler_initialize_local(compiler);
				else if (compiler->locals.entries[slot].constant)
					uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
						"Redeclaration of constant '%s'",
						ucv_string_get(name));
			}
		}
	}
	else if (require_name) {
		uc_compiler_syntax_error(compiler, compiler->parser->curr.pos, "Expecting function name");
	}

	uc_compiler_init(&fncompiler,
		name ? ucv_string_get(name) : NULL,
		uc_compiler_current_source(compiler),
		compiler->parser->prev.pos,
		compiler->program,
		uc_compiler_is_strict(compiler));

	/* Bind a named function expression's name to its own callee slot so the
	 * body can reference itself for recursion, without touching the enclosing
	 * scope. Declarations keep the name in the enclosing scope (handled above). */
	if (name && !require_name) {
		ucv_put(fncompiler.locals.entries[0].name);
		fncompiler.locals.entries[0].name = ucv_get(name);
	}

	fncompiler.parent = compiler;
	fncompiler.parser = compiler->parser;
	fncompiler.exprstack = compiler->exprstack;
	fn = (uc_function_t *)fncompiler.function;

	uc_compiler_parse_consume(&fncompiler, TK_LPAREN);

	uc_compiler_enter_scope(&fncompiler);

	/* compile argument specification */
	while (true) {
		if (uc_compiler_parse_check(&fncompiler, TK_RPAREN))
			break;

		if (uc_compiler_parse_match(&fncompiler, TK_ELLIP))
			fn->vararg = true;

		if (uc_compiler_parse_match(&fncompiler, TK_LABEL)) {
			fn->nargs++;

			uc_compiler_declare_local(&fncompiler, fncompiler.parser->prev.uv, false);
			uc_compiler_initialize_local(&fncompiler);

			if (fn->vararg ||
			    !uc_compiler_parse_match(&fncompiler, TK_COMMA))
				break;
		}
		else {
			uc_compiler_syntax_error(&fncompiler, fncompiler.parser->curr.pos,
				"Expecting Label");

			break;
		}
	}

	uc_compiler_parse_consume(&fncompiler, TK_RPAREN);

	/* parse and compile function body */
	if (uc_compiler_parse_match(&fncompiler, TK_COLON)) {
		last_statement_type = uc_compiler_compile_delimitted_block(&fncompiler, TK_ENDFUNC);
		uc_compiler_parse_consume(&fncompiler, TK_ENDFUNC);
	}
	else if (uc_compiler_parse_match(&fncompiler, TK_LBRACE)) {
		last_statement_type = uc_compiler_compile_delimitted_block(&fncompiler, TK_RBRACE);
		uc_compiler_parse_consume(&fncompiler, TK_RBRACE);
	}
	else {
		uc_compiler_syntax_error(&fncompiler, fncompiler.parser->curr.pos,
			"Expecting '{' or ':' after function parameters");
	}

	/* emit load instruction for function value */
	uc_compiler_emit_insn(compiler, pos, (type == TK_ARROW) ? I_ARFN : I_CLFN);
	load_off = uc_compiler_emit_u32(compiler, 0, 0);

	/* encode upvalue information */
	for (i = 0; i < fn->nupvals; i++)
		uc_compiler_emit_s32(compiler, 0,
			fncompiler.upvals.entries[i].local
				? -(fncompiler.upvals.entries[i].index + 1)
				: fncompiler.upvals.entries[i].index);

	/* finalize function compiler */
	fn = uc_compiler_finish(&fncompiler, last_statement_type);

	if (fn)
		uc_compiler_set_u32(compiler, load_off,
			uc_program_function_id(compiler->program, fn));

	/* if a local variable of the same name already existed, overwrite its value
	 * with the compiled function here */
	if (slot != -1) {
		uc_compiler_emit_insn(compiler, 0, I_SLOC);
		uc_compiler_emit_u32(compiler, 0, slot);
		uc_compiler_emit_insn(compiler, 0, I_POP);
	}
}

static void
uc_compiler_compile_funcexpr(uc_compiler_t *compiler)
{
	return uc_compiler_compile_funcexpr_common(compiler, false);
}

static void
uc_compiler_compile_funcdecl(uc_compiler_t *compiler)
{
	return uc_compiler_compile_funcexpr_common(compiler, true);
}

static void
uc_compiler_compile_dot(uc_compiler_t *compiler)
{
	bool optional_chaining = (compiler->parser->prev.type == TK_QDOT);

	/* no regexp literal possible after property access */
	compiler->parser->lex.no_regexp = true;

	if (optional_chaining)
		uc_compiler_emit_jmpnt(compiler, compiler->parser->prev.pos,
			(1u << UC_ARRAY) | (1u << UC_OBJECT) | (1u << UC_RESOURCE), 0);

	/* parse label lhs */
	uc_compiler_parse_consume(compiler, TK_LABEL);
	uc_compiler_emit_constant(compiler, compiler->parser->prev.pos, compiler->parser->prev.uv);

	/* depending on context, compile into I_UVAL, I_SVAL or I_LVAL operation */
	if (!uc_compiler_exprstack_is(compiler, F_ASSIGNABLE) || !uc_compiler_compile_assignment(compiler, NULL))
		uc_compiler_emit_variable_rw(compiler, NULL, optional_chaining ? TK_QDOT : 0);

	compiler->patchlist->depth = uc_compiler_current_chunk(compiler)->count;
}

static void
uc_compiler_compile_subscript(uc_compiler_t *compiler)
{
	bool optional_chaining = (compiler->parser->prev.type == TK_QLBRACK);

	if (optional_chaining)
		uc_compiler_emit_jmpnt(compiler, compiler->parser->prev.pos,
			(1u << UC_ARRAY) | (1u << UC_OBJECT) | (1u << UC_RESOURCE), 0);

	/* compile lhs */
	uc_compiler_compile_expression(compiler, false);

	/* no regexp literal possible after computed property access */
	compiler->parser->lex.no_regexp = true;
	uc_compiler_parse_consume(compiler, TK_RBRACK);

	/* depending on context, compile into I_UVAL, I_SVAL or I_LVAL operation */
	if (!uc_compiler_exprstack_is(compiler, F_ASSIGNABLE) || !uc_compiler_compile_assignment(compiler, NULL))
		uc_compiler_emit_variable_rw(compiler, NULL, optional_chaining ? TK_QLBRACK : 0);

	compiler->patchlist->depth = uc_compiler_current_chunk(compiler)->count;
}

static void
uc_compiler_compile_ternary(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off, jmp_off;

	/* jump to false branch */
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);

	/* compile true branch */
	uc_compiler_parse_precedence(compiler, P_ASSIGN);

	/* jump after false branch */
	jmp_off = uc_compiler_emit_jmp(compiler, 0);

	uc_compiler_parse_consume(compiler, TK_COLON);

	/* compile false branch */
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
	uc_compiler_parse_precedence(compiler, P_TERNARY);
	uc_compiler_set_jmpaddr(compiler, jmp_off, chunk->count);
}

static void
uc_compiler_compile_array(uc_compiler_t *compiler)
{
	size_t hint_off, hint_count = 0, len = 0;

	/* create empty array on stack */
	uc_compiler_emit_insn(compiler, 0, I_NARR);
	hint_off = uc_compiler_emit_u32(compiler, 0, 0);

	/* parse initializer values */
	do {
		if (uc_compiler_parse_check(compiler, TK_RBRACK)) {
			break;
		}
		else if (uc_compiler_parse_match(compiler, TK_ELLIP)) {
			/* push items on stack so far... */
			if (len > 0) {
				uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_PARR);
				uc_compiler_emit_u32(compiler, 0, len);
				len = 0;
			}

			/* compile spread value expression */
			uc_compiler_parse_precedence(compiler, P_ASSIGN);

			/* emit merge operation */
			uc_compiler_emit_insn(compiler, 0, I_MARR);
		}
		else {
			/* push items on stack so far... */
			if (len >= 0xffffffff) {
				uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_PARR);
				uc_compiler_emit_u32(compiler, 0, len);
				len = 0;
			}

			/* compile item value expression */
			uc_compiler_parse_precedence(compiler, P_ASSIGN);

			hint_count++;
			len++;
		}
	}
	while (uc_compiler_parse_match(compiler, TK_COMMA));

	/* no regexp literal possible after array literal */
	compiler->parser->lex.no_regexp = true;
	uc_compiler_parse_consume(compiler, TK_RBRACK);

	/* push items on stack */
	if (len > 0) {
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_PARR);
		uc_compiler_emit_u32(compiler, 0, len);
	}

	/* set initial size hint */
	uc_compiler_set_u32(compiler, hint_off, hint_count);
}

static void
uc_compiler_compile_method(uc_compiler_t *compiler, uc_value_t *method_name)
{
	uc_compiler_t fncompiler = { 0 };
	uc_function_t *fn;
	size_t i, load_off;

	uc_compiler_init(&fncompiler, method_name ? ucv_string_get(method_name) : NULL,
	                 uc_compiler_current_source(compiler),
	                 compiler->parser->prev.pos,
	                 compiler->program,
	                 uc_compiler_is_strict(compiler));

	fncompiler.parent = compiler;
	fncompiler.parser = compiler->parser;
	fncompiler.exprstack = compiler->exprstack;

	fn = (uc_function_t *)fncompiler.function;

	/* parse parameters - '(' is already consumed, expect parameters or ')' */
	uc_compiler_enter_scope(&fncompiler);

	while (true) {
		if (uc_compiler_parse_check(&fncompiler, TK_RPAREN))
			break;

		if (uc_compiler_parse_match(&fncompiler, TK_ELLIP))
			fn->vararg = true;

		uc_compiler_parse_consume(&fncompiler, TK_LABEL);
		fn->nargs++;

		uc_compiler_declare_local(&fncompiler,
			fncompiler.parser->prev.uv, false);
		uc_compiler_initialize_local(&fncompiler);

		if (fn->vararg ||
		    !uc_compiler_parse_match(&fncompiler, TK_COMMA))
			break;
	}

	uc_compiler_parse_consume(&fncompiler, TK_RPAREN);

	/* parse function body - must be a block */
	uc_compiler_parse_consume(&fncompiler, TK_LBRACE);

	while (!uc_compiler_parse_check(&fncompiler, TK_RBRACE) &&
	       !uc_compiler_parse_check(&fncompiler, TK_EOF))
		uc_compiler_compile_declaration(&fncompiler);

	uc_compiler_parse_consume(&fncompiler, TK_RBRACE);

	/* emit load instruction for method */
	uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_CLFN);
	load_off = uc_compiler_emit_u32(compiler, 0, 0);

	/* encode upvalue information */
	for (i = 0; i < fn->nupvals; i++)
		uc_compiler_emit_s32(compiler, 0,
			fncompiler.upvals.entries[i].local
				? -(fncompiler.upvals.entries[i].index + 1)
				: fncompiler.upvals.entries[i].index);

	/* finalize function compiler */
	fn = uc_compiler_finish(&fncompiler, TK_RETURN);

	if (fn)
		uc_compiler_set_u32(compiler, load_off,
			uc_program_function_id(compiler->program, fn));
}


static void
uc_compiler_compile_object(uc_compiler_t *compiler)
{
	size_t hint_off, hint_count = 0, len = 0;

	/* create empty object on stack */
	uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_NOBJ);
	hint_off = uc_compiler_emit_u32(compiler, 0, 0);

	/* parse initializer values */
	do {
		/* End of object literal */
		if (uc_compiler_parse_check(compiler, TK_RBRACE))
			break;

		/* Spread operator */
		if (uc_compiler_parse_match(compiler, TK_ELLIP)) {
			/* set items on stack so far... */
			if (len > 0) {
				uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_SOBJ);
				uc_compiler_emit_u32(compiler, 0, len);
				len = 0;
			}

			/* compile spread value expression */
			uc_compiler_parse_precedence(compiler, P_ASSIGN);

			/* emit merge operation */
			uc_compiler_emit_insn(compiler, 0, I_MOBJ);

			compiler->parser->lex.no_keyword = true;
			continue;
		}

		/* Computed property/method name */
		if (uc_compiler_parse_match(compiler, TK_LBRACK)) {
			/* parse property name expression */
			uc_compiler_parse_precedence(compiler, P_ASSIGN);

			/* consume closing bracket */
			uc_compiler_parse_consume(compiler, TK_RBRACK);

			/* Check if this is a computed method: [expr]() { ... } */
			if (uc_compiler_parse_match(compiler, TK_LPAREN)) {
				/* compile method - key is already on stack from [expr] */
				uc_compiler_compile_method(compiler, NULL);
			}
			else {
				/* regular computed property: [expr]: value */
				uc_compiler_parse_consume(compiler, TK_COLON);
				uc_compiler_parse_precedence(compiler, P_ASSIGN);
			}
		}

		/* Property/method declaration or property shorthand */
		else {
			/* parse key expression */
			uc_tokentype_t key_type = compiler->parser->curr.type;
			if (!uc_compiler_parse_match(compiler, TK_LABEL) &&
			    !uc_compiler_parse_match(compiler, TK_STRING))
				uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
					"Expecting label");

			/* hold a reference to the key: matching TK_LPAREN below advances
			 * the parser, which releases the underlying prev.uv token value */
			uc_value_t *key = ucv_get(compiler->parser->prev.uv);

			/* load label */
			uc_compiler_emit_constant(compiler, compiler->parser->prev.pos,
				key);

			/* Check if this is a shorthand method: foo() { ... } */
			if (uc_compiler_parse_match(compiler, TK_LPAREN)) {
				/* disallow keywords as method names */
				if (uc_lexer_is_keyword(key))
					uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
						"Invalid identifier");

				uc_compiler_compile_method(compiler, key);
			}
			/* Check if this is property shorthand: foo */
			else if (key_type == TK_LABEL &&
			         (uc_compiler_parse_check(compiler, TK_COMMA) ||
			          uc_compiler_parse_check(compiler, TK_RBRACE))) {
				/* disallow keywords in this case */
				if (uc_lexer_is_keyword(key))
					uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
						"Invalid identifier");

				uc_compiler_emit_variable_rw(compiler, key, 0);
			}
			/* ... otherwise treat it as ordinary `key: value` tuple */
			else {
				uc_compiler_parse_consume(compiler, TK_COLON);

				/* parse value expression */
				uc_compiler_parse_precedence(compiler, P_ASSIGN);
			}

			ucv_put(key);
		}

		/* set items on stack so far... */
		if (len >= 0xfffffffe) {
			uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_SOBJ);
			uc_compiler_emit_u32(compiler, 0, len);
			len = 0;
		}

		hint_count += 2;
		len += 2;

		compiler->parser->lex.no_keyword = true;
	}
	while (uc_compiler_parse_match(compiler, TK_COMMA));

	/* no regexp literal possible after object literal */
	compiler->parser->lex.no_regexp = true;
	uc_compiler_parse_consume(compiler, TK_RBRACE);

	/* set items on stack */
	if (len > 0) {
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_SOBJ);
		uc_compiler_emit_u32(compiler, 0, len);
	}

	/* set initial size hint */
	uc_compiler_set_u32(compiler, hint_off, hint_count);
}


static void
uc_compiler_declare_local_null(uc_compiler_t *compiler, size_t srcpos, uc_value_t *varname)
{
	ssize_t existing_slot = uc_compiler_declare_local(compiler, varname, false);

	uc_compiler_emit_insn(compiler, srcpos, I_LNULL);

	if (existing_slot == -1) {
		uc_compiler_initialize_local(compiler);
	}
	else {
		uc_compiler_emit_insn(compiler, 0, I_SLOC);
		uc_compiler_emit_u32(compiler, 0, existing_slot);
		uc_compiler_emit_insn(compiler, 0, I_POP);
	}
}

static size_t
uc_compiler_declare_internal(uc_compiler_t *compiler, size_t srcpos, const char *name)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_locals_t *locals = &compiler->locals;

	uc_vector_push(locals, {
		.name = ucv_string_new(name),
		.depth = compiler->scope_depth,
		.captured = false,
		.from = chunk->count
	});

	return locals->count - 1;
}

static void
uc_compiler_compile_declexpr(uc_compiler_t *compiler, bool constant)
{
	ssize_t slot;

	do {
		/* parse variable name */
		if (!uc_compiler_parse_match(compiler, TK_LABEL)) {
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
				"Expecting variable name");

			return;
		}

		/* declare local variable */
		slot = uc_compiler_declare_local(compiler, compiler->parser->prev.uv, constant);

		/* if followed by '=', parse initializer expression */
		if (uc_compiler_parse_match(compiler, TK_ASSIGN))
			uc_compiler_parse_precedence(compiler, P_ASSIGN);
		/* otherwise, for writable variables, load implicit null */
		else if (!constant)
			uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LNULL);
		/* for constant variables, a missing initializer is a syntax error */
		else
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Expecting initializer expression");

		/* initialize local */
		if (slot == -1) {
			uc_compiler_initialize_local(compiler);
		}
		/* if the variable was redeclared, overwrite it */
		else {
			uc_compiler_emit_insn(compiler, 0, I_SLOC);
			uc_compiler_emit_u32(compiler, 0, slot);
			uc_compiler_emit_insn(compiler, 0, I_POP);
		}
	}
	while (uc_compiler_parse_match(compiler, TK_COMMA));
}

static void
uc_compiler_compile_local(uc_compiler_t *compiler)
{
	uc_compiler_emit_stmt_start(compiler, &compiler->parser->prev);
	uc_compiler_compile_declexpr(compiler, false);
	uc_compiler_parse_consume(compiler, TK_SCOL);
	uc_compiler_emit_stmt_end(compiler);
}

static void
uc_compiler_compile_const(uc_compiler_t *compiler)
{
	uc_compiler_emit_stmt_start(compiler, &compiler->parser->prev);
	uc_compiler_compile_declexpr(compiler, true);
	uc_compiler_parse_consume(compiler, TK_SCOL);
	uc_compiler_emit_stmt_end(compiler);
}

static uc_tokentype_t
uc_compiler_compile_altifblock(uc_compiler_t *compiler)
{
	uc_compiler_enter_scope(compiler);

	while (true) {
		switch (compiler->parser->curr.type) {
		case TK_ELIF:
		case TK_ELSE:
		case TK_ENDIF:
		case TK_EOF:
			uc_compiler_leave_scope(compiler);

			return compiler->parser->curr.type;

		default:
			uc_compiler_compile_declaration(compiler);
			break;
		}
	}

	return 0;
}

static void
uc_compiler_compile_if(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t jmpz_off, jmp_off, i;
	bool expect_endif = false;
	uc_jmplist_t elifs = { 0 };
	uc_tokentype_t type;

	/* parse & compile condition expression */
	uc_compiler_parse_consume(compiler, TK_LPAREN);
	uc_compiler_compile_expression(compiler, true);
	uc_compiler_parse_consume(compiler, TK_RPAREN);

	/* conditional jump to else/elif branch */
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);

	if (uc_compiler_parse_match(compiler, TK_COLON)) {
		compiler->exprstack->flags |= F_ALTBLOCKMODE;

		while (true) {
			/* compile elsif or else branch */
			type = uc_compiler_compile_altifblock(compiler);

			/* we just compiled an elsif block */
			if (!expect_endif && type == TK_ELIF) {
				/* emit jump to skip to the end */
				uc_vector_push(&elifs, uc_compiler_emit_jmp(compiler, 0));

				/* point previous conditional jump to beginning of branch */
				uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);

				/* parse & compile elsif condition */
				uc_compiler_parse_advance(compiler);
				uc_compiler_parse_consume(compiler, TK_LPAREN);
				uc_compiler_compile_expression(compiler, true);
				uc_compiler_parse_consume(compiler, TK_RPAREN);
				uc_compiler_parse_consume(compiler, TK_COLON);

				/* conditional jump to else/elif branch */
				jmpz_off = uc_compiler_emit_jmpz(compiler, 0);
			}
			else if (!expect_endif && type == TK_ELSE) {
				/* emit jump to skip to the end */
				uc_vector_push(&elifs, uc_compiler_emit_jmp(compiler, 0));

				/* point previous conditional jump to beginning of branch */
				uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
				jmpz_off = 0;

				/* skip "else" keyword */
				uc_compiler_parse_advance(compiler);

				expect_endif = true;
			}
			else if (type == TK_ENDIF) {
				/* if no else clause, point previous conditional jump after block */
				if (jmpz_off)
					uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);

				/* patch the elif branch jumps to point here after the else */
				for (i = 0; i < elifs.count; i++)
					uc_compiler_set_jmpaddr(compiler, elifs.entries[i],
						chunk->count);

				/* skip the "endif" keyword */
				uc_compiler_parse_advance(compiler);
				break;
			}
			else {
				uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
					expect_endif
						? "Expecting 'endif'"
						: "Expecting 'elif', 'else' or 'endif'");

				break;
			}
		}

		uc_vector_clear(&elifs);
	}
	else {
		/* compile true branch */
		uc_compiler_compile_statement(compiler);

		/* ... when present, handle false branch */
		if (uc_compiler_parse_match(compiler, TK_ELSE)) {
			/* jump to skip else branch */
			jmp_off = uc_compiler_emit_jmp(compiler, 0);

			/* set conditional jump address */
			uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);

			/* compile false branch */
			uc_compiler_compile_statement(compiler);

			/* set else skip jump address */
			uc_compiler_set_jmpaddr(compiler, jmp_off, chunk->count);
		}
		/* ... otherwise point the conditional jump after the true branch */
		else {
			uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);
		}
	}
}

static void
uc_compiler_compile_while(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_patchlist_t p = { .depth = compiler->scope_depth, .token = TK_WHILE };
	size_t cond_off, jmpz_off, end_off;

	p.parent = compiler->patchlist;
	compiler->patchlist = &p;

	cond_off = chunk->count;

	/* parse & compile loop condition */
	uc_compiler_parse_consume(compiler, TK_LPAREN);
	uc_compiler_compile_expression(compiler, true);
	uc_compiler_parse_consume(compiler, TK_RPAREN);

	/* conditional jump to end */
	jmpz_off = uc_compiler_emit_jmpz(compiler, 0);

	/* compile loop body */
	if (uc_compiler_parse_match(compiler, TK_COLON)) {
		uc_compiler_enter_scope(compiler);

		if (uc_compiler_compile_delimitted_block(compiler, TK_ENDWHILE) == TK_EOF)
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
				"Expecting 'endwhile'");
		else
			uc_compiler_parse_advance(compiler);

		uc_compiler_leave_scope(compiler);
	}
	else {
		uc_compiler_compile_statement(compiler);
	}

	end_off = chunk->count;

	/* jump back to condition */
	uc_compiler_emit_jmp_dest(compiler, 0, cond_off);

	/* set conditional jump target */
	uc_compiler_set_jmpaddr(compiler, jmpz_off, chunk->count);

	/* patch up break/continue */
	uc_compiler_backpatch(compiler, chunk->count, end_off);
}

static void
uc_compiler_compile_for_in(uc_compiler_t *compiler, bool local, uc_token_t *kvar, uc_token_t *vvar)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_patchlist_t p = { .depth = compiler->scope_depth + 1, .token = TK_FOR };
	size_t skip_jmp, test_jmp, key_slot, val_slot;

	p.parent = compiler->patchlist;
	compiler->patchlist = &p;

	uc_compiler_enter_scope(compiler);

	/* declare internal loop variables */
	uc_compiler_emit_insn(compiler, 0, I_LNULL);
	key_slot = uc_compiler_declare_internal(compiler, 0, "(for in key)");

	uc_compiler_emit_insn(compiler, 0, I_LNULL);
	val_slot = uc_compiler_declare_internal(compiler, 0, "(for in value)");

	/* declare loop variables */
	if (local) {
		uc_compiler_declare_local_null(compiler, kvar->pos, kvar->uv);

		if (vvar)
			uc_compiler_declare_local_null(compiler, vvar->pos, vvar->uv);
	}

	/* value to iterate */
	uc_compiler_compile_expression(compiler, false);
	uc_compiler_emit_stmt_end(compiler);
	uc_compiler_parse_consume(compiler, TK_RPAREN);
	uc_compiler_emit_insn(compiler, 0, I_SLOC);
	uc_compiler_emit_u32(compiler, 0, val_slot);

	/* initial key value */
	uc_compiler_emit_insn(compiler, 0, I_LNULL);
	uc_compiler_emit_insn(compiler, 0, I_SLOC);
	uc_compiler_emit_u32(compiler, 0, key_slot);

	/* jump over variable read for first cycle */
	skip_jmp = uc_compiler_emit_jmp(compiler, 0);

	/* read value */
	uc_compiler_emit_insn(compiler, 0, I_LLOC);
	uc_compiler_emit_u32(compiler, 0, val_slot);

	/* read key */
	uc_compiler_emit_insn(compiler, 0, I_LLOC);
	uc_compiler_emit_u32(compiler, 0, key_slot);

	/* backpatch skip jump */
	uc_compiler_set_jmpaddr(compiler, skip_jmp, chunk->count);

	/* load loop variable and get next key from object */
	uc_compiler_emit_insn(compiler, 0, vvar ? I_NEXTKV : I_NEXTK);

	/* set internal key variable */
	uc_compiler_emit_insn(compiler, 0, I_SLOC);
	uc_compiler_emit_u32(compiler, 0, key_slot);

	/* test for != null */
	uc_compiler_emit_insn(compiler, 0, I_LNULL);
	uc_compiler_emit_insn(compiler, 0, I_NES);

	/* jump after loop body if no next key */
	test_jmp = uc_compiler_emit_jmpz(compiler, 0);

	/* set key and value variables */
	if (vvar) {
		uc_compiler_emit_variable_rw(compiler, vvar->uv, TK_ASSIGN);
		uc_compiler_emit_insn(compiler, 0, I_POP);
	}

	/* set key variable */
	uc_compiler_emit_variable_rw(compiler, kvar->uv, TK_ASSIGN);
	uc_compiler_emit_insn(compiler, 0, I_POP);

	/* compile loop body */
	if (uc_compiler_parse_match(compiler, TK_COLON)) {
		uc_compiler_enter_scope(compiler);

		if (uc_compiler_compile_delimitted_block(compiler, TK_ENDFOR) == TK_EOF)
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
				"Expecting 'endfor'");
		else
			uc_compiler_parse_advance(compiler);

		uc_compiler_leave_scope(compiler);
	}
	else {
		uc_compiler_compile_statement(compiler);
	}

	/* jump back to retrieve next key */
	uc_compiler_emit_jmp_dest(compiler, 0, skip_jmp + 5);

	/* back patch conditional jump */
	uc_compiler_set_jmpaddr(compiler, test_jmp, chunk->count);

	/* pop loop variables */
	uc_compiler_emit_insn(compiler, 0, I_POP);

	if (vvar)
		uc_compiler_emit_insn(compiler, 0, I_POP);

	/* patch up break/continue */
	uc_compiler_backpatch(compiler, chunk->count, skip_jmp + 5);

	uc_compiler_leave_scope(compiler);
}

static void
uc_compiler_compile_for_count(uc_compiler_t *compiler, bool local, uc_token_t *predecl, uc_token_t *var)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t test_off = 0, incr_off, skip_off, cond_off = 0;
	uc_patchlist_t p = { .depth = compiler->scope_depth + 1, .token = TK_FOR };

	p.parent = compiler->patchlist;
	compiler->patchlist = &p;

	uc_compiler_enter_scope(compiler);

	/* Initializer ---------------------------------------------------------- */

	/* A leading declaration without initializer, e.g. the `x` in
	 * `for (let x, y = 0; ...)`, was consumed while disambiguating for-in */
	if (predecl && local)
		uc_compiler_declare_local_null(compiler, predecl->pos, predecl->uv);

	/* If we parsed at least one label, try continue parsing as variable
	 * expression... */
	if (var) {
		/* We parsed a `local x` or `local x, y` expression, so (re)declare
		 * last label as local initializer variable */
		if (local)
			uc_compiler_declare_local_null(compiler, var->pos, var->uv);

		uc_compiler_exprstack_push(compiler, TK_FOR, F_ASSIGNABLE);

		uc_compiler_compile_labelexpr(compiler);
		uc_compiler_emit_insn(compiler, 0, I_POP);

		/* If followed by a comma, continue parsing expression */
		if (uc_compiler_parse_match(compiler, TK_COMMA)) {
			/* Is a continuation of a declaration list... */
			if (local) {
				uc_compiler_compile_declexpr(compiler, false);
			}
			/* ... otherwise an unrelated expression */
			else {
				uc_compiler_compile_expression(compiler, false);
				uc_compiler_emit_insn(compiler, 0, I_POP);
			}
		}

		uc_compiler_exprstack_pop(compiler);
	}
	/* ... otherwise try parsing an entire expression (which might be absent) */
	else if (!uc_compiler_parse_check(compiler, TK_SCOL)) {
		uc_compiler_compile_expression(compiler, false);
		uc_compiler_emit_insn(compiler, 0, I_POP);
	}

	uc_compiler_emit_stmt_end(compiler);
	uc_compiler_parse_consume(compiler, TK_SCOL);


	/* Condition ------------------------------------------------------------ */
	if (!uc_compiler_parse_check(compiler, TK_SCOL)) {
		cond_off = chunk->count;

		uc_compiler_compile_expression(compiler, true);

		test_off = uc_compiler_emit_jmpz(compiler, 0);
	}

	uc_compiler_parse_consume(compiler, TK_SCOL);

	/* jump over incrementer */
	skip_off = uc_compiler_emit_jmp(compiler, 0);


	/* Incrementer ---------------------------------------------------------- */
	incr_off = chunk->count;

	if (!uc_compiler_parse_check(compiler, TK_RPAREN)) {
		uc_compiler_compile_expression(compiler, true);
		uc_compiler_emit_insn(compiler, 0, I_POP);
	}

	uc_compiler_parse_consume(compiler, TK_RPAREN);

	/* if we have a condition, jump back to it, else continue to the loop body */
	if (test_off)
		uc_compiler_emit_jmp_dest(compiler, 0, cond_off);

	/* back patch skip address */
	uc_compiler_set_jmpaddr(compiler, skip_off, chunk->count);


	/* Body ----------------------------------------------------------------- */
	if (uc_compiler_parse_match(compiler, TK_COLON)) {
		uc_compiler_enter_scope(compiler);

		if (uc_compiler_compile_delimitted_block(compiler, TK_ENDFOR) == TK_EOF)
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
				"Expecting 'endfor'");
		else
			uc_compiler_parse_advance(compiler);

		uc_compiler_leave_scope(compiler);
	}
	else {
		uc_compiler_compile_statement(compiler);
	}

	/* jump back to incrementer */
	uc_compiler_emit_jmp_dest(compiler, 0, incr_off);

	/* back patch conditional jump */
	if (test_off)
		uc_compiler_set_jmpaddr(compiler, test_off, chunk->count);

	/* patch up break/continue */
	uc_compiler_backpatch(compiler, chunk->count, incr_off);

	uc_compiler_leave_scope(compiler);
}

static void
uc_compiler_compile_for(uc_compiler_t *compiler)
{
	uc_token_t keyvar = { 0 }, valvar = { 0 };
	bool local;

	uc_compiler_parse_consume(compiler, TK_LPAREN);

	uc_compiler_emit_stmt_start(compiler, &compiler->parser->curr);

	/* check the next few tokens and see if we have either a
	 * `let x in` / `let x, y` expression or an ordinary initializer
	 * statement */

	local = uc_compiler_parse_match(compiler, TK_LOCAL);

	if (uc_compiler_parse_match(compiler, TK_LABEL)) {
		keyvar = compiler->parser->prev;
		ucv_get(keyvar.uv);

		if (uc_compiler_parse_match(compiler, TK_COMMA)) {
			uc_compiler_parse_consume(compiler, TK_LABEL);

			valvar = compiler->parser->prev;
			ucv_get(valvar.uv);
		}

		/* is a for-in loop */
		if (uc_compiler_parse_match(compiler, TK_IN)) {
			uc_compiler_compile_for_in(compiler, local, &keyvar,
				valvar.type ? &valvar : NULL);

			goto out;
		}
	}
	else if (local) {
		uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
			"Expecting label after 'local'");

		goto out;
	}

	/*
	 * The previous expression ruled out a for-in loop, so continue parsing
	 * as counting for loop...
	 */
	if (valvar.uv)
		uc_compiler_compile_for_count(compiler, local,
			keyvar.uv ? &keyvar : NULL, &valvar);
	else
		uc_compiler_compile_for_count(compiler, local,
			NULL, keyvar.uv ? &keyvar : NULL);

out:
	ucv_put(keyvar.uv);
	ucv_put(valvar.uv);
}

static void
uc_compiler_compile_switch(uc_compiler_t *compiler)
{
	size_t i, test_jmp, skip_jmp, next_jmp = 0, value_slot, default_off = 0;
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_patchlist_t p = { .depth = compiler->scope_depth, .token = TK_SWITCH };
	uc_locals_t *locals = &compiler->locals;
	uc_jmplist_t cases = { 0 };

	p.parent = compiler->patchlist;
	compiler->patchlist = &p;

	uc_compiler_enter_scope(compiler);

	/* parse and compile match value */
	uc_compiler_parse_consume(compiler, TK_LPAREN);
	uc_compiler_compile_expression(compiler, true);
	uc_compiler_parse_consume(compiler, TK_RPAREN);
	uc_compiler_parse_consume(compiler, TK_LBRACE);

	value_slot = uc_compiler_declare_internal(compiler, 0, "(switch value)");

	/* jump to branch tests */
	test_jmp = uc_compiler_emit_jmp(compiler, 0);

	/* parse and compile case matches */
	while (!uc_compiler_parse_check(compiler, TK_RBRACE) &&
	       !uc_compiler_parse_check(compiler, TK_EOF)) {
		/* handle `default:` */
		if (uc_compiler_parse_match(compiler, TK_DEFAULT)) {
			if (default_off) {
				uc_vector_clear(&cases);
				uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
					"more than one switch default case");

				break;
			}

			uc_compiler_parse_consume(compiler, TK_COLON);

			/* remember address of default branch */
			default_off = chunk->count;

			/* Store three values in case offset list:
			 *  1) amount of local variables declared so far
			 *  2) beginning of condition expression
			 *  3) end of condition expression
			 * For the `default` case, beginning and end offsets of the
			 * condition expression are equal.
			 */
			uc_vector_extend(&cases, 3);
			cases.entries[cases.count++] = (locals->count - 1) - value_slot;
			cases.entries[cases.count++] = chunk->count;
			cases.entries[cases.count++] = chunk->count;
		}

		/* handle `case …:` */
		else if (uc_compiler_parse_match(compiler, TK_CASE)) {
			/* jump over `case …:` label expression */
			skip_jmp = uc_compiler_emit_jmp(compiler, 0);

			/* compile case value expression */
			uc_compiler_compile_expression(compiler, false);
			uc_compiler_parse_consume(compiler, TK_COLON);

			/* Store three values in case offset list:
			 *  1) amount of local variables declared so far
			 *  2) beginning of condition expression
			 *  3) end of condition expression
			 */
			uc_vector_extend(&cases, 3);
			cases.entries[cases.count++] = (locals->count - 1) - value_slot;
			cases.entries[cases.count++] = skip_jmp + 5;
			cases.entries[cases.count++] = uc_compiler_emit_jmp(compiler, 0);

			/* patch jump skipping over the case value */
			uc_compiler_set_jmpaddr(compiler, skip_jmp, chunk->count);
		}

		/* handle interleaved statement */
		else if (cases.count) {
			uc_compiler_compile_declaration(compiler);
		}

		/* a statement or expression preceding any `default` or `case` is a
		 * syntax error */
		else {
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
				"Expecting 'case' or 'default'");

			break;
		}
	}

	uc_compiler_parse_consume(compiler, TK_RBRACE);

	/* evaluate case matches */
	if (cases.count) {
		skip_jmp = uc_compiler_emit_jmp(compiler, 0);

		uc_compiler_set_jmpaddr(compiler, test_jmp, chunk->count);

		for (i = 0, default_off = cases.count; i < cases.count; i += 3) {
			/* remember and skip default case */
			if (cases.entries[i + 1] == cases.entries[i + 2]) {
				default_off = i;
				continue;
			}

			/* read switch match value */
			uc_compiler_emit_insn(compiler, 0, I_LLOC);
			uc_compiler_emit_u32(compiler, 0, value_slot);

			/* jump to case value expression code */
			uc_compiler_emit_jmp_dest(compiler, 0, cases.entries[i + 1]);

			/* patch final case value expression jump back here */
			uc_compiler_set_jmpaddr(compiler, cases.entries[i + 2], chunk->count);

			/* strict equal test */
			uc_compiler_emit_insn(compiler, 0, I_EQS);

			/* conditional jump to next match */
			next_jmp = uc_compiler_emit_jmpz(compiler, 0);

			/* fill local slots */
			while (cases.entries[i + 0] > 0) {
				uc_compiler_emit_insn(compiler, 0, I_LNULL);
				cases.entries[i + 0]--;
			}

			/* jump to target code */
			uc_compiler_emit_jmp_dest(compiler, 0, cases.entries[i + 2] + 5);

			/* patch next jump */
			uc_compiler_set_jmpaddr(compiler, next_jmp, chunk->count);
		}

		/* handle default case (if any) */
		if (default_off < cases.count) {
			/* fill local slots */
			while (cases.entries[default_off + 0] > 0) {
				uc_compiler_emit_insn(compiler, 0, I_LNULL);
				cases.entries[default_off + 0]--;
			}

			/* jump to target */
			uc_compiler_emit_jmp_dest(compiler, 0, cases.entries[default_off + 2]);

			/* do not patch final match failure jump later, we handle it here
			 * in the default case */
			next_jmp = 0;
		}

		uc_compiler_set_jmpaddr(compiler, skip_jmp, chunk->count);
	}
	else {
		uc_compiler_set_jmpaddr(compiler, test_jmp, test_jmp + 5);
	}

	uc_vector_clear(&cases);

	uc_compiler_leave_scope(compiler);

	/* if no default case exists, patch last case match failure jump */
	if (next_jmp) {
		/* There's pop instructions for all local variables including the
		 * switch test value itself on the stack. Jump onto the last POP
		 * instruction (-1) to get rid of the on-stack switch test value
		 * but skip the POP instructions for all other scoped local variables
		 * which never have been initialized. */
		uc_compiler_set_jmpaddr(compiler, next_jmp, chunk->count - 1);
	}

	uc_compiler_backpatch(compiler, chunk->count, 0);
}

static void
uc_compiler_compile_try(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t try_from = 0, try_to = 0, jmp_off = 0, ehvar_slot = 0;
	uc_ehranges_t *ranges = &chunk->ehranges;

	try_from = chunk->count;
	ehvar_slot = compiler->locals.count;

	/* Try block ------------------------------------------------------------ */
	uc_compiler_enter_scope(compiler);

	/* while compiling the protected block, tail calls are avoided since the
	 * current frame must be kept around to catch callee exceptions */
	compiler->try_depth++;

	uc_compiler_parse_consume(compiler, TK_LBRACE);

	while (!uc_compiler_parse_check(compiler, TK_RBRACE) &&
	       !uc_compiler_parse_check(compiler, TK_EOF))
		uc_compiler_compile_declaration(compiler);

	uc_compiler_parse_consume(compiler, TK_RBRACE);

	compiler->try_depth--;

	uc_compiler_leave_scope(compiler);

	/* jump beyond catch branch */
	try_to = chunk->count;
	jmp_off = uc_compiler_emit_jmp(compiler, 0);


	/* Catch block ---------------------------------------------------------- */
	if (try_to > try_from) {
		uc_vector_push(ranges, {
			.from   = try_from,
			.to     = try_to,
			.target = chunk->count,
			.slot   = ehvar_slot
		});
	}

	uc_compiler_enter_scope(compiler);

	uc_compiler_parse_consume(compiler, TK_CATCH);

	/* have exception variable */
	if (uc_compiler_parse_match(compiler, TK_LPAREN)) {
		uc_compiler_parse_consume(compiler, TK_LABEL);

		uc_compiler_declare_local(compiler, compiler->parser->prev.uv, false);
		uc_compiler_initialize_local(compiler);

		uc_compiler_parse_consume(compiler, TK_RPAREN);
	}
	/* ... else pop exception object from stack */
	else {
		uc_compiler_emit_insn(compiler, 0, I_POP);
	}

	uc_compiler_parse_consume(compiler, TK_LBRACE);

	while (!uc_compiler_parse_check(compiler, TK_RBRACE) &&
	       !uc_compiler_parse_check(compiler, TK_EOF))
		uc_compiler_compile_declaration(compiler);

	uc_compiler_parse_consume(compiler, TK_RBRACE);

	uc_compiler_leave_scope(compiler);

	uc_compiler_set_jmpaddr(compiler, jmp_off, chunk->count);
}

static void
uc_compiler_compile_control(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	uc_tokentype_t type = compiler->parser->prev.type;
	uc_patchlist_t *p = compiler->patchlist;
	uc_locals_t *locals = &compiler->locals;
	size_t i, pos = compiler->parser->prev.pos;

	/* select applicable patchlist: for continue statements select the
	 * first non-switch scope */
	while (p) {
		if (type != TK_CONTINUE || p->token != TK_SWITCH)
			break;

		p = p->parent;
	}

	if (!p || p->token == TK_EXPORT) {
		uc_compiler_syntax_error(compiler, pos,
			(type == TK_BREAK)
				? "break must be inside loop or switch"
				: "continue must be inside loop");

		return;
	}

	/* pop locals in all scopes covered by the target patchlist */
	for (i = locals->count; i > 0 && (size_t)locals->entries[i - 1].depth > p->depth; i--)
		uc_compiler_emit_insn(compiler, 0,
			locals->entries[i - 1].captured ? I_CUPV : I_POP);

	uc_vector_push(p,
		uc_compiler_emit_jmp_dest(compiler, pos, chunk->count + type));

	uc_compiler_parse_consume(compiler, TK_SCOL);
}

static void
uc_compiler_compile_return(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);

	if (compiler->function->module) {
		uc_compiler_syntax_error(compiler, 0, "return must be inside function body");

		return;
	}

	/* if we compiled an empty expression statement (`;`), load implicit null */
	if (uc_compiler_compile_expstmt(compiler) == TK_NULL)
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_LNULL);

	/* otherwise overwrite the final I_POP instruction with I_RETURN */
	else
		uc_chunk_pop(chunk);

	/* invoke a function call in tail position to avoid growing the stack */
	bool tailcall = uc_compiler_tailcall_pending(compiler);

	uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_RETURN);

	if (tailcall)
		uc_compiler_emit_tailcall_marker(compiler);
}

static void
uc_compiler_compile_tplexp(uc_compiler_t *compiler)
{
	uc_chunk_t *chunk = uc_compiler_current_chunk(compiler);
	size_t off = chunk->count;

	uc_compiler_compile_expression(compiler, true);

	/* XXX: the lexer currently emits a superfluous trailing semicolon... */
	uc_compiler_parse_match(compiler, TK_SCOL);

	uc_compiler_parse_consume(compiler, TK_REXP);

	if (chunk->count > off)
		uc_compiler_emit_insn(compiler, 0, I_PRINT);
}

static void
uc_compiler_compile_text(uc_compiler_t *compiler)
{
	uc_compiler_emit_constant(compiler, compiler->parser->prev.pos, compiler->parser->prev.uv);
	uc_compiler_emit_insn(compiler, 0, I_PRINT);
}

static uc_tokentype_t
uc_compiler_compile_block(uc_compiler_t *compiler)
{
	uc_tokentype_t last_statement_type = TK_NULL;

	uc_compiler_enter_scope(compiler);

	while (!uc_compiler_parse_check(compiler, TK_RBRACE) &&
	       !uc_compiler_parse_check(compiler, TK_EOF))
		last_statement_type = uc_compiler_compile_declaration(compiler);

	uc_compiler_parse_consume(compiler, TK_RBRACE);

	uc_compiler_leave_scope(compiler);

	return last_statement_type;
}

static uc_tokentype_t
uc_compiler_compile_expstmt(uc_compiler_t *compiler)
{
	/* empty statement */
	if (uc_compiler_parse_match(compiler, TK_SCOL))
		return TK_NULL;

	uc_compiler_compile_expression(compiler, false);

	/* allow omitting final semicolon */
	switch (compiler->parser->curr.type) {
	case TK_RBRACE:
	case TK_ELIF:
	case TK_ENDIF:
	case TK_ENDFOR:
	case TK_ENDWHILE:
	case TK_ENDFUNC:
	case TK_EOF:
		break;

	case TK_ELSE:
		if (!uc_compiler_exprstack_is(compiler, F_ALTBLOCKMODE))
			uc_compiler_parse_consume(compiler, TK_SCOL);

		break;

	default:
		uc_compiler_parse_consume(compiler, TK_SCOL);

		break;
	}

	uc_compiler_emit_insn(compiler, 0, I_POP);

	return TK_SCOL;
}

static uc_tokentype_t
uc_compiler_compile_statement(uc_compiler_t *compiler)
{
	uc_tokentype_t last_statement_type = compiler->parser->curr.type;
	uc_exprstack_t expr = {
		.token = compiler->parser->curr.type,
		.parent = compiler->exprstack
	};

	compiler->exprstack = &expr;

	if (uc_compiler_parse_match(compiler, TK_LBRACE)) {
		last_statement_type = uc_compiler_compile_block(compiler);
	}
	else {
		uc_compiler_emit_stmt_start(compiler, &compiler->parser->curr);

		if (uc_compiler_parse_match(compiler, TK_IF))
			uc_compiler_compile_if(compiler);
		else if (uc_compiler_parse_match(compiler, TK_WHILE))
			uc_compiler_compile_while(compiler);
		else if (uc_compiler_parse_match(compiler, TK_FOR))
			uc_compiler_compile_for(compiler);
		else if (uc_compiler_parse_match(compiler, TK_SWITCH))
			uc_compiler_compile_switch(compiler);
		else if (uc_compiler_parse_match(compiler, TK_TRY))
			uc_compiler_compile_try(compiler);
		else if (uc_compiler_parse_match(compiler, TK_FUNC))
			uc_compiler_compile_funcdecl(compiler);
		else if (uc_compiler_parse_match(compiler, TK_BREAK))
			uc_compiler_compile_control(compiler);
		else if (uc_compiler_parse_match(compiler, TK_CONTINUE))
			uc_compiler_compile_control(compiler);
		else if (uc_compiler_parse_match(compiler, TK_RETURN))
			uc_compiler_compile_return(compiler);
		else if (uc_compiler_parse_match(compiler, TK_TEXT))
			uc_compiler_compile_text(compiler);
		else if (uc_compiler_parse_match(compiler, TK_LEXP))
			uc_compiler_compile_tplexp(compiler);
		else
			last_statement_type = uc_compiler_compile_expstmt(compiler);

		uc_compiler_emit_stmt_end(compiler);
	}

	compiler->exprstack = expr.parent;

	return last_statement_type;
}

static void
uc_compiler_export_add(uc_compiler_t *compiler, uc_value_t *name, ssize_t slot)
{
	uc_source_t *source = uc_compiler_current_source(compiler);

	uc_compiler_inc_exportnum(compiler);

	if (!uc_source_export_add(source, name)) {
		if (name)
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Duplicate export '%s' for module '%s'", ucv_string_get(name), source->filename);
		else
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Duplicate default export for module '%s'", source->filename);
	}
	else {
		uc_vector_push(compiler->patchlist, slot);
	}
}

static void
uc_compiler_compile_exportlist(uc_compiler_t *compiler)
{
	uc_value_t *label, *name;
	bool constant;
	ssize_t slot;

	/* parse export symbols */
	do {
		uc_compiler_parse_consume(compiler, TK_LABEL);

		label = ucv_get(compiler->parser->prev.uv);
		name = NULL;

		slot = uc_compiler_resolve_local(compiler, label, &constant);

		if (slot == -1) {
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Attempt to export undeclared or non-local variable '%s'",
				ucv_string_get(label));
		}

		if (uc_compiler_keyword_match(compiler, "as")) {
			if (uc_compiler_parse_match(compiler, TK_LABEL) || uc_compiler_parse_match(compiler, TK_STRING)) {
				name = ucv_get(compiler->parser->prev.uv);
			}
			else if (!uc_compiler_parse_match(compiler, TK_DEFAULT)) {
				uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
					"Unexpected token\nExpecting Label, String or 'default'");
			}
		}
		else {
			name = ucv_get(label);
		}

		uc_compiler_export_add(compiler, name, slot);

		ucv_put(label);
		ucv_put(name);

		if (uc_compiler_parse_match(compiler, TK_RBRACE))
			break;
	}
	while (uc_compiler_parse_match(compiler, TK_COMMA));

	uc_compiler_parse_consume(compiler, TK_SCOL);
}

static void
uc_compiler_compile_export(uc_compiler_t *compiler)
{
	uc_locals_t *locals = &compiler->locals;
	size_t off = locals->count;
	uc_value_t *name;
	ssize_t slot;

	if (!compiler->function->module || compiler->scope_depth) {
		uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
			"Exports may only appear at top level of a module");

		return;
	}

	uc_compiler_emit_stmt_start(compiler, &compiler->parser->prev);

	if (uc_compiler_parse_match(compiler, TK_LBRACE)) {
		uc_compiler_compile_exportlist(compiler);
		uc_compiler_emit_stmt_end(compiler);

		return;
	}

	if (uc_compiler_parse_match(compiler, TK_LOCAL))
		uc_compiler_compile_declexpr(compiler, false);
	else if (uc_compiler_parse_match(compiler, TK_CONST))
		uc_compiler_compile_declexpr(compiler, true);
	else if (uc_compiler_parse_match(compiler, TK_FUNC)) {
		uc_compiler_compile_funcdecl(compiler);

		for (; off < locals->count; off++)
			uc_compiler_export_add(compiler, locals->entries[off].name, off);

		return;
	}
	else if (uc_compiler_parse_match(compiler, TK_DEFAULT))
		uc_compiler_compile_expression(compiler, false);
	else
		uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
			"Unexpected token\nExpecting 'let', 'const', 'function', 'default' or '{'");

	if (off == locals->count) {
		name = ucv_string_new("(module default export)");
		slot = uc_compiler_declare_local(compiler, name, true);
		ucv_put(name);

		if (slot != -1)
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Duplicate default export statement");
		else
			uc_compiler_export_add(compiler, NULL, compiler->locals.count - 1);
	}
	else {
		for (; off < locals->count; off++)
			uc_compiler_export_add(compiler, locals->entries[off].name, off);
	}

	uc_compiler_parse_consume(compiler, TK_SCOL);

	uc_compiler_emit_stmt_end(compiler);
}

static uc_program_t *
uc_compile_from_source(uc_parse_config_t *config, uc_source_t *source, uc_program_t *prog, char **errp);

static bool
uc_compiler_compile_dynload(uc_compiler_t *compiler, const char *name, uc_value_t *imports)
{
	uc_value_t *modname = ucv_string_new(name);
	size_t i, n_imports;
	uc_value_t *import;

	for (i = 0, n_imports = 0; i < ucv_array_length(imports); i++) {
		import = ucv_array_get(imports, i);

		if (ucv_boolean_get(import)) {
			uc_compiler_emit_constant(compiler, 0, modname);
			uc_compiler_emit_insn(compiler, 0, I_DYNLOAD);
			uc_compiler_emit_u32(compiler, 0, 0);
		}
		else {
			n_imports++;
		}
	}

	if (n_imports > 0) {
		uc_compiler_emit_constant(compiler, 0, modname);
		uc_compiler_emit_insn(compiler, 0, I_DYNLOAD);
		uc_compiler_emit_u32(compiler, 0, n_imports | ((compiler->upvals.count - n_imports) << 16));

		for (i = 0; i < ucv_array_length(imports); i++) {
			import = ucv_get(ucv_array_get(imports, i));

			if (!import)
				import = ucv_string_new("default");

			if (!ucv_boolean_get(import))
				uc_compiler_emit_constant_index(compiler, 0, import);

			ucv_put(import);
		}
	}

	ucv_put(modname);

	return true;
}

static bool
uc_compiler_compile_module_source(uc_compiler_t *compiler, const char *modname, uc_source_t *source, uc_value_t *imports, char **errp)
{
	uc_parse_config_t config = {
		.raw_mode = true,
		.strict_declarations = true,
		.module_search_path = compiler->parser->lex.config->module_search_path
	};

	size_t i, load_idx = 0, n_imports = 0;
	bool loaded = false;
	uc_value_t *import;
	ssize_t slot;

	uc_program_function_foreach(compiler->program, fn) {
		if (uc_program_function_source(fn) == source) {
			loaded = true;
			break;
		}
	}

	/* An offset of -1 marks a source whose compilation was entered but not yet
	 * completed. Re-entering it is either a circular dependency (its function is
	 * still present) or a module whose earlier compilation failed and freed its
	 * function. Recompiling in either case recurses without bound across a large
	 * import graph, so report the cycle and propagate the failure instead. */
	if (source->exports.offset == (size_t)-1) {
		if (loaded)
			uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
				"Circular dependency");
		else if (errp)
			xasprintf(errp, "Module previously failed to compile\n");

		return false;
	}

	if (!loaded) {
		/* We do not yet support linking precompiled modules at compile time,
		   turn static import operation into dynamic load one. */
		if (uc_source_type_test(source) == UC_SOURCE_TYPE_PRECOMPILED)
			return uc_compiler_compile_dynload(compiler, modname, imports);

		load_idx = uc_program_function_id(compiler->program,
			uc_program_function_last(compiler->program)) + 1;

		source->exports.offset = (size_t)-1;

		if (!uc_compile_from_source(&config, source, compiler->program, errp))
			return false;

		source->exports.offset = uc_compiler_get_exportnum(compiler) - source->exports.count;
		uc_compiler_current_source(compiler)->exports.offset += source->exports.count;

		/* emit load, call & pop instructions */
		uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_CLFN);
		uc_compiler_emit_u32(compiler, 0, load_idx);

		uc_compiler_emit_insn(compiler, 0, I_CALL);
		uc_compiler_emit_u32(compiler, 0, 0);

		uc_compiler_emit_insn(compiler, 0, I_POP);
	}

	/* count imports, handle wildcard imports */
	for (i = 0; i < ucv_array_length(imports); i++) {
		if (ucv_boolean_get(ucv_array_get(imports, i))) {
			if (source->exports.offset > 0xffff || source->exports.count > 0xffff) {
				uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
					"Too many module exports");
			}

			/* emit import instruction... */
			uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_IMPORT);
			uc_compiler_emit_u32(compiler, 0, source->exports.count | (0xffffu << 16));

			/* ... followed by first module export offset ... */
			uc_compiler_emit_u16(compiler, 0, source->exports.offset);

			/* ... and constant indexes for all exported names */
			for (load_idx = 0; load_idx < source->exports.count; load_idx++) {
				if (source->exports.entries[load_idx])
					import = ucv_get(source->exports.entries[load_idx]);
				else
					import = ucv_string_new("default");

				uc_compiler_emit_constant_index(compiler, 0, import);
				ucv_put(import);
			}

		}
		else {
			n_imports++;
		}
	}

	/* 0xffff is reserved for wildcard import */
	if (n_imports > 0xfffe)
		uc_compiler_syntax_error(compiler, 0, "Too many imports");

	/* emit non-wildcard import instructions */
	for (i = 0; i < ucv_array_length(imports); i++) {
		import = ucv_array_get(imports, i);

		if (!ucv_boolean_get(import)) {
			slot = uc_source_export_lookup(source, import);

			if (slot == -1) {
				if (import)
					uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
						"Module %s does not export '%s'", source->filename, ucv_string_get(import));
				else
					uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
						"Module %s has no default export", source->filename);
			}
			else if (slot > 0xffff) {
				uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
					"Too many module exports");
			}
			else {
				uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_IMPORT);
				uc_compiler_emit_u32(compiler, 0,
					(source->exports.offset + slot) | ((compiler->upvals.count - n_imports + i) << 16));
			}
		}
	}

	return true;
}

static char *
uc_compiler_canonicalize_path(const char *path, const char *runpath)
{
	char *p, *resolved;
	const char *slash;

	if (*path == '/')
		xasprintf(&p, "%s", path);
	else if (runpath && (slash = strrchr(runpath, '/')) != NULL)
		xasprintf(&p, "%.*s/%s", (int)(slash - runpath), runpath, path);
	else
		xasprintf(&p, "./%s", path);

	resolved = realpath(p, NULL);

	free(p);

	return resolved;
}

static char *
uc_compiler_expand_module_path(const char *name, const char *runpath, const char *template)
{
	int namelen, prefixlen;
	const char *asterix;
	char *path, *p;

	asterix = strchr(template, '*');

	if (!asterix)
		return NULL;

	prefixlen = asterix - template;
	namelen = strlen(name);

	xasprintf(&path, "%.*s%.*s%s", prefixlen, template, namelen, name, asterix + 1);

	for (p = path + prefixlen; namelen > 0; namelen--, p++)
		if (*p == '.')
			*p = '/';

	p = uc_compiler_canonicalize_path(path, runpath);

	free(path);

	return p;
}

static char *
uc_compiler_resolve_module_path(uc_compiler_t *compiler, const char *name)
{
	uc_search_path_t *search = &compiler->parser->lex.config->module_search_path;
	uc_source_t *source = uc_compiler_current_source(compiler);
	char *path = NULL;
	size_t i;

	if (strchr(name, '/'))
		return uc_compiler_canonicalize_path(name, source->runpath);

	for (i = 0; i < search->count && !path; i++)
		path = uc_compiler_expand_module_path(name, source->runpath, search->entries[i]);

	return path;
}

static uc_source_t *
uc_compiler_acquire_source(uc_compiler_t *compiler, const char *path)
{
	size_t i;

	for (i = 0; i < compiler->program->sources.count; i++)
		if (!strcmp(compiler->program->sources.entries[i]->filename, path))
			return uc_source_get(compiler->program->sources.entries[i]);

	return uc_source_new_file(path);
}

static bool
uc_compiler_is_dynlink_module(uc_compiler_t *compiler, const char *name, const char *path)
{
	uc_search_path_t *dynlink_list = &compiler->parser->config->force_dynlink_list;
	const char *dot;
	size_t i;

	for (i = 0; i < dynlink_list->count; i++)
		if (!strcmp(dynlink_list->entries[i], name))
			return true;

	if (!path)
		return false;

	dot = strrchr(path, '.');

	return (dot && !strcmp(dot, ".so"));
}

static bool
uc_compiler_compile_module(uc_compiler_t *compiler, const char *name, uc_value_t *imports)
{
	uc_source_t *source;
	char *path, *err;
	bool res;

	if (!name)
		return false;

	path = uc_compiler_resolve_module_path(compiler, name);

	if (uc_compiler_is_dynlink_module(compiler, name, path)) {
		res = uc_compiler_compile_dynload(compiler, name, imports);
	}
	else if (path) {
		source = uc_compiler_acquire_source(compiler, path);

		if (source) {
			err = NULL;
			res = uc_compiler_compile_module_source(compiler, name, source, imports, &err);

			if (!res) {
				uc_error_message_indent(&err);
				uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
					"Unable to compile module '%s':\n\n%s", source->filename, err);
			}

			free(err);
		}
		else {
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
				"Unable to open module '%s': %s",
				path, strerror(errno));

			res = false;
		}

		uc_source_put(source);
	}
	else {
		uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
			"Unable to resolve path for module '%s'", name);

		return false;
	}

	free(path);

	return res;
}

static void
uc_compiler_import_add(uc_compiler_t *compiler, uc_value_t *name)
{
	bool constant;
	ssize_t slot;

	slot = uc_compiler_resolve_local(compiler, name, &constant);

	if (slot != -1) {
		uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
			"Import name '%s' is already declared as local variable",
			ucv_string_get(name));

		return;
	}

	slot = uc_compiler_resolve_upval(compiler, name, &constant);

	if (slot != -1) {
		uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
			"Import name '%s' is already used",
			ucv_string_get(name));

		return;
	}

	uc_compiler_add_upval(compiler, (2 << 14) + compiler->upvals.count, false, name, true);
}

static void
uc_compiler_compile_importlist(uc_compiler_t *compiler, uc_value_t *namelist)
{
	uc_value_t *label, *name;

	/* parse export symbols */
	do {
		name = NULL;
		label = NULL;

		if (uc_compiler_parse_match(compiler, TK_DEFAULT)) {
			uc_compiler_keyword_consume(compiler, "as");
			uc_compiler_parse_consume(compiler, TK_LABEL);

			label = ucv_get(compiler->parser->prev.uv);
		}
		else if (uc_compiler_parse_match(compiler, TK_STRING)) {
			name = ucv_get(compiler->parser->prev.uv);

			uc_compiler_keyword_consume(compiler, "as");
			uc_compiler_parse_consume(compiler, TK_LABEL);

			label = ucv_get(compiler->parser->prev.uv);
		}
		else if (uc_compiler_parse_match(compiler, TK_LABEL)) {
			name = ucv_get(compiler->parser->prev.uv);

			if (uc_compiler_keyword_match(compiler, "as")) {
				uc_compiler_parse_consume(compiler, TK_LABEL);

				label = ucv_get(compiler->parser->prev.uv);
			}
			else {
				label = ucv_get(name);
			}
		}
		else {
			uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
				"Unexpected token\nExpecting Label, String or 'default'");
		}

		uc_compiler_import_add(compiler, label);
		ucv_array_push(namelist, name);
		ucv_put(label);

		if (uc_compiler_parse_match(compiler, TK_RBRACE))
			return;
	}
	while (uc_compiler_parse_match(compiler, TK_COMMA));

	uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
		"Unexpected token\nExpecting 'as', ',' or '}'");
}

static void
uc_compiler_compile_importcall(uc_compiler_t *compiler)
{
	uc_compiler_parse_consume(compiler, TK_LPAREN);
	uc_compiler_parse_precedence(compiler, P_ASSIGN);
	uc_compiler_parse_consume(compiler, TK_RPAREN);

	uc_compiler_emit_insn(compiler, compiler->parser->prev.pos, I_DYNLOAD);
	uc_compiler_emit_u32(compiler, 0, 0);
}

static uc_tokentype_t
uc_compiler_compile_import(uc_compiler_t *compiler)
{
	uc_value_t *namelist = ucv_array_new(NULL);

	/* import(...) */
	if (uc_compiler_parse_check(compiler, TK_LPAREN)) {
		uc_compiler_parse_precedence_for_token(compiler, P_UNARY,
		                                       &compiler->parser->prev);

		uc_compiler_emit_insn(compiler, 0, I_POP);

		return TK_SCOL;
	}

	if (compiler->scope_depth) {
		uc_compiler_syntax_error(compiler, compiler->parser->prev.pos,
			"Imports may only appear at top level");

		ucv_put(namelist);

		return TK_IMPORT;
	}

	uc_compiler_emit_stmt_start(compiler, &compiler->parser->prev);

	/* import { ... } from */
	if (uc_compiler_parse_match(compiler, TK_LBRACE)) {
		uc_compiler_compile_importlist(compiler, namelist);
		uc_compiler_keyword_consume(compiler, "from");
	}

	/* import * as name from */
	else if (uc_compiler_parse_match(compiler, TK_MUL)) {
		uc_compiler_keyword_consume(compiler, "as");
		uc_compiler_parse_consume(compiler, TK_LABEL);

		uc_compiler_declare_local(compiler, compiler->parser->prev.uv, true);
		uc_compiler_initialize_local(compiler);
		ucv_array_push(namelist, ucv_boolean_new(true));

		uc_compiler_keyword_consume(compiler, "from");
	}

	/* import defaultExport [, ... ] from */
	else if (uc_compiler_parse_match(compiler, TK_LABEL)) {
		uc_compiler_import_add(compiler, compiler->parser->prev.uv);
		ucv_array_push(namelist, NULL);

		/* import defaultExport, ... from */
		if (uc_compiler_parse_match(compiler, TK_COMMA)) {
			/* import defaultExport, { ... } from */
			if (uc_compiler_parse_match(compiler, TK_LBRACE)) {
				uc_compiler_compile_importlist(compiler, namelist);
			}

			/* import defaultExport, * as name from */
			else if (uc_compiler_parse_match(compiler, TK_MUL)) {
				uc_compiler_keyword_consume(compiler, "as");
				uc_compiler_parse_consume(compiler, TK_LABEL);

				uc_compiler_declare_local(compiler, compiler->parser->prev.uv, true);
				uc_compiler_initialize_local(compiler);
				ucv_array_push(namelist, ucv_boolean_new(true));
			}

			/* error */
			else {
				uc_compiler_syntax_error(compiler, compiler->parser->curr.pos,
					"Unexpected token\nExpecting '{' or '*'");
			}
		}

		uc_compiler_keyword_consume(compiler, "from");
	}

	uc_compiler_parse_consume(compiler, TK_STRING);

	uc_compiler_compile_module(compiler, ucv_string_get(compiler->parser->prev.uv), namelist);

	uc_compiler_parse_consume(compiler, TK_SCOL);

	uc_compiler_emit_stmt_end(compiler);

	ucv_put(namelist);

	return TK_IMPORT;
}

static uc_tokentype_t
uc_compiler_compile_declaration(uc_compiler_t *compiler)
{
	uc_tokentype_t last_statement_type = compiler->parser->curr.type;

	if (uc_compiler_parse_match(compiler, TK_LOCAL))
		uc_compiler_compile_local(compiler);
	else if (uc_compiler_parse_match(compiler, TK_CONST))
		uc_compiler_compile_const(compiler);
	else if (uc_compiler_parse_match(compiler, TK_EXPORT))
		uc_compiler_compile_export(compiler);
	else if (uc_compiler_parse_match(compiler, TK_IMPORT))
		last_statement_type = uc_compiler_compile_import(compiler);
	else
		last_statement_type = uc_compiler_compile_statement(compiler);

	if (compiler->parser->synchronizing)
		uc_compiler_parse_synchronize(compiler);

	return last_statement_type;
}

#endif /* NO_COMPILE */


static uc_program_t *
uc_compile_from_source(uc_parse_config_t *config, uc_source_t *source, uc_program_t *prog, char **errp)
{
#ifdef NO_COMPILE
	if (errp)
		xasprintf(errp, "Source code compilation not supported\n");

	return NULL;
#else
	bool is_module = (prog != NULL) || (config && config->compile_module);
	uc_patchlist_t exports = { .token = TK_EXPORT };
	uc_exprstack_t expr = { .token = TK_EOF };
	uc_parser_t parser = { .config = config };
	uc_compiler_t compiler = { .parser = &parser, .exprstack = &expr };
	uc_tokentype_t last_statement_type = TK_NULL;
	uc_program_t *progptr;
	uc_function_t *fn;
	const char *name;

	if (!prog) {
		progptr = uc_program_new();
		name = is_module ? "module" : "main";
	}
	else {
		progptr = prog;
		name = "module";
	}

	uc_lexer_init(&parser.lex, config, source);
	uc_compiler_init(&compiler, name, source, 0, progptr,
		config && config->strict_declarations);

	if (is_module) {
		compiler.patchlist = &exports;
		compiler.function->module = true;
	}

	uc_compiler_parse_advance(&compiler);

	while (!uc_compiler_parse_match(&compiler, TK_EOF))
		last_statement_type = uc_compiler_compile_declaration(&compiler);

	if (!compiler.function->module && last_statement_type == TK_SCOL) {
		uc_chunk_pop(uc_compiler_current_chunk(&compiler));

		/* invoke a trailing call statement in tail position */
		bool tailcall = uc_compiler_tailcall_pending(&compiler);

		uc_compiler_emit_insn(&compiler, 0, I_RETURN);

		if (tailcall)
			uc_compiler_emit_tailcall_marker(&compiler);

		last_statement_type = TK_RETURN;
	}

	fn = uc_compiler_finish(&compiler, last_statement_type);

	if (errp) {
		*errp = parser.error ? parser.error->buf : NULL;
		free(parser.error);
	}
	else {
		printbuf_free(parser.error);
	}

	uc_lexer_free(&parser.lex);
	uc_vector_clear(&exports);

	if (!fn) {
		if (progptr != prog)
			ucv_put(&progptr->header);

		return NULL;
	}

	return progptr;
#endif
}

static uc_program_t *
uc_compile_from_bytecode(uc_parse_config_t *config, uc_source_t *source, char **errp)
{
	uc_program_t *prog;

	prog = uc_program_load(source, errp);

	if (prog && !uc_program_entry(prog)) {
		if (errp)
			xasprintf(errp, "Program file contains no entry function\n");

		ucv_put(&prog->header);
		prog = NULL;
	}

	return prog;
}

uc_program_t *
uc_compile(uc_parse_config_t *config, uc_source_t *source, char **errp)
{
	uc_program_t *prog = NULL;

	if (!config)
		config = &uc_default_parse_config;

	switch (uc_source_type_test(source)) {
	case UC_SOURCE_TYPE_PLAIN:
		prog = uc_compile_from_source(config, source, NULL, errp);
		break;

	case UC_SOURCE_TYPE_PRECOMPILED:
		prog = uc_compile_from_bytecode(config, source, errp);
		break;

	default:
		if (errp)
			xasprintf(errp, "Unrecognized source type\n");

		break;
	}

	return prog;
}

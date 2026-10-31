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

/* Internal compiler state. */

#ifndef UCODE_INTERNAL_COMPILER_H
#define UCODE_INTERNAL_COMPILER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "ucode/internal/source.h"
#include "ucode/internal/lexer.h"
#include "ucode/internal/types.h"
#include "ucode/compiler.h"

typedef enum {
	P_NONE,

	P_COMMA,	/* , */

	P_ASSIGN,	/* = += -= *= /= %= <<= >>= &= ^= |= ||= &&= **= ??= */

	P_TERNARY,	/* ?: */

	P_OR,		/* || ?? */
	P_AND,		/* && */
	P_BOR,		/* | */
	P_BXOR,		/* ^ */
	P_BAND,		/* & */

	P_EQUAL,	/* === !== == != */
	P_COMPARE,	/* < <= > >= in */

	P_SHIFT,	/* << >> */

	P_ADD,		/* + - */
	P_MUL,		/* * / % */

	P_EXP,		/* ** */

	P_UNARY,	/* ! ~ +… -… ++… --… */

	P_INC,		/* …++ …-- */

	P_CALL,		/* ….…, …[…], …(…) */

	P_PRIMARY	/* (…) */
} uc_precedence_t;

typedef enum {
	F_ASSIGNABLE = (1 << 0),
	F_ALTBLOCKMODE = (1 << 1),
} uc_exprflag_t;

typedef struct uc_patchlist {
	struct uc_patchlist *parent;
	size_t depth, count, *entries, capacity;
	uc_tokentype_t token;
} uc_patchlist_t;

typedef struct uc_exprstack {
	struct uc_exprstack *parent;
	uint32_t flags;
	uc_tokentype_t token;
} uc_exprstack_t;

typedef struct {
	uc_value_t *name;
	ssize_t depth;
	size_t from;
	bool captured;
	bool constant;
	bool funcstub;
} uc_local_t;

typedef struct {
	uc_value_t *name;
	size_t index;
	bool local;
	bool constant;
} uc_upval_t;

uc_declare_vector(uc_locals_t, uc_local_t);
uc_declare_vector(uc_upvals_t, uc_upval_t);
uc_declare_vector(uc_jmplist_t, size_t);

typedef struct {
	uc_parse_config_t *config;
	uc_lexer_t lex;
	uc_token_t prev, curr;
	uc_stringbuf_t *error;
	bool synchronizing;
	struct {
		size_t from, to, count;
		uc_bracket_t *entries;
	} scanned;
} uc_parser_t;

typedef struct uc_compiler {
	struct uc_compiler *parent;
	uc_locals_t locals;
	uc_upvals_t upvals;
	uc_patchlist_t *patchlist;
	uc_exprstack_t *exprstack;
	uc_function_t *function;
	uc_parser_t *parser;
	uc_program_t *program;
	size_t scope_depth, current_srcpos, last_insn, try_depth, tailcall_off;
} uc_compiler_t;

typedef struct {
	void (*prefix)(uc_compiler_t *);
	void (*infix)(uc_compiler_t *);
	uc_precedence_t precedence;
} uc_parse_rule_t;

#define uc_compiler_exprstack_push(compiler, token, flags) \
	uc_exprstack_t expr = { compiler->exprstack, flags, token }; \
	compiler->exprstack = &expr

#define uc_compiler_exprstack_pop(compiler) \
	if (compiler->exprstack) \
		compiler->exprstack = compiler->exprstack->parent

#endif /* UCODE_INTERNAL_COMPILER_H */

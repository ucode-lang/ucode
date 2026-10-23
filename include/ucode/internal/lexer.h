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

#ifndef UCODE_INTERNAL_LEXER_H
#define UCODE_INTERNAL_LEXER_H

#include "ucode/internal/source.h"
#include "ucode/internal/types.h"
#include "ucode/internal/util.h"
#include "ucode/compiler.h"


typedef enum {
	TK_LEXP = 1,
	TK_REXP,
	TK_LSTM,
	TK_RSTM,
	TK_IF,
	TK_ELSE,
	TK_COMMA,
	TK_ASSIGN,
	TK_ASADD,
	TK_ASSUB,
	TK_ASMUL,
	TK_ASDIV,
	TK_ASMOD,
	TK_ASLEFT,
	TK_ASRIGHT,
	TK_ASBAND,
	TK_ASBXOR,
	TK_ASBOR,
	TK_QMARK,
	TK_COLON,
	TK_OR,
	TK_AND,
	TK_BOR,
	TK_BXOR,
	TK_BAND,
	TK_EQS,
	TK_NES,
	TK_EQ,
	TK_NE,
	TK_LT,
	TK_LE,
	TK_GT,
	TK_GE,
	TK_IN,
	TK_LSHIFT,
	TK_RSHIFT,
	TK_ADD,
	TK_SUB,
	TK_MUL,
	TK_DIV,
	TK_MOD,
	TK_EXP,
	TK_NOT,
	TK_COMPL,
	TK_INC,
	TK_DEC,
	TK_DOT,
	TK_LBRACK,
	TK_RBRACK,
	TK_LPAREN,
	TK_RPAREN,
	TK_TEXT,
	TK_LBRACE,
	TK_RBRACE,
	TK_SCOL,
	TK_ENDIF,
	TK_ELIF,
	TK_WHILE,
	TK_ENDWHILE,
	TK_FOR,
	TK_ENDFOR,
	TK_FUNC,
	TK_LABEL,
	TK_ENDFUNC,
	TK_TRY,
	TK_CATCH,
	TK_SWITCH,
	TK_CASE,
	TK_DEFAULT,
	TK_ELLIP,
	TK_RETURN,
	TK_BREAK,
	TK_CONTINUE,
	TK_LOCAL,
	TK_ARROW,
	TK_TRUE,
	TK_FALSE,
	TK_NUMBER,
	TK_DOUBLE,
	TK_STRING,
	TK_REGEXP,
	TK_NULL,
	TK_THIS,
	TK_DELETE,
	TK_CONST,
	TK_QLBRACK,
	TK_QLPAREN,
	TK_QDOT,
	TK_ASEXP,
	TK_ASAND,
	TK_ASOR,
	TK_ASNULLISH,
	TK_NULLISH,
	TK_PLACEH,
	TK_TEMPLATE,
	TK_IMPORT,
	TK_EXPORT,
	TK_TYPEOF,
	TK_THROW,

	TK_EOF,
	TK_COMMENT,
	TK_ERROR
} uc_tokentype_t;

typedef enum {
	UC_LEX_IDENTIFY_BLOCK,
	UC_LEX_BLOCK_EXPRESSION_EMIT_TAG,
	UC_LEX_BLOCK_STATEMENT_EMIT_TAG,
	UC_LEX_BLOCK_COMMENT,
	UC_LEX_IDENTIFY_TOKEN,
	UC_LEX_PLACEHOLDER_START,
	UC_LEX_PLACEHOLDER_END,
	UC_LEX_EOF
} uc_lex_state_t;

typedef struct {
	uc_tokentype_t type;
	uc_value_t *uv;
	size_t pos;
	size_t end;
} uc_token_t;

typedef struct {
	uc_lex_state_t state;
	uc_parse_config_t *config;
	uc_source_t *source;
	uint8_t no_regexp:1;
	uint8_t no_keyword:1;
	uc_token_t curr;
	int lead_surrogate;
	size_t lastoff;
	enum {
		UNSPEC,
		PLUS,
		MINUS,
		NEWLINE
	} modifier;
	enum {
		NONE,
		EXPRESSION = '{',
		STATEMENTS = '%',
		COMMENT = '#'
	} block;
	struct {
		size_t count;
		size_t *entries;
	} templates;
	struct {
		size_t count;
		char *entries;
	} buffer;
	const uint8_t *rbuf;
	size_t rlen, rpos, off;
} uc_lexer_t;


void uc_lexer_init(uc_lexer_t *lex, uc_parse_config_t *config, uc_source_t *source);
void uc_lexer_free(uc_lexer_t *lex);

uc_token_t *uc_lexer_next_token(uc_lexer_t *lex);

__hidden bool uc_lexer_is_keyword(uc_value_t *label);

/* What follows a closing bracket. */
typedef enum {
	UC_FOLLOW_OTHER,
	UC_FOLLOW_ASSIGN,   /* = */
	UC_FOLLOW_ARROW     /* => */
} uc_follow_t;

typedef struct {
	size_t pos;
	uc_follow_t follow;
} uc_bracket_t;

uc_declare_vector(uc_bracketlist_t, uc_bracket_t);
uc_declare_vector(uc_offsetstack_t, size_t);

__hidden bool uc_lexer_skip_balanced(uc_lexer_t *lex, size_t off, size_t *end,
                                     uc_follow_t *follow, uc_bracketlist_t *enclosed);

__hidden bool utf8enc(char **out, int *rem, int code);

__hidden const char *uc_tokenname(unsigned type);

#endif /* UCODE_INTERNAL_LEXER_H */

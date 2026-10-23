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

#include <ctype.h>

#include "ucode/internal/lexer.h"
#include "ucode/internal/vallist.h"

struct keyword {
	unsigned type;
	const char *pat;
	unsigned plen;
};

#define dec(o) \
	((o) - '0')

#define hex(x) \
	(((x) >= 'a') ? (10 + (x) - 'a') : \
		(((x) >= 'A') ? (10 + (x) - 'A') : dec(x)))

#ifndef NO_COMPILE

static const struct keyword reserved_words[] = {
	{ TK_ENDFUNC,	"endfunction", 11 },
	{ TK_CONTINUE,	"continue", 8 },
	{ TK_ENDWHILE,	"endwhile", 8 },
	{ TK_FUNC,		"function", 8 },
	{ TK_DEFAULT,	"default", 7 },
	{ TK_DELETE,	"delete", 6 },
	{ TK_RETURN,	"return", 6 },
	{ TK_ENDFOR,	"endfor", 6 },
	{ TK_SWITCH,	"switch", 6 },
	{ TK_IMPORT,	"import", 6 },
	{ TK_EXPORT,	"export", 6 },
	{ TK_TYPEOF,	"typeof", 6 },
	{ TK_ENDIF,		"endif", 5 },
	{ TK_WHILE,		"while", 5 },
	{ TK_BREAK,		"break", 5 },
	{ TK_CATCH,		"catch", 5 },
	{ TK_THROW,		"throw", 5 },
	{ TK_CONST,		"const", 5 },
	{ TK_FALSE,		"false", 5 },
	{ TK_TRUE,		"true",  4 },
	{ TK_ELIF,		"elif",  4 },
	{ TK_ELSE,		"else",  4 },
	{ TK_THIS,		"this",  4 },
	{ TK_NULL,		"null",  4 },
	{ TK_CASE,		"case",  4 },
	{ TK_TRY,		"try",   3 },
	{ TK_FOR,		"for",   3 },
	{ TK_LOCAL,		"let",   3 },
	{ TK_IF,		"if",    2 },
	{ TK_IN,		"in",    2 },
};


static int
fill_buf(uc_lexer_t *lex) {
	size_t off;

	lex->rbuf = uc_source_read_block(lex->source, &lex->rlen, &off);
	lex->rpos = 0;

	if (!lex->rbuf)
		return EOF;

	/* everything in front of the block has been consumed, so this merely
	 * establishes the offset of the first byte of code */
	lex->off = off;

	lex->rpos++;

	return (int)lex->rbuf[0];
}

static int
update_line(uc_lexer_t *lex, int ch) {
	if (ch == '\n')
		uc_source_line_next(lex->source);
	else if (ch != EOF)
		uc_source_line_update(lex->source, 1);

	lex->off++;

	return ch;
}

static int
lookahead_char(uc_lexer_t *lex) {
	int c;

	if (lex->rpos < lex->rlen)
		return (int)lex->rbuf[lex->rpos];

	c = fill_buf(lex);
	lex->rpos = 0;

	return c;
}

static bool
check_char(uc_lexer_t *lex, int ch) {
	if (lookahead_char(lex) != ch)
		return false;

	lex->rpos++;

	update_line(lex, ch);

	return true;
}

static int
next_char(uc_lexer_t *lex) {
	int ch = (lex->rpos < lex->rlen) ? (int)lex->rbuf[lex->rpos++] : fill_buf(lex);

	return update_line(lex, ch);
}

static uc_token_t *
emit_op(uc_lexer_t *lex, ssize_t pos, int type, uc_value_t *uv)
{
	lex->curr.type = type;
	lex->curr.uv = uv;

	if (pos < 0)
		lex->curr.pos = lex->off + pos;
	else
		lex->curr.pos = (size_t)pos;

	lex->curr.end = lex->off;

	return &lex->curr;
}

static uc_token_t *
emit_buffer(uc_lexer_t *lex, ssize_t pos, int type, const char *strip_trailing_chars) {
	uc_token_t *rv = NULL;
	char *p;

	if (lex->buffer.count) {
		if (strip_trailing_chars)
			for (p = uc_vector_last(&lex->buffer);
			     p && strchr(strip_trailing_chars, *p);
			     lex->buffer.count--, p = uc_vector_last(&lex->buffer));

		rv = emit_op(lex, pos, type, ucv_string_new_length(uc_vector_first(&lex->buffer), lex->buffer.count));

		uc_vector_clear(&lex->buffer);
	}
	else if (type != TK_TEXT) {
		rv = emit_op(lex, pos, type, ucv_string_new_length("", 0));
	}

	return rv;
}


static uc_token_t *
parse_comment(uc_lexer_t *lex, int kind)
{
	size_t off = lex->off - 1;
	int ch;

	uc_vector_push(&lex->buffer, '/');

	/* consume the second opening character so that the opening star of a block
	 * comment cannot double as the closing star of an empty comment */
	uc_vector_push(&lex->buffer, next_char(lex));

	while (true) {
		ch = next_char(lex);

		uc_vector_push(&lex->buffer, ch);

		if (kind == '/' && (ch == '\n' || ch == EOF))
			break;

		if (kind == '*' && ch == '*' && check_char(lex, '/')) {
			uc_vector_push(&lex->buffer, '/');
			break;
		}

		if (ch == EOF) {
			lex->state = UC_LEX_EOF;

			return emit_op(lex, lex->lastoff, TK_ERROR, ucv_string_new("Unterminated comment"));
		}
	}

	return emit_buffer(lex, off, TK_COMMENT, NULL);
}

static void
append_utf8(uc_lexer_t *lex, int code) {
	char ustr[8], *up;
	int rem;

	up = ustr;
	rem = sizeof(ustr);

	if (utf8enc(&up, &rem, code))
		for (up = ustr; rem < (int)sizeof(ustr); rem++)
			uc_vector_push(&lex->buffer, *up++);
}

static uc_token_t *
parse_escape(uc_lexer_t *lex, const char *regex_macros)
{
	int code, ch, i;
	const char *p;

	/* unicode escape sequence */
	if (check_char(lex, 'u')) {
		for (i = 0, code = 0; i < 4; i++) {
			ch = next_char(lex);

			if (!isxdigit(ch))
				return emit_op(lex, -1, TK_ERROR, ucv_string_new("Invalid escape sequence"));

			code = code * 16 + hex(ch);
		}

		/* is a leading surrogate value */
		if ((code & 0xFC00) == 0xD800) {
			/* found a subsequent leading surrogate, ignore and emit replacement char for previous one */
			if (lex->lead_surrogate)
				append_utf8(lex, 0xFFFD);

			/* store surrogate value and advance to next escape sequence */
			lex->lead_surrogate = code;
		}

		/* is a trailing surrogate value */
		else if ((code & 0xFC00) == 0xDC00) {
			/* found a trailing surrogate following a leading one, combine and encode */
			if (lex->lead_surrogate) {
				code = 0x10000 + ((lex->lead_surrogate & 0x3FF) << 10) + (code & 0x3FF);
				lex->lead_surrogate = 0;
			}

			/* trailing surrogate not following a leading one, ignore and use replacement char */
			else {
				code = 0xFFFD;
			}

			append_utf8(lex, code);
		}

		/* is a normal codepoint */
		else {
			append_utf8(lex, code);
		}
	}

	/* hex escape sequence */
	else if (check_char(lex, 'x')) {
		for (i = 0, code = 0; i < 2; i++) {
			ch = next_char(lex);

			if (!isxdigit(ch))
				return emit_op(lex, -1, TK_ERROR, ucv_string_new("Invalid escape sequence"));

			code = code * 16 + hex(ch);
		}

		uc_vector_push(&lex->buffer, code);
	}

	/* octal or letter */
	else {
		/* try to parse octal sequence... */
		for (i = 0, code = 0, ch = lookahead_char(lex);
		     i < 3 && ch >= '0' && ch <= '7';
		     i++, next_char(lex), ch = lookahead_char(lex)) {
			code = code * 8 + dec(ch);
		}

		if (i) {
			if (code > 255)
				return emit_op(lex, -3, TK_ERROR, ucv_string_new("Invalid escape sequence"));

			uc_vector_push(&lex->buffer, code);
		}

		/* ... no octal sequence, handle potential regex macros */
		else if (strchr(regex_macros, ch)) {
			ch = next_char(lex);

			switch (ch) {
			case 'd': p = "[[:digit:]]";   break;
			case 'D': p = "[^[:digit:]]";  break;
			case 'w': p = "[[:alnum:]_]";  break;
			case 'W': p = "[^[:alnum:]_]"; break;
			case 's': p = "[[:space:]]";   break;
			case 'S': p = "[^[:space:]]";  break;
			default:  p = NULL;
			}

			if (p) {
				while (*p)
					uc_vector_push(&lex->buffer, *p++);
			}
			else {
				uc_vector_push(&lex->buffer, '\\');
				uc_vector_push(&lex->buffer, ch);
			}
		}

		/* ... handle other escape */
		else {
			ch = next_char(lex);

			switch (ch) {
			case 'a': uc_vector_push(&lex->buffer, '\a'); break;
			case 'b': uc_vector_push(&lex->buffer, '\b'); break;
			case 'e': uc_vector_push(&lex->buffer, '\033'); break;
			case 'f': uc_vector_push(&lex->buffer, '\f'); break;
			case 'n': uc_vector_push(&lex->buffer, '\n'); break;
			case 'r': uc_vector_push(&lex->buffer, '\r'); break;
			case 't': uc_vector_push(&lex->buffer, '\t'); break;
			case 'v': uc_vector_push(&lex->buffer, '\v'); break;

			case EOF:
				return emit_op(lex, -2, TK_ERROR, ucv_string_new("Unterminated string"));

			default:
				uc_vector_push(&lex->buffer, ch);
			}
		}
	}

	return NULL;
}

static uc_token_t *
parse_string(uc_lexer_t *lex, int kind)
{
	uc_token_t *err, *tok;
	unsigned type;
	int code, ch;
	size_t off;

	if (kind == '`')
		type = TK_TEMPLATE;
	else if (kind == '/')
		type = TK_REGEXP;
	else
		type = TK_STRING;

	off = lex->off - 1;

	for (ch = next_char(lex); ch != EOF; ch = next_char(lex)) {
		switch (ch) {
		/* placeholder */
		case '$':
			if (type == TK_TEMPLATE && check_char(lex, '{')) {
				lex->state = UC_LEX_PLACEHOLDER_START;

				tok = emit_buffer(lex, off, type, NULL);
				tok->end -= 2;

				return tok;
			}

			uc_vector_push(&lex->buffer, '$');
			break;

		/* regexp bracket expression */
		case '[':
			uc_vector_push(&lex->buffer, '[');

			if (type == TK_REGEXP) {
				/* skip leading negation (^) */
				if (check_char(lex, '^'))
					uc_vector_push(&lex->buffer, '^');

				/* skip leading `]` - it is literal and not closing the bracket expr */
				if (check_char(lex, ']'))
					uc_vector_push(&lex->buffer, ']');

				/* read until closing `]` */
				for (ch = next_char(lex); ch != EOF; ch = next_char(lex)) {
					if (ch == '\\') {
						err = parse_escape(lex, "^");

						if (err) {
							uc_vector_clear(&lex->buffer);

							return err;
						}

						continue;
					}

					uc_vector_push(&lex->buffer, ch);

					if (ch == ']')
						break;

					/* skip nested char classes / equivalence classes / collating chars */
					if (ch == '[') {
						code = lookahead_char(lex);

						if (code == ':' || code == '.' || code == '=') {
							uc_vector_push(&lex->buffer, code);
							next_char(lex);

							for (ch = next_char(lex); ch != EOF; ch = next_char(lex)) {
								if (ch == '\\') {
									err = parse_escape(lex, "");

									if (err) {
										uc_vector_clear(&lex->buffer);

										return err;
									}

									continue;
								}

								uc_vector_push(&lex->buffer, ch);

								if (ch == code && check_char(lex, ']')) {
									uc_vector_push(&lex->buffer, ']');
									break;
								}
							}
						}
					}
				}
			}

			break;

		/* escape sequence */
		case '\\':
			err = parse_escape(lex,
				(type == TK_REGEXP) ? "^bBdDsSwW<>.[$()|*+?{\\" : "");

			if (err) {
				uc_vector_clear(&lex->buffer);

				return err;
			}

			break;

		/* other character */
		default:
			/* terminating delimitter */
			if (ch == kind)
				return emit_buffer(lex, off, type, NULL);

			uc_vector_push(&lex->buffer, ch);
		}
	}

	// FIXME
	lex->state = UC_LEX_EOF;

	return emit_op(lex, lex->lastoff, TK_ERROR, ucv_string_new("Unterminated string"));
}


/*
 * Parses a regexp literal from the given buffer.
 *
 * Returns a negative value on error, otherwise the amount of consumed
 * characters from the given buffer.
 *
 * Error values:
 *  -UC_ERROR_UNTERMINATED_STRING	Unterminated regexp
 *  -UC_ERROR_INVALID_ESCAPE		Invalid escape sequence
 *  -UC_ERROR_OVERLONG_STRING		Regexp literal too long
 *  -UC_ERROR_INVALID_REGEXP        Could not compile regexp
 */

enum {
	UC_LEX_PARSE_REGEX_INIT,
	UC_LEX_PARSE_REGEX_PATTERN,
	UC_LEX_PARSE_REGEX_FLAGS
};

static uc_token_t *
parse_regexp(uc_lexer_t *lex)
{
	bool is_reg_global = false, is_reg_icase = false, is_reg_newline = false;
	uc_token_t *rv;
	size_t len;
	char *s;

	rv = parse_string(lex, '/');

	if (rv->type == TK_REGEXP) {
		while (true) {
			if (check_char(lex, 'g'))
				is_reg_global = true;
			else if (check_char(lex, 'i'))
				is_reg_icase = true;
			else if (check_char(lex, 's'))
				is_reg_newline = true;
			else
				break;
		}

		/* encode the flags byte followed by the raw pattern bytes; a plain
		 * printf conversion cannot be used as the pattern may contain NUL */
		len = ucv_string_length(rv->uv);
		s = xalloc(len + 1);
		s[0] = (is_reg_global << 0) | (is_reg_icase << 1) | (is_reg_newline << 2);
		memcpy(s + 1, ucv_string_get(rv->uv), len);

		ucv_free(rv->uv, false);
		rv->uv = ucv_string_new_length(s, len + 1);
		free(s);
	}

	return rv;
}


/*
 * Parses a label from the given buffer.
 *
 * Returns a negative value on error, otherwise the amount of consumed
 * characters from the given buffer.
 *
 * Error values:
 *  -UC_ERROR_OVERLONG_STRING	Label too long
 */

static uc_token_t *
parse_label(uc_lexer_t *lex, int ch)
{
	const struct keyword *word;
	size_t i, len;

	while (true) {
		uc_vector_push(&lex->buffer, ch);
		ch = lookahead_char(lex);

		if (!isalnum(ch) && ch != '_')
			break;

		next_char(lex);
	}

	len = lex->buffer.count;

	if (!lex->no_keyword) {
		for (i = 0, word = &reserved_words[0]; i < ARRAY_SIZE(reserved_words); i++, word = &reserved_words[i]) {
			if (lex->buffer.count == word->plen && !strncmp(uc_vector_first(&lex->buffer), word->pat, word->plen)) {
				uc_vector_clear(&lex->buffer);

				return emit_op(lex, -len, word->type, NULL);
			}
		}
	}

	return emit_buffer(lex, -len, TK_LABEL, NULL);
}


/*
 * Parses a number literal from the given buffer.
 *
 * Returns a negative value on error, otherwise the amount of consumed
 * characters from the given buffer.
 *
 * Error values:
 *  -UC_ERROR_INVALID_ESCAPE	Invalid number character
 */

static inline bool
is_numeric_char(uc_lexer_t *lex, char c)
{
	char prev = lex->buffer.count ? *uc_vector_last(&lex->buffer) : 0;

	switch (c|32) {
	case '.':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		return true;

	case 'a':
	case 'b':
	case 'c':
	case 'd':
	case 'e':
	case 'f':
	case 'o':
	case 'x':
		/* require previous char, a number literal cannot start with these */
		return prev != 0;

	case '+':
	case '-':
		/* sign is only allowed after a decimal exponent char; in a hex
		 * literal `e`/`E` is a digit, not an exponent marker */
		if ((prev|32) != 'e')
			return false;

		if (lex->buffer.count >= 2) {
			char *b = uc_vector_first(&lex->buffer);

			if (b[0] == '0' && (b[1]|32) == 'x')
				return false;
		}

		return true;
	}

	return false;
}

static uc_token_t *
parse_number(uc_lexer_t *lex, int ch)
{
	uc_value_t *nv = NULL;
	size_t len;
	char *e;

	while (true) {
		uc_vector_push(&lex->buffer, ch);
		ch = lookahead_char(lex);

		if (!is_numeric_char(lex, ch))
			break;

		next_char(lex);
	}

	len = lex->buffer.count;

	uc_vector_push(&lex->buffer, '\0');

	nv = uc_number_parse_octal(uc_vector_first(&lex->buffer), &e);

	uc_vector_clear(&lex->buffer);

	switch (ucv_type(nv)) {
	case UC_DOUBLE:
		return emit_op(lex, -len, TK_DOUBLE, nv);

	case UC_INTEGER:
		return emit_op(lex, -len, TK_NUMBER, nv);

	default:
		return emit_op(lex, -len, TK_ERROR, ucv_string_new("Invalid number literal"));
	}
}

static uc_token_t *
lex_find_token(uc_lexer_t *lex)
{
	bool tpl = !(lex->config && lex->config->raw_mode);
	int ch = next_char(lex);

	while (isspace(ch))
		ch = next_char(lex);

	switch (ch) {
	case '~':
		return emit_op(lex, -1, TK_COMPL, NULL);

	case '}':
		if (tpl && check_char(lex, '}'))
			return emit_op(lex, -2, TK_REXP, NULL);

		return emit_op(lex, -1, TK_RBRACE, NULL);

	case '|':
		if (check_char(lex, '|')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_ASOR, NULL);

			return emit_op(lex, -2, TK_OR, NULL);
		}

		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_ASBOR, NULL);

		return emit_op(lex, -1, TK_BOR, NULL);

	case '{':
		if (tpl && check_char(lex, '{'))
			return emit_op(lex, -2, TK_LEXP, NULL);

		if (tpl && check_char(lex, '%'))
			return emit_op(lex, -2, TK_LSTM, NULL);

		return emit_op(lex, -1, TK_LBRACE, NULL);

	case '^':
		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_ASBXOR, NULL);

		return emit_op(lex, -1, TK_BXOR, NULL);

	case '[':
		return emit_op(lex, -1, TK_LBRACK, NULL);

	case ']':
		return emit_op(lex, -1, TK_RBRACK, NULL);

	case '?':
		if (check_char(lex, '?')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_ASNULLISH, NULL);

			return emit_op(lex, -2, TK_NULLISH, NULL);
		}

		if (check_char(lex, '.')) {
			if (check_char(lex, '['))
				return emit_op(lex, -3, TK_QLBRACK, NULL);

			if (check_char(lex, '('))
				return emit_op(lex, -3, TK_QLPAREN, NULL);

			return emit_op(lex, -2, TK_QDOT, NULL);
		}

		return emit_op(lex, -1, TK_QMARK, NULL);

	case '>':
		if (check_char(lex, '>')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_ASRIGHT, NULL);

			return emit_op(lex, -2, TK_RSHIFT, NULL);
		}

		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_GE, NULL);

		return emit_op(lex, -1, TK_GT, NULL);

	case '=':
		if (check_char(lex, '=')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_EQS, NULL);

			return emit_op(lex, -2, TK_EQ, NULL);
		}

		if (check_char(lex, '>'))
			return emit_op(lex, -2, TK_ARROW, NULL);

		return emit_op(lex, -1, TK_ASSIGN, NULL);

	case '<':
		if (check_char(lex, '<')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_ASLEFT, NULL);

			return emit_op(lex, -2, TK_LSHIFT, NULL);
		}

		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_LE, NULL);

		return emit_op(lex, -1, TK_LT, NULL);

	case ';':
		return emit_op(lex, -1, TK_SCOL, NULL);

	case ':':
		return emit_op(lex, -1, TK_COLON, NULL);

	case '/':
		ch = lookahead_char(lex);
		lex->lastoff = lex->off - 1;

		if (ch == '/' || ch == '*')
			return parse_comment(lex, ch);

		if (lex->no_regexp) {
			if (check_char(lex, '='))
				return emit_op(lex, -2, TK_ASDIV, NULL);

			return emit_op(lex, -1, TK_DIV, NULL);
		}

		return parse_regexp(lex);

	case '.':
		if (check_char(lex, '.')) {
			if (check_char(lex, '.'))
				return emit_op(lex, -3, TK_ELLIP, NULL);

			/* The sequence ".." cannot be a valid */
			return emit_op(lex, -2, TK_ERROR, ucv_string_new("Unexpected character"));
		}

		return emit_op(lex, -1, TK_DOT, NULL);

	case '-':
		if (tpl && check_char(lex, '}')) {
			if (check_char(lex, '}')) {
				lex->modifier = MINUS;

				return emit_op(lex, -3, TK_REXP, NULL);
			}

			/* The sequence "-}" cannot be a valid */
			return emit_op(lex, -1, TK_ERROR, ucv_string_new("Unexpected character"));
		}

		if (tpl && check_char(lex, '%')) {
			if (check_char(lex, '}')) {
				lex->modifier = MINUS;

				return emit_op(lex, -3, TK_RSTM, NULL);
			}

			/* The sequence "-%" cannot be a valid */
			return emit_op(lex, -1, TK_ERROR, ucv_string_new("Unexpected character"));
		}

		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_ASSUB, NULL);

		if (check_char(lex, '-'))
			return emit_op(lex, -2, TK_DEC, NULL);

		return emit_op(lex, -1, TK_SUB, NULL);

	case ',':
		return emit_op(lex, -1, TK_COMMA, NULL);

	case '+':
		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_ASADD, NULL);

		if (check_char(lex, '+'))
			return emit_op(lex, -2, TK_INC, NULL);

		return emit_op(lex, -1, TK_ADD, NULL);

	case '*':
		if (check_char(lex, '*')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_ASEXP, NULL);

			return emit_op(lex, -2, TK_EXP, NULL);
		}

		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_ASMUL, NULL);

		return emit_op(lex, -1, TK_MUL, NULL);

	case '(':
		return emit_op(lex, -1, TK_LPAREN, NULL);

	case ')':
		return emit_op(lex, -1, TK_RPAREN, NULL);

	case '\'':
	case '"':
	case '`':
		lex->lastoff = lex->off - 1;

		return parse_string(lex, ch);

	case '&':
		if (check_char(lex, '&')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_ASAND, NULL);

			return emit_op(lex, -2, TK_AND, NULL);
		}

		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_ASBAND, NULL);

		return emit_op(lex, -1, TK_BAND, NULL);

	case '%':
		if (tpl && check_char(lex, '}'))
			return emit_op(lex, -2, TK_RSTM, NULL);

		if (check_char(lex, '='))
			return emit_op(lex, -2, TK_ASMOD, NULL);

		return emit_op(lex, -1, TK_MOD, NULL);

	case '!':
		if (check_char(lex, '=')) {
			if (check_char(lex, '='))
				return emit_op(lex, -3, TK_NES, NULL);

			return emit_op(lex, -2, TK_NE, NULL);
		}

		return emit_op(lex, -1, TK_NOT, NULL);

	case EOF:
		return emit_op(lex, -1, TK_EOF, NULL);

	default:
		if (isalpha(ch) || ch == '_')
			return parse_label(lex, ch);

		if (isdigit(ch))
			return parse_number(lex, ch);

		return emit_op(lex, -1, TK_ERROR, ucv_string_new("Unexpected character"));
	}
}

static uc_token_t *
lex_step(uc_lexer_t *lex)
{
	const char *strip = NULL;
	uc_token_t *tok;
	size_t *nest;
	int ch;

	while (lex->state != UC_LEX_EOF) {
		switch (lex->state) {
		case UC_LEX_IDENTIFY_BLOCK:
			ch = next_char(lex);

			/* previous block had strip trailing whitespace flag, skip leading whitespace */
			if (lex->modifier == MINUS) {
				while (isspace(ch))
					ch = next_char(lex);

				lex->modifier = UNSPEC;
			}

			/* previous block was a statement block and trim_blocks is enabled, skip leading newline */
			else if (lex->modifier == NEWLINE) {
				if (ch == '\n')
					ch = next_char(lex);

				lex->modifier = UNSPEC;
			}

			/* scan forward through buffer to identify block start token */
			while (ch != EOF) {
				if (ch == '{') {
					ch = next_char(lex);

					switch (ch) {
					/* found start of comment block */
					case '#':
						lex->state = UC_LEX_BLOCK_COMMENT;
						lex->block = COMMENT;

						if (check_char(lex, '-'))
							strip = " \n\t\v\f\r";

						break;

					/* found start of expression block */
					case '{':
						lex->state = UC_LEX_BLOCK_EXPRESSION_EMIT_TAG;

						if (check_char(lex, '-'))
							strip = " \n\t\v\f\r";

						break;

					/* found start of statement block */
					case '%':
						lex->state = UC_LEX_BLOCK_STATEMENT_EMIT_TAG;

						if (check_char(lex, '-'))
							strip = " \n\t\v\f\r";
						else if (check_char(lex, '+'))
							strip = NULL;
						else if (lex->config && lex->config->lstrip_blocks)
							strip = " \t\v\f\r";

						break;

					default:
						/* not a start tag, remember char and move on */
						uc_vector_push(&lex->buffer, '{');
						continue;
					}

					break;
				}

				uc_vector_push(&lex->buffer, ch);
				ch = next_char(lex);
			}

			if (ch == EOF)
				lex->state = UC_LEX_EOF;

			/* push out leading text */
			tok = emit_buffer(lex, lex->lastoff, TK_TEXT, strip);
			lex->lastoff = lex->off - 2;

			if (!tok)
				continue;

			tok->end -= 2;

			return tok;


		case UC_LEX_BLOCK_COMMENT:
			ch = next_char(lex);

			/* scan forward through buffer to identify end token */
			while (ch != EOF) {
				if (ch == '-' && check_char(lex, '#') && check_char(lex, '}')) {
					lex->modifier = MINUS;
					break;
				}

				if (ch == '#' && check_char(lex, '}'))
					break;

				ch = next_char(lex);
			}

			if (ch == EOF) {
				lex->state = UC_LEX_EOF;

				return emit_op(lex, lex->lastoff, TK_ERROR, ucv_string_new("Unterminated template block"));
			}

			tok = emit_op(lex, lex->lastoff, TK_COMMENT, NULL);

			lex->lastoff = lex->off;
			lex->state = UC_LEX_IDENTIFY_BLOCK;

			return tok;

		case UC_LEX_BLOCK_EXPRESSION_EMIT_TAG:
			lex->state = UC_LEX_IDENTIFY_TOKEN;
			lex->block = EXPRESSION;

			return emit_op(lex, lex->off - 2, TK_LEXP, NULL);

		case UC_LEX_BLOCK_STATEMENT_EMIT_TAG:
			lex->state = UC_LEX_IDENTIFY_TOKEN;
			lex->block = STATEMENTS;

			return emit_op(lex, lex->off - 2, TK_LSTM, NULL);

		case UC_LEX_IDENTIFY_TOKEN:
			do { tok = lex_find_token(lex); } while (tok == NULL);

			/* disallow nesting blocks */
			if (tok->type == TK_LSTM || tok->type == TK_LEXP)
				return emit_op(lex, -2, TK_ERROR, ucv_string_new("Template blocks may not be nested"));

			/* found end of statement block */
			if (lex->block == STATEMENTS && tok->type == TK_RSTM) {
				/* strip newline after statement block? */
				if (lex->modifier == UNSPEC && lex->config && lex->config->trim_blocks)
					lex->modifier = NEWLINE;

				lex->lastoff = lex->off;
				lex->state = UC_LEX_IDENTIFY_BLOCK;
				lex->block = NONE;

				tok = emit_op(lex, -2, TK_RSTM, NULL);
			}

			/* found end of expression block */
			else if (lex->block == EXPRESSION && tok->type == TK_REXP) {
				lex->lastoff = lex->off;
				lex->state = UC_LEX_IDENTIFY_BLOCK;
				lex->block = NONE;
			}

			/* track opening braces */
			else if (tok->type == TK_LBRACE && lex->templates.count > 0) {
				nest = uc_vector_last(&lex->templates);
				(*nest)++;
			}

			/* check end of placeholder expression */
			else if (tok->type == TK_RBRACE && lex->templates.count > 0) {
				nest = uc_vector_last(&lex->templates);

				if (*nest == 0) {
					lex->templates.count--;
					lex->state = UC_LEX_PLACEHOLDER_END;
				}
				else {
					(*nest)--;
				}
			}

			/* premature EOF? */
			else if (tok->type == TK_EOF && lex->block != STATEMENTS) {
				lex->state = UC_LEX_EOF;

				return emit_op(lex, -2, TK_ERROR, ucv_string_new("Unterminated template block"));
			}

			return tok;


		case UC_LEX_PLACEHOLDER_START:
			lex->state = UC_LEX_IDENTIFY_TOKEN;

			uc_vector_push(&lex->templates, 0);

			return emit_op(lex, -2, TK_PLACEH, NULL);


		case UC_LEX_PLACEHOLDER_END:
			lex->state = UC_LEX_IDENTIFY_TOKEN;

			tok = parse_string(lex, '`');
			tok->pos++;

			return tok;


		case UC_LEX_EOF:
			break;
		}
	}

	return emit_op(lex, lex->off, TK_EOF, NULL);
}

void
uc_lexer_init(uc_lexer_t *lex, uc_parse_config_t *config, uc_source_t *source)
{
	lex->state = UC_LEX_IDENTIFY_BLOCK;

	lex->config = config;
	lex->source = uc_source_get(source);

	lex->block = NONE;
	lex->modifier = UNSPEC;

	lex->rlen = 0;
	lex->rpos = 0;
	lex->off = 0;
	lex->rbuf = NULL;

	lex->buffer.count = 0;
	lex->buffer.entries = NULL;

	lex->lead_surrogate = 0;

	lex->lastoff = 0;

	lex->templates.count = 0;
	lex->templates.entries = NULL;

	if (config && config->raw_mode) {
		lex->state = UC_LEX_IDENTIFY_TOKEN;
		lex->block = STATEMENTS;
	}
}

void
uc_lexer_free(uc_lexer_t *lex)
{
	uc_vector_clear(&lex->buffer);
	uc_vector_clear(&lex->templates);

	uc_source_put(lex->source);
}

uc_token_t *
uc_lexer_next_token(uc_lexer_t *lex)
{
	uc_token_t *rv = NULL;

	rv = lex_step(lex);

	if (rv && rv->type != TK_COMMENT) {
		lex->no_keyword = false;
		lex->no_regexp = false;
	}

	return rv;
}

const char *
uc_tokenname(unsigned type)
{
	static char buf[sizeof("'endfunction'")];
	const char *tokennames[] = {
		[TK_LEXP] = "'{{'",
		[TK_REXP] = "'}}'",
		[TK_LSTM] = "'{%'",
		[TK_RSTM] = "'%}'",
		[TK_COMMA] = "','",
		[TK_ASSIGN] = "'='",
		[TK_ASADD] = "'+='",
		[TK_ASSUB] = "'-='",
		[TK_ASMUL] = "'*='",
		[TK_ASDIV] = "'/='",
		[TK_ASMOD] = "'%='",
		[TK_ASLEFT] = "'<<='",
		[TK_ASRIGHT] = "'>>='",
		[TK_ASBAND] = "'&='",
		[TK_ASBXOR] = "'^='",
		[TK_ASBOR] = "'|='",
		[TK_QMARK] = "'?'",
		[TK_COLON] = "':'",
		[TK_OR] = "'||'",
		[TK_AND] = "'&&'",
		[TK_BOR] = "'|'",
		[TK_BXOR] = "'^'",
		[TK_BAND] = "'&'",
		[TK_EQS] = "'==='",
		[TK_NES] = "'!=='",
		[TK_EQ] = "'=='",
		[TK_NE] = "'!='",
		[TK_LT] = "'<'",
		[TK_LE] = "'<='",
		[TK_GT] = "'>'",
		[TK_GE] = "'>='",
		[TK_LSHIFT] = "'<<'",
		[TK_RSHIFT] = "'>>'",
		[TK_ADD] = "'+'",
		[TK_SUB] = "'-'",
		[TK_MUL] = "'*'",
		[TK_DIV] = "'/'",
		[TK_MOD] = "'%'",
		[TK_EXP] = "'**'",
		[TK_NOT] = "'!'",
		[TK_COMPL] = "'~'",
		[TK_INC] = "'++'",
		[TK_DEC] = "'--'",
		[TK_DOT] = "'.'",
		[TK_LBRACK] = "'['",
		[TK_RBRACK] = "']'",
		[TK_LPAREN] = "'('",
		[TK_RPAREN] = "')'",
		[TK_LBRACE] = "'{'",
		[TK_RBRACE] = "'}'",
		[TK_SCOL] = "';'",
		[TK_ELLIP] = "'...'",
		[TK_ARROW] = "'=>'",
		[TK_QLBRACK] = "'?.['",
		[TK_QLPAREN] = "'?.('",
		[TK_QDOT] = "'?.'",
		[TK_ASEXP] = "'**='",
		[TK_ASAND] = "'&&='",
		[TK_ASOR] = "'||='",
		[TK_ASNULLISH] = "'\?\?='",
		[TK_NULLISH] = "'\?\?'",
		[TK_PLACEH] = "'${'",

		[TK_TEXT] = "Text",
		[TK_LABEL] = "Label",
		[TK_NUMBER] = "Number",
		[TK_DOUBLE] = "Double",
		[TK_STRING] = "String",
		[TK_REGEXP] = "Regexp",
		[TK_TEMPLATE] = "Template",
		[TK_ERROR] = "Error",
		[TK_EOF] = "End of file",
	};

	size_t i;

	for (i = 0; i < ARRAY_SIZE(reserved_words); i++) {
		if (reserved_words[i].type != type)
			continue;

		snprintf(buf, sizeof(buf), "'%s'", reserved_words[i].pat);

		return buf;
	}

	return tokennames[type] ? tokennames[type] : "?";
}

bool
uc_lexer_is_keyword(uc_value_t *label)
{
	size_t i;

	if (ucv_type(label) != UC_STRING)
		return false;

	for (i = 0; i < ARRAY_SIZE(reserved_words); i++)
		if (!strcmp(reserved_words[i].pat, ucv_string_get(label)))
			return true;

	return false;
}

#endif /* NO_COMPILE */

/*
 * Stores the given codepoint as a utf8 multibyte sequence into the given
 * output buffer and subtracts the required amount of bytes from the given
 * length pointer.
 *
 * Returns false if the multibyte sequence would not fit into the buffer,
 * otherwise true.
 */

bool
utf8enc(char **out, int *rem, int code)
{
	if (code >= 0 && code <= 0x7F) {
		if (*rem < 1)
			return false;

		*(*out)++ = code; (*rem)--;

		return true;
	}
	else if (code > 0 && code <= 0x7FF) {
		if (*rem < 2)
			return false;

		*(*out)++ = ((code >>  6) & 0x1F) | 0xC0; (*rem)--;
		*(*out)++ = ( code        & 0x3F) | 0x80; (*rem)--;

		return true;
	}
	else if (code > 0 && code <= 0xFFFF) {
		if (*rem < 3)
			return false;

		*(*out)++ = ((code >> 12) & 0x0F) | 0xE0; (*rem)--;
		*(*out)++ = ((code >>  6) & 0x3F) | 0x80; (*rem)--;
		*(*out)++ = ( code        & 0x3F) | 0x80; (*rem)--;

		return true;
	}
	else if (code > 0 && code <= 0x10FFFF) {
		if (*rem < 4)
			return false;

		*(*out)++ = ((code >> 18) & 0x07) | 0xF0; (*rem)--;
		*(*out)++ = ((code >> 12) & 0x3F) | 0x80; (*rem)--;
		*(*out)++ = ((code >>  6) & 0x3F) | 0x80; (*rem)--;
		*(*out)++ = ( code        & 0x3F) | 0x80; (*rem)--;

		return true;
	}

	return true;
}

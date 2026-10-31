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

/* Incremental JSON parser that builds ucode values. Its grammar, its errors
 * and the positions where it stops follow the tokener of json-c 0.18 in its
 * default mode, which json() used before, so that scripts see the same
 * results: comments, single quoted strings, NaN, Infinity, literals in any
 * case and trailing commas are accepted, and a NUL byte ends the input.
 * Unlike json-c, object keys keep NUL bytes. */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ucode/json.h"
#include "ucode/internal/types.h"

#define UC_JSON_MAX_DEPTH 32

typedef enum {
	STATE_EATWS,
	STATE_START,
	STATE_FINISH,
	STATE_NULL,
	STATE_COMMENT_START,
	STATE_COMMENT,
	STATE_COMMENT_EOL,
	STATE_COMMENT_END,
	STATE_STRING,
	STATE_STRING_ESCAPE,
	STATE_ESCAPE_UNICODE,
	STATE_ESCAPE_UNICODE_NEED_ESCAPE,
	STATE_ESCAPE_UNICODE_NEED_U,
	STATE_BOOLEAN,
	STATE_NUMBER,
	STATE_ARRAY,
	STATE_ARRAY_AFTER_SEP,
	STATE_ARRAY_ADD,
	STATE_ARRAY_SEP,
	STATE_OBJECT_FIELD_START,
	STATE_OBJECT_FIELD_START_AFTER_SEP,
	STATE_OBJECT_FIELD,
	STATE_OBJECT_FIELD_END,
	STATE_OBJECT_VALUE,
	STATE_OBJECT_VALUE_ADD,
	STATE_OBJECT_SEP,
	STATE_INF,
} uc_json_state_t;

typedef struct {
	uc_json_state_t state, saved_state;
	uc_value_t *current;
	uc_value_t *key;
} uc_json_level_t;

struct uc_json_parser {
	uc_vm_t *vm;
	uc_json_level_t stack[UC_JSON_MAX_DEPTH];
	int depth;
	uc_json_error_t err;
	size_t offset;
	uc_stringbuf_t pb;
	char quote;
	int st_pos;
	bool is_double;
	unsigned int ucs_char, high_surrogate;
};

static const char *uc_json_errors[] = {
	"success",
	"continue",
	"nesting too deep",
	"unexpected end of data",
	"unexpected character",
	"null expected",
	"boolean expected",
	"number expected",
	"array value separator ',' expected",
	"quoted object property name expected",
	"object property name separator ':' expected",
	"object value separator ',' expected",
	"invalid string sequence",
	"expected comment",
};

static const char replacement_char[3] = { (char)0xef, (char)0xbf, (char)0xbd };

const char *
uc_json_error_desc(uc_json_error_t err)
{
	if ((size_t)err >= sizeof(uc_json_errors) / sizeof(uc_json_errors[0]))
		return "unknown error";

	return uc_json_errors[err];
}

static bool
uc_json_is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool
uc_json_is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static unsigned int
uc_json_hex(char c)
{
	return (c <= '9') ? c - '0' : (c & 7) + 9;
}

static void
uc_json_level_reset(uc_json_parser_t *p, int depth)
{
	uc_json_level_t *l = &p->stack[depth];

	l->state = STATE_EATWS;
	l->saved_state = STATE_START;
	ucv_put(l->current);
	l->current = NULL;
	ucv_put(l->key);
	l->key = NULL;
}

static void
uc_json_reset(uc_json_parser_t *p)
{
	for (int i = p->depth; i >= 0; i--)
		uc_json_level_reset(p, i);

	p->depth = 0;
}

uc_json_parser_t *
uc_json_parser_new(uc_vm_t *vm)
{
	uc_json_parser_t *p = xalloc(sizeof(*p));

	p->vm = vm;
	uc_json_reset(p);

	return p;
}

void
uc_json_parser_free(uc_json_parser_t *p)
{
	if (!p)
		return;

	uc_json_reset(p);
	free(p->pb.buf);
	free(p);
}

uc_json_error_t
uc_json_parser_error(uc_json_parser_t *p)
{
	return p->err;
}

size_t
uc_json_parser_end(uc_json_parser_t *p)
{
	return p->offset;
}

static void
uc_json_utf8_add(uc_stringbuf_t *pb, unsigned int c)
{
	char u[4];

	if (c < 0x80) {
		u[0] = c;
		uc_stringbuf_add(pb, u, 1);
	}
	else if (c < 0x800) {
		u[0] = 0xc0 | (c >> 6);
		u[1] = 0x80 | (c & 0x3f);
		uc_stringbuf_add(pb, u, 2);
	}
	else if (c < 0x10000) {
		u[0] = 0xe0 | (c >> 12);
		u[1] = 0x80 | ((c >> 6) & 0x3f);
		u[2] = 0x80 | (c & 0x3f);
		uc_stringbuf_add(pb, u, 3);
	}
	else if (c < 0x110000) {
		u[0] = 0xf0 | ((c >> 18) & 0x07);
		u[1] = 0x80 | ((c >> 12) & 0x3f);
		u[2] = 0x80 | ((c >> 6) & 0x3f);
		u[3] = 0x80 | (c & 0x3f);
		uc_stringbuf_add(pb, u, 4);
	}
	else {
		uc_stringbuf_add(pb, replacement_char, 3);
	}
}

/* Converts the collected number text, as json_parse_int64(),
 * json_parse_uint64() and strtod() did for json-c and ucv_from_json() did
 * with the result; returns false if it is no number. */
static bool
uc_json_number(uc_json_parser_t *p, uc_value_t **res)
{
	const char *s = p->pb.buf;
	uint64_t u64;
	int64_t n64;
	double d;
	char *end;

	if (p->is_double) {
		d = strtod(s, &end);

		if (end != s + p->pb.bpos)
			return false;

		*res = ucv_double_new(d);

		return true;
	}

	errno = 0;

	if (*s == '-') {
		n64 = strtoll(s, &end, 10);

		if (end == s || (n64 == 0 && errno != 0))
			return false;

		*res = ucv_int64_new(n64);

		return true;
	}

	u64 = strtoull(s, &end, 10);

	if (end == s || (u64 == 0 && errno != 0))
		return false;

	*res = (u64 < INT64_MAX) ? ucv_int64_new((int64_t)u64) : ucv_uint64_new(u64);

	return true;
}

#define state p->stack[p->depth].state
#define saved_state p->stack[p->depth].saved_state
#define current p->stack[p->depth].current
#define field_key p->stack[p->depth].key

/* Takes the next input byte into `dest`; at the end of the input, sets
 * the result of the call, which is complete if a top-level value is. */
#define PEEK_CHAR(dest) \
	((p->offset == len) \
		? ((p->depth == 0 && state == STATE_EATWS && saved_state == STATE_FINISH) \
			? ((p->err = UC_JSON_SUCCESS), 0) \
			: ((p->err = UC_JSON_CONTINUE), 0)) \
		: (((dest) = *str), 1))

/* Advances to the next byte; yields the previous byte, 0 at a NUL byte */
#define ADVANCE_CHAR() (++str, p->offset++, c)

uc_value_t *
uc_json_parse(uc_json_parser_t *p, const char *str, size_t len)
{
	uc_value_t *obj = NULL, *res;
	const char *case_start;
	size_t case_len, size;
	bool is_exponent, neg_sign_ok, pos_sign_ok;
	char c = '\1', *e_loc, last;

	p->offset = 0;
	p->err = UC_JSON_SUCCESS;

	while (PEEK_CHAR(c)) {
redo_char:
		switch (state) {
		case STATE_EATWS:
			while (uc_json_is_ws(c))
				if (!ADVANCE_CHAR() || !PEEK_CHAR(c))
					goto out;

			if (c == '/') {
				uc_stringbuf_reset(&p->pb);
				uc_stringbuf_add(&p->pb, &c, 1);
				state = STATE_COMMENT_START;
			}
			else {
				state = saved_state;
				goto redo_char;
			}

			break;

		case STATE_START:
			switch (c) {
			case '{':
				state = STATE_EATWS;
				saved_state = STATE_OBJECT_FIELD_START;
				current = ucv_object_new(p->vm);
				break;

			case '[':
				state = STATE_EATWS;
				saved_state = STATE_ARRAY;
				current = ucv_array_new(p->vm);
				break;

			case 'I':
			case 'i':
				state = STATE_INF;
				uc_stringbuf_reset(&p->pb);
				p->st_pos = 0;
				goto redo_char;

			case 'N':
			case 'n':
				state = STATE_NULL;
				uc_stringbuf_reset(&p->pb);
				p->st_pos = 0;
				goto redo_char;

			case '\'':
			case '"':
				state = STATE_STRING;
				uc_stringbuf_reset(&p->pb);
				p->quote = c;
				break;

			case 'T':
			case 't':
			case 'F':
			case 'f':
				state = STATE_BOOLEAN;
				uc_stringbuf_reset(&p->pb);
				p->st_pos = 0;
				goto redo_char;

			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
			case '-':
				state = STATE_NUMBER;
				uc_stringbuf_reset(&p->pb);
				p->is_double = false;
				goto redo_char;

			default:
				p->err = UC_JSON_ERROR_UNEXPECTED;
				goto out;
			}

			break;

		case STATE_FINISH:
			if (p->depth == 0)
				goto out;

			obj = current;
			current = NULL;
			uc_json_level_reset(p, p->depth);
			p->depth--;
			goto redo_char;

		case STATE_INF:
			while (p->st_pos < 8) {
				if (*str != "Infinity"[p->st_pos] && *str != "iNFINITY"[p->st_pos]) {
					p->err = UC_JSON_ERROR_UNEXPECTED;
					goto out;
				}

				p->st_pos++;
				(void)ADVANCE_CHAR();

				if (!PEEK_CHAR(c))
					goto out;
			}

			current = ucv_double_new((p->pb.bpos > 0 && p->pb.buf[0] == '-') ? -INFINITY : INFINITY);
			saved_state = STATE_FINISH;
			state = STATE_EATWS;
			goto redo_char;

		case STATE_NULL:
			uc_stringbuf_add(&p->pb, &c, 1);
			size = (p->st_pos + 1 < 4) ? p->st_pos + 1 : 4;

			if (strncasecmp("null", p->pb.buf, size) == 0) {
				if (p->st_pos == 4) {
					current = NULL;
					saved_state = STATE_FINISH;
					state = STATE_EATWS;
					goto redo_char;
				}
			}
			else if (strncasecmp("NaN", p->pb.buf, (p->st_pos + 1 < 3) ? p->st_pos + 1 : 3) == 0) {
				if (p->st_pos == 3) {
					current = ucv_double_new(NAN);
					saved_state = STATE_FINISH;
					state = STATE_EATWS;
					goto redo_char;
				}
			}
			else {
				p->err = UC_JSON_ERROR_NULL;
				goto out;
			}

			p->st_pos++;
			break;

		case STATE_COMMENT_START:
			if (c == '*') {
				state = STATE_COMMENT;
			}
			else if (c == '/') {
				state = STATE_COMMENT_EOL;
			}
			else {
				p->err = UC_JSON_ERROR_COMMENT;
				goto out;
			}

			uc_stringbuf_add(&p->pb, &c, 1);
			break;

		case STATE_COMMENT:
			case_start = str;

			while (c != '*') {
				if (!ADVANCE_CHAR() || !PEEK_CHAR(c)) {
					uc_stringbuf_add(&p->pb, case_start, str - case_start);
					goto out;
				}
			}

			uc_stringbuf_add(&p->pb, case_start, 1 + str - case_start);
			state = STATE_COMMENT_END;
			break;

		case STATE_COMMENT_EOL:
			case_start = str;

			while (c != '\n') {
				if (!ADVANCE_CHAR() || !PEEK_CHAR(c)) {
					uc_stringbuf_add(&p->pb, case_start, str - case_start);
					goto out;
				}
			}

			uc_stringbuf_add(&p->pb, case_start, str - case_start);
			state = STATE_EATWS;
			break;

		case STATE_COMMENT_END:
			uc_stringbuf_add(&p->pb, &c, 1);
			state = (c == '/') ? STATE_EATWS : STATE_COMMENT;
			break;

		case STATE_STRING:
		case STATE_OBJECT_FIELD:
			case_start = str;

			while (true) {
				if (c == p->quote) {
					uc_stringbuf_add(&p->pb, case_start, str - case_start);

					if (state == STATE_STRING) {
						current = ucv_string_new_length(p->pb.buf, p->pb.bpos);
						saved_state = STATE_FINISH;
					}
					else {
						field_key = ucv_string_new_length(p->pb.buf, p->pb.bpos);
						saved_state = STATE_OBJECT_FIELD_END;
					}

					state = STATE_EATWS;
					break;
				}
				else if (c == '\\') {
					uc_stringbuf_add(&p->pb, case_start, str - case_start);
					saved_state = state;
					state = STATE_STRING_ESCAPE;
					break;
				}

				if (!ADVANCE_CHAR() || !PEEK_CHAR(c)) {
					uc_stringbuf_add(&p->pb, case_start, str - case_start);
					goto out;
				}
			}

			break;

		case STATE_STRING_ESCAPE:
			switch (c) {
			case '"':
			case '\\':
			case '/':
				uc_stringbuf_add(&p->pb, &c, 1);
				state = saved_state;
				break;

			case 'b': uc_stringbuf_add(&p->pb, "\b", 1); state = saved_state; break;
			case 'n': uc_stringbuf_add(&p->pb, "\n", 1); state = saved_state; break;
			case 'r': uc_stringbuf_add(&p->pb, "\r", 1); state = saved_state; break;
			case 't': uc_stringbuf_add(&p->pb, "\t", 1); state = saved_state; break;
			case 'f': uc_stringbuf_add(&p->pb, "\f", 1); state = saved_state; break;

			case 'u':
				p->ucs_char = 0;
				p->st_pos = 0;
				state = STATE_ESCAPE_UNICODE;
				break;

			default:
				p->err = UC_JSON_ERROR_STRING;
				goto out;
			}

			break;

		case STATE_ESCAPE_UNICODE:
			while (true) {
				if (!c || !uc_json_is_hex(c)) {
					p->err = UC_JSON_ERROR_STRING;
					goto out;
				}

				p->ucs_char |= uc_json_hex(c) << ((3 - p->st_pos) * 4);
				p->st_pos++;

				if (p->st_pos >= 4)
					break;

				(void)ADVANCE_CHAR();

				if (!PEEK_CHAR(c))
					goto out;
			}

			p->st_pos = 0;

			if (p->high_surrogate) {
				if ((p->ucs_char & 0xfc00) == 0xdc00)
					p->ucs_char = ((p->high_surrogate & 0x3ff) << 10) +
					              (p->ucs_char & 0x3ff) + 0x10000;
				else
					uc_stringbuf_add(&p->pb, replacement_char, 3);

				p->high_surrogate = 0;
			}

			if ((p->ucs_char & 0xfc00) == 0xd800) {
				p->high_surrogate = p->ucs_char;
				p->ucs_char = 0;
				state = STATE_ESCAPE_UNICODE_NEED_ESCAPE;
				break;
			}

			if ((p->ucs_char & 0xfc00) == 0xdc00)
				uc_stringbuf_add(&p->pb, replacement_char, 3);
			else
				uc_json_utf8_add(&p->pb, p->ucs_char);

			state = saved_state;
			break;

		case STATE_ESCAPE_UNICODE_NEED_ESCAPE:
			if (!c || c != '\\') {
				uc_stringbuf_add(&p->pb, replacement_char, 3);
				p->high_surrogate = 0;
				p->ucs_char = 0;
				p->st_pos = 0;
				state = saved_state;
				goto redo_char;
			}

			state = STATE_ESCAPE_UNICODE_NEED_U;
			break;

		case STATE_ESCAPE_UNICODE_NEED_U:
			if (!c || c != 'u') {
				uc_stringbuf_add(&p->pb, replacement_char, 3);
				p->high_surrogate = 0;
				p->ucs_char = 0;
				p->st_pos = 0;
				state = STATE_STRING_ESCAPE;
				goto redo_char;
			}

			state = STATE_ESCAPE_UNICODE;
			break;

		case STATE_BOOLEAN:
			uc_stringbuf_add(&p->pb, &c, 1);

			if (strncasecmp("true", p->pb.buf, (p->st_pos + 1 < 4) ? p->st_pos + 1 : 4) == 0) {
				if (p->st_pos == 4) {
					current = ucv_boolean_new(true);
					saved_state = STATE_FINISH;
					state = STATE_EATWS;
					goto redo_char;
				}
			}
			else if (strncasecmp("false", p->pb.buf, (p->st_pos + 1 < 5) ? p->st_pos + 1 : 5) == 0) {
				if (p->st_pos == 5) {
					current = ucv_boolean_new(false);
					saved_state = STATE_FINISH;
					state = STATE_EATWS;
					goto redo_char;
				}
			}
			else {
				p->err = UC_JSON_ERROR_BOOLEAN;
				goto out;
			}

			p->st_pos++;
			break;

		case STATE_NUMBER:
			case_start = str;
			case_len = 0;
			is_exponent = false;
			neg_sign_ok = true;
			pos_sign_ok = false;

			/* resume the flags of a number that the last input ended in */
			if (p->pb.bpos > 0) {
				e_loc = memchr(p->pb.buf, 'e', p->pb.bpos);

				if (!e_loc)
					e_loc = memchr(p->pb.buf, 'E', p->pb.bpos);

				if (e_loc) {
					is_exponent = true;
					pos_sign_ok = neg_sign_ok = (e_loc == &p->pb.buf[p->pb.bpos - 1]);
				}
			}

			while (c && ((c >= '0' && c <= '9') ||
			             (!is_exponent && (c == 'e' || c == 'E')) ||
			             (neg_sign_ok && c == '-') || (pos_sign_ok && c == '+') ||
			             (!p->is_double && c == '.'))) {
				pos_sign_ok = neg_sign_ok = false;
				case_len++;

				if (c == '.') {
					p->is_double = true;
					pos_sign_ok = neg_sign_ok = true;
				}
				else if (c == 'e' || c == 'E') {
					is_exponent = true;
					p->is_double = true;
					pos_sign_ok = neg_sign_ok = true;
				}

				if (!ADVANCE_CHAR() || !PEEK_CHAR(c)) {
					uc_stringbuf_add(&p->pb, case_start, case_len);
					goto out;
				}
			}

			/* within a container, a byte that cannot follow a number is
			 * an error; at the top level, it may start the next input */
			if (p->depth > 0 && c != ',' && c != ']' && c != '}' && c != '/' &&
			    c != 'I' && c != 'i' && !uc_json_is_ws(c)) {
				p->err = UC_JSON_ERROR_NUMBER;
				goto out;
			}

			if (case_len > 0)
				uc_stringbuf_add(&p->pb, case_start, case_len);

			if (p->pb.buf[0] == '-' && case_len <= 1 && (c == 'i' || c == 'I')) {
				state = STATE_INF;
				p->st_pos = 0;
				goto redo_char;
			}

			/* "123e+" and the like parse as their digits */
			while (p->is_double && p->pb.bpos > 1) {
				last = p->pb.buf[p->pb.bpos - 1];

				if (last != 'e' && last != 'E' && last != '-' && last != '+')
					break;

				p->pb.buf[--p->pb.bpos] = 0;
			}

			if (!uc_json_number(p, &res)) {
				p->err = UC_JSON_ERROR_NUMBER;
				goto out;
			}

			current = res;
			saved_state = STATE_FINISH;
			state = STATE_EATWS;
			goto redo_char;

		case STATE_ARRAY_AFTER_SEP:
		case STATE_ARRAY:
			if (c == ']') {
				saved_state = STATE_FINISH;
				state = STATE_EATWS;
			}
			else {
				if (p->depth >= UC_JSON_MAX_DEPTH - 1) {
					p->err = UC_JSON_ERROR_DEPTH;
					goto out;
				}

				state = STATE_ARRAY_ADD;
				p->depth++;
				uc_json_level_reset(p, p->depth);
				goto redo_char;
			}

			break;

		case STATE_ARRAY_ADD:
			ucv_array_push(current, obj);
			obj = NULL;
			saved_state = STATE_ARRAY_SEP;
			state = STATE_EATWS;
			goto redo_char;

		case STATE_ARRAY_SEP:
			if (c == ']') {
				saved_state = STATE_FINISH;
				state = STATE_EATWS;
			}
			else if (c == ',') {
				saved_state = STATE_ARRAY_AFTER_SEP;
				state = STATE_EATWS;
			}
			else {
				p->err = UC_JSON_ERROR_ARRAY;
				goto out;
			}

			break;

		case STATE_OBJECT_FIELD_START:
		case STATE_OBJECT_FIELD_START_AFTER_SEP:
			if (c == '}') {
				saved_state = STATE_FINISH;
				state = STATE_EATWS;
			}
			else if (c == '"' || c == '\'') {
				p->quote = c;
				uc_stringbuf_reset(&p->pb);
				state = STATE_OBJECT_FIELD;
			}
			else {
				p->err = UC_JSON_ERROR_KEY_NAME;
				goto out;
			}

			break;

		case STATE_OBJECT_FIELD_END:
			if (c == ':') {
				saved_state = STATE_OBJECT_VALUE;
				state = STATE_EATWS;
			}
			else {
				p->err = UC_JSON_ERROR_KEY_SEP;
				goto out;
			}

			break;

		case STATE_OBJECT_VALUE:
			if (p->depth >= UC_JSON_MAX_DEPTH - 1) {
				p->err = UC_JSON_ERROR_DEPTH;
				goto out;
			}

			state = STATE_OBJECT_VALUE_ADD;
			p->depth++;
			uc_json_level_reset(p, p->depth);
			goto redo_char;

		case STATE_OBJECT_VALUE_ADD:
			ucv_object_insert_string(current, field_key, obj);
			obj = NULL;
			ucv_put(field_key);
			field_key = NULL;
			saved_state = STATE_OBJECT_SEP;
			state = STATE_EATWS;
			goto redo_char;

		case STATE_OBJECT_SEP:
			if (c == '}') {
				saved_state = STATE_FINISH;
				state = STATE_EATWS;
			}
			else if (c == ',') {
				saved_state = STATE_OBJECT_FIELD_START_AFTER_SEP;
				state = STATE_EATWS;
			}
			else {
				p->err = UC_JSON_ERROR_VALUE_SEP;
				goto out;
			}

			break;
		}

		(void)ADVANCE_CHAR();

		if (!c)
			break;
	}

out:
	/* a NUL byte ends the input */
	if (!c && state != STATE_FINISH && saved_state != STATE_FINISH)
		p->err = UC_JSON_ERROR_EOF;

	ucv_put(obj);

	if (p->err != UC_JSON_SUCCESS)
		return NULL;

	res = current;
	current = NULL;
	uc_json_reset(p);

	return res;
}

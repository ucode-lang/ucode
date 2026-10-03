/*
 * Copyright (C) 2021 Jo-Philipp Wich <jo@mein.io>
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

#include <errno.h>
#include <ctype.h>

#include "ucode/internal/source.h"
#include "ucode/internal/platform.h"


uc_source_t *
uc_source_new_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	uc_source_t *src;

	if (!fp)
		return NULL;

	src = xalloc(ALIGN(sizeof(*src)) + strlen(path) + 1);

	src->header.type = UC_SOURCE;
	src->header.refcount = 1;

	src->fp = fp;
	src->buffer = NULL;
	src->filename = strcpy((char *)src + ALIGN(sizeof(*src)), path);
	src->runpath = src->filename;

	src->lineinfo.count = 0;
	src->lineinfo.entries = NULL;

	return src;
}

uc_source_t *
uc_source_new_buffer(const char *name, char *buf, size_t len)
{
	FILE *fp = fmemopen(buf, len, "rb");
	uc_source_t *src;

	if (!fp)
		return NULL;

	src = xalloc(ALIGN(sizeof(*src)) + strlen(name) + 1);

	src->header.type = UC_SOURCE;
	src->header.refcount = 1;

	src->fp = fp;
	src->buffer = buf;
	src->filename = strcpy((char *)src + ALIGN(sizeof(*src)), name);
	src->runpath = src->filename;

	src->lineinfo.count = 0;
	src->lineinfo.entries = NULL;

	return src;
}

uc_source_t *
uc_source_get(uc_source_t *source)
{
	return (uc_source_t *)ucv_get(source ? &source->header : NULL);
}

void
uc_source_put(uc_source_t *source)
{
	ucv_put(source ? &source->header : NULL);
}

size_t
uc_source_get_line(uc_source_t *source, size_t *offset)
{
	uc_lineinfo_t *lines = &source->lineinfo;
	size_t i, pos = 0, line = 1, lastoff = 0;

	for (i = 0; i <= lines->count; i++) {
		if (pos >= *offset || i == lines->count) {
			*offset = (*offset - lastoff) + 1;

			return line;
		}

		/* don't count first line jump as actual byte */
		if (i > 0 && (lines->entries[i] & 0x80)) {
			line++;
			pos++;
			lastoff = pos;
		}

		pos += (lines->entries[i] & 0x7f);
	}

	return 0;
}

uc_source_type_t
uc_source_type_test(uc_source_t *source)
{
	union { char s[sizeof(uint32_t)]; uint32_t n; } buf = { 0 };
	uc_source_type_t type = UC_SOURCE_TYPE_PLAIN;
	FILE *fp = source->fp;
	size_t rlen;
	int c = 0;

	if (fread(buf.s, 1, 2, fp) == 2 && !strncmp(buf.s, "#!", 2)) {
		source->off += 2;

		while ((c = fgetc(fp)) != EOF) {
			source->off++;

			if (c == '\n')
				break;
		}
	}
	else {
		if (fseek(fp, 0L, SEEK_SET) == -1)
			fprintf(stderr, "Failed to rewind source buffer: %s\n", strerror(errno));
	}

	rlen = fread(buf.s, 1, 4, fp);

	if (rlen == 4 && buf.n == htobe32(UC_PRECOMPILED_BYTECODE_MAGIC)) {
		type = UC_SOURCE_TYPE_PRECOMPILED;
	}
	else {
		if (c == '\n') {
			uc_source_line_update(source, source->off - 1);
			uc_source_line_next(source);
		}
		else {
			uc_source_line_update(source, source->off);
		}
	}

	if (fseek(fp, -(long)rlen, SEEK_CUR) == -1)
		fprintf(stderr, "Failed to rewind source buffer: %s\n", strerror(errno));

	return type;
}

/* lineinfo is encoded in bytes: the most significant bit specifies whether
 * to advance the line count by one or not, while the remaining 7 bits encode
 * the amounts of bytes on the current line.
 *
 * If a line has more than 127 characters, the first byte will be set to
 * 0xff (1 1111111) and subsequent bytes will encode the remaining characters
 * in bits 1..7 while setting bit 8 to 0. A line with 400 characters will thus
 * be encoded as 0xff 0x7f 0x7f 0x13 (1:1111111 + 0:1111111 + 0:1111111 + 0:1111111).
 *
 * The newline character itself is not counted, so an empty line is encoded as
 * 0x80 (1:0000000).
 */

void
uc_source_line_next(uc_source_t *source)
{
	uc_vector_push(&source->lineinfo, 0x80);
}

void
uc_source_line_update(uc_source_t *source, size_t off)
{
	uc_lineinfo_t *lines = &source->lineinfo;
	uint8_t *entry, n;

	if (!lines->count)
		uc_source_line_next(source);

	entry = uc_vector_last(lines);

	if ((entry[0] & 0x7f) + off <= 0x7f) {
		entry[0] += off;
	}
	else {
		off -= (0x7f - (entry[0] & 0x7f));
		entry[0] |= 0x7f;

		while (off > 0) {
			n = (off > 0x7f) ? 0x7f : off;
			uc_vector_push(lines, n);
			off -= n;
		}
	}
}

void
uc_source_runpath_set(uc_source_t *source, const char *runpath)
{
	if (source->runpath != source->filename)
		free(source->runpath);

	source->runpath = runpath ? xstrdup(runpath) : NULL;
}

bool
uc_source_export_add(uc_source_t *source, uc_value_t *name)
{
	ssize_t idx = uc_source_export_lookup(source, name);

	if (idx > -1)
		return false;

	uc_vector_push(&source->exports, ucv_get(name));

	return true;
}

ssize_t
uc_source_export_lookup(uc_source_t *source, uc_value_t *name)
{
	size_t i;

	for (i = 0; i < source->exports.count; i++)
		if (ucv_is_equal(source->exports.entries[i], name))
			return i;

	return -1;
}


/* Expressions longer than this are cut short, and only their tails, which
 * contain the name actually being invoked, are shown. */
#define UC_EXPR_MAXLEN 64

/* The number of characters which may still be read while recovering an
 * expression. Since only the source of the arguments passed to the failing call
 * can be arbitrarily wide, this bounds the effort spent on an error message. */
#define UC_EXPR_MAXREAD 4096

/* The text of an invoked expression is recovered into a buffer which is filled
 * from its end towards its beginning, so that the characters end up in the
 * order they have in the source without a second pass. */
static char uc_expr_text[UC_EXPR_MAXLEN + 1];
static size_t uc_expr_pos, uc_expr_read;

/* Returns true if `ch` closes a string or template literal. */
static bool
uc_expr_quote(int ch)
{
	return ch == '\'' || ch == '"' || ch == '`';
}

/* Returns true if `ch` is a member access operator, all of which may be
 * surrounded by whitespace without ceasing to belong to the same expression. */
static bool
uc_expr_member_char(int ch)
{
	return ch == '.' || ch == '[' || ch == '?';
}

/* Returns true if `ch` may appear in the spine of a member access or call
 * expression, that is outside of an argument list, subscript or literal value.
 * The characters accepted here are those of identifiers, as recognised by the
 * lexer, plus the member access operators. Notably `:` is absent, so that
 * `foo ? bar : baz()` identifies `baz`. */
static bool
uc_expr_spine_char(int ch)
{
	return isalnum(ch) || ch == '_' || ch == '.' || ch == '?';
}

/* Reads the character at `off`, returning EOF once the read budget is used up.
 * The caller keeps track of the position, so that it stays in a register. */
static int
uc_expr_at(FILE *fp, size_t off)
{
	if (uc_expr_read == 0)
		return EOF;

	uc_expr_read--;

	fseek(fp, (long)off, SEEK_SET);

	return fgetc(fp);
}

/* Puts `ch` in front of the characters collected so far. Characters which no
 * longer fit are dropped, since only the tail of an over-long expression is of
 * interest. */
static inline void
uc_expr_collect(int ch)
{
	if (uc_expr_pos > 0)
		uc_expr_text[--uc_expr_pos] = ch;
}

/* Returns true if the literal quote at `off` is the opening one, which it is
 * when it is preceded by an even number of backslashes, since each of those
 * escapes the next character. */
static bool
uc_expr_quote_open(FILE *fp, size_t off)
{
	size_t n = 0;

	while (off-- > 0 && uc_expr_at(fp, off) == '\\')
		n++;

	return n % 2 == 0;
}

/* Returns the offset of the quote opening the literal which closes at `off`, or
 * SIZE_MAX if there is none. The characters passed over are collected along the
 * way; they are of no interest to the caller only in the latter case, in which
 * the scan is abandoned altogether. */
static size_t
uc_expr_back_literal(FILE *fp, int quote, size_t off)
{
	int ch;

	while (off-- > 0) {
		if ((ch = uc_expr_at(fp, off)) == EOF)
			return SIZE_MAX;

		uc_expr_collect(ch);

		if (ch == quote && uc_expr_quote_open(fp, off))
			return off;
	}

	return SIZE_MAX;
}

/* Returns the offset of the delimiter opening the balanced pair whose closing
 * delimiter is at `off`, or SIZE_MAX if the pair is not balanced within the
 * source. Literal values are skipped over as a whole, so that a bracket inside
 * one of them cannot be mistaken for the opening delimiter. */
static size_t
uc_expr_back_pair(FILE *fp, size_t off, int open)
{
	size_t depth = 0, beg;
	int ch;

	while (off-- > 0) {
		ch = uc_expr_at(fp, off);

		if (ch == EOF)
			return SIZE_MAX;

		uc_expr_collect(ch);

		if (uc_expr_quote(ch)) {
			if ((beg = uc_expr_back_literal(fp, ch, off)) == SIZE_MAX)
				return SIZE_MAX;

			off = beg;

			continue;
		}

		switch (ch) {
		case ')': case ']': case '}':
			depth++;
			break;

		case '(': case '[': case '{':
			if (depth == 0)
				return ch == open ? off : SIZE_MAX;

			depth--;
			break;
		}
	}

	return SIZE_MAX;
}

/* Extracts the expression invoked by the call whose closing parenthesis is at
 * source offset `end`, for use in error messages about a failed call.
 *
 * The source in front of `end` is scanned backwards over the balanced pair
 * ending there, which delimits the arguments passed to the failing call and
 * whose opening parenthesis therefore marks the end of the invoked expression.
 * The scan then continues over the spine of that expression, taking subscripts,
 * nested calls and literal values as a whole so that their contents cannot be
 * mistaken for part of the surrounding expression, and dropping whitespace
 * which does not surround a member access operator, since such whitespace
 * separates the expression from whatever precedes it.
 *
 * Returns the recovered expression, which stays valid until the next call, or
 * NULL if it could not be determined. */
__hidden const char *
uc_source_callee_expr(uc_source_t *source, size_t end)
{
	const char *res = NULL;
	size_t i, beg, pos;
	int last = 0;
	long save;
	FILE *fp;

	if (!source || !source->fp || end == 0)
		return NULL;

	fp = source->fp;
	save = ftell(fp);

	uc_expr_pos = UC_EXPR_MAXLEN;
	uc_expr_read = UC_EXPR_MAXREAD;

	/* the instruction of a failing call refers to the closing parenthesis of
	 * the arguments passed to it */
	if (uc_expr_at(fp, end) != ')')
		goto out;

	/* the arguments themselves are of no interest, so whatever the scan
	 * collected while looking for the parenthesis opening them is dropped */
	pos = uc_expr_pos;

	if ((beg = uc_expr_back_pair(fp, end, '(')) == SIZE_MAX || beg == 0)
		goto out;

	uc_expr_pos = pos;

	i = beg;

	/* once the buffer is full, the expression is known to be incomplete, and
	 * whatever comes next cannot make a difference anymore */
	while (i-- > 0 && uc_expr_pos > 0) {
		int ch = uc_expr_at(fp, i);

		if (ch == EOF)
			break;

		/* literal values, subscripts and nested calls are taken as a whole, so
		 * that their contents cannot be mistaken for part of the expression */
		if (uc_expr_quote(ch) || ch == ')' || ch == ']' || ch == '}') {
			bool quote = uc_expr_quote(ch);
			/* the opener matching a closer sits one below it, except for the
			 * square and curly braces, which sit two below */
			int open = quote ? ch : ch - 1 - (ch > ')');

			uc_expr_collect(ch);

			beg = quote
				? uc_expr_back_literal(fp, ch, i)
				: uc_expr_back_pair(fp, i, open);

			if (beg == SIZE_MAX)
				goto out;

			i = beg;
			last = open;

			continue;
		}

		if (isspace((unsigned char)ch)) {
			while (i > 0 && (ch = uc_expr_at(fp, --i)) != EOF
			       && isspace((unsigned char)ch))
				;

			if (ch == EOF)
				break;

			/* whitespace surrounding a member access operator is merely
			 * formatting and thus belongs to the expression, as is the
			 * whitespace in front of the arguments, which is skipped before any
			 * of the expression has been collected; everywhere else it ends
			 * the expression */
			if (!uc_expr_member_char(last)
			    && uc_expr_pos != UC_EXPR_MAXLEN
			    && !uc_expr_member_char(ch))
				break;
		}

		if (!uc_expr_spine_char((unsigned char)ch))
			break;

		uc_expr_collect(ch);

		last = ch;
	}

	if (uc_expr_pos == UC_EXPR_MAXLEN)
		goto out;

	/* the expression is longer than the buffer, so only its tail is known, and
	 * the dots overwrite some of the characters which were collected */
	if (uc_expr_pos == 0)
		uc_expr_text[0] = uc_expr_text[1] = uc_expr_text[2] = '.';
	/* an expression starting with a member access operator is missing its
	 * left-hand side, so the scan could not be completed */
	else if (uc_expr_member_char(uc_expr_text[uc_expr_pos]))
		goto out;

	uc_expr_text[UC_EXPR_MAXLEN] = 0;

	res = uc_expr_text + uc_expr_pos;

out:
	if (save >= 0)
		fseek(fp, save, SEEK_SET);

	return res;
}

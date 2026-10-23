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


static uc_source_t *
uc_source_alloc(const char *name)
{
	uc_source_t *src = xalloc(ALIGN(sizeof(*src)) + strlen(name) + 1);

	src->header.type = UC_SOURCE;
	src->header.refcount = 1;

	src->filename = strcpy((char *)src + ALIGN(sizeof(*src)), name);
	src->runpath = src->filename;

	return src;
}

uc_source_t *
uc_source_new_slurp(const char *name, FILE *fp)
{
	size_t rlen = 0, tlen = 0;
	char buf[128], *p = NULL;

	while (true) {
		rlen = fread(buf, 1, sizeof(buf), fp);

		if (rlen == 0)
			break;

		/* uc_program_write() stores the buffer as a string */
		p = xrealloc(p, tlen + rlen + 1);
		memcpy(p + tlen, buf, rlen);
		tlen += rlen;
		p[tlen] = 0;
	}

	/* On empty input, provide a dummy buffer and ensure that it is
	 * at least one byte long, due to
	 * https://github.com/google/sanitizers/issues/627 */
	if (p == NULL) {
		p = xstrdup("\n");
		tlen = 1;
	}

	return uc_source_new_buffer(name, p, tlen);
}

uc_source_t *
uc_source_new_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	uc_source_t *src;

	if (!fp)
		return NULL;

	/* the type test seeks in the file, and the content of a pipe cannot be
	 * read again for an error message */
	if (fseek(fp, 0L, SEEK_SET)) {
		src = uc_source_new_slurp(path, fp);
		fclose(fp);

		return src;
	}

	src = uc_source_alloc(path);
	src->fp = fp;
	src->path = realpath(path, NULL);

	return src;
}

uc_source_t *
uc_source_new_buffer(const char *name, char *buf, size_t len)
{
	FILE *fp = fmemopen(buf, len, "rb");
	uc_source_t *src;

	if (!fp)
		return NULL;

	src = uc_source_alloc(name);
	src->fp = fp;
	src->buffer = buf;

	return src;
}

uc_source_t *
uc_source_new_path(const char *path)
{
	uc_source_t *src = uc_source_alloc(path);

	src->path = realpath(path, NULL);

	return src;
}

/* Number of source bytes lent to the lexer per block read. */
#define UC_SOURCE_BLOCKSIZE 128

/* The file is opened again after the working directory can have changed, so
 * a source keeps the absolute path that was valid when it was created. */
static FILE *
uc_source_file_open(uc_source_t *source)
{
	return fopen(source->path ? source->path : source->filename, "rb");
}

/* Make sure the stream is open. A source holds its file open only while it is
 * being read, so that the number of live programs does not count against the
 * limit on open files. */
static bool
uc_source_stream_ensure(uc_source_t *source)
{
	if (!source->fp)
		source->fp = uc_source_file_open(source);

	return source->fp != NULL;
}

/* The end of the text was reached, or reading it failed. Buffer backed
 * sources have nothing to close, but they are done all the same. */
static void
uc_source_stream_finish(uc_source_t *source)
{
	source->done = true;

	if (source->buffer || !source->fp)
		return;

	fclose(source->fp);
	source->fp = NULL;
}

void
uc_source_reset(uc_source_t *source)
{
	free(source->rbuf);

	source->rbuf = NULL;
	source->off = 0;
	source->done = false;
	source->sized = false;
	source->moved = false;
	source->ahead_len = 0;

	if (source->buffer) {
		fseek(source->fp, 0, SEEK_SET);
	}
	else if (source->fp) {
		fclose(source->fp);
		source->fp = NULL;
	}
}

/* Number of source bytes kept for looking ahead of the lexer. */
#define UC_SOURCE_AHEAD 16384

/* Read text at an arbitrary offset, e.g. to look ahead of the lexer. Returns
 * the number of bytes read, which can be less than requested. Looking ahead is
 * done in a cache which is refilled on demand, so that doing it frequently for
 * nearby text does not come at the price of repositioning the stream each time.
 * Call uc_source_resync() afterwards. */
size_t
uc_source_read_at(uc_source_t *source, size_t off, void *buf, size_t len)
{
	size_t n;

	if (off < source->ahead_off || off >= source->ahead_off + source->ahead_len) {
		if (!uc_source_stream_ensure(source) || fseek(source->fp, off, SEEK_SET) != 0)
			return 0;

		if (!source->ahead)
			source->ahead = xalloc(UC_SOURCE_AHEAD);

		source->moved = true;
		source->ahead_off = off;
		source->ahead_len = fread(source->ahead, 1, UC_SOURCE_AHEAD, source->fp);

		if (source->ahead_len == 0)
			return 0;
	}

	n = source->ahead_off + source->ahead_len - off;

	if (n > len)
		n = len;

	memcpy(buf, source->ahead + (off - source->ahead_off), n);

	return n;
}

/* Continue sequential reading where it has been left off. */
void
uc_source_resync(uc_source_t *source)
{
	if (source->moved && source->fp && !source->done)
		fseek(source->fp, source->off, SEEK_SET);

	source->moved = false;
}

const uint8_t *
uc_source_read_block(uc_source_t *source, size_t *len, size_t *off)
{
	size_t rlen;

	*len = 0;

	if (source->done || !uc_source_stream_ensure(source))
		return NULL;

	if (!source->rbuf)
		source->rbuf = xalloc(UC_SOURCE_BLOCKSIZE);

	rlen = fread(source->rbuf, 1, UC_SOURCE_BLOCKSIZE, source->fp);

	if (off)
		*off = source->off;

	source->off += rlen;

	if (rlen < UC_SOURCE_BLOCKSIZE || feof(source->fp) || ferror(source->fp))
		uc_source_stream_finish(source);

	*len = rlen;

	return rlen ? source->rbuf : NULL;
}

size_t
uc_source_read(uc_source_t *source, void *buf, size_t len)
{
	size_t rlen;

	errno = 0;

	if (!len || source->done || !uc_source_stream_ensure(source))
		return 0;

	rlen = fread(buf, 1, len, source->fp);
	source->off += rlen;

	/* a short read leaves errno set if the read failed, and clear if the text
	 * ended prematurely */
	if (rlen < len || uc_source_remaining(source) == 0)
		uc_source_stream_finish(source);

	return rlen;
}

size_t
uc_source_offset(uc_source_t *source)
{
	return source->off;
}

size_t
uc_source_remaining(uc_source_t *source)
{
	FILE *fp;
	long pos, end;

	if (!source->sized) {
		fp = source->fp ? source->fp : uc_source_file_open(source);

		if (!fp)
			return 0;

		/* the size of text that cannot seek is not known, treat it as
		 * unbounded */
		source->size = SIZE_MAX;
		source->sized = true;

		pos = ftell(fp);

		if (pos >= 0 && fseek(fp, 0, SEEK_END) == 0) {
			end = ftell(fp);

			if (end >= 0)
				source->size = end;

			fseek(fp, pos, SEEK_SET);
		}

		if (fp != source->fp)
			fclose(fp);
	}

	return (source->size > source->off) ? source->size - source->off : 0;
}

/* Random access to the text of a source for error messages. The text of a
 * closed source is read from its path, which can be gone or differ from the
 * file that was compiled. A file that was open is left positioned as found,
 * one that was not is closed again. A path that names a pipe or a terminal
 * by then has no text to show, and reading it would block. */
static FILE *
uc_source_text_open(uc_source_t *source, bool *temp, long *pos)
{
	FILE *fp = source->fp ? source->fp : uc_source_file_open(source);

	*temp = (fp != source->fp);

	if (!fp)
		return NULL;

	*pos = ftell(fp);

	if (*pos != -1)
		return fp;

	if (*temp)
		fclose(fp);

	return NULL;
}

static void
uc_source_text_close(FILE *fp, bool temp, long pos)
{
	if (temp)
		fclose(fp);
	else
		fseek(fp, pos, SEEK_SET);
}

size_t
uc_source_read_line(uc_source_t *source, size_t off, char *line, size_t size,
                    size_t *lpos, bool *truncated)
{
	size_t len, rlen = 0, eline = 1;
	bool temp, cont = false;
	long pos;
	FILE *fp;

	*truncated = false;

	if (size < 2 || !(fp = uc_source_text_open(source, &temp, &pos)))
		return 0;

	fseek(fp, 0, SEEK_SET);

	while (fgets(line, size, fp)) {
		len = strlen(line);
		rlen += len;

		if (rlen >= off) {
			*truncated = cont;
			*lpos = rlen - len;

			uc_source_text_close(fp, temp, pos);

			return eline;
		}

		/* a chunk without a newline is part of a line longer than the buffer,
		 * its remainder is delivered by the next one and does not advance the
		 * line count */
		cont = (len > 0 && line[len - 1] != '\n');
		eline += !cont;
	}

	uc_source_text_close(fp, temp, pos);

	return 0;
}

char *
uc_source_read_content(uc_source_t *source, size_t *len)
{
	size_t rlen, tlen = 0;
	char *text = NULL;
	bool temp;
	long pos;
	FILE *fp;

	if (!(fp = uc_source_text_open(source, &temp, &pos)))
		return NULL;

	fseek(fp, 0, SEEK_SET);

	do {
		text = xrealloc(text, tlen + 4096);
		rlen = fread(text + tlen, 1, 4096, fp);
		tlen += rlen;
	} while (rlen == 4096);

	uc_source_text_close(fp, temp, pos);

	*len = tlen;

	return text;
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

static void
uc_source_state_clear(uc_source_t *source)
{
	size_t i;

	for (i = 0; i < source->exports.count; i++)
		ucv_put(source->exports.entries[i]);

	uc_vector_clear(&source->exports);
	uc_vector_clear(&source->lineinfo);
}

void
uc_source_free(uc_source_t *source)
{
	if (source->runpath != source->filename)
		free(source->runpath);

	uc_source_state_clear(source);

	uc_source_reset(source);

	if (source->buffer)
		fclose(source->fp);

	free(source->ahead);
	free(source->buffer);
	free(source->path);
}

uc_source_type_t
uc_source_begin(uc_source_t *source)
{
	union { char s[sizeof(uint32_t)]; uint32_t n; } buf = { 0 };
	uc_source_type_t type = UC_SOURCE_TYPE_PLAIN;
	size_t rlen;
	FILE *fp;
	int c = 0;

	/* A compilation records the line information and the export table in the
	 * source, and a source can outlive its compilation. Drop that state, and
	 * start over from the beginning of the text. */
	uc_source_reset(source);
	uc_source_state_clear(source);

	source->exports.offset = 0;

	if (!uc_source_stream_ensure(source))
		return UC_SOURCE_TYPE_UNAVAILABLE;

	fp = source->fp;

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

	/* leave the stream on the first byte of code */
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

/* The number of source bytes, ending with the closing parenthesis of the call,
 * which are looked at while recovering an expression. Only the source of the
 * arguments passed to the failing call can be arbitrarily wide, so the window
 * starts out small and is doubled while the scan runs into its start, up to a
 * limit which bounds the effort spent on an error message. */
#define UC_EXPR_WINDOW_MIN 256
#define UC_EXPR_WINDOW_MAX 4096

/* The state of one recovery, which lives on the stack of expr_recover().
 *
 * The window of source text being scanned is `len` bytes of the allocated
 * buffer `win` starting at the source offset `base`. `cut` notes that the scan
 * wanted to look in front of a window which does not start at the beginning of
 * the file. */
typedef struct {
	char *win;
	size_t base, len;
	bool cut;
} expr_t;

/* Returns true if `ch` closes a string or template literal. */
static bool
expr_quote(int ch)
{
	return ch == '\'' || ch == '"' || ch == '`';
}

/* Returns true if `ch` is a member access operator, all of which may be
 * surrounded by whitespace without ceasing to belong to the same expression. */
static bool
expr_member_char(int ch)
{
	return ch == '.' || ch == '[' || ch == '?';
}

/* Returns true if `ch` may appear in the spine of a member access or call
 * expression, that is outside of an argument list, subscript or literal value.
 * The characters accepted here are those of identifiers, as recognised by the
 * lexer, plus the member access operators. Notably `:` is absent, so that
 * `foo ? bar : baz()` identifies `baz`. */
static bool
expr_spine_char(int ch)
{
	return isalnum(ch) || ch == '_' || ch == '.' || ch == '?';
}

/* Returns the character at source offset `off`, or EOF if it is not within the
 * window. */
static int
expr_at(expr_t *x, size_t off)
{
	if (off < x->base) {
		x->cut |= (x->base > 0);

		return EOF;
	}

	if (off - x->base >= x->len)
		return EOF;

	return (unsigned char)x->win[off - x->base];
}

/* Returns true if the literal quote at `off` is the opening one, which it is
 * when it is preceded by an even number of backslashes, since each of those
 * escapes the next character. */
static bool
expr_quote_open(expr_t *x, size_t off)
{
	size_t n = 0;

	while (off-- > 0 && expr_at(x, off) == '\\')
		n++;

	return n % 2 == 0;
}

/* Returns the offset of the quote opening the literal which closes at `off`, or
 * SIZE_MAX if there is none. */
static size_t
expr_back_literal(expr_t *x, int quote, size_t off)
{
	int ch;

	while (off-- > 0) {
		if ((ch = expr_at(x, off)) == EOF)
			return SIZE_MAX;

		if (ch == quote && expr_quote_open(x, off))
			return off;
	}

	return SIZE_MAX;
}

/* Returns the offset of the delimiter opening the balanced pair whose closing
 * delimiter is at `off`, or SIZE_MAX if the pair is not balanced within the
 * source. Literal values are skipped over as a whole, so that a bracket inside
 * one of them cannot be mistaken for the opening delimiter. */
static size_t
expr_back_pair(expr_t *x, size_t off, int open)
{
	size_t depth = 0, beg;
	int ch;

	while (off-- > 0) {
		ch = expr_at(x, off);

		if (ch == EOF)
			return SIZE_MAX;

		if (expr_quote(ch)) {
			if ((beg = expr_back_literal(x, ch, off)) == SIZE_MAX)
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

/* Returns the offset of the first byte of the expression invoked by the call
 * whose closing parenthesis is at `end`, or SIZE_MAX if it cannot be
 * determined; `*open` is set to the offset of the parenthesis opening the
 * arguments, which is where the expression ends.
 *
 * The source in front of `end` is scanned backwards over the balanced pair
 * ending there, which delimits the arguments passed to the failing call. The
 * scan then continues over the spine of the invoked expression, taking
 * subscripts, nested calls and literal values as a whole so that their contents
 * cannot be mistaken for part of the surrounding expression, and passing over
 * whitespace which surrounds a member access operator, while any other
 * whitespace separates the expression from whatever precedes it. */
static size_t
expr_start(expr_t *x, size_t end, size_t *open)
{
	size_t i, beg, start;
	int last = 0;

	/* the instruction of a failing call refers to the closing parenthesis of
	 * the arguments passed to it */
	if (expr_at(x, end) != ')')
		return SIZE_MAX;

	if ((beg = expr_back_pair(x, end, '(')) == SIZE_MAX || beg == 0)
		return SIZE_MAX;

	*open = start = i = beg;

	while (i-- > 0) {
		int ch = expr_at(x, i);

		if (ch == EOF)
			break;

		if (isspace((unsigned char)ch)) {
			while (i > 0 && (ch = expr_at(x, --i)) != EOF
			       && isspace((unsigned char)ch))
				;

			if (ch == EOF)
				break;

			/* whitespace surrounding a member access operator is merely
			 * formatting and thus belongs to the expression, as is the
			 * whitespace in front of the arguments, which is skipped before any
			 * of the expression has been seen; everywhere else it ends the
			 * expression */
			if (!expr_member_char(last) && start != *open &&
			    !expr_member_char(ch))
				break;
		}

		if (expr_quote(ch) || ch == ')' || ch == ']' || ch == '}') {
			bool quote = expr_quote(ch);
			/* the opener matching a closer sits one below it, except for the
			 * square and curly braces, which sit two below */
			int opener = quote ? ch : ch - 1 - (ch > ')');

			beg = quote
				? expr_back_literal(x, ch, i)
				: expr_back_pair(x, i, opener);

			if (beg == SIZE_MAX)
				return SIZE_MAX;

			i = start = beg;
			last = opener;

			continue;
		}

		if (!expr_spine_char(ch))
			break;

		start = i;
		last = ch;
	}

	/* nothing but whitespace in front of the arguments, or the scan ran into
	 * the start of the window while the expression was still being followed */
	if (start == *open || x->cut)
		return SIZE_MAX;

	return start;
}

/* Rewrites the source text in `s` up to `e` into `d`, which does not lie behind
 * `s`, leaving out the whitespace of the expression spine. Whitespace within
 * subscripts, arguments and literal values is kept as it is. Returns the end of
 * the output. */
static char *
expr_format(char *d, const char *s, const char *e)
{
	size_t depth = 0;
	int quote;

	while (s < e) {
		if (expr_quote(*s)) {
			quote = *s;

			for (*d++ = *s++; s < e && *s != quote; ) {
				if (*s == '\\' && s + 1 < e)
					*d++ = *s++;

				*d++ = *s++;
			}

			if (s < e)
				*d++ = *s++;

			continue;
		}

		switch (*s) {
		case ' ': case '\t': case '\n': case '\v': case '\f': case '\r':
			if (depth == 0) {
				s++;

				continue;
			}

			break;

		case '(': case '[': case '{':
			depth++;
			break;

		case ')': case ']': case '}':
			depth -= (depth > 0);
			break;
		}

		*d++ = *s++;
	}

	return d;
}

/* Recovers the expression invoked by the call whose closing parenthesis is at
 * source offset `end` from the `window` bytes of source ending there, see
 * uc_source_callee_expr(). `*cut` is set if the scan wanted to look in front of
 * the window, which makes a larger window worth a try if nothing was found. */
static char *
expr_recover(uc_source_t *source, size_t end, size_t window, bool *cut)
{
	size_t start, open = 0, len;
	expr_t expr, *x = &expr;
	char *tail;
	bool temp;
	long save;
	FILE *fp;

	*cut = false;

	/* read the bytes in front of the closing parenthesis, and the parenthesis
	 * itself, in one go and scan them in memory */
	x->base = (end >= window) ? end + 1 - window : 0;
	x->len = end + 1 - x->base;
	x->cut = false;
	x->win = xrealloc(NULL, x->len);

	fp = uc_source_text_open(source, &temp, &save);

	if (!fp) {
		free(x->win);

		return NULL;
	}

	if (fseek(fp, x->base, SEEK_SET) != 0 ||
	    fread(x->win, 1, x->len, fp) != x->len) {
		uc_source_text_close(fp, temp, save);
		free(x->win);

		return NULL;
	}

	uc_source_text_close(fp, temp, save);

	start = expr_start(x, end, &open);
	*cut = x->cut;

	if (start == SIZE_MAX) {
		free(x->win);

		return NULL;
	}

	/* the text is rewritten in place, and ends up at the start of the window */
	tail = expr_format(x->win, x->win + (start - x->base), x->win + (open - x->base));
	len = tail - x->win;

	if (len > UC_EXPR_MAXLEN) {
		/* the expression is longer than the limit, so only its tail is known,
		 * and the dots replace some of its characters */
		memmove(x->win, tail - UC_EXPR_MAXLEN, UC_EXPR_MAXLEN);
		memcpy(x->win, "...", 3);

		len = UC_EXPR_MAXLEN;
	}
	/* an expression starting with a member access operator is missing its
	 * left-hand side, so the scan could not be completed */
	else if (len == 0 || expr_member_char((unsigned char)x->win[0])) {
		free(x->win);

		return NULL;
	}

	x->win[len] = 0;

	return xrealloc(x->win, len + 1);
}

/* Extracts the expression invoked by the call whose closing parenthesis is at
 * source offset `end`, for use in error messages about a failed call.
 *
 * The window of source text in front of `end` starts out small, and is doubled
 * while the scan of the expression runs into its start.
 *
 * Returns the recovered expression, which the caller releases using free(), or
 * NULL if it could not be determined. */
__hidden char *
uc_source_callee_expr(uc_source_t *source, size_t end)
{
	size_t window = UC_EXPR_WINDOW_MIN;
	bool cut;
	char *res;

	if (!source || end == 0)
		return NULL;

	while (true) {
		res = expr_recover(source, end, window, &cut);

		if (res || !cut || window >= UC_EXPR_WINDOW_MAX)
			return res;

		window *= 2;
	}
}

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
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <regex.h>

#include "debug_highlight.h"

/* -- styling, ported verbatim from the pre-protocol lib/debug.c ---------- */

enum {
	BOLD  = (1 << 0),
	FAINT = (1 << 1),
	ULINE = (1 << 2),
};

typedef enum {
	FG_NONE    =   0,
	FG_BLACK   =  30,
	FG_RED     =  31,
	FG_GREEN   =  32,
	FG_YELLOW  =  33,
	FG_BLUE    =  34,
	FG_MAGENTA =  35,
	FG_CYAN    =  36,
	FG_GRAY    =  37,
	FG_BBLACK  =  90,
	FG_BRED    =  91,
	FG_BGREEN  =  92,
	FG_BYELLOW =  93,
	FG_BBLUE   =  94,
	FG_BMAGENT =  95,
	FG_BCYAN   =  96,
	FG_BWHITE  =  97,
} fg_color_t;

typedef enum {
	BG_NONE  =   0,
	BG_BLACK =  40,
	BG_GRAY  = 100,
} bg_color_t;

typedef struct {
	fg_color_t fg;
	bg_color_t bg;
	unsigned int styles;
} style_t;

static void
cs(FILE *out, const style_t *style)
{
	int codes[8] = { 0 };
	size_t i = 0;

	if (style == NULL) {
		fputs("\033[0m", out);
		return;
	}

	if ((style->styles & (BOLD | FAINT | ULINE)) == 0)
		codes[i++] = 0;

	if (style->styles & BOLD)  codes[i++] = 1;
	if (style->styles & FAINT) codes[i++] = 2;
	if (style->styles & ULINE) codes[i++] = 4;

	codes[i++] = style->fg ? style->fg : 39;
	codes[i++] = style->bg ? style->bg : 49;

	fputs("\033[", out);

	for (size_t n = 0; n < i; n++)
		fprintf(out, "%s%d", n ? ";" : "", codes[n]);

	fputc('m', out);
}

/* -- syntax highlighting rules, ported verbatim -------------------------- */

static struct {
	fg_color_t color;
	const char *start, *end;
} highlight_rules[] = {
	{ FG_GRAY, "^#!.*", NULL },

	/* declarations */
	{ FG_GREEN, "\\<(let|const|function|this)\\>", NULL },

	/* arrow functions */
	{ FG_GREEN, "(\\<\\w+\\>|\\([[:alnum:][:space:]_,.]*\\))[[:space:]]*=>", NULL },

	/* flow control */
	{ FG_BYELLOW, "\\<(while|if|else|elif|switch|case|default|for|in|endif|endfor|endwhile|endfunction)\\>", NULL },

	/* keywords */
	{ FG_BYELLOW, "\\<(export|import|try|catch|delete|typeof)\\>", NULL },

	/* exit points */
	{ FG_MAGENTA, "\\<(break|continue|return|throw)\\>", NULL },

	/* numeric literals */
	{ FG_CYAN, "\\<([0-9]+\\.[0-9]+([eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+)\\>", NULL },
	{ FG_CYAN, "\\<0[xX][[:xdigit:]]+(\\.[[:xdigit:]]+)?\\>", NULL },
	{ FG_CYAN, "\\<(0[oO][0-7]+|0[bB][01]+|[0-9]+)\\>", NULL },

	/* special values */
	{ FG_CYAN, "\\<(true|false|null|NaN|Infinity)\\>", NULL },

	/* strings */
	{ FG_BMAGENT, "\"([^\"\\{%#}]|\\\\.|\\{[^\"\\{%#]|[%#}][^\"\\}]|[{%#}]\\\\.)*[{%#}]?\"", NULL },
	{ FG_BMAGENT, "'([^'\\{%#}]|\\\\.|\\{[^'\\{%#]|[%#}][^'\\}]|[{%#}]\\\\.)*[{%#}]?'", NULL },
	{ FG_BMAGENT, "`([^`\\{%#}]|\\\\.|\\{[^`\\{%#]|[%#}][^`\\}]|[{%#}]\\\\.)*[{%#}]?`", NULL },

	/* template string expressions */
	{ FG_BWHITE, "\\$\\{", "}" },

	/* comments */
	{ FG_BBLUE, "(^|[[:blank:]])//.*", NULL },
	{ FG_BBLUE, "(^|[[:space:]])/\\*", "\\*/" },
	{ FG_BBLUE, "\\{#", "#\\}" },

	/* text outside template directives */
	{ FG_GRAY, "[}%#]\\}", "\\{[{%#]" },
	{ FG_GRAY, "^#!.*(\\<utpl\\>|[[:space:]]-[[:alnum:]]*T[[:alnum:]]*\\>)", "\\{[{%#]" },
	{ FG_GRAY, "^([^{%#}]|\\{[^{%#]|[%#}][^}])+\\{[{%#]", NULL },

	/* template tags */
	{ FG_BWHITE, "\\{[{%][+-]?|-?[%}]\\}", NULL },
	{ FG_BBLUE, "\\{#[+-]?|-?#\\}", NULL },
};

#define NRULES (sizeof(highlight_rules) / sizeof(highlight_rules[0]))

static regex_t compiled_patterns[NRULES * 2];
static bool have_highlighting = false;
static bool init_attempted = false;

bool
debug_highlight_init(void)
{
	regex_t *re = NULL;
	int err = 0;
	size_t i;

	if (init_attempted)
		return have_highlighting;

	init_attempted = true;

	for (i = 0; i < NRULES; i++) {
		re = &compiled_patterns[i * 2];
		err = regcomp(re, highlight_rules[i].start, REG_EXTENDED);

		if (err != 0)
			goto err;

		re = &compiled_patterns[i * 2 + 1];

		if (highlight_rules[i].end) {
			err = regcomp(re, highlight_rules[i].end, REG_EXTENDED);

			if (err != 0)
				goto err;
		}
	}

	have_highlighting = true;

	return true;

err:
	{
		char errbuf[128];

		regerror(err, re, errbuf, sizeof(errbuf));
		fprintf(stderr, "debug_highlight: regex error: %s\n", errbuf);
	}

	for (i = 0; i < NRULES * 2; i++)
		regfree(&compiled_patterns[i]);

	have_highlighting = false;

	return false;
}

/* -- source rendering, ported from print_source_location() --------------
 *
 * The original computed hl_start/hl_end/cursor_pos as byte offsets into
 * the whole source file (it read lines off a live, seekable FILE*, so a
 * single running byte counter was the natural coordinate space). This
 * version instead receives an already-split line array and a per-line
 * column range (`hl`, in the debug protocol's own {line, col} terms), so
 * the equivalent bounds are recomputed per line instead of accumulated
 * globally - the rendering logic itself (per-character style diffing, tab/
 * control-char placeholders, truncation, background shading) is otherwise
 * unchanged. The single ULINE-underlined "current instruction" character
 * the original also drew is dropped: the protocol only ever hands clients
 * a statement *range*, not that finer-grained instruction position. */

typedef struct {
	fg_color_t color;
	ssize_t from, to;
} color_span_t;

static color_span_t *
colors_grow(color_span_t *colors, size_t *count, size_t *cap)
{
	if (*count >= *cap) {
		size_t newcap = *cap ? *cap * 2 : 16;
		color_span_t *p = realloc(colors, newcap * sizeof(*p));

		if (!p)
			return colors;

		colors = p;
		*cap = newcap;
	}

	return colors;
}

void
debug_highlight_print_source(FILE *out, char **lines, size_t nlines,
                              size_t from, size_t to,
                              const debug_highlight_span_t *hl,
                              size_t left_pad, size_t columns)
{
	debug_highlight_range_t range = { from, to };

	debug_highlight_print_source_ranges(out, lines, nlines, 1, &range, hl, left_pad, columns);
}

void
debug_highlight_print_source_ranges(FILE *out, char **lines, size_t nlines,
                                     size_t nranges,
                                     const debug_highlight_range_t *ranges,
                                     const debug_highlight_span_t *hl,
                                     size_t left_pad, size_t columns)
{
	color_span_t *colors = NULL;
	size_t colors_count = 0, colors_cap = 0;
	regex_t *ml_rule_re_end = NULL;
	fg_color_t ml_rule_color = FG_NONE;
	style_t style = { FG_BWHITE, BG_BLACK, 0 };
	size_t linenum, start_line = SIZE_MAX, end_line = 0;
	ssize_t last_indent = -1;
	size_t r;

	for (r = 0; r < nranges; r++) {
		if (ranges[r].from == 0 || ranges[r].to == 0)
			continue;

		if (ranges[r].from < start_line)
			start_line = ranges[r].from;

		if (ranges[r].to > end_line)
			end_line = ranges[r].to;
	}

	if (end_line > nlines)
		end_line = nlines;

	for (linenum = 1; linenum <= end_line; linenum++) {
		const char *linestr = lines[linenum - 1];
		ssize_t linelen = (ssize_t)strlen(linestr);
		size_t ml_rule_from = 0;
		size_t line_hl_from = SIZE_MAX, line_hl_to = SIZE_MAX;
		regmatch_t m;
		const char *p;
		int rf;

		colors_count = 0;

		/* apply highlighting rules */
		if (have_highlighting) {
			size_t i;

			/* single line matches */
			for (i = 0; i < NRULES; i++) {
				regex_t *re = &compiled_patterns[i * 2];

				if (highlight_rules[i].end != NULL)
					continue;

				for (rf = 0, p = linestr;
				     regexec(re, p, 1, &m, rf) == 0;
				     rf = REG_NOTBOL, p += m.rm_eo) {
					colors = colors_grow(colors, &colors_count, &colors_cap);
					colors[colors_count++] = (color_span_t){
						.color = highlight_rules[i].color,
						.from  = p + m.rm_so - linestr,
						.to    = p + m.rm_eo - linestr
					};

					if (m.rm_eo == m.rm_so)
						break;
				}
			}

			/* multi line matches */
			for (rf = 0, p = linestr, ml_rule_from = 0;
			     rf == 0 || ml_rule_re_end != NULL;
			     rf = REG_NOTBOL) {

				if (ml_rule_re_end != NULL) {
					if (regexec(ml_rule_re_end, p, 1, &m, 0) == 0) {
						colors = colors_grow(colors, &colors_count, &colors_cap);
						colors[colors_count++] = (color_span_t){
							.color = ml_rule_color,
							.from  = (ssize_t)ml_rule_from,
							.to    = p + m.rm_eo - linestr
						};

						ml_rule_re_end = NULL;
						ml_rule_color = FG_NONE;
						ml_rule_from = 0;
						p += m.rm_eo;
					}
					else {
						colors = colors_grow(colors, &colors_count, &colors_cap);
						colors[colors_count++] = (color_span_t){
							.color = ml_rule_color,
							.from  = (ssize_t)ml_rule_from,
							.to    = linelen
						};

						break;
					}
				}

				{
					size_t i;
					bool found = false;

					for (i = 0; i < NRULES; i++) {
						regex_t *re_start = &compiled_patterns[i * 2];
						regex_t *re_end = &compiled_patterns[i * 2 + 1];

						if (highlight_rules[i].end == NULL)
							continue;

						if (regexec(re_start, p, 1, &m, rf) == 0) {
							ml_rule_re_end = re_end;
							ml_rule_color = highlight_rules[i].color;
							ml_rule_from = (size_t)(p + m.rm_so - linestr);
							p += m.rm_eo;
							found = true;
							break;
						}
					}

					if (!found && ml_rule_re_end == NULL)
						break;
				}
			}
		}

		{
			bool print_line = false, more_lines = false;

			for (r = 0; r < nranges; r++) {
				if (ranges[r].from == 0 || ranges[r].to == 0)
					continue;

				print_line |= (linenum >= ranges[r].from && linenum <= ranges[r].to);
				more_lines |= (ranges[r].from > start_line && ranges[r].from == linenum + 1);
			}

			if (!print_line) {
				if (more_lines) {
					size_t pad = (size_t)(last_indent < 0 ? 0 : last_indent);
					size_t i;

					for (i = 0; i < left_pad; i++)
						fputc(' ', out);

					cs(out, &((style_t){ FG_GRAY, BG_BLACK, FAINT }));
					fputs("   \xe2\x80\xa6 " /* "   … " */, out);

					for (i = 0; i < pad; i++)
						fputc(' ', out);

					fputs("\xe2\x80\xa6", out);

					if (columns > 6 + pad)
						for (i = 0; i < columns - 6 - pad; i++)
							fputc(' ', out);

					cs(out, NULL);
					fputc('\n', out);
				}

				continue;
			}
		}

		/* per-line highlight bounds, translated from the {line,col}
		 * range (see comment above) */
		if (hl && hl->from_line > 0 && linenum >= hl->from_line && linenum <= hl->to_line) {
			line_hl_from = (linenum == hl->from_line) ? hl->from_col : 0;
			line_hl_to = (linenum == hl->to_line) ? hl->to_col : SIZE_MAX;
		}

		size_t trunc = 0;

		/* determine display width of line and whether it is too long */
		if (columns > 6) {
			size_t c;
			ssize_t i;

			for (i = 0, c = 0; i < linelen; i++) {
				c += (linestr[i] == '\t') ? 4 : 1;

				if (c > columns - 6) {
					trunc = (size_t)(linelen - i);
					linelen = i;
					break;
				}
			}
		}

		size_t linecols = 0;
		ssize_t i;

		for (i = 0; i < (ssize_t)left_pad; i++)
			fputc(' ', out);

		cs(out, &((style_t){ FG_GRAY, BG_BLACK, FAINT }));
		fprintf(out, "%4zu ", linenum);
		cs(out, &style);

		for (i = 0; i < linelen; i++) {
			style_t newstyle = {
				.fg = FG_BWHITE,
				.bg = ((size_t)i >= line_hl_from && (size_t)i < line_hl_to)
					? BG_GRAY : BG_BLACK,
				.styles = (hl && hl->have_ip && linenum == hl->ip_line &&
				           (size_t)i == hl->ip_col) ? ULINE : 0
			};
			size_t j;

			for (j = 0; j < colors_count; j++)
				if (colors[j].from <= i && colors[j].to > i)
					newstyle.fg = colors[j].color;

			if (memcmp(&style, &newstyle, sizeof(style))) {
				style = newstyle;
				cs(out, &style);
			}

			if (linestr[i] == '\t') {
				linecols += 4;
				cs(out, &((style_t){ FG_BBLACK, style.bg, FAINT }));
				fputs("<-> ", out);
				cs(out, &style);
			}
			else if (linestr[i] < ' ' || linestr[i] == 0x7f) {
				linecols++;
				cs(out, &((style_t){ FG_BBLACK, style.bg, FAINT }));
				fputc('.', out);
				cs(out, &style);
			}
			else {
				if (last_indent == -1)
					last_indent = (ssize_t)linecols;

				linecols++;
				fputc(linestr[i], out);
			}
		}

		/* reset char styles */
		style.styles = 0;
		style.bg = ((size_t)linelen >= line_hl_from && (size_t)(linelen) + trunc <= line_hl_to)
			? BG_GRAY : BG_BLACK;
		cs(out, &style);

		if (trunc > 0) {
			if (columns > 6 && linecols < columns - 6)
				for (i = 0; i < (ssize_t)((columns - 6) - linecols); i++)
					fputc(' ', out);

			fputs("\xe2\x80\xa6" /* U+2026 HORIZONTAL ELLIPSIS */, out);
		}
		else if (columns > 5 && linecols < columns - 5) {
			if (style.bg != BG_BLACK) {
				style.bg = BG_BLACK;
				cs(out, &style);
			}

			for (i = 0; i < (ssize_t)((columns - 5) - linecols); i++)
				fputc(' ', out);
		}

		cs(out, &((style_t){ FG_NONE, BG_NONE, 0 }));
		fputc('\n', out);
	}

	free(colors);
}

/* -- header bar, ported from format_context_header_backtrace()/
 * format_context_header_callframe() -------------------------------------- */

/* Elide the front of `s` (in place) down to at most `maxcols` bytes,
 * prefixing a horizontal-ellipsis marker, so the *tail* stays visible -
 * matches the original's choice for both filenames (basename matters more
 * than the leading directories) and call breadcrumbs (the innermost/
 * current frame matters more than the outermost). Byte-based rather than
 * the original's UTF-8/ANSI-escape-aware column counting - a reasonable
 * simplification for what is normally short, plain ASCII text (paths,
 * identifiers). */
static char *
truncate_head(const char *s, size_t maxcols)
{
	static const char ellipsis[] = "\xe2\x80\xa6"; /* U+2026, 1 column, 3 bytes */
	size_t len = strlen(s);
	char *out;

	if (maxcols == 0 || len <= maxcols)
		return strdup(s);

	if (maxcols <= 1)
		return strdup(ellipsis);

	out = malloc(sizeof(ellipsis) - 1 + (maxcols - 1) + 1);
	memcpy(out, ellipsis, sizeof(ellipsis) - 1);
	memcpy(out + sizeof(ellipsis) - 1, s + (len - (maxcols - 1)), maxcols - 1);
	out[sizeof(ellipsis) - 1 + (maxcols - 1)] = '\0';

	return out;
}

void
debug_highlight_print_header_bar(FILE *out, const char *bracket, const char *rest,
                                  size_t left_pad, size_t columns)
{
	size_t columns_avail = (columns > left_pad) ? columns - left_pad : 0;
	size_t bracket_width = (columns_avail >= 42) ? (columns_avail - 2) / 4 : columns_avail;
	char *bracket_trunc = columns_avail ? truncate_head(bracket, bracket_width) : strdup(bracket);
	size_t printed = 2 + strlen(bracket_trunc);
	size_t i;

	for (i = 0; i < left_pad; i++)
		fputc(' ', out);

	cs(out, &((style_t){ FG_BWHITE, BG_GRAY, 0 }));
	fprintf(out, "[%s]", bracket_trunc);
	free(bracket_trunc);

	if (rest && *rest && (!columns_avail || columns_avail > printed + 2 + 10)) {
		size_t rest_width = columns_avail ? columns_avail - printed - 2 : 0;
		char *rest_trunc = columns_avail ? truncate_head(rest, rest_width) : strdup(rest);

		fprintf(out, " %s ", rest_trunc);
		printed += 2 + strlen(rest_trunc);
		free(rest_trunc);
	}

	if (columns_avail > printed)
		for (i = 0; i < columns_avail - printed; i++)
			fputc(' ', out);

	cs(out, NULL);
	fputc('\n', out);
}

/* -- disassembly, ported from cmd_disasm() (formerly lib/debug.c) -------- */

/* Minimal growable byte buffer, used to build one disassembly line at a
 * time (with real "\033[...m" sequences already embedded) so it can be
 * measured and truncated to `columns` before being written out - mirrors
 * what the pre-protocol version did with a uc_stringbuf_t. */
typedef struct {
	char *buf;
	size_t len, cap;
} dbuf_t;

static void
dbuf_reserve(dbuf_t *b, size_t extra)
{
	if (b->len + extra + 1 > b->cap) {
		size_t newcap = b->cap ? b->cap * 2 : 128;

		while (newcap < b->len + extra + 1)
			newcap *= 2;

		b->buf = realloc(b->buf, newcap);
		b->cap = newcap;
	}
}

static void
dbuf_style(dbuf_t *b, const style_t *style)
{
	char tmp[32];
	int codes[8] = { 0 };
	size_t i = 0, n = 0;

	if (style == NULL) {
		dbuf_reserve(b, 4);
		memcpy(b->buf + b->len, "\033[0m", 4);
		b->len += 4;
		return;
	}

	if ((style->styles & (BOLD | FAINT | ULINE)) == 0)
		codes[i++] = 0;

	if (style->styles & BOLD)  codes[i++] = 1;
	if (style->styles & FAINT) codes[i++] = 2;
	if (style->styles & ULINE) codes[i++] = 4;

	codes[i++] = style->fg ? style->fg : 39;
	codes[i++] = style->bg ? style->bg : 49;

	n += sprintf(tmp + n, "\033[");

	for (size_t k = 0; k < i; k++)
		n += sprintf(tmp + n, "%s%d", k ? ";" : "", codes[k]);

	n += sprintf(tmp + n, "m");

	dbuf_reserve(b, n);
	memcpy(b->buf + b->len, tmp, n);
	b->len += n;
}

__attribute__((format(printf, 2, 3))) static void
dbuf_printf(dbuf_t *b, const char *fmt, ...)
{
	va_list ap, ap2;
	int n;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);

	if (n > 0) {
		dbuf_reserve(b, (size_t)n);
		vsnprintf(b->buf + b->len, (size_t)n + 1, fmt, ap);
		b->len += (size_t)n;
	}

	va_end(ap);
}

/* Byte-based visible-width truncation with a trailing ellipsis, skipping
 * embedded "\033[...m" escape sequences when counting columns - the same
 * "reasonable simplification" truncate_head() above documents, since
 * disassembly text (mnemonics, hex, identifiers) is normally plain ASCII. */
static void
dbuf_truncate_tail(dbuf_t *b, size_t maxcols)
{
	size_t col = 0, i = 0, cut = SIZE_MAX;

	if (maxcols == 0)
		return;

	while (i < b->len) {
		if (b->buf[i] == '\033') {
			size_t j = i + 1;

			if (j < b->len && b->buf[j] == '[') {
				j++;

				while (j < b->len && b->buf[j] != 'm')
					j++;

				if (j < b->len)
					j++;
			}

			i = j;
			continue;
		}

		if (col + 1 == maxcols && cut == SIZE_MAX)
			cut = i;

		col++;
		i++;
	}

	if (col > maxcols && cut != SIZE_MAX) {
		b->len = cut;
		dbuf_printf(b, "\xe2\x80\xa6" /* U+2026 HORIZONTAL ELLIPSIS */);
	}
}

static void
dbuf_flush(dbuf_t *b, FILE *out, size_t columns)
{
	dbuf_truncate_tail(b, columns);
	dbuf_style(b, NULL);
	fwrite(b->buf, 1, b->len, out);
	fputc('\n', out);
	b->len = 0;
}

void
debug_highlight_print_disassembly(FILE *out, const char *function,
                                   const debug_disasm_insn_t *insns,
                                   size_t ninsns, size_t columns)
{
	dbuf_t line = { 0 };
	static const style_t st_none   = { FG_NONE, 0, 0 };
	static const style_t st_op     = { FG_BMAGENT, 0, 0 };
	static const style_t st_cyan   = { FG_CYAN, 0, 0 };
	static const style_t st_white  = { FG_BWHITE, 0, 0 };
	static const style_t st_yellow = { FG_YELLOW, 0, 0 };
	static const style_t st_red    = { FG_RED, 0, 0 };

	fprintf(out, "Function: %s\n", function ? function : "?");

	for (size_t idx = 0; idx < ninsns; idx++) {
		const debug_disasm_insn_t *ins = &insns[idx];
		int fmt = ins->format;
		int absfmt = (fmt < 0) ? -fmt : fmt;

		if (absfmt > 4)
			absfmt = 4;

		dbuf_printf(&line, "%06zu:", ins->offset);

		/* Only the base instruction (opcode + its fixed-width operand, per
		 * `format`) is shown here - CLFN/ARFN's per-upvalue-capture bytes
		 * and CALL's per-argument unpack bytes that may follow in `bytes`
		 * (uc_vm_insn_call() needs the *full* instruction length to skip
		 * over them) get their own indented hex dump below instead. */
		for (size_t j = 0; j < ins->nbytes && j <= (size_t)absfmt; j++) {
			dbuf_printf(&line, " ");
			dbuf_style(&line, (j == 0) ? &st_none : &st_op);
			dbuf_printf(&line, "%02x", ins->bytes[j]);
			dbuf_style(&line, NULL);
		}

		for (int j = 0; j < 3 * (4 - absfmt); j++)
			dbuf_printf(&line, " ");

		dbuf_printf(&line, "  %7s", ins->mnemonic ? ins->mnemonic : "?");

		switch (fmt) {
		case 0:
			break;

		case -4: {
			int64_t v = ins->operand;
			uint32_t mag = (uint32_t)((v < 0) ? -v : v);

			dbuf_printf(&line, " {");
			dbuf_style(&line, &st_op);
			dbuf_printf(&line, "%c0x%x", (v < 0) ? '-' : '+', mag);
			dbuf_style(&line, NULL);
			dbuf_printf(&line, "}");
			break;
		}

		case 1:
			dbuf_printf(&line, " {");
			dbuf_style(&line, &st_op);
			dbuf_printf(&line, "%" PRIu64, (uint64_t)ins->operand);
			dbuf_style(&line, NULL);
			dbuf_printf(&line, "}");
			break;

		case 2:
			dbuf_printf(&line, " {");
			dbuf_style(&line, &st_op);
			dbuf_printf(&line, "0x%" PRIx64, (uint64_t)ins->operand);
			dbuf_style(&line, NULL);
			dbuf_printf(&line, "}");
			break;

		case 4:
			dbuf_printf(&line, " {");
			dbuf_style(&line, &st_op);
			dbuf_printf(&line, "0x%" PRIx64, (uint64_t)ins->operand);
			dbuf_style(&line, NULL);

			if (ins->have_constant) {
				dbuf_printf(&line, " : ");
				dbuf_style(&line, ins->constant_is_string ? &st_op : &st_cyan);
				dbuf_printf(&line, "%s", ins->constant_repr ? ins->constant_repr : "null");
				dbuf_style(&line, NULL);
			}
			else if (ins->variable_kind && strcmp(ins->variable_kind, "global") == 0) {
				dbuf_printf(&line, " : global ");
				dbuf_style(&line, &st_white);
				dbuf_printf(&line, "%s", ins->variable_name ? ins->variable_name : "(unknown)");
				dbuf_style(&line, NULL);
			}
			else if (ins->variable_kind) {
				bool upval = !strcmp(ins->variable_kind, "upval");

				dbuf_printf(&line, " : %s ", ins->variable_kind);
				dbuf_style(&line, upval ? &st_cyan : &st_white);
				dbuf_printf(&line, "%s", ins->variable_name ? ins->variable_name : "(unknown)");
				dbuf_style(&line, NULL);
			}
			else if (ins->have_closure) {
				dbuf_printf(&line, " : %s ", ins->closure_kind ? ins->closure_kind : "closure");
				dbuf_style(&line, &st_op);
				dbuf_printf(&line, "#%" PRIu32, ins->closure_index);
				dbuf_style(&line, NULL);
			}
			else if (ins->have_call) {
				dbuf_printf(&line, " : ");

				if (ins->stdlib_name) {
					dbuf_style(&line, &st_cyan);
					dbuf_printf(&line, "%s", ins->stdlib_name);
					dbuf_style(&line, NULL);
					dbuf_printf(&line, ", ");
				}
				else if (ins->call_mcall) {
					dbuf_printf(&line, "mcall, ");
				}

				dbuf_style(&line, &st_op);
				dbuf_printf(&line, "%" PRIu32, ins->call_nargs);
				dbuf_style(&line, NULL);
				dbuf_printf(&line, " arg%s", (ins->call_nargs == 1) ? "" : "s");

				if (ins->call_tail) {
					dbuf_style(&line, &st_yellow);
					dbuf_printf(&line, " (tail call)");
					dbuf_style(&line, NULL);
				}
			}

			dbuf_printf(&line, "}");
			break;

		default:
			dbuf_style(&line, &st_red);
			dbuf_printf(&line, " (unknown operand format: %d)", fmt);
			dbuf_style(&line, NULL);
			break;
		}

		/* this I_RETURN terminates a tail call: the 0x00 marker byte the
		 * compiler emits right after it was consumed as part of this
		 * instruction (it is pure data, never executed), so annotate the
		 * return rather than showing a separate NOOP line. */
		if (ins->return_tailcall) {
			dbuf_printf(&line, " ; ");
			dbuf_style(&line, &st_yellow);
			dbuf_printf(&line, "tail call (marker consumed)");
			dbuf_style(&line, NULL);
		}

		dbuf_flush(&line, out, columns);

		for (size_t j = 0; j < ins->ncaptures; j++) {
			bool upval = ins->captures[j].upval;
			int64_t slot = ins->captures[j].slot;
			uint32_t mag = (uint32_t)((slot < 0) ? -slot : slot);

			dbuf_printf(&line, "         \xe2\x80\xa6 " /* "   … " */);
			dbuf_style(&line, &st_yellow);

			for (size_t k = 0; k < 4; k++)
				dbuf_printf(&line, "%s%02x", k ? " " : "", ins->captures[j].bytes[k]);

			dbuf_style(&line, NULL);
			dbuf_printf(&line, "  capture {");
			dbuf_style(&line, &st_yellow);
			dbuf_printf(&line, "%c0x%x", (slot < 0) ? '-' : '+', mag);
			dbuf_style(&line, NULL);
			dbuf_printf(&line, " : %s ", upval ? "upval" : "local");
			dbuf_style(&line, upval ? &st_cyan : &st_white);
			dbuf_printf(&line, "%s", ins->captures[j].name ? ins->captures[j].name : "(unknown)");
			dbuf_style(&line, NULL);
			dbuf_printf(&line, "}");

			dbuf_flush(&line, out, columns);
		}

		for (size_t j = 0; j < ins->nunpacks; j++) {
			uint16_t slot = ins->unpacks[j].slot;

			dbuf_printf(&line, "         \xe2\x80\xa6 " /* "   … " */);
			dbuf_style(&line, &st_yellow);
			dbuf_printf(&line, "%02x %02x", ins->unpacks[j].bytes[0], ins->unpacks[j].bytes[1]);
			dbuf_style(&line, NULL);
			dbuf_printf(&line, "         unpack {");
			dbuf_style(&line, &st_yellow);
			dbuf_printf(&line, "0x%x", slot);
			dbuf_style(&line, NULL);
			dbuf_printf(&line, " : stack slot ");
			dbuf_style(&line, &st_op);
			dbuf_printf(&line, "-0x%x", (unsigned)(slot + 1));
			dbuf_style(&line, NULL);
			dbuf_printf(&line, "}");

			dbuf_flush(&line, out, columns);
		}
	}

	free(line.buf);
}

/* -- variables listing, ported from print_variables() (formerly
 * lib/debug.c) ------------------------------------------------------------ */

/* Like dbuf_truncate_tail(), but for a compact JSON-ish value repr: places
 * the ellipsis just before a synthetic closing bracket/quote so a truncated
 * object/array/string still visually reads as one and stays on a single
 * line, matching printbuf_append_uv()'s (formerly lib/debug.c) truncation
 * exactly. Byte-based rather than UTF-8-aware, the same simplification
 * truncate_head() above documents. */
static void
dbuf_truncate_value(dbuf_t *b, size_t maxcols)
{
	const char *end;
	size_t keep;

	if (maxcols == 0 || b->len <= maxcols)
		return;

	switch (b->buf[0]) {
	case '{': keep = (maxcols > 3) ? maxcols - 3 : 0; end = "\xe2\x80\xa6 }"; break;
	case '[': keep = (maxcols > 3) ? maxcols - 3 : 0; end = "\xe2\x80\xa6 ]"; break;
	case '"': keep = (maxcols > 2) ? maxcols - 2 : 0; end = "\xe2\x80\xa6\""; break;
	default:  keep = (maxcols > 1) ? maxcols - 1 : 0; end = "\xe2\x80\xa6";   break;
	}

	b->len = keep;
	dbuf_printf(b, "%s", end);
}

void
debug_highlight_print_variables(FILE *out, const debug_variable_t *vars,
                                 size_t nvars, const char *indent,
                                 size_t columns)
{
	static const style_t st_upval = { FG_CYAN, 0, BOLD };
	static const style_t st_faint = { FG_BWHITE, 0, FAINT };
	static const style_t st_err   = { FG_RED, 0, BOLD };
	static const char shadowed_suffix[] = "  (shadowed)";
	size_t indent_len = indent ? strlen(indent) : 0;
	size_t value_cols = 0;
	dbuf_t namebuf = { 0 }, valuebuf = { 0 };

	if (columns > indent_len + 19)
		value_cols = columns - indent_len - 19;

	for (size_t i = 0; i < nvars; i++) {
		const debug_variable_t *v = &vars[i];
		const char *kind = v->kind ? v->kind : "";
		const char *name = v->name ? v->name : "?";
		const char *repr = v->value_repr ? v->value_repr : "";
		bool upval = !strcmp(kind, "upvalue");
		bool faint = v->shadowed || !strcmp(kind, "this") || !strcmp(kind, "internal");
		bool err = !strcmp(repr, "<out of range>");
		size_t namelen;

		namebuf.len = 0;
		valuebuf.len = 0;

		dbuf_printf(&namebuf, "%s", name);
		namelen = namebuf.len;
		dbuf_truncate_tail(&namebuf, 16);

		if (indent)
			fputs(indent, out);

		/* A shadowed entry is rendered faint throughout, taking priority
		 * over its own kind's usual color (still cyan/upvalue matters
		 * far less than "this isn't what the name resolves to anymore"). */
		if (v->shadowed)
			cs(out, &st_faint);
		else if (upval)
			cs(out, &st_upval);
		else if (faint)
			cs(out, &st_faint);

		fwrite(namebuf.buf, 1, namebuf.len, out);

		if (v->shadowed || upval || faint)
			cs(out, NULL);

		for (; namelen < 16; namelen++)
			fputc(' ', out);

		cs(out, &st_faint);
		fputs(" : ", out);
		cs(out, NULL);

		if (err) {
			cs(out, &st_err);
			fputs(repr, out);
			cs(out, NULL);
		}
		else {
			size_t this_value_cols = value_cols;

			/* Reserve room for the trailing "(shadowed)" marker printed
			 * below, or it doesn't count against the line's width budget
			 * and can push the whole line past `columns`, wrapping. */
			if (v->shadowed && this_value_cols > sizeof(shadowed_suffix) - 1)
				this_value_cols -= sizeof(shadowed_suffix) - 1;

			if (v->shadowed)
				cs(out, &st_faint);

			dbuf_printf(&valuebuf, "%s", repr);

			/* value_repr is always the compact, single-line repr (see
			 * build_variables_json() in lib/debug.c) - guard against a
			 * literal embedded newline anyway, since byte-counting
			 * truncation across one would garble rather than shorten it. */
			if (columns > 0 && !strchr(repr, '\n'))
				dbuf_truncate_value(&valuebuf, this_value_cols);

			fwrite(valuebuf.buf, 1, valuebuf.len, out);

			if (v->shadowed)
				cs(out, NULL);
		}

		if (v->shadowed) {
			cs(out, &st_faint);
			fputs(shadowed_suffix, out);
			cs(out, NULL);
		}

		fputc('\n', out);
	}

	free(namebuf.buf);
	free(valuebuf.buf);
}

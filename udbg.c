/*
 * udbg - ucode debugger client
 *
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
 * Interactive client for ucode's line-based debug protocol (one uppercase
 * VERB, optionally followed by a space and a JSON object, per '\n'-terminated
 * line - see lib/debug_proto.h). This client owns all user-facing rendering:
 * the server-side debug core never emits ANSI or formatted columns, only
 * structured data (plus, where a rendering-rich port needed more than the
 * original data model had - e.g. DISASSEMBLE's raw instruction bytes - a
 * small additive extension of that same structured data, never markup).
 *
 * Three ways to obtain a connection:
 *   udbg <pid>    - SIGUSR1-attach to a running `-X` process (gdb -p style)
 *   udbg <path>   - connect to an explicit debug.listen(path) socket
 *   udbg --fd N   - use an already-connected, inherited fd N (used
 *                   internally by the local `-x` CLI, which forks this
 *                   binary with one end of a socketpair on fd 3)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>

#include "ucode/types.h"
#include "ucode/json.h"

#include "debug_highlight.h"
#include "debug_lineedit.h"

/* -- ANSI colors ----------------------------------------------------------- */

#define C_RESET   "\033[0m"
#define C_DIM     "\033[2m"
#define C_BOLD    "\033[1m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"
#define C_EVENT   "\033[2;3m" /* faint + italic, for async server events */

#define MAX_LINE 65536
#define DEFAULT_SOCKET_DIR "/tmp"
#define MAX_WAIT_TIME 30

/* -- wire framing ----------------------------------------------------------
 *
 * Mirrors lib/debug_proto.c's framing without depending on it; payloads are
 * ucode values without a VM. */

static void
proto_write(int fd, const char *verb, uc_value_t *payload)
{
	char *json = NULL;
	char *line;
	size_t len;
	ssize_t n;
	const char *p;

	if (payload) {
		json = ucv_to_jsonstring(NULL, payload);
		len = strlen(verb) + 1 + strlen(json) + 1;
		line = malloc(len + 1);
		snprintf(line, len + 1, "%s %s\n", verb, json);
	}
	else {
		len = strlen(verb) + 1;
		line = malloc(len + 1);
		snprintf(line, len + 1, "%s\n", verb);
	}

	p = line;

	while (len > 0) {
		n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			break;
		}

		p += n;
		len -= (size_t)n;
	}

	free(line);
	free(json);
}

/* Growable line-buffered reader, one instance per connection. */
typedef struct {
	char *data;
	size_t len, cap;
} linebuf_t;

static bool
linebuf_append(linebuf_t *lb, const char *data, size_t n)
{
	if (lb->len + n > lb->cap) {
		size_t newcap = lb->cap ? lb->cap : 4096;

		while (newcap < lb->len + n)
			newcap *= 2;

		char *p = realloc(lb->data, newcap);

		if (!p)
			return false;

		lb->data = p;
		lb->cap = newcap;
	}

	memcpy(lb->data + lb->len, data, n);
	lb->len += n;

	return true;
}

/* Parses one payload; NULL if it is no valid JSON */
static uc_value_t *
jparse(const char *s)
{
	uc_json_parser_t *parser = uc_json_parser_new(NULL);
	uc_value_t *val = uc_json_parse(parser, s, strlen(s) + 1);

	if (uc_json_parser_error(parser) != UC_JSON_SUCCESS) {
		ucv_put(val);
		val = NULL;
	}

	uc_json_parser_free(parser);

	return val;
}

/* Extract one already-buffered "VERB [json]" line, if any, without touching
 * the fd. Returns false if no full line is buffered yet. */
static bool
linebuf_pop(linebuf_t *lb, char **verb_out, uc_value_t **payload_out)
{
	char *nl = memchr(lb->data, '\n', lb->len);
	size_t linelen, verblen;
	char *line, *sp;

	if (!nl)
		return false;

	linelen = (size_t)(nl - lb->data);
	line = malloc(linelen + 1);
	memcpy(line, lb->data, linelen);
	line[linelen] = '\0';

	memmove(lb->data, lb->data + linelen + 1, lb->len - linelen - 1);
	lb->len -= linelen + 1;

	if (linelen > 0 && line[linelen - 1] == '\r')
		line[--linelen] = '\0';

	sp = memchr(line, ' ', linelen);
	verblen = sp ? (size_t)(sp - line) : linelen;

	*verb_out = malloc(verblen + 1);
	memcpy(*verb_out, line, verblen);
	(*verb_out)[verblen] = '\0';

	*payload_out = NULL;

	if (sp && *(sp + 1))
		*payload_out = jparse(sp + 1);

	free(line);

	return true;
}

/* Block until a full message is available on `fd`/`lb` and pop it - used for
 * the synchronous SOURCE request/response round-trip triggered from within
 * rendering. Only safe to call while the session is paused and no other
 * request is outstanding (true for every call site below): the server only
 * ever answers strictly in request order while paused, so the first message
 * to arrive is the one we asked for, barring the rare case of an async
 * EVENT interleaving, which is not handled specially here. */
/* Returns the slot of the value of `key` in the object `obj`, or NULL. The
 * strings of payloads are read from their slots, since a short string lives
 * in the value pointer itself. */
static uc_value_t **
jslot(uc_value_t *obj, const char *key)
{
	uc_object_t *object = (uc_object_t *)obj;
	size_t len = strlen(key);

	if (ucv_type(obj) != UC_OBJECT)
		return NULL;

	for (uint32_t i = 0; i < object->used; i++) {
		uc_object_entry_t *e = &object->entries[i];

		if (e->key && ucv_string_length(e->key) == len && !memcmp(ucv_string_get(e->key), key, len))
			return &e->value;
	}

	return NULL;
}

/* Returns the slot of item `i` of the array `arr`, or NULL */
static uc_value_t **
jitem(uc_value_t *arr, size_t i)
{
	if (ucv_type(arr) != UC_ARRAY || i >= ucv_array_length(arr))
		return NULL;

	return &((uc_array_t *)arr)->entries[i];
}

static uc_value_t *
jget(uc_value_t *obj, const char *key)
{
	uc_value_t **slot = jslot(obj, key);

	return slot ? *slot : NULL;
}

static bool
jhas(uc_value_t *obj, const char *key)
{
	return jslot(obj, key) != NULL;
}

static const char *
jslot_str(uc_value_t **slot, const char *dflt)
{
	if (slot && ucv_type(*slot) == UC_STRING)
		return _ucv_string_get(slot);

	return dflt;
}

static const char *
jstr(uc_value_t *obj, const char *key, const char *dflt)
{
	return jslot_str(jslot(obj, key), dflt);
}

static int64_t
jint(uc_value_t *obj, const char *key, int64_t dflt)
{
	uc_value_t **slot = jslot(obj, key);

	return slot ? ucv_to_integer(*slot) : dflt;
}

/* Returns a new object with the string `value` under `key` */
static uc_value_t *
jobject_str(const char *key, const char *value)
{
	uc_value_t *obj = ucv_object_new(NULL);

	ucv_object_add(obj, key, ucv_string_new(value));

	return obj;
}

/* Every "col"/"from_col"/"to_col" field the protocol sends is 1-based (see
 * uc_source_get_line() in source.c), meant for human-readable "line:col"
 * display - debug_highlight's span/ip columns are 0-based byte indices
 * into the line string, so any such field needs this before being used as
 * one. */
static size_t
col0(int64_t col)
{
	return (col > 0) ? (size_t)(col - 1) : 0;
}

/* -- source cache & syntax highlighting ----------------------------------- */

typedef struct source_cache_entry {
	char *file;
	char **lines;
	size_t nlines;
	struct source_cache_entry *next;
} source_cache_entry_t;

static source_cache_entry_t *source_cache = NULL;

static char **
split_lines(const char *text, size_t *nlines_out)
{
	size_t count = 1, i;
	char **lines;
	const char *p, *start;

	for (p = text; *p; p++)
		if (*p == '\n')
			count++;

	lines = calloc(count, sizeof(char *));
	i = 0;
	start = text;

	for (p = text; ; p++) {
		if (*p == '\n' || *p == '\0') {
			size_t len = (size_t)(p - start);

			if (len > 0 && start[len - 1] == '\r')
				len--;

			lines[i] = malloc(len + 1);
			memcpy(lines[i], start, len);
			lines[i][len] = '\0';
			i++;

			if (*p == '\0')
				break;

			start = p + 1;
		}
	}

	*nlines_out = i;

	return lines;
}

static char **
find_cached_source(const char *file, size_t *nlines_out)
{
	source_cache_entry_t *e;

	for (e = source_cache; e; e = e->next) {
		if (!strcmp(e->file, file)) {
			*nlines_out = e->nlines;

			return e->lines;
		}
	}

	return NULL;
}

/* Split and cache already-known source `text` for `file` (e.g. from a
 * SOURCE response the caller already has in hand), so a later
 * render_source_lines() call for the same file doesn't re-request it. */
static char **
cache_source_text(const char *file, const char *text, size_t *nlines_out)
{
	source_cache_entry_t *e;
	char **cached = find_cached_source(file, nlines_out);

	if (cached)
		return cached;

	e = malloc(sizeof(source_cache_entry_t));
	e->file = strdup(file);
	e->lines = split_lines(text, &e->nlines);
	e->next = source_cache;
	source_cache = e;

	*nlines_out = e->nlines;

	return e->lines;
}

/* Local source root override (-s/--srcdir), used when the path the server
 * reports doesn't exist as-is on this machine - see try_load_local_file(). */
static const char *opt_srcdir = NULL;

static char *
read_whole_file(FILE *fp)
{
	char buf[65536];
	size_t n, cap = 0, len = 0;
	char *text = NULL;

	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		if (len + n + 1 > cap) {
			cap = cap ? cap * 2 : 65536;

			while (cap < len + n + 1)
				cap *= 2;

			text = realloc(text, cap);
		}

		memcpy(text + len, buf, n);
		len += n;
	}

	if (!text)
		text = malloc(1);

	text[len] = '\0';

	return text;
}

/* Debugging usually either runs fully locally (the client and the debugged
 * script share the same filesystem - the common `-x`/`udbg <pid>`-on-the-
 * same-box case) or from a development checkout against a remote target
 * (the *client* has the better/only real source access, not the server) -
 * in both cases, the client reading the file itself is at least as likely
 * to succeed as asking the server for it, and doesn't need a round trip.
 * Only once this fails do callers fall back to requesting SOURCE from the
 * server (e.g. the target is a remote embedded device with no shared
 * filesystem, or running precompiled bytecode with only embedded source).
 *
 * Tries the path exactly as the server reported it first (already correct
 * for the local case, and for absolute paths that happen to also exist on
 * this machine), then, if `-s/--srcdir DIR` was given, DIR joined with
 * just the reported path's basename - a simple heuristic for "the server's
 * path is from a different checkout/build root than this one". */
static char **
try_load_local_file(const char *file, size_t *nlines_out)
{
	FILE *fp = fopen(file, "rb");
	char *joined = NULL;

	if (!fp && opt_srcdir) {
		const char *base = strrchr(file, '/');

		base = base ? base + 1 : file;
		joined = malloc(strlen(opt_srcdir) + 1 + strlen(base) + 1);
		sprintf(joined, "%s/%s", opt_srcdir, base);
		fp = fopen(joined, "rb");
	}

	free(joined);

	if (!fp)
		return NULL;

	{
		char *text = read_whole_file(fp);
		char **lines = cache_source_text(file, text, nlines_out);

		fclose(fp);
		free(text);

		return lines;
	}
}

/* Rendering a source range/context needs the actual text, which only ever
 * arrives asynchronously as a SOURCE response processed by the normal main
 * loop - never via a nested blocking round-trip from inside another
 * response's rendering, which would re-enter the single shared connection
 * state from two places at once. So when the file isn't cached yet, a
 * render call fires off a SOURCE request and remembers what it wanted to
 * show as `pending_source`; the main loop's SOURCE handler finishes the
 * render once the response actually arrives. */
typedef struct {
	bool active;
	char *file;
	int64_t from, to;
	debug_highlight_span_t hl;
	bool have_hl;
	size_t left_pad;
} pending_source_t;

static pending_source_t pending_source = { 0 };

static void
request_source(int fd, const char *file, int64_t from, int64_t to,
                const debug_highlight_span_t *hl, size_t left_pad)
{
	uc_value_t *payload;

	/* A fetch for this exact file is already in flight (e.g. the initial
	 * PAUSED's own auto-context request hasn't resolved yet when a
	 * "lines" response also wants it) - don't send a second SOURCE
	 * request that would only overwrite this same pending_source's
	 * tracking with no way to reconcile the two, just adopt whichever
	 * range was asked for most recently and let the one response in
	 * flight satisfy it. */
	if (pending_source.active && !strcmp(pending_source.file, file)) {
		pending_source.from = from;
		pending_source.to = to;
		pending_source.have_hl = (hl != NULL);
		pending_source.left_pad = left_pad;

		if (hl)
			pending_source.hl = *hl;

		return;
	}

	payload = jobject_str("file", file);
	proto_write(fd, "SOURCE", payload);
	ucv_put(payload);

	free(pending_source.file);
	pending_source.active = true;
	pending_source.file = strdup(file);
	pending_source.from = from;
	pending_source.to = to;
	pending_source.have_hl = (hl != NULL);
	pending_source.left_pad = left_pad;

	if (hl)
		pending_source.hl = *hl;
}

/* Current terminal width, for the same wrap/pad behavior
 * debug_highlight_print_source()'s ported original had via term_width().
 * Falls back to 80 columns when stdout isn't a tty (e.g. piped output). */
static size_t
term_columns(void)
{
	struct winsize w;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
		return w.ws_col;

	return 80;
}

/* Print source lines [from, to] (1-based, inclusive) from `file`, shading
 * the `hl` statement span if given. If the text isn't cached yet,
 * asynchronously requests it (see `pending_source` above) and returns
 * without printing anything - the main loop's SOURCE response handler
 * re-invokes this once the text has actually arrived. */

/* Mirrors format_context_statement()'s range-splitting for a statement/
 * function too long to show in full: a window of context around `from`,
 * a gap, and a window around the current instruction and/or `to` - using
 * the same 2-line-before/2-line-after context radius render_paused()/
 * render_backtrace_final() already use. Returns the number of ranges
 * written to `ranges` (1 if the span is short enough to just show whole,
 * up to 3 otherwise). Falls back to a single [from, to] range verbatim if
 * there's no known "current line" to anchor the split around. */
static size_t
compute_context_ranges(int64_t from, int64_t to, const debug_highlight_span_t *hl,
                        debug_highlight_range_t ranges[3])
{
	const int64_t ctx = 2;
	int64_t ip;
	debug_highlight_range_t r[3] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
	size_t n = 0, i;

	if (from < 1)
		from = 1;

	if (!hl || !hl->have_ip || to - from <= 4) {
		ranges[0] = (debug_highlight_range_t){ (size_t)from, (size_t)to };
		return 1;
	}

	ip = (int64_t)hl->ip_line;

	if (ip < from)
		ip = from;

	if (ip > to)
		ip = to;

	if (ip - from <= (ctx + ctx + 2)) {
		r[1].from = (size_t)from;
	}
	else {
		r[0].from = (size_t)from;
		r[0].to = (size_t)(from + ctx);
		r[1].from = (size_t)(ip - ctx);
	}

	if (to - ip <= (ctx + ctx + 2)) {
		r[1].to = (size_t)to;
	}
	else {
		r[1].to = (size_t)(ip + ctx);
		r[2].from = (size_t)(to - ctx);
		r[2].to = (size_t)to;
	}

	for (i = 0; i < 3; i++)
		if (r[i].from && r[i].to)
			ranges[n++] = r[i];

	return n;
}

static void
render_source_lines(int fd, const char *file, int64_t from, int64_t to,
                     const debug_highlight_span_t *hl, size_t left_pad)
{
	size_t nlines;
	char **lines = find_cached_source(file, &nlines);

	if (!lines)
		lines = try_load_local_file(file, &nlines);

	if (!lines) {
		request_source(fd, file, from, to, hl, left_pad);
		return;
	}

	if (from < 1)
		from = 1;

	{
		size_t columns = term_columns();
		debug_highlight_range_t ranges[3];
		size_t nranges;

		columns = (columns > left_pad) ? columns - left_pad : 0;
		nranges = compute_context_ranges(from, to, hl, ranges);

		debug_highlight_print_source_ranges(stdout, lines, nlines,
			nranges, ranges, hl, left_pad, columns);
	}
}

/* -- response rendering ------------------------------------------------- */

/* Join a JSON array of strings with " \xc2\xbb " (U+00BB, " » "), matching
 * format_context_breadcrumb()'s separator. Caller frees the result. */
static char *
join_breadcrumb(uc_value_t *arr)
{
	static const char sep[] = " \xc2\xbb "; /* U+00BB RIGHT-POINTING GUILLEMET */
	size_t n = ucv_array_length(arr);
	size_t len = 0, i;
	char *out, *p;

	if (n == 0)
		return strdup("");

	for (i = 0; i < n; i++)
		len += strlen(jslot_str(jitem(arr, i), ""));

	len += (n - 1) * (sizeof(sep) - 1);
	out = p = malloc(len + 1);

	for (i = 0; i < n; i++) {
		const char *s = jslot_str(jitem(arr, i), "");
		size_t l = strlen(s);

		if (i > 0) {
			memcpy(p, sep, sizeof(sep) - 1);
			p += sizeof(sep) - 1;
		}

		memcpy(p, s, l);
		p += l;
	}

	*p = '\0';

	return out;
}

static void
render_paused(int fd, uc_value_t *p)
{
	int64_t line = jint(p, "line", 0);
	const char *file = jstr(p, "file", NULL);

	printf(C_BOLD "Paused" C_RESET " (%s) in " C_BOLD "%s()" C_RESET ", %s:%" PRId64 ":%" PRId64 "\n",
		jstr(p, "reason", "?"),
		jstr(p, "function", "?"),
		file ? file : "?",
		line,
		jint(p, "col", 0));

	if (jhas(p, "breakpoint_id"))
		printf("  " C_GREEN "breakpoint #%" PRId64 C_RESET "\n", jint(p, "breakpoint_id", 0));

	if (jhas(p, "exception_message"))
		printf("  " C_RED "exception: %s" C_RESET "\n", jstr(p, "exception_message", ""));

	if (file && line > 0) {
		uc_value_t *breadcrumb_arr = jget(p, "breadcrumb");
		char *breadcrumb = join_breadcrumb(breadcrumb_arr);
		int64_t col = jint(p, "col", 0);
		debug_highlight_span_t hl = {
			.from_line = (size_t)line, .from_col = 0,
			.to_line = (size_t)line, .to_col = SIZE_MAX,
			.have_ip = true, .ip_line = (size_t)line, .ip_col = col0(col)
		};

		debug_highlight_print_header_bar(stdout, file, breadcrumb, 0, term_columns());
		free(breadcrumb);

		render_source_lines(fd, file, line - 2, line + 2, &hl, 0);
	}
}

static void
render_breakpoints(uc_value_t *p)
{
	uc_value_t *items = jget(p, "items");
	size_t i, n = ucv_array_length(items);

	if (n == 0) {
		printf("No breakpoints set\n");
		return;
	}

	for (i = 0; i < n; i++) {
		uc_value_t *it = ucv_array_get(items, i);

		if (jhas(it, "id"))
			printf(C_BOLD "#%-4" PRId64 C_RESET " ", jint(it, "id", 0));
		else
			printf(C_DIM "(%-4s)" C_RESET " ", jstr(it, "kind", "?"));

		if (jhas(it, "file"))
			printf("%s:%" PRId64 ":%" PRId64 " - %s\n",
				jstr(it, "file", "?"), jint(it, "line", 0),
				jint(it, "col", 0), jstr(it, "function", "?"));
		else
			printf("<next instruction>\n");
	}
}

static void
render_variables_array(uc_value_t *items, const char *indent)
{
	size_t i, n = ucv_array_length(items);
	debug_variable_t *vars = calloc(n ? n : 1, sizeof(*vars));

	for (i = 0; i < n; i++) {
		uc_value_t *it = ucv_array_get(items, i);
		uc_value_t *shadowed_j = jget(it, "shadowed");

		vars[i].name = jstr(it, "name", "?");
		vars[i].kind = jstr(it, "kind", "");
		vars[i].value_repr = jstr(it, "value_repr", "");
		vars[i].shadowed = ucv_is_truish(shadowed_j);
	}

	debug_highlight_print_variables(stdout, vars, n, indent, term_columns());
	free(vars);
}

/* Async multi-file fetch for render_backtrace(): a backtrace can span
 * several source files at once (unlike PAUSED/LINES, which only ever need
 * one), so a single pending_source-style slot isn't enough - this instead
 * queues every file the frames need that isn't cached yet, fetches them
 * one at a time, and only actually prints once all of them have arrived. */
typedef struct {
	bool active;
	uc_value_t *payload;
	char **files;
	size_t nfiles, next;
} pending_backtrace_t;

static pending_backtrace_t pending_backtrace = { 0 };

static void
request_backtrace_file(int fd, const char *file)
{
	uc_value_t *payload = jobject_str("file", file);

	proto_write(fd, "SOURCE", payload);
	ucv_put(payload);
}

static void
render_backtrace_final(int fd, uc_value_t *p)
{
	uc_value_t *frames = jget(p, "frames");
	size_t i, n = ucv_array_length(frames);

	for (i = 0; i < n; i++) {
		uc_value_t *fr = ucv_array_get(frames, i);
		const char *file = jstr(fr, "file", NULL);
		int64_t line = jint(fr, "line", 0);
		int64_t col = jint(fr, "col", 0);
		bool native = !strcmp(jstr(fr, "kind", ""), "native");
		char signature[256];

		char prefix[16];
		size_t prefix_len, columns = term_columns();

		snprintf(signature, sizeof(signature), "%s()", jstr(fr, "function", "?"));

		/* "#N " is printed right before the header bar, on the same line -
		 * left_pad itself would make the bar draw *another* copy of that
		 * indentation (it is meant for a bar that draws its own leading
		 * blanks, see render_paused() above for that usage), so instead
		 * just shrink the width budget by the prefix that already went
		 * out via printf() below, and leave left_pad at 0. Without this,
		 * the bar is sized for the full terminal width and the combined
		 * line overflows it by exactly the prefix's length. */
		prefix_len = (size_t)snprintf(prefix, sizeof(prefix), "#%-2" PRId64 " ", jint(fr, "index", 0));
		columns = (columns > prefix_len) ? columns - prefix_len : 0;

		printf(C_BOLD "%s" C_RESET, prefix);

		debug_highlight_print_header_bar(stdout,
			native ? "C" : (file ? file : "?"), signature, 0, columns);

		if (!native && file && line > 0) {
			debug_highlight_span_t hl = {
				.from_line = (size_t)line, .from_col = 0,
				.to_line = (size_t)line, .to_col = SIZE_MAX,
				.have_ip = true, .ip_line = (size_t)line, .ip_col = col0(col)
			};

			render_source_lines(fd, file, line - 2, line + 2, &hl, 2);
		}

		/* tail call optimization reuses this frame for the callee, so the
		 * intermediate frames of the tail calls are absent from the stack -
		 * show the gap (see uc_vm_frame_reinit() in vm.c). */
		if (jint(fr, "tco", 0) > 0)
			printf(C_DIM "  (%" PRId64 " tail call frames omitted)" C_RESET "\n",
				jint(fr, "tco", 0));

		if (jhas(fr, "variables"))
			render_variables_array(jget(fr, "variables"), "     - ");

		printf("\n");
	}
}

static void
render_backtrace(int fd, uc_value_t *p)
{
	uc_value_t *frames = jget(p, "frames");
	size_t i, n = ucv_array_length(frames);
	char **missing = NULL;
	size_t n_missing = 0, cap = 0;

	for (i = 0; i < n; i++) {
		uc_value_t *fr = ucv_array_get(frames, i);
		const char *file = jstr(fr, "file", NULL);
		size_t dummy;
		size_t j;
		bool already = false;

		if (!file || strcmp(jstr(fr, "kind", ""), "script"))
			continue;

		if (find_cached_source(file, &dummy))
			continue;

		if (try_load_local_file(file, &dummy))
			continue;

		for (j = 0; j < n_missing; j++)
			if (!strcmp(missing[j], file))
				already = true;

		if (already)
			continue;

		if (n_missing >= cap) {
			cap = cap ? cap * 2 : 4;
			missing = realloc(missing, cap * sizeof(*missing));
		}

		missing[n_missing++] = strdup(file);
	}

	if (n_missing == 0) {
		free(missing);
		render_backtrace_final(fd, p);
		return;
	}

	pending_backtrace.active = true;
	pending_backtrace.payload = ucv_get(p);
	pending_backtrace.files = missing;
	pending_backtrace.nfiles = n_missing;
	pending_backtrace.next = 0;

	request_backtrace_file(fd, missing[0]);
}

static void
render_source_range(int fd, uc_value_t *p)
{
	const char *file = jstr(p, "file", NULL);
	uc_value_t *cursor = jget(p, "cursor");
	int64_t from = jint(p, "from", 0);
	int64_t to = jint(p, "to", 0);
	debug_highlight_span_t hl;

	if (!file) {
		printf("(no source range)\n");
		return;
	}

	if (cursor) {
		hl.from_line = (size_t)jint(cursor, "from_line", 0);
		hl.from_col = col0(jint(cursor, "from_col", 0));
		hl.to_line = (size_t)jint(cursor, "to_line", 0);
		hl.to_col = col0(jint(cursor, "to_col", 0));

		/* The protocol only gives us the statement's *span*, not the
		 * exact current instruction position within it (which can differ
		 * for multi-part expressions) - approximate with the span start,
		 * which is exact for the common case of a simple statement. */
		hl.have_ip = true;
		hl.ip_line = hl.from_line;
		hl.ip_col = hl.from_col;

		render_source_lines(fd, file, from, to, &hl, 0);
	}
	else {
		render_source_lines(fd, file, from, to, NULL, 0);
	}
}

/* Fill in the raw byte array fields of a debug_disasm_insn_t (or a
 * capture/unpack sub-entry) from a JSON array of small integers. `dst` must
 * already point at storage for at least `cap` bytes; only the first
 * min(array length, cap) entries are filled. */
static void
jbytes(uc_value_t *arr, unsigned char *dst, size_t cap)
{
	size_t n = ucv_array_length(arr);

	if (n > cap)
		n = cap;

	for (size_t i = 0; i < n; i++)
		dst[i] = (unsigned char)ucv_to_integer(ucv_array_get(arr, i));
}

static void
render_disassembly(uc_value_t *p)
{
	uc_value_t *insns_j = jget(p, "instructions");
	size_t n = ucv_array_length(insns_j);
	debug_disasm_insn_t *insns;

	insns = calloc(n, sizeof(*insns));

	for (size_t i = 0; i < n; i++) {
		uc_value_t *ins = ucv_array_get(insns_j, i);
		uc_value_t *bytes_j = jget(ins, "bytes");
		uc_value_t *captures_j = jget(ins, "captures");
		uc_value_t *unpacks_j = jget(ins, "unpacks");
		debug_disasm_insn_t *d = &insns[i];
		size_t nbytes = ucv_array_length(bytes_j);
		unsigned char *bytes = malloc(nbytes ? nbytes : 1);

		jbytes(bytes_j, bytes, nbytes);

		d->offset = (size_t)jint(ins, "offset", 0);
		d->mnemonic = jstr(ins, "mnemonic", "?");
		d->format = (int)jint(ins, "format", 0);
		d->bytes = bytes;
		d->nbytes = nbytes;
		d->operand = jint(ins, "operand", 0);

		if (jhas(ins, "constant")) {
			d->have_constant = true;
			d->constant_repr = ucv_to_jsonstring(NULL, jget(ins, "constant"));
			d->constant_is_string = (ucv_type(jget(ins, "constant")) == UC_STRING);
		}

		if (jhas(ins, "variable_kind")) {
			d->variable_kind = jstr(ins, "variable_kind", NULL);
			d->variable_name = jstr(ins, "variable_name", NULL);
		}

		if (jhas(ins, "closure_index")) {
			d->have_closure = true;
			d->closure_kind = jstr(ins, "closure_kind", "closure");
			d->closure_index = (uint32_t)jint(ins, "closure_index", 0);
		}

		if (jhas(ins, "call_nargs")) {
			d->have_call = true;
			d->call_mcall = ucv_is_truish(jget(ins, "call_mcall"));
			d->call_nargs = (uint32_t)jint(ins, "call_nargs", 0);
			d->call_tail = ucv_is_truish(jget(ins, "call_tail"));
			d->stdlib_name = jstr(ins, "stdlib_name", NULL);
		}

		if (jhas(ins, "return_tailcall"))
			d->return_tailcall = true;

		d->ncaptures = ucv_array_length(captures_j);
		d->captures = calloc(d->ncaptures ? d->ncaptures : 1, sizeof(*d->captures));

		for (size_t j = 0; j < d->ncaptures; j++) {
			uc_value_t *cap = ucv_array_get(captures_j, j);

			d->captures[j].slot = jint(cap, "slot", 0);
			d->captures[j].upval = !strcmp(jstr(cap, "kind", ""), "upval");
			d->captures[j].name = jstr(cap, "name", "(unknown)");
			jbytes(jget(cap, "bytes"), d->captures[j].bytes, 4);
		}

		d->nunpacks = ucv_array_length(unpacks_j);
		d->unpacks = calloc(d->nunpacks ? d->nunpacks : 1, sizeof(*d->unpacks));

		for (size_t j = 0; j < d->nunpacks; j++) {
			uc_value_t *u = ucv_array_get(unpacks_j, j);

			d->unpacks[j].slot = (uint16_t)jint(u, "slot", 0);
			jbytes(jget(u, "bytes"), d->unpacks[j].bytes, 2);
		}
	}

	debug_highlight_print_disassembly(stdout, jstr(p, "function", "?"), insns, n, term_columns());

	for (size_t i = 0; i < n; i++) {
		free((void *)insns[i].bytes);
		free((void *)insns[i].constant_repr);
		free(insns[i].captures);
		free(insns[i].unpacks);
	}

	free(insns);
}

/* Async server events (see EVENT in lib/debug_proto.h) can land at any
 * time, unprompted by anything the user typed - set in a faint italic
 * style to visually set them apart from direct command responses. */
static void
render_event(uc_value_t *p)
{
	const char *event = jstr(p, "event", "?");

	printf(C_EVENT);

	if (!strcmp(event, "exception")) {
		uc_value_t *exc = jget(p, "exception");

		printf("*** exception: %s: %s ***", jstr(exc, "type", "Error"),
			jstr(exc, "message", "?"));
	}
	else if (!strcmp(event, "exit")) {
		const char *status = jstr(p, "status", "?");

		if (!strcmp(status, "OK")) {
			printf("*** program finished ***");
		}
		else if (!strcmp(status, "EXIT")) {
			printf("*** program exited (code %" PRId64 ") ***", jint(p, "code", 0));
		}
		else {
			uc_value_t *exc = jget(p, "exception");

			if (exc)
				printf("*** program terminated: %s: %s ***",
					jstr(exc, "type", "Error"), jstr(exc, "message", "?"));
			else
				printf("*** program terminated (%s) ***", status);
		}
	}
	else {
		char *json = ucv_to_jsonstring(NULL, p);

		printf("*** event: %s %s ***", event, json);
		free(json);
	}

	printf(C_RESET "\n");
}

static void
render_response(int fd, const char *verb, uc_value_t *payload)
{
	if (!strcmp(verb, "PAUSED"))
		render_paused(fd, payload);
	else if (!strcmp(verb, "BREAKPOINTS"))
		render_breakpoints(payload);
	else if (!strcmp(verb, "VARIABLES"))
		render_variables_array(jget(payload, "vars"), "");
	else if (!strcmp(verb, "BACKTRACE"))
		render_backtrace(fd, payload);
	else if (!strcmp(verb, "SOURCE_RANGE"))
		render_source_range(fd, payload);
	else if (!strcmp(verb, "DISASSEMBLY"))
		render_disassembly(payload);
	else if (!strcmp(verb, "ERROR"))
		printf(C_RED "Error: %s" C_RESET "\n", jstr(payload, "message", "(unknown error)"));
	else if (!strcmp(verb, "VALUE"))
		printf("%s\n", jstr(payload, "repr", ""));
	else if (!strcmp(verb, "BREAKPOINT_ADDED"))
		printf(C_GREEN "Breakpoint #%" PRId64 " added" C_RESET "\n", jint(payload, "id", 0));
	else if (!strcmp(verb, "EVENT"))
		render_event(payload);
	else if (!strcmp(verb, "SOURCE")) {
		const char *text = jstr(payload, "text", NULL);
		const char *file = jstr(payload, "file", "?");

		if (text) {
			size_t nlines;

			cache_source_text(file, text, &nlines);

			/* A render_backtrace() multi-file fetch takes priority: advance
			 * its queue and either request the next missing file or, once
			 * every frame's file is cached, finally print the whole thing. */
			if (pending_backtrace.active && pending_backtrace.next < pending_backtrace.nfiles &&
			    !strcmp(pending_backtrace.files[pending_backtrace.next], file)) {
				pending_backtrace.next++;

				if (pending_backtrace.next < pending_backtrace.nfiles) {
					request_backtrace_file(fd, pending_backtrace.files[pending_backtrace.next]);
				}
				else {
					size_t i;

					render_backtrace_final(fd, pending_backtrace.payload);
					ucv_put(pending_backtrace.payload);

					for (i = 0; i < pending_backtrace.nfiles; i++)
						free(pending_backtrace.files[i]);

					free(pending_backtrace.files);
					pending_backtrace = (pending_backtrace_t){ 0 };
				}
			}
			/* Finishing an auto-fetch triggered by render_paused()/
			 * render_source_range() (see pending_source) is distinct from
			 * a direct response to a user-typed "source <file>" command:
			 * the former re-renders exactly the range that was originally
			 * requested, the latter shows the whole file. */
			else if (pending_source.active && !strcmp(pending_source.file, file)) {
				int64_t from = pending_source.from;
				int64_t to = pending_source.to;
				bool have_hl = pending_source.have_hl;
				debug_highlight_span_t hl = pending_source.hl;
				size_t left_pad = pending_source.left_pad;

				pending_source.active = false;
				render_source_lines(fd, file, from, to, have_hl ? &hl : NULL, left_pad);
			}
			else {
				printf("--- %s ---\n", file);
				render_source_lines(fd, file, 1, (int64_t)nlines, NULL, 0);
			}
		}
		else {
			printf("(source unavailable: %s)\n", jstr(payload, "error", "?"));
			pending_source.active = false;

			if (pending_backtrace.active) {
				size_t i;

				/* Missing source for one frame shouldn't block showing the
				 * rest - just print what we have (unavailable files will
				 * fall back to "no snippet" for that frame). */
				render_backtrace_final(fd, pending_backtrace.payload);
				ucv_put(pending_backtrace.payload);

				for (i = 0; i < pending_backtrace.nfiles; i++)
					free(pending_backtrace.files[i]);

				free(pending_backtrace.files);
				pending_backtrace = (pending_backtrace_t){ 0 };
			}
		}
	}
	else if (!strcmp(verb, "HELP")) {
		uc_value_t *cmds = jget(payload, "commands");
		size_t i, n = ucv_array_length(cmds);

		for (i = 0; i < n; i++) {
			uc_value_t *c = ucv_array_get(cmds, i);

			printf("%-16s %s\n", jstr(c, "verb", "?"), jstr(c, "help", ""));
		}
	}
	else if (!strcmp(verb, "SOURCES")) {
		uc_value_t *items = jget(payload, "items");
		size_t i, n = ucv_array_length(items);

		for (i = 0; i < n; i++) {
			uc_value_t *it = ucv_array_get(items, i);

			printf("#%-2" PRId64 " %s\n", jint(it, "index", 0), jstr(it, "file", "?"));
		}
	}
	else if (!strcmp(verb, "OK")) {
		printf("OK\n");
	}
	else if (!strcmp(verb, "RESUME")) {
		printf("(resumed)\n");
	}
	else if (payload) {
		char *json = ucv_to_jsonstring(NULL, payload);

		printf("%s %s\n", verb, json);
		free(json);
	}
	else {
		printf("%s\n", verb);
	}
}

/* -- typed command line -> VERB {payload} translation -------------------- */

static char *
trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;

	end = s + strlen(s);

	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';

	return s;
}

/* Split off the first whitespace-delimited word from *rest, returning it and
 * advancing *rest to the remainder (leading space trimmed). */
static char *
shift_word(char **rest)
{
	char *p = *rest;
	char *word;

	while (isspace((unsigned char)*p))
		p++;

	word = p;

	while (*p && !isspace((unsigned char)*p))
		p++;

	if (*p) {
		*p = '\0';
		p++;

		while (isspace((unsigned char)*p))
			p++;
	}

	*rest = p;

	return word;
}

/* True if `typed` is a non-empty prefix of any of the NUL-separated names
 * in `names` (e.g. "list\0ls\0") - shortest-unique-prefix command matching,
 * same as the original interactive CLI's `commands[]` dispatch. Ambiguous
 * prefixes (matching more than one command) resolve to whichever command
 * is checked first below, in the same fixed order the original table
 * declared them in. */
static bool
match_cmd(const char *names, const char *typed)
{
	size_t typed_len = strlen(typed);
	const char *p = names;

	if (typed_len == 0)
		return false;

	while (*p) {
		size_t len = strlen(p);

		if (len >= typed_len && !strncmp(p, typed, typed_len))
			return true;

		p += len + 1;
	}

	return false;
}

/* CLI usage documentation, ported verbatim from the pre-protocol interactive
 * debugger's `commands[]`/cmd_help() (formerly lib/debug.c) - this describes
 * *this client's* typed command syntax, so unlike everything else in this
 * file it is never fetched from the server: the server's own HELP verb
 * answers a different question (the wire protocol's verbs and payload
 * shapes, for anything else that might speak the protocol directly) and
 * showing that to an interactive user here just reads as raw protocol
 * internals. `names` is a NUL-separated list of aliases, primary name
 * first - match_cmd() already implements the exact prefix-matching lookup
 * this needs, so it is reused here for `help <partial-name>` filtering. */
static const struct {
	const char *names;
	const char *help;
} cli_help_table[] = {
	{ "help\0h\0?\0",
		"Print help information." },
	{ "break\0b\0",
		"The break command sets a breakpoint at the given location, "
		"instructing the virtual machine to stop execution at this "
		"point and handing control to the debugger.\n\n"
		"Breakpoint locations may be specified either as filename, "
		"line number and optional character offset within the line "
		"or as a ucode expression that evaluates to a function in "
		"which a breakpoint is set.\n\n"
		"Examples:\n"
		"  break example.uc:13  # Set breakpoint in line 13 of example.uc\n"
		"  break 4:17           # Break in line in 4, char 17 of current file\n"
		"  break myobj.method   # Break in function `method` of `myobj`\n"
		"  break (string.uc)    # Parens to disambiguate expression from path"
	},
	{ "delete\0d\0",
		"Delete a breakpoint. When no argument is given, the current "
		"breakpoint is deleted, otherwise this function deletes the breakpoint "
		"with the given index.\n\n"
		"Examples:\n"
		"  delete               # Delete current breakpoint\n"
		"  delete 2             # Delete breakpoint #2"
	},
	{ "list\0ls\0",
		"List all currently set breakpoints. User defined breakpoints are "
		"prefixed with a number identifying the breakpoint, internal "
		"breakpoints used by the debugger are prefixed with a breakpoint type "
		"enclosed in parens, e.g. '(step)'."
	},
	{ "next\0n\0",
		"Execute the next statement and stop again."
	},
	{ "step\0s\0",
		"Execute the next statement, in case of function calls step into the "
		"called function and stop there."
	},
	{ "continue\0c\0",
		"Continue execution until the next breakpoint or end of program."
	},
	{ "return\0",
		"Continue executing the current function until it returns, then stop "
		"in the calling function. If the current function is the program entry "
		"function, then run until the end of the program."
	},
	{ "backtrace\0bt\0",
		"Print a trace of the current callstack, with most recent callframes "
		"output first. If the optional 'full' argument is specified, "
		"additional information about each call frame is printed.\n\n"
		"Examples:\n"
		"  backtrace            # Print backtrace\n"
		"  backtrace full       # Print backtrace with additional information"
	},
	{ "variables\0vars\0",
		"Print local variables and their contents for the current execution "
		"context. Internal variables which are unreachable by script code "
		"are shown faint, upvalues (variables captured from parent scopes) "
		"are shown in bold cyan and ordinary variables use the default color.\n\n"
		"Examples:\n"
		"  variables            # Print local variables"
	},
	{ "sources\0src\0",
		"Print a list of loaded source buffers."
	},
	{ "print\0p\0",
		"Evaluate an ucode expression and print the resulting value - like "
		"the ucode CLI's `-p`.\n\n"
		"Examples:\n"
		"  print varname        # Print value of variable 'varname'\n"
		"  print myobj.prop     # Print `prop` property of `myobj`\n"
		"  print keys(myobj)    # Invoke a stdlib function"
	},
	{ "eval\0e\0",
		"Evaluate an ucode expression, discarding its result instead of "
		"printing it - like the ucode CLI's `-e`. The idiomatic way to "
		"change a variable's value while paused: assignment is just "
		"ordinary expression syntax, so a plain variable, a property path "
		"or an array index all work the same way a script would write "
		"them, without a separate dedicated command for it.\n\n"
		"Examples:\n"
		"  eval x = 5            # Assign the number 5 to variable 'x'\n"
		"  eval x.y = 1          # Assign 1 to property 'y' of 'x'\n"
		"  eval delete foo.bar   # Delete property 'bar' of 'foo'"
	},
	{ "lines\0ln\0",
		"Print source code lines surrounding the given location specified "
		"either as filename with line number or as expression evaluating to a "
		"function value.\n\n"
		"The amount of preceding and following lines to print may be "
		"specified as second and third argument respectively. By default, two "
		"lines of context are printed before and after the location.\n\n"
		"Examples:\n"
		"  lines                # Output lines surrounding current line\n"
		"  lines example.uc     # Print first three lines of example.uc\n"
		"  lines (obj.func)     # Parens to disambiguate expression from path\n"
		"  lines foo 5 8        # Print 5 lines before foo() till 8 lines in\n"
		"  lines #123           # Print source of instruction offset 123\n"
		"  lines +0 3 3         # Print 3 lines before and after current line\n"
		"  lines -5             # Print source 5 lines before current line\n"
		"  lines +3             # Print source 3 lines after current line"
	},
	{ "throw\0",
		"Raise an exception at the current instruction offset.\n\n"
		"Examples:\n"
		"  throw \"Message\"    # Throw exception with given message"
	},
	{ "disassemble\0disasm\0",
		"Disassemble the given function or statement location and output the "
		"corresponding byte code in a human readable manner. The location to "
		"disassemble may be either a function name, a single instruction "
		"offset, an instruction offset range or a ucode expression.\n\n"
		"Examples:\n"
		"  disassemble          # Disassemble current statment\n"
		"  disassemble foo      # Disassemble body of foo()\n"
		"  disassemble foo+100  # Disassemble first 100 byte of function foo()\n"
		"  disassemble #5       # Disassemble statement containing instruction 5\n"
		"  disassemble #2-10    # Disassemble instructions 2 to 10\n"
		"  disassemble #22+100  # Disassemble instructions 22 to 122\n"
		"  disassemble (12/3*4) # Disassemble ucode expression"
	},
	{ "source\0",
		"Fetch and print the raw source text the server has for a file path, "
		"without syntax highlighting - mostly useful to check exactly what "
		"the server sees when it differs from the local copy."
	},
	{ "quit\0q\0",
		"Forcibly terminate the currently running program. The termination "
		"happens in the same manner as if 'exit()' has been called from "
		"script code."
	},
};

/* Word-wrap and print one help entry's body to `columns`, preserving
 * existing line breaks (so an "Examples:" block's indentation survives)
 * and paragraph gaps - ported verbatim from cmd_help(), formerly
 * lib/debug.c, with term_printf()/term_print() replaced by printf(). */
static void
print_help_entry(const char *names, const char *help, size_t columns)
{
	const char *p = help;

	printf(C_BOLD "%s" C_RESET "\n\n", names);

	while (*p != '\0') {
		size_t pad = strspn(p, " ");
		size_t len = strcspn(p, "\r\n") - pad;

		if (pad + len <= columns) {
			printf("%.*s\n", (int)(pad + len), p);
			p += pad + len + (p[pad + len] == '\n');
		}
		else {
			if (pad > columns)
				pad = 1;

			const char *l = p + pad;

			while (len > columns - pad) {
				printf("%.*s", (int)pad, p);

				for (size_t j = columns - pad; j > 0; j--) {
					if (l[j - 1] == ' ') {
						printf("%.*s\n", (int)j, l);
						l += j;
						len -= j;
						break;
					}
				}
			}

			printf("%.*s", (int)pad, p);
			printf("%.*s\n", (int)len, l);
			p = l + len + (l[len] == '\n');
		}
	}

	printf("\n\n");
}

static void
print_help(const char *cmd)
{
	size_t columns = term_columns();
	size_t n = sizeof(cli_help_table) / sizeof(cli_help_table[0]);

	for (size_t i = 0; i < n; i++) {
		if (cmd && *cmd && !match_cmd(cli_help_table[i].names, cmd))
			continue;

		print_help_entry(cli_help_table[i].names, cli_help_table[i].help, columns);
	}
}

static bool
send_command(int fd, char *line, bool *resuming, bool *sent)
{
	char *cmd = shift_word(&line);
	uc_value_t *payload = NULL;

	*resuming = false;
	*sent = true;

	if (!*cmd) {
		*sent = false;
		return true;
	}

	if (match_cmd("help\0h\0?\0", cmd)) {
		print_help(*line ? line : NULL);
		*sent = false;
	}
	else if (match_cmd("break\0b\0", cmd)) {
		payload = jobject_str("spec", line);
		proto_write(fd, "BREAK", payload);
	}
	else if (match_cmd("delete\0d\0", cmd)) {
		if (*line) {
			payload = ucv_object_new(NULL);
			ucv_object_add(payload, "id", ucv_int64_new(strtoll(line, NULL, 10)));
		}

		proto_write(fd, "DELETE", payload);
	}
	else if (match_cmd("list\0ls\0", cmd)) {
		proto_write(fd, "LIST_BREAKPOINTS", NULL);
	}
	else if (match_cmd("next\0n\0", cmd)) {
		proto_write(fd, "NEXT", NULL);
		*resuming = true;
	}
	else if (match_cmd("step\0s\0", cmd)) {
		proto_write(fd, "STEP", NULL);
		*resuming = true;
	}
	else if (match_cmd("continue\0c\0", cmd)) {
		proto_write(fd, "CONTINUE", NULL);
		*resuming = true;
	}
	else if (match_cmd("return\0", cmd)) {
		proto_write(fd, "RETURN", NULL);
		*resuming = true;
	}
	else if (match_cmd("backtrace\0bt\0", cmd)) {
		payload = ucv_object_new(NULL);
		ucv_object_add(payload, "full", ucv_boolean_new(!strcmp(trim(line), "full")));
		proto_write(fd, "BACKTRACE", payload);
	}
	else if (match_cmd("variables\0vars\0", cmd)) {
		proto_write(fd, "VARIABLES", NULL);
	}
	else if (match_cmd("sources\0src\0", cmd)) {
		proto_write(fd, "SOURCES", NULL);
	}
	else if (match_cmd("print\0p\0", cmd)) {
		payload = jobject_str("expr", line);
		proto_write(fd, "PRINT", payload);
	}
	else if (match_cmd("eval\0e\0", cmd)) {
		payload = jobject_str("expr", line);
		proto_write(fd, "EVAL", payload);
	}
	else if (match_cmd("lines\0ln\0", cmd)) {
		char *spec = shift_word(&line);
		char *before = shift_word(&line);
		char *after = shift_word(&line);

		payload = ucv_object_new(NULL);

		if (*spec)
			ucv_object_add(payload, "spec", ucv_string_new(spec));

		if (*before)
			ucv_object_add(payload, "before", ucv_int64_new(strtoll(before, NULL, 10)));

		if (*after)
			ucv_object_add(payload, "after", ucv_int64_new(strtoll(after, NULL, 10)));

		proto_write(fd, "LINES", payload);
	}
	else if (match_cmd("throw\0", cmd)) {
		char *first = shift_word(&line);
		static const char *types[] = {
			"syntax", "runtime", "type", "reference", "user", "exit"
		};
		size_t i;
		bool is_type = false;

		for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
			if (!strncmp(types[i], first, strlen(first))) {
				is_type = true;
				break;
			}
		}

		payload = ucv_object_new(NULL);

		if (is_type && *line) {
			ucv_object_add(payload, "type", ucv_string_new(first));
			ucv_object_add(payload, "message", ucv_string_new(line));
		}
		else {
			char *msg = *line ? line : first;

			ucv_object_add(payload, "message", ucv_string_new(msg));
		}

		proto_write(fd, "THROW", payload);
	}
	else if (match_cmd("disassemble\0disasm\0", cmd)) {
		if (*line) {
			payload = jobject_str("spec", line);
		}

		proto_write(fd, "DISASSEMBLE", payload);
	}
	else if (match_cmd("source\0", cmd)) {
		payload = jobject_str("file", line);
		proto_write(fd, "SOURCE", payload);
	}
	else if (match_cmd("quit\0q\0", cmd)) {
		bool force = !strcmp(trim(line), "-f");

		if (!force && isatty(STDIN_FILENO)) {
			char confirm[16];

			/* This wants a plain, cooked-mode, blocking fgets() prompt of
			 * its own - drop out of lineedit's raw/non-blocking mode for
			 * it, then re-engage before returning. */
			lineedit_suspend();

			printf("Terminate program? (y/n) > ");
			fflush(stdout);

			bool confirmed = fgets(confirm, sizeof(confirm), stdin) &&
				tolower((unsigned char)confirm[0]) == 'y';

			lineedit_resume();

			if (!confirmed) {
				*sent = false;
				return true;
			}
		}

		proto_write(fd, "QUIT", NULL);
		return false;
	}
	else {
		printf("Unrecognized command '%s' (try 'help')\n", cmd);
		*sent = false;
	}

	return true;
}

/* -- connection setup ----------------------------------------------------- */

static int
connect_socket(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);

		return -1;
	}

	return fd;
}

static char *
get_socket_path_for_pid(pid_t pid)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/ucode-debug-%d.sock", DEFAULT_SOCKET_DIR, pid);

	return path;
}

static int
wait_for_socket(const char *path, int timeout_sec)
{
	int elapsed = 0;
	struct stat st;

	while (elapsed < timeout_sec) {
		if (stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFSOCK)
			return 0;

		sleep(1);
		elapsed++;
	}

	return -1;
}

/* The debuggee's PID, for Ctrl-C-while-running (see maybe_send_interrupt()
 * below) - resolved once right after connecting, however that happened
 * (explicit <pid>, a socket path, or an inherited --fd), via SO_PEERCRED:
 * works uniformly for all three, since all of them are - or, for --fd,
 * were, at the moment the debuggee created it and only then forked - a
 * connected AF_UNIX socket. -1 if this somehow couldn't be determined
 * (Ctrl-C-while-running is then a no-op; everything else about the
 * session is unaffected). */
static pid_t debuggee_pid = -1;

static void
resolve_debuggee_pid(int fd)
{
#if defined(__linux__)
	struct ucred cred;
	socklen_t len = sizeof(cred);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
		debuggee_pid = cred.pid;
#elif defined(__APPLE__)
	pid_t pid;
	socklen_t len = sizeof(pid);

	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0)
		debuggee_pid = pid;
#endif
}

static void
print_usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-s DIR] <pid>\n", prog);
	fprintf(stderr, "       %s [-s DIR] <socket-path>\n", prog);
	fprintf(stderr, "       %s [-s DIR] --fd <n>\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "Debugger client for ucode, speaking the line-based debug protocol.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  <pid>           SIGUSR1-attach to a running `-X` process, gdb -p style.\n");
	fprintf(stderr, "  <socket-path>   connect to an explicit debug.listen(path) socket.\n");
	fprintf(stderr, "  --fd <n>        use an already-connected fd (internal, used by `-x`).\n");
	fprintf(stderr, "  -s, --srcdir DIR\n");
	fprintf(stderr, "                  Local directory to also look for source files under\n");
	fprintf(stderr, "                  (by basename) when the path the server reports doesn't\n");
	fprintf(stderr, "                  exist as-is on this machine - e.g. the target runs on a\n");
	fprintf(stderr, "                  different host/root than this checkout. Source is always\n");
	fprintf(stderr, "                  tried locally first (at the server's exact reported path)\n");
	fprintf(stderr, "                  before ever asking the server for it.\n");
}

int
main(int argc, char **argv)
{
	int fd;
	fd_set readfds;
	char buf[MAX_LINE];
	linebuf_t lb = { 0 };

	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	debug_highlight_init();

	{
		size_t n = sizeof(cli_help_table) / sizeof(cli_help_table[0]);
		static lineedit_completion_t comps[sizeof(cli_help_table) / sizeof(cli_help_table[0])];

		for (size_t i = 0; i < n; i++)
			comps[i].names = cli_help_table[i].names;

		lineedit_set_completions(comps, n);
	}

	lineedit_init();

	/* Pull -s/--srcdir DIR out of argv wherever it appears, leaving the
	 * rest of argument parsing below untouched. */
	{
		int ai = 1;

		while (ai < argc) {
			if (!strcmp(argv[ai], "-s") || !strcmp(argv[ai], "--srcdir")) {
				if (ai + 1 >= argc) {
					print_usage(argv[0]);

					return 1;
				}

				opt_srcdir = argv[ai + 1];
				memmove(&argv[ai], &argv[ai + 2], (size_t)(argc - ai - 2) * sizeof(char *));
				argc -= 2;

				continue;
			}

			ai++;
		}
	}

	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		print_usage(argv[0]);

		return (argc < 2) ? 1 : 0;
	}

	if (!strcmp(argv[1], "--fd")) {
		if (argc < 3) {
			print_usage(argv[0]);

			return 1;
		}

		fd = atoi(argv[2]);
	}
	else if (strchr(argv[1], '/')) {
		fd = connect_socket(argv[1]);

		if (fd < 0) {
			fprintf(stderr, "Failed to connect to %s: %s\n", argv[1], strerror(errno));

			return 1;
		}
	}
	else {
		pid_t pid = atoi(argv[1]);
		char *socket_path;
		struct stat st;

		if (pid <= 0) {
			fprintf(stderr, "Invalid PID: %s\n", argv[1]);

			return 1;
		}

		socket_path = get_socket_path_for_pid(pid);

		/* If the attach socket already exists, the target already has a
		 * session waiting (e.g. `-X <expr>`/debug.attach()) - just connect.
		 * Only send SIGUSR1 for the classic bare `-X` flow, where nothing is
		 * listening yet until asked to. */
		if (stat(socket_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
			fprintf(stderr, "Debugger socket already present, connecting...\n");
		}
		else {
			if (kill(pid, SIGUSR1) < 0) {
				fprintf(stderr, "Failed to send SIGUSR1 to process %d: %s\n", pid, strerror(errno));

				return 1;
			}

			fprintf(stderr, "Sent SIGUSR1 to process %d, waiting for debugger socket...\n", pid);

			if (wait_for_socket(socket_path, MAX_WAIT_TIME) < 0) {
				fprintf(stderr, "Timeout waiting for debugger socket at %s\n", socket_path);

				return 1;
			}
		}

		fd = connect_socket(socket_path);

		if (fd < 0) {
			fprintf(stderr, "Failed to connect to %s: %s\n", socket_path, strerror(errno));

			return 1;
		}
	}

	resolve_debuggee_pid(fd);

	fprintf(stderr, "Connected to ucode debugger\n\n");

	bool stdin_done = false;
	/* True whenever the session is sitting at a PAUSED prompt waiting for
	 * a command - i.e. exactly when a "dbg > " prompt should be visible.
	 * Cleared the instant a resuming command (next/step/continue/return)
	 * is sent, since there is no synchronous ack for those (see
	 * lib/debug_proto.h) - the prompt only comes back once a new PAUSED
	 * (or the connection closing) says so. */
	bool paused = false;
	/* True from the moment any command is sent until its response has
	 * actually been drained and rendered - keeps the prompt from
	 * reappearing (and racing ahead of) a response that just hasn't
	 * arrived over the socket yet. */
	bool awaiting_response = false;
	/* Tracks whether lineedit_begin() has already been called for the
	 * current accepting_input span, so the prompt (and a fresh, empty
	 * edit line) is (re)started exactly once per command, not on every
	 * select() wakeup while still mid-edit. */
	bool prompt_shown = false;

	for (;;) {
		char *verb;
		uc_value_t *payload;

		while (linebuf_pop(&lb, &verb, &payload)) {
			render_response(fd, verb, payload);
			awaiting_response = false;

			if (!strcmp(verb, "PAUSED"))
				paused = true;
			else if (!strcmp(verb, "RESUME") || !strcmp(verb, "EVENT"))
				paused = false;

			free(verb);
			ucv_put(payload);
		}

		/* Only accept (and select on) stdin for actual command input while
		 * sitting at a prompt: gating this on the exact same condition
		 * that shows the prompt is what stops a command from racing ahead
		 * of - and getting interleaved with - the connection's own
		 * initial PAUSED message or a still-in-flight response to a
		 * previous command. */
		bool accepting_input = paused && !stdin_done && !awaiting_response
			&& !pending_source.active && !pending_backtrace.active;

		if (!accepting_input)
			prompt_shown = false;
		else if (!prompt_shown) {
			lineedit_begin("dbg > ");
			prompt_shown = true;
		}

		FD_ZERO(&readfds);

		/* Outside of accepting_input, stdin is still watched (whenever
		 * raw-mode editing is active, i.e. a real terminal - piped/
		 * scripted input has no Ctrl-C to speak of) purely to catch
		 * Ctrl-C-while-running: see the interrupt handling below. */
		if (accepting_input || (lineedit_active() && !stdin_done))
			FD_SET(STDIN_FILENO, &readfds);

		FD_SET(fd, &readfds);

		if (select(fd + 1, &readfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;

			break;
		}

		if (FD_ISSET(fd, &readfds)) {
			ssize_t n = read(fd, buf, sizeof(buf));

			if (n <= 0) {
				printf("\nConnection closed\n");
				break;
			}

			linebuf_append(&lb, buf, (size_t)n);
		}

		if (!stdin_done && FD_ISSET(STDIN_FILENO, &readfds) && !accepting_input) {
			/* Not sitting at a prompt (the debuggee is running) - stdin is
			 * only being watched here for Ctrl-C, not full line editing;
			 * anything else typed while running had no effect before this
			 * feature existed either, so it's simply discarded rather
			 * than queued up to confuse the next prompt. lineedit's raw
			 * mode (a prerequisite for even reaching this branch, see the
			 * FD_SET above) already made stdin non-blocking. */
			char ibuf[64];
			ssize_t n = read(STDIN_FILENO, ibuf, sizeof(ibuf));

			for (ssize_t i = 0; i < n; i++) {
				if (ibuf[i] == 3 /* Ctrl-C */ && debuggee_pid > 0) {
					kill(debuggee_pid, SIGUSR1);
					break;
				}
			}
		}
		else if (!stdin_done && FD_ISSET(STDIN_FILENO, &readfds)) {
			bool eof = false;

			if (lineedit_feed(buf, sizeof(buf), &eof)) {
				prompt_shown = false;

				if (*trim(buf)) {
					bool resuming, sent;
					bool keep_going = send_command(fd, trim(buf), &resuming, &sent);

					/* An unrecognized/empty command (or "quit" declined at its
					 * confirmation prompt) never reaches the server, so there
					 * is no response to wait for - re-show the prompt right
					 * away instead of waiting forever for one that isn't
					 * coming. */
					awaiting_response = sent;

					if (resuming)
						paused = false;

					if (!keep_going) {
						/* QUIT was sent - keep looping (without reading
						 * further stdin) to drain and render any trailing
						 * responses (e.g. a final EVENT exit) until the
						 * server closes the connection, instead of exiting
						 * immediately and losing output that was already in
						 * flight. */
						stdin_done = true;
					}
				}
			}
			else if (eof) {
				proto_write(fd, "QUIT", NULL);
				stdin_done = true;
			}
		}
	}

	close(fd);

	return 0;
}

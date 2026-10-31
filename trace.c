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

/* ucode-trace: resolves the stack traces that native programs built without
 * debug information print, using the map file written at build time. */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "ucode/internal/native.h"

int
main(int argc, char **argv)
{
	FILE *in = stdin;
	int rv;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s MAPFILE [LOGFILE]\n", argv[0]);

		return 1;
	}

	if (argc == 3) {
		in = fopen(argv[2], "r");

		if (!in) {
			fprintf(stderr, "Unable to open %s: %s\n", argv[2], strerror(errno));

			return 1;
		}
	}

	rv = uc_native_trace_decode(argv[1], in, stdout);

	if (in != stdin)
		fclose(in);

	return rv;
}

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

/* Internal library / stdlib entry points. */

#ifndef UCODE_INTERNAL_LIB_H
#define UCODE_INTERNAL_LIB_H

#include "ucode/lib.h"
#include "ucode/internal/util.h"

__hidden extern const uc_function_list_t uc_stdlib_functions[];

/* Cross-library: called by modules (e.g. debug) from libucode, so it must stay exported. */
bool uc_source_context_format(uc_stringbuf_t *buf, uc_source_t *src, size_t off, bool compact);

__hidden bool uc_error_context_format(uc_stringbuf_t *buf, uc_source_t *src,
	uc_value_t *stacktrace, size_t startoff, size_t endoff, size_t off);
__hidden void uc_error_message_indent(char **msg);

__hidden uc_value_t *uc_require_library(uc_vm_t *vm, uc_value_t *nameval, bool module_mode);

#endif /* UCODE_INTERNAL_LIB_H */

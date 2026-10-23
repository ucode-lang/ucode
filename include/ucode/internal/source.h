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

#ifndef UCODE_INTERNAL_SOURCE_H
#define UCODE_INTERNAL_SOURCE_H

#include "ucode/source.h"
#include "ucode/internal/types.h"
#include "ucode/internal/util.h"

__hidden uc_source_t *uc_source_new_path(const char *path);

__hidden void uc_source_free(uc_source_t *source);

__hidden const uint8_t *uc_source_read_block(uc_source_t *source, size_t *len,
                                             size_t *off);

__hidden size_t uc_source_read_line(uc_source_t *source, size_t off, char *line,
                                    size_t size, size_t *lpos, bool *truncated);

char *uc_source_read_content(uc_source_t *source, size_t *len);

__hidden size_t uc_source_read(uc_source_t *source, void *buf, size_t len);

__hidden size_t uc_source_offset(uc_source_t *source);
__hidden size_t uc_source_remaining(uc_source_t *source);

__hidden void uc_source_reset(uc_source_t *source);
__hidden size_t uc_source_read_at(uc_source_t *source, size_t off, void *buf, size_t len);
__hidden void uc_source_resync(uc_source_t *source);

__hidden uc_source_type_t uc_source_begin(uc_source_t *source);

__hidden void uc_source_line_next(uc_source_t *source);
__hidden void uc_source_line_update(uc_source_t *source, size_t off);

__hidden void uc_source_runpath_set(uc_source_t *source, const char *runpath);

__hidden bool uc_source_export_add(uc_source_t *source, uc_value_t *name);
__hidden ssize_t uc_source_export_lookup(uc_source_t *source, uc_value_t *name);

__hidden char *uc_source_callee_expr(uc_source_t *source, size_t end);

#endif /* UCODE_INTERNAL_SOURCE_H */

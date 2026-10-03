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

#ifndef UCODE_CHUNK_H
#define UCODE_CHUNK_H

#include <stdint.h>
#include <stddef.h>

#include "ucode/internal/vallist.h"
#include "ucode/internal/util.h"
#include "ucode/internal/types.h"

__hidden void uc_chunk_init(uc_chunk_t *chunk);
__hidden void uc_chunk_free(uc_chunk_t *chunk);
__hidden size_t uc_chunk_add(uc_chunk_t *chunk, uint8_t byte, size_t offset);

__hidden void uc_chunk_pop(uc_chunk_t *chunk);

__hidden void uc_chunk_stmt_start(uc_chunk_t *chunk, size_t offset);
__hidden void uc_chunk_stmt_end(uc_chunk_t *chunk, size_t offset);

__hidden bool uc_chunk_debug_stmt_bounds(uc_chunk_t *chunk, size_t off,
                                         size_t *startoff, size_t *endoff);

size_t uc_chunk_debug_get_srcpos(uc_chunk_t *chunk, size_t offset);
__hidden void uc_chunk_debug_add_variable(uc_chunk_t *chunk, size_t from, size_t to, size_t slot, bool upval, uc_value_t *name);
uc_value_t *uc_chunk_debug_get_variable(uc_chunk_t *chunk, size_t offset, size_t slot, bool upval);

#endif /* UCODE_CHUNK_H */

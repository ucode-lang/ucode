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

#ifndef UCODE_JSON_H
#define UCODE_JSON_H

#include <stddef.h>

#include "ucode/types.h"

typedef enum {
	UC_JSON_SUCCESS,
	UC_JSON_CONTINUE,
	UC_JSON_ERROR_DEPTH,
	UC_JSON_ERROR_EOF,
	UC_JSON_ERROR_UNEXPECTED,
	UC_JSON_ERROR_NULL,
	UC_JSON_ERROR_BOOLEAN,
	UC_JSON_ERROR_NUMBER,
	UC_JSON_ERROR_ARRAY,
	UC_JSON_ERROR_KEY_NAME,
	UC_JSON_ERROR_KEY_SEP,
	UC_JSON_ERROR_VALUE_SEP,
	UC_JSON_ERROR_STRING,
	UC_JSON_ERROR_COMMENT,
} uc_json_error_t;

typedef struct uc_json_parser uc_json_parser_t;

uc_json_parser_t *uc_json_parser_new(uc_vm_t *vm);
void uc_json_parser_free(uc_json_parser_t *parser);

/* Parses `len` more bytes of input. A value is complete when
 * uc_json_parser_error() is UC_JSON_SUCCESS afterwards; the call then
 * returns it, and the parser starts over with the next one. Otherwise it
 * returns NULL, and the parser waits for more input (UC_JSON_CONTINUE) or
 * failed. */
uc_value_t *uc_json_parse(uc_json_parser_t *parser, const char *buf, size_t len);
uc_json_error_t uc_json_parser_error(uc_json_parser_t *parser);

/* the number of input bytes the last call consumed */
size_t uc_json_parser_end(uc_json_parser_t *parser);

const char *uc_json_error_desc(uc_json_error_t err);

#endif /* UCODE_JSON_H */

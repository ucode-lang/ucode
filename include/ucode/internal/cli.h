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

/* Command line options shared by the ucode CLI and native programs. */

#ifndef UCODE_INTERNAL_CLI_H
#define UCODE_INTERNAL_CLI_H

#include <stdbool.h>
#include <stdio.h>

#include "ucode/types.h"
#include "ucode/compiler.h"

/* -D [name=]value */
bool uc_cli_define_string(char *opt, uc_value_t *globals);

/* -F [name=]path; `stdin_unused` is cleared once stdin has been consumed */
bool uc_cli_define_file(char *opt, uc_value_t *globals, FILE **stdin_unused);

/* -L pattern */
void uc_cli_search_path(char *pattern, uc_parse_config_t *config);

/* -l [name=]library */
bool uc_cli_library_load(char *opt, uc_vm_t *vm);

#endif /* UCODE_INTERNAL_CLI_H */

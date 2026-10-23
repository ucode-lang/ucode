/*
 * Copyright (C) 2022 Jo-Philipp Wich <jo@mein.io>
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

#ifndef UCODE_PROGRAM_H
#define UCODE_PROGRAM_H

#include <stdio.h>

#include <ucode/types.h>

/* Serialized bytecode format versions.
 *
 * Instructions are appended rather than reordered, so the encodings stay
 * decodable across releases; what the versions keep apart is bytecode using an
 * instruction which a VM predating it cannot run.
 *
 * UCODE_BYTECODE_VERSION_MIN is the oldest bytecode this release accepts, and
 * what a program requiring nothing newer is written as, so that a VM predating
 * the newest instruction can still load it.  UCODE_BYTECODE_VERSION is the
 * newest this release knows; a program records it once it is compiled to use an
 * instruction introduced since then, see uc_program_require_version().
 *
 * 0x03 introduced I_STCALL. */
#define UCODE_BYTECODE_VERSION_MIN 0x02
#define UCODE_BYTECODE_VERSION     0x03

/* Whether bytecode of this version can be loaded by this release. */
#define uc_bytecode_version_supported(_v) \
	((_v) >= UCODE_BYTECODE_VERSION_MIN && (_v) <= UCODE_BYTECODE_VERSION)

/* Create a new, empty program. */
uc_program_t *uc_program_new(void);

/* Reference counting. uc_program_t is an opaque refcounted value; these are
 * the only ways downstream should acquire/release a program. */
uc_program_t *uc_program_get(uc_program_t *prog);
void uc_program_put(uc_program_t *prog);

/* Serialize a program to a stream, or load one (plain or precompiled) from a
 * source. uc_program_load takes ownership of the source. */
void uc_program_write(uc_program_t *prog, FILE *fp, bool compressed);
uc_program_t *uc_program_load(uc_source_t *source, char **errp);

/* Return a new-reference closure for the program's top-level entry function,
 * or NULL if the program is empty. The caller owns the returned reference. */
uc_value_t *uc_program_main(uc_vm_t *vm, uc_program_t *prog);

#endif /* UCODE_PROGRAM_H */

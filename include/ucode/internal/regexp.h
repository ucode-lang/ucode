/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ucode - a small dynamic scripting language for embedded systems
 *
 * Copyright (C) 2023 Jo-Philipp Wich <jo@mein.io>
 */

#ifndef UCODE_INTERNAL_REGEXP_H
#define UCODE_INTERNAL_REGEXP_H

#include <stdbool.h>
#include <stddef.h>

#include "ucode/internal/util.h"

/* Scanning state of a POSIX ERE bracket expression. */
typedef struct uc_re_class {
	bool first;
	bool neg;
	bool esc;
	bool pending;
	bool elemend;
	char elem;
} uc_re_class_t;

__hidden bool uc_re_class_step(uc_re_class_t *cls, int ch);

#endif /* UCODE_INTERNAL_REGEXP_H */

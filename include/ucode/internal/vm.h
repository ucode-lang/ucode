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

/* Internal VM instruction set. */

#ifndef UCODE_INTERNAL_VM_H
#define UCODE_INTERNAL_VM_H

#include "ucode/vm.h"

#define __insns \
__insn(NOOP) \
__insn(LOAD) \
__insn(LOAD8) \
__insn(LOAD16) \
__insn(LOAD32) \
__insn(LTHIS) \
__insn(LREXP) \
__insn(LNULL) \
__insn(LTRUE) \
__insn(LFALSE) \
__insn(LLOC) \
__insn(LUPV) \
__insn(LVAR) \
__insn(LVAL) \
__insn(PVAL) \
__insn(CLFN) \
__insn(ARFN) \
__insn(SLOC) \
__insn(SUPV) \
__insn(SVAR) \
__insn(SVAL) \
__insn(ULOC) \
__insn(UUPV) \
__insn(UVAR) \
__insn(UVAL) \
__insn(NARR) \
__insn(PARR) \
__insn(MARR) \
__insn(NOBJ) \
__insn(SOBJ) \
__insn(MOBJ) \
__insn(BOR) \
__insn(BXOR) \
__insn(BAND) \
__insn(EQS) \
__insn(NES) \
__insn(EQ) \
__insn(NE) \
__insn(LT) \
__insn(LE) \
__insn(GT) \
__insn(GE) \
__insn(IN) \
__insn(LSHIFT) \
__insn(RSHIFT) \
__insn(ADD) \
__insn(SUB) \
__insn(MUL) \
__insn(DIV) \
__insn(MOD) \
__insn(EXP) \
__insn(NOT) \
__insn(COMPL) \
__insn(PLUS) \
__insn(MINUS) \
__insn(JMP) \
__insn(JMPZ) \
__insn(JMPNT) \
__insn(COPY) \
__insn(POP) \
__insn(CUPV) \
__insn(RETURN) \
__insn(CALL) \
__insn(PRINT) \
__insn(NEXTK) \
__insn(NEXTKV) \
__insn(DELETE) \
__insn(IMPORT) \
__insn(EXPORT) \
__insn(DYNLOAD) \
__insn(STCALL)


#undef __insn
#define __insn(_name) I_##_name,

typedef enum {
	__insns
	__I_MAX
} uc_vm_insn_t;

extern uint32_t insns[__I_MAX];

extern const int8_t uc_vm_insn_format[__I_MAX];

int8_t uc_vm_insn_to_argtype(uc_vm_insn_t insn);

static inline bool
uc_vm_is_strict(uc_vm_t *vm)
{
	return (vm->callframes.count && uc_vector_last(&vm->callframes)->strict);
}

#endif /* UCODE_INTERNAL_VM_H */

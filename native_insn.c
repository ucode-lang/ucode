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

/* The instructions of native functions. Each helper does what the handler of
 * its instruction in vm.c does, through the same vmcore.c functions, on the
 * slots of the native frame instead of the VM stack: the operand stack of the
 * instruction starts at slot `d`, the depth before the instruction. Helpers
 * returning bool report whether the instruction completed without raising an
 * exception. */

#include <stdlib.h>

#include "ucode/internal/native.h"
#include "ucode/internal/vmcore.h"
#include "ucode/internal/program.h"

#define SLOT(i) (*uc_native_slot(f, (i)))

static uc_value_t *uc_native_arith(uc_vm_t *vm, uc_vm_insn_t operation,
                                   uc_value_t *value, uc_value_t *operand);

UC_NATIVE_INLINE static void
uc_native_store(uc_vm_t *vm, uc_native_frame_t *f, size_t slot, uc_value_t *v)
{
	/* only an upvalue reference resolves, and only counted values are one */
	if (ucv_is_counted(v))
		v = uc_vm_resolve_upval(vm, v);

	uc_native_slot_set(f, slot, v);
}

/* ucv_get() without a call for the values that have no reference count */
UC_NATIVE_INLINE static uc_value_t *
uc_native_ref(uc_value_t *v)
{
	return ucv_is_counted(v) ? ucv_get(v) : v;
}

/* The closure of the frame keeps the program and so its constants alive */
UC_NATIVE_INLINE static uc_value_t *
uc_native_constant(uc_native_frame_t *f, size_t idx)
{
	return uc_program_peek_constant(f->closure->function->program, idx);
}

UC_NATIVE_INLINE static uint64_t
uc_native_constant_hash(uc_native_frame_t *f, size_t idx)
{
	return uc_program_get_constant_hash(f->closure->function->program, idx);
}

UC_NATIVE_INLINE static bool
uc_native_ok(uc_vm_t *vm)
{
	return vm->exception.type == EXCEPTION_NONE;
}

UC_NATIVE_INLINE static bool
uc_native_strict(uc_native_frame_t *f)
{
	return f->closure->function->strict;
}

UC_NATIVE_INLINE static uc_upvalref_t *
uc_native_upval(uc_native_frame_t *f, size_t idx)
{
	return f->closure->upvals[idx];
}

UC_NATIVE_INLINE static uc_value_t *
uc_native_upval_value(uc_vm_t *vm, uc_upvalref_t *ref)
{
	return ref->closed ? ref->value : vm->stack.entries[ref->slot];
}

UC_NATIVE_INLINE static void
uc_native_upval_store(uc_vm_t *vm, uc_upvalref_t *ref, uc_value_t *val)
{
	if (ref->closed) {
		ucv_put(ref->value);
		ref->value = val;
	}
	else {
		uc_vm_stack_set(vm, ref->slot, val);
	}
}

/* The bodies of the instructions that the fused helpers at the end combine;
 * the helpers of the single instructions call them as well */

UC_NATIVE_INLINE static void
uc_native_insn_load(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t arg)
{
	uc_native_store(vm, f, d, uc_native_ref(uc_native_constant(f, arg)));
}

UC_NATIVE_INLINE static void
uc_native_insn_lloc(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t arg)
{
	uc_native_store(vm, f, d, uc_native_ref(SLOT(arg)));
}

UC_NATIVE_INLINE static void
uc_native_insn_lupv(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t arg)
{
	uc_native_store(vm, f, d, uc_native_ref(uc_native_upval_value(vm, uc_native_upval(f, arg))));
}

UC_NATIVE_INLINE static bool
uc_native_insn_lval(uc_vm_t *vm, uc_native_frame_t *f, uint32_t pc, int32_t d)
{
	uc_value_t *res;
	bool ok;

	uc_native_pc_set(vm, pc);
	ok = uc_vm_property_get(vm, SLOT(d - 2), SLOT(d - 1), &res);
	uc_native_slot_clear(f, d - 1);
	uc_native_slot_clear(f, d - 2);

	if (ok)
		uc_native_store(vm, f, d - 2, res);

	return uc_native_ok(vm);
}

UC_NATIVE_INLINE static bool
uc_native_insn_pval(uc_vm_t *vm, uc_native_frame_t *f, uint32_t pc, int32_t d)
{
	uc_value_t *res;
	bool ok;

	uc_native_pc_set(vm, pc);
	ok = uc_vm_property_get(vm, SLOT(d - 2), SLOT(d - 1), &res);
	uc_native_slot_clear(f, d - 1);

	if (ok)
		uc_native_store(vm, f, d - 1, res);

	return uc_native_ok(vm);
}

UC_NATIVE_INLINE static void
uc_native_insn_sloc(uc_native_frame_t *f, int32_t d, uint32_t arg)
{
	uc_native_slot_set(f, arg, uc_native_ref(SLOT(d - 1)));
}

UC_NATIVE_INLINE static void
uc_native_insn_pop(uc_vm_t *vm, uc_native_frame_t *f, int32_t d)
{
	uc_native_slot_clear(f, d - 1);
	uc_vm_gc_step(vm);
}

UC_NATIVE_INLINE static bool
uc_native_insn_jmpz(uc_native_frame_t *f, int32_t d)
{
	uc_value_t *v = uc_native_slot_take(f, d - 1);
	bool falsy;

	/* null and the booleans that conditions mostly test need no call */
	if (v == NULL)
		return true;

	if (((uintptr_t)v & 3) == UC_BOOLEAN)
		return !(((uintptr_t)v >> 2) & 1);

	falsy = !ucv_is_truish(v);
	ucv_put(v);

	return falsy;
}

void
uc_native_load(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_insn_load(vm, f, d, arg);
}

void
uc_native_load8(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d, ucv_uint64_new(arg));
}

void
uc_native_load16(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d, ucv_uint64_new(arg));
}

void
uc_native_load32(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d, ucv_uint64_new(arg));
}

void
uc_native_lthis(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d, uc_native_ref(f->ctx));
}

bool
uc_native_lrexp(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *re, *jstr = uc_native_constant(f, arg);
	bool loaded;

	uc_native_pc_set(vm, pc);
	loaded = uc_vm_regexp_load(vm, jstr, &re);

	if (loaded)
		uc_native_store(vm, f, d, re);

	return uc_native_ok(vm);
}

void
uc_native_lnull(UC_NATIVE_INSN_ARGS)
{
	uc_native_slot_set(f, d, NULL);
}

UC_NATIVE_NOINLINE void
uc_native_ltrue(UC_NATIVE_INSN_ARGS)
{
	uc_native_slot_set(f, d, ucv_boolean_new(true));
}

UC_NATIVE_NOINLINE void
uc_native_lfalse(UC_NATIVE_INSN_ARGS)
{
	uc_native_slot_set(f, d, ucv_boolean_new(false));
}

void
uc_native_lloc(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_insn_lloc(vm, f, d, arg);
}

void
uc_native_lupv(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_insn_lupv(vm, f, d, arg);
}

bool
uc_native_lvar(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *name = uc_native_constant(f, arg);
	uc_value_t *val;

	uc_native_pc_set(vm, pc);
	val = uc_vm_global_get(vm, name, uc_native_constant_hash(f, arg), uc_native_strict(f));
	uc_native_store(vm, f, d, val);

	return uc_native_ok(vm);
}

bool
uc_native_lval(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	return uc_native_insn_lval(vm, f, pc, d);
}

bool
uc_native_pval(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	return uc_native_insn_pval(vm, f, pc, d);
}

static void
uc_native_closure(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t arg,
                  const int32_t *x, bool arrow)
{
	uc_native_function_info_t *info = f->closure->function->native;
	uc_function_t *function = info->functions[arg - 1];
	uc_closure_t *closure = (uc_closure_t *)ucv_closure_new(vm, function, arrow);
	uc_upvalref_t *ref;

	uc_native_slot_set(f, d, &closure->header);

	if (function->module)
		return;

	for (size_t i = 0; i < function->nupvals; i++) {
		if (x[i] < 0)
			ref = uc_native_slot_capture(f, -(x[i] + 1));
		else
			ref = uc_native_upval(f, x[i]);

		closure->upvals[i] = ref;
		ucv_get(&ref->header);
	}
}

UC_NATIVE_NOINLINE void
uc_native_clfn(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_closure(vm, f, d, arg, x, false);
}

UC_NATIVE_NOINLINE void
uc_native_arfn(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_closure(vm, f, d, arg, x, true);
}

void
uc_native_sloc(UC_NATIVE_INSN_ARGS)
{
	uc_native_insn_sloc(f, d, arg);
}

void
uc_native_supv(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_upval_store(vm, uc_native_upval(f, arg), uc_native_ref(SLOT(d - 1)));
}

bool
uc_native_svar(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *name = uc_native_constant(f, arg);
	bool stored;

	uc_native_pc_set(vm, pc);
	stored = uc_vm_global_set(vm, name, uc_native_constant_hash(f, arg), SLOT(d - 1),
	                          uc_native_strict(f));

	if (!stored)
		uc_native_slot_clear(f, d - 1);

	return uc_native_ok(vm);
}

bool
uc_native_sval(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *res;
	bool ok;

	uc_native_pc_set(vm, pc);
	ok = uc_vm_property_set(vm, SLOT(d - 3), SLOT(d - 2), SLOT(d - 1), &res);
	uc_native_slot_clear(f, d - 1);
	uc_native_slot_clear(f, d - 2);
	uc_native_slot_clear(f, d - 3);

	if (ok)
		uc_native_store(vm, f, d - 3, res);

	return uc_native_ok(vm);
}

bool
uc_native_uloc(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	size_t slot = arg & 0x00ffffff;
	uc_value_t *val;

	uc_native_pc_set(vm, pc);
	val = uc_native_arith(vm, arg >> 24, SLOT(slot), SLOT(d - 1));
	uc_native_store(vm, f, d - 1, val);
	uc_native_slot_set(f, slot, uc_native_ref(SLOT(d - 1)));

	return uc_native_ok(vm);
}

bool
uc_native_uupv(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_upvalref_t *ref = uc_native_upval(f, arg & 0x00ffffff);
	uc_value_t *val;

	uc_native_pc_set(vm, pc);
	val = uc_native_arith(vm, arg >> 24, uc_native_upval_value(vm, ref), SLOT(d - 1));
	uc_native_store(vm, f, d - 1, val);
	uc_native_upval_store(vm, ref, uc_native_ref(SLOT(d - 1)));

	return uc_native_ok(vm);
}

bool
uc_native_uvar(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *name = uc_native_constant(f, arg & 0x00ffffff);
	uc_value_t *val;
	bool ok;

	uc_native_pc_set(vm, pc);
	ok = uc_vm_global_update(vm, arg >> 24, name, uc_native_constant_hash(f, arg & 0x00ffffff),
	                         SLOT(d - 1), uc_native_strict(f), &val);
	uc_native_slot_clear(f, d - 1);

	if (ok)
		uc_native_store(vm, f, d - 1, val);

	return uc_native_ok(vm);
}

bool
uc_native_uval(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *res;
	bool ok;

	uc_native_pc_set(vm, pc);
	ok = uc_vm_property_update(vm, arg, SLOT(d - 3), SLOT(d - 2), SLOT(d - 1), &res);
	uc_native_slot_clear(f, d - 1);
	uc_native_slot_clear(f, d - 2);
	uc_native_slot_clear(f, d - 3);

	if (ok)
		uc_native_store(vm, f, d - 3, res);

	return uc_native_ok(vm);
}

void
uc_native_narr(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_slot_set(f, d, ucv_array_new_length(vm, arg));
}

void
uc_native_parr(UC_NATIVE_INSN_ARGS)
{
	uc_value_t *arr = SLOT(d - arg - 1), *v;

	for (uint32_t i = 0; i < arg; i++) {
		v = uc_native_slot_take(f, d - arg + i);
		uc_vm_array_append(arr, &v, 1);
	}
}

bool
uc_native_marr(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_pc_set(vm, pc);

	return uc_vm_array_spread(vm, SLOT(d - 2), uc_native_slot_take(f, d - 1));
}

void
uc_native_nobj(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_slot_set(f, d, uc_vm_object_new(vm, arg / 2));
}

bool
uc_native_sobj(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *obj = SLOT(d - arg - 1);
	uint32_t i;

	uc_native_pc_set(vm, pc);

	for (i = 0; i < arg; i += 2)
		uc_vm_object_init_key(vm, obj, SLOT(d - arg + i), SLOT(d - arg + i + 1));

	for (i = 0; i < arg; i++)
		uc_native_slot_clear(f, d - arg + i);

	return uc_native_ok(vm);
}

bool
uc_native_mobj(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_pc_set(vm, pc);

	return uc_vm_object_spread(vm, SLOT(d - 2), uc_native_slot_take(f, d - 1));
}

/* Computes with integers that the pointers hold without a call; the result
 * equals that of uc_vm_value_arith(), which handles everything else,
 * including products that wrap and division by zero. */
static uc_value_t *
uc_native_arith(uc_vm_t *vm, uc_vm_insn_t operation, uc_value_t *value, uc_value_t *operand)
{
	int64_t n1, n2, n;

	if (!ucv_is_small_integer(value) || !ucv_is_small_integer(operand))
		return uc_vm_value_arith(vm, operation, value, operand);

	n1 = ucv_small_integer_get(value);
	n2 = ucv_small_integer_get(operand);

	switch (operation) {
	case I_ADD:
	case I_PLUS:
		n = n1 + n2;
		break;

	case I_SUB:
	case I_MINUS:
		n = n1 - n2;
		break;

	case I_MUL:
		if (__builtin_mul_overflow(n1, n2, &n))
			return uc_vm_value_arith(vm, operation, value, operand);

		break;

	case I_DIV:
		if (n2 == 0)
			return uc_vm_value_arith(vm, operation, value, operand);

		n = n1 / n2;
		break;

	case I_MOD:
		if (n2 == 0)
			return uc_vm_value_arith(vm, operation, value, operand);

		n = n1 % n2;
		break;

	default:
		return uc_vm_value_arith(vm, operation, value, operand);
	}

	return (n < 0) ? ucv_int64_new(n) : ucv_uint64_new(n);
}

/* Compares integers that the pointers hold without a call; the result equals
 * that of uc_vm_value_compare() and uc_vm_value_equality(). */
static bool
uc_native_compare(uc_vm_insn_t operation, uc_value_t *v1, uc_value_t *v2)
{
	int64_t n1, n2;

	if (!ucv_is_small_integer(v1) || !ucv_is_small_integer(v2))
		return (operation == I_EQS || operation == I_NES)
			? uc_vm_value_equality(operation, v1, v2)
			: uc_vm_value_compare(operation, v1, v2);

	n1 = ucv_small_integer_get(v1);
	n2 = ucv_small_integer_get(v2);

	switch (operation) {
	case I_LT:  return n1 < n2;
	case I_LE:  return n1 <= n2;
	case I_GT:  return n1 > n2;
	case I_GE:  return n1 >= n2;
	case I_EQ:
	case I_EQS: return n1 == n2;
	default:    return n1 != n2;
	}
}

/* The part of ADD and SUB that LLVM inlines at the instruction: integers that
 * the pointers hold, whose sum fits; anything else returns false. Neither
 * operand nor the result has a reference count. */
UC_NATIVE_INLINE static bool
uc_native_arith_inline(uc_native_frame_t *f, int32_t d, uc_vm_insn_t insn)
{
	uc_value_t *v1 = SLOT(d - 2), *v2 = SLOT(d - 1);
	int64_t n;

	if (!ucv_is_small_integer(v1) || !ucv_is_small_integer(v2))
		return false;

	if (insn == I_ADD)
		n = ucv_small_integer_get(v1) + ucv_small_integer_get(v2);
	else
		n = ucv_small_integer_get(v1) - ucv_small_integer_get(v2);

	SLOT(d - 2) = (n < 0) ? ucv_int64_new(n) : ucv_uint64_new(n);
	SLOT(d - 1) = NULL;

	return true;
}

/* The part of a comparison that LLVM inlines at the instruction, as
 * uc_native_arith_inline() */
UC_NATIVE_INLINE static bool
uc_native_compare_inline(uc_native_frame_t *f, int32_t d, uc_vm_insn_t insn)
{
	uc_value_t *v1 = SLOT(d - 2), *v2 = SLOT(d - 1);

	if (!ucv_is_small_integer(v1) || !ucv_is_small_integer(v2))
		return false;

	SLOT(d - 2) = ucv_boolean_new(uc_native_compare(insn, v1, v2));
	SLOT(d - 1) = NULL;

	return true;
}

__attribute__((noinline)) static bool
uc_native_binary(uc_vm_t *vm, uc_native_frame_t *f, uint32_t pc, int32_t d,
                 uc_value_t *(*op)(uc_vm_t *, uc_vm_insn_t, uc_value_t *, uc_value_t *),
                 uc_vm_insn_t insn)
{
	uc_value_t *rv;

	uc_native_pc_set(vm, pc);
	rv = op(vm, insn, SLOT(d - 2), SLOT(d - 1));
	uc_native_slot_clear(f, d - 1);
	uc_native_store(vm, f, d - 2, rv);

	return uc_native_ok(vm);
}

static bool
uc_native_predicate(uc_vm_t *vm, uc_native_frame_t *f, uint32_t pc, int32_t d, bool res)
{
	uc_native_slot_clear(f, d - 1);
	uc_native_slot_set(f, d - 2, ucv_boolean_new(res));

	return uc_native_ok(vm);
}

__attribute__((noinline)) static bool
uc_native_comparison(uc_vm_t *vm, uc_native_frame_t *f, uint32_t pc, int32_t d,
                     uc_vm_insn_t insn)
{
	uc_native_pc_set(vm, pc);

	return uc_native_predicate(vm, f, pc, d, uc_native_compare(insn, SLOT(d - 2), SLOT(d - 1)));
}

#define UC_NATIVE_ARITH(name, insn) \
	bool uc_native_##name(UC_NATIVE_INSN_ARGS) \
	{ return uc_native_binary(f->vm, f, pc, d, uc_native_arith, insn); }

#define UC_NATIVE_ARITH_INLINE(name, insn) \
	UC_NATIVE_INLINE bool uc_native_##name(UC_NATIVE_INSN_ARGS) \
	{ return uc_native_arith_inline(f, d, insn) || \
	         uc_native_binary(f->vm, f, pc, d, uc_native_arith, insn); }

#define UC_NATIVE_BITOP(name, insn) \
	bool uc_native_##name(UC_NATIVE_INSN_ARGS) \
	{ return uc_native_binary(f->vm, f, pc, d, uc_vm_value_bitop, insn); }

#define UC_NATIVE_COMPARE(name, insn) \
	UC_NATIVE_NOINLINE bool uc_native_##name(UC_NATIVE_INSN_ARGS) \
	{ return uc_native_comparison(f->vm, f, pc, d, insn); }

/* the order comparisons of loop conditions are inlined; equality is mostly
 * applied to other values than integers, where inlining only costs space */
#define UC_NATIVE_COMPARE_INLINE(name, insn) \
	UC_NATIVE_INLINE bool uc_native_##name(UC_NATIVE_INSN_ARGS) \
	{ return uc_native_compare_inline(f, d, insn) || \
	         uc_native_comparison(f->vm, f, pc, d, insn); }

UC_NATIVE_ARITH_INLINE(add, I_ADD)
UC_NATIVE_ARITH_INLINE(sub, I_SUB)
UC_NATIVE_ARITH(mul, I_MUL)
UC_NATIVE_ARITH(div, I_DIV)
UC_NATIVE_ARITH(mod, I_MOD)
UC_NATIVE_ARITH(exp, I_EXP)
UC_NATIVE_BITOP(lshift, I_LSHIFT)
UC_NATIVE_BITOP(rshift, I_RSHIFT)
UC_NATIVE_BITOP(band, I_BAND)
UC_NATIVE_BITOP(bxor, I_BXOR)
UC_NATIVE_BITOP(bor, I_BOR)
UC_NATIVE_COMPARE(eqs, I_EQS)
UC_NATIVE_COMPARE(nes, I_NES)
UC_NATIVE_COMPARE(eq, I_EQ)
UC_NATIVE_COMPARE(ne, I_NE)
UC_NATIVE_COMPARE_INLINE(lt, I_LT)
UC_NATIVE_COMPARE_INLINE(le, I_LE)
UC_NATIVE_COMPARE_INLINE(gt, I_GT)
UC_NATIVE_COMPARE_INLINE(ge, I_GE)

void
uc_native_in(UC_NATIVE_INSN_ARGS)
{
	bool res = uc_vm_value_in(SLOT(d - 2), SLOT(d - 1));

	uc_native_slot_clear(f, d - 1);
	uc_native_slot_set(f, d - 2, ucv_boolean_new(res));
}

void
uc_native_not(UC_NATIVE_INSN_ARGS)
{
	uc_native_slot_set(f, d - 1, ucv_boolean_new(uc_vm_value_not(SLOT(d - 1))));
}

void
uc_native_compl(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d - 1, uc_vm_value_complement(SLOT(d - 1)));
}

void
uc_native_plus(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d - 1, uc_vm_value_unary(I_PLUS, SLOT(d - 1)));
}

void
uc_native_minus(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d - 1, uc_vm_value_unary(I_MINUS, SLOT(d - 1)));
}

/* Returns whether the branch is taken. */
bool
uc_native_jmpz(UC_NATIVE_INSN_ARGS)
{
	return uc_native_insn_jmpz(f, d);
}

/* Returns whether the branch is taken; it then drops the values above the
 * guarded one and leaves null in its place, as uc_vm_insn_jmpnt() does. */
bool
uc_native_jmpnt(UC_NATIVE_INSN_ARGS)
{
	uint16_t types = (arg >> 16) & 0x1fff;
	uint8_t depth = (arg >> 29) & 0x7;

	if (types & (1u << ucv_type(SLOT(d - 1))))
		return false;

	for (int32_t i = 0; i <= depth; i++)
		uc_native_slot_clear(f, d - 1 - i);

	return true;
}

void
uc_native_copy(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_store(vm, f, d, uc_native_ref(SLOT(d - 1 - arg)));
}

void
uc_native_pop(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_insn_pop(vm, f, d);
}

void
uc_native_cupv(UC_NATIVE_INSN_ARGS)
{
	uc_native_slot_clear(f, d - 1);
}

UC_NATIVE_NOINLINE void
uc_native_return(UC_NATIVE_INSN_ARGS)
{
	ucv_put(f->result);
	f->result = uc_native_slot_take(f, d - 1);
	uc_native_frame_release(f, d - 1);
}

/* Moves the callee of a CALL, its receiver for a method call and its
 * arguments, expanding spread arguments, onto the VM stack in the layout that
 * uc_vm_call_function() builds. Returns the number of arguments, or -1 if a
 * spread argument is not an array. */
static ssize_t
uc_native_call_push(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t arg,
                    const int32_t *x, size_t *stackoff)
{
	size_t nargs = arg & 0xffff, nspreads = (arg >> 16) & 0x7fff, i, slot;
	bool mcall = arg & 0x80000000;
	int32_t fi = d - nargs - 1;

	if (mcall)
		uc_vm_stack_push(vm, uc_native_slot_take(f, fi - 1));

	*stackoff = vm->stack.count;
	uc_vm_stack_push(vm, uc_native_slot_take(f, fi));

	for (i = 0; i < nargs; i++)
		uc_vm_stack_push(vm, uc_native_slot_take(f, fi + 1 + i));

	if (!nspreads)
		return nargs;

	for (i = 0, slot = nargs; i < nspreads; i++) {
		if (!uc_vm_spread_push(vm, *stackoff + 1, nargs, slot, x[i]))
			return -1;

		slot = x[i];
	}

	uc_vm_spread_finish(vm, *stackoff + 1, nargs, slot);

	return vm->stack.count - *stackoff - 1;
}

static void
uc_native_stack_unwind(uc_vm_t *vm, size_t count)
{
	while (vm->stack.count > count)
		ucv_put(uc_vm_stack_pop(vm));
}

static uc_value_t **
uc_native_stack_args(uc_vm_t *vm, size_t nargs)
{
	uc_value_t **args = nargs ? xalloc(nargs * sizeof(*args)) : NULL;

	for (size_t i = nargs; i > 0; i--)
		args[i - 1] = uc_vm_stack_pop(vm);

	return args;
}

/* Calls the value on the VM stack at `stackoff`, with the receiver below it
 * for a method call and `nargs` arguments above it, as uc_vm_call_function()
 * does, and leaves the stack as it was before the receiver was pushed. With
 * `tail`, a native closure is not called but stored as the tail call request
 * of the frame. */
static bool
uc_native_call_stack(uc_vm_t *vm, uc_native_frame_t *f, uc_value_t *ctx, bool mcall,
                     size_t stackoff, size_t nargs, bool tail, uc_value_t **res)
{
	size_t base = stackoff - mcall;
	uc_value_t *fno = ucv_get(vm->stack.entries[stackoff]);
	uc_closure_t *closure;
	uc_value_t **args;
	bool bytecode;

	*res = NULL;

	if (!uc_vm_call_depth_check(vm, tail)) {
		ucv_put(ctx);
		ucv_put(fno);
		uc_native_stack_unwind(vm, base);

		return false;
	}

	if (!uc_vm_call_dispatch(vm, &ctx, &fno, mcall, nargs)) {
		uc_native_stack_unwind(vm, base);

		return false;
	}

	closure = fno ? ucv_as_closure(fno) : NULL;
	bytecode = closure && !closure->function->native;

	/* the interpreter, if the program links it, runs a bytecode closure on
	 * the stack, and its frame pop leaves the result in place of the callee
	 * and the receiver, as a C function that already ran does */
	if (bytecode)
		uc_vm_closure_call(vm, closure, ctx, mcall, nargs, stackoff);

	if (!closure || bytecode) {
		if (vm->stack.count > base)
			*res = uc_vm_stack_pop(vm);

		uc_native_stack_unwind(vm, base);

		return uc_native_ok(vm);
	}

	args = uc_native_stack_args(vm, nargs);
	uc_native_stack_unwind(vm, base);

	if (tail) {
		f->tail.fn = fno;
		f->tail.ctx = ctx;
		f->tail.mcall = mcall;
		f->tail.args = args;
		f->tail.nargs = nargs;
		f->tail.pending = true;

		return true;
	}

	*res = uc_native_invoke(vm, closure, ctx, mcall, args, nargs);
	ucv_put(fno);
	free(args);

	return uc_native_ok(vm);
}

static uc_value_t *
uc_native_call_ctx(uc_native_frame_t *f, int32_t d, uint32_t arg)
{
	size_t nargs = arg & 0xffff;
	int32_t fi = d - nargs - 1;

	if (ucv_is_arrowfn(SLOT(fi)))
		return ucv_get(f->ctx);

	return (arg & 0x80000000) ? ucv_get(SLOT(fi - 1)) : NULL;
}

static bool
uc_native_closure_is_native(uc_value_t *v)
{
	return ucv_type(v) == UC_CLOSURE && ucv_as_closure(v)->function->native;
}

/* Calls a native closure without the VM stack: the arguments move from the
 * slots of the frame into the frame of the callee. */
static bool
uc_native_call_direct(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t arg)
{
	bool mcall = arg & 0x80000000;
	size_t nargs = arg & 0xffff, i;
	int32_t fi = d - nargs - 1, s;
	/* without cells, the slots of the arguments serve as the array, which
	 * the callee clears */
	bool in_slots = !f->captured;
	uc_value_t *buf[16], **args = in_slots ? &f->slots[fi + 1] : buf, *ctx, *res;

	ctx = uc_native_call_ctx(f, d, arg);

	if (!in_slots) {
		if (nargs > ARRAY_SIZE(buf))
			args = xalloc(nargs * sizeof(*args));

		for (i = 0; i < nargs; i++)
			args[i] = uc_native_slot_take(f, fi + 1 + i);
	}

	res = uc_native_invoke(vm, ucv_as_closure(SLOT(fi)), ctx, mcall, args, nargs);

	if (!in_slots && args != buf)
		free(args);

	for (s = d - 1; s >= fi - (int32_t)mcall; s--)
		uc_native_slot_clear(f, s);

	if (!uc_native_ok(vm)) {
		ucv_put(res);

		return false;
	}

	uc_native_store(vm, f, fi - mcall, res);

	return uc_native_signals_dispatch(vm);
}

static bool
uc_native_call_common(uc_vm_t *vm, uc_native_frame_t *f, uint32_t pc, int32_t d,
                      uint32_t arg, const int32_t *x, bool tail)
{
	bool mcall = arg & 0x80000000;
	size_t nargs = arg & 0xffff, stackoff;
	int32_t fi = d - nargs - 1, i;
	uc_value_t *ctx, *res = NULL;
	ssize_t n;
	bool ok;

	uc_native_pc_set(vm, pc);

	if (!tail && !(arg & 0x7fff0000) && uc_native_closure_is_native(SLOT(fi)))
		return uc_native_call_direct(vm, f, d, arg);

	ctx = uc_native_call_ctx(f, d, arg);
	n = uc_native_call_push(vm, f, d, arg, x, &stackoff);

	if (n < 0) {
		ucv_put(ctx);
		uc_native_stack_unwind(vm, stackoff - mcall);
		ok = false;
	}
	else {
		ok = uc_native_call_stack(vm, f, ctx, mcall, stackoff, n, tail, &res);
	}

	for (i = d - 1; i >= fi - (int32_t)mcall; i--)
		uc_native_slot_clear(f, i);

	if (tail && ok && !f->tail.pending) {
		ucv_put(f->result);
		f->result = res;
	}
	else if (ok && !tail) {
		uc_native_store(vm, f, fi - mcall, res);
	}
	else {
		ucv_put(res);
	}

	if (ok && !tail)
		ok = uc_native_signals_dispatch(vm);

	return ok;
}

bool
uc_native_call(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	return uc_native_call_common(vm, f, pc, d, arg, x, false);
}

bool
uc_native_tailcall(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	return uc_native_call_common(vm, f, pc, d, arg, x, true);
}

bool
uc_native_stcall(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	size_t nargs = arg & 0xffff, base = d - nargs, keep = vm->stack.count;
	uc_value_t *res = NULL;

	uc_native_pc_set(vm, pc);

	for (size_t i = 0; i < nargs; i++)
		uc_vm_stack_push(vm, uc_native_slot_take(f, base + i));

	uc_vm_stcall(vm, arg >> 16, nargs);

	if (vm->stack.count > keep)
		res = uc_vm_stack_pop(vm);

	uc_native_stack_unwind(vm, keep);

	for (size_t i = 0; i < nargs; i++)
		uc_native_slot_clear(f, base + i);

	uc_native_store(vm, f, base, res);

	return uc_native_ok(vm);
}

bool
uc_native_print(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_pc_set(vm, pc);
	uc_vm_value_print(vm, SLOT(d - 1));
	uc_native_slot_clear(f, d - 1);

	return uc_native_ok(vm);
}

bool
uc_native_nextk(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *k = uc_native_slot_take(f, d - 1);
	uc_value_t *v = uc_native_slot_take(f, d - 2);
	uc_value_t *key = NULL, *val = NULL;

	uc_native_pc_set(vm, pc);

	if (uc_vm_iterator_next(vm, I_NEXTK, v, &k, &key, &val)) {
		uc_native_store(vm, f, d - 2, key);
		uc_native_store(vm, f, d - 1, k);
	}
	else {
		ucv_put(k);
	}

	ucv_put(v);

	return uc_native_ok(vm);
}

bool
uc_native_nextkv(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *k = uc_native_slot_take(f, d - 1);
	uc_value_t *v = uc_native_slot_take(f, d - 2);
	uc_value_t *key = NULL, *val = NULL;

	uc_native_pc_set(vm, pc);

	if (uc_vm_iterator_next(vm, I_NEXTKV, v, &k, &key, &val)) {
		uc_native_store(vm, f, d - 2, key);
		uc_native_store(vm, f, d - 1, val);
		uc_native_store(vm, f, d, k);
	}
	else {
		ucv_put(k);
	}

	ucv_put(v);

	return uc_native_ok(vm);
}

bool
uc_native_delete(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_value_t *res;
	bool ok;

	uc_native_pc_set(vm, pc);
	ok = uc_vm_property_delete(vm, SLOT(d - 2), SLOT(d - 1), &res);
	uc_native_slot_clear(f, d - 1);
	uc_native_slot_clear(f, d - 2);

	if (ok)
		uc_native_store(vm, f, d - 2, res);

	return uc_native_ok(vm);
}

bool
uc_native_import(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uint16_t to = arg >> 16;

	uc_native_pc_set(vm, pc);

	if (to != 0xffff) {
		uc_vm_import(vm, arg & 0xffff, to);

		return uc_native_ok(vm);
	}

	uc_native_store(vm, f, d,
		uc_vm_import_namespace(vm, x[0], (const uint32_t *)&x[1], arg & 0xffff));

	return uc_native_ok(vm);
}

void
uc_native_export(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uc_modexports_t *exports = uc_vm_export_target(vm);

	if (exports)
		uc_vm_export_add(exports, uc_native_slot_capture(f, arg));
}

bool
uc_native_dynload(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	uint16_t count = arg & 0xffff, to = arg >> 16;
	uc_value_t *name, *modscope;
	bool ok = true;

	uc_native_pc_set(vm, pc);
	name = uc_native_slot_take(f, d - 1);
	modscope = uc_vm_dynload_module(vm, name);
	ucv_put(name);

	if (!modscope)
		return uc_native_ok(vm);

	if (count == 0) {
		uc_native_store(vm, f, d - 1, uc_vm_dynload_namespace(vm, modscope));
		ucv_put(modscope);

		return uc_native_ok(vm);
	}

	for (uint16_t i = 0; ok && i < count; i++)
		ok = uc_vm_dynload_bind(vm, modscope, x[i], to + i);

	ucv_put(modscope);

	return ok && uc_native_ok(vm);
}

/* Fused helpers: each runs a sequence of instructions that the backend found
 * without a branch into it, with the depth `d` before the first one and the
 * position `pc` after the one that can raise. */

/* The part of getk for an own key of an object, which needs neither the key
 * as a value on the stack nor its hash computed again; returns false for
 * anything else, which the generic path handles. */
static bool
uc_native_getk_own(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t arg)
{
	uint32_t k = arg & 0x00ffffff;
	uc_value_t *key, *res;

	if (ucv_type(SLOT(d - 1)) != UC_OBJECT)
		return false;

	key = uc_native_constant(f, k);

	if (ucv_type(key) != UC_STRING ||
	    !ucv_key_get_hashed(vm, SLOT(d - 1), key, uc_native_constant_hash(f, k), &res))
		return false;

	if (arg & UC_NATIVE_GETK_KEEP) {
		uc_native_store(vm, f, d, res);
	}
	else {
		uc_native_slot_clear(f, d - 1);
		uc_native_store(vm, f, d - 1, res);
	}

	return true;
}

/* LOAD of the constant key in the low 24 bits of `arg`, then LVAL, or PVAL
 * with UC_NATIVE_GETK_KEEP; with `src` other than UC_NATIVE_GETK_STACK, an
 * LLOC of the slot or an LUPV of the upvalue with UC_NATIVE_GETK_UPVAL
 * comes first. */
bool
uc_native_getk(uc_native_frame_t *f, uint32_t pc, int32_t d, uint32_t arg, uint32_t src)
{
	uc_vm_t *vm = f->vm;

	if (src != UC_NATIVE_GETK_STACK) {
		if (src & UC_NATIVE_GETK_UPVAL)
			uc_native_insn_lupv(vm, f, d, src & ~UC_NATIVE_GETK_UPVAL);
		else
			uc_native_insn_lloc(vm, f, d, src);

		d++;
	}

	if (uc_native_getk_own(vm, f, d, arg))
		return true;

	uc_native_insn_load(vm, f, d, arg & 0x00ffffff);

	if (arg & UC_NATIVE_GETK_KEEP)
		return uc_native_insn_pval(vm, f, pc, d + 1);

	return uc_native_insn_lval(vm, f, pc, d + 1);
}

/* `arg` times POP */
void
uc_native_popn(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	for (uint32_t i = 0; i < arg; i++)
		uc_native_insn_pop(vm, f, d - i);
}

/* CALL, then POP of its result */
bool
uc_native_callpop(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	if (!uc_native_call(f, pc, d, arg, x))
		return false;

	uc_native_insn_pop(vm, f, d - (arg & 0xffff) - (arg >> 31));

	return true;
}

/* LLOC of the slot in UC_NATIVE_LCALL_SLOT of `arg`, then CALL without
 * spread arguments */
bool
uc_native_lcall(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_insn_lloc(vm, f, d, (arg & UC_NATIVE_LCALL_SLOT) >> 16);

	return uc_native_call_common(vm, f, pc, d + 1, arg & ~UC_NATIVE_LCALL_SLOT, x, false);
}

/* LLOC, then CALL as uc_native_lcall(), then POP of its result */
bool
uc_native_lcallpop(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	if (!uc_native_lcall(f, pc, d, arg, x))
		return false;

	uc_native_insn_pop(vm, f, d + 1 - (arg & 0xffff) - (arg >> 31));

	return true;
}

/* COPY, then JMPZ, which test the value `arg` slots below the top without
 * taking it, then POP if UC_NATIVE_COPYJMPZ_POP is set and the branch is
 * not taken; returns whether the branch is taken. */
bool
uc_native_copyjmpz(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;
	bool falsy = !ucv_is_truish(SLOT(d - 1 - (arg & ~UC_NATIVE_COPYJMPZ_POP)));

	if (!falsy && (arg & UC_NATIVE_COPYJMPZ_POP))
		uc_native_insn_pop(vm, f, d);

	return falsy;
}

/* SLOC, then POP */
void
uc_native_slocpop(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_insn_sloc(f, d, arg);
	uc_native_insn_pop(vm, f, d);
}

UC_NATIVE_INLINE static void
uc_native_load_kind(uc_vm_t *vm, uc_native_frame_t *f, int32_t d, uint32_t op)
{
	uint32_t idx = op & UC_NATIVE_LOAD2_INDEX;

	switch (op >> UC_NATIVE_LOAD2_KIND_SHIFT) {
	case UC_NATIVE_LOAD2_LLOC:
		uc_native_insn_lloc(vm, f, d, idx);
		break;

	case UC_NATIVE_LOAD2_LUPV:
		uc_native_insn_lupv(vm, f, d, idx);
		break;

	default:
		uc_native_insn_load(vm, f, d, idx);
		break;
	}
}

/* Two of LLOC, LUPV and LOAD, the first in the low 16 bits of `arg` */
void
uc_native_load2(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_load_kind(vm, f, d, arg & 0xffff);
	uc_native_load_kind(vm, f, d + 1, arg >> 16);
}

/* LNULL, which has the empty slot `d` to push to, then EQS, or NES if `arg`
 * is set, then JMPZ; returns whether the branch is taken. Strict equality
 * cannot raise. */
bool
uc_native_jnull(UC_NATIVE_INSN_ARGS)
{
	uc_vm_t *vm = f->vm;

	uc_native_comparison(vm, f, pc, d + 1, arg ? I_NES : I_EQS);

	return uc_native_insn_jmpz(f, d);
}

/* NOT, then JMPZ; returns whether the branch is taken. NOT cannot raise. */
bool
uc_native_notjmpz(UC_NATIVE_INSN_ARGS)
{
	uc_native_slot_set(f, d - 1, ucv_boolean_new(uc_vm_value_not(SLOT(d - 1))));

	return uc_native_insn_jmpz(f, d);
}

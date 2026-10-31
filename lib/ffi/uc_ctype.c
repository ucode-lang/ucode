/*
** C type management.
** Copyright (C) 2005-2025 Mike Pall. See Copyright Notice below.
**
** This file contains derived work from LuaJIT's FFI type system (lj_ctype.c).
**
** Modifications:
** - Adapted VM interactions to use ucode's API (uc_vm_t, uc_value_t, etc.)
** - Adapted type registration and lookup for ucode resource system
** - Removed JIT-specific code and dependencies
**
** See NOTICE and ATTRIBUTION.md for complete attribution details.
*/


#include <stdio.h>
#include <assert.h>

#include "ucode/util.h"

#include "uc_ctype.h"

/* Hash constants from lj_tab.h - inlined since lj_tab.h removed */
#define HASH_ROT1	14
#define HASH_ROT2	5
#define HASH_ROT3	13

static UC_AINLINE uint32_t hashrot(uint32_t lo, uint32_t hi)
{
#if UC_TARGET_X86ORX64
  lo ^= hi; hi = uc_rol(hi, HASH_ROT1);
  lo -= hi; hi = uc_rol(hi, HASH_ROT2);
  hi ^= lo; hi -= uc_rol(lo, HASH_ROT3);
#else
  lo ^= hi;
  lo = lo - uc_rol(hi, HASH_ROT1);
  hi = lo ^ uc_rol(hi, HASH_ROT1 + HASH_ROT2);
  hi = hi - uc_rol(lo, HASH_ROT3);
#endif
  return hi;
}

/* -- C type definitions -------------------------------------------------- */

/* Predefined typedefs. */
#define CTTDDEF(_)                 \
	/* Vararg handling. */         \
	_("va_list", P_VOID)           \
	_("__builtin_va_list", P_VOID) \
	_("__gnuc_va_list", P_VOID)    \
	/* From stddef.h. */           \
	_("ptrdiff_t", INT_PSZ)        \
	_("size_t", UINT_PSZ)          \
	_("wchar_t", WCHAR)            \
	/* Subset of stdint.h. */      \
	_("int8_t", INT8)              \
	_("int16_t", INT16)            \
	_("int32_t", INT32)            \
	_("int64_t", INT64)            \
	_("uint8_t", UINT8)            \
	_("uint16_t", UINT16)          \
	_("uint32_t", UINT32)          \
	_("uint64_t", UINT64)          \
	_("intptr_t", INT_PSZ)         \
	_("uintptr_t", UINT_PSZ)       \
	/* From POSIX. */              \
	_("ssize_t", INT_PSZ)          \
	/* End of typedef list. */

/* Keywords (only the ones we actually care for). */
#define CTKWDEF(_)                              \
	/* Type specifiers. */                      \
	_("void", -1, CTOK_VOID)                    \
	_("_Bool", 0, CTOK_BOOL)                    \
	_("bool", 1, CTOK_BOOL)                     \
	_("char", 1, CTOK_CHAR)                     \
	_("int", 4, CTOK_INT)                       \
	_("__int8", 1, CTOK_INT)                    \
	_("__int16", 2, CTOK_INT)                   \
	_("__int32", 4, CTOK_INT)                   \
	_("__int64", 8, CTOK_INT)                   \
	_("float", 4, CTOK_FP)                      \
	_("double", 8, CTOK_FP)                     \
	_("long", 0, CTOK_LONG)                     \
	_("short", 0, CTOK_SHORT)                   \
	_("_Complex", 0, CTOK_COMPLEX)              \
	_("complex", 0, CTOK_COMPLEX)               \
	_("__complex", 0, CTOK_COMPLEX)             \
	_("__complex__", 0, CTOK_COMPLEX)           \
	_("signed", 0, CTOK_SIGNED)                 \
	_("__signed", 0, CTOK_SIGNED)               \
	_("__signed__", 0, CTOK_SIGNED)             \
	_("unsigned", 0, CTOK_UNSIGNED)             \
	/* Type qualifiers. */                      \
	_("const", 0, CTOK_CONST)                   \
	_("__const", 0, CTOK_CONST)                 \
	_("__const__", 0, CTOK_CONST)               \
	_("volatile", 0, CTOK_VOLATILE)             \
	_("__volatile", 0, CTOK_VOLATILE)           \
	_("__volatile__", 0, CTOK_VOLATILE)         \
	_("restrict", 0, CTOK_RESTRICT)             \
	_("__restrict", 0, CTOK_RESTRICT)           \
	_("__restrict__", 0, CTOK_RESTRICT)         \
	_("inline", 0, CTOK_INLINE)                 \
	_("__inline", 0, CTOK_INLINE)               \
	_("__inline__", 0, CTOK_INLINE)             \
	/* Storage class specifiers. */             \
	_("typedef", 0, CTOK_TYPEDEF)               \
	_("extern", 0, CTOK_EXTERN)                 \
	_("static", 0, CTOK_STATIC)                 \
	_("auto", 0, CTOK_AUTO)                     \
	_("register", 0, CTOK_REGISTER)             \
	/* GCC Attributes. */                       \
	_("__extension__", 0, CTOK_EXTENSION)       \
	_("__attribute", 0, CTOK_ATTRIBUTE)         \
	_("__attribute__", 0, CTOK_ATTRIBUTE)       \
	_("asm", 0, CTOK_ASM)                       \
	_("__asm", 0, CTOK_ASM)                     \
	_("__asm__", 0, CTOK_ASM)                   \
	/* MSVC Attributes. */                      \
	_("__declspec", 0, CTOK_DECLSPEC)           \
	_("__cdecl", CTCC_CDECL, CTOK_CCDECL)       \
	_("__thiscall", CTCC_THISCALL, CTOK_CCDECL) \
	_("__fastcall", CTCC_FASTCALL, CTOK_CCDECL) \
	_("__stdcall", CTCC_STDCALL, CTOK_CCDECL)   \
	_("__ptr32", 4, CTOK_PTRSZ)                 \
	_("__ptr64", 8, CTOK_PTRSZ)                 \
	/* Other type specifiers. */                \
	_("struct", 0, CTOK_STRUCT)                 \
	_("union", 0, CTOK_UNION)                   \
	_("enum", 0, CTOK_ENUM)                     \
	/* Operators. */                            \
	_("sizeof", 0, CTOK_SIZEOF)                 \
	_("__alignof", 0, CTOK_ALIGNOF)             \
	_("__alignof__", 0, CTOK_ALIGNOF)           \
	/* End of keyword list. */

/* Type info for predefined types. Size merged in. */
static CTInfo uc_ctype_typeinfo[] = {
#define CTTYINFODEF(id, sz, ct, info) CTINFO((ct), (((sz) & 0x3fu) << 10) + (info)),
#define CTTDINFODEF(name, id) CTINFO(CT_TYPEDEF, CTID_##id),
#define CTKWINFODEF(name, sz, kw) CTINFO(CT_KW, (((sz) & 0x3fu) << 10) + (kw)),
	CTTYDEF(CTTYINFODEF)
		CTTDDEF(CTTDINFODEF)
			CTKWDEF(CTKWINFODEF)
#undef CTTYINFODEF
#undef CTTDINFODEF
#undef CTKWINFODEF
				0};

/* Predefined type names collected in a single string. */
static const char *const uc_ctype_typenames =
#define CTTDNAMEDEF(name, id) name "\0"
#define CTKWNAMEDEF(name, sz, cds) name "\0"
	CTTDDEF(CTTDNAMEDEF)
		CTKWDEF(CTKWNAMEDEF)
#undef CTTDNAMEDEF
#undef CTKWNAMEDEF
	;

#define CTTYPEINFO_NUM (sizeof(uc_ctype_typeinfo) / sizeof(CTInfo) - 1)
#ifdef LUAJIT_CTYPE_CHECK_ANCHOR
#define CTTYPETAB_MIN CTTYPEINFO_NUM
#else
#define CTTYPETAB_MIN 128
#endif

/* -- C type interning ---------------------------------------------------- */

#define ct_hashtype(info, size) (hashrot(info, size) & CTHASH_MASK)

static inline uint32_t
ct_hashname(uc_value_t *uv)
{
	uint8_t *u8 = NULL;
	size_t len = 0;
	uint32_t h;

	h = ucv_type(uv);

	switch (h)
	{
	case UC_STRING:
		u8 = (uint8_t *)ucv_string_get(uv);
		len = ucv_string_length(uv);
		break;

	default:
		assert(0);
	}

	while (len > 0)
	{
		h = h * 129 + (*u8++) + 0x9e370001UL;
		len--;
	}

	return h & CTHASH_MASK;
}

/* Create new type element. */
CTypeID uc_ctype_new(CTState *cts, CType **ctp)
{
	CTypeID id = cts->vtab.count;
	CType *ct;
	if (UC_UNLIKELY(id >= CTID_MAX))
	{
		uc_vm_raise_exception(cts->vm, EXCEPTION_RUNTIME,
			"FFI type table overflow");
		return 0;
	}

	uc_vector_grow(&cts->vtab);
	*ctp = ct = &cts->vtab.entries[cts->vtab.count++];
	ct->info = 0;
	ct->size = 0;
	ct->sib = 0;
	ct->next = 0;
	ct->uv_name = NULL;
	return id;
}

/* Intern a type element. */
CTypeID uc_ctype_intern(CTState *cts, CTInfo info, CTSize size)
{
	uint32_t h = ct_hashtype(info, size);
	CTypeID id = cts->hash[h];
	uc_assertCTS(cts->L, "uninitialized cts->L");
	while (id)
	{
		CType *ct = ctype_get(cts, id);
		if (ct->info == info && ct->size == size)
			return id;
		id = ct->next;
	}
	uc_vector_grow(&cts->vtab);
	id = cts->vtab.count++;
	cts->vtab.entries[id].info = info;
	cts->vtab.entries[id].size = size;
	cts->vtab.entries[id].sib = 0;
	cts->vtab.entries[id].next = cts->hash[h];
	cts->vtab.entries[id].uv_name = NULL;
	cts->hash[h] = (CTypeID1)id;
	return id;
}

/* Add type element to hash table. */
static void ctype_addtype(CTState *cts, CType *ct, CTypeID id)
{
	uint32_t h = ct_hashtype(ct->info, ct->size);
	ct->next = cts->hash[h];
	cts->hash[h] = (CTypeID1)id;
}

/* Add named element to hash table. */
void uc_ctype_addname(CTState *cts, CType *ct, CTypeID id)
{
	uint32_t h = ct_hashname(ct->uv_name);
	ct->next = cts->hash[h];
	cts->hash[h] = (CTypeID1)id;
}

/* Get a C type by name, matching the type mask. */
CTypeID uc_ctype_getname(CTState *cts, CType **ctp, uc_value_t *name, uint32_t tmask)
{
	CTypeID id = cts->hash[ct_hashname(name)];
	while (id)
	{
		CType *ct = ctype_get(cts, id);

		if (ucv_is_equal(ct->uv_name, name) && ((tmask >> ctype_type(ct->info)) & 1))
		{
			*ctp = ct;
			return id;
		}
		id = ct->next;
	}
	*ctp = &cts->vtab.entries[0]; /* Simplify caller logic. ctype_get() would assert. */
	return 0;
}

/* Get a struct/union/enum/function field by name. */
CType *uc_ctype_getfieldq(CTState *cts, CType *ct, uc_value_t *name, CTSize *ofs,
						  CTInfo *qual)
{
	while (ct->sib)
	{
		ct = ctype_get(cts, ct->sib);
		if (ucv_is_equal(ct->uv_name, name))
		{
			*ofs = ct->size;
			return ct;
		}
		if (ctype_isxattrib(ct->info, CTA_SUBTYPE))
		{
			CType *fct, *cct = ctype_child(cts, ct);
			CTInfo q = 0;
			while (ctype_isattrib(cct->info))
			{
				if (ctype_attrib(cct->info) == CTA_QUAL)
					q |= cct->size;
				cct = ctype_child(cts, cct);
			}
			fct = uc_ctype_getfieldq(cts, cct, name, ofs, qual);
			if (fct)
			{
				if (qual)
					*qual |= q;
				*ofs += ct->size;
				return fct;
			}
		}
	}
	return NULL; /* Not found. */
}

/* -- C type information -------------------------------------------------- */

/* Follow references and get raw type for a C type ID. */
CType *uc_ctype_rawref(CTState *cts, CTypeID id)
{
	CType *ct = ctype_get(cts, id);
	while (ctype_isattrib(ct->info) || ctype_isref(ct->info))
		ct = ctype_child(cts, ct);
	return ct;
}

/* Get size for a C type ID. Does NOT support VLA/VLS. */
CTSize uc_ctype_size(CTState *cts, CTypeID id)
{
	CType *ct = ctype_raw(cts, id);
	return ctype_hassize(ct->info) ? ct->size : CTSIZE_INVALID;
}

/* Get size for a variable-length C type. Does NOT support other C types. */
CTSize uc_ctype_vlsize(CTState *cts, CType *ct, CTSize nelem)
{
	uint64_t xsz = 0;
	if (ctype_isstruct(ct->info))
	{
		CTypeID arrid = 0, fid = ct->sib;
		xsz = ct->size; /* Add the struct size. */
		while (fid)
		{
			CType *ctf = ctype_get(cts, fid);
			if (ctype_type(ctf->info) == CT_FIELD)
				arrid = ctype_cid(ctf->info); /* Remember last field of VLS. */
			fid = ctf->sib;
		}
		ct = ctype_raw(cts, arrid);
	}
	uc_assertCTS(ctype_isvlarray(ct->info), "VLA expected");
	ct = ctype_rawchild(cts, ct); /* Get array element. */
	uc_assertCTS(ctype_hassize(ct->info), "bad VLA without size");
	/* Calculate actual size of VLA and check for overflow. */
	xsz += (uint64_t)ct->size * nelem;
	return xsz < 0x80000000u ? (CTSize)xsz : CTSIZE_INVALID;
}

/* Get type, qualifiers, size and alignment for a C type ID. */
CTInfo uc_ctype_info(CTState *cts, CTypeID id, CTSize *szp)
{
	CTInfo qual = 0;
	CType *ct = ctype_get(cts, id);
	for (;;)
	{
		CTInfo info = ct->info;
		if (ctype_isenum(info))
		{
			/* Follow child. Need to look at its attributes, too. */
		}
		else if (ctype_isattrib(info))
		{
			if (ctype_isxattrib(info, CTA_QUAL))
				qual |= ct->size;
			else if (ctype_isxattrib(info, CTA_ALIGN) && !(qual & CTFP_ALIGNED))
				qual |= CTFP_ALIGNED + CTALIGN(ct->size);
		}
		else
		{
			if (!(qual & CTFP_ALIGNED))
				qual |= (info & CTF_ALIGN);
			qual |= (info & ~(CTF_ALIGN | CTMASK_CID));
			uc_assertCTS(ctype_hassize(info) || ctype_isfunc(info),
						 "ctype without size");
			*szp = ctype_isfunc(info) ? CTSIZE_INVALID : ct->size;
			break;
		}
		ct = ctype_get(cts, ctype_cid(info));
	}
	return qual;
}

/* Ditto, but follow a reference. */
CTInfo uc_ctype_info_raw(CTState *cts, CTypeID id, CTSize *szp)
{
	CType *ct = ctype_get(cts, id);
	if (ctype_isref(ct->info))
		id = ctype_cid(ct->info);
	return uc_ctype_info(cts, id, szp);
}

/* -- C type representation ----------------------------------------------- */

/* Fixed max. length of a C type representation. */
#define CTREPR_MAX 512

typedef struct CTRepr
{
	char *pb, *pe;
	CTState *cts;
	int needsp;
	int ok;
	char buf[CTREPR_MAX];
} CTRepr;

/* Prepend string. */
static void ctype_prepstr(CTRepr *ctr, const char *str, size_t len)
{
	char *p = ctr->pb;
	if (ctr->buf + len + 1 > p)
	{
		ctr->ok = 0;
		return;
	}
	if (ctr->needsp)
		*--p = ' ';
	ctr->needsp = 1;
	p -= len;
	while (len-- > 0)
		p[len] = str[len];
	ctr->pb = p;
}

#define ctype_preplit(ctr, str) ctype_prepstr((ctr), "" str, sizeof(str) - 1)

/* Prepend char. */
static void ctype_prepc(CTRepr *ctr, int c)
{
	if (ctr->buf >= ctr->pb)
	{
		ctr->ok = 0;
		return;
	}
	*--ctr->pb = c;
}

/* Prepend number. */
static void ctype_prepnum(CTRepr *ctr, uint32_t n)
{
	char *p = ctr->pb;
	if (ctr->buf + 10 + 1 > p)
	{
		ctr->ok = 0;
		return;
	}
	do
	{
		*--p = (char)('0' + n % 10);
	} while (n /= 10);
	ctr->pb = p;
	ctr->needsp = 0;
}

/* Append char. */
static void ctype_appc(CTRepr *ctr, int c)
{
	if (ctr->pe >= ctr->buf + CTREPR_MAX)
	{
		ctr->ok = 0;
		return;
	}
	*ctr->pe++ = c;
}

/* Append number. */
static void ctype_appnum(CTRepr *ctr, uint32_t n)
{
	char buf[10];
	char *p = buf + sizeof(buf);
	char *q = ctr->pe;
	if (q > ctr->buf + CTREPR_MAX - 10)
	{
		ctr->ok = 0;
		return;
	}
	do
	{
		*--p = (char)('0' + n % 10);
	} while (n /= 10);
	do
	{
		*q++ = *p++;
	} while (p < buf + sizeof(buf));
	ctr->pe = q;
}

/* Prepend qualifiers. */
static void ctype_prepqual(CTRepr *ctr, CTInfo info)
{
	if ((info & CTF_VOLATILE))
		ctype_preplit(ctr, "volatile");
	if ((info & CTF_CONST))
		ctype_preplit(ctr, "const");
}

/* Prepend named type. */
static void ctype_preptype(CTRepr *ctr, CType *ct, CTInfo qual, const char *t)
{
	if (ct->uv_name)
	{
		ctype_prepstr(ctr, ucv_string_get(ct->uv_name), ucv_string_length(ct->uv_name));
	}
	else
	{
		if (ctr->needsp)
			ctype_prepc(ctr, ' ');
		ctype_prepnum(ctr, ctype_typeid(ctr->cts, ct));
		ctr->needsp = 1;
	}
	ctype_prepstr(ctr, t, strlen(t));
	ctype_prepqual(ctr, qual);
}

static void ctype_repr(CTRepr *ctr, CTypeID id)
{
	CType *ct = ctype_get(ctr->cts, id);
	CTInfo qual = 0;
	int ptrto = 0;
	for (;;)
	{
		CTInfo info = ct->info;
		CTSize size = ct->size;
		switch (ctype_type(info))
		{
		case CT_NUM:
			if ((info & CTF_BOOL))
			{
				ctype_preplit(ctr, "bool");
			}
			else if ((info & CTF_FP))
			{
				if (size == sizeof(double))
					ctype_preplit(ctr, "double");
				else if (size == sizeof(float))
					ctype_preplit(ctr, "float");
				else
					ctype_preplit(ctr, "long double");
			}
			else if (size == 1)
			{
				if (!((info ^ CTF_UCHAR) & CTF_UNSIGNED))
					ctype_preplit(ctr, "char");
				else if (CTF_UCHAR)
					ctype_preplit(ctr, "signed char");
				else
					ctype_preplit(ctr, "unsigned char");
			}
			else if (size < 8)
			{
				if (size == 4)
					ctype_preplit(ctr, "int");
				else
					ctype_preplit(ctr, "short");
				if ((info & CTF_UNSIGNED))
					ctype_preplit(ctr, "unsigned");
			}
			else
			{
				ctype_preplit(ctr, "_t");
				ctype_prepnum(ctr, size * 8);
				ctype_preplit(ctr, "int");
				if ((info & CTF_UNSIGNED))
					ctype_prepc(ctr, 'u');
			}
			ctype_prepqual(ctr, (qual | info));
			return;
		case CT_VOID:
			ctype_preplit(ctr, "void");
			ctype_prepqual(ctr, (qual | info));
			return;
		case CT_STRUCT:
			ctype_preptype(ctr, ct, qual, (info & CTF_UNION) ? "union" : "struct");
			return;
		case CT_ENUM:
			if (id == CTID_CTYPEID)
			{
				ctype_preplit(ctr, "ctype");
				return;
			}
			ctype_preptype(ctr, ct, qual, "enum");
			return;
		case CT_ATTRIB:
			if (ctype_attrib(info) == CTA_QUAL)
				qual |= size;
			break;
		case CT_PTR:
			if ((info & CTF_REF))
			{
				CType *ct_child = ctype_get(ctr->cts, ctype_cid(info));
				if (ctype_type(ct_child->info) != CT_PTR)
					ctype_prepc(ctr, '&');
			}
			else
			{
				ctype_prepqual(ctr, (qual | info));
				if (UC_64 && size == 4)
					ctype_preplit(ctr, "__ptr32");
				ctype_prepc(ctr, '*');
			}
			qual = 0;
			ptrto = 1;
			ctr->needsp = 1;
			break;
		case CT_ARRAY:
			if (ctype_isrefarray(info))
			{
				ctr->needsp = 1;
				if (ptrto)
				{
					ptrto = 0;
					ctype_prepc(ctr, '(');
					ctype_appc(ctr, ')');
				}
				ctype_appc(ctr, '[');
				if (size != CTSIZE_INVALID)
				{
					CTSize csize = ctype_child(ctr->cts, ct)->size;
					ctype_appnum(ctr, csize ? size / csize : 0);
				}
				else if ((info & CTF_VLA))
				{
					ctype_appc(ctr, '?');
				}
				ctype_appc(ctr, ']');
			}
			else if ((info & CTF_COMPLEX))
			{
				if (size == 2 * sizeof(float))
					ctype_preplit(ctr, "float");
				ctype_preplit(ctr, "complex");
				return;
			}
			else
			{
				ctype_preplit(ctr, ")))");
				ctype_prepnum(ctr, size);
				ctype_preplit(ctr, "__attribute__((vector_size(");
			}
			break;
		case CT_FUNC:
			ctr->needsp = 1;
			if (ptrto)
			{
				ptrto = 0;
				ctype_prepc(ctr, '(');
				ctype_appc(ctr, ')');
			}
			ctype_appc(ctr, '(');
			ctype_appc(ctr, ')');
			break;
		default:
			uc_assertG_(ctr->cts->g, 0, "bad ctype %08x", info);
			break;
		}
		ct = ctype_get(ctr->cts, ctype_cid(info));
	}
}

/* Return a printable representation of a C type. */
uc_value_t *uc_ctype_repr(uc_vm_t *vm, CTypeID id, uc_value_t *name)
{
	CTRepr ctr;
	ctr.pb = ctr.pe = &ctr.buf[CTREPR_MAX / 2];
	ctr.cts = ctype_cts(vm);
	ctr.ok = 1;
	ctr.needsp = 0;
	if (name)
		ctype_prepstr(&ctr, ucv_string_get(name), ucv_string_length(name));
	ctype_repr(&ctr, id);
	if (UC_UNLIKELY(!ctr.ok))
		return ucv_string_new("?");
	return ucv_string_new_length(ctr.pb, ctr.pe - ctr.pb);
}

/* Convert int64_t/uint64_t to string with 'LL' or 'ULL' suffix. */
uc_value_t *uc_ctype_repr_int64(uint64_t n, int isunsigned)
{
	char buf[1 + 20 + 3];
	char *p = buf + sizeof(buf);
	int sign = 0;
	*--p = 'L';
	*--p = 'L';
	if (isunsigned)
	{
		*--p = 'U';
	}
	else if ((int64_t)n < 0)
	{
		n = ~n + 1u;
		sign = 1;
	}
	do
	{
		*--p = (char)('0' + n % 10);
	} while (n /= 10);
	if (sign)
		*--p = '-';
	return ucv_string_new_length(p, (size_t)(buf + sizeof(buf) - p));
}

/* Convert complex to string with 'i' or 'I' suffix. */
uc_value_t *uc_ctype_repr_complex(void *sp, CTSize size)
{
	uc_stringbuf_t *sb = ucv_stringbuf_new();
	TValue re, im;

	/* Only the three complex widths are representable. Anything else must not
	 * fall through to the float case below, since reinterpreting the value
	 * turns the re / im conversions back into an array read and recurses until
	 * the stack is exhausted. */
	if (size != 2 * sizeof(float) &&
	    size != 2 * sizeof(double) &&
	    size != 2 * sizeof(long double))
		return ucv_string_new("?");

	if (size == 2 * sizeof(double))
	{
		re.n = *(double *)sp;
		im.n = ((double *)sp)[1];
	}
	else
	{
		re.n = (double)*(float *)sp;
		im.n = (double)((float *)sp)[1];
	}
	ucv_stringbuf_printf(sb, "%.14g", re.n);
	if (!(im.u32.hi & 0x80000000u) || im.n != im.n)
		ucv_stringbuf_append(sb, "+");
	ucv_stringbuf_printf(sb, "%.14g", im.n);
	if (sb->buf[sb->bpos - 1] >= 'a')
		ucv_stringbuf_append(sb, "I");
	else
		ucv_stringbuf_append(sb, "i");
	return ucv_stringbuf_finish(sb);
}

/* -- C type state -------------------------------------------------------- */

static void free_ctypes(void *ud)
{
	CTState *cts = ud;

	while (cts->vtab.count)
		ucv_put(cts->vtab.entries[--cts->vtab.count].uv_name);

	free(cts->vtab.entries);
	free(cts->vcbid.entries);
	free(cts);
}

/* Initialize C type table and state. */
CTState *uc_ctype_init(uc_vm_t *vm)
{
	CTState *cts = xalloc(sizeof(CTState));
	CType *ct; // = lj_mem_newvec(L, CTTYPETAB_MIN, CType);
	const char *name = uc_ctype_typenames;
	CTypeID id;
	memset(cts, 0, sizeof(CTState));
	// cts->tab = ct;
	// cts->sizetab = CTTYPETAB_MIN;
	// cts->top = CTTYPEINFO_NUM;
	//cts->L = NULL;
	//cts->g = G(L);
	cts->vm = vm;

	for (id = 0; id < CTTYPEINFO_NUM; id++, ct++)
	{
		CTInfo info = uc_ctype_typeinfo[id];
		uc_vector_grow(&cts->vtab);
		ct = &cts->vtab.entries[cts->vtab.count++];
		ct->size = (CTSize)((int32_t)(info << 16) >> 26);
		ct->info = info & 0xffff03ffu;
		ct->sib = 0;
		if (ctype_type(info) == CT_KW || ctype_istypedef(info))
		{
			size_t len = strlen(name);
			uc_value_t *str = ucv_string_new_length(name, len);
			ctype_setname(ct, str);
			ucv_put(str);
			name += len + 1;
			uc_ctype_addname(cts, ct, id);
		}
		else
		{
			ct->uv_name = NULL;
			ct->next = 0;
			if (!ctype_isenum(info))
				ctype_addtype(cts, ct, id);
		}
	}

	//setmref(G(L)->ctype_state, cts);

	uc_resource_type_t *cts_res;

	cts_res = ucv_resource_type_add(vm, "ffi.types", NULL, free_ctypes);

	uc_vm_registry_set(vm, "ffi.types", ucv_resource_new(cts_res, cts));

	return cts;
}

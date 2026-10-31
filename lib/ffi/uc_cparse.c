/*
** C declaration parser.
** Copyright (C) 2005-2025 Mike Pall. See Copyright Notice below.
**
** This file contains derived work from LuaJIT's FFI C parser (uc_cparse.c).
**
** Modifications:
** - Adapted VM interactions to use ucode's API (uc_vm_t, uc_value_t, etc.)
** - Removed JIT-specific code and dependencies
** - Adapted error handling to use ucode exceptions
**
** See NOTICE and ATTRIBUTION.md for complete attribution details.
*/

#include <ctype.h>
#include "ucode/vm.h"
#include "uc_ctype.h"
#include "uc_cparse.h"

#include "ucode/util.h"
#include "ucode/internal/types.h"
#include "ucode/internal/util.h"

/*
** Important note: this is NOT a validating C parser! This is a minimal
** C declaration parser, solely for use by the LuaJIT FFI.
**
** It ought to return correct results for properly formed C declarations,
** but it may accept some invalid declarations, too (and return nonsense).
** Also, it shows rather generic error messages to avoid unnecessary bloat.
** If in doubt, please check the input against your favorite C compiler.
*/

/* Assertions disabled for production build. */
#define uc_assertCP(c, ...) ((void)0)

/* Check if two function types are structurally equivalent. */
static bool
ctype_func_is_equiv(CTState *cts, CType *ct1, CType *ct2)
{
	CType *p1, *p2;

	if (ct1->info != ct2->info || ct1->size != ct2->size) {
		return false;
	}

	/* Compare parameter chains. */
	p1 = ctype_get(cts, ct1->sib);
	p2 = ctype_get(cts, ct2->sib);

	while (p1 && p2) {
		/* Stop at non-field entries (end of parameters or attributes). */
		if (!ctype_isfield(p1->info) || !ctype_isfield(p2->info))
			break;
		if (p1->info != p2->info || p1->size != p2->size) {
			return false;
		}
		p1 = ctype_get(cts, p1->sib);
		p2 = ctype_get(cts, p2->sib);
	}

	return (p1 == p2);
}

/* -- Miscellaneous ------------------------------------------------------- */

/* Match string against a C literal. */
#define cp_str_is(str, k) \
	(ucv_string_length(str) == sizeof(k) - 1 && !memcmp(ucv_string_get(str), k, sizeof(k) - 1))

/* Check string against a linear list of matches. */
int uc_cparse_case(uc_value_t *str, const char *match)
{
	size_t len;
	int n;
	for (n = 0; (len = *match++); n++, match += len)
	{
		if (ucv_string_length(str) == len && !memcmp(match, ucv_string_get(str), len))
			return n;
	}
	return -1;
}

/* -- C lexer ------------------------------------------------------------- */

/* C lexer token names. */
static const char *const ctoknames[] = {
#define CTOKSTR(name, str) str,
	CTOKDEF(CTOKSTR)
#undef CTOKSTR
		NULL};

/* Forward declaration. */
static void cp_err(CPState *cp, const char *em);

static const char *cp_tok2str(CPState *cp, CPToken tok)
{
	char *e;
	uc_assertCP(tok < CTOK_FIRSTDECL, "bad CPToken %d", tok);
	if (tok > CTOK_OFS)
		return ctoknames[tok - CTOK_OFS - 1];
	else if (!iscntrl(tok))
	{
		xasprintf(&e, "%c", tok);
		return e;
	}
	else
	{
		xasprintf(&e, "char(%d)", tok);
		return e;
	}
}

/* End-of-line? */
static UC_AINLINE int cp_iseol(CPChar c)
{
	return (c == '\n' || c == '\r');
}

/* Peek next raw character. */
static UC_AINLINE CPChar cp_rawpeek(CPState *cp)
{
	return (CPChar)(uint8_t)(*cp->p);
}

static UC_NOINLINE CPChar cp_get_bs(CPState *cp);

/* Get next character. */
static UC_AINLINE CPChar cp_get(CPState *cp)
{
	cp->c = (CPChar)(uint8_t)(*cp->p++);
	if (UC_LIKELY(cp->c != '\\'))
		return cp->c;
	return cp_get_bs(cp);
}

/* Transparently skip backslash-escaped line breaks. */
static UC_NOINLINE CPChar cp_get_bs(CPState *cp)
{
	CPChar c2, c = cp_rawpeek(cp);
	if (!cp_iseol(c))
		return cp->c;
	cp->p++;
	c2 = cp_rawpeek(cp);
	if (cp_iseol(c2) && c2 != c)
		cp->p++;
	cp->linenumber++;
	return cp_get(cp);
}

/* Save character in buffer. */
static UC_AINLINE void cp_save(CPState *cp, CPChar c)
{
	uc_stringbuf_printf(&cp->pb, "%c", c);
}

/* Skip line break. Handles "\n", "\r", "\r\n" or "\n\r". */
static void cp_newline(CPState *cp)
{
	CPChar c = cp_rawpeek(cp);
	if (cp_iseol(c) && c != cp->c)
		cp->p++;
	cp->linenumber++;
}

static void __attribute__((format(printf, 3, 0)))
cp_errmsg(CPState *cp, CPToken tok, const char *em, ...)
{
	const char *tokstr;
	char *s, *msg;
	va_list argp;

	if (cp->error)
		return;

	if (tok == 0)
	{
		tokstr = NULL;
	}
	else if (tok == CTOK_IDENT || tok == CTOK_INTEGER || tok == CTOK_STRING ||
			 tok >= CTOK_FIRSTDECL)
	{
		if (cp->pb.bpos == 0)
			cp_save(cp, '$');
		cp_save(cp, '\0');
		tokstr = cp->pb.buf;
	}
	else
	{
		tokstr = cp_tok2str(cp, tok);
	}
	va_start(argp, em);
	xvasprintf(&msg, em, argp);
	va_end(argp);
	if (tokstr)
	{
		xasprintf(&s, "%s near '%s'", msg, tokstr);
		free(msg);
		msg = s;
	}
	if (cp->linenumber > 1)
	{
		xasprintf(&s, "%s at line %d", msg, cp->linenumber);
		free(msg);
		msg = s;
	}

	cp->error = msg;
}

static void cp_err_token(CPState *cp, CPToken tok)
{
	cp_errmsg(cp, cp->tok, "'%s' expected", cp_tok2str(cp, tok));
}

static void cp_err_badidx(CPState *cp, CType *ct)
{
	uc_value_t *s = uc_ctype_repr(cp->uv_vm, ctype_typeid(cp->cts, ct), NULL);
	cp_errmsg(cp, 0, "'%s' cannot be indexed", ucv_string_get(s));
	ucv_put(s);
}

static void cp_err(CPState *cp, const char *em)
{
	cp_errmsg(cp, 0, "%s", em);
}

/* -- Main lexical scanner ------------------------------------------------ */

static inline bool is_ident(uint8_t c)
{
	return (c >= 48 && c <= 57) ||
		   (c >= 65 && c <= 90) ||
		   (c == '_') ||
		   (c >= 97 && c <= 122) ||
		   (c >= 128);
}

/* Parse number literal. Only handles int32_t/uint32_t right now. */
static CPToken cp_number(CPState *cp)
{
	unsigned long long val;
	bool sign = false;
	int base = 0;
	char *s, *e;

	do
	{
		cp_save(cp, cp->c);
	} while (is_ident(cp_get(cp)));

	cp_save(cp, '\0');

	s = cp->pb.buf;

	while (isspace(*s))
		s++;

	if (*s == '-')
	{
		sign = true;
		s++;
	}
	else if (*s == '+')
	{
		s++;
	}

	if (*s == '0')
	{
		switch (s[1] | 32)
		{
		case 'x':
			base = 16;
			s += 2;
			break;

		case 'o':
			base = 8;
			s += 2;
			break;

		case 'b':
			base = 2;
			s += 2;
			break;
		}
	}

	val = strtoull(s, &e, base);

	/* handle potential suffix */
	if (!strcasecmp(e, "ull") || !strcasecmp(e, "llu"))
	{
		if (sizeof(unsigned long long) > sizeof(uint32_t) && !(cp->mode & CPARSE_MODE_SKIP))
			cp_errmsg(cp, CTOK_INTEGER, "malformed number");

		cp->val.id = CTID_UINT32;
		e += 3;
	}
	else if (!strcasecmp(e, "ll"))
	{
		if (sizeof(long long) > sizeof(int32_t) && !(cp->mode & CPARSE_MODE_SKIP))
			cp_errmsg(cp, CTOK_INTEGER, "malformed number");

		cp->val.id = CTID_INT32;
		e += 2;
	}
	else if (!strcasecmp(e, "ul") || !strcasecmp(e, "lu"))
	{
		if (sizeof(unsigned long) > sizeof(uint32_t) && !(cp->mode & CPARSE_MODE_SKIP))
			cp_errmsg(cp, CTOK_INTEGER, "malformed number");

		cp->val.id = CTID_UINT32;
		e += 2;
	}
	else if ((*e | 32) == 'u')
	{
		cp->val.id = CTID_UINT32;
		e++;
	}
	else if ((*e | 32) == 'l')
	{
		if (sizeof(long) > sizeof(int32_t) && !(cp->mode & CPARSE_MODE_SKIP))
			cp_errmsg(cp, CTOK_INTEGER, "malformed number");

		cp->val.id = CTID_INT32;
		e++;
	}
	else if ((*e | 32) == 'i')
	{
		if (!(cp->mode & CPARSE_MODE_SKIP))
			cp_errmsg(cp, CTOK_INTEGER, "malformed number");

		e++;
	}
	else
	{
		cp->val.id = CTID_INT32;
	}

	while (isspace(*e))
		e++;

	if (*e)
		cp_errmsg(cp, CTOK_INTEGER, "malformed number");

	cp->val.u32 = sign ? (uint32_t)-val : (uint32_t)val;
	return CTOK_INTEGER;
}

/* Parse identifier or keyword. */
static CPToken cp_ident(CPState *cp)
{
	do
	{
		cp_save(cp, cp->c);
	} while (is_ident(cp_get(cp)));
	ucv_put(cp->uv_str);
	cp->uv_str = ucv_string_new(cp->pb.buf);
	// cp->str = lj_buf_str(cp->L, &cp->sb);
	cp->val.id = uc_ctype_getname(cp->cts, &cp->ct, cp->uv_str, cp->tmask);

	if (ctype_type(cp->ct->info) == CT_KW)
		return ctype_cid(cp->ct->info);
	return CTOK_IDENT;
}

/* Parse parameter. */
static CPToken cp_param(CPState *cp)
{
	CPChar c = cp_get(cp);
	// TValue *o = cp->param;
	uc_value_t **uv = cp->uv_param;
	if (is_ident(c) || c == '$') /* Reserve $xyz for future extensions. */ {
		cp_errmsg(cp, c, "syntax error");
		return CTOK_EOF;
	}
	if (!uv || uv >= &cp->uv_vm->stack.entries[cp->uv_vm->stack.count]) {
		cp_err(cp, "wrong number of type parameters");
		return CTOK_EOF;
	}
	cp->uv_param = uv + 1;
	if (ucv_type(*uv) == UC_STRING)
	{
		ucv_put(cp->uv_str);
		cp->uv_str = ucv_get(*uv);
		cp->val.id = 0;
		cp->ct = &cp->cts->vtab.entries[0];
		return CTOK_IDENT;
	}
	else if (ucv_type(*uv) == UC_INTEGER)
	{
		cp->val.i32 = (int32_t)ucv_int64_get(*uv);
		cp->val.id = CTID_INT32;
		return CTOK_INTEGER;
	}
	else
	{
		void *ctype = ucv_resource_dataptr(*uv, "ffi.ctype");
		if (!ctype)
			cp_errmsg(cp, 0, "type parameter expected, got %s", ucv_typename(*uv));
		// lj_err_argtype(cp->L, (int)(o-cp->L->base)+1, "type parameter");
		cp->val.id = (CTypeID)(uintptr_t)ctype;
		return '$';
	}
}

/* Parse string or character constant. */
static CPToken cp_string(CPState *cp)
{
	CPChar delim = cp->c;
	cp_get(cp);
	while (cp->c != delim)
	{
		CPChar c = cp->c;
		if (c == '\0') {
			cp_errmsg(cp, CTOK_EOF, "unfinished string");
			return CTOK_EOF;
		}
		if (c == '\\')
		{
			c = cp_get(cp);
			switch (c)
			{
			case '\0':
				cp_errmsg(cp, CTOK_EOF, "unfinished string");
				return CTOK_EOF;
			case 'a':
				c = '\a';
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'v':
				c = '\v';
				break;
			case 'e':
				c = 27;
				break;
			case 'x':
				c = 0;
				while (isxdigit(cp_get(cp)))
					c = (c << 4) + (isdigit(cp->c) ? cp->c - '0' : (cp->c & 15) + 9);
				cp_save(cp, (c & 0xff));
				continue;
			default:
				if (isdigit(c))
				{
					c -= '0';
					if (isdigit(cp_get(cp)))
					{
						c = c * 8 + (cp->c - '0');
						if (isdigit(cp_get(cp)))
						{
							c = c * 8 + (cp->c - '0');
							cp_get(cp);
						}
					}
					cp_save(cp, (c & 0xff));
					continue;
				}
				break;
			}
		}
		cp_save(cp, c);
		cp_get(cp);
	}
	cp_get(cp);
	if (delim == '"')
	{
		// FIXME: consider ucv_stringbuf_new
		ucv_put(cp->uv_str);
		cp->uv_str = ucv_string_new(cp->pb.buf);
		// cp->str = lj_buf_str(cp->L, &cp->sb);
		return CTOK_STRING;
	}
	else
	{
		if (uc_stringbuf_length(&cp->pb) != 1)
			cp_err_token(cp, '\'');
		cp->val.i32 = (int32_t)(char)*cp->pb.buf;
		cp->val.id = CTID_INT32;
		return CTOK_INTEGER;
	}
}

/* Skip C comment. */
static void cp_comment_c(CPState *cp)
{
	do
	{
		if (cp_get(cp) == '*')
		{
			do
			{
				if (cp_get(cp) == '/')
				{
					cp_get(cp);
					return;
				}
			} while (cp->c == '*');
		}
		if (cp_iseol(cp->c))
			cp_newline(cp);
	} while (cp->c != '\0');
}

/* Skip C++ comment. */
static void cp_comment_cpp(CPState *cp)
{
	while (!cp_iseol(cp_get(cp)) && cp->c != '\0')
		;
}

/* Lexical scanner for C. Only a minimal subset is implemented. */
static CPToken cp_next_(CPState *cp)
{
	//lj_buf_reset(&cp->sb);
	if (cp->pb.buf)
		uc_stringbuf_reset(&cp->pb);

	for (;;)
	{
		if (is_ident(cp->c))
			return isdigit(cp->c) ? cp_number(cp) : cp_ident(cp);
		switch (cp->c)
		{
		case '\n':
		case '\r':
			cp_newline(cp); /* fallthrough. */
		case ' ':
		case '\t':
		case '\v':
		case '\f':
			cp_get(cp);
			break;
		case '"':
		case '\'':
			return cp_string(cp);
		case '/':
			if (cp_get(cp) == '*')
				cp_comment_c(cp);
			else if (cp->c == '/')
				cp_comment_cpp(cp);
			else
				return '/';
			break;
		case '|':
			if (cp_get(cp) != '|')
				return '|';
			cp_get(cp);
			return CTOK_OROR;
		case '&':
			if (cp_get(cp) != '&')
				return '&';
			cp_get(cp);
			return CTOK_ANDAND;
		case '=':
			if (cp_get(cp) != '=')
				return '=';
			cp_get(cp);
			return CTOK_EQ;
		case '!':
			if (cp_get(cp) != '=')
				return '!';
			cp_get(cp);
			return CTOK_NE;
		case '<':
			if (cp_get(cp) == '=')
			{
				cp_get(cp);
				return CTOK_LE;
			}
			else if (cp->c == '<')
			{
				cp_get(cp);
				return CTOK_SHL;
			}
			return '<';
		case '>':
			if (cp_get(cp) == '=')
			{
				cp_get(cp);
				return CTOK_GE;
			}
			else if (cp->c == '>')
			{
				cp_get(cp);
				return CTOK_SHR;
			}
			return '>';
		case '-':
			if (cp_get(cp) != '>')
				return '-';
			cp_get(cp);
			return CTOK_DEREF;
		case '$':
			return cp_param(cp);
		case '\0':
			return CTOK_EOF;
		default:
		{
			CPToken c = cp->c;
			cp_get(cp);
			return c;
		}
		}
	}
}

static UC_NOINLINE CPToken cp_next(CPState *cp)
{
	return (cp->tok = cp_next_(cp));
}

/* -- C parser ------------------------------------------------------------ */

/* Namespaces for resolving identifiers. */
#define CPNS_DEFAULT \
	((1u << CT_KW) | (1u << CT_TYPEDEF) | (1u << CT_FUNC) | (1u << CT_EXTERN) | (1u << CT_CONSTVAL))
#define CPNS_STRUCT ((1u << CT_KW) | (1u << CT_STRUCT) | (1u << CT_ENUM))

typedef CTypeID CPDeclIdx; /* Index into declaration stack. */
typedef uint32_t CPscl;	   /* Storage class flags. */

/* Type declaration context. */
typedef struct CPDecl
{
	CPDeclIdx top;					   /* Top of declaration stack. */
	CPDeclIdx pos;					   /* Insertion position in declaration chain. */
	CPDeclIdx specpos;				   /* Saved position for declaration specifier. */
	uint32_t mode;					   /* Declarator mode. */
	CPState *cp;					   /* C parser state. */
	CTypeID nameid;					   /* Existing typedef for declared identifier. */
	CTInfo attr;					   /* Attributes. */
	CTInfo fattr;					   /* Function attributes. */
	CTInfo specattr;				   /* Saved attributes. */
	CTInfo specfattr;				   /* Saved function attributes. */
	CTSize bits;					   /* Field size in bits (if any). */
	CType stack[CPARSE_MAX_DECLSTACK]; /* Type declaration stack. */
	uc_value_t *uv_name;
	uc_value_t *uv_redir;
} CPDecl;

/* Forward declarations. */
static CPscl cp_decl_spec(CPState *cp, CPDecl *decl, CPscl scl);
static void cp_declarator(CPState *cp, CPDecl *decl);
static CTypeID cp_decl_abstract(CPState *cp);

/* Initialize C parser state. Caller must set up: L, p, srcname, mode. */
static void cp_init(CPState *cp)
{
	cp->error = NULL;
	cp->linenumber = 1;
	cp->depth = 0;
	cp->curpack = 0;
	cp->packstack[0] = 255;
	cp->pb.bpos = 0;
	cp->pb.buf = 0;
	cp->pb.size = 0;
	cp->uv_str = NULL;
	// lj_buf_init(cp->L, &cp->sb);
	uc_assertCP(cp->p != NULL, "uninitialized cp->p");
	cp_get(cp); /* Read-ahead first char. */
	cp->tok = 0;
	cp->tmask = CPNS_DEFAULT;
	cp_next(cp); /* Read-ahead first token. */
}

/* Cleanup C parser state. */
static void cp_cleanup(CPState *cp)
{
	// global_State *g = G(cp->L);
	// lj_buf_free(g, &cp->sb);
	ucv_put(cp->uv_str);
	free(cp->pb.buf);
	free(cp->error);
}

/* Check and consume optional token. */
static int cp_opt(CPState *cp, CPToken tok)
{
	if (cp->tok == tok)
	{
		cp_next(cp);
		return 1;
	}
	return 0;
}

/* Check and consume token. */
static void cp_check(CPState *cp, CPToken tok)
{
	if (cp->tok != tok)
		cp_err_token(cp, tok);
	cp_next(cp);
}

/* Check if the next token may start a type declaration. */
static int cp_istypedecl(CPState *cp)
{
	if (cp->tok >= CTOK_FIRSTDECL && cp->tok <= CTOK_LASTDECL)
		return 1;
	if (cp->tok == CTOK_IDENT && ctype_istypedef(cp->ct->info))
		return 1;
	if (cp->tok == '$')
		return 1;
	return 0;
}

/* -- Constant expression evaluator --------------------------------------- */

/* Forward declarations. */
static void cp_expr_unary(CPState *cp, CPValue *k);
static void cp_expr_sub(CPState *cp, CPValue *k, int pri);

/* Please note that type handling is very weak here. Most ops simply
** assume integer operands. Accessors are only needed to compute types and
** return synthetic values. The only purpose of the expression evaluator
** is to compute the values of constant expressions one would typically
** find in C header files. And again: this is NOT a validating C parser!
*/

/* Parse comma separated expression and return last result. */
static void cp_expr_comma(CPState *cp, CPValue *k)
{
	do
	{
		cp_expr_sub(cp, k, 0);
		if (cp->error)
			return;
	} while (cp_opt(cp, ','));
}

/* Parse sizeof/alignof operator. */
static void cp_expr_sizeof(CPState *cp, CPValue *k, int wantsz)
{
	CTSize sz;
	CTInfo info;
	if (cp_opt(cp, '('))
	{
		if (cp_istypedecl(cp))
			k->id = cp_decl_abstract(cp);
		else
			cp_expr_comma(cp, k);
		cp_check(cp, ')');
	}
	else
	{
		cp_expr_unary(cp, k);
	}
	info = uc_ctype_info_raw(cp->cts, k->id, &sz);
	if (wantsz)
	{
		if (sz != CTSIZE_INVALID)
			k->u32 = sz;
		else if (k->id != CTID_A_CCHAR) /* Special case for sizeof("string"). */
		{
			cp_err(cp, "size of C type is unknown or too large");
			return;
		}
	}
	else
	{
		k->u32 = 1u << ctype_align(info);
	}
	k->id = CTID_UINT32; /* Really size_t. */
}

/* Parse prefix operators. */
static void cp_expr_prefix(CPState *cp, CPValue *k)
{
	if (cp->tok == CTOK_INTEGER)
	{
		*k = cp->val;
		cp_next(cp);
	}
	else if (cp_opt(cp, '+'))
	{
		cp_expr_unary(cp, k); /* Nothing to do (well, integer promotion). */
	}
	else if (cp_opt(cp, '-'))
	{
		cp_expr_unary(cp, k);
		k->i32 = (int32_t)(~(uint32_t)k->i32 + 1);
	}
	else if (cp_opt(cp, '~'))
	{
		cp_expr_unary(cp, k);
		k->i32 = ~k->i32;
	}
	else if (cp_opt(cp, '!'))
	{
		cp_expr_unary(cp, k);
		k->i32 = !k->i32;
		k->id = CTID_INT32;
	}
	else if (cp_opt(cp, '('))
	{
		if (cp_istypedecl(cp))
		{ /* Cast operator. */
			CTypeID id = cp_decl_abstract(cp);
			cp_check(cp, ')');
			cp_expr_unary(cp, k);
			k->id = id; /* No conversion performed. */
		}
		else
		{ /* Sub-expression. */
			cp_expr_comma(cp, k);
			cp_check(cp, ')');
		}
	}
	else if (cp_opt(cp, '*'))
	{ /* Indirection. */
		CType *ct;
		cp_expr_unary(cp, k);
		if (cp->error)
			return;
		ct = uc_ctype_rawref(cp->cts, k->id);
		if (!ctype_ispointer(ct->info))
		{
			cp_err_badidx(cp, ct);
			return;
		}
		k->u32 = 0;
		k->id = ctype_cid(ct->info);
	}
	else if (cp_opt(cp, '&'))
	{ /* Address operator. */
		cp_expr_unary(cp, k);
		k->id = uc_ctype_intern(cp->cts, CTINFO(CT_PTR, CTALIGN_PTR + k->id),
								CTSIZE_PTR);
	}
	else if (cp_opt(cp, CTOK_SIZEOF))
	{
		cp_expr_sizeof(cp, k, 1);
	}
	else if (cp_opt(cp, CTOK_ALIGNOF))
	{
		cp_expr_sizeof(cp, k, 0);
	}
	else if (cp->tok == CTOK_IDENT)
	{
		if (ctype_type(cp->ct->info) == CT_CONSTVAL)
		{
			k->u32 = cp->ct->size;
			k->id = ctype_cid(cp->ct->info);
		}
		else if (ctype_type(cp->ct->info) == CT_EXTERN)
		{
			k->u32 = cp->val.id;
			k->id = ctype_cid(cp->ct->info);
		}
		else if (ctype_type(cp->ct->info) == CT_FUNC)
		{
			k->u32 = cp->val.id;
			k->id = cp->val.id;
		}
		else
		{
			goto err_expr;
		}
		cp_next(cp);
	}
	else if (cp->tok == CTOK_STRING)
	{
		CTSize sz = ucv_string_length(cp->uv_str);
		while (cp_next(cp) == CTOK_STRING)
			sz += ucv_string_length(cp->uv_str);
		k->u32 = sz + 1;
		k->id = CTID_A_CCHAR;
	}
	else
	{
	err_expr:
		cp_errmsg(cp, cp->tok, "unexpected symbol");
	}
}

/* Parse postfix operators. */
static void cp_expr_postfix(CPState *cp, CPValue *k)
{
	for (;;)
	{
		CType *ct;
		if (cp_opt(cp, '['))
		{ /* Array/pointer index. */
			CPValue k2;
			cp_expr_comma(cp, &k2);
			ct = uc_ctype_rawref(cp->cts, k->id);
			if (!ctype_ispointer(ct->info))
			{
				ct = uc_ctype_rawref(cp->cts, k2.id);
				if (!ctype_ispointer(ct->info)) {
					cp_err_badidx(cp, ct);
					return;
				}
			}
			cp_check(cp, ']');
			k->u32 = 0;
		}
		else if (cp->tok == '.' || cp->tok == CTOK_DEREF)
		{ /* Struct deref. */
			CTSize ofs;
			CType *fct;
			ct = uc_ctype_rawref(cp->cts, k->id);
			if (cp->tok == CTOK_DEREF)
			{
				if (!ctype_ispointer(ct->info)) {
					cp_err_badidx(cp, ct);
					return;
				}
				ct = uc_ctype_rawref(cp->cts, ctype_cid(ct->info));
			}
			cp_next(cp);
			if (cp->tok != CTOK_IDENT) {
				cp_err_token(cp, CTOK_IDENT);
				return;
			}
			if (!ctype_isstruct(ct->info) || ct->size == CTSIZE_INVALID ||
				!(fct = uc_ctype_getfield(cp->cts, ct, cp->uv_str, &ofs)) ||
				ctype_isbitfield(fct->info))
			{
				uc_value_t *s = uc_ctype_repr(cp->uv_vm, ctype_typeid(cp->cts, ct), NULL);
				cp_errmsg(cp, 0, "'%s' has no member named '%s'", ucv_string_get(s), ucv_string_get(cp->uv_str));
				ucv_put(s);

				return;
			}
			ct = fct;
			k->u32 = ctype_isconstval(ct->info) ? ct->size : 0;
			cp_next(cp);
		}
		else
		{
			return;
		}
		k->id = ctype_cid(ct->info);
	}
}

/* Parse infix operators. */
static void cp_expr_infix(CPState *cp, CPValue *k, int pri)
{
	CPValue k2;
	k2.u32 = 0;
	k2.id = 0; /* Silence the compiler. */
	for (;;)
	{
		switch (pri)
		{
		case 0:
			if (cp_opt(cp, '?'))
			{
				CPValue k3;
				cp_expr_comma(cp, &k2); /* Right-associative. */
				if (cp->error)
					return;
				cp_check(cp, ':');
				cp_expr_sub(cp, &k3, 0);
				if (cp->error)
					return;
				k->u32 = k->u32 ? k2.u32 : k3.u32;
				k->id = k2.id > k3.id ? k2.id : k3.id;
				continue;
			}
			/* fallthrough */
		case 1:
			if (cp_opt(cp, CTOK_OROR))
			{
				cp_expr_sub(cp, &k2, 2);
				if (cp->error)
					return;
				k->i32 = k->u32 || k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			/* fallthrough */
		case 2:
			if (cp_opt(cp, CTOK_ANDAND))
			{
				cp_expr_sub(cp, &k2, 3);
				if (cp->error)
					return;
				k->i32 = k->u32 && k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			/* fallthrough */
		case 3:
			if (cp_opt(cp, '|'))
			{
				cp_expr_sub(cp, &k2, 4);
				if (cp->error)
					return;
				k->u32 = k->u32 | k2.u32;
				goto arith_result;
			}
			/* fallthrough */
		case 4:
			if (cp_opt(cp, '^'))
			{
				cp_expr_sub(cp, &k2, 5);
				if (cp->error)
					return;
				k->u32 = k->u32 ^ k2.u32;
				goto arith_result;
			}
			/* fallthrough */
		case 5:
			if (cp_opt(cp, '&'))
			{
				cp_expr_sub(cp, &k2, 6);
				if (cp->error)
					return;
				k->u32 = k->u32 & k2.u32;
				goto arith_result;
			}
			/* fallthrough */
		case 6:
			if (cp_opt(cp, CTOK_EQ))
			{
				cp_expr_sub(cp, &k2, 7);
				if (cp->error)
					return;
				k->i32 = k->u32 == k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			else if (cp_opt(cp, CTOK_NE))
			{
				cp_expr_sub(cp, &k2, 7);
				if (cp->error)
					return;
				k->i32 = k->u32 != k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			/* fallthrough */
		case 7:
			if (cp_opt(cp, '<'))
			{
				cp_expr_sub(cp, &k2, 8);
				if (cp->error)
					return;
				if (k->id == CTID_INT32 && k2.id == CTID_INT32)
					k->i32 = k->i32 < k2.i32;
				else
					k->i32 = k->u32 < k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			else if (cp_opt(cp, '>'))
			{
				cp_expr_sub(cp, &k2, 8);
				if (cp->error)
					return;
				if (k->id == CTID_INT32 && k2.id == CTID_INT32)
					k->i32 = k->i32 > k2.i32;
				else
					k->i32 = k->u32 > k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			else if (cp_opt(cp, CTOK_LE))
			{
				cp_expr_sub(cp, &k2, 8);
				if (cp->error)
					return;
				if (k->id == CTID_INT32 && k2.id == CTID_INT32)
					k->i32 = k->i32 <= k2.i32;
				else
					k->i32 = k->u32 <= k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			else if (cp_opt(cp, CTOK_GE))
			{
				cp_expr_sub(cp, &k2, 8);
				if (cp->error)
					return;
				if (k->id == CTID_INT32 && k2.id == CTID_INT32)
					k->i32 = k->i32 >= k2.i32;
				else
					k->i32 = k->u32 >= k2.u32;
				k->id = CTID_INT32;
				continue;
			}
			/* fallthrough */
		case 8:
			if (cp_opt(cp, CTOK_SHL))
			{
				cp_expr_sub(cp, &k2, 9);
				if (cp->error)
					return;
				k->u32 = k->u32 << k2.u32;
				continue;
			}
			else if (cp_opt(cp, CTOK_SHR))
			{
				cp_expr_sub(cp, &k2, 9);
				if (cp->error)
					return;
				if (k->id == CTID_INT32)
					k->i32 = k->i32 >> k2.i32;
				else
					k->u32 = k->u32 >> k2.u32;
				continue;
			}
			/* fallthrough */
		case 9:
			if (cp_opt(cp, '+'))
			{
				cp_expr_sub(cp, &k2, 10);
				if (cp->error)
					return;
				k->u32 = k->u32 + k2.u32;
			arith_result:
				if (k2.id > k->id)
					k->id = k2.id; /* Trivial promotion to unsigned. */
				continue;
			}
			else if (cp_opt(cp, '-'))
			{
				cp_expr_sub(cp, &k2, 10);
				if (cp->error)
					return;
				k->u32 = k->u32 - k2.u32;
				goto arith_result;
			}
			/* fallthrough */
		case 10:
			if (cp_opt(cp, '*'))
			{
				cp_expr_unary(cp, &k2);
				if (cp->error)
					return;
				k->u32 = k->u32 * k2.u32;
				goto arith_result;
			}
			else if (cp_opt(cp, '/'))
			{
				cp_expr_unary(cp, &k2);
				if (cp->error)
					return;
				if (k2.id > k->id)
					k->id = k2.id; /* Trivial promotion to unsigned. */
				if (k2.u32 == 0 ||
					(k->id == CTID_INT32 && k->u32 == 0x80000000u && k2.i32 == -1))
				{
					cp_err(cp, "invalid value");
					return;
				}
				if (k->id == CTID_INT32)
					k->i32 = k->i32 / k2.i32;
				else
					k->u32 = k->u32 / k2.u32;
				continue;
			}
			else if (cp_opt(cp, '%'))
			{
				cp_expr_unary(cp, &k2);
				if (cp->error)
					return;
				if (k2.id > k->id)
					k->id = k2.id; /* Trivial promotion to unsigned. */
				if (k2.u32 == 0 ||
					(k->id == CTID_INT32 && k->u32 == 0x80000000u && k2.i32 == -1))
				{
					cp_err(cp, "invalid value");
					return;
				}
				if (k->id == CTID_INT32)
					k->i32 = k->i32 % k2.i32;
				else
					k->u32 = k->u32 % k2.u32;
				continue;
			}
		default:
			return;
		}
	}
}

/* Parse and evaluate unary expression. */
static void cp_expr_unary(CPState *cp, CPValue *k)
{
	if (++cp->depth > CPARSE_MAX_DECLDEPTH)
	{
		cp_err(cp, "chunk has too many syntax levels");
		return;
	}
	cp_expr_prefix(cp, k);
	if (cp->error)
		return;
	cp_expr_postfix(cp, k);
	cp->depth--;
}

/* Parse and evaluate sub-expression. */
static void cp_expr_sub(CPState *cp, CPValue *k, int pri)
{
	cp_expr_unary(cp, k);
	if (cp->error)
		return;
	cp_expr_infix(cp, k, pri);
}

/* Parse constant integer expression. */
static void cp_expr_kint(CPState *cp, CPValue *k)
{
	CType *ct;
	cp_expr_sub(cp, k, 0);
	if (cp->error)
		return;
	ct = ctype_raw(cp->cts, k->id);
	if (!ctype_isinteger(ct->info))
	{
		cp_err(cp, "invalid value");
		return;
	}
}

/* Parse (non-negative) size expression. */
static CTSize cp_expr_ksize(CPState *cp)
{
	CPValue k;
	cp_expr_kint(cp, &k);
	if (cp->error)
		return CTSIZE_INVALID;
	if (k.u32 >= 0x80000000u)
	{
		cp_err(cp, "size of C type is unknown or too large");
		return CTSIZE_INVALID;
	}
	return k.u32;
}

/* -- Type declaration stack management ----------------------------------- */

/* Add declaration element behind the insertion position. */
static CPDeclIdx cp_add(CPDecl *decl, CTInfo info, CTSize size)
{
	CPDeclIdx top = decl->top;
	if (top >= CPARSE_MAX_DECLSTACK)
	{
		cp_err(decl->cp, "chunk has too many syntax levels");
		return 0;
	}
	decl->stack[top].info = info;
	decl->stack[top].size = size;
	decl->stack[top].sib = 0;
	decl->stack[top].uv_name = NULL;
	// setgcrefnull(decl->stack[top].name);
	decl->stack[top].next = decl->stack[decl->pos].next;
	decl->stack[decl->pos].next = (CTypeID1)top;
	decl->top = top + 1;
	return top;
}

/* Push declaration element before the insertion position. */
static CPDeclIdx cp_push(CPDecl *decl, CTInfo info, CTSize size)
{
	return (decl->pos = cp_add(decl, info, size));
}

/* Push or merge attributes. */
static void cp_push_attributes(CPDecl *decl)
{
	CType *ct = &decl->stack[decl->pos];
	if (ctype_isfunc(ct->info))
	{ /* Ok to modify in-place. */
#if UC_TARGET_X86
		if ((decl->fattr & CTFP_CCONV))
			ct->info = (ct->info & (CTMASK_NUM | CTF_VARARG | CTMASK_CID)) +
					   (decl->fattr & ~CTMASK_CID);
#endif
	}
	else
	{
		if ((decl->attr & CTFP_ALIGNED) && !(decl->mode & CPARSE_MODE_FIELD))
			cp_push(decl, CTINFO(CT_ATTRIB, CTATTRIB(CTA_ALIGN)),
					ctype_align(decl->attr));
	}
}

/* Push unrolled type to declaration stack and merge qualifiers. */
static void cp_push_type(CPDecl *decl, CTypeID id)
{
	CType *ct = ctype_get(decl->cp->cts, id);
	CTInfo info = ct->info;
	CTSize size = ct->size;
	switch (ctype_type(info))
	{
	case CT_STRUCT:
	case CT_ENUM:
		cp_push(decl, CTINFO(CT_TYPEDEF, id), 0); /* Don't copy unique types. */
		if ((decl->attr & CTF_QUAL))
		{ /* Push unmerged qualifiers. */
			cp_push(decl, CTINFO(CT_ATTRIB, CTATTRIB(CTA_QUAL)),
					(decl->attr & CTF_QUAL));
			decl->attr &= ~CTF_QUAL;
		}
		break;
	case CT_ATTRIB:
		if (ctype_isxattrib(info, CTA_QUAL))
			decl->attr &= ~size;				 /* Remove redundant qualifiers. */
		cp_push_type(decl, ctype_cid(info));	 /* Unroll. */
		cp_push(decl, info & ~CTMASK_CID, size); /* Copy type. */
		break;
	case CT_ARRAY:
		if ((ct->info & (CTF_VECTOR | CTF_COMPLEX)))
		{
			info |= (decl->attr & CTF_QUAL);
			decl->attr &= ~CTF_QUAL;
		}
		cp_push_type(decl, ctype_cid(info));	 /* Unroll. */
		cp_push(decl, info & ~CTMASK_CID, size); /* Copy type. */
		decl->stack[decl->pos].sib = 1;			 /* Mark as already checked and sized. */
		/* Note: this is not copied to the ct->sib in the C type table. */
		break;
	case CT_FUNC:
		/* Copy type, link parameters (shared). */
		decl->stack[cp_push(decl, info, size)].sib = ct->sib;
		break;
	default:
		/* Copy type, merge common qualifiers. */
		cp_push(decl, info | (decl->attr & CTF_QUAL), size);
		decl->attr &= ~CTF_QUAL;
		break;
	}
}

/* Consume the declaration element chain and intern the C type. */
static CTypeID cp_decl_intern(CPState *cp, CPDecl *decl)
{
	CTypeID id = 0;
	CPDeclIdx idx = 0;
	CTSize csize = CTSIZE_INVALID;
	CTSize cinfo = 0;
	do
	{
		CType *ct = &decl->stack[idx];
		CTInfo info = ct->info;
		CTInfo size = ct->size;
		/* The cid is already part of info for copies of pointers/functions. */
		idx = ct->next;
		if (ctype_istypedef(info))
		{
			uc_assertCP(id == 0, "typedef not at toplevel");
			id = ctype_cid(info);
			/* Always refetch info/size, since struct/enum may have been completed. */
			cinfo = ctype_get(cp->cts, id)->info;
			csize = ctype_get(cp->cts, id)->size;
			uc_assertCP(ctype_isstruct(cinfo) || ctype_isenum(cinfo),
						"typedef of bad type");
		}
		else if (ctype_isfunc(info))
		{ /* Intern function. */
			CType *fct;
			CTypeID fid;
			CTypeID sib;
			if (id)
			{
				CType *refct = ctype_raw(cp->cts, id);
				/* Reject function or refarray return types. */
				if (ctype_isfunc(refct->info) || ctype_isrefarray(refct->info)) {
					cp_err(cp, "invalid C type");
					return 0;
				}
			}
			/* No intervening attributes allowed, skip forward. */
			while (idx)
			{
				CType *ctn = &decl->stack[idx];
				if (!ctype_isattrib(ctn->info))
					break;
				idx = ctn->next; /* Skip attribute. */
			}
			sib = ct->sib; /* Next line may reallocate the C type table. */
			fid = uc_ctype_new(cp->cts, &fct);
			csize = CTSIZE_INVALID;
			fct->info = cinfo = info + id;
			fct->size = size;
			fct->sib = sib;
			id = fid;
		}
		else if (ctype_isattrib(info))
		{
			if (ctype_isxattrib(info, CTA_QUAL))
				cinfo |= size;
			else if (ctype_isxattrib(info, CTA_ALIGN))
				CTF_INSERT(cinfo, ALIGN, size);
			id = uc_ctype_intern(cp->cts, info + id, size);
			/* Inherit csize/cinfo from original type. */
		}
		else
		{
			if (ctype_isnum(info))
			{ /* Handle mode/vector-size attributes. */
				uc_assertCP(id == 0, "number not at toplevel");
				if (!(info & CTF_BOOL))
				{
					CTSize msize = ctype_msizeP(decl->attr);
					CTSize vsize = ctype_vsizeP(decl->attr);
					if (msize && (!(info & CTF_FP) || (msize == 4 || msize == 8)))
					{
						CTSize malign = uc_fls(msize);
						if (malign > 4)
							malign = 4; /* Limit alignment. */
						CTF_INSERT(info, ALIGN, malign);
						size = msize; /* Override size via mode. */
					}
					if (vsize)
					{ /* Vector size set? */
						CTSize esize = uc_fls(size);
						if (vsize >= esize)
						{
							/* Intern the element type first. */
							id = uc_ctype_intern(cp->cts, info, size);
							/* Then create a vector (array) with vsize alignment. */
							size = (1u << vsize);
							if (vsize > 4)
								vsize = 4; /* Limit alignment. */
							if (ctype_align(info) > vsize)
								vsize = ctype_align(info);
							info = CTINFO(CT_ARRAY, (info & CTF_QUAL) + CTF_VECTOR +
														CTALIGN(vsize));
						}
					}
				}
			}
			else if (ctype_isptr(info))
			{
				/* Reject pointer/ref to ref. */
				if (id && ctype_isref(ctype_raw(cp->cts, id)->info)) {
					cp_err(cp, "invalid C type");
					return 0;
				}
				if (ctype_isref(info))
				{
					info &= ~CTF_VOLATILE; /* Refs are always const, never volatile. */
					/* No intervening attributes allowed, skip forward. */
					while (idx)
					{
						CType *ctn = &decl->stack[idx];
						if (!ctype_isattrib(ctn->info))
							break;
						idx = ctn->next; /* Skip attribute. */
					}
				}
			}
			else if (ctype_isarray(info))
			{ /* Check for valid array size etc. */
				if (ct->sib == 0)
				{							/* Only check/size arrays not copied by unroll. */
					if (ctype_isref(cinfo)) /* Reject arrays of refs. */ {
						cp_err(cp, "invalid C type");
						return 0;
					}
					/* Reject VLS or unknown-sized types. */
					if (ctype_isvltype(cinfo) || csize == CTSIZE_INVALID) {
						cp_err(cp, "size of C type is unknown or too large");
						return 0;
					}
					/* a[] and a[?] keep their invalid size. */
					if (size != CTSIZE_INVALID)
					{
						uint64_t xsz = (uint64_t)size * csize;
						if (xsz >= 0x80000000u) {
							cp_err(cp, "size of C type is unknown or too large");
							return 0;
						}
						size = (CTSize)xsz;
					}
				}
				if ((cinfo & CTF_ALIGN) > (info & CTF_ALIGN)) /* Find max. align. */
					info = (info & ~CTF_ALIGN) | (cinfo & CTF_ALIGN);
				info |= (cinfo & CTF_QUAL); /* Inherit qual. */
			}
			else
			{
				uc_assertCP(ctype_isvoid(info), "bad ctype %08x", info);
			}
			csize = size;
			cinfo = info + id;
			id = uc_ctype_intern(cp->cts, info + id, size);
		}
	} while (idx);
	return id;
}

/* -- C declaration parser ------------------------------------------------ */

/* Reset declaration state to declaration specifier. */
static void cp_decl_reset(CPDecl *decl)
{
	ucv_put(decl->uv_name);
	ucv_put(decl->uv_redir);

	decl->pos = decl->specpos;
	decl->top = decl->specpos + 1;
	decl->stack[decl->specpos].next = 0;
	decl->attr = decl->specattr;
	decl->fattr = decl->specfattr;
	decl->uv_name = NULL;
	decl->uv_redir = NULL;
}

/* Parse constant initializer. */
/* NYI: FP constants and strings as initializers. */
static CTypeID cp_decl_constinit(CPState *cp, CType **ctp, CTypeID ctypeid)
{
	CType *ctt = ctype_get(cp->cts, ctypeid);
	CTInfo info;
	CTSize size;
	CPValue k;
	CTypeID constid;
	while (ctype_isattrib(ctt->info))
	{									/* Skip attributes. */
		ctypeid = ctype_cid(ctt->info); /* Update ID, too. */
		ctt = ctype_get(cp->cts, ctypeid);
	}
	info = ctt->info;
	size = ctt->size;
	if (!ctype_isinteger(info) || !(info & CTF_CONST) || size > 4)
	{
		cp_err(cp, "invalid C type");
		return 0;
	}
	cp_check(cp, '=');
	if (cp->error)
		return 0;
	cp_expr_sub(cp, &k, 0);
	if (cp->error)
		return 0;
	constid = uc_ctype_new(cp->cts, ctp);
	(*ctp)->info = CTINFO(CT_CONSTVAL, CTF_CONST | ctypeid);
	k.u32 <<= 8 * (4 - size);
	if ((info & CTF_UNSIGNED))
		k.u32 >>= 8 * (4 - size);
	else
		k.u32 = (uint32_t)((int32_t)k.u32 >> 8 * (4 - size));
	(*ctp)->size = k.u32;
	return constid;
}

/* Parse size in parentheses as part of attribute. */
static CTSize cp_decl_sizeattr(CPState *cp)
{
	CTSize sz;
	uint32_t oldtmask = cp->tmask;
	cp->tmask = CPNS_DEFAULT; /* Required for expression evaluator. */
	cp_check(cp, '(');
	if (cp->error)
		return 0;
	sz = cp_expr_ksize(cp);
	if (cp->error)
		return 0;
	cp->tmask = oldtmask;
	cp_check(cp, ')');
	if (cp->error)
		return 0;
	return sz;
}

/* Parse alignment attribute. */
static void cp_decl_align(CPState *cp, CPDecl *decl)
{
	CTSize al = 4; /* Unspecified alignment is 16 bytes. */
	if (cp->tok == '(')
	{
		al = cp_decl_sizeattr(cp);
		if (cp->error)
			return;
		al = al ? uc_fls(al) : 0;
	}
	CTF_INSERT(decl->attr, ALIGN, al);
	decl->attr |= CTFP_ALIGNED;
}

/* Parse GCC asm("name") redirect. */
static void cp_decl_asm(CPState *cp, unused CPDecl *decl)
{
	cp_next(cp);
	if (cp->error)
		return;
	cp_check(cp, '(');
	if (cp->error)
		return;
	if (cp->tok == CTOK_STRING)
	{
		uc_stringbuf_t *buf = ucv_stringbuf_new();
		// GCstr *str = cp->str;
		ucv_stringbuf_addstr(buf, ucv_string_get(cp->uv_str), ucv_string_length(cp->uv_str));
		while (cp_next(cp) == CTOK_STRING)
		{
			if (cp->error)
				return;
			ucv_stringbuf_addstr(buf, ucv_string_get(cp->uv_str), ucv_string_length(cp->uv_str));
		}
		decl->uv_redir = ucv_stringbuf_finish(buf);
	}
	cp_check(cp, ')');
	if (cp->error)
		return;
}

/* Parse GCC __attribute__((mode(...))). */
static void cp_decl_mode(CPState *cp, CPDecl *decl)
{
	cp_check(cp, '(');
	if (cp->tok == CTOK_IDENT)
	{
		const char *s = ucv_string_get(cp->uv_str);
		CTSize sz = 0, vlen = 0;
		if (s[0] == '_' && s[1] == '_')
			s += 2;
		if (*s == 'V')
		{
			s++;
			vlen = *s++ - '0';
			if (*s >= '0' && *s <= '9')
				vlen = vlen * 10 + (*s++ - '0');
		}
		switch (*s++)
		{
		case 'Q':
			sz = 1;
			break;
		case 'H':
			sz = 2;
			break;
		case 'S':
			sz = 4;
			break;
		case 'D':
			sz = 8;
			break;
		case 'T':
			sz = 16;
			break;
		case 'O':
			sz = 32;
			break;
		default:
			goto bad_size;
		}
		if (*s == 'I' || *s == 'F')
		{
			CTF_INSERT(decl->attr, MSIZEP, sz);
			if (vlen)
				CTF_INSERT(decl->attr, VSIZEP, uc_fls(vlen * sz));
		}
	bad_size:
		cp_next(cp);
	}
	cp_check(cp, ')');
}

/* Parse GCC __attribute__((...)). */
static void cp_decl_gccattribute(CPState *cp, CPDecl *decl)
{
	cp_next(cp);
	if (cp->error)
		return;
	cp_check(cp, '(');
	if (cp->error)
		return;
	cp_check(cp, '(');
	if (cp->error)
		return;
	while (cp->tok != ')')
	{
		if (cp->tok == CTOK_IDENT)
		{
			uc_value_t *attrstr = ucv_get(cp->uv_str);
			cp_next(cp);
			if (cp->error)
				return;
			switch (uc_cparse_case(attrstr,
								   "\007aligned"
								   "\013__aligned__"
								   "\006packed"
								   "\012__packed__"
								   "\004mode"
								   "\010__mode__"
								   "\013vector_size"
								   "\017__vector_size__"
#if UC_TARGET_X86
								   "\007regparm"
								   "\013__regparm__"
								   "\005cdecl"
								   "\011__cdecl__"
								   "\010thiscall"
								   "\014__thiscall__"
								   "\010fastcall"
								   "\014__fastcall__"
								   "\007stdcall"
								   "\013__stdcall__"
								   "\012sseregparm"
								   "\016__sseregparm__"
#endif
								   ))
			{
			case 0:
			case 1: /* aligned */
				cp_decl_align(cp, decl);
				break;
			case 2:
			case 3: /* packed */
				decl->attr |= CTFP_PACKED;
				break;
			case 4:
			case 5: /* mode */
				cp_decl_mode(cp, decl);
				break;
			case 6:
			case 7: /* vector_size */
			{
				CTSize vsize = cp_decl_sizeattr(cp);
				if (vsize)
					CTF_INSERT(decl->attr, VSIZEP, uc_fls(vsize));
			}
			break;
#if UC_TARGET_X86
			case 8:
			case 9: /* regparm */
				CTF_INSERT(decl->fattr, REGPARM, cp_decl_sizeattr(cp));
				decl->fattr |= CTFP_CCONV;
				break;
			case 10:
			case 11: /* cdecl */
				CTF_INSERT(decl->fattr, CCONV, CTCC_CDECL);
				decl->fattr |= CTFP_CCONV;
				break;
			case 12:
			case 13: /* thiscall */
				CTF_INSERT(decl->fattr, CCONV, CTCC_THISCALL);
				decl->fattr |= CTFP_CCONV;
				break;
			case 14:
			case 15: /* fastcall */
				CTF_INSERT(decl->fattr, CCONV, CTCC_FASTCALL);
				decl->fattr |= CTFP_CCONV;
				break;
			case 16:
			case 17: /* stdcall */
				CTF_INSERT(decl->fattr, CCONV, CTCC_STDCALL);
				decl->fattr |= CTFP_CCONV;
				break;
			case 18:
			case 19: /* sseregparm */
				decl->fattr |= CTF_SSEREGPARM;
				decl->fattr |= CTFP_CCONV;
				break;
#endif
			default: /* Skip all other attributes. */
				ucv_put(attrstr);
				goto skip_attr;
			}
			ucv_put(attrstr);
		}
		else if (cp->tok >= CTOK_FIRSTDECL)
		{ /* For __attribute((const)) etc. */
			cp_next(cp);
		skip_attr:
			if (cp_opt(cp, '('))
			{
				while (cp->tok != ')' && cp->tok != CTOK_EOF)
				{
					cp_next(cp);
					if (cp->error)
						return;
				}
				cp_check(cp, ')');
				if (cp->error)
					return;
			}
		}
		else
		{
			break;
		}
		if (!cp_opt(cp, ','))
			break;
	}
	cp_check(cp, ')');
	if (cp->error)
		return;
	cp_check(cp, ')');
	if (cp->error)
		return;
}

/* Parse MSVC __declspec(...). */
static void cp_decl_msvcattribute(CPState *cp, CPDecl *decl)
{
	cp_next(cp);
	if (cp->error)
		return;
	cp_check(cp, '(');
	if (cp->error)
		return;
	while (cp->tok == CTOK_IDENT)
	{
		uc_value_t *attrstr = ucv_get(cp->uv_str);
		cp_next(cp);
		if (cp->error)
			return;
		if (cp_str_is(attrstr, "align"))
		{
			cp_decl_align(cp, decl);
			if (cp->error)
				return;
		}
		else
		{ /* Ignore all other attributes. */
			if (cp_opt(cp, '('))
			{
				while (cp->tok != ')' && cp->tok != CTOK_EOF)
				{
					cp_next(cp);
					if (cp->error)
						return;
				}
				cp_check(cp, ')');
				if (cp->error)
					return;
			}
		}
		ucv_put(attrstr);
	}
	cp_check(cp, ')');
	if (cp->error)
		return;
}

/* Parse declaration attributes (and common qualifiers). */
static void cp_decl_attributes(CPState *cp, CPDecl *decl)
{
	for (;;)
	{
		switch (cp->tok)
		{
		case CTOK_CONST:
			decl->attr |= CTF_CONST;
			break;
		case CTOK_VOLATILE:
			decl->attr |= CTF_VOLATILE;
			break;
		case CTOK_RESTRICT:
			break; /* Ignore. */
		case CTOK_EXTENSION:
			break; /* Ignore. */
		case CTOK_ATTRIBUTE:
			cp_decl_gccattribute(cp, decl);
			if (cp->error)
				return;
			continue;
		case CTOK_ASM:
			cp_decl_asm(cp, decl);
			if (cp->error)
				return;
			continue;
		case CTOK_DECLSPEC:
			cp_decl_msvcattribute(cp, decl);
			if (cp->error)
				return;
			continue;
		case CTOK_CCDECL:
#if UC_TARGET_X86
			CTF_INSERT(decl->fattr, CCONV, cp->ct->size);
			decl->fattr |= CTFP_CCONV;
#endif
			break;
		case CTOK_PTRSZ:
#if UC_64
			CTF_INSERT(decl->attr, MSIZEP, cp->ct->size);
#endif
			break;
		default:
			return;
		}
		cp_next(cp);
	}
}

/* Parse struct/union/enum name. */
static CTypeID cp_struct_name(CPState *cp, CPDecl *sdecl, CTInfo info)
{
	CTypeID sid;
	CType *ct;
	cp->tmask = CPNS_STRUCT;
	cp_next(cp);
	cp_decl_attributes(cp, sdecl);
	cp->tmask = CPNS_DEFAULT;
	if (cp->tok != '{')
	{
		if (cp->tok != CTOK_IDENT)
			cp_err_token(cp, CTOK_IDENT);
		if (cp->val.id)
		{ /* Name of existing struct/union/enum. */
			sid = cp->val.id;
			ct = cp->ct;
			if ((ct->info ^ info) & (CTMASK_NUM | CTF_UNION)) /* Wrong type. */
				cp_errmsg(cp, 0, "attempt to redefine '%s'", ucv_string_get(ct->uv_name));
		}
		else
		{ /* Create named, incomplete struct/union/enum. */
			if ((cp->mode & CPARSE_MODE_NOIMPLICIT))
				cp_errmsg(cp, 0, "undeclared or implicit tag '%s'", ucv_string_get(cp->uv_str));
			sid = uc_ctype_new(cp->cts, &ct);
			ct->info = info;
			ct->size = CTSIZE_INVALID;
			ctype_setname(ct, cp->uv_str);
			uc_ctype_addname(cp->cts, ct, sid);
		}
		cp_next(cp);
	}
	else
	{ /* Create anonymous, incomplete struct/union/enum. */
		sid = uc_ctype_new(cp->cts, &ct);
		ct->info = info;
		ct->size = CTSIZE_INVALID;
	}
	if (cp->tok == '{')
	{
		if (ct->size != CTSIZE_INVALID || ct->sib)
			cp_errmsg(cp, 0, "attempt to redefine '%s'", ucv_string_get(ct->uv_name));
		ct->sib = 1; /* Indicate the type is currently being defined. */
	}
	return sid;
}

/* Determine field alignment. */
static CTSize cp_field_align(unused CPState *cp, unused CType *ct, CTInfo info)
{
	CTSize align = ctype_align(info);
#if (UC_TARGET_X86 && !UC_ABI_WIN) || (UC_TARGET_ARM && __APPLE__)
	/* The SYSV i386 and iOS ABIs limit alignment of non-vector fields to 2^2. */
	if (align > 2 && !(info & CTFP_ALIGNED))
	{
		if (ctype_isarray(info) && !(info & CTF_VECTOR))
		{
			do
			{
				ct = ctype_rawchild(cp->cts, ct);
				info = ct->info;
			} while (ctype_isarray(info) && !(info & CTF_VECTOR));
		}
		if (ctype_isnum(info) || ctype_isenum(info))
			align = 2;
	}
#endif
	return align;
}

/* Layout struct/union fields. */
static void cp_struct_layout(CPState *cp, CTypeID sid, CTInfo sattr)
{
	CTSize bofs = 0, bmaxofs = 0; /* Bit offset and max. bit offset. */
	CTSize maxalign = ctype_align(sattr);
	CType *sct = ctype_get(cp->cts, sid);
	CTInfo sinfo = sct->info;
	CTypeID fieldid = sct->sib;
	while (fieldid)
	{
		CType *ct = ctype_get(cp->cts, fieldid);
		CTInfo attr = ct->size; /* Field declaration attributes (temp.). */

		if (ctype_isfield(ct->info) ||
			(ctype_isxattrib(ct->info, CTA_SUBTYPE) && attr))
		{
			CTSize align, amask; /* Alignment (pow2) and alignment mask (bits). */
			CTSize sz;
			CTInfo info = uc_ctype_info(cp->cts, ctype_cid(ct->info), &sz);
			CTSize bsz, csz = 8 * sz;				/* Field size and container size (in bits). */
			sinfo |= (info & (CTF_QUAL | CTF_VLA)); /* Merge pseudo-qualifiers. */

			/* Check for size overflow and determine alignment. */
			if (sz >= 0x20000000u || bofs + csz < bofs || (info & CTF_VLA))
			{
				if (!(sz == CTSIZE_INVALID && ctype_isarray(info) &&
					  !(sinfo & CTF_UNION))) {
					cp_err(cp, "size of C type is unknown or too large");
					return;
				}
				csz = sz = 0; /* Treat a[] and a[?] as zero-sized. */
			}
			align = cp_field_align(cp, ct, info);
			if (((attr | sattr) & CTFP_PACKED) ||
				((attr & CTFP_ALIGNED) && ctype_align(attr) > align))
				align = ctype_align(attr);
			if (cp->packstack[cp->curpack] < align)
				align = cp->packstack[cp->curpack];
			if (align > maxalign)
				maxalign = align;
			amask = (8u << align) - 1;

			bsz = ctype_bitcsz(ct->info); /* Bitfield size (temp.). */
			if (bsz == CTBSZ_FIELD || !ctype_isfield(ct->info))
			{
				bsz = csz;						/* Regular fields or subtypes always fill the container. */
				bofs = (bofs + amask) & ~amask; /* Start new aligned field. */
				ct->size = (bofs >> 3);			/* Store field offset. */
			}
			else
			{ /* Bitfield. */
				if (bsz == 0 || (attr & CTFP_ALIGNED) ||
					(!((attr | sattr) & CTFP_PACKED) && (bofs & amask) + bsz > csz))
					bofs = (bofs + amask) & ~amask; /* Start new aligned field. */

				/* Prefer regular field over bitfield. */
				if (bsz == csz && (bofs & amask) == 0)
				{
					ct->info = CTINFO(CT_FIELD, ctype_cid(ct->info));
					ct->size = (bofs >> 3); /* Store field offset. */
				}
				else
				{
					ct->info = CTINFO(CT_BITFIELD,
									  (info & (CTF_QUAL | CTF_UNSIGNED | CTF_BOOL)) +
										  (csz << (CTSHIFT_BITCSZ - 3)) + (bsz << CTSHIFT_BITBSZ));
#if UC_BE
					ct->info += ((csz - (bofs & (csz - 1)) - bsz) << CTSHIFT_BITPOS);
#else
					ct->info += ((bofs & (csz - 1)) << CTSHIFT_BITPOS);
#endif
					ct->size = ((bofs & ~(csz - 1)) >> 3); /* Store container offset. */
				}
			}

			/* Determine next offset or max. offset. */
			if ((sinfo & CTF_UNION))
			{
				if (bsz > bmaxofs)
					bmaxofs = bsz;
			}
			else
			{
				bofs += bsz;
			}
		} /* All other fields in the chain are already set up. */

		fieldid = ct->sib;
	}

	/* Complete struct/union. */
	sct->info = sinfo + CTALIGN(maxalign);
	bofs = (sinfo & CTF_UNION) ? bmaxofs : bofs;
	maxalign = (8u << maxalign) - 1;
	sct->size = (((bofs + maxalign) & ~maxalign) >> 3);
}

/* Parse struct/union declaration. */
static CTypeID cp_decl_struct(CPState *cp, CPDecl *sdecl, CTInfo sinfo)
{
	CTypeID sid = cp_struct_name(cp, sdecl, sinfo);
	if (cp->error)
		return 0;
	if (cp_opt(cp, '{'))
	{ /* Struct/union definition. */
		CTypeID lastid = sid;
		int lastdecl = 0;
		while (cp->tok != '}')
		{
			CPDecl decl = { 0 };
			CPscl scl = cp_decl_spec(cp, &decl, CDF_STATIC);
			if (cp->error)
				return 0;
			decl.mode = scl ? CPARSE_MODE_DIRECT : CPARSE_MODE_DIRECT | CPARSE_MODE_ABSTRACT | CPARSE_MODE_FIELD;

			for (;;)
			{
				CTypeID ctypeid;

				if (lastdecl)
				{
					cp_err_token(cp, '}');
					return 0;
				}

				/* Parse field declarator. */
				decl.bits = CTSIZE_INVALID;
				cp_declarator(cp, &decl);
				if (cp->error)
					return 0;
				ctypeid = cp_decl_intern(cp, &decl);
				if (cp->error)
					return 0;

				if ((scl & CDF_STATIC))
				{ /* Static constant in struct namespace. */
					CType *ct;
					CTypeID fieldid = cp_decl_constinit(cp, &ct, ctypeid);
					if (cp->error)
						return 0;
					ctype_get(cp->cts, lastid)->sib = fieldid;
					lastid = fieldid;
					ctype_setname(ct, decl.uv_name);
				}
				else
				{
					CTSize bsz = CTBSZ_FIELD; /* Temp. for layout phase. */
					CType *ct;
					CTypeID fieldid = uc_ctype_new(cp->cts, &ct); /* Do this first. */
					CType *tct = ctype_raw(cp->cts, ctypeid);

					if (decl.bits == CTSIZE_INVALID)
					{ /* Regular field. */
						if (ctype_isarray(tct->info) && tct->size == CTSIZE_INVALID)
							lastdecl = 1; /* a[] or a[?] must be the last declared field. */

						/* Accept transparent struct/union/enum. */
						if (!decl.uv_name)
						{
							if (!((ctype_isstruct(tct->info) && !(tct->info & CTF_VLA)) ||
								  ctype_isenum(tct->info)))
							{
								cp_err_token(cp, CTOK_IDENT);
								return 0;
							}
							ct->info = CTINFO(CT_ATTRIB, CTATTRIB(CTA_SUBTYPE) + ctypeid);
							ct->size = ctype_isstruct(tct->info) ? (decl.attr | 0x80000000u) : 0; /* For layout phase. */
							goto add_field;
						}
					}
					else
					{ /* Bitfield. */
						bsz = decl.bits;
						if (!ctype_isinteger_or_bool(tct->info) ||
							(bsz == 0 && decl.uv_name) || 8 * tct->size > CTBSZ_MAX ||
							bsz > ((tct->info & CTF_BOOL) ? 1 : 8 * tct->size))
						{
							cp_errmsg(cp, ':', "invalid value");
							return 0;
						}
					}

					/* Create temporary field for layout phase. */
					ct->info = CTINFO(CT_FIELD, ctypeid + (bsz << CTSHIFT_BITCSZ));
					ct->size = decl.attr;
					if (decl.uv_name)
						ctype_setname(ct, decl.uv_name);

				add_field:
					ctype_get(cp->cts, lastid)->sib = fieldid;
					lastid = fieldid;
				}
				cp_decl_reset(&decl);
				if (!cp_opt(cp, ','))
					break;
			}
			cp_check(cp, ';');
			if (cp->error)
				return 0;
		}
		cp_check(cp, '}');
		if (cp->error)
			return 0;
		ctype_get(cp->cts, lastid)->sib = 0; /* Drop sib = 1 for empty structs. */
		cp_decl_attributes(cp, sdecl);		 /* Layout phase needs postfix attributes. */
		if (cp->error)
			return 0;
		cp_struct_layout(cp, sid, sdecl->attr);
	}
	return sid;
}

/* Parse enum declaration. */
static CTypeID cp_decl_enum(CPState *cp, CPDecl *sdecl)
{
	CTypeID eid = cp_struct_name(cp, sdecl, CTINFO(CT_ENUM, CTID_VOID));
	if (cp->error)
		return 0;
	CTInfo einfo = CTINFO(CT_ENUM, CTALIGN(2) + CTID_UINT32);
	CTSize esize = 4; /* Only 32 bit enums are supported. */
	if (cp_opt(cp, '{'))
	{ /* Enum definition. */
		CPValue k;
		CTypeID lastid = eid;
		k.u32 = 0;
		k.id = CTID_INT32;
		do
		{
			uc_value_t *name = ucv_get(cp->uv_str);
			if (cp->tok != CTOK_IDENT)
			{
				cp_err_token(cp, CTOK_IDENT);
				return 0;
			}
			if (cp->val.id)
			{
				cp_errmsg(cp, 0, "attempt to redefine '%s'", ucv_string_get(name));
				return 0;
			}
			cp_next(cp);
			if (cp_opt(cp, '='))
			{
				cp_expr_kint(cp, &k);
				if (cp->error)
					return 0;
				if (k.id == CTID_UINT32)
				{
					/* C99 says that enum constants are always (signed) integers.
					** But since unsigned constants like 0x80000000 are quite common,
					** those are left as uint32_t.
					*/
					if (k.i32 >= 0)
						k.id = CTID_INT32;
				}
				else
				{
					/* OTOH it's common practice and even mandated by some ABIs
					** that the enum type itself is unsigned, unless there are any
					** negative constants.
					*/
					k.id = CTID_INT32;
					if (k.i32 < 0)
						einfo = CTINFO(CT_ENUM, CTALIGN(2) + CTID_INT32);
				}
			}
			/* Add named enum constant. */
			{
				CType *ct;
				CTypeID constid = uc_ctype_new(cp->cts, &ct);
				ctype_get(cp->cts, lastid)->sib = constid;
				lastid = constid;
				ctype_setname(ct, name);
				ct->info = CTINFO(CT_CONSTVAL, CTF_CONST | k.id);
				ct->size = k.u32++;
				if (k.u32 == 0x80000000u)
					k.id = CTID_UINT32;
				uc_ctype_addname(cp->cts, ct, constid);
			}
			ucv_put(name);
			if (!cp_opt(cp, ','))
				break;
		} while (cp->tok != '}'); /* Trailing ',' is ok. */
		cp_check(cp, '}');
		if (cp->error)
			return 0;
		/* Complete enum. */
		ctype_get(cp->cts, eid)->info = einfo;
		ctype_get(cp->cts, eid)->size = esize;
	}
	return eid;
}

/* Parse declaration specifiers. */
static CPscl cp_decl_spec(CPState *cp, CPDecl *decl, CPscl scl)
{
	uint32_t cds = 0, sz = 0;
	CTypeID tdef = 0;

	decl->cp = cp;
	decl->mode = cp->mode;
	decl->uv_name = NULL;
	decl->uv_redir = NULL;
	decl->attr = 0;
	decl->fattr = 0;
	decl->pos = decl->top = 0;
	decl->stack[0].next = 0;

	for (;;)
	{ /* Parse basic types. */
		cp_decl_attributes(cp, decl);
		if (cp->error)
			return 0;
		if (cp->tok >= CTOK_FIRSTDECL && cp->tok <= CTOK_LASTDECLFLAG)
		{
			uint32_t cbit;
			if (cp->ct->size)
			{
				if (sz)
					goto end_decl;
				sz = cp->ct->size;
			}
			cbit = (1u << (cp->tok - CTOK_FIRSTDECL));
			cds = cds | cbit | ((cbit & cds & CDF_LONG) << 1);
			if (cp->tok >= CTOK_FIRSTSCL)
			{
				if (!(scl & cbit))
				{
					cp_errmsg(cp, cp->tok, "bad storage class");
					return 0;
				}
			}
			else if (tdef)
			{
				goto end_decl;
			}
			cp_next(cp);
			continue;
		}
		if (sz || tdef ||
			(cds & (CDF_SHORT | CDF_LONG | CDF_SIGNED | CDF_UNSIGNED | CDF_COMPLEX)))
			break;
		switch (cp->tok)
		{
		case CTOK_STRUCT:
			tdef = cp_decl_struct(cp, decl, CTINFO(CT_STRUCT, 0));
			if (cp->error)
				return 0;
			continue;
		case CTOK_UNION:
			tdef = cp_decl_struct(cp, decl, CTINFO(CT_STRUCT, CTF_UNION));
			if (cp->error)
				return 0;
			continue;
		case CTOK_ENUM:
			tdef = cp_decl_enum(cp, decl);
			if (cp->error)
				return 0;
			continue;
		case CTOK_IDENT:
			if (ctype_istypedef(cp->ct->info))
			{
				tdef = ctype_cid(cp->ct->info); /* Get typedef. */
				cp_next(cp);
				continue;
			}
			break;
		case '$':
			tdef = cp->val.id;
			cp_next(cp);
			continue;
		default:
			break;
		}
		break;
	}
end_decl:

	if ((cds & CDF_COMPLEX)) /* Use predefined complex types. */
		tdef = sz == 4 ? CTID_COMPLEX_FLOAT : CTID_COMPLEX_DOUBLE;

	if (tdef)
	{
		cp_push_type(decl, tdef);
	}
	else if ((cds & CDF_VOID))
	{
		cp_push(decl, CTINFO(CT_VOID, (decl->attr & CTF_QUAL)), CTSIZE_INVALID);
		decl->attr &= ~CTF_QUAL;
	}
	else
	{
		/* Determine type info and size. */
		CTInfo info = CTINFO(CT_NUM, (cds & CDF_UNSIGNED) ? CTF_UNSIGNED : 0);
		if ((cds & CDF_BOOL))
		{
			if ((cds & ~(CDF_SCL | CDF_BOOL | CDF_INT | CDF_SIGNED | CDF_UNSIGNED)))
			{
				cp_errmsg(cp, 0, "invalid C type");
				return 0;
			}
			info |= CTF_BOOL;
			if (!(cds & CDF_SIGNED))
				info |= CTF_UNSIGNED;
			if (!sz)
			{
				sz = 1;
			}
		}
		else if ((cds & CDF_FP))
		{
			info = CTINFO(CT_NUM, CTF_FP);
			if ((cds & CDF_LONG))
				sz = sizeof(long double);
		}
		else if ((cds & CDF_CHAR))
		{
			if ((cds & (CDF_CHAR | CDF_SIGNED | CDF_UNSIGNED)) == CDF_CHAR)
				info |= CTF_UCHAR; /* Handle platforms where char is unsigned. */
		}
		else if ((cds & CDF_SHORT))
		{
			sz = sizeof(short);
		}
		else if ((cds & CDF_LONGLONG))
		{
			sz = 8;
		}
		else if ((cds & CDF_LONG))
		{
			info |= CTF_LONG;
			sz = sizeof(long);
		}
		else if (!sz)
		{
			if (!(cds & (CDF_SIGNED | CDF_UNSIGNED)))
			{
				cp_errmsg(cp, cp->tok, "declaration specifier expected");
				return 0;
			}
			sz = sizeof(int);
		}
		uc_assertCP(sz != 0, "basic ctype with zero size");
		info += CTALIGN(uc_fls(sz));	 /* Use natural alignment. */
		info += (decl->attr & CTF_QUAL); /* Merge qualifiers. */
		cp_push(decl, info, sz);
		decl->attr &= ~CTF_QUAL;
	}
	decl->specpos = decl->pos;
	decl->specattr = decl->attr;
	decl->specfattr = decl->fattr;
	return (cds & CDF_SCL); /* Return storage class. */
}

/* Parse array declaration. */
static void cp_decl_array(CPState *cp, CPDecl *decl)
{
	CTInfo info = CTINFO(CT_ARRAY, 0);
	CTSize nelem = CTSIZE_INVALID; /* Default size for a[] or a[?]. */
	cp_decl_attributes(cp, decl);
	if (cp->error)
		return;
	if (cp_opt(cp, '?'))
		info |= CTF_VLA; /* Create variable-length array a[?]. */
	else if (cp->tok != ']')
	{
		nelem = cp_expr_ksize(cp);
		if (cp->error)
			return;
	}
	cp_check(cp, ']');
	cp_add(decl, info, nelem);
}

/* Parse function declaration. */
static void cp_decl_func(CPState *cp, CPDecl *fdecl)
{
	CTSize nargs = 0;
	CTInfo info = CTINFO(CT_FUNC, 0);
	CTypeID lastid = 0, anchor = 0;
	if (cp->tok != ')')
	{
		do
		{
			CPDecl decl = {0};
			CTypeID ctypeid, fieldid;
			CType *ct;
			if (cp_opt(cp, '.'))
			{					   /* Vararg function. */
				cp_check(cp, '.'); /* Workaround for the minimalistic lexer. */
				cp_check(cp, '.');
				info |= CTF_VARARG;
				break;
			}
			cp_decl_spec(cp, &decl, CDF_REGISTER);
			if (cp->error)
				return;
			decl.mode = CPARSE_MODE_DIRECT | CPARSE_MODE_ABSTRACT;
			cp_declarator(cp, &decl);
			if (cp->error)
				return;
			ctypeid = cp_decl_intern(cp, &decl);
			if (cp->error)
				return;
			ct = ctype_raw(cp->cts, ctypeid);
			if (ctype_isvoid(ct->info))
				break;
			else if (ctype_isrefarray(ct->info))
				ctypeid = uc_ctype_intern(cp->cts,
										  CTINFO(CT_PTR, CTALIGN_PTR | ctype_cid(ct->info)), CTSIZE_PTR);
			else if (ctype_isfunc(ct->info))
				ctypeid = uc_ctype_intern(cp->cts,
										  CTINFO(CT_PTR, CTALIGN_PTR | ctypeid), CTSIZE_PTR);
			/* Add new parameter. */
			fieldid = uc_ctype_new(cp->cts, &ct);
			if (anchor)
				ctype_get(cp->cts, lastid)->sib = fieldid;
			else
				anchor = fieldid;
			lastid = fieldid;
			if (decl.uv_name)
				ctype_setname(ct, decl.uv_name);
			ct->info = CTINFO(CT_FIELD, ctypeid);
			ct->size = nargs++;
			cp_decl_reset(&decl);
		} while (cp_opt(cp, ','));
	}
	cp_check(cp, ')');
	if (cp->error)
		return;
	if (cp_opt(cp, '{'))
	{ /* Skip function definition. */
		int level = 1;
		cp->mode |= CPARSE_MODE_SKIP;
		for (;;)
		{
			if (cp->tok == '{')
				level++;
			else if (cp->tok == '}' && --level == 0)
				break;
			else if (cp->tok == CTOK_EOF) {
				cp_err_token(cp, '}');
				return;
			}
			cp_next(cp);
		}
		cp->mode &= ~CPARSE_MODE_SKIP;
		cp->tok = ';'; /* Ok for cp_decl_multi(), error in cp_decl_single(). */
	}
	info |= (fdecl->fattr & ~CTMASK_CID);
	fdecl->fattr = 0;
	fdecl->stack[cp_add(fdecl, info, nargs)].sib = anchor;
}

/* Parse declarator. */
static void cp_declarator(CPState *cp, CPDecl *decl)
{
	if (++cp->depth > CPARSE_MAX_DECLDEPTH)
	{
		cp_err(cp, "chunk has too many syntax levels");
		return;
	}

	for (;;)
	{ /* Head of declarator. */
		if (cp_opt(cp, '*'))
		{ /* Pointer. */
			CTSize sz;
			CTInfo info;
			cp_decl_attributes(cp, decl);
			if (cp->error)
				return;
			sz = CTSIZE_PTR;
			info = CTINFO(CT_PTR, CTALIGN_PTR);
#if UC_64
			if (ctype_msizeP(decl->attr) == 4)
			{
				sz = 4;
				info = CTINFO(CT_PTR, CTALIGN(2));
			}
#endif
			info += (decl->attr & (CTF_QUAL | CTF_REF));
			decl->attr &= ~(CTF_QUAL | (CTMASK_MSIZEP << CTSHIFT_MSIZEP));
			cp_push(decl, info, sz);
		}
		else if (cp_opt(cp, '&') || cp_opt(cp, CTOK_ANDAND))
		{ /* Reference. */
			decl->attr &= ~(CTF_QUAL | (CTMASK_MSIZEP << CTSHIFT_MSIZEP));
			cp_push(decl, CTINFO_REF(0), CTSIZE_PTR);
		}
		else
		{
			break;
		}
	}

	if (cp_opt(cp, '('))
	{ /* Inner declarator. */
		CPDeclIdx pos;
		cp_decl_attributes(cp, decl);
		if (cp->error)
			return;
		/* Resolve ambiguity between inner declarator and 1st function parameter. */
		if ((decl->mode & CPARSE_MODE_ABSTRACT) &&
			(cp->tok == ')' || cp_istypedecl(cp)))
			goto func_decl;
		pos = decl->pos;
		cp_declarator(cp, decl);
		if (cp->error)
			return;
		cp_check(cp, ')');
		if (cp->error)
			return;
		decl->pos = pos;
	}
	else if (cp->tok == CTOK_IDENT)
	{ /* Direct declarator. */
		if (!(decl->mode & CPARSE_MODE_DIRECT))
		{
			cp_err_token(cp, CTOK_EOF);
			return;
		}
		decl->uv_name = ucv_get(cp->uv_str);
		decl->nameid = cp->val.id;
		cp_next(cp);
	}
	else
	{ /* Abstract declarator. */
		if (!(decl->mode & CPARSE_MODE_ABSTRACT))
		{
			cp_err_token(cp, CTOK_IDENT);
			return;
		}
	}

	for (;;)
	{ /* Tail of declarator. */
		if (cp_opt(cp, '['))
		{ /* Array. */
			cp_decl_array(cp, decl);
			if (cp->error)
				return;
		}
		else if (cp_opt(cp, '('))
		{ /* Function. */
		func_decl:
			cp_decl_func(cp, decl);
			if (cp->error)
				return;
		}
		else
		{
			break;
		}
	}

	if ((decl->mode & CPARSE_MODE_FIELD) && cp_opt(cp, ':')) /* Field width. */
	{
		decl->bits = cp_expr_ksize(cp);
		if (cp->error)
			return;
	}

	/* Process postfix attributes. */
	cp_decl_attributes(cp, decl);
	if (cp->error)
		return;
	cp_push_attributes(decl);

	cp->depth--;
}

/* Parse an abstract type declaration and return it's C type ID. */
static CTypeID cp_decl_abstract(CPState *cp)
{
	CPDecl decl = {0};
	cp_decl_spec(cp, &decl, 0);
	if (cp->error)
		return 0;
	decl.mode = CPARSE_MODE_ABSTRACT;
	cp_declarator(cp, &decl);
	if (cp->error)
		return 0;
	CTypeID rv = cp_decl_intern(cp, &decl);
	if (cp->error)
		return 0;
	cp_decl_reset(&decl);
	return rv;
}

/* Handle pragmas. */
static void cp_pragma(CPState *cp, size_t pragmaline)
{
	cp_next(cp);
	if (cp->error)
		return;
	if (cp->tok == CTOK_IDENT && cp_str_is(cp->uv_str, "pack"))
	{
		cp_next(cp);
		if (cp->error)
			return;
		cp_check(cp, '(');
		if (cp->error)
			return;
		if (cp->tok == CTOK_IDENT)
		{
			if (cp_str_is(cp->uv_str, "push"))
			{
				if (cp->curpack < CPARSE_MAX_PACKSTACK - 1)
				{
					cp->packstack[cp->curpack + 1] = cp->packstack[cp->curpack];
					cp->curpack++;
				}
				else
				{
					cp_errmsg(cp, cp->tok, "chunk has too many syntax levels");
					return;
				}
			}
			else if (cp_str_is(cp->uv_str, "pop"))
			{
				if (cp->curpack > 0)
					cp->curpack--;
			}
			else
			{
				cp_errmsg(cp, cp->tok, "unexpected symbol");
				return;
			}
			cp_next(cp);
			if (cp->error)
				return;
			if (!cp_opt(cp, ','))
				goto end_pack;
		}
		if (cp->tok == CTOK_INTEGER)
		{
			cp->packstack[cp->curpack] = cp->val.u32 ? uc_fls(cp->val.u32) : 0;
			cp_next(cp);
			if (cp->error)
				return;
		}
		else
		{
			cp->packstack[cp->curpack] = 255;
		}
	end_pack:
		cp_check(cp, ')');
		if (cp->error)
			return;
	}
	else
	{ /* Ignore all other pragmas. */
		while (cp->tok != CTOK_EOF && cp->linenumber == pragmaline)
			cp_next(cp);
	}
}

/* Handle line number. */
static void cp_line(CPState *cp, size_t hashline)
{
	size_t newline = cp->val.u32;
	/* TODO: Handle file name and include it in error messages. */
	while (cp->tok != CTOK_EOF && cp->linenumber == hashline)
	{
		cp_next(cp);
		if (cp->error)
			return;
	}
	cp->linenumber = newline;
}

/* Parse multiple C declarations of types or extern identifiers. */
static void cp_decl_multi(CPState *cp)
{
	int first = 1;
	while (cp->tok != CTOK_EOF)
	{
		CPDecl decl = {0};
		CPscl scl;
		if (cp_opt(cp, ';'))
		{ /* Skip empty statements. */
			first = 0;
			continue;
		}
		if (cp->tok == '#')
		{ /* Workaround, since we have no preprocessor, yet. */
			size_t hashline = cp->linenumber;
			CPToken tok = cp_next(cp);
			if (cp->error)
				return;
			if (tok == CTOK_INTEGER)
			{
				cp_line(cp, hashline);
				if (cp->error)
					return;
				continue;
			}
			else if (tok == CTOK_IDENT && cp_str_is(cp->uv_str, "line"))
			{
				if (cp_next(cp) != CTOK_INTEGER) {
					cp_err_token(cp, tok);
					return;
				}
				cp_line(cp, hashline);
				if (cp->error)
					return;
				continue;
			}
			else if (tok == CTOK_IDENT && cp_str_is(cp->uv_str, "pragma"))
			{
				cp_pragma(cp, hashline);
				if (cp->error)
					return;
				continue;
			}
			else
			{
				cp_errmsg(cp, cp->tok, "unexpected symbol");
				return;
			}
		}
		scl = cp_decl_spec(cp, &decl, CDF_TYPEDEF | CDF_EXTERN | CDF_STATIC);
		if (cp->error)
			return;
		if ((cp->tok == ';' || cp->tok == CTOK_EOF) &&
			ctype_istypedef(decl.stack[0].info))
		{
			CTInfo info = ctype_rawchild(cp->cts, &decl.stack[0])->info;
			if (ctype_isstruct(info) || ctype_isenum(info))
				goto decl_end; /* Accept empty declaration of struct/union/enum. */
		}
		for (;;)
		{
			CTypeID ctypeid;
			cp_declarator(cp, &decl);
			if (cp->error)
				return;
			ctypeid = cp_decl_intern(cp, &decl);
			if (cp->error)
				return;
			if (decl.uv_name)
			{
				if (decl.nameid)
				{ /* Redeclaration detected - check compatibility. */
					CType *existing_ct = ctype_get(cp->cts, decl.nameid);
					CType *new_ct = ctype_get(cp->cts, ctypeid);

					/* Skip extern and attributes to get the actual function type. */
					CType *existing_func = existing_ct;
					while (ctype_isextern(existing_func->info) || ctype_isattrib(existing_func->info))
						existing_func = ctype_rawchild(cp->cts, existing_func);

					/* Unwrap CT_FUNC if needed. */
					CType *new_func = new_ct;
					if (ctype_isptr(new_ct->info))
						new_func = ctype_rawchild(cp->cts, new_ct);

					if (ctype_isfunc(existing_func->info) && ctype_isfunc(new_func->info) &&
					    ctype_func_is_equiv(cp->cts, existing_func, new_func))
					{ /* Compatible: reuse existing type. */
						if (!existing_ct->uv_name)
							ctype_setname(existing_ct, decl.uv_name);
						ctypeid = decl.nameid; /* Use existing ID */
						cp->val.id = ctypeid; /* Update parser state */
						/* Add to hash table with the name. */
						uc_ctype_addname(cp->cts, existing_ct, ctypeid);
						/* Record function type ID if func_ids vector is provided */
						if (cp->func_ids && (ctype_isfunc(existing_ct->info) ||
						    (ctype_isextern(existing_ct->info) && ctype_isfunc(ctype_get(cp->cts, ctype_cid(existing_ct->info))->info)))) {
							uc_vector_push(cp->func_ids, ctypeid);
						}
					}
					else
					{ /* Incompatible redeclaration. */
						cp_errmsg(cp, 0, "redeclaration of '%s' as different type",
						          ucv_string_get(decl.uv_name));
					}
				}
				else
				{ /* New declaration. */
					CType *ct;
					CTypeID id;
					if ((scl & CDF_TYPEDEF))
					{ /* Create new typedef. */
						id = uc_ctype_new(cp->cts, &ct);
						ct->info = CTINFO(CT_TYPEDEF, ctypeid);
						goto noredir;
					}
					else if (ctype_isfunc(ctype_get(cp->cts, ctypeid)->info))
					{
						/* Treat both static and extern function declarations as extern. */
						ct = ctype_get(cp->cts, ctypeid);
						/* We always get new anonymous functions (typedefs are copied). */
						uc_assertCP(ct->uv_name == NULL, "unexpected named function");
						id = ctypeid; /* Just name it. */
					}
					else if ((scl & CDF_STATIC))
					{ /* Accept static constants. */
						id = cp_decl_constinit(cp, &ct, ctypeid);
						goto noredir;
					}
					else
					{ /* External references have extern or no storage class. */
						id = uc_ctype_new(cp->cts, &ct);
						ct->info = CTINFO(CT_EXTERN, ctypeid);
					}
					if (decl.uv_redir)
					{ /* Add attribute for redirected symbol name. */
						CType *cta;
						CTypeID aid = uc_ctype_new(cp->cts, &cta);
						ct = ctype_get(cp->cts, id); /* Table may have been reallocated. */
						cta->info = CTINFO(CT_ATTRIB, CTATTRIB(CTA_REDIR));
						cta->sib = ct->sib;
						ct->sib = aid;
						ctype_setname(cta, decl.uv_redir);
					}
				noredir:
					ctype_setname(ct, decl.uv_name);
					uc_ctype_addname(cp->cts, ct, id);

					/* Record function type ID if func_ids vector is provided */
					if (cp->func_ids && (ctype_isfunc(ct->info) ||
					    (ctype_isextern(ct->info) && ctype_isfunc(ctype_get(cp->cts, ctype_cid(ct->info))->info)))) {
						uc_vector_push(cp->func_ids, id);
					}
				}
			}
			cp_decl_reset(&decl);
			if (!cp_opt(cp, ','))
				break;
		}
	decl_end:
		if (cp->tok == CTOK_EOF && first)
			break; /* May omit ';' for 1 decl. */
		first = 0;
		cp_check(cp, ';');
	}
}

/* Parse a single C type declaration. */
static void cp_decl_single(CPState *cp)
{
	CPDecl decl = {0};
	cp_decl_spec(cp, &decl, 0);
	if (cp->error)
		return;
	cp_declarator(cp, &decl);
	if (cp->error)
		return;
	cp->val.id = cp_decl_intern(cp, &decl);
	if (cp->error)
		return;

	CTypeID ctypeid = cp->val.id;

	if (decl.uv_name)
	{
		if (decl.nameid)
		{ /* Redeclaration detected - check compatibility. */
			CType *existing_ct = ctype_get(cp->cts, decl.nameid);
			CType *new_ct = ctype_get(cp->cts, ctypeid);

			/* Skip extern and attributes to get the actual function type. */
			CType *existing_func = existing_ct;
			while (ctype_isextern(existing_func->info) || ctype_isattrib(existing_func->info))
				existing_func = ctype_rawchild(cp->cts, existing_func);

			/* Unwrap CT_PTR if needed. */
			CType *new_func = new_ct;
			if (ctype_isptr(new_ct->info))
				new_func = ctype_rawchild(cp->cts, new_ct);

			if (ctype_isfunc(existing_func->info) && ctype_isfunc(new_func->info) &&
			    ctype_func_is_equiv(cp->cts, existing_func, new_func))
			{ /* Compatible: reuse existing type. */
				if (!existing_ct->uv_name)
					ctype_setname(existing_ct, decl.uv_name);
				ctypeid = decl.nameid; /* Use existing ID */
				cp->val.id = ctypeid; /* Update parser state */
				/* Add to hash table with the name. */
				uc_ctype_addname(cp->cts, existing_ct, ctypeid);
			}
			else
			{ /* Incompatible redeclaration. */
				cp_errmsg(cp, 0, "redeclaration of '%s' as different type",
				          ucv_string_get(decl.uv_name));
			}
		}
		else
		{ /* New declaration. */
			CType *ct;
			CTypeID id;

			/* Treat both static and extern function declarations as extern. */
			ct = ctype_get(cp->cts, ctypeid);
			/* We always get new anonymous functions (typedefs are copied). */
			uc_assertCP(ct->uv_name == NULL, "unexpected named function");
			id = ctypeid; /* Just name it. */

			if (ctype_isfunc(ct->info) && decl.uv_redir)
			{ /* Add attribute for redirected symbol name. */
				CType *cta;
				CTypeID aid = uc_ctype_new(cp->cts, &cta);
				ct = ctype_get(cp->cts, id); /* Table may have been reallocated. */
				cta->info = CTINFO(CT_ATTRIB, CTATTRIB(CTA_REDIR));
				cta->sib = ct->sib;
				ct->sib = aid;
				ctype_setname(cta, decl.uv_redir);
			}

			ctype_setname(ct, decl.uv_name);
			uc_ctype_addname(cp->cts, ct, id);
		}
	}

	cp_decl_reset(&decl);

	if (cp->tok != CTOK_EOF)
		cp_err_token(cp, CTOK_EOF);
}

/* ------------------------------------------------------------------------ */

/* C parser. */
bool uc_cparse(CPState *cp)
{
	bool rv = true;

	cp_init(cp);

	if ((cp->mode & CPARSE_MODE_MULTI))
		cp_decl_multi(cp);
	else
		cp_decl_single(cp);

	if (cp->uv_param && cp->uv_param != &cp->uv_vm->stack.entries[cp->uv_vm->stack.count])
		cp_err(cp, "wrong number of type parameters");

	uc_assertCP(cp->depth == 0, "unbalanced cparser declaration depth");

	if (cp->error) {
		uc_vm_raise_exception(cp->cts->vm, EXCEPTION_SYNTAX,
			"invalid C type: %s", cp->error);

		rv = false;
	}

	cp_cleanup(cp);

	return rv;
}

/*
** C declaration parser.
** Copyright (C) 2005-2025 Mike Pall. See Copyright Notice below.
**
** This header contains derived work from LuaJIT's FFI C parser.
**
** Modifications:
** - Adapted for ucode VM integration
** - Removed JIT-specific dependencies
**
** See NOTICE and ATTRIBUTION.md for complete attribution details.
*/

#ifndef _UC_CPARSE_H
#define _UC_CPARSE_H

#include "ucode/types.h"
#include "ucode/vm.h"

#include "uc_def.h"
#include "uc_ctype.h"

/* C parser limits. */
#define CPARSE_MAX_BUF		32768	/* Max. token buffer size. */
#define CPARSE_MAX_DECLSTACK	100	/* Max. declaration stack depth. */
#define CPARSE_MAX_DECLDEPTH	20	/* Max. recursive declaration depth. */
#define CPARSE_MAX_PACKSTACK	7	/* Max. pack pragma stack depth. */

/* Flags for C parser mode. */
#define CPARSE_MODE_MULTI	1	/* Process multiple declarations. */
#define CPARSE_MODE_ABSTRACT	2	/* Accept abstract declarators. */
#define CPARSE_MODE_DIRECT	4	/* Accept direct declarators. */
#define CPARSE_MODE_FIELD	8	/* Accept field width in bits, too. */
#define CPARSE_MODE_NOIMPLICIT	16	/* Reject implicit declarations. */
#define CPARSE_MODE_SKIP	32	/* Skip definitions, ignore errors. */

typedef int CPChar;	/* C parser character. Unsigned ext. from char. */
typedef int CPToken;	/* C parser token. */

/* C parser internal value representation. */
typedef struct CPValue {
  union {
    int32_t i32;	/* Value for CTID_INT32. */
    uint32_t u32;	/* Value for CTID_UINT32. */
  };
  CTypeID id;		/* C Type ID of the value. */
} CPValue;

/* C parser state. */
typedef struct CPState {
  CPChar c;		/* Current character. */
  CPToken tok;		/* Current token. */
  CPValue val;		/* Token value. */
  CType *ct;		/* C type table entry. */
  const char *p;	/* Current position in input buffer. */
  CTState *cts;		/* C type state. */
  const char *srcname;	/* Current source name. */
  size_t linenumber;	/* Input line counter. */
  int depth;		/* Recursive declaration depth. */
  uint32_t tmask;	/* Type mask for next identifier. */
  uint32_t mode;	/* C parser mode. */
  uint8_t packstack[CPARSE_MAX_PACKSTACK];  /* Stack for pack pragmas. */
  uint8_t curpack;	/* Current position in pack pragma stack. */
  uc_vm_t *uv_vm;
  uc_stringbuf_t pb;
  uc_value_t *uv_str;
  uc_value_t **uv_param;
  char *error;
  struct cp_func_ids_buf { size_t count; CTypeID *entries; size_t capacity; } func_ids_buf;  /* Optional buffer to collect function type IDs */
  struct cp_func_ids_buf *func_ids;     /* Pointer to func_ids_buf or external buffer */
} CPState;

UC_NOAPI bool uc_cparse(CPState *cp);

UC_NOAPI int uc_cparse_case(uc_value_t *str, const char *match);

#endif

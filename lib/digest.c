/*
 * Copyright (C) 2024 Sebastian Ertz <sebastian.ertz@gmx.de>
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

/**
 * # Digest Functions
 *
 * The `digest` module bundles various digest functions.
 *
 * @module digest
 */
#include <sys/random.h>

#include <md5.h>
#include <sha1.h>
#include <sha2.h>

#ifdef HAVE_DIGEST_EXTENDED
#include <md2.h>
#include <md4.h>
#endif

#include "ucode/module.h"

#define UC_DIGEST_MD5_CRYPT_SALT_LEN		8
#define UC_DIGEST_MD5_CRYPT_HEX_LEN		4

static uc_value_t *
uc_digest_calc_data(uc_value_t *str, char *(*fn)(const uint8_t *,size_t,char *))
{
	char buf[SHA512_DIGEST_STRING_LENGTH];

	if( ucv_type(str) != UC_STRING )
		return NULL;

	if( fn((const uint8_t *)ucv_string_get(str), ucv_string_length(str), buf) )
		return ucv_string_new(buf);

	return NULL;
}

static uc_value_t *
uc_digest_calc_file(uc_value_t *path, char *(fn)(const char *,char *))
{
	char buf[SHA512_DIGEST_STRING_LENGTH];

	if( ucv_type(path) != UC_STRING )
		return NULL;

	if( fn(ucv_string_get(path), buf) )
		return ucv_string_new(buf);

	return NULL;
}

static char *
uc_digest_fnv1a64_data_hash(const uint8_t *data, size_t len, char *out)
{
	static const char hex[] = "0123456789abcdef";
	uint64_t hash = 0xCBF29CE484222325;

	for (size_t i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 0x00000100000001B3;
	}

	for (int i = 0; i < 8; i++) {
		uint8_t byte = (hash >> ((7 - i) * 8)) & 0xFF;
		out[i * 2]     = hex[byte >> 4];
		out[i * 2 + 1] = hex[byte & 0xF];
	}

	out[16] = '\0';
	return out;
}

static char *
uc_digest_fnv1a64_file_hash(const char *path, char *out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

	static const char hex[] = "0123456789abcdef";
	uint64_t hash = 0xCBF29CE484222325;
	unsigned char buf[4096];
	size_t n;

	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		for (size_t i = 0; i < n; i++) {
			hash ^= buf[i];
			hash *= 0x00000100000001B3;
		}
	}

	if (ferror(f)) {
		fclose(f);
		return NULL;
	}

	fclose(f);

	for (int i = 0; i < 8; i++) {
		uint8_t byte = (hash >> ((7 - i) * 8)) & 0xFF;
		out[i * 2]     = hex[byte >> 4];
		out[i * 2 + 1] = hex[byte & 0xF];
	}

	out[16] = '\0';
	return out;
}

/**
 * Calculates the MD5 hash of string and returns that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#md5
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * md5("This is a test");  // Returns "ce114e4501d2f4e2dcea3e17b546f339"
 * md5(123);               // Returns null
 */
static uc_value_t *
uc_digest_md5(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), MD5Data);
}

static const char Crypt_Base64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void uc_digest_md5_crypt_to64(char buf[UC_DIGEST_MD5_CRYPT_HEX_LEN + 1],
				     unsigned long data, int len)
{
	for (int i = 0; i < len; i++) {
		buf[i] = Crypt_Base64[data & 0x3f];
		data >>= 6;
	}

	buf[len] = '\0';
}

static int
uc_digest_md5_crypt_gen_salt(char buf[UC_DIGEST_MD5_CRYPT_SALT_LEN + 1])
{
	int ret;
	int i;

	ret = getentropy(buf, UC_DIGEST_MD5_CRYPT_SALT_LEN);
	if (ret)
		return ret;

	for (i = 0; i < UC_DIGEST_MD5_CRYPT_SALT_LEN + 1; i++)
		buf[i] = Crypt_Base64[buf[i] & 0x3F];

	buf[UC_DIGEST_MD5_CRYPT_SALT_LEN] = '\0';

	return 0;
}

/**
 * Generate a random salt, calculates the MD5 Crypt hash
 * of string and returns the full shadow entry.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#md5_crypt
 *
 * @param {string} str
 * The string to generate shadow entry for.
 *
 * @returns {?string}
 *
 * @example
 * md5_crypt("test");  // Returns "$1$2NhbKqI8$qxTnya0K2/Fy9CNx0BlM4/"
 */
static uc_value_t *
uc_digest_md5_crypt(uc_vm_t *vm, size_t nargs)
{
	unsigned char buf[MD5_DIGEST_STRING_LENGTH];
	char salt_buf[UC_DIGEST_MD5_CRYPT_SALT_LEN + 1];
	char hex_buf[UC_DIGEST_MD5_CRYPT_HEX_LEN + 1];
	unsigned int data_len, salt_len;
	const uint8_t *data, *salt;
	const char *ident = "$1$";
	MD5_CTX ctx, alt_ctx;
	uc_stringbuf_t *res;
	uc_value_t *str;
	int ret;
	int i;

	str = uc_fn_arg(0);
	ret = uc_digest_md5_crypt_gen_salt(salt_buf);
	if (ret)
		return NULL;

	data = (const uint8_t *)ucv_string_get(str);
	data_len = ucv_string_length(str);

	salt = (const uint8_t *)salt_buf;
	salt_len = 8;

	MD5Init(&ctx);

	/* string + $1$ + salt */
	MD5Update(&ctx, data, data_len);
	MD5Update(&ctx, (const uint8_t *)ident, strlen(ident));
	MD5Update(&ctx, salt, salt_len);

	/* string + salt + string */
	MD5Init(&alt_ctx);
	MD5Update(&alt_ctx, data, data_len);
	MD5Update(&alt_ctx, salt, salt_len);
	MD5Update(&alt_ctx, data, data_len);
	MD5Final(buf, &alt_ctx);

	for (i = data_len; i > MD5_DIGEST_LENGTH; i -= MD5_DIGEST_LENGTH)
		MD5Update(&ctx, buf, MD5_DIGEST_LENGTH);
	MD5Update(&ctx, buf, i);

	for (i = data_len; i > 0; i >>= 1)
		if (i & 1)
			MD5Update(&ctx, (const uint8_t *)"\0", 1);
		else
			MD5Update(&ctx, data, 1);

	MD5Final(buf, &ctx);

	for (i = 0; i < 1000; i++) {
		MD5Init(&ctx);

		if (i & 1)
			MD5Update(&ctx, data, data_len);
		else
			MD5Update(&ctx, buf, MD5_DIGEST_LENGTH);

		if (i % 3)
			MD5Update(&ctx, salt, salt_len);

		if (i % 7)
			MD5Update(&ctx, data, data_len);

		if (i & 1)
			MD5Update(&ctx, buf, MD5_DIGEST_LENGTH);
		else
			MD5Update(&ctx, data, data_len);

		MD5Final(buf, &ctx);
	}

	res = ucv_stringbuf_new();
	ucv_stringbuf_addstr(res, ident, strlen(ident));
	ucv_stringbuf_addstr(res, (const char *)salt, salt_len);
	ucv_stringbuf_append(res, "$");

	/* Apply strange byte ordering */
	for (i = 0; i <= MD5_DIGEST_LENGTH; i+=4) {
		unsigned int hex_data;

		hex_data = buf[(i/4)] << 16;
		hex_data |= buf[6+(i/4)] << 8;
		hex_data |= 12+(i/4) < MD5_DIGEST_LENGTH ? buf[12+(i/4)] : buf[5];

		uc_digest_md5_crypt_to64(hex_buf, hex_data,
					 UC_DIGEST_MD5_CRYPT_HEX_LEN);
		ucv_stringbuf_addstr(res, hex_buf, strlen(hex_buf));
	}
	uc_digest_md5_crypt_to64(hex_buf, buf[11],
				 UC_DIGEST_MD5_CRYPT_HEX_LEN - 2);
	ucv_stringbuf_addstr(res, hex_buf, strlen(hex_buf));

	memset(&ctx, '\0', sizeof(ctx));
	memset(&alt_ctx, '\0', sizeof(alt_ctx));
	memset(salt_buf, '\0', sizeof(salt_buf));
	memset(buf, '\0', sizeof(buf));
	memset(hex_buf, '\0', sizeof(hex_buf));

	return ucv_stringbuf_finish(res);
}

/**
 * Calculates the SHA1 hash of string and returns that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#sha1
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * sha1("This is a test");  // Returns "a54d88e06612d820bc3be72877c74f257b561b19"
 * sha1(123);               // Returns null
 */
static uc_value_t *
uc_digest_sha1(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), SHA1Data);
}

/**
 * Calculates the SHA256 hash of string and returns that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#sha256
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * sha256("This is a test");  // Returns "c7be1ed902fb8dd4d48997c6452f5d7e509fbcdbe2808b16bcf4edce4c07d14e"
 * sha256(123);               // Returns null
 */
static uc_value_t *
uc_digest_sha256(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), SHA256Data);
}

/**
 * Calculates the 64-bit FNV-1a non-cryptographic hash of string and returns
 * that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#fnv1a64
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * fnv1a64("This is a test");  // Returns "25f0b040ca8b4ce0"
 * fnv1a64(123);               // Returns null
 */
static uc_value_t *
uc_digest_fnv1a64(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), uc_digest_fnv1a64_data_hash);
}

/**
 * Calculates the 64-bit FNV-1a non-cryptographic hash of a given file and
 * returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#fnv1a64_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_fnv1a64_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), uc_digest_fnv1a64_file_hash);
}

#ifdef HAVE_DIGEST_EXTENDED
/**
 * Calculates the MD2 hash of string and returns that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#md2
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * md2("This is a test");  // Returns "dc378580fd0722e56b82666a6994c718"
 * md2(123);               // Returns null
 */
static uc_value_t *
uc_digest_md2(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), MD2Data);
}

/**
 * Calculates the MD4 hash of string and returns that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#md4
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * md4("This is a test");  // Returns "3b487cf6856af7e330bc4b1b7d977ef8"
 * md4(123);               // Returns null
 */
static uc_value_t *
uc_digest_md4(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), MD4Data);
}

/**
 * Calculates the SHA384 hash of string and returns that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#sha384
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * sha384("This is a test");  // Returns "a27c7667e58200d4c0688ea136968404a0da366b1a9fc19bb38a0c7a609a1eef2bcc82837f4f4d92031a66051494b38c"
 * sha384(123);               // Returns null
 */
static uc_value_t *
uc_digest_sha384(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), SHA384Data);
}

/**
 * Calculates the SHA512 hash of string and returns that hash.
 *
 * Returns `null` if a non-string argument is given.
 *
 * @function module:digest#sha512
 *
 * @param {string} str
 * The string to hash.
 *
 * @returns {?string}
 *
 * @example
 * sha512("This is a test");  // Returns "a028d4f74b602ba45eb0a93c9a4677240dcf281a1a9322f183bd32f0bed82ec72de9c3957b2f4c9a1ccf7ed14f85d73498df38017e703d47ebb9f0b3bf116f69"
 * sha512(123);               // Returns null
 */
static uc_value_t *
uc_digest_sha512(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_data(uc_fn_arg(0), SHA512Data);
}
#endif

/**
 * Calculates the MD5 hash of a given file and returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#md5_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_md5_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), MD5File);
}

/**
 * Calculates the SHA1 hash of a given file and returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#sha1_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_sha1_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), SHA1File);
}

/**
 * Calculates the SHA256 hash of a given file and returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#sha256_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_sha256_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), SHA256File);
}

#ifdef HAVE_DIGEST_EXTENDED
/**
 * Calculates the MD2 hash of a given file and returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#md2_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_md2_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), MD2File);
}

/**
 * Calculates the MD4 hash of a given file and returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#md4_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_md4_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), MD4File);
}

/**
 * Calculates the SHA384 hash of a given file and returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#sha384_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_sha384_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), SHA384File);
}

/**
 * Calculates the SHA512 hash of a given file and returns that hash.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:digest#sha512_file
 *
 * @param {string} path
 * The path to the file.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_digest_sha512_file(uc_vm_t *vm, size_t nargs)
{
	return uc_digest_calc_file(uc_fn_arg(0), SHA512File);
}
#endif


static const uc_function_list_t global_fns[] = {
	{ "md5",         uc_digest_md5         },
	{ "sha1",        uc_digest_sha1        },
	{ "sha256",      uc_digest_sha256      },
	{ "fnv1a64",     uc_digest_fnv1a64     },
	{ "md5_file",    uc_digest_md5_file    },
	{ "md5_crypt",   uc_digest_md5_crypt   },
	{ "sha1_file",   uc_digest_sha1_file   },
	{ "sha256_file", uc_digest_sha256_file },
	{ "fnv1a64_file",uc_digest_fnv1a64_file},
#ifdef HAVE_DIGEST_EXTENDED
	{ "md2",         uc_digest_md2         },
	{ "md4",         uc_digest_md4         },
	{ "sha384",      uc_digest_sha384      },
	{ "sha512",      uc_digest_sha512      },
	{ "md2_file",    uc_digest_md2_file    },
	{ "md4_file",    uc_digest_md4_file    },
	{ "sha384_file", uc_digest_sha384_file },
	{ "sha512_file", uc_digest_sha512_file },
#endif
};

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, global_fns);
}

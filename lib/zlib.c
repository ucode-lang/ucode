/*
 * Copyright (C) 2024 Thibaut VARÈNE <hacks@slashdirt.org>
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
 * # Zlib bindings
 *
 * The `zlib` module provides single-call and stream-oriented functions for interacting with zlib data.
 *
 * @module zlib
 */

#include <assert.h>
#include <errno.h>
#include <zlib.h>

#include "ucode/module.h"
#include "ucode/internal/util.h"

// https://zlib.net/zlib_how.html

/*
 * CHUNK is simply the buffer size for feeding data to and pulling data from
 * the zlib routines. Larger buffer sizes would be more efficient, especially
 * for inflate(). If the memory is available, buffers sizes on the order of
 * 128K or 256K bytes should be used.
 */
#ifndef UC_ZLIB_CHUNK
#define UC_ZLIB_CHUNK 16384
#endif

#ifdef CHUNK
#undef CHUNK
#endif
#define CHUNK (UC_ZLIB_CHUNK)

static unused const unsigned int _chunk_check = CHUNK;

static uc_resource_type_t *zstrmd_type, *zstrmi_type;

static int last_error = 0;
#define err_return(err) do { last_error = err; return NULL; } while(0)

typedef struct {
	z_stream strm;
	uc_stringbuf_t *outbuf;
	int flush;
} zstrm_t;

/* zlib init error message */
static const char * ziniterr(int ret)
{
	const char * msg;

	switch (ret) {
	case Z_ERRNO:
		msg = strerror(errno);
		break;
	case Z_STREAM_ERROR:	// can only happen for deflateInit2() by construction
		msg = "invalid compression level";
		break;
	case Z_MEM_ERROR:
		msg = "out of memory";
		break;
	case Z_VERSION_ERROR:
		msg = "zlib version mismatch!";
		break;
	default:
		msg = "unknown error";
		break;
	}

	return msg;
}

static int
def_chunks(zstrm_t * const zstrm)
{
	int ret;

	/* run deflate() on input until output buffer not full */
	do {
		if (uc_stringbuf_memset(zstrm->outbuf, -1, 0, CHUNK))
			return Z_MEM_ERROR;

		zstrm->outbuf->bpos -= CHUNK;

		zstrm->strm.avail_out = CHUNK;
		zstrm->strm.next_out = (unsigned char *)(zstrm->outbuf->buf + zstrm->outbuf->bpos);

		ret = deflate(&zstrm->strm, zstrm->flush);
		assert(ret != Z_STREAM_ERROR);

		zstrm->outbuf->bpos += CHUNK - zstrm->strm.avail_out;
	} while (zstrm->strm.avail_out == 0);
	assert(zstrm->strm.avail_in == 0);	// all input will be used

	return ret;
}

static bool
uc_zlib_def_object(uc_vm_t *const vm, uc_value_t * const obj, zstrm_t * const zstrm)
{
	int ret;
	bool eof = false, rv = false;
	uc_value_t *rfn, *rbuf = NULL;

	rfn = ucv_get(ucv_property_get(obj, "read"));

	if (!ucv_is_callable(rfn)) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				      "Input object does not implement read() method");
		goto out;
	}

	do {
		rbuf = NULL;
		uc_vm_stack_push(vm, ucv_get(obj));
		uc_vm_stack_push(vm, ucv_get(rfn));
		uc_vm_stack_push(vm, ucv_int64_new(CHUNK));

		if (uc_vm_call(vm, true, 1) != EXCEPTION_NONE)
			goto out;

		rbuf = uc_vm_stack_pop(vm);	// read output chunk

		/* we only accept strings */
		if (rbuf != NULL && ucv_type(rbuf) != UC_STRING) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					      "Input object read() method returned non-string value");
			goto out;
		}

		/* check EOF */
		eof = (rbuf == NULL || ucv_string_length(rbuf) == 0);

		zstrm->strm.next_in = (unsigned char *)ucv_string_get(rbuf);
		zstrm->strm.avail_in = ucv_string_length(rbuf);

		zstrm->flush = eof ? Z_FINISH : Z_NO_FLUSH;
		ret = def_chunks(zstrm);
		(void)ret;	// XXX make annoying compiler that ignores assert() happy

		ucv_put(rbuf);	// release rbuf
		rbuf = NULL;
	} while (!eof);	// finish compression if all of source has been read in
	assert(ret == Z_STREAM_END);	// stream will be complete

	rv = true;

out:
	ucv_put(rbuf);
	ucv_put(rfn);
	return rv;
}

static bool
uc_zlib_def_string(uc_vm_t * const vm, uc_value_t * const str, zstrm_t * const zstrm)
{
	zstrm->strm.next_in = (unsigned char *)ucv_string_get(str);
	zstrm->strm.avail_in = ucv_string_length(str);

	last_error = def_chunks(zstrm);

	return true;
}

/**
 * Compresses data in Zlib or gzip format.
 *
 * If the input argument is a plain string, it is directly compressed.
 *
 * If an array, object or resource value is given, this function will attempt to
 * invoke a `read()` method on it to read chunks of input text to incrementally
 * compress. Reading will stop if the object's `read()` method returns
 * either `null` or an empty string.
 *
 * Throws an exception on errors.
 *
 * Returns the compressed data.
 *
 * @function module:zlib#deflate
 *
 * @param {string} str_or_resource
 * The string or resource object to be compressed.
 *
 * @param {?boolean} [gzip=false]
 * Add a gzip header if true (creates a gzip-compliant output, otherwise defaults to Zlib)
 *
 * @param {?number} [level=Z_DEFAULT_COMPRESSION]
 * The compression level (0-9).
 *
 * @returns {?string}
 *
 * @example
 * // deflate content using default compression
 * const deflated = deflate(content);
 *
 * // deflate content using fastest compression
 * const deflated = deflate(content, Z_BEST_SPEED);
 */
static uc_value_t *
uc_zlib_deflate(uc_vm_t * const vm, const size_t nargs)
{
	uc_value_t *rv = NULL;
	uc_value_t *src = uc_fn_arg(0);
	uc_value_t *gzip = uc_fn_arg(1);
	uc_value_t *level = uc_fn_arg(2);
	int ret, lvl = Z_DEFAULT_COMPRESSION;
	bool success, gz = false;
	zstrm_t zstrm = {
		.strm = {
			.zalloc = Z_NULL,
			.zfree = Z_NULL,
			.opaque = Z_NULL,
		},
		.outbuf = NULL,
		.flush = Z_FINISH,
	};

	if (gzip) {
		if (ucv_type(gzip) != UC_BOOLEAN) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Passed gzip flag is not a boolean");
			goto out;
		}

		gz = (int)ucv_boolean_get(gzip);
	}

	if (level) {
		if (ucv_type(level) != UC_INTEGER) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Passed level is not a number");
			goto out;
		}

		lvl = (int)ucv_int64_get(level);
	}

	ret = deflateInit2(&zstrm.strm, lvl,
			   Z_DEFLATED,		// only allowed method
			   gz ? 15+16 : 15,	// 15 Zlib default, +16 for gzip
			   8,			// default value
			   Z_DEFAULT_STRATEGY);	// default value
	if (ret != Z_OK) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Zlib error: %s", ziniterr(ret));
		goto out;
	}

	zstrm.outbuf = ucv_stringbuf_new();

	switch (ucv_type(src)) {
	case UC_STRING:
		success = uc_zlib_def_string(vm, src, &zstrm);
		break;

	case UC_RESOURCE:
	case UC_OBJECT:
	case UC_ARRAY:
		success = uc_zlib_def_object(vm, src, &zstrm);
		break;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				      "Passed value is neither a string nor an object");
		uc_stringbuf_free(zstrm.outbuf);
		goto out;
	}

	if (!success) {
		if (vm->exception.type == EXCEPTION_NONE)	// do not clobber previous exception
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Zlib error: %s", zstrm.strm.msg);
		uc_stringbuf_free(zstrm.outbuf);
		goto out;
	}

	rv = ucv_stringbuf_finish(zstrm.outbuf);

out:
	(void)deflateEnd(&zstrm.strm);
	return rv;
}

static int
inf_chunks(zstrm_t * const zstrm)
{
	int ret;

	/* run inflate() on input until output buffer not full */
	do {
		if (uc_stringbuf_memset(zstrm->outbuf, -1, 0, CHUNK))
			return Z_MEM_ERROR;

		zstrm->outbuf->bpos -= CHUNK;

		zstrm->strm.avail_out = CHUNK;
		zstrm->strm.next_out = (unsigned char *)(zstrm->outbuf->buf + zstrm->outbuf->bpos);

		ret = inflate(&zstrm->strm, zstrm->flush);
		assert(ret != Z_STREAM_ERROR);
		switch (ret) {
		case Z_NEED_DICT:
		case Z_DATA_ERROR:
		case Z_MEM_ERROR:
			return ret;
		}

		zstrm->outbuf->bpos += CHUNK - zstrm->strm.avail_out;
	} while (zstrm->strm.avail_out == 0);

	return ret;
}

static bool
uc_zlib_inf_object(uc_vm_t *const vm, uc_value_t * const obj, zstrm_t * const zstrm)
{
	int ret = Z_STREAM_ERROR;	// error out if EOF on first loop
	bool eof = false, rv = false;
	uc_value_t *rfn, *rbuf = NULL;

	rfn = ucv_get(ucv_property_get(obj, "read"));

	if (!ucv_is_callable(rfn)) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				      "Input object does not implement read() method");
		goto out;
	}

	do {
		rbuf = NULL;
		uc_vm_stack_push(vm, ucv_get(obj));
		uc_vm_stack_push(vm, ucv_get(rfn));
		uc_vm_stack_push(vm, ucv_int64_new(CHUNK));

		if (uc_vm_call(vm, true, 1) != EXCEPTION_NONE)
			goto out;

		rbuf = uc_vm_stack_pop(vm);	// read output chunk

		/* we only accept strings */
		if (rbuf != NULL && ucv_type(rbuf) != UC_STRING) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
					      "Input object read() method returned non-string value");
			goto out;
		}

		/* check EOF */
		eof = (rbuf == NULL || ucv_string_length(rbuf) == 0);
		if (eof)
			break;

		zstrm->strm.next_in = (unsigned char *)ucv_string_get(rbuf);
		zstrm->strm.avail_in = ucv_string_length(rbuf);

		do {
			if (ret == Z_STREAM_END && inflateReset(&zstrm->strm) != Z_OK)
				goto out;

			ret = inf_chunks(zstrm);
			switch (ret) {
			case Z_NEED_DICT:
			case Z_DATA_ERROR:
			case Z_MEM_ERROR:
				goto out;
			}
		} while (ret == Z_STREAM_END && zstrm->strm.avail_in > 0);

		ucv_put(rbuf);	// release rbuf
		rbuf = NULL;
	} while (true);

	rv = (ret == Z_STREAM_END);	// data error otherwise

out:
	ucv_put(rbuf);
	ucv_put(rfn);
	return rv;
}

static bool
uc_zlib_inf_string(uc_vm_t * const vm, uc_value_t * const str, zstrm_t * const zstrm)
{
	int ret;

	zstrm->strm.next_in = (unsigned char *)ucv_string_get(str);
	zstrm->strm.avail_in = ucv_string_length(str);

	while (true) {
		ret = inf_chunks(zstrm);

		if (ret != Z_STREAM_END || zstrm->strm.avail_in == 0)
			break;

		if (inflateReset(&zstrm->strm) != Z_OK)
			break;
	}

	last_error = ret;

	return Z_STREAM_END == ret;
}

/**
 * Decompresses data in Zlib or gzip format.
 *
 * If the input argument is a plain string, it is directly decompressed.
 *
 * If an array, object or resource value is given, this function will attempt to
 * invoke a `read()` method on it to read chunks of input text to incrementally
 * decompress. Reading will stop if the object's `read()` method returns
 * either `null` or an empty string.
 *
 * Throws an exception on errors.
 *
 * Returns the decompressed data.
 *
 * @function module:zlib#inflate
 *
 * @param {string} str_or_resource
 * The string or resource object to be parsed as JSON.
 *
 * @returns {?string}
 */
static uc_value_t *
uc_zlib_inflate(uc_vm_t * const vm, const size_t nargs)
{
	uc_value_t *rv = NULL;
	uc_value_t *src = uc_fn_arg(0);
	bool success;
	int ret;
	zstrm_t zstrm = {
		.strm = {
			.zalloc = Z_NULL,
			.zfree = Z_NULL,
			.opaque = Z_NULL,
			.avail_in = 0,		// must be initialized before call to inflateInit
			.next_in = Z_NULL,	// must be initialized before call to inflateInit
		},
		.outbuf = NULL,
	};

	/* tell inflateInit2 to perform either zlib or gzip decompression: 15+32 */
	ret = inflateInit2(&zstrm.strm, 15+32);
	if (ret != Z_OK) {
		uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Zlib error: %s", ziniterr(ret));
		goto out;
	}

	zstrm.outbuf = ucv_stringbuf_new();

	switch (ucv_type(src)) {
	case UC_STRING:
		zstrm.flush = Z_FINISH;
		success = uc_zlib_inf_string(vm, src, &zstrm);
		break;

	case UC_RESOURCE:
	case UC_OBJECT:
	case UC_ARRAY:
		zstrm.flush = Z_NO_FLUSH;
		success = uc_zlib_inf_object(vm, src, &zstrm);
		break;

	default:
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				      "Passed value is neither a string nor an object");
		uc_stringbuf_free(zstrm.outbuf);
		goto out;
	}

	if (!success) {
		if (vm->exception.type == EXCEPTION_NONE)	// do not clobber previous exception
			uc_vm_raise_exception(vm, EXCEPTION_RUNTIME, "Zlib error: %s", zstrm.strm.msg);
		uc_stringbuf_free(zstrm.outbuf);
		goto out;
	}

	rv = ucv_stringbuf_finish(zstrm.outbuf);

out:
	(void)inflateEnd(&zstrm.strm);
	return rv;
}

/**
 * Represents a handle for interacting with a deflate stream initiated by deflater().
 *
 * @class module:zlib.deflate
 * @hideconstructor
 *
 * @see {@link module:zlib#deflater()}
 *
 * @example
 *
 * const zstrmd = deflater(…);
 *
 * for (let data = ...; data; data = ...) {
 * 	zstrmd.write(data, Z_PARTIAL_FLUSH);	// write uncompressed data to stream
 * 	if (foo)
	* 	let defl = zstrmd.read();	// read back compressed stream content
 * }
 *
 * // terminate the stream at the end of data input to complete a valid archive
 * zstrmd.write(last_data, Z_FINISH);
 * defl = ztrmd.read();
 *
 * zstrmd.error();
 */

/**
 * Initializes a deflate stream.
 *
 * Returns a stream handle on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:zlib#deflater
 *
 * @param {?boolean} [gzip=false]
 * Add a gzip header if true (creates a gzip-compliant output, otherwise defaults to Zlib)
 *
 * @param {?number} [level=Z_DEFAULT_COMPRESSION]
 * The compression level (0-9).
 *
 * @returns {?module:zlib.deflate}
 *
 * @example
 * // initialize a Zlib deflate stream using default compression
 * const zstrmd = deflater();
 *
 * // initialize a gzip deflate stream using fastest compression
 * const zstrmd = deflater(true, Z_BEST_SPEED);
 */
 static uc_value_t *
uc_zlib_deflater(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *gzip = uc_fn_arg(0);
	uc_value_t *level = uc_fn_arg(1);
	int ret, lvl = Z_DEFAULT_COMPRESSION;
	bool gz = false;
	zstrm_t *zstrm;

	zstrm = calloc(1, sizeof(*zstrm));
	if (!zstrm)
		err_return(ENOMEM);

	zstrm->strm.zalloc = Z_NULL;
	zstrm->strm.zfree = Z_NULL;
	zstrm->strm.opaque = Z_NULL;

	if (gzip) {
		if (ucv_type(gzip) != UC_BOOLEAN) {
			last_error = EINVAL;
			goto fail;
		}

		gz = (int)ucv_boolean_get(gzip);
	}

	if (level) {
		if (ucv_type(level) != UC_INTEGER) {
			last_error = EINVAL;
			goto fail;
		}

		lvl = (int)ucv_int64_get(level);
	}

	ret = deflateInit2(&zstrm->strm, lvl,
			   Z_DEFLATED,		// only allowed method
			   gz ? 15+16 : 15,	// 15 Zlib default, +16 for gzip
			   8,			// default value
			   Z_DEFAULT_STRATEGY);	// default value
	if (ret != Z_OK) {
		last_error = ret;
		goto fail;
	}

	return uc_resource_new(zstrmd_type, zstrm);

fail:
	free(zstrm);
	return NULL;
}

/**
 * Writes a chunk of data to the deflate stream.
 *
 * Input data must be a string, it is internally compressed by the zlib `deflate()` routine,
 * the end result is buffered according to the requested `flush` mode until read via
 * {@link module:zlib.zstrmd#read}.
 * Valid `flush`values are `Z_NO_FLUSH` (the default),
 * `Z_SYNC_FLUSH, Z_PARTIAL_FLUSH, Z_FULL_FLUSH, Z_FINISH`.
 * If `flush` is `Z_FINISH` then no more data can be written to the stream.
 * Refer to the {@link https://zlib.net/manual.html Zlib manual} for details
 * on each flush mode.
 *
 * Returns `true` on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:zlib.deflate#write
 *
 * @param {string} src
 * The string of data to deflate.
 *
 * @param {?number} [flush=Z_NO_FLUSH]
 * The zlib flush mode.
 *
 * @returns {?boolean}
 */
static uc_value_t *
uc_zlib_defwrite(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *src = uc_fn_arg(0);
	uc_value_t *flush = uc_fn_arg(1);
	zstrm_t **z = uc_fn_this("zlib.deflate");
	zstrm_t *zstrm;

	if (!z || !*z)
		err_return(EBADF);

	zstrm = *z;

	if (Z_FINISH == zstrm->flush)
		err_return(EPIPE);	// can't reuse a finished stream

	if (flush) {
		if (ucv_type(flush) != UC_INTEGER)
			err_return(EINVAL);

		zstrm->flush = (int)ucv_int64_get(flush);
		switch (zstrm->flush) {
		case Z_NO_FLUSH:
		case Z_SYNC_FLUSH:
		case Z_PARTIAL_FLUSH:
		case Z_FULL_FLUSH:
		case Z_FINISH:
			break;
		default:
			err_return(EINVAL);
		}
	}
	else
		zstrm->flush = Z_NO_FLUSH;

	/* we only accept strings */
	if (!src || ucv_type(src) != UC_STRING)
		err_return(EINVAL);

	if (!zstrm->outbuf)
		zstrm->outbuf = ucv_stringbuf_new();

	return ucv_boolean_new(uc_zlib_def_string(vm, src, zstrm));
}

/**
 * Reads a chunk of compressed data from the deflate stream.
 *
 * Returns the current content of the deflate buffer, fed through
 * {@link module:zlib.deflate#write}.
 *
 * Returns compressed chunk on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:zlib.deflate#read
 *
 * @returns {?string}
 */
static uc_value_t *
uc_zlib_defread(uc_vm_t *vm, size_t nargs)
{
	zstrm_t **z = uc_fn_this("zlib.deflate");
	zstrm_t *zstrm;
	uc_value_t *rv;

	if (!z || !*z)
		err_return(EBADF);

	zstrm = *z;

	if (!zstrm->outbuf)
		err_return(ENODATA);

	if (Z_FINISH == zstrm->flush)
		(void)deflateEnd(&zstrm->strm);

	rv = ucv_stringbuf_finish(zstrm->outbuf);
	zstrm->outbuf = NULL;	// outbuf is now unuseable
	return rv;
}

/**
 * Represents a handle for interacting with an inflate stream initiated by inflater().
 *
 * @class module:zlib.inflate
 * @hideconstructor
 *
 * @borrows module:zlib.deflate#error as module:fs.inflate#error
 *
 * @see {@link module:zlib#inflater()}
 *
 * @example
 *
 * const zstrmi = inflater();
 *
 * for (let data = ...; data; data = ...) {
 * 	zstrmi.write(data, Z_SYNC_FLUSH);	// write compressed data to stream
 * 	if (foo)
	* 	let defl = zstrmi.read();	// read back decompressed stream content
 * }
 *
 * zstrmi.error();
 */

/**
 * Initializes an inflate stream. Can process either Zlib or gzip data.
 *
 * Returns a stream handle on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:zlib#inflater
 *
 * @returns {?module:zlib.inflate}
 *
 * @example
 * // initialize an inflate stream
 * const zstrmi = inflater();
 */
 static uc_value_t *
uc_zlib_inflater(uc_vm_t *vm, size_t nargs)
{
	int ret;
	zstrm_t *zstrm;

	zstrm = calloc(1, sizeof(*zstrm));
	if (!zstrm)
		err_return(ENOMEM);

	zstrm->strm.zalloc = Z_NULL;
	zstrm->strm.zfree = Z_NULL;
	zstrm->strm.opaque = Z_NULL;
	zstrm->strm.avail_in = 0;
	zstrm->strm.next_in = Z_NULL;

	/* tell inflateInit2 to perform either zlib or gzip decompression: 15+32 */
	ret = inflateInit2(&zstrm->strm, 15+32);
	if (ret != Z_OK) {
		last_error = ret;
		goto fail;
	}

	return uc_resource_new(zstrmi_type, zstrm);

fail:
	free(zstrm);
	return NULL;
}

/**
 * Writes a chunk of data to the inflate stream.
 *
 * Input data must be a string, it is internally decompressed by the zlib `inflate()` routine,
 * the end result is buffered according to the requested `flush` mode until read via
 * {@link module:zlib.inflate#read}.
 * Valid `flush` values are `Z_NO_FLUSH` (the default), `Z_SYNC_FLUSH, Z_FINISH`.
 * If `flush` is `Z_FINISH` then no more data can be written to the stream.
 * Refer to the {@link https://zlib.net/manual.html Zlib manual} for details
 * on each flush mode.
 *
 * Returns `true` on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:zlib.inflate#write
 *
 * @param {string} src
 * The string of data to inflate.
 *
 * @param {?number} [flush=Z_NO_FLUSH]
 * The zlib flush mode.
 *
 * @returns {?boolean}
 */
static uc_value_t *
uc_zlib_infwrite(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *src = uc_fn_arg(0);
	uc_value_t *flush = uc_fn_arg(1);
	zstrm_t **z = uc_fn_this("zlib.inflate");
	zstrm_t *zstrm;

	if (!z || !*z)
		err_return(EBADF);

	zstrm = *z;

	if (Z_FINISH == zstrm->flush)
		err_return(EPIPE);	// can't reuse a finished stream

	if (flush) {
		if (ucv_type(flush) != UC_INTEGER)
			err_return(EINVAL);

		zstrm->flush = (int)ucv_int64_get(flush);
		switch (zstrm->flush) {
		case Z_NO_FLUSH:
		case Z_SYNC_FLUSH:
		case Z_FINISH:
			break;
		default:
			err_return(EINVAL);
		}
	}
	else
		zstrm->flush = Z_NO_FLUSH;

	/* we only accept strings */
	if (!src || ucv_type(src) != UC_STRING)
		err_return(EINVAL);

	if (!zstrm->outbuf)
		zstrm->outbuf = ucv_stringbuf_new();

	return ucv_boolean_new(uc_zlib_inf_string(vm, src, zstrm));
}

/**
 * Reads a chunk of decompressed data from the inflate stream.
 *
 * Returns the current content of the inflate buffer, fed through
 * {@link module:zlib.inflate#write}.
 *
 * Returns decompressed chunk on success.
 *
 * Returns `null` if an error occurred.
 *
 * @function module:zlib.inflate#read
 *
 * @returns {?string}
 */
static uc_value_t *
uc_zlib_infread(uc_vm_t *vm, size_t nargs)
{
	zstrm_t **z = uc_fn_this("zlib.inflate");
	zstrm_t *zstrm;
	uc_value_t *rv;

	if (!z || !*z)
		err_return(EBADF);

	zstrm = *z;

	if (!zstrm->outbuf)
		err_return(ENODATA);

	if (Z_FINISH == zstrm->flush)
		(void)inflateEnd(&zstrm->strm);

	rv = ucv_stringbuf_finish(zstrm->outbuf);
	zstrm->outbuf = NULL;	// outbuf is now unuseable
	return rv;
}

/**
 * Queries error information.
 *
 * Returns a string containing a description of the last occurred error or
 * `null` if there is no error information.
 *
 * @function module:zlib.deflate#error
 *
 *
 * @returns {?string}
 */
static uc_value_t *
uc_zlib_error(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *errmsg;

	if (!last_error)
		return NULL;

	// negative last_error only happens for zlib init returns
	errmsg = ucv_string_new(last_error < 0 ? ziniterr(last_error) : strerror(last_error));
	last_error = 0;
	return errmsg;
}

static const uc_function_list_t strmd_fns[] = {
	{ "write",	uc_zlib_defwrite },
	{ "read",	uc_zlib_defread },
	{ "error",	uc_zlib_error },
};

static const uc_function_list_t strmi_fns[] = {
	{ "write",	uc_zlib_infwrite },
	{ "read",	uc_zlib_infread },
	{ "error",	uc_zlib_error },
};

static const uc_function_list_t global_fns[] = {
	{ "deflate",	uc_zlib_deflate },
	{ "inflate",	uc_zlib_inflate },
	{ "deflater",	uc_zlib_deflater },
	{ "inflater",	uc_zlib_inflater },
};

static void destroy_zstrmd(void *z)
{
	zstrm_t *zstrm = z;

	if (zstrm) {
		(void)deflateEnd(&zstrm->strm);
		uc_stringbuf_free(zstrm->outbuf);
		free(zstrm);
	}
}

static void destroy_zstrmi(void *z)
{
	zstrm_t *zstrm = z;

	if (zstrm) {
		(void)inflateEnd(&zstrm->strm);
		uc_stringbuf_free(zstrm->outbuf);
		free(zstrm);
	}
}

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, global_fns);

	zstrmd_type = uc_type_declare(vm, "zlib.deflate", strmd_fns, destroy_zstrmd);
	zstrmi_type = uc_type_declare(vm, "zlib.inflate", strmi_fns, destroy_zstrmi);

#define ADD_CONST(x) ucv_object_add(scope, #x, ucv_int64_new(x))

	/**
	 * @typedef
	 * @name Compression levels
	 * @description Constants representing predefined compression levels.
	 * @property {number} Z_NO_COMPRESSION.
	 * @property {number} Z_BEST_SPEED.
	 * @property {number} Z_BEST_COMPRESSION.
	 * @property {number} Z_DEFAULT_COMPRESSION - default compromise between speed and compression (currently equivalent to level 6).
	 */
	ADD_CONST(Z_NO_COMPRESSION);
	ADD_CONST(Z_BEST_SPEED);
	ADD_CONST(Z_BEST_COMPRESSION);
	ADD_CONST(Z_DEFAULT_COMPRESSION);

	/**
	 * @typedef
	 * @name flush options
	 * @description Constants representing flush options.
	 * @property {number} Z_NO_FLUSH.
	 * @property {number} Z_PARTIAL_FLUSH.
	 * @property {number} Z_SYNC_FLUSH.
	 * @property {number} Z_FULL_FLUSH.
	 * @property {number} Z_FINISH.
	 */
	ADD_CONST(Z_NO_FLUSH);
	ADD_CONST(Z_PARTIAL_FLUSH);
	ADD_CONST(Z_SYNC_FLUSH);
	ADD_CONST(Z_FULL_FLUSH);
	ADD_CONST(Z_FINISH);
}

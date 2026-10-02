/*
 * Copyright (C) 2026 ucode contributors
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
 * # WebSocket Module
 *
 * The `websocket` module provides functions for interacting with WebSocket
 * (RFC 6455) servers using an event driven, uloop based API.
 *
 * Functions can be individually imported and directly accessed using the
 * {@link https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/import#named_import named import}
 * syntax:
 *
 *   ```javascript
 *   import { connect } from 'websocket';
 *   import * as uloop from 'uloop';
 *
 *   uloop.init();
 *
 *   let ws = connect('ws://10.0.0.1:8080/path');
 *
 *   ws.on('message', (ws, data, is_text) => print(data));
 *
 *   ws.on('open', (ws) => ws.send('hello'));
 *
 *   ws.on('close', (ws, code, reason) => print(`closed ${code} ${reason}\n`));
 *
 *   ws.on('error', (ws, error) => print(`error: ${error}\n`));
 *
 *   uloop.run();
 *   ```
 *
 * Alternatively, the module namespace can be imported
 * using a wildcard import statement:
 *
 *   ```javascript
 *   import * as websocket from 'websocket';
 *
 *   let ws = websocket.connect('ws://10.0.0.1:8080/path');
 *   ```
 *
 * Additionally, the websocket module namespace may also be imported by
 * invoking the `ucode` interpreter with the `-lwebsocket` switch.
 *
 * @module websocket
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <libubox/uloop.h>
#include <wslay/wslay.h>

#include "ucode/module.h"

#define ok_return(expr) do { last_error = 0; return (expr); } while(0)
#define err_return(err) do { last_error = err; return NULL; } while(0)

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_HS_BUFSIZE 2048
#define WS_DEFAULT_TIMEOUT 15000
#define WS_DEFAULT_MAX_MSG (256 * 1024)
#define WS_MIN_TIMEOUT 100
#define WS_CLOSE_TIMEOUT 5000

#ifndef MSG_MORE
#define MSG_MORE 0
#endif

#if defined(__APPLE__)
# define SOCK_NONBLOCK (1 << 16)
# define SOCK_CLOEXEC (1 << 17)
#endif

static int last_error = 0;

enum {
	WS_STATE_CONNECTING,
	WS_STATE_HANDSHAKE_WRITE,
	WS_STATE_HANDSHAKE_READ,
	WS_STATE_OPEN,
	WS_STATE_CLOSING,
	WS_STATE_CLOSED,
};

typedef struct {
	uc_vm_t *vm;
	uc_value_t *obj;

	struct uloop_fd ufd;
	struct uloop_timeout timeout;

	wslay_event_context_ptr ctx;

	int state;
	int port;
	int io_errno;
	bool eof;
	int dispatching;
	bool need_flush;
	bool in_recv;

	char *host;
	char *path;

	char *hs_req;
	size_t hs_req_len;
	size_t hs_sent;

	char hs_buf[WS_HS_BUFSIZE];
	size_t hs_len;
	size_t hs_pendoff;
	size_t hs_pendlen;
	char hs_accept[32];

	uint64_t max_msg_len;

	int close_code;
	char close_reason[124];
} uc_websocket_t;

static void ws_flush(uc_websocket_t *ws);
static void ws_readable(uc_websocket_t *ws);

/* ---------------------------------------------------------------------- */
/* SHA-1 (RFC 3174) and base64 encoding for the opening handshake        */
/* ---------------------------------------------------------------------- */

typedef struct {
	uint32_t state[5];
	uint64_t count;
	uint8_t buffer[64];
} sha1_ctx_t;

#define SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void
sha1_init(sha1_ctx_t *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->count = 0;
}

static void
sha1_transform(sha1_ctx_t *ctx, const uint8_t *p)
{
	uint32_t w[80], a, b, c, d, e, t;
	size_t i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];

	for (i = 16; i < 80; i++)
		w[i] = SHA1_ROTL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];

	for (i = 0; i < 80; i++) {
		if (i < 20)
			t = ((b & c) | ((~b) & d)) + 0x5A827999;
		else if (i < 40)
			t = (b ^ c ^ d) + 0x6ED9EBA1;
		else if (i < 60)
			t = ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDC;
		else
			t = (b ^ c ^ d) + 0xCA62C1D6;

		t += SHA1_ROTL(a, 5) + e + w[i];
		e = d;
		d = c;
		c = SHA1_ROTL(b, 30);
		b = a;
		a = t;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
}

static void
sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
	size_t i = 0, n;

	if (ctx->count % 64) {
		n = 64 - (ctx->count % 64);

		if (n > len)
			n = len;

		memcpy(ctx->buffer + (ctx->count % 64), data, n);
		ctx->count += n;
		i = n;

		if (ctx->count % 64)
			return;

		sha1_transform(ctx, ctx->buffer);
	}

	for (; i + 64 <= len; i += 64) {
		sha1_transform(ctx, data + i);
		ctx->count += 64;
	}

	n = len - i;

	if (n) {
		memcpy(ctx->buffer, data + i, n);
		ctx->count += n;
	}
}

static void
sha1_final(sha1_ctx_t *ctx, uint8_t digest[20])
{
	static const uint8_t pad[64] = { 0x80 };
	uint64_t bits = ctx->count * 8;
	uint8_t tail[8];
	size_t i;

	for (i = 0; i < 8; i++)
		tail[i] = (uint8_t)(bits >> (56 - 8 * i));

	sha1_update(ctx, pad, 1 + ((119 - ctx->count % 64) % 64));
	sha1_update(ctx, tail, 8);

	for (i = 0; i < 5; i++) {
		digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

static const char b64tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t
b64_encode(const uint8_t *in, size_t inlen, char *out, size_t outsize)
{
	size_t i, j = 0;

	for (i = 0; i + 3 <= inlen; i += 3) {
		uint32_t v = ((uint32_t)in[i] << 16) |
		             ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];

		out[j++] = b64tab[(v >> 18) & 63];
		out[j++] = b64tab[(v >> 12) & 63];
		out[j++] = b64tab[(v >> 6) & 63];
		out[j++] = b64tab[v & 63];
	}

	if (i < inlen) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < inlen)
			v |= (uint32_t)in[i + 1] << 8;

		out[j++] = b64tab[(v >> 18) & 63];
		out[j++] = b64tab[(v >> 12) & 63];
		out[j++] = (i + 1 < inlen) ? b64tab[(v >> 6) & 63] : '=';
		out[j++] = '=';
		i += 3;
	}

	(void)i;
	out[j] = '\0';

	return j;
}

static int
ws_urandom(uint8_t *buf, size_t len)
{
	static int fd = -1;
	ssize_t n;

	if (fd == -1) {
		fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

		if (fd == -1)
			return -1;
	}

	do {
		n = read(fd, buf, len);
	} while (n < 0 && errno == EINTR);

	return (n == (ssize_t)len) ? 0 : -1;
}

/* ---------------------------------------------------------------------- */
/* URL parsing                                                            */
/* ---------------------------------------------------------------------- */

static bool
ws_parse_url(const char *url, char **host, int *port, char **path, bool *tls)
{
	const char *p = url, *host_start, *host_end, *port_start = NULL;
	const char *bracket_close = NULL;
	char hostbuf[256], *end;
	size_t hostlen;
	long n;

	if (!strncasecmp(p, "ws://", 5)) {
		host_start = p + 5;
		*tls = false;
	}
	else if (!strncasecmp(p, "wss://", 6)) {
		host_start = p + 6;
		*tls = true;
	}
	else {
		return false;
	}

	if (*host_start == '[') {
		bracket_close = strchr(host_start, ']');

		if (!bracket_close || bracket_close == host_start + 1)
			return false;

		host_end = bracket_close + 1;

		if (*host_end == ':') {
			port_start = host_end + 1;
		}
		else if (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#') {
			return false;
		}

		/* store the IPv6 literal without brackets for getaddrinfo() */
		hostlen = (size_t)(bracket_close - host_start - 1);

		if (hostlen >= sizeof(hostbuf))
			return false;

		memcpy(hostbuf, host_start + 1, hostlen);
		hostbuf[hostlen] = '\0';
	}
	else {
		for (host_end = host_start; *host_end; host_end++) {
			if (*host_end == '/' || *host_end == '?' || *host_end == '#')
				break;

			if (*host_end == '@')
				return false;

			if (*host_end == ':') {
				if (port_start)
					return false;

				port_start = host_end + 1;
			}
		}

		hostlen = (size_t)((port_start ? port_start - 1 : host_end) - host_start);

		if (!hostlen || hostlen >= sizeof(hostbuf))
			return false;

		memcpy(hostbuf, host_start, hostlen);
		hostbuf[hostlen] = '\0';
	}

	if (port_start) {
		n = strtol(port_start, &end, 10);

		if (!*port_start || n < 1 || n > 65535)
			return false;

		if (!bracket_close && end != host_end)
			return false;

		if (bracket_close) {
			if (*end && *end != '/' && *end != '?' && *end != '#')
				return false;

			host_end = end;
		}

		*port = (int)n;
	}
	else {
		*port = *tls ? 443 : 80;
	}

	*host = strdup(hostbuf);

	if (*host_end == '/')
		*path = strdup(host_end);
	else if (*host_end)
		*path = NULL; /* ?query or #fragment: prepend slash below */
	else
		*path = strdup("/");

	if (!*path && *host_end) {
		*path = malloc(strlen(host_end) + 2);

		if (*path)
			snprintf(*path, strlen(host_end) + 2, "/%s", host_end);
	}

	return (*host && *path);
}

/* ---------------------------------------------------------------------- */
/* Event dispatching                                                      */
/* ---------------------------------------------------------------------- */

static bool
ws_vm_call(uc_vm_t *vm, bool mcall, size_t nargs)
{
	uc_value_t *exh, *val;

	if (uc_vm_call(vm, mcall, nargs) == EXCEPTION_NONE)
		return true;

	exh = uc_vm_registry_get(vm, "uloop.ex_handler");

	if (!ucv_is_callable(exh))
		goto error;

	val = uc_vm_exception_object(vm);
	uc_vm_stack_push(vm, ucv_get(exh));
	uc_vm_stack_push(vm, val);

	if (uc_vm_call(vm, false, 1) != EXCEPTION_NONE)
		goto error;

	ucv_put(uc_vm_stack_pop(vm));

	return false;

error:
	uloop_end();

	return false;
}

static void
ws_invoke(uc_websocket_t *ws, size_t slot, uc_value_t **args, size_t nargs)
{
	uc_value_t *fn;
	size_t i;

	if (!ws->obj || ws->state == WS_STATE_CLOSED)
		return;

	fn = ucv_resource_value_get(ws->obj, slot);

	if (!ucv_is_callable(fn))
		return;

	uc_vm_stack_push(ws->vm, ucv_get(ws->obj));
	uc_vm_stack_push(ws->vm, ucv_get(fn));
	uc_vm_stack_push(ws->vm, ucv_get(ws->obj));

	for (i = 0; i < nargs; i++)
		uc_vm_stack_push(ws->vm, ucv_get(args[i]));

	ws->dispatching++;

	if (ws_vm_call(ws->vm, true, nargs + 1))
		ucv_put(uc_vm_stack_pop(ws->vm));

	ws->dispatching--;

	/* defer wslay_event_send() until we are completely outside of the
	 * wslay_event_recv() call stack: the epilogue below still runs while
	 * wslay_event_recv() is active, so an ongoing receive is another
	 * reason to keep the flush pending */
	if (!ws->dispatching && !ws->in_recv && ws->need_flush) {
		ws->need_flush = false;

		if (ws->state == WS_STATE_OPEN || ws->state == WS_STATE_CLOSING)
			ws_flush(ws);
	}
}

static void
ws_emit_error(uc_websocket_t *ws, const char *msg)
{
	uc_value_t *args[1] = { ucv_string_new(msg) };

	ws_invoke(ws, 3, args, 1);
	ucv_put(args[0]);
}

static void
ws_emit_close(uc_websocket_t *ws, int code, const char *reason)
{
	uc_value_t *args[2];

	args[0] = ucv_int64_new(code);
	args[1] = ucv_string_new(reason ? reason : "");

	ws_invoke(ws, 2, args, 2);
	ucv_put(args[0]);
	ucv_put(args[1]);
}

/* ---------------------------------------------------------------------- */
/* Teardown                                                               */
/* ---------------------------------------------------------------------- */

static void
ws_teardown(uc_websocket_t *ws)
{
	uc_value_t *obj;
	uc_resource_ext_t *ext;
	size_t i;

	if (!ws || ws->state == WS_STATE_CLOSED)
		return;

	ws->state = WS_STATE_CLOSED;

	uloop_fd_delete(&ws->ufd);
	uloop_timeout_cancel(&ws->timeout);

	if (ws->ctx) {
		wslay_event_context_free(ws->ctx);
		ws->ctx = NULL;
	}

	if (ws->ufd.fd >= 0) {
		close(ws->ufd.fd);
		ws->ufd.fd = -1;
	}

	free(ws->hs_req);
	free(ws->host);
	free(ws->path);

	obj = ws->obj;
	ws->obj = NULL;

	ext = (uc_resource_ext_t *)obj;

	for (i = 0; i < ext->uvcount; i++)
		ucv_resource_value_set(obj, i, NULL);

	ucv_resource_persistent_set(obj, false);
	ucv_put(obj);
}

/* ---------------------------------------------------------------------- */
/* wslay callbacks                                                        */
/* ---------------------------------------------------------------------- */

static ssize_t
ws_wslay_recv(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
              int flags, void *user_data)
{
	uc_websocket_t *ws = user_data;
	ssize_t n;
	size_t chunk;

	(void)flags;

	/* serve bytes that arrived pipelined with the handshake response
	 * before reading new data from the socket */
	if (ws->hs_pendlen) {
		chunk = (ws->hs_pendlen < len) ? ws->hs_pendlen : len;

		memcpy(buf, ws->hs_buf + ws->hs_pendoff, chunk);

		ws->hs_pendoff += chunk;
		ws->hs_pendlen -= chunk;

		return (ssize_t)chunk;
	}

	do {
		n = read(ws->ufd.fd, buf, len);
	} while (n < 0 && errno == EINTR);

	if (n > 0)
		return n;

	if (n == 0)
		ws->eof = true;
	else if (errno != EAGAIN && errno != EWOULDBLOCK)
		ws->io_errno = errno;

	wslay_event_set_error(ctx,
		(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			? WSLAY_ERR_WOULDBLOCK : WSLAY_ERR_CALLBACK_FAILURE);

	return -1;
}

static ssize_t
ws_wslay_send(wslay_event_context_ptr ctx, const uint8_t *buf, size_t len,
              int flags, void *user_data)
{
	uc_websocket_t *ws = user_data;
	ssize_t n;

	(void)ctx;

	do {
		n = send(ws->ufd.fd, buf, len,
			(flags & WSLAY_MSG_MORE) ? MSG_MORE : 0);
	} while (n < 0 && errno == EINTR);

	if (n >= 0)
		return n;

	if (errno != EAGAIN && errno != EWOULDBLOCK)
		ws->io_errno = errno;

	wslay_event_set_error(ctx,
		(errno == EAGAIN || errno == EWOULDBLOCK)
			? WSLAY_ERR_WOULDBLOCK : WSLAY_ERR_CALLBACK_FAILURE);

	return -1;
}

static int
ws_wslay_genmask(wslay_event_context_ptr ctx, uint8_t *buf, size_t len,
                 void *user_data)
{
	(void)ctx;
	(void)user_data;

	return ws_urandom(buf, len);
}

static void
ws_wslay_on_msg(wslay_event_context_ptr ctx,
                const struct wslay_event_on_msg_recv_arg *arg, void *user_data)
{
	uc_websocket_t *ws = user_data;
	uc_value_t *args[2];

	(void)ctx;

	switch (arg->opcode) {
	case WSLAY_CONNECTION_CLOSE:
		/* reply close frame is queued automatically by wslay_event_recv() */
		ws->close_code = (int)arg->status_code;

		if (arg->msg_length > 2) {
			size_t n = arg->msg_length - 2;

			if (n > sizeof(ws->close_reason) - 1)
				n = sizeof(ws->close_reason) - 1;

			memcpy(ws->close_reason, arg->msg + 2, n);
			ws->close_reason[n] = '\0';
		}

		break;

	case WSLAY_TEXT_FRAME:
	case WSLAY_BINARY_FRAME:
		args[0] = ucv_string_new_length((const char *)arg->msg, arg->msg_length);
		args[1] = ucv_boolean_new(arg->opcode == WSLAY_TEXT_FRAME);

		ws_invoke(ws, 1, args, 2);

		ucv_put(args[0]);
		ucv_put(args[1]);
		break;

	default:
		/* ping frames are answered automatically by wslay_event_recv() */
		break;
	}
}

static const struct wslay_event_callbacks ws_wslay_callbacks = {
	.recv_callback = ws_wslay_recv,
	.send_callback = ws_wslay_send,
	.genmask_callback = ws_wslay_genmask,
	.on_frame_recv_start_callback = NULL,
	.on_frame_recv_chunk_callback = NULL,
	.on_frame_recv_end_callback = NULL,
	.on_msg_recv_callback = ws_wslay_on_msg,
};

/* ---------------------------------------------------------------------- */
/* Connection lifecycle                                                   */
/* ---------------------------------------------------------------------- */

static void
ws_update_poll(uc_websocket_t *ws)
{
	unsigned int flags = 0;

	if (ws->state == WS_STATE_CONNECTING || ws->state == WS_STATE_HANDSHAKE_WRITE) {
		flags = ULOOP_WRITE;
	}
	else if (ws->state == WS_STATE_HANDSHAKE_READ) {
		flags = ULOOP_READ;
	}
	else if (ws->ctx) {
		if (wslay_event_get_read_enabled(ws->ctx))
			flags |= ULOOP_READ;

		if (wslay_event_want_write(ws->ctx))
			flags |= ULOOP_WRITE;
	}

	if (flags)
		uloop_fd_add(&ws->ufd, flags);
	else
		uloop_fd_delete(&ws->ufd);
}

static void
ws_check_lifecycle(uc_websocket_t *ws)
{
	if (ws->state != WS_STATE_OPEN && ws->state != WS_STATE_CLOSING)
		return;

	/* entering the closing phase: either the peer started the close
	 * handshake, or wslay disabled reads after queueing an automatic
	 * close reply (oversized message, protocol error, ...) */
	if (ws->state == WS_STATE_OPEN && ws->ctx &&
	    (!wslay_event_get_read_enabled(ws->ctx) ||
	     wslay_event_get_close_received(ws->ctx))) {
		ws->state = WS_STATE_CLOSING;
		uloop_timeout_set(&ws->timeout, WS_CLOSE_TIMEOUT);
	}

	if ((ws->eof || ws->io_errno) && !wslay_event_get_close_received(ws->ctx)) {
		ws_emit_error(ws, ws->io_errno
			? strerror(ws->io_errno) : "connection closed unexpectedly");
		ws_emit_close(ws, 1006, "");
		ws_teardown(ws);
		return;
	}

	/* once our close frame is sent and no further reads are possible
	 * (wslay disabled reads after a protocol violation or oversized
	 * message), the close handshake cannot progress any further */
	if (ws->ctx && wslay_event_get_close_sent(ws->ctx) &&
	    !wslay_event_get_read_enabled(ws->ctx)) {
		int code = ws->close_code;
		const char *reason = ws->close_reason;

		if (!wslay_event_get_close_received(ws->ctx)) {
			code = (int)wslay_event_get_status_code_sent(ws->ctx);

			if (code < 1000 || code > 4999)
				code = 1006;

			reason = "";
		}

		ws_emit_close(ws, code, reason);
		ws_teardown(ws);
		return;
	}

	if (wslay_event_get_close_received(ws->ctx) && wslay_event_get_close_sent(ws->ctx)) {
		ws_emit_close(ws, ws->close_code, ws->close_reason);
		ws_teardown(ws);
	}
}

static void
ws_flush(uc_websocket_t *ws)
{
	int rv;

	if (!ws->ctx)
		return;

	rv = wslay_event_send(ws->ctx);

	if (rv < 0) {
		ws_emit_error(ws, rv == WSLAY_ERR_NOMEM
			? "out of memory" : "send failure");
		ws_emit_close(ws, 1006, "");
		ws_teardown(ws);
		return;
	}

	if (ws->state == WS_STATE_OPEN || ws->state == WS_STATE_CLOSING)
		ws_update_poll(ws);
}

static bool
ws_validate_handshake(uc_websocket_t *ws)
{
	const char *eol, *end, *p = ws->hs_buf;
	bool have_upgrade = false, have_connection = false, have_accept = false;
	char line[256];
	size_t len;

	/* data may be pipelined behind the handshake response, so split
	 * at the header terminator instead of requiring it at the end */
	end = strstr(ws->hs_buf, "\r\n\r\n");

	if (!end || ws->hs_len < 4 ||
	    (size_t)(end + 4 - ws->hs_buf) > ws->hs_len)
		return false;

	ws->hs_pendoff = (size_t)(end + 4 - ws->hs_buf);
	ws->hs_pendlen = ws->hs_len - ws->hs_pendoff;

	if (strncmp(p, "HTTP/1.1 101", 12) && strncmp(p, "HTTP/1.0 101", 12))
		return false;

	while (*p && p < end) {
		eol = strstr(p, "\r\n");

		if (!eol || eol > end)
			break;

		len = (size_t)(eol - p);

		if (len >= sizeof(line))
			return false;

		memcpy(line, p, len);
		line[len] = '\0';

		if (!strncasecmp(line, "Upgrade:", 8)) {
			if (!strcasestr(line, "websocket"))
				return false;

			have_upgrade = true;
		}
		else if (!strncasecmp(line, "Connection:", 11)) {
			if (!strcasestr(line, "upgrade"))
				return false;

			have_connection = true;
		}
		else if (!strncasecmp(line, "Sec-WebSocket-Accept:", 21)) {
			p = line + 21;

			while (*p == ' ' || *p == '\t')
				p++;

			if (strncmp(p, ws->hs_accept, strlen(ws->hs_accept)))
				return false;

			have_accept = true;
		}

		p = eol + 2;
	}

	return have_upgrade && have_connection && have_accept;
}

static void
ws_established(uc_websocket_t *ws)
{
	if (wslay_event_context_client_init(&ws->ctx, &ws_wslay_callbacks, ws)) {
		ws_emit_error(ws, "out of memory");
		ws_teardown(ws);
		return;
	}

	wslay_event_config_set_max_recv_msg_length(ws->ctx, ws->max_msg_len);

	ws->state = WS_STATE_OPEN;

	uloop_timeout_cancel(&ws->timeout);

	ws_invoke(ws, 0, NULL, 0);

	if (ws->state == WS_STATE_OPEN || ws->state == WS_STATE_CLOSING)
		ws_update_poll(ws);

	/* data pipelined behind the handshake response is already waiting
	 * in hs_buf; no further read event would ever fire, so drain it
	 * through wslay right away */
	if (ws->hs_pendlen && ws->state == WS_STATE_OPEN)
		ws_readable(ws);
}

static void
ws_handshake_read(uc_websocket_t *ws)
{
	ssize_t n;

	do {
		n = read(ws->ufd.fd, ws->hs_buf + ws->hs_len,
			sizeof(ws->hs_buf) - ws->hs_len - 1);
	} while (n < 0 && errno == EINTR);

	if (n == 0) {
		ws_emit_error(ws, "connection closed during handshake");
		ws_teardown(ws);
		return;
	}

	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			ws_emit_error(ws, strerror(errno));
			ws_teardown(ws);
		}

		return;
	}

	ws->hs_len += n;
	ws->hs_buf[ws->hs_len] = '\0';

	if (!strstr(ws->hs_buf, "\r\n\r\n")) {
		if (ws->hs_len + 1 >= sizeof(ws->hs_buf)) {
			ws_emit_error(ws, "handshake response too large");
			ws_teardown(ws);
		}

		return;
	}

	if (!ws_validate_handshake(ws)) {
		ws_emit_error(ws, "invalid handshake response");
		ws_teardown(ws);
		return;
	}

	ws_established(ws);
}

static void
ws_handshake_write(uc_websocket_t *ws)
{
	ssize_t n;

	while (ws->hs_sent < ws->hs_req_len) {
		do {
			n = write(ws->ufd.fd, ws->hs_req + ws->hs_sent,
				ws->hs_req_len - ws->hs_sent);
		} while (n < 0 && errno == EINTR);

		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				ws_emit_error(ws, strerror(errno));
				ws_teardown(ws);
			}

			return;
		}

		ws->hs_sent += n;
	}

	ws->state = WS_STATE_HANDSHAKE_READ;

	ws_update_poll(ws);
}

static void
ws_connected(uc_websocket_t *ws)
{
	int err = 0;
	socklen_t errlen = sizeof(err);

	if (getsockopt(ws->ufd.fd, SOL_SOCKET, SO_ERROR, &err, &errlen) || err) {
		ws_emit_error(ws, err ? strerror(err) : "connection failed");
		ws_teardown(ws);
		return;
	}

	ws->state = WS_STATE_HANDSHAKE_WRITE;

	ws_handshake_write(ws);
}

static void
ws_readable(uc_websocket_t *ws)
{
	int rv;

	if (ws->state == WS_STATE_HANDSHAKE_READ) {
		ws_handshake_read(ws);
		return;
	}

	ws->in_recv = true;
	rv = wslay_event_recv(ws->ctx);
	ws->in_recv = false;

	if (rv < 0 && !ws->eof && !ws->io_errno) {
		ws_emit_error(ws, rv == WSLAY_ERR_NOMEM
			? "out of memory" : "receive failure");
		ws_teardown(ws);
		return;
	}

	/* flush data queued from within message callbacks now that the
	 * receive operation completed; a fatal send error tears the
	 * connection down safely here */
	if (ws->need_flush && !ws->dispatching) {
		ws->need_flush = false;

		if (ws->state == WS_STATE_OPEN || ws->state == WS_STATE_CLOSING)
			ws_flush(ws);
	}

	ws_check_lifecycle(ws);

	if (ws->state == WS_STATE_OPEN || ws->state == WS_STATE_CLOSING)
		ws_update_poll(ws);
}

static void
ws_ufd_cb(struct uloop_fd *fd, unsigned int events)
{
	uc_websocket_t *ws = container_of(fd, uc_websocket_t, ufd);

	if (ws->state == WS_STATE_CLOSED)
		return;

	if (events & ULOOP_WRITE) {
		switch (ws->state) {
		case WS_STATE_CONNECTING:
			ws_connected(ws);
			return;

		case WS_STATE_HANDSHAKE_WRITE:
			ws_handshake_write(ws);
			return;

		default:
			ws_flush(ws);
			ws_check_lifecycle(ws);
			return;
		}
	}

	if (events & ULOOP_READ)
		ws_readable(ws);
}

static void
ws_timeout_cb(struct uloop_timeout *timeout)
{
	uc_websocket_t *ws = container_of(timeout, uc_websocket_t, timeout);

	if (ws->state == WS_STATE_CLOSING && ws->ctx) {
		ws_emit_error(ws, "close handshake timeout");
		ws_emit_close(ws, wslay_event_get_close_received(ws->ctx)
			? ws->close_code : 1006,
			wslay_event_get_close_received(ws->ctx)
				? ws->close_reason : "");
	}
	else {
		ws_emit_error(ws, "connection or handshake timeout");
		ws_emit_close(ws, 1006, "");
	}

	ws_teardown(ws);
}

/* ---------------------------------------------------------------------- */
/* Handshake request generation                                           */
/* ---------------------------------------------------------------------- */

static char *
ws_build_request(uc_vm_t *vm, uc_websocket_t *ws, uc_value_t *headers)
{
	char req[WS_HS_BUFSIZE], key[32], accept_src[96], *hvs;
	uint8_t nonce[16], digest[20];
	sha1_ctx_t sha1;
	size_t len, n;
	bool ok = true;

	if (ws_urandom(nonce, sizeof(nonce)))
		return NULL;

	b64_encode(nonce, sizeof(nonce), key, sizeof(key));

	snprintf(accept_src, sizeof(accept_src), "%s%s", key, WS_GUID);

	sha1_init(&sha1);
	sha1_update(&sha1, (const uint8_t *)accept_src, strlen(accept_src));
	sha1_final(&sha1, digest);

	b64_encode(digest, sizeof(digest), ws->hs_accept, sizeof(ws->hs_accept));

	if (strchr(ws->host, ':')) {
		/* IPv6 literal: brackets are required in the Host header */
		if (ws->port == 80)
			len = snprintf(req, sizeof(req),
				"GET %s HTTP/1.1\r\n"
				"Host: [%s]\r\n"
				"Upgrade: websocket\r\n"
				"Connection: Upgrade\r\n"
				"Sec-WebSocket-Key: %s\r\n"
				"Sec-WebSocket-Version: 13\r\n",
				ws->path, ws->host, key);
		else
			len = snprintf(req, sizeof(req),
				"GET %s HTTP/1.1\r\n"
				"Host: [%s]:%d\r\n"
				"Upgrade: websocket\r\n"
				"Connection: Upgrade\r\n"
				"Sec-WebSocket-Key: %s\r\n"
				"Sec-WebSocket-Version: 13\r\n",
				ws->path, ws->host, ws->port, key);
	}
	else if (ws->port == 80)
		len = snprintf(req, sizeof(req),
			"GET %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Key: %s\r\n"
			"Sec-WebSocket-Version: 13\r\n",
			ws->path, ws->host, key);
	else
		len = snprintf(req, sizeof(req),
			"GET %s HTTP/1.1\r\n"
			"Host: %s:%d\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Key: %s\r\n"
			"Sec-WebSocket-Version: 13\r\n",
			ws->path, ws->host, ws->port, key);

	if (len >= sizeof(req))
		return NULL;

	if (headers && ucv_type(headers) == UC_OBJECT) {
		ucv_object_foreach(headers, hk, hval) {
			if (!hk || strchr(hk, '\r') || strchr(hk, '\n') || strchr(hk, ':')) {
				ok = false;
				break;
			}

			hvs = ucv_to_string(vm, hval);

			if (!hvs || strchr(hvs, '\r') || strchr(hvs, '\n')) {
				free(hvs);
				ok = false;
				break;
			}

			n = snprintf(req + len, sizeof(req) - len, "%s: %s\r\n", hk, hvs);

			free(hvs);

			if (n >= sizeof(req) - len - 2) {
				ok = false;
				break;
			}

			len += n;
		}
	}

	if (!ok)
		return NULL;

	len += snprintf(req + len, sizeof(req) - len, "\r\n");

	return strdup(req);
}

/* ---------------------------------------------------------------------- */
/* Resource methods                                                       */
/* ---------------------------------------------------------------------- */

static int
ws_event_slot(const char *name)
{
	if (!strcmp(name, "open")) return 0;
	if (!strcmp(name, "message")) return 1;
	if (!strcmp(name, "close")) return 2;
	if (!strcmp(name, "error")) return 3;

	return -1;
}

static uc_value_t *
uc_ws_on(uc_vm_t *vm, size_t nargs)
{
	uc_websocket_t *ws = uc_fn_thisval("websocket.connection");
	uc_value_t *event = uc_fn_arg(0);
	uc_value_t *fn = uc_fn_arg(1);
	const char *name;
	int slot;

	if (!ws)
		err_return(EINVAL);

	if (!ws->obj || ws->state == WS_STATE_CLOSED)
		err_return(ENOTCONN);

	name = ucv_string_get(event);

	if (!name || (slot = ws_event_slot(name)) < 0) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"Event must be one of 'open', 'message', 'close' or 'error'");

		return NULL;
	}

	if (!ucv_is_callable(fn)) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Callback must be a function");

		return NULL;
	}

	ucv_resource_value_set(ws->obj, slot, ucv_get(fn));

	ok_return(ucv_boolean_new(true));
}

static uc_value_t *
uc_ws_send(uc_vm_t *vm, size_t nargs)
{
	uc_websocket_t *ws = uc_fn_thisval("websocket.connection");
	uc_value_t *data = uc_fn_arg(0);
	struct wslay_event_msg msg;
	uint8_t *bin = NULL;
	const uint8_t *payload;
	size_t len, i;
	int64_t b;
	int rv;

	if (!ws)
		err_return(EINVAL);

	if (ws->state != WS_STATE_OPEN)
		err_return(ENOTCONN);

	if (ucv_type(data) == UC_STRING) {
		payload = (const uint8_t *)ucv_string_get(data);
		len = ucv_string_length(data);
		msg.opcode = WSLAY_TEXT_FRAME;
	}
	else if (ucv_type(data) == UC_ARRAY) {
		len = ucv_array_length(data);
		bin = malloc(len ? len : 1);

		if (!bin)
			err_return(ENOMEM);

		for (i = 0; i < len; i++) {
			b = ucv_int64_get(ucv_array_get(data, i));

			if (errno || b < 0 || b > 255) {
				free(bin);
				err_return(EINVAL);
			}

			bin[i] = (uint8_t)b;
		}

		payload = bin;
		msg.opcode = WSLAY_BINARY_FRAME;
	}
	else {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"Data must be a string or an array of bytes");

		return NULL;
	}

	msg.msg = payload;
	msg.msg_length = len;

	rv = wslay_event_queue_msg(ws->ctx, &msg);

	free(bin);

	if (rv < 0)
		err_return(rv == WSLAY_ERR_NO_MORE_MSG ? EPIPE : ENOMEM);

	if (ws->dispatching)
		ws->need_flush = true;
	else
		ws_flush(ws);

	ok_return(ucv_boolean_new(true));
}

static uc_value_t *
uc_ws_ping(uc_vm_t *vm, size_t nargs)
{
	uc_websocket_t *ws = uc_fn_thisval("websocket.connection");
	uc_value_t *data = uc_fn_arg(0);
	struct wslay_event_msg msg;
	const char *payload = "";
	size_t len = 0;
	int rv;

	if (!ws)
		err_return(EINVAL);

	if (ws->state != WS_STATE_OPEN)
		err_return(ENOTCONN);

	if (data && ucv_type(data) == UC_STRING) {
		payload = ucv_string_get(data);
		len = ucv_string_length(data);

		if (len > 125) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"Ping payload must not exceed 125 bytes");

			return NULL;
		}
	}

	msg.opcode = WSLAY_PING;
	msg.msg = (const uint8_t *)payload;
	msg.msg_length = len;

	rv = wslay_event_queue_msg(ws->ctx, &msg);

	if (rv < 0)
		err_return(rv == WSLAY_ERR_NO_MORE_MSG ? EPIPE : ENOMEM);

	if (ws->dispatching)
		ws->need_flush = true;
	else
		ws_flush(ws);

	ok_return(ucv_boolean_new(true));
}

static uc_value_t *
uc_ws_close(uc_vm_t *vm, size_t nargs)
{
	uc_websocket_t *ws = uc_fn_thisval("websocket.connection");
	uc_value_t *code = uc_fn_arg(0);
	uc_value_t *reason = uc_fn_arg(1);
	const char *r = "";
	uint16_t c = 1000;
	int64_t n = 1000;
	int rv;

	if (!ws)
		err_return(EINVAL);

	if (ws->state == WS_STATE_CLOSED)
		err_return(ENOTCONN);

	/* close() while the connection is still being established aborts it */
	if (ws->state != WS_STATE_OPEN && ws->state != WS_STATE_CLOSING) {
		ws_emit_close(ws, 1006, "");
		ws_teardown(ws);
		ok_return(ucv_boolean_new(true));
	}

	/* subsequent close() calls while already closing are no-ops */
	if (ws->state == WS_STATE_CLOSING)
		ok_return(ucv_boolean_new(true));

	if (code) {
		errno = 0;
		n = ucv_int64_get(code);

		if (errno || n < 1000 || n > 4999) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"Close code must be between 1000 and 4999");

			return NULL;
		}
	}

	if (reason && ucv_type(reason) == UC_STRING) {
		r = ucv_string_get(reason);

		if (strlen(r) > 123 || strchr(r, '\r') || strchr(r, '\n')) {
			uc_vm_raise_exception(vm, EXCEPTION_TYPE,
				"Close reason must not exceed 123 bytes or contain line breaks");

			return NULL;
		}
	}

	c = (uint16_t)n;

	ws->state = WS_STATE_CLOSING;

	uloop_timeout_set(&ws->timeout, WS_CLOSE_TIMEOUT);

	rv = wslay_event_queue_close(ws->ctx, c, (const uint8_t *)r, strlen(r));

	if (rv < 0)
		err_return(EPIPE);

	if (ws->dispatching)
		ws->need_flush = true;
	else {
		ws_flush(ws);
		ws_check_lifecycle(ws);
	}

	ok_return(ucv_boolean_new(true));
}

static uc_value_t *
uc_ws_state(uc_vm_t *vm, size_t nargs)
{
	uc_websocket_t *ws = uc_fn_thisval("websocket.connection");
	const char *state;

	if (!ws)
		err_return(EINVAL);

	switch (ws->state) {
	case WS_STATE_CONNECTING:
	case WS_STATE_HANDSHAKE_WRITE:
	case WS_STATE_HANDSHAKE_READ:
		state = "connecting";
		break;

	case WS_STATE_OPEN:
		state = "open";
		break;

	case WS_STATE_CLOSING:
		state = "closing";
		break;

	default:
		state = "closed";
		break;
	}

	ok_return(ucv_string_new(state));
}

static uc_value_t *
uc_ws_fileno(uc_vm_t *vm, size_t nargs)
{
	uc_websocket_t *ws = uc_fn_thisval("websocket.connection");

	if (!ws)
		err_return(EINVAL);

	ok_return(ucv_int64_new(ws->ufd.fd));
}

/* ---------------------------------------------------------------------- */
/* connect()                                                              */
/* ---------------------------------------------------------------------- */

/**
 * Initiate a WebSocket connection to the given URL.
 *
 * The URL must use the `ws://` scheme (`wss://` requires TLS support which
 * is not implemented yet). IPv6 literals may be given in bracket notation,
 * e.g. `ws://[::1]:8080/path`. Userinfo components are rejected.
 *
 * Name resolution is performed synchronously; the TCP connection, the
 * protocol handshake and the subsequent data exchange are fully
 * asynchronous and driven by the uloop event loop, so `uloop.run()` must
 * be invoked for the connection to progress.
 *
 * Runtime failures (refused connections, handshake errors, timeouts,
 * abrupt resets) are reported through the `error` event, invalid
 * arguments raise type exceptions.
 *
 * @function module:websocket#connect
 *
 * @param {string} url
 * The WebSocket URL to connect to, e.g. `ws://host:port/path`.
 *
 * @param {Object} [options]
 * Additional connection options:
 *
 *   - `timeout` (number): connect and handshake timeout in milliseconds,
 *     `0` disables the timeout. Default: `15000`.
 *   - `max_frame_size` (number): maximum accepted message size in bytes;
 *     larger messages cause a close handshake with status code 1009.
 *     Clamped to the range 1024 to 16777216. Default: `262144`.
 *   - `headers` (Object): additional HTTP headers to include in the
 *     handshake request. Values containing line breaks are rejected.
 *
 * @returns {?websocket.connection}
 * Returns the connection resource or `null` on error, e.g. when name
 * resolution fails. Consult `error()` for the cause in that case.
 *
 * @example
 * ```javascript
 * import { connect } from 'websocket';
 * import * as uloop from 'uloop';
 *
 * uloop.init();
 *
 * let ws = connect('ws://10.0.0.1:8080/telemetry', {
 *     headers: { Authorization: 'Bearer secret' },
 *     max_frame_size: 65536
 * });
 *
 * ws.on('open',    (ws) => ws.send('hello'));
 * ws.on('message', (ws, data, is_text) => print(data));
 * ws.on('close',   (ws, code, reason) => print(`closed ${code}\n`));
 * ws.on('error',   (ws, error) => print(`error: ${error}\n`));
 *
 * uloop.run();
 * ```
 */
static uc_value_t *
uc_ws_connect(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *url = uc_fn_arg(0);
	uc_value_t *options = uc_fn_arg(1);
	uc_value_t *hv, *hdrs = NULL;
	struct addrinfo hints, *res = NULL, *rp;
	uc_websocket_t *ws = NULL;
	char *host = NULL, *path = NULL;
	bool tls = false;
	int port = 0, fd = -1, rv, immediate = 0;
	int64_t timeout = WS_DEFAULT_TIMEOUT;
	uint64_t maxmsg = WS_DEFAULT_MAX_MSG;

	if (ucv_type(url) != UC_STRING) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "URL must be a string");

		return NULL;
	}

	if (!ws_parse_url(ucv_string_get(url), &host, &port, &path, &tls)) {
		free(host);
		free(path);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid WebSocket URL");

		return NULL;
	}

	if (tls) {
		free(host);
		free(path);
		uc_vm_raise_exception(vm, EXCEPTION_TYPE,
			"TLS (wss://) is not supported yet");

		return NULL;
	}

	if (options && ucv_type(options) == UC_OBJECT) {
		hv = ucv_object_get(options, "timeout", NULL);

		if (hv) {
			timeout = ucv_int64_get(hv);

			if (timeout < 0)
				timeout = 0;
		}

		hv = ucv_object_get(options, "max_frame_size", NULL);

		if (hv) {
			maxmsg = ucv_uint64_get(hv);

			if (maxmsg < 1024)
				maxmsg = 1024;

			if (maxmsg > 16 * 1024 * 1024)
				maxmsg = 16 * 1024 * 1024;
		}

		hdrs = ucv_object_get(options, "headers", NULL);
	}

	if (timeout && timeout < WS_MIN_TIMEOUT)
		timeout = WS_MIN_TIMEOUT;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	rv = getaddrinfo(host, NULL, &hints, &res);

	if (rv) {
		free(host);
		free(path);
		err_return(EHOSTUNREACH);
	}

	for (rp = res; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

		if (fd < 0)
			continue;

		if (rp->ai_family == AF_INET6)
			((struct sockaddr_in6 *)rp->ai_addr)->sin6_port = htons(port);
		else
			((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(port);

		rv = connect(fd, rp->ai_addr, rp->ai_addrlen);

		if (rv == 0) {
			immediate = 1;
			break;
		}

		if (errno == EINPROGRESS)
			break;

		close(fd);
		fd = -1;
	}

	if (fd < 0) {
		freeaddrinfo(res);
		free(host);
		free(path);
		err_return(errno ? errno : ECONNREFUSED);
	}

	hv = ucv_resource_create_ex(vm, "websocket.connection",
		(void **)&ws, 4, sizeof(*ws));

	if (!hv) {
		close(fd);
		freeaddrinfo(res);
		free(host);
		free(path);
		err_return(ENOMEM);
	}

	memset(ws, 0, sizeof(*ws));

	ws->vm = vm;
	ws->obj = ucv_get(hv);
	ws->host = host;
	ws->path = path;
	ws->port = port;
	ws->max_msg_len = maxmsg;
	ws->ufd.fd = fd;
	ws->ufd.cb = ws_ufd_cb;
	ws->timeout.cb = ws_timeout_cb;
	ws->state = immediate ? WS_STATE_HANDSHAKE_WRITE : WS_STATE_CONNECTING;

	ucv_resource_persistent_set(ws->obj, true);

	ws->hs_req = ws_build_request(vm, ws,
		(hdrs && ucv_type(hdrs) == UC_OBJECT) ? hdrs : NULL);

	if (!ws->hs_req) {
		ws_teardown(ws);
		ucv_put(hv);
		err_return(EINVAL);
	}

	ws->hs_req_len = strlen(ws->hs_req);

	freeaddrinfo(res);

	if (uloop_fd_add(&ws->ufd, ULOOP_WRITE) != 0) {
		ws_teardown(ws);
		ucv_put(hv);
		err_return(errno ? errno : EINVAL);
	}

	if (timeout)
		uloop_timeout_set(&ws->timeout, (int)timeout);

	if (immediate)
		ws_handshake_write(ws);

	ok_return(hv);
}

/* ---------------------------------------------------------------------- */
/* Registration                                                           */
/* ---------------------------------------------------------------------- */

static uc_value_t *
uc_ws_error(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *errmsg;

	if (last_error == 0)
		return NULL;

	errmsg = ucv_string_new(strerror(last_error));
	last_error = 0;

	return errmsg;
}

static const uc_function_list_t ws_methods[] = {
	{ "on",			uc_ws_on },
	{ "send",		uc_ws_send },
	{ "ping",		uc_ws_ping },
	{ "close",		uc_ws_close },
	{ "state",		uc_ws_state },
	{ "fileno",		uc_ws_fileno },
};

static const uc_function_list_t ws_functions[] = {
	{ "connect",	uc_ws_connect },
	{ "error",		uc_ws_error },
};

static void
ws_free_resource(void *ud)
{
	ws_teardown(ud);
}

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, ws_functions);

	uc_type_declare(vm, "websocket.connection", ws_methods, ws_free_resource);

	uloop_init();
}

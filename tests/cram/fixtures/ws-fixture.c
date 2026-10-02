/*
 * Minimal scripted WebSocket server used as test fixture by the
 * tests/cram/websocket test cases.
 *
 * Scenarios are selected through the request path:
 *
 *   /announce        send a text message "path=<path> host=<host>" then echo
 *   /echo            echo data, binary and ping frames
 *   /close           initiate close(1001, "server bye") then echo loop
 *   /big<N>          send a single N byte text frame, then echo loop
 *   /frag<N>         send N bytes as one fragmented message in 1 KiB frames
 *   /flood<N>        send N x 1 KiB text messages, then echo loop
 *   /ping            send a ping, then echo loop
 *   /reset           tear the connection down with TCP RST
 *   /badaccept       complete the handshake with a wrong accept hash
 *   /http200         reply with a plain HTTP 200 instead of 101
 *
 * Usage: ws-fixture <port> [connections]
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static uint32_t sha1_state[5] = {
	0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
};

static uint64_t sha1_count;
static uint8_t sha1_buffer[64];

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void
sha1_transform(const uint8_t *p)
{
	uint32_t w[80], a, b, c, d, e, t;
	size_t i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];

	for (i = 16; i < 80; i++)
		w[i] = ROTL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

	a = sha1_state[0]; b = sha1_state[1]; c = sha1_state[2];
	d = sha1_state[3]; e = sha1_state[4];

	for (i = 0; i < 80; i++) {
		if (i < 20)
			t = ((b & c) | ((~b) & d)) + 0x5A827999;
		else if (i < 40)
			t = (b ^ c ^ d) + 0x6ED9EBA1;
		else if (i < 60)
			t = ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDC;
		else
			t = (b ^ c ^ d) + 0xCA62C1D6;

		t += ROTL(a, 5) + e + w[i];
		e = d; d = c; c = ROTL(b, 30); b = a; a = t;
	}

	sha1_state[0] += a; sha1_state[1] += b; sha1_state[2] += c;
	sha1_state[3] += d; sha1_state[4] += e;
}

static void
sha1_reset(void)
{
	sha1_state[0] = 0x67452301; sha1_state[1] = 0xEFCDAB89;
	sha1_state[2] = 0x98BADCFE; sha1_state[3] = 0x10325476;
	sha1_state[4] = 0xC3D2E1F0;
	sha1_count = 0;
}

static void
sha1_update(const uint8_t *data, size_t len)
{
	size_t i = 0, n;

	while (i < len) {
		n = 64 - (sha1_count % 64);

		if (n > len - i)
			n = len - i;

		memcpy(sha1_buffer + (sha1_count % 64), data + i, n);
		sha1_count += n;
		i += n;

		if (sha1_count % 64 == 0)
			sha1_transform(sha1_buffer);
	}
}

static void
sha1_final(uint8_t digest[20])
{
	static const uint8_t pad[64] = { 0x80 };
	uint64_t bits = sha1_count * 8;
	uint8_t tail[8];
	size_t i;

	for (i = 0; i < 8; i++)
		tail[i] = (uint8_t)(bits >> (56 - 8 * i));

	sha1_update(pad, 1 + ((119 - sha1_count % 64) % 64));
	sha1_update(tail, 8);

	for (i = 0; i < 5; i++) {
		digest[i * 4] = (uint8_t)(sha1_state[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(sha1_state[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(sha1_state[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)(sha1_state[i]);
	}
}

static const char b64tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
b64_encode(const uint8_t *in, size_t inlen, char *out)
{
	size_t i = 0, j = 0;

	while (i + 3 <= inlen) {
		uint32_t v = ((uint32_t)in[i] << 16) |
			((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];

		out[j++] = b64tab[(v >> 18) & 63];
		out[j++] = b64tab[(v >> 12) & 63];
		out[j++] = b64tab[(v >> 6) & 63];
		out[j++] = b64tab[v & 63];
		i += 3;
	}

	if (i < inlen) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < inlen)
			v |= (uint32_t)in[i + 1] << 8;

		out[j++] = b64tab[(v >> 18) & 63];
		out[j++] = b64tab[(v >> 12) & 63];
		out[j++] = (i + 1 < inlen) ? b64tab[(v >> 6) & 63] : '=';
		out[j++] = '=';
	}

	out[j] = '\0';
}

static int
readn(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	ssize_t n;

	while (len > 0) {
		n = read(fd, p, len);

		if (n == 0)
			return -1;

		if (n < 0) {
			if (errno == EINTR)
				continue;

			return -1;
		}

		p += n;
		len -= n;
	}

	return 0;
}

static int
writeall(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	ssize_t n;

	while (len > 0) {
		n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			return -1;
		}

		p += n;
		len -= n;
	}

	return 0;
}

static int
send_frame(int fd, int opcode, int fin, const uint8_t *payload, size_t len)
{
	uint8_t hdr[10];
	size_t hlen = 2;

	hdr[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);

	if (len < 126) {
		hdr[1] = (uint8_t)len;
	}
	else if (len <= 0xFFFF) {
		hdr[1] = 126;
		hdr[2] = (uint8_t)(len >> 8);
		hdr[3] = (uint8_t)len;
		hlen = 4;
	}
	else {
		hdr[1] = 127;
		for (int i = 0; i < 8; i++)
			hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
		hlen = 10;
	}

	if (writeall(fd, hdr, hlen) < 0)
		return -1;

	return writeall(fd, payload, len);
}

static int
recv_frame(int fd, int *opcode, int *fin, uint8_t **payload, size_t *len)
{
	uint8_t hdr[2], ext[8], mask[4];
	uint64_t plen;
	size_t need;
	uint8_t *buf;

	if (readn(fd, hdr, 2) < 0)
		return -1;

	*fin = (hdr[0] >> 7) & 1;
	*opcode = hdr[0] & 0x0F;

	if (!(hdr[1] & 0x80))
		return -1; /* client frames must be masked */

	plen = hdr[1] & 0x7F;

	if (plen == 126) {
		if (readn(fd, ext, 2) < 0)
			return -1;
		plen = ((uint64_t)ext[0] << 8) | ext[1];
	}
	else if (plen == 127) {
		if (readn(fd, ext, 8) < 0)
			return -1;
		plen = 0;
		for (int i = 0; i < 8; i++)
			plen = (plen << 8) | ext[i];
	}

	if (readn(fd, mask, 4) < 0)
		return -1;

	if (plen > 16 * 1024 * 1024)
		return -1;

	need = (size_t)plen;
	buf = malloc(need ? need : 1);

	if (!buf)
		return -1;

	if (readn(fd, buf, need) < 0) {
		free(buf);
		return -1;
	}

	for (size_t i = 0; i < need; i++)
		buf[i] ^= mask[i % 4];

	*payload = buf;
	*len = need;

	return 0;
}

static int
do_handshake(int fd, char *path, size_t pathsize, char *host, size_t hostsize,
             char *key, size_t keysize)
{
	char req[4096];
	size_t len = 0;
	ssize_t n;
	char *line, *eol, *k;

	while (len + 1 < sizeof(req)) {
		n = read(fd, req + len, sizeof(req) - len - 1);

		if (n <= 0)
			return -1;

		len += n;
		req[len] = '\0';

		if (strstr(req, "\r\n\r\n"))
			break;
	}

	if (!strstr(req, "\r\n\r\n"))
		return -1;

	path[0] = host[0] = key[0] = '\0';

	for (line = req; (eol = strstr(line, "\r\n")); line = eol + 2) {
		if (!strncmp(line, "GET ", 4)) {
			k = strchr(line + 4, ' ');

			if (k) {
				snprintf(path, pathsize, "%.*s",
					(int)(k - (line + 4)), line + 4);
			}
		}
		else if (!strncasecmp(line, "Host:", 5)) {
			k = line + 5;

			while (*k == ' ')
				k++;

			snprintf(host, hostsize, "%.*s", (int)(eol - k), k);
		}
		else if (!strncasecmp(line, "Sec-WebSocket-Key:", 18)) {
			k = line + 18;

			while (*k == ' ')
				k++;

			snprintf(key, keysize, "%.*s", (int)(eol - k), k);
		}
	}

	return (key[0] && path[0]) ? 0 : -1;
}

static int
send_upgrade_response(int fd, const char *key, int mode)
{
	char accept_src[128], accept[64], response[512];
	uint8_t digest[20];

	sha1_reset();
	snprintf(accept_src, sizeof(accept_src), "%s%s", key, WS_GUID);
	sha1_update((const uint8_t *)accept_src, strlen(accept_src));
	sha1_final(digest);
	b64_encode(digest, 20, accept);

	switch (mode) {
	case 1: /* wrong accept hash */
		accept[3] = (accept[3] == 'A') ? 'B' : 'A';
		break;

	case 2: /* plain HTTP reply */
		return writeall(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 38);
	}

	snprintf(response, sizeof(response),
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n\r\n", accept);

	return writeall(fd, response, strlen(response));
}

static int
echo_loop(int fd)
{
	uint8_t *payload;
	size_t len;
	int opcode, fin;

	while (recv_frame(fd, &opcode, &fin, &payload, &len) == 0) {
		if (opcode == 8) {
			uint8_t reply[2] = { payload[0], payload[1] };

			send_frame(fd, 8, 1, reply, len >= 2 ? 2 : 0);
			free(payload);
			return 0;
		}
		else if (opcode == 9) {
			send_frame(fd, 10, 1, payload, len);
		}
		else if (opcode == 1 || opcode == 2) {
			send_frame(fd, opcode, 1, payload, len);
		}

		free(payload);
	}

	return -1;
}

static void
fill_pattern(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)('a' + (i % 26));
}

static int
handle_connection(int fd)
{
	char path[256], host[256], key[64];
	char *s;
	long n;
	uint8_t *blob;
	int mode = 0;

	if (do_handshake(fd, path, sizeof(path), host, sizeof(host),
	                 key, sizeof(key)) < 0)
		return -1;

	if (!strncmp(path, "/badaccept", 10))
		mode = 1;
	else if (!strncmp(path, "/http200", 8))
		mode = 2;

	if (send_upgrade_response(fd, key, mode) < 0)
		return -1;

	if (!strncmp(path, "/reset", 6)) {
		struct linger l = { .l_onoff = 1, .l_linger = 0 };

		setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));

		return -1;
	}

	if (!strncmp(path, "/msgreset", 9)) {
		struct linger l = { .l_onoff = 1, .l_linger = 0 };

		send_frame(fd, 1, 1, (const uint8_t *)"bye", 3);
		setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));

		return -1;
	}

	if (!strncmp(path, "/announce", 9)) {
		char msg[512];

		snprintf(msg, sizeof(msg), "path=%.220s host=%.220s", path, host);
		send_frame(fd, 1, 1, (const uint8_t *)msg, strlen(msg));
	}
	else if (!strncmp(path, "/close", 6)) {
		const char *reason = "server bye";
		uint8_t payload[125];

		payload[0] = 1001 >> 8;
		payload[1] = 1001 & 0xFF;
		memcpy(payload + 2, reason, strlen(reason));

		send_frame(fd, 8, 1, payload, 2 + strlen(reason));
	}
	else if (!strncmp(path, "/big", 4)) {
		n = strtol(path + 4, &s, 10);

		if (n > 0 && n <= 16 * 1024 * 1024) {
			blob = malloc(n);

			if (blob) {
				fill_pattern(blob, n);
				send_frame(fd, 1, 1, blob, n);
				free(blob);
			}
		}
	}
	else if (!strncmp(path, "/frag", 5)) {
		n = strtol(path + 5, &s, 10);

		if (n > 0 && n <= 16 * 1024 * 1024) {
			size_t total = (size_t)n;

			blob = malloc(total);

			if (blob) {
				size_t off = 0, chunk;

				fill_pattern(blob, total);

				while (off < total) {
					chunk = total - off;

					if (chunk > 1024)
						chunk = 1024;

					send_frame(fd, (off == 0) ? 1 : 0,
					           (off + chunk == total) ? 1 : 0,
					           blob + off, chunk);
					off += chunk;
				}

				free(blob);
			}
		}
	}
	else if (!strncmp(path, "/flood", 6)) {
		n = strtol(path + 6, &s, 10);

		if (n > 0 && n <= 100000) {
			blob = malloc(1024);

			if (blob) {
				fill_pattern(blob, 1024);

				for (long i = 0; i < n; i++)
					send_frame(fd, 1, 1, blob, 1024);

				free(blob);
			}
		}
	}
	else if (!strncmp(path, "/ping", 5)) {
		send_frame(fd, 9, 1, (const uint8_t *)"fixture-ping", 12);
	}

	return echo_loop(fd);
}

int
main(int argc, char **argv)
{
	struct sockaddr_in6 addr;
	int srv, fd, one = 1, connections = 1, served = 0;
	uint16_t port;
	char *s;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <port> [connections]\n", argv[0]);
		return 1;
	}

	port = (uint16_t)strtoul(argv[1], &s, 10);

	if (argc > 2)
		connections = (int)strtoul(argv[2], &s, 10);

	signal(SIGPIPE, SIG_IGN);

	srv = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (srv < 0)
		return 1;

	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &(int){ 0 }, sizeof(int));

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_any;
	addr.sin6_port = htons(port);

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "bind failed: %s\n", strerror(errno));
		return 1;
	}

	if (listen(srv, 8) < 0)
		return 1;

	while (served < connections) {
		fd = accept4(srv, NULL, NULL, SOCK_CLOEXEC);

		if (fd < 0) {
			if (errno == EINTR)
				continue;

			return 1;
		}

		handle_connection(fd);

		close(fd);
		served++;
	}

	return 0;
}

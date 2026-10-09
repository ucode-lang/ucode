/*
Copyright 2021 Jo-Philipp Wich <jo@mein.io>

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/**
 * # Routing Netlink
 *
 * The `rtnl` module provides functions for interacting with the routing netlink interface.
 *
 * Functions can be individually imported and directly accessed using the
 * {@link https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/import#named_import named import}
 * syntax:
 *
 *   ```javascript
 *   import { error, request, listener, RTM_GETROUTE, RTM_NEWROUTE, RTM_DELROUTE, AF_INET } from 'rtnl';
 *
 *   // Send a netlink request
 *   let response = request(RTM_GETROUTE, 0, { family: AF_INET });
 *
 *   // Create a listener for route changes
 *   let routeListener = listener((msg) => {
 *       print('Received route message:', msg, '\n');
 *   }, [RTM_NEWROUTE, RTM_DELROUTE]);
 *   ```
 *
 * Alternatively, the module namespace can be imported
 * using a wildcard import statement:
 *
 *   ```javascript
 *   import * as rtnl from 'rtnl';
 *
 *   // Send a netlink request
 *   let response = rtnl.request(rtnl.RTM_GETROUTE, 0, { family: rtnl.AF_INET });
 *
 *   // Create a listener for route changes
 *   let listener = rtnl.listener((msg) => {
 *       print('Received route message:', msg, '\n');
 *   }, [rtnl.RTM_NEWROUTE, rtnl.RTM_DELROUTE]);
 *   ```
 *
 * Additionally, the rtnl module namespace may also be imported by invoking
 * the `ucode` interpreter with the `-lrtnl` switch.
 *
 * @module rtnl
 */

#include <errno.h>
#include <assert.h>

#include <netinet/ether.h>
#include <arpa/inet.h>
#include <netlink/attr.h>

#include <linux/if_tunnel.h>
#include <linux/ip6_tunnel.h>
#include <linux/lwtunnel.h>
#include <linux/mpls.h>
#include <linux/mpls_iptunnel.h>
#include <linux/seg6_iptunnel.h>
#include <linux/seg6_hmac.h>
#include <linux/veth.h>
#include <linux/ila.h>
#include <linux/fib_rules.h>
#include <linux/if_addrlabel.h>
#include <linux/if_bridge.h>
#include <linux/netconf.h>
#include <linux/ipv6.h>
#include <linux/can/netlink.h>
#include <linux/can/vxcan.h>

#include <libubox/uloop.h>

#include "ucode/module.h"

#define DIV_ROUND_UP(n, d)      (((n) + (d) - 1) / (d))

#define err_return(code, ...) do { set_error(code, __VA_ARGS__); return NULL; } while(0)

#define NLM_F_STRICT_CHK (1 << 15)

#define RTNL_CMDS_BITMAP_SIZE	DIV_ROUND_UP(__RTM_MAX, 32)
#define RTNL_GRPS_BITMAP_SIZE	DIV_ROUND_UP(__RTNLGRP_MAX, 32)

/* Can't use net/if.h for declarations as it clashes with linux/if.h
 * on certain musl versions.
 * Ref: https://www.openwall.com/lists/musl/2017/04/16/1 */
extern unsigned int if_nametoindex (const char *);
extern char *if_indextoname (unsigned int ifindex, char *ifname);

static struct {
	int code;
	char *msg;
} last_error;

__attribute__((format(printf, 2, 3))) static void
set_error(int errcode, const char *fmt, ...) {
	va_list ap;

	free(last_error.msg);

	last_error.code = errcode;
	last_error.msg = NULL;

	if (fmt) {
		va_start(ap, fmt);
		xvasprintf(&last_error.msg, fmt, ap);
		va_end(ap);
	}
}

static uc_resource_type_t *listener_type;
static uc_value_t *listener_registry;
static uc_vm_t *listener_vm;

typedef struct {
	uint32_t cmds[RTNL_CMDS_BITMAP_SIZE];
	size_t index;
} uc_nl_listener_t;

typedef struct {
	uint8_t family;
	uint8_t mask;
	uint8_t alen;
	uint8_t bitlen;
	union {
		struct in_addr in;
		struct in6_addr in6;
		struct mpls_label mpls[16];
	} addr;
} uc_nl_cidr_t;

static bool
uc_nl_parse_u32(uc_value_t *val, uint32_t *n)
{
	uint64_t u;

	u = ucv_to_unsigned(val);

	if (errno != 0 || u > UINT32_MAX)
		return false;

	*n = (uint32_t)u;

	return true;
}

static bool
uc_nl_parse_s32(uc_value_t *val, uint32_t *n)
{
	int64_t i;

	i = ucv_to_integer(val);

	if (errno != 0 || i < INT32_MIN || i > INT32_MAX)
		return false;

	*n = (uint32_t)i;

	return true;
}

static bool
uc_nl_parse_u64(uc_value_t *val, uint64_t *n)
{
	*n = ucv_to_unsigned(val);

	return (errno == 0);
}

static const char *
addr64_ntop(const void *addr, char *buf, size_t buflen)
{
	const union { uint64_t u64; uint16_t u16[4]; } *a64 = addr;
	int len;

	errno = 0;

	len = snprintf(buf, buflen, "%04x:%04x:%04x:%04x",
	               ntohs(a64->u16[0]), ntohs(a64->u16[1]),
	               ntohs(a64->u16[2]), ntohs(a64->u16[3]));

	if ((size_t)len >= buflen) {
		errno = ENOSPC;

		return NULL;
	}

	return buf;
}

static int
addr64_pton(const char *src, void *dst)
{
	union { uint64_t u64; uint16_t u16[4]; } *a64 = dst;
	unsigned long n;
	size_t i;
	char *e;

	for (i = 0; i < ARRAY_SIZE(a64->u16); i++) {
		n = strtoul(src, &e, 16);

		if (e == src || n > 0xffff)
			return -1;

		a64->u16[i] = htons(n);

		if (*e == 0)
			break;

		if (i >= 3 || *e != ':')
			return -1;

		src += (e - src) + 1;
	}

	return 0;
}

static const char *
mpls_ntop(const void *addr, size_t addrlen, char *buf, size_t buflen)
{
	const struct mpls_label *p = addr;
	size_t remlen = buflen;
	uint32_t entry, label;
	char *s = buf;
	int len;

	errno = 0;

	while (addrlen >= sizeof(*p)) {
		entry = ntohl(p->entry);
		label = (entry & MPLS_LS_LABEL_MASK) >> MPLS_LS_LABEL_SHIFT;

		len = snprintf(s, remlen, "%u", label);

		if ((size_t)len >= remlen)
			break;

		if (entry & MPLS_LS_S_MASK)
			return buf;

		s += len;
		remlen -= len;

		if (remlen) {
			*s++ = '/';
			remlen--;
		}

		p++;

		addrlen -= sizeof(*p);
	}

	errno = ENOSPC;

	return NULL;
}

static int
mpls_pton(int af, const char *src, void *dst, size_t dstlen)
{
	size_t max = dstlen / sizeof(struct mpls_label);
	struct mpls_label *p = dst;
	uint32_t label;
	char *e;

	errno = 0;

	if (af != AF_MPLS) {
		errno = EAFNOSUPPORT;

		return -1;
	}

	while (max > 0) {
		label = strtoul(src, &e, 0);

		if (label >= (1 << 20))
			return 0;

		if (e == src)
			return 0;

		p->entry = htonl(label << MPLS_LS_LABEL_SHIFT);

		if (*e == 0) {
			p->entry |= htonl(1 << MPLS_LS_S_SHIFT);

			return 1;
		}

		if (*e != '/')
			return 0;

		src += (e - src) + 1;
		max--;
		p++;
	}

	errno = ENOSPC;

	return -1;
}

static bool
uc_nl_parse_cidr(uc_vm_t *vm, uc_value_t *val, uc_nl_cidr_t *p)
{
	char *s = ucv_to_string(vm, val);
	struct in6_addr mask6 = { 0 };
	struct in_addr mask = { 0 };
	bool valid = true;
	char *m, *e;
	long n = 0;
	size_t i;

	if (!s)
		return false;

	m = strchr(s, '/');

	if (m)
		*m++ = '\0';

	if (inet_pton(AF_INET6, s, &p->addr.in6) == 1) {
		if (m) {
			if (inet_pton(AF_INET6, m, &mask6) == 1) {
				while (n < 128 && (mask6.s6_addr[n / 8] << (n % 8)) & 128)
					n++;
			}
			else {
				n = strtol(m, &e, 10);

				if (e == m || *e || n < 0 || n > 128)
					valid = false;
			}

			p->mask = (uint8_t)n;
		}
		else {
			p->mask = 128;
		}

		p->family = AF_INET6;
		p->alen = sizeof(mask6);
		p->bitlen = p->alen * 8;
	}
	else if (strchr(s, '.') && inet_pton(AF_INET, s, &p->addr.in) == 1) {
		if (m) {
			if (inet_pton(AF_INET, m, &mask) == 1) {
				mask.s_addr = ntohl(mask.s_addr);

				while (n < 32 && (mask.s_addr << n) & 0x80000000)
					n++;
			}
			else {
				n = strtol(m, &e, 10);

				if (e == m || *e || n < 0 || n > 32)
					valid = false;
			}

			p->mask = (uint8_t)n;
		}
		else {
			p->mask = 32;
		}

		p->family = AF_INET;
		p->alen = sizeof(mask);
		p->bitlen = p->alen * 8;
	}
	else {
		if (m)
			m[-1] = '/';

		if (mpls_pton(AF_MPLS, s, &p->addr.mpls, sizeof(p->addr.mpls)) == 1) {
			p->family = AF_MPLS;
			p->alen = 0;

			for (i = 0; i < ARRAY_SIZE(p->addr.mpls); i++) {
				p->alen += sizeof(struct mpls_label);

				if (ntohl(p->addr.mpls[i].entry) & MPLS_LS_S_MASK)
					break;
			}

			p->bitlen = p->alen * 8;
			p->mask = p->bitlen;
		}
		else {
			valid = false;
		}
	}

	free(s);

	return valid;
}

typedef enum {
	DT_FLAG,
	DT_BOOL,
	DT_U8,
	DT_U16,
	DT_U32,
	DT_S32,
	DT_U64,
	DT_STRING,
	DT_NETDEV,
	DT_LLADDR,
	DT_INADDR,
	DT_IN6ADDR,
	DT_U64ADDR,
	DT_MPLSADDR,
	DT_ANYADDR,
	DT_BRIDGEID,
	DT_LINKINFO,
	DT_MULTIPATH,
	DT_NUMRANGE,
	DT_AFSPEC,
	DT_FLAGS,
	DT_ENCAP,
	DT_SRH,
	DT_IPOPTS,
	DT_U32_OR_MEMBER,
	DT_NESTED,
} uc_nl_attr_datatype_t;

enum {
	DF_NO_SET = (1 << 0),
	DF_NO_GET = (1 << 1),
	DF_ALLOW_NONE = (1 << 2),
	DF_BYTESWAP = (1 << 3),
	DF_MAX_1 = (1 << 4),
	DF_MAX_255 = (1 << 5),
	DF_MAX_65535 = (1 << 6),
	DF_MAX_16777215 = (1 << 7),
	DF_STORE_MASK = (1 << 8),
	DF_MULTIPLE = (1 << 9),
	DF_FLAT = (1 << 10),
	DF_FAMILY_HINT = (1 << 11),
};

typedef struct uc_nl_attr_spec {
	size_t attr;
	const char *key;
	uc_nl_attr_datatype_t type;
	uint32_t flags;
	const void *auxdata;
} uc_nl_attr_spec_t;

typedef struct uc_nl_nested_spec {
	size_t headsize;
	size_t nattrs;
	const uc_nl_attr_spec_t attrs[];
} uc_nl_nested_spec_t;

#define SIZE(type) (void *)(uintptr_t)sizeof(struct type)
#define MEMBER(type, field) (void *)(uintptr_t)offsetof(struct type, field)

static const uc_nl_nested_spec_t route_cacheinfo_rta = {
	.headsize = NLA_ALIGN(sizeof(struct rta_cacheinfo)),
	.nattrs = 8,
	.attrs = {
		{ RTA_UNSPEC, "clntref", DT_U32, 0, MEMBER(rta_cacheinfo, rta_clntref) },
		{ RTA_UNSPEC, "lastuse", DT_U32, 0, MEMBER(rta_cacheinfo, rta_lastuse) },
		{ RTA_UNSPEC, "expires", DT_S32, 0, MEMBER(rta_cacheinfo, rta_expires) },
		{ RTA_UNSPEC, "error", DT_U32, 0, MEMBER(rta_cacheinfo, rta_error) },
		{ RTA_UNSPEC, "used", DT_U32, 0, MEMBER(rta_cacheinfo, rta_used) },
		{ RTA_UNSPEC, "id", DT_U32, 0, MEMBER(rta_cacheinfo, rta_id) },
		{ RTA_UNSPEC, "ts", DT_U32, 0, MEMBER(rta_cacheinfo, rta_ts) },
		{ RTA_UNSPEC, "tsage", DT_U32, 0, MEMBER(rta_cacheinfo, rta_tsage) },
	}
};

static const uc_nl_nested_spec_t route_metrics_rta = {
	.headsize = 0,
	.nattrs = 16,
	.attrs = {
		{ RTAX_MTU, "mtu", DT_U32, 0, NULL },
		{ RTAX_HOPLIMIT, "hoplimit", DT_U32, DF_MAX_255, NULL },
		{ RTAX_ADVMSS, "advmss", DT_U32, 0, NULL },
		{ RTAX_REORDERING, "reordering", DT_U32, 0, NULL },
		{ RTAX_RTT, "rtt", DT_U32, 0, NULL },
		{ RTAX_WINDOW, "window", DT_U32, 0, NULL },
		{ RTAX_CWND, "cwnd", DT_U32, 0, NULL },
		{ RTAX_INITCWND, "initcwnd", DT_U32, 0, NULL },
		{ RTAX_INITRWND, "initrwnd", DT_U32, 0, NULL },
		{ RTAX_FEATURES, "ecn", DT_U32, DF_MAX_1, NULL },
		{ RTAX_QUICKACK, "quickack", DT_U32, DF_MAX_1, NULL },
		{ RTAX_CC_ALGO, "cc_algo", DT_STRING, 0, NULL },
		{ RTAX_RTTVAR, "rttvar", DT_U32, 0, NULL },
		{ RTAX_SSTHRESH, "ssthresh", DT_U32, 0, NULL },
		{ RTAX_FASTOPEN_NO_COOKIE, "fastopen_no_cookie", DT_U32, DF_MAX_1, NULL },
		{ RTAX_LOCK, "lock", DT_U32, 0, NULL },
	}
};

static const uc_nl_nested_spec_t route_msg = {
	.headsize = NLA_ALIGN(sizeof(struct rtmsg)),
	.nattrs = 28,
	.attrs = {
		{ RTA_UNSPEC, "family", DT_U8, 0, MEMBER(rtmsg, rtm_family) },
		{ RTA_UNSPEC, "tos", DT_U8, 0, MEMBER(rtmsg, rtm_tos) },
		{ RTA_UNSPEC, "protocol", DT_U8, 0, MEMBER(rtmsg, rtm_protocol) },
		{ RTA_UNSPEC, "scope", DT_U8, 0, MEMBER(rtmsg, rtm_scope) },
		{ RTA_UNSPEC, "type", DT_U8, 0, MEMBER(rtmsg, rtm_type) },
		{ RTA_UNSPEC, "flags", DT_U32, 0, MEMBER(rtmsg, rtm_flags) },
		{ RTA_SRC, "src", DT_ANYADDR, DF_STORE_MASK|DF_FAMILY_HINT, MEMBER(rtmsg, rtm_src_len) },
		{ RTA_DST, "dst", DT_ANYADDR, DF_STORE_MASK|DF_FAMILY_HINT, MEMBER(rtmsg, rtm_dst_len) },
		{ RTA_IIF, "iif", DT_NETDEV, 0, NULL },
		{ RTA_OIF, "oif", DT_NETDEV, 0, NULL },
		{ RTA_GATEWAY, "gateway", DT_ANYADDR, DF_FAMILY_HINT, NULL },
		{ RTA_PRIORITY, "priority", DT_U32, 0, NULL },
		{ RTA_PREFSRC, "prefsrc", DT_ANYADDR, DF_FAMILY_HINT, NULL },
		{ RTA_METRICS, "metrics", DT_NESTED, 0, &route_metrics_rta },
		{ RTA_MULTIPATH, "multipath", DT_MULTIPATH, 0, NULL },
		{ RTA_FLOW, "flow", DT_U32, 0, NULL },
		{ RTA_CACHEINFO, "cacheinfo", DT_NESTED, DF_NO_SET, &route_cacheinfo_rta },
		{ RTA_TABLE, "table", DT_U32_OR_MEMBER, DF_MAX_255, MEMBER(rtmsg, rtm_table) },
		{ RTA_MARK, "mark", DT_U32, 0, NULL },
		//RTA_MFC_STATS,
		{ RTA_PREF, "pref", DT_U8, 0, NULL },
		{ RTA_ENCAP, "encap", DT_ENCAP, 0, NULL },
		{ RTA_EXPIRES, "expires", DT_U32, 0, NULL },
		{ RTA_UID, "uid", DT_U32, 0, NULL },
		{ RTA_TTL_PROPAGATE, "ttl_propagate", DT_BOOL, 0, NULL },
		{ RTA_IP_PROTO, "ip_proto", DT_U8, 0, NULL },
		{ RTA_SPORT, "sport", DT_U16, DF_BYTESWAP, NULL },
		{ RTA_DPORT, "dport", DT_U16, DF_BYTESWAP, NULL },
		{ RTA_NH_ID, "nh_id", DT_U32, 0, NULL },
	}
};

static const uc_nl_attr_spec_t route_encap_mpls_attrs[] = {
	{ MPLS_IPTUNNEL_DST, "dst", DT_MPLSADDR, 0, NULL },
	{ MPLS_IPTUNNEL_TTL, "ttl", DT_U8, 0, NULL },
};

static const uc_nl_attr_spec_t route_encap_ip_attrs[] = {
	{ LWTUNNEL_IP_ID, "id", DT_U64, DF_BYTESWAP, NULL },
	{ LWTUNNEL_IP_DST, "dst", DT_INADDR, 0, NULL },
	{ LWTUNNEL_IP_SRC, "src", DT_INADDR, 0, NULL },
	{ LWTUNNEL_IP_TOS, "tos", DT_U8, 0, NULL },
	{ LWTUNNEL_IP_TTL, "ttl", DT_U8, 0, NULL },
	{ LWTUNNEL_IP_OPTS, "opts", DT_IPOPTS, 0, NULL },
	{ LWTUNNEL_IP_FLAGS, "flags", DT_U16, 0, NULL },
};

static const uc_nl_attr_spec_t route_encap_ila_attrs[] = {
	{ ILA_ATTR_LOCATOR, "locator", DT_U64ADDR, 0, NULL },
	{ ILA_ATTR_CSUM_MODE, "csum_mode", DT_U8, 0, NULL },
	{ ILA_ATTR_IDENT_TYPE, "ident_type", DT_U8, 0, NULL },
	{ ILA_ATTR_HOOK_TYPE, "hook_type", DT_U8, 0, NULL },
};

static const uc_nl_attr_spec_t route_encap_ip6_attrs[] = {
	{ LWTUNNEL_IP6_ID, "id", DT_U64, DF_BYTESWAP, NULL },
	{ LWTUNNEL_IP6_DST, "dst", DT_IN6ADDR, 0, NULL },
	{ LWTUNNEL_IP6_SRC, "src", DT_IN6ADDR, 0, NULL },
	{ LWTUNNEL_IP6_TC, "tc", DT_U32, 0, NULL },
	{ LWTUNNEL_IP6_HOPLIMIT, "hoplimit", DT_U8, 0, NULL },
	{ LWTUNNEL_IP6_OPTS, "opts", DT_IPOPTS, 0, NULL },
	{ LWTUNNEL_IP6_FLAGS, "flags", DT_U16, 0, NULL },
};

static const uc_nl_attr_spec_t route_encap_seg6_attrs[] = {
	{ SEG6_IPTUNNEL_SRH, "srh", DT_SRH, 0, NULL },
};

#define IPV4_DEVCONF_ENTRY(name) ((void *)((IPV4_DEVCONF_##name - 1) * sizeof(uint32_t)))

static const uc_nl_nested_spec_t link_attrs_af_spec_inet_devconf_rta = {
	.headsize = NLA_ALIGN(IPV4_DEVCONF_MAX * sizeof(uint32_t)),
	.nattrs = 32,
	.attrs = {
		{ 0, "forwarding", DT_U32, 0, IPV4_DEVCONF_ENTRY(FORWARDING) },
		{ 0, "mc_forwarding", DT_U32, 0, IPV4_DEVCONF_ENTRY(MC_FORWARDING) },
		{ 0, "proxy_arp", DT_U32, 0, IPV4_DEVCONF_ENTRY(PROXY_ARP) },
		{ 0, "accept_redirects", DT_U32, 0, IPV4_DEVCONF_ENTRY(ACCEPT_REDIRECTS) },
		{ 0, "secure_redirects", DT_U32, 0, IPV4_DEVCONF_ENTRY(SECURE_REDIRECTS) },
		{ 0, "send_redirects", DT_U32, 0, IPV4_DEVCONF_ENTRY(SEND_REDIRECTS) },
		{ 0, "shared_media", DT_U32, 0, IPV4_DEVCONF_ENTRY(SHARED_MEDIA) },
		{ 0, "rp_filter", DT_U32, 0, IPV4_DEVCONF_ENTRY(RP_FILTER) },
		{ 0, "accept_source_route", DT_U32, 0, IPV4_DEVCONF_ENTRY(ACCEPT_SOURCE_ROUTE) },
		{ 0, "bootp_relay", DT_U32, 0, IPV4_DEVCONF_ENTRY(BOOTP_RELAY) },
		{ 0, "log_martians", DT_U32, 0, IPV4_DEVCONF_ENTRY(LOG_MARTIANS) },
		{ 0, "tag", DT_U32, 0, IPV4_DEVCONF_ENTRY(TAG) },
		{ 0, "arpfilter", DT_U32, 0, IPV4_DEVCONF_ENTRY(ARPFILTER) },
		{ 0, "medium_id", DT_U32, 0, IPV4_DEVCONF_ENTRY(MEDIUM_ID) },
		{ 0, "noxfrm", DT_U32, 0, IPV4_DEVCONF_ENTRY(NOXFRM) },
		{ 0, "nopolicy", DT_U32, 0, IPV4_DEVCONF_ENTRY(NOPOLICY) },
		{ 0, "force_igmp_version", DT_U32, 0, IPV4_DEVCONF_ENTRY(FORCE_IGMP_VERSION) },
		{ 0, "arp_announce", DT_U32, 0, IPV4_DEVCONF_ENTRY(ARP_ANNOUNCE) },
		{ 0, "arp_ignore", DT_U32, 0, IPV4_DEVCONF_ENTRY(ARP_IGNORE) },
		{ 0, "promote_secondaries", DT_U32, 0, IPV4_DEVCONF_ENTRY(PROMOTE_SECONDARIES) },
		{ 0, "arp_accept", DT_U32, 0, IPV4_DEVCONF_ENTRY(ARP_ACCEPT) },
		{ 0, "arp_notify", DT_U32, 0, IPV4_DEVCONF_ENTRY(ARP_NOTIFY) },
		{ 0, "accept_local", DT_U32, 0, IPV4_DEVCONF_ENTRY(ACCEPT_LOCAL) },
		{ 0, "src_vmark", DT_U32, 0, IPV4_DEVCONF_ENTRY(SRC_VMARK) },
		{ 0, "proxy_arp_pvlan", DT_U32, 0, IPV4_DEVCONF_ENTRY(PROXY_ARP_PVLAN) },
		{ 0, "route_localnet", DT_U32, 0, IPV4_DEVCONF_ENTRY(ROUTE_LOCALNET) },
		{ 0, "igmpv2_unsolicited_report_interval", DT_U32, 0, IPV4_DEVCONF_ENTRY(IGMPV2_UNSOLICITED_REPORT_INTERVAL) },
		{ 0, "igmpv3_unsolicited_report_interval", DT_U32, 0, IPV4_DEVCONF_ENTRY(IGMPV3_UNSOLICITED_REPORT_INTERVAL) },
		{ 0, "ignore_routes_with_linkdown", DT_U32, 0, IPV4_DEVCONF_ENTRY(IGNORE_ROUTES_WITH_LINKDOWN) },
		{ 0, "drop_unicast_in_l2_multicast", DT_U32, 0, IPV4_DEVCONF_ENTRY(DROP_UNICAST_IN_L2_MULTICAST) },
		{ 0, "drop_gratuitous_arp", DT_U32, 0, IPV4_DEVCONF_ENTRY(DROP_GRATUITOUS_ARP) },
		{ 0, "bc_forwarding", DT_U32, 0, IPV4_DEVCONF_ENTRY(BC_FORWARDING) },
	}
};

static const uc_nl_nested_spec_t link_attrs_af_spec_inet_rta = {
	.headsize = 0,
	.nattrs = 1,
	.attrs = {
		{ IFLA_INET_CONF, "conf", DT_NESTED, 0, &link_attrs_af_spec_inet_devconf_rta },
	}
};

#define IPV6_DEVCONF_ENTRY(name) ((void *)(DEVCONF_##name * sizeof(uint32_t)))

static const uc_nl_nested_spec_t link_attrs_af_spec_inet6_devconf_rta = {
	.headsize = NLA_ALIGN(DEVCONF_MAX * sizeof(uint32_t)),
	.nattrs = 53,
	.attrs = {
		{ 0, "forwarding", DT_S32, 0, IPV6_DEVCONF_ENTRY(FORWARDING) },
		{ 0, "hoplimit", DT_S32, 0, IPV6_DEVCONF_ENTRY(HOPLIMIT) },
		{ 0, "mtu6", DT_S32, 0, IPV6_DEVCONF_ENTRY(MTU6) },
		{ 0, "accept_ra", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA) },
		{ 0, "accept_redirects", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_REDIRECTS) },
		{ 0, "autoconf", DT_S32, 0, IPV6_DEVCONF_ENTRY(AUTOCONF) },
		{ 0, "dad_transmits", DT_S32, 0, IPV6_DEVCONF_ENTRY(DAD_TRANSMITS) },
		{ 0, "rtr_solicits", DT_S32, 0, IPV6_DEVCONF_ENTRY(RTR_SOLICITS) },
		{ 0, "rtr_solicit_interval", DT_S32, 0, IPV6_DEVCONF_ENTRY(RTR_SOLICIT_INTERVAL) },
		{ 0, "rtr_solicit_delay", DT_S32, 0, IPV6_DEVCONF_ENTRY(RTR_SOLICIT_DELAY) },
		{ 0, "use_tempaddr", DT_S32, 0, IPV6_DEVCONF_ENTRY(USE_TEMPADDR) },
		{ 0, "temp_valid_lft", DT_S32, 0, IPV6_DEVCONF_ENTRY(TEMP_VALID_LFT) },
		{ 0, "temp_prefered_lft", DT_S32, 0, IPV6_DEVCONF_ENTRY(TEMP_PREFERED_LFT) },
		{ 0, "regen_max_retry", DT_S32, 0, IPV6_DEVCONF_ENTRY(REGEN_MAX_RETRY) },
		{ 0, "max_desync_factor", DT_S32, 0, IPV6_DEVCONF_ENTRY(MAX_DESYNC_FACTOR) },
		{ 0, "max_addresses", DT_S32, 0, IPV6_DEVCONF_ENTRY(MAX_ADDRESSES) },
		{ 0, "force_mld_version", DT_S32, 0, IPV6_DEVCONF_ENTRY(FORCE_MLD_VERSION) },
		{ 0, "accept_ra_defrtr", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_DEFRTR) },
		{ 0, "accept_ra_pinfo", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_PINFO) },
		{ 0, "accept_ra_rtr_pref", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_RTR_PREF) },
		{ 0, "rtr_probe_interval", DT_S32, 0, IPV6_DEVCONF_ENTRY(RTR_PROBE_INTERVAL) },
		{ 0, "accept_ra_rt_info_max_plen", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_RT_INFO_MAX_PLEN) },
		{ 0, "proxy_ndp", DT_S32, 0, IPV6_DEVCONF_ENTRY(PROXY_NDP) },
		{ 0, "optimistic_dad", DT_S32, 0, IPV6_DEVCONF_ENTRY(OPTIMISTIC_DAD) },
		{ 0, "accept_source_route", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_SOURCE_ROUTE) },
		{ 0, "mc_forwarding", DT_S32, 0, IPV6_DEVCONF_ENTRY(MC_FORWARDING) },
		{ 0, "disable_ipv6", DT_S32, 0, IPV6_DEVCONF_ENTRY(DISABLE_IPV6) },
		{ 0, "accept_dad", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_DAD) },
		{ 0, "force_tllao", DT_S32, 0, IPV6_DEVCONF_ENTRY(FORCE_TLLAO) },
		{ 0, "ndisc_notify", DT_S32, 0, IPV6_DEVCONF_ENTRY(NDISC_NOTIFY) },
		{ 0, "mldv1_unsolicited_report_interval", DT_S32, 0, IPV6_DEVCONF_ENTRY(MLDV1_UNSOLICITED_REPORT_INTERVAL) },
		{ 0, "mldv2_unsolicited_report_interval", DT_S32, 0, IPV6_DEVCONF_ENTRY(MLDV2_UNSOLICITED_REPORT_INTERVAL) },
		{ 0, "suppress_frag_ndisc", DT_S32, 0, IPV6_DEVCONF_ENTRY(SUPPRESS_FRAG_NDISC) },
		{ 0, "accept_ra_from_local", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_FROM_LOCAL) },
		{ 0, "use_optimistic", DT_S32, 0, IPV6_DEVCONF_ENTRY(USE_OPTIMISTIC) },
		{ 0, "accept_ra_mtu", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_MTU) },
		{ 0, "stable_secret", DT_S32, 0, IPV6_DEVCONF_ENTRY(STABLE_SECRET) },
		{ 0, "use_oif_addrs_only", DT_S32, 0, IPV6_DEVCONF_ENTRY(USE_OIF_ADDRS_ONLY) },
		{ 0, "accept_ra_min_hop_limit", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_MIN_HOP_LIMIT) },
		{ 0, "ignore_routes_with_linkdown", DT_S32, 0, IPV6_DEVCONF_ENTRY(IGNORE_ROUTES_WITH_LINKDOWN) },
		{ 0, "drop_unicast_in_l2_multicast", DT_S32, 0, IPV6_DEVCONF_ENTRY(DROP_UNICAST_IN_L2_MULTICAST) },
		{ 0, "drop_unsolicited_na", DT_S32, 0, IPV6_DEVCONF_ENTRY(DROP_UNSOLICITED_NA) },
		{ 0, "keep_addr_on_down", DT_S32, 0, IPV6_DEVCONF_ENTRY(KEEP_ADDR_ON_DOWN) },
		{ 0, "rtr_solicit_max_interval", DT_S32, 0, IPV6_DEVCONF_ENTRY(RTR_SOLICIT_MAX_INTERVAL) },
		{ 0, "seg6_enabled", DT_S32, 0, IPV6_DEVCONF_ENTRY(SEG6_ENABLED) },
		{ 0, "seg6_require_hmac", DT_S32, 0, IPV6_DEVCONF_ENTRY(SEG6_REQUIRE_HMAC) },
		{ 0, "enhanced_dad", DT_S32, 0, IPV6_DEVCONF_ENTRY(ENHANCED_DAD) },
		{ 0, "addr_gen_mode", DT_S32, 0, IPV6_DEVCONF_ENTRY(ADDR_GEN_MODE) },
		{ 0, "disable_policy", DT_S32, 0, IPV6_DEVCONF_ENTRY(DISABLE_POLICY) },
		{ 0, "accept_ra_rt_info_min_plen", DT_S32, 0, IPV6_DEVCONF_ENTRY(ACCEPT_RA_RT_INFO_MIN_PLEN) },
		{ 0, "ndisc_tclass", DT_S32, 0, IPV6_DEVCONF_ENTRY(NDISC_TCLASS) },
		{ 0, "rpl_seg_enabled", DT_S32, 0, IPV6_DEVCONF_ENTRY(RPL_SEG_ENABLED) },
		{ 0, "ra_defrtr_metric", DT_S32, 0, IPV6_DEVCONF_ENTRY(RA_DEFRTR_METRIC) },
	}
};

static const uc_nl_nested_spec_t link_attrs_af_spec_inet6_rta = {
	.headsize = 0,
	.nattrs = 3,
	.attrs = {
		{ IFLA_INET6_ADDR_GEN_MODE, "mode", DT_U8, 0, NULL },
		{ IFLA_INET6_FLAGS, "flags", DT_U32, DF_NO_SET, NULL },
		{ IFLA_INET6_CONF, "conf", DT_NESTED, DF_NO_SET, &link_attrs_af_spec_inet6_devconf_rta },
	}
};

static const uc_nl_nested_spec_t link_attrs_af_spec_rta = {
	.headsize = 0,
	.nattrs = 2,
	.attrs = {
		{ AF_INET, "inet", DT_NESTED, DF_NO_SET, &link_attrs_af_spec_inet_rta },
		{ AF_INET6, "inet6", DT_NESTED, 0, &link_attrs_af_spec_inet6_rta },
	}
};

static const uc_nl_nested_spec_t link_attrs_stats64_rta = {
	.headsize = NLA_ALIGN(sizeof(struct rtnl_link_stats64)),
	.nattrs = 24,
	.attrs = {
		{ IFLA_UNSPEC, "rx_packets", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_packets) },
		{ IFLA_UNSPEC, "tx_packets", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_packets) },
		{ IFLA_UNSPEC, "rx_bytes", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_bytes) },
		{ IFLA_UNSPEC, "tx_bytes", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_bytes) },
		{ IFLA_UNSPEC, "rx_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_errors) },
		{ IFLA_UNSPEC, "tx_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_errors) },
		{ IFLA_UNSPEC, "rx_dropped", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_dropped) },
		{ IFLA_UNSPEC, "tx_dropped", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_dropped) },
		{ IFLA_UNSPEC, "multicast", DT_U64, 0, MEMBER(rtnl_link_stats64, multicast) },
		{ IFLA_UNSPEC, "collisions", DT_U64, 0, MEMBER(rtnl_link_stats64, collisions) },
		{ IFLA_UNSPEC, "rx_length_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_length_errors) },
		{ IFLA_UNSPEC, "rx_over_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_over_errors) },
		{ IFLA_UNSPEC, "rx_crc_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_crc_errors) },
		{ IFLA_UNSPEC, "rx_frame_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_frame_errors) },
		{ IFLA_UNSPEC, "rx_fifo_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_fifo_errors) },
		{ IFLA_UNSPEC, "rx_missed_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_missed_errors) },
		{ IFLA_UNSPEC, "tx_aborted_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_aborted_errors) },
		{ IFLA_UNSPEC, "tx_carrier_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_carrier_errors) },
		{ IFLA_UNSPEC, "tx_fifo_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_fifo_errors) },
		{ IFLA_UNSPEC, "tx_heartbeat_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_heartbeat_errors) },
		{ IFLA_UNSPEC, "tx_window_errors", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_window_errors) },
		{ IFLA_UNSPEC, "rx_compressed", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_compressed) },
		{ IFLA_UNSPEC, "tx_compressed", DT_U64, 0, MEMBER(rtnl_link_stats64, tx_compressed) },
		{ IFLA_UNSPEC, "rx_nohandler", DT_U64, 0, MEMBER(rtnl_link_stats64, rx_nohandler) },
	}
};

static const uc_nl_nested_spec_t link_msg = {
	.headsize = NLA_ALIGN(sizeof(struct ifinfomsg)),
	.nattrs = 26,
	.attrs = {
		{ IFLA_UNSPEC, "family", DT_U8, 0, MEMBER(ifinfomsg, ifi_family) },
		{ IFLA_UNSPEC, "type", DT_U16, 0, MEMBER(ifinfomsg, ifi_type) },
		{ IFLA_UNSPEC, "dev", DT_NETDEV, 0, MEMBER(ifinfomsg, ifi_index) },
		{ IFLA_UNSPEC, "flags", DT_FLAGS, 0, MEMBER(ifinfomsg, ifi_flags) },
		{ IFLA_UNSPEC, "change", DT_FLAGS, 0, MEMBER(ifinfomsg, ifi_change) },
		{ IFLA_ADDRESS, "address", DT_LLADDR, 0, NULL },
		{ IFLA_BROADCAST, "broadcast", DT_LLADDR, 0, NULL },
		{ IFLA_TXQLEN, "txqlen", DT_U32, 0, NULL },
		{ IFLA_MTU, "mtu", DT_U32, 0, NULL },
		{ IFLA_CARRIER, "carrier", DT_BOOL, 0, NULL },
		{ IFLA_MASTER, "master", DT_NETDEV, DF_ALLOW_NONE, NULL },
		{ IFLA_IFALIAS, "ifalias", DT_STRING, 0, NULL },
		{ IFLA_LINKMODE, "linkmode", DT_U8, 0, NULL },
		{ IFLA_OPERSTATE, "operstate", DT_U8, 0, NULL },
		{ IFLA_NUM_TX_QUEUES, "num_tx_queues", DT_U32, 0, NULL },
		{ IFLA_NUM_RX_QUEUES, "num_rx_queues", DT_U32, 0, NULL },
		{ IFLA_AF_SPEC, "af_spec", DT_AFSPEC, 0, NULL },
		{ IFLA_LINK_NETNSID, "link_netnsid", DT_U32, 0, NULL },
		{ IFLA_TARGET_NETNSID, "target_netnsid", DT_S32, 0, NULL },
		{ IFLA_PROTO_DOWN, "proto_down", DT_BOOL, 0, NULL },
		{ IFLA_GROUP, "group", DT_U32, 0, NULL },
		{ IFLA_LINK, "link", DT_NETDEV, 0, NULL },
		{ IFLA_IFNAME, "ifname", DT_STRING, 0, NULL },
		{ IFLA_LINKINFO, "linkinfo", DT_LINKINFO, 0, NULL }, /* XXX: DF_NO_GET ? */
		{ IFLA_EXT_MASK, "ext_mask", DT_U32, 0, NULL },
		{ IFLA_STATS64, "stats64", DT_NESTED, DF_NO_SET, &link_attrs_stats64_rta },
		/* TODO: IFLA_VFINFO_LIST */
		/* TODO: the following two should be straightforward, just uncomment and test */
		/* { IFLA_NET_NS_PID, "net_ns_pid", DT_S32, 0, NULL }, */
		/* { IFLA_NET_NS_FD, "net_ns_fd", DT_S32, 0, NULL }, */
	}
};

static const uc_nl_attr_spec_t link_bareudp_attrs[] = {
	{ IFLA_BAREUDP_ETHERTYPE, "ethertype", DT_U16, 0, NULL },
	{ IFLA_BAREUDP_MULTIPROTO_MODE, "multiproto_mode", DT_FLAG, 0, NULL },
	{ IFLA_BAREUDP_PORT, "port", DT_U16, 0, NULL },
	{ IFLA_BAREUDP_SRCPORT_MIN, "srcport_min", DT_U16, 0, NULL },
};

static const uc_nl_nested_spec_t link_bond_ad_info_rta = {
	.headsize = 0,
	.nattrs = 5,
	.attrs = {
		{ IFLA_BOND_AD_INFO_ACTOR_KEY, "ad_info_actor_key", DT_U16, DF_NO_SET, NULL },
		{ IFLA_BOND_AD_INFO_AGGREGATOR, "ad_info_aggregator", DT_U16, DF_NO_SET, NULL },
		{ IFLA_BOND_AD_INFO_NUM_PORTS, "ad_info_num_ports", DT_U16, DF_NO_SET, NULL },
		{ IFLA_BOND_AD_INFO_PARTNER_KEY, "ad_info_partner_key", DT_U16, DF_NO_SET, NULL },
		{ IFLA_BOND_AD_INFO_PARTNER_MAC, "ad_info_partner_mac", DT_LLADDR, DF_NO_SET, NULL },
	}
};

static const uc_nl_attr_spec_t link_bond_attrs[] = {
	{ IFLA_BOND_ACTIVE_SLAVE, "active_slave", DT_NETDEV, DF_ALLOW_NONE, NULL },
	{ IFLA_BOND_AD_ACTOR_SYSTEM, "ad_actor_system", DT_LLADDR, 0, NULL },
	{ IFLA_BOND_AD_ACTOR_SYS_PRIO, "ad_actor_sys_prio", DT_U16, 0, NULL },
	{ IFLA_BOND_AD_INFO, "ad_info", DT_NESTED, DF_NO_SET, &link_bond_ad_info_rta },
	{ IFLA_BOND_AD_LACP_RATE, "ad_lacp_rate", DT_U8, 0, NULL },
	{ IFLA_BOND_AD_SELECT, "ad_select", DT_U8, 0, NULL },
	{ IFLA_BOND_AD_USER_PORT_KEY, "ad_user_port_key", DT_U16, 0, NULL },
	{ IFLA_BOND_ALL_SLAVES_ACTIVE, "all_slaves_active", DT_U8, 0, NULL },
	{ IFLA_BOND_ARP_ALL_TARGETS, "arp_all_targets", DT_U32, 0, NULL },
	{ IFLA_BOND_ARP_INTERVAL, "arp_interval", DT_U32, 0, NULL },
	{ IFLA_BOND_ARP_IP_TARGET, "arp_ip_target", DT_INADDR, DF_MULTIPLE, NULL },
	{ IFLA_BOND_ARP_VALIDATE, "arp_validate", DT_U32, 0, NULL },
	{ IFLA_BOND_DOWNDELAY, "downdelay", DT_U32, 0, NULL },
	{ IFLA_BOND_FAIL_OVER_MAC, "fail_over_mac", DT_U8, 0, NULL },
	{ IFLA_BOND_LP_INTERVAL, "lp_interval", DT_U32, 0, NULL },
	{ IFLA_BOND_MIIMON, "miimon", DT_U32, 0, NULL },
	{ IFLA_BOND_MIN_LINKS, "min_links", DT_U32, 0, NULL },
	{ IFLA_BOND_MODE, "mode", DT_U8, 0, NULL },
	{ IFLA_BOND_NUM_PEER_NOTIF, "num_peer_notif", DT_U8, 0, NULL },
	{ IFLA_BOND_PACKETS_PER_SLAVE, "packets_per_slave", DT_U32, 0, NULL },
	{ IFLA_BOND_PRIMARY, "primary", DT_NETDEV, 0, NULL },
	{ IFLA_BOND_PRIMARY_RESELECT, "primary_reselect", DT_U8, 0, NULL },
	{ IFLA_BOND_RESEND_IGMP, "resend_igmp", DT_U32, 0, NULL },
	{ IFLA_BOND_TLB_DYNAMIC_LB, "tlb_dynamic_lb", DT_U8, 0, NULL },
	{ IFLA_BOND_UPDELAY, "updelay", DT_U32, 0, NULL },
	{ IFLA_BOND_USE_CARRIER, "use_carrier", DT_U8, 0, NULL },
	{ IFLA_BOND_XMIT_HASH_POLICY, "xmit_hash_policy", DT_U8, 0, NULL },
};

static const uc_nl_attr_spec_t link_bond_slave_attrs[] = {
	{ IFLA_BOND_SLAVE_AD_ACTOR_OPER_PORT_STATE, "ad_actor_oper_port_state", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BOND_SLAVE_AD_AGGREGATOR_ID, "ad_aggregator_id", DT_U16, DF_NO_SET, NULL },
	{ IFLA_BOND_SLAVE_AD_PARTNER_OPER_PORT_STATE, "ad_partner_oper_port_state", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BOND_SLAVE_LINK_FAILURE_COUNT, "link_failure_count", DT_U32, DF_NO_SET, NULL },
	{ IFLA_BOND_SLAVE_MII_STATUS, "mii_status", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BOND_SLAVE_PERM_HWADDR, "perm_hwaddr", DT_LLADDR, DF_NO_SET, NULL },
	{ IFLA_BOND_SLAVE_QUEUE_ID, "queue_id", DT_U16, 0, NULL },
	{ IFLA_BOND_SLAVE_STATE, "state", DT_U8, DF_NO_SET, NULL },
};

static const uc_nl_attr_spec_t link_bridge_attrs[] = {
	{ IFLA_BR_AGEING_TIME, "ageing_time", DT_U32, 0, NULL },
	{ IFLA_BR_BRIDGE_ID, "bridge_id", DT_BRIDGEID, DF_NO_SET, NULL },
	{ IFLA_BR_FDB_FLUSH, "fdb_flush", DT_FLAG, DF_NO_GET, NULL },
	{ IFLA_BR_FORWARD_DELAY, "forward_delay", DT_U32, 0, NULL },
	{ IFLA_BR_GC_TIMER, "gc_timer", DT_U64, DF_NO_SET, NULL },
	{ IFLA_BR_GROUP_ADDR, "group_addr", DT_LLADDR, 0, NULL },
	{ IFLA_BR_GROUP_FWD_MASK, "group_fwd_mask", DT_U16, 0, NULL },
	{ IFLA_BR_HELLO_TIME, "hello_time", DT_U32, 0, NULL },
	{ IFLA_BR_HELLO_TIMER, "hello_timer", DT_U64, DF_NO_SET, NULL },
	{ IFLA_BR_MAX_AGE, "max_age", DT_U32, 0, NULL },
	{ IFLA_BR_MCAST_HASH_ELASTICITY, "mcast_hash_elasticity", DT_U32, 0, NULL },
	{ IFLA_BR_MCAST_HASH_MAX, "mcast_hash_max", DT_U32, 0, NULL },
	{ IFLA_BR_MCAST_IGMP_VERSION, "mcast_igmp_version", DT_U8, 0, NULL },
	{ IFLA_BR_MCAST_LAST_MEMBER_CNT, "mcast_last_member_cnt", DT_U32, 0, NULL },
	{ IFLA_BR_MCAST_LAST_MEMBER_INTVL, "mcast_last_member_intvl", DT_U64, 0, NULL },
	{ IFLA_BR_MCAST_MEMBERSHIP_INTVL, "mcast_membership_intvl", DT_U64, 0, NULL },
	{ IFLA_BR_MCAST_MLD_VERSION, "mcast_mld_version", DT_U8, 0, NULL },
	{ IFLA_BR_MCAST_QUERIER, "mcast_querier", DT_U8, 0, NULL },
	{ IFLA_BR_MCAST_QUERIER_INTVL, "mcast_querier_intvl", DT_U64, 0, NULL },
	{ IFLA_BR_MCAST_QUERY_INTVL, "mcast_query_intvl", DT_U64, 0, NULL },
	{ IFLA_BR_MCAST_QUERY_RESPONSE_INTVL, "mcast_query_response_intvl", DT_U64, 0, NULL },
	{ IFLA_BR_MCAST_QUERY_USE_IFADDR, "mcast_query_use_ifaddr", DT_U8, 0, NULL },
	{ IFLA_BR_MCAST_ROUTER, "mcast_router", DT_U8, 0, NULL },
	{ IFLA_BR_MCAST_SNOOPING, "mcast_snooping", DT_U8, 0, NULL },
	{ IFLA_BR_MCAST_STARTUP_QUERY_CNT, "mcast_startup_query_cnt", DT_U32, 0, NULL },
	{ IFLA_BR_MCAST_STARTUP_QUERY_INTVL, "mcast_startup_query_intvl", DT_U64, 0, NULL },
	{ IFLA_BR_MCAST_STATS_ENABLED, "mcast_stats_enabled", DT_U8, 0, NULL },
	{ IFLA_BR_NF_CALL_ARPTABLES, "nf_call_arptables", DT_U8, 0, NULL },
	{ IFLA_BR_NF_CALL_IP6TABLES, "nf_call_ip6tables", DT_U8, 0, NULL },
	{ IFLA_BR_NF_CALL_IPTABLES, "nf_call_iptables", DT_U8, 0, NULL },
	{ IFLA_BR_PRIORITY, "priority", DT_U16, 0, NULL },
	{ IFLA_BR_ROOT_ID, "root_id", DT_BRIDGEID, DF_NO_SET, NULL },
	{ IFLA_BR_ROOT_PATH_COST, "root_path_cost", DT_U32, DF_NO_SET, NULL },
	{ IFLA_BR_ROOT_PORT, "root_port", DT_U16, DF_NO_SET, NULL },
	{ IFLA_BR_STP_STATE, "stp_state", DT_U32, 0, NULL },
	{ IFLA_BR_TCN_TIMER, "tcn_timer", DT_U64, DF_NO_SET, NULL },
	{ IFLA_BR_TOPOLOGY_CHANGE, "topology_change", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BR_TOPOLOGY_CHANGE_DETECTED, "topology_change_detected", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BR_TOPOLOGY_CHANGE_TIMER, "topology_change_timer", DT_U64, DF_NO_SET, NULL },
	{ IFLA_BR_VLAN_DEFAULT_PVID, "vlan_default_pvid", DT_U16, 0, NULL },
	{ IFLA_BR_VLAN_FILTERING, "vlan_filtering", DT_U8, 0, NULL },
	{ IFLA_BR_VLAN_PROTOCOL, "vlan_protocol", DT_U16, 0, NULL },
	{ IFLA_BR_VLAN_STATS_ENABLED, "vlan_stats_enabled", DT_U8, 0, NULL },
};

static const uc_nl_attr_spec_t link_bridge_slave_attrs[] = {
	{ IFLA_BRPORT_BACKUP_PORT, "backup_port", DT_NETDEV, 0, NULL },
	//{ IFLA_BRPORT_BCAST_FLOOD, "bcast-flood", DT_??, 0, NULL },
	{ IFLA_BRPORT_BRIDGE_ID, "bridge_id", DT_BRIDGEID, DF_NO_SET, NULL },
	{ IFLA_BRPORT_CONFIG_PENDING, "config_pending", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BRPORT_COST, "cost", DT_U32, 0, NULL },
	{ IFLA_BRPORT_DESIGNATED_COST, "designated_cost", DT_U16, DF_NO_SET, NULL },
	{ IFLA_BRPORT_DESIGNATED_PORT, "designated_port", DT_U16, DF_NO_SET, NULL },
	{ IFLA_BRPORT_FAST_LEAVE, "fast_leave", DT_U8, 0, NULL },
	{ IFLA_BRPORT_FLUSH, "flush", DT_FLAG, DF_NO_GET, NULL },
	{ IFLA_BRPORT_FORWARD_DELAY_TIMER, "forward_delay_timer", DT_U64, DF_NO_SET, NULL },
	{ IFLA_BRPORT_GROUP_FWD_MASK, "group_fwd_mask", DT_U16, 0, NULL },
	{ IFLA_BRPORT_GUARD, "guard", DT_U8, 0, NULL },
	{ IFLA_BRPORT_HOLD_TIMER, "hold_timer", DT_U64, DF_NO_SET, NULL },
	{ IFLA_BRPORT_ID, "id", DT_U16, DF_NO_SET, NULL },
	{ IFLA_BRPORT_ISOLATED, "isolated", DT_U8, 0, NULL },
	{ IFLA_BRPORT_LEARNING, "learning", DT_U8, 0, NULL },
	{ IFLA_BRPORT_LEARNING_SYNC, "learning_sync", DT_U8, 0, NULL },
	{ IFLA_BRPORT_MCAST_FLOOD, "mcast_flood", DT_U8, 0, NULL },
	{ IFLA_BRPORT_MCAST_TO_UCAST, "mcast_to_ucast", DT_U8, 0, NULL },
	{ IFLA_BRPORT_MESSAGE_AGE_TIMER, "message_age_timer", DT_U64, DF_NO_SET, NULL },
	{ IFLA_BRPORT_MODE, "mode", DT_U8, 0, NULL },
	{ IFLA_BRPORT_MULTICAST_ROUTER, "multicast_router", DT_U8, 0, NULL },
	{ IFLA_BRPORT_NEIGH_SUPPRESS, "neigh_suppress", DT_U8, 0, NULL },
	{ IFLA_BRPORT_NO, "no", DT_U16, DF_NO_SET, NULL },
	{ IFLA_BRPORT_PRIORITY, "priority", DT_U16, 0, NULL },
	{ IFLA_BRPORT_PROTECT, "protect", DT_U8, 0, NULL },
	{ IFLA_BRPORT_PROXYARP, "proxyarp", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BRPORT_PROXYARP_WIFI, "proxyarp_wifi", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BRPORT_ROOT_ID, "root_id", DT_BRIDGEID, DF_NO_SET, NULL },
	{ IFLA_BRPORT_STATE, "state", DT_U8, 0, NULL },
	{ IFLA_BRPORT_TOPOLOGY_CHANGE_ACK, "topology_change_ack", DT_U8, DF_NO_SET, NULL },
	{ IFLA_BRPORT_UNICAST_FLOOD, "unicast_flood", DT_U8, 0, NULL },
	{ IFLA_BRPORT_VLAN_TUNNEL, "vlan_tunnel", DT_U8, 0, NULL },
};

static const uc_nl_nested_spec_t link_can_bittiming_rta = {
	.headsize = NLA_ALIGN(sizeof(struct can_bittiming)),
	.nattrs = 8,
	.attrs = {
		{ RTA_UNSPEC, "bitrate", DT_U32, 0, MEMBER(can_bittiming, bitrate) },
		{ RTA_UNSPEC, "sample_point", DT_U32, 0, MEMBER(can_bittiming, sample_point) },
		{ RTA_UNSPEC, "tq", DT_U32, 0, MEMBER(can_bittiming, tq) },
		{ RTA_UNSPEC, "prop_seg", DT_U32, 0, MEMBER(can_bittiming, prop_seg) },
		{ RTA_UNSPEC, "phase_seg1", DT_U32, 0, MEMBER(can_bittiming, phase_seg1) },
		{ RTA_UNSPEC, "phase_seg2", DT_U32, 0, MEMBER(can_bittiming, phase_seg2) },
		{ RTA_UNSPEC, "sjw", DT_U32, 0, MEMBER(can_bittiming, sjw) },
		{ RTA_UNSPEC, "brp", DT_U32, 0, MEMBER(can_bittiming, brp) },
	}
};

static const uc_nl_nested_spec_t link_can_bittiming_const_rta = {
	.headsize = NLA_ALIGN(sizeof(struct can_bittiming_const)),
	.nattrs = 8,
	.attrs = {
		{ RTA_UNSPEC, "tseg1_min", DT_U32, 0, MEMBER(can_bittiming_const, tseg1_min) },
		{ RTA_UNSPEC, "tseg1_max", DT_U32, 0, MEMBER(can_bittiming_const, tseg1_max) },
		{ RTA_UNSPEC, "tseg2_min", DT_U32, 0, MEMBER(can_bittiming_const, tseg2_min) },
		{ RTA_UNSPEC, "tseg2_max", DT_U32, 0, MEMBER(can_bittiming_const, tseg2_max) },
		{ RTA_UNSPEC, "sjw_max", DT_U32, 0, MEMBER(can_bittiming_const, sjw_max) },
		{ RTA_UNSPEC, "brp_min", DT_U32, 0, MEMBER(can_bittiming_const, brp_min) },
		{ RTA_UNSPEC, "brp_max", DT_U32, 0, MEMBER(can_bittiming_const, brp_max) },
		{ RTA_UNSPEC, "brp_inc", DT_U32, 0, MEMBER(can_bittiming_const, brp_inc) },
	}
};

static const uc_nl_nested_spec_t link_can_clock_rta = {
	.headsize = NLA_ALIGN(sizeof(struct can_clock)),
	.nattrs = 1,
	.attrs = {
		{ RTA_UNSPEC, "freq", DT_U32, 0, MEMBER(can_clock, freq) },
	}
};

static const uc_nl_nested_spec_t link_can_ctrlmode_rta = {
	.headsize = NLA_ALIGN(sizeof(struct can_ctrlmode)),
	.nattrs = 2,
	.attrs = {
		{ RTA_UNSPEC, "mask", DT_U32, 0, MEMBER(can_ctrlmode, mask) },
		{ RTA_UNSPEC, "flags", DT_U32, 0, MEMBER(can_ctrlmode, flags) },
	}
};

static const uc_nl_nested_spec_t link_can_berr_counter_rta = {
	.headsize = NLA_ALIGN(sizeof(struct can_berr_counter)),
	.nattrs = 2,
	.attrs = {
		{ RTA_UNSPEC, "txerr", DT_U16, 0, MEMBER(can_berr_counter, txerr) },
		{ RTA_UNSPEC, "rxerr", DT_U16, 0, MEMBER(can_berr_counter, rxerr) },
	}
};

static const uc_nl_attr_spec_t link_can_attrs[] = {
	{ IFLA_CAN_BERR_COUNTER, "berr_counter", DT_NESTED, DF_NO_SET, &link_can_berr_counter_rta },
	{ IFLA_CAN_BITTIMING, "bittiming", DT_NESTED, 0, &link_can_bittiming_rta },
	{ IFLA_CAN_BITTIMING_CONST, "bittiming_const", DT_NESTED, DF_NO_SET, &link_can_bittiming_const_rta },
	{ IFLA_CAN_CLOCK, "clock", DT_NESTED, DF_NO_SET, &link_can_clock_rta },
	{ IFLA_CAN_CTRLMODE, "ctrlmode", DT_NESTED, 0, &link_can_ctrlmode_rta },
	{ IFLA_CAN_DATA_BITTIMING, "data_bittiming", DT_NESTED, 0, &link_can_bittiming_rta },
	{ IFLA_CAN_DATA_BITTIMING_CONST, "data_bittiming_const", DT_NESTED, DF_NO_SET, &link_can_bittiming_const_rta },
	{ IFLA_CAN_RESTART, "restart", DT_U32, DF_NO_GET, NULL },
	{ IFLA_CAN_RESTART_MS, "restart_ms", DT_U32, 0, NULL },
	{ IFLA_CAN_STATE, "state", DT_U32, DF_NO_SET, NULL },
	{ IFLA_CAN_TERMINATION, "termination", DT_U16, 0, NULL },
};

static const uc_nl_attr_spec_t link_geneve_attrs[] = {
	{ IFLA_GENEVE_COLLECT_METADATA, "collect_metadata", DT_FLAG, DF_NO_GET, NULL },
	{ IFLA_GENEVE_ID, "id", DT_U32, 0, NULL },
	{ IFLA_GENEVE_LABEL, "label", DT_U32, 0, NULL },
	{ IFLA_GENEVE_PORT, "port", DT_U16, 0, NULL },
	{ IFLA_GENEVE_REMOTE, "remote", DT_INADDR, 0, NULL },
	{ IFLA_GENEVE_REMOTE6, "remote6", DT_IN6ADDR, 0, NULL },
	{ IFLA_GENEVE_TOS, "tos", DT_U8, 0, NULL },
	{ IFLA_GENEVE_TTL, "ttl", DT_U8, 0, NULL },
	{ IFLA_GENEVE_UDP_CSUM, "udp_csum", DT_U8, 0, NULL },
	{ IFLA_GENEVE_UDP_ZERO_CSUM6_RX, "udp_zero_csum6_rx", DT_U8, 0, NULL },
	{ IFLA_GENEVE_UDP_ZERO_CSUM6_TX, "udp_zero_csum6_tx", DT_U8, 0, NULL },
};

static const uc_nl_attr_spec_t link_hsr_attrs[] = {
	{ IFLA_HSR_MULTICAST_SPEC, "multicast_spec", DT_STRING, DF_NO_GET, NULL },
	{ IFLA_HSR_SEQ_NR, "seq_nr", DT_U16, DF_NO_SET, NULL },
	{ IFLA_HSR_SLAVE1, "slave1", DT_NETDEV, 0, NULL },
	{ IFLA_HSR_SLAVE2, "slave2", DT_NETDEV, 0, NULL },
	{ IFLA_HSR_SUPERVISION_ADDR, "supervision_addr", DT_LLADDR, DF_NO_SET, NULL },
	{ IFLA_HSR_VERSION, "version", DT_STRING, DF_NO_GET, NULL },
};

static const uc_nl_attr_spec_t link_ipoib_attrs[] = {
	{ IFLA_IPOIB_MODE, "mode", DT_U16, 0, NULL },
	{ IFLA_IPOIB_PKEY, "pkey", DT_U16, 0, NULL },
	{ IFLA_IPOIB_UMCAST, "umcast", DT_U16, 0, NULL },
};

static const uc_nl_attr_spec_t link_ipvlan_attrs[] = {
	{ IFLA_IPVLAN_FLAGS, "flags", DT_U16, 0, NULL },
	{ IFLA_IPVLAN_MODE, "mode", DT_U16, 0, NULL },
};

static const uc_nl_attr_spec_t link_macvlan_attrs[] = {
	{ IFLA_MACVLAN_FLAGS, "flags", DT_U16, 0, NULL },
	{ IFLA_MACVLAN_MACADDR, "macaddr", DT_LLADDR, DF_NO_GET, NULL },
	{ IFLA_MACVLAN_MACADDR_COUNT, "macaddr_count", DT_U32, DF_NO_SET, NULL },
	{ IFLA_MACVLAN_MACADDR_DATA, "macaddr_data", DT_LLADDR, DF_MULTIPLE, (void *)IFLA_MACVLAN_MACADDR },
	{ IFLA_MACVLAN_MACADDR_MODE, "macaddr_mode", DT_U32, DF_NO_GET, NULL },
	{ IFLA_MACVLAN_MODE, "mode", DT_U32, 0, NULL },
};

static const uc_nl_attr_spec_t link_rmnet_attrs[] = {
	//{ IFLA_RMNET_FLAGS, "flags", DT_??, 0, NULL },
	{ IFLA_RMNET_MUX_ID, "mux_id", DT_U16, 0, NULL },
};


static const uc_nl_attr_spec_t link_vlan_attrs[] = {
	{ IFLA_VLAN_EGRESS_QOS, "egress_qos_map", DT_NUMRANGE, DF_MULTIPLE, (void *)IFLA_VLAN_QOS_MAPPING },
	{ IFLA_VLAN_FLAGS, "flags", DT_FLAGS, 0, NULL },
	{ IFLA_VLAN_ID, "id", DT_U16, 0, NULL },
	{ IFLA_VLAN_INGRESS_QOS, "ingress_qos_map", DT_NUMRANGE, DF_MULTIPLE, (void *)IFLA_VLAN_QOS_MAPPING },
	{ IFLA_VLAN_PROTOCOL, "protocol", DT_U16, 0, NULL },
};

static const uc_nl_attr_spec_t link_vrf_attrs[] = {
	{ IFLA_VRF_PORT_TABLE, "port_table", DT_U32, DF_NO_SET, NULL },
	{ IFLA_VRF_TABLE, "table", DT_U32, 0, NULL },
};

static const uc_nl_attr_spec_t link_vxcan_attrs[] = {
	{ VXCAN_INFO_PEER, "info_peer", DT_NESTED, 0, &link_msg },
};

static const uc_nl_attr_spec_t link_vxlan_attrs[] = {
	{ IFLA_VXLAN_AGEING, "ageing", DT_U32, 0, NULL },
	{ IFLA_VXLAN_COLLECT_METADATA, "collect_metadata", DT_U8, 0, NULL },
	{ IFLA_VXLAN_GBP, "gbp", DT_FLAG, 0, NULL },
	{ IFLA_VXLAN_GPE, "gpe", DT_FLAG, 0, NULL },
	{ IFLA_VXLAN_GROUP, "group", DT_INADDR, 0, NULL },
	{ IFLA_VXLAN_GROUP6, "group6", DT_IN6ADDR, 0, NULL },
	{ IFLA_VXLAN_ID, "id", DT_U32, 0, NULL },
	{ IFLA_VXLAN_L2MISS, "l2miss", DT_U8, 0, NULL },
	{ IFLA_VXLAN_L3MISS, "l3miss", DT_U8, 0, NULL },
	{ IFLA_VXLAN_LABEL, "label", DT_U32, 0, NULL },
	{ IFLA_VXLAN_LEARNING, "learning", DT_U8, 0, NULL },
	{ IFLA_VXLAN_LIMIT, "limit", DT_U32, 0, NULL },
	{ IFLA_VXLAN_LINK, "link", DT_U32, 0, NULL },
	{ IFLA_VXLAN_LOCAL, "local", DT_INADDR, 0, NULL },
	{ IFLA_VXLAN_LOCAL6, "local6", DT_IN6ADDR, 0, NULL },
	{ IFLA_VXLAN_PORT, "port", DT_U16, DF_BYTESWAP, NULL },
	{ IFLA_VXLAN_PORT_RANGE, "port_range", DT_NUMRANGE, DF_MAX_65535|DF_BYTESWAP, NULL },
	{ IFLA_VXLAN_PROXY, "proxy", DT_U8, 0, NULL },
	//{ IFLA_VXLAN_REMCSUM_NOPARTIAL, "remcsum-nopartial", DT_??, 0, NULL },
	{ IFLA_VXLAN_REMCSUM_RX, "remcsum_rx", DT_BOOL, 0, NULL },
	{ IFLA_VXLAN_REMCSUM_TX, "remcsum_tx", DT_BOOL, 0, NULL },
	{ IFLA_VXLAN_RSC, "rsc", DT_BOOL, 0, NULL },
	{ IFLA_VXLAN_TOS, "tos", DT_U8, 0, NULL },
	{ IFLA_VXLAN_TTL, "ttl", DT_U8, 0, NULL },
	{ IFLA_VXLAN_TTL_INHERIT, "ttl_inherit", DT_FLAG, 0, NULL },
	{ IFLA_VXLAN_UDP_CSUM, "udp_csum", DT_BOOL, 0, NULL },
	{ IFLA_VXLAN_UDP_ZERO_CSUM6_RX, "udp_zero_csum6_rx", DT_BOOL, 0, NULL },
	{ IFLA_VXLAN_UDP_ZERO_CSUM6_TX, "udp_zero_csum6_tx", DT_BOOL, 0, NULL },
};

static const uc_nl_attr_spec_t link_gre_attrs[] = {
	{ IFLA_GRE_COLLECT_METADATA, "collect_metadata", DT_FLAG, 0, NULL },
	{ IFLA_GRE_ENCAP_DPORT, "encap_dport", DT_U16, DF_BYTESWAP, NULL },
	{ IFLA_GRE_ENCAP_FLAGS, "encap_flags", DT_U16, 0, NULL },
	{ IFLA_GRE_ENCAP_LIMIT, "encap_limit", DT_U8, 0, NULL },
	{ IFLA_GRE_ENCAP_SPORT, "encap_sport", DT_U16, DF_BYTESWAP, NULL },
	{ IFLA_GRE_ENCAP_TYPE, "encap_type", DT_U16, 0, NULL },
	{ IFLA_GRE_ERSPAN_DIR, "erspan_dir", DT_U8, 0, NULL },
	{ IFLA_GRE_ERSPAN_HWID, "erspan_hwid", DT_U16, 0, NULL },
	{ IFLA_GRE_ERSPAN_INDEX, "erspan_index", DT_U32, 0, NULL },
	{ IFLA_GRE_ERSPAN_VER, "erspan_ver", DT_U8, 0, NULL },
	{ IFLA_GRE_FLAGS, "flags", DT_U32, 0, NULL },
	{ IFLA_GRE_FLOWINFO, "flowinfo", DT_U32, DF_BYTESWAP, NULL },
	{ IFLA_GRE_FWMARK, "fwmark", DT_U32, 0, NULL },
	{ IFLA_GRE_IFLAGS, "iflags", DT_U16, 0, NULL },
	{ IFLA_GRE_IGNORE_DF, "ignore_df", DT_BOOL, 0, NULL },
	{ IFLA_GRE_IKEY, "ikey", DT_U32, 0, NULL },
	{ IFLA_GRE_LINK, "link", DT_NETDEV, 0, NULL },
	{ IFLA_GRE_LOCAL, "local", DT_ANYADDR, 0, NULL },
	{ IFLA_GRE_OFLAGS, "oflags", DT_U16, 0, NULL },
	{ IFLA_GRE_OKEY, "okey", DT_U32, 0, NULL },
	{ IFLA_GRE_PMTUDISC, "pmtudisc", DT_BOOL, 0, NULL },
	{ IFLA_GRE_REMOTE, "remote", DT_ANYADDR, 0, NULL },
	{ IFLA_GRE_TOS, "tos", DT_U8, 0, NULL },
	{ IFLA_GRE_TTL, "ttl", DT_U8, 0, NULL },
};

#define link_gretap_attrs link_gre_attrs
#define link_erspan_attrs link_gre_attrs
#define link_ip6gre_attrs link_gre_attrs
#define link_ip6gretap_attrs link_gre_attrs
#define link_ip6erspan_attrs link_gre_attrs

static const uc_nl_attr_spec_t link_ip6tnl_attrs[] = {
	{ IFLA_IPTUN_6RD_PREFIX, "6rd_prefix", DT_IN6ADDR, 0, NULL },
	{ IFLA_IPTUN_6RD_PREFIXLEN, "6rd_prefixlen", DT_U16, 0, NULL },
	{ IFLA_IPTUN_6RD_RELAY_PREFIX, "6rd_relay_prefix", DT_INADDR, 0, NULL },
	{ IFLA_IPTUN_6RD_RELAY_PREFIXLEN, "6rd_relay_prefixlen", DT_U16, 0, NULL },
	{ IFLA_IPTUN_COLLECT_METADATA, "collect_metadata", DT_BOOL, 0, NULL },
	{ IFLA_IPTUN_ENCAP_DPORT, "encap_dport", DT_U16, DF_BYTESWAP, NULL },
	{ IFLA_IPTUN_ENCAP_FLAGS, "encap_flags", DT_U16, 0, NULL },
	{ IFLA_IPTUN_ENCAP_LIMIT, "encap_limit", DT_U8, 0, NULL },
	{ IFLA_IPTUN_ENCAP_SPORT, "encap_sport", DT_U16, DF_BYTESWAP, NULL },
	{ IFLA_IPTUN_ENCAP_TYPE, "encap_type", DT_U16, 0, NULL },
	{ IFLA_IPTUN_FLAGS, "flags", DT_U16, 0, NULL },
	{ IFLA_IPTUN_FLOWINFO, "flowinfo", DT_U32, DF_BYTESWAP, NULL },
	{ IFLA_IPTUN_FWMARK, "fwmark", DT_U32, 0, NULL },
	{ IFLA_IPTUN_LINK, "link", DT_NETDEV, 0, NULL },
	{ IFLA_IPTUN_LOCAL, "local", DT_ANYADDR, 0, NULL },
	{ IFLA_IPTUN_PMTUDISC, "pmtudisc", DT_BOOL, 0, NULL },
	{ IFLA_IPTUN_PROTO, "proto", DT_U8, 0, NULL },
	{ IFLA_IPTUN_REMOTE, "remote", DT_ANYADDR, 0, NULL },
	{ IFLA_IPTUN_TOS, "tos", DT_U8, 0, NULL },
	{ IFLA_IPTUN_TTL, "ttl", DT_U8, 0, NULL },
};

#define link_ipip_attrs link_ip6tnl_attrs
#define link_sit_attrs link_ip6tnl_attrs

static const uc_nl_attr_spec_t link_veth_attrs[] = {
	{ VETH_INFO_PEER, "info_peer", DT_NESTED, 0, &link_msg },
};

static const uc_nl_attr_spec_t link_vti_attrs[] = {
	{ IFLA_VTI_FWMARK, "fwmark", DT_U32, 0, NULL },
	{ IFLA_VTI_IKEY, "ikey", DT_U32, 0, NULL },
	{ IFLA_VTI_LINK, "link", DT_U32, 0, NULL },
	{ IFLA_VTI_LOCAL, "local", DT_ANYADDR, 0, NULL },
	{ IFLA_VTI_OKEY, "okey", DT_U32, 0, NULL },
	{ IFLA_VTI_REMOTE, "remote", DT_ANYADDR, 0, NULL },
};

#define link_vti6_attrs link_vti_attrs

static const uc_nl_attr_spec_t link_xfrm_attrs[] = {
	{ IFLA_XFRM_IF_ID, "if_id", DT_U32, 0, NULL },
	{ IFLA_XFRM_LINK, "link", DT_NETDEV, 0, NULL },
};

static const uc_nl_attr_spec_t lwtipopt_erspan_attrs[] = {
	{ LWTUNNEL_IP_OPT_ERSPAN_VER, "ver", DT_U8, 0, NULL },
	{ LWTUNNEL_IP_OPT_ERSPAN_INDEX, "index", DT_U16, DF_BYTESWAP, NULL },
	{ LWTUNNEL_IP_OPT_ERSPAN_DIR, "dir", DT_U8, 0, NULL },
	{ LWTUNNEL_IP_OPT_ERSPAN_HWID, "hwid", DT_U8, 0, NULL },
};

static const uc_nl_attr_spec_t lwtipopt_geneve_attrs[] = {
	{ LWTUNNEL_IP_OPT_GENEVE_CLASS, "class", DT_U16, DF_BYTESWAP, NULL },
	{ LWTUNNEL_IP_OPT_GENEVE_TYPE, "type", DT_U8, 0, NULL },
	{ LWTUNNEL_IP_OPT_GENEVE_DATA, "data", DT_STRING, 0, NULL },
};

static const uc_nl_attr_spec_t lwtipopt_vxlan_attrs[] = {
	{ LWTUNNEL_IP_OPT_VXLAN_GBP, "gbp", DT_U32, 0, NULL },
};

static const uc_nl_nested_spec_t neigh_cacheinfo_rta = {
	.headsize = NLA_ALIGN(sizeof(struct nda_cacheinfo)),
	.nattrs = 4,
	.attrs = {
		{ NDA_UNSPEC, "confirmed", DT_U32, 0, MEMBER(nda_cacheinfo, ndm_confirmed) },
		{ NDA_UNSPEC, "used", DT_U32, 0, MEMBER(nda_cacheinfo, ndm_used) },
		{ NDA_UNSPEC, "updated", DT_U32, 0, MEMBER(nda_cacheinfo, ndm_updated) },
		{ NDA_UNSPEC, "refcnt", DT_U32, 0, MEMBER(nda_cacheinfo, ndm_refcnt) },
	}
};

static const uc_nl_nested_spec_t neigh_msg = {
	.headsize = NLA_ALIGN(sizeof(struct ndmsg)),
	.nattrs = 16,
	.attrs = {
		{ NDA_UNSPEC, "family", DT_U8, 0, MEMBER(ndmsg, ndm_family) },
		{ NDA_UNSPEC, "dev" /* actually ifindex, but avoid clash with NDA_IFINDEX */, DT_NETDEV, DF_ALLOW_NONE, MEMBER(ndmsg, ndm_ifindex) },
		{ NDA_UNSPEC, "state", DT_U16, 0, MEMBER(ndmsg, ndm_state) },
		{ NDA_UNSPEC, "flags", DT_U8, 0, MEMBER(ndmsg, ndm_flags) },
		{ NDA_UNSPEC, "type", DT_U8, 0, MEMBER(ndmsg, ndm_type) },
		{ NDA_CACHEINFO, "cacheinfo", DT_NESTED, DF_NO_SET, &neigh_cacheinfo_rta },
		{ NDA_DST, "dst", DT_ANYADDR, 0, NULL },
		{ NDA_IFINDEX, "ifindex", DT_NETDEV, 0, NULL },
		{ NDA_LINK_NETNSID, "link_netnsid", DT_U32, DF_NO_SET, NULL },
		{ NDA_LLADDR, "lladdr", DT_LLADDR, 0, NULL },
		{ NDA_MASTER, "master", DT_NETDEV, 0, NULL },
		{ NDA_PORT, "port", DT_U16, DF_BYTESWAP, NULL },
		{ NDA_PROBES, "probes", DT_U32, DF_NO_SET, NULL },
		{ NDA_SRC_VNI, "src_vni", DT_U32, DF_NO_SET, NULL },
		{ NDA_VLAN, "vlan", DT_U16, 0, NULL },
		{ NDA_VNI, "vni", DT_U32, DF_MAX_16777215, NULL },
	}
};

static const uc_nl_nested_spec_t addr_cacheinfo_rta = {
	.headsize = NLA_ALIGN(sizeof(struct ifa_cacheinfo)),
	.nattrs = 4,
	.attrs = {
		{ IFA_UNSPEC, "preferred", DT_U32, 0, MEMBER(ifa_cacheinfo, ifa_prefered) },
		{ IFA_UNSPEC, "valid", DT_U32, 0, MEMBER(ifa_cacheinfo, ifa_valid) },
		{ IFA_UNSPEC, "cstamp", DT_U32, 0, MEMBER(ifa_cacheinfo, cstamp) },
		{ IFA_UNSPEC, "tstamp", DT_U32, 0, MEMBER(ifa_cacheinfo, tstamp) },
	}
};

static const uc_nl_nested_spec_t addr_msg = {
	.headsize = NLA_ALIGN(sizeof(struct ifaddrmsg)),
	.nattrs = 11,
	.attrs = {
		{ IFA_UNSPEC, "family", DT_U8, 0, MEMBER(ifaddrmsg, ifa_family) },
		{ IFA_FLAGS, "flags", DT_U32_OR_MEMBER, DF_MAX_255, MEMBER(ifaddrmsg, ifa_flags) },
		{ IFA_UNSPEC, "scope", DT_U8, 0, MEMBER(ifaddrmsg, ifa_scope) },
		{ IFA_UNSPEC, "dev", DT_NETDEV, 0, MEMBER(ifaddrmsg, ifa_index) },
		{ IFA_ADDRESS, "address", DT_ANYADDR, DF_STORE_MASK, MEMBER(ifaddrmsg, ifa_prefixlen) },
		{ IFA_LOCAL, "local", DT_ANYADDR, 0, NULL },
		{ IFA_LABEL, "label", DT_STRING, 0, NULL },
		{ IFA_BROADCAST, "broadcast", DT_ANYADDR, 0, NULL },
		{ IFA_ANYCAST, "anycast", DT_ANYADDR, 0, NULL },
		{ IFA_CACHEINFO, "cacheinfo", DT_NESTED, DF_NO_SET, &addr_cacheinfo_rta },
		{ IFA_RT_PRIORITY, "metric", DT_U32, 0, NULL },
	}
};

static const uc_nl_nested_spec_t rule_msg = {
	.headsize = NLA_ALIGN(sizeof(struct fib_rule_hdr)),
	.nattrs = 23,
	.attrs = {
		{ FRA_UNSPEC, "family", DT_U8, 0, MEMBER(fib_rule_hdr, family) },
		{ FRA_UNSPEC, "tos", DT_U8, 0, MEMBER(fib_rule_hdr, tos) },
		{ FRA_UNSPEC, "action", DT_U8, 0, MEMBER(fib_rule_hdr, action) },
		{ FRA_UNSPEC, "flags", DT_U32, 0, MEMBER(fib_rule_hdr, flags) },
		{ FRA_PRIORITY, "priority", DT_U32, 0, NULL },
		{ FRA_SRC, "src", DT_ANYADDR, DF_STORE_MASK|DF_FAMILY_HINT, MEMBER(fib_rule_hdr, src_len) },
		{ FRA_DST, "dst", DT_ANYADDR, DF_STORE_MASK|DF_FAMILY_HINT, MEMBER(fib_rule_hdr, dst_len) },
		{ FRA_FWMARK, "fwmark", DT_U32, 0, NULL },
		{ FRA_FWMASK, "fwmask", DT_U32, 0, NULL },
		{ FRA_IFNAME, "iif", DT_NETDEV, 0, NULL },
		{ FRA_OIFNAME, "oif", DT_NETDEV, 0, NULL },
		{ FRA_L3MDEV, "l3mdev", DT_U8, 0, NULL },
		{ FRA_UID_RANGE, "uid_range", DT_NUMRANGE, 0, NULL },
		{ FRA_IP_PROTO, "ip_proto", DT_U8, 0, NULL },
		{ FRA_SPORT_RANGE, "sport_range", DT_NUMRANGE, DF_MAX_65535, NULL },
		{ FRA_DPORT_RANGE, "dport_range", DT_NUMRANGE, DF_MAX_65535, NULL },
		{ FRA_TABLE, "table", DT_U32_OR_MEMBER, DF_MAX_255, MEMBER(fib_rule_hdr, table) },
		{ FRA_SUPPRESS_PREFIXLEN, "suppress_prefixlen", DT_S32, 0, NULL },
		{ FRA_SUPPRESS_IFGROUP, "suppress_ifgroup", DT_U32, 0, NULL },
		{ FRA_FLOW, "flow", DT_U32, 0, NULL },
		{ RTA_GATEWAY, "gateway", DT_ANYADDR, DF_FAMILY_HINT, NULL },
		{ FRA_GOTO, "goto", DT_U32, 0, NULL },
		{ FRA_PROTOCOL, "protocol", DT_U8, 0, NULL },
	}
};

#define IFAL_UNSPEC 0

static const uc_nl_nested_spec_t addrlabel_msg = {
	.headsize = NLA_ALIGN(sizeof(struct ifaddrlblmsg)),
	.nattrs = 6,
	.attrs = {
		{ IFAL_UNSPEC, "family", DT_U8, 0, MEMBER(ifaddrlblmsg, ifal_family) },
		{ IFAL_UNSPEC, "flags", DT_U8, 0, MEMBER(ifaddrlblmsg, ifal_flags) },
		{ IFAL_UNSPEC, "dev", DT_NETDEV, 0, MEMBER(ifaddrlblmsg, ifal_index) },
		{ IFAL_UNSPEC, "seq", DT_U32, 0, MEMBER(ifaddrlblmsg, ifal_seq) },
		{ IFAL_ADDRESS, "address", DT_ANYADDR, DF_STORE_MASK, MEMBER(ifaddrlblmsg, ifal_prefixlen) },
		{ IFAL_LABEL, "label", DT_U32, 0, NULL },
	}
};

static const uc_nl_nested_spec_t neightbl_params_rta = {
	.headsize = 0,
	.nattrs = 13,
	.attrs = {
		{ NDTPA_IFINDEX, "dev", DT_NETDEV, 0, NULL },
		{ NDTPA_BASE_REACHABLE_TIME, "base_reachable_time", DT_U64, 0, NULL },
		{ NDTPA_RETRANS_TIME, "retrans_time", DT_U64, 0, NULL },
		{ NDTPA_GC_STALETIME, "gc_staletime", DT_U64, 0, NULL },
		{ NDTPA_DELAY_PROBE_TIME, "delay_probe_time", DT_U64, 0, NULL },
		{ NDTPA_QUEUE_LEN, "queue_len", DT_U32, 0, NULL },
		{ NDTPA_APP_PROBES, "app_probes", DT_U32, 0, NULL },
		{ NDTPA_UCAST_PROBES, "ucast_probes", DT_U32, 0, NULL },
		{ NDTPA_MCAST_PROBES, "mcast_probes", DT_U32, 0, NULL },
		{ NDTPA_ANYCAST_DELAY, "anycast_delay", DT_U64, 0, NULL },
		{ NDTPA_PROXY_DELAY, "proxy_delay", DT_U64, 0, NULL },
		{ NDTPA_PROXY_QLEN, "proxy_qlen", DT_U32, 0, NULL },
		{ NDTPA_LOCKTIME, "locktime", DT_U64, 0, NULL },
	}
};

static const uc_nl_nested_spec_t neightbl_config_rta = {
	.headsize = NLA_ALIGN(sizeof(struct ndt_config)),
	.nattrs = 9,
	.attrs = {
		{ NDTA_UNSPEC, "key_len", DT_U16, 0, MEMBER(ndt_config, ndtc_key_len) },
		{ NDTA_UNSPEC, "entry_size", DT_U16, 0, MEMBER(ndt_config, ndtc_entry_size) },
		{ NDTA_UNSPEC, "entries", DT_U32, 0, MEMBER(ndt_config, ndtc_entries) },
		{ NDTA_UNSPEC, "last_flush", DT_U32, 0, MEMBER(ndt_config, ndtc_last_flush) },
		{ NDTA_UNSPEC, "last_rand", DT_U32, 0, MEMBER(ndt_config, ndtc_last_rand) },
		{ NDTA_UNSPEC, "hash_rnd", DT_U32, 0, MEMBER(ndt_config, ndtc_hash_rnd) },
		{ NDTA_UNSPEC, "hash_mask", DT_U32, 0, MEMBER(ndt_config, ndtc_hash_mask) },
		{ NDTA_UNSPEC, "hash_chain_gc", DT_U32, 0, MEMBER(ndt_config, ndtc_hash_chain_gc) },
		{ NDTA_UNSPEC, "proxy_qlen", DT_U32, 0, MEMBER(ndt_config, ndtc_proxy_qlen) },
	}
};

static const uc_nl_nested_spec_t neightbl_stats_rta = {
	.headsize = NLA_ALIGN(sizeof(struct ndt_stats)),
	.nattrs = 10,
	.attrs = {
		{ NDTA_UNSPEC, "allocs", DT_U64, 0, MEMBER(ndt_stats, ndts_allocs) },
		{ NDTA_UNSPEC, "destroys", DT_U64, 0, MEMBER(ndt_stats, ndts_destroys) },
		{ NDTA_UNSPEC, "hash_grows", DT_U64, 0, MEMBER(ndt_stats, ndts_hash_grows) },
		{ NDTA_UNSPEC, "res_failed", DT_U64, 0, MEMBER(ndt_stats, ndts_res_failed) },
		{ NDTA_UNSPEC, "lookups", DT_U64, 0, MEMBER(ndt_stats, ndts_lookups) },
		{ NDTA_UNSPEC, "hits", DT_U64, 0, MEMBER(ndt_stats, ndts_hits) },
		{ NDTA_UNSPEC, "rcv_probes_mcast", DT_U64, 0, MEMBER(ndt_stats, ndts_rcv_probes_mcast) },
		{ NDTA_UNSPEC, "rcv_probes_ucast", DT_U64, 0, MEMBER(ndt_stats, ndts_rcv_probes_ucast) },
		{ NDTA_UNSPEC, "periodic_gc_runs", DT_U64, 0, MEMBER(ndt_stats, ndts_periodic_gc_runs) },
		{ NDTA_UNSPEC, "forced_gc_runs", DT_U64, 0, MEMBER(ndt_stats, ndts_forced_gc_runs) },
	}
};

static const uc_nl_nested_spec_t neightbl_msg = {
	.headsize = NLA_ALIGN(sizeof(struct ndtmsg)),
	.nattrs = 9,
	.attrs = {
		{ NDTA_UNSPEC, "family", DT_U8, 0, MEMBER(ndtmsg, ndtm_family) },
		{ NDTA_NAME, "name", DT_STRING, 0, NULL },
		{ NDTA_THRESH1, "thresh1", DT_U32, 0, NULL },
		{ NDTA_THRESH2, "thresh2", DT_U32, 0, NULL },
		{ NDTA_THRESH3, "thresh3", DT_U32, 0, NULL },
		{ NDTA_GC_INTERVAL, "gc_interval", DT_U64, 0, NULL },
		{ NDTA_PARMS, "params", DT_NESTED, 0, &neightbl_params_rta },
		{ NDTA_CONFIG, "config", DT_NESTED, DF_NO_SET, &neightbl_config_rta },
		{ NDTA_STATS, "stats", DT_NESTED, DF_NO_SET, &neightbl_stats_rta },
	}
};

static const uc_nl_nested_spec_t netconf_msg = {
	.headsize = NLA_ALIGN(sizeof(struct netconfmsg)),
	.nattrs = 8,
	.attrs = {
		{ NETCONFA_UNSPEC, "family", DT_U8, 0, MEMBER(netconfmsg, ncm_family) },
		{ NETCONFA_IFINDEX, "dev", DT_NETDEV, 0, NULL },
		{ NETCONFA_FORWARDING, "forwarding", DT_U32, DF_NO_SET, NULL },
		{ NETCONFA_RP_FILTER, "rp_filter", DT_U32, DF_NO_SET, NULL },
		{ NETCONFA_MC_FORWARDING, "mc_forwarding", DT_U32, DF_NO_SET, NULL },
		{ NETCONFA_PROXY_NEIGH, "proxy_neigh", DT_U32, DF_NO_SET, NULL },
		{ NETCONFA_IGNORE_ROUTES_WITH_LINKDOWN, "ignore_routes_with_linkdown", DT_U32, DF_NO_SET, NULL },
		{ NETCONFA_INPUT, "input", DT_U32, DF_NO_SET, NULL },
	}
};


static bool
nla_check_len(struct nlattr *nla, size_t sz)
{
	return (nla && nla_len(nla) >= (ssize_t)sz);
}

static bool
nla_parse_error(const uc_nl_attr_spec_t *spec, uc_vm_t *vm, uc_value_t *v, const char *msg)
{
	char *s;

	s = ucv_to_string(vm, v);

	set_error(NLE_INVAL, "%s `%s` has invalid value `%s`: %s",
		spec->attr ? "attribute" : "field",
		spec->key,
		s,
		msg);

	free(s);

	return false;
}

static void
uc_nl_put_struct_member(char *base, const void *offset, size_t datalen, void *data)
{
	memcpy(base + (uintptr_t)offset, data, datalen);
}

static void
uc_nl_put_struct_member_u8(char *base, const void *offset, uint8_t u8)
{
	base[(uintptr_t)offset] = u8;
}

static void
uc_nl_put_struct_member_u16(char *base, const void *offset, uint16_t u16)
{
	uc_nl_put_struct_member(base, offset, sizeof(u16), &u16);
}

static void
uc_nl_put_struct_member_u32(char *base, const void *offset, uint32_t u32)
{
	uc_nl_put_struct_member(base, offset, sizeof(u32), &u32);
}

static void *
uc_nl_get_struct_member(char *base, const void *offset, size_t datalen, void *data)
{
	memcpy(data, base + (uintptr_t)offset, datalen);

	return data;
}

static uint8_t
uc_nl_get_struct_member_u8(char *base, const void *offset)
{
	return (uint8_t)base[(uintptr_t)offset];
}

static uint16_t
uc_nl_get_struct_member_u16(char *base, const void *offset)
{
	uint16_t u16;

	uc_nl_get_struct_member(base, offset, sizeof(u16), &u16);

	return u16;
}

static uint32_t
uc_nl_get_struct_member_u32(char *base, const void *offset)
{
	uint32_t u32;

	uc_nl_get_struct_member(base, offset, sizeof(u32), &u32);

	return u32;
}

static uint64_t
uc_nl_get_struct_member_u64(char *base, const void *offset)
{
	uint64_t u64;

	uc_nl_get_struct_member(base, offset, sizeof(u64), &u64);

	return u64;
}

static bool
uc_nl_parse_attr(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val, size_t idx);

static uc_value_t *
uc_nl_convert_attr(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, struct nlattr **tb, uc_vm_t *vm);

static bool
uc_nl_convert_attrs(struct nl_msg *msg, void *buf, size_t buflen, size_t headsize, const uc_nl_attr_spec_t *attrs, size_t nattrs, uc_vm_t *vm, uc_value_t *obj)
{
	size_t i, maxattr = 0, structlen = headsize;
	struct nlattr **tb, *nla, *nla_nest;
	uc_value_t *v, *arr;
	int rem;

	for (i = 0; i < nattrs; i++)
		if (attrs[i].attr > maxattr)
			maxattr = attrs[i].attr;

	tb = calloc(maxattr + 1, sizeof(struct nlattr *));

	if (!tb)
		return false;

	if (buflen > headsize) {
		if (maxattr)
			nla_parse(tb, maxattr, buf + headsize, buflen - headsize, NULL);
	}
	else {
		structlen = buflen;
	}

	for (i = 0; i < nattrs; i++) {
		if (attrs[i].attr == 0 && (uintptr_t)attrs[i].auxdata >= structlen)
			continue;

		if (attrs[i].attr != 0 && !tb[attrs[i].attr])
			continue;

		if (attrs[i].flags & DF_NO_GET)
			continue;

		if (attrs[i].flags & DF_MULTIPLE) {
			/* can't happen, but needed to nudge clang-analyzer */
			if (!tb[attrs[i].attr])
				continue;

			arr = ucv_array_new(vm);
			nla_nest = tb[attrs[i].attr];

			nla_for_each_attr(nla, nla_data(nla_nest), nla_len(nla_nest), rem) {
				if (attrs[i].auxdata && nla_type(nla) != (intptr_t)attrs[i].auxdata)
					continue;

				tb[attrs[i].attr] = nla;

				v = uc_nl_convert_attr(&attrs[i], msg, (char *)buf, tb, vm);

				if (!v)
					continue;

				ucv_array_push(arr, v);
			}

			if (!ucv_array_length(arr)) {
				ucv_put(arr);

				continue;
			}

			v = arr;
		}
		else {
			v = uc_nl_convert_attr(&attrs[i], msg, (char *)buf, tb, vm);

			if (!v)
				continue;
		}

		ucv_object_add(obj, attrs[i].key, v);
	}

	free(tb);

	return true;
}

static bool
uc_nl_parse_attrs(struct nl_msg *msg, char *base, const uc_nl_attr_spec_t *attrs, size_t nattrs, uc_vm_t *vm, uc_value_t *obj)
{
	struct nlattr *nla_nest = NULL;
	size_t i, j, idx;
	uc_value_t *v;
	bool exists;

	for (i = 0; i < nattrs; i++) {
		v = ucv_object_get(obj, attrs[i].key, &exists);

		if (!exists)
			continue;

		if (attrs[i].flags & DF_MULTIPLE) {
			if (!(attrs[i].flags & DF_FLAT)) {
				nla_nest = nla_nest_start(msg, attrs[i].attr);

				if (!nla_nest)
					return nla_parse_error(&attrs[i], vm, v,
						"netlink message is full");
			}

			if (ucv_type(v) == UC_ARRAY) {
				for (j = 0; j < ucv_array_length(v); j++) {
					if (attrs[i].flags & DF_FLAT)
						idx = attrs[i].attr;
					else if (attrs[i].auxdata)
						idx = (uintptr_t)attrs[i].auxdata;
					else
						idx = j;

					if (!uc_nl_parse_attr(&attrs[i], msg, base, vm, ucv_array_get(v, j), idx))
						return false;
				}
			}
			else {
				if (attrs[i].flags & DF_FLAT)
					idx = attrs[i].attr;
				else if (attrs[i].auxdata)
					idx = (uintptr_t)attrs[i].auxdata;
				else
					idx = 0;

				if (!uc_nl_parse_attr(&attrs[i], msg, base, vm, v, idx))
					return false;
			}

			if (nla_nest)
				nla_nest_end(msg, nla_nest);
		}
		else if (!uc_nl_parse_attr(&attrs[i], msg, base, vm, v, 0)) {
			return false;
		}
	}

	return true;
}

static bool
uc_nl_parse_rta_nexthop(struct nl_msg *msg, uc_vm_t *vm, uc_value_t *val)
{
	struct { uint16_t family; char addr[sizeof(struct in6_addr)]; } via;
	struct nlmsghdr *hdr = nlmsg_hdr(msg);
	struct rtmsg *rtm = NLMSG_DATA(hdr);
	struct nlattr *rta_gateway;
	struct rtnexthop *rtnh;
	uc_nl_cidr_t cidr = { 0 };
	uc_value_t *v;
	uint32_t u;
	int aflen;
	char *s;

	if (ucv_type(val) != UC_OBJECT)
		return false;

	if (!uc_nl_parse_cidr(vm, ucv_object_get(val, "via", NULL), &cidr))
		return false;

	aflen = (cidr.family == AF_INET6 ? sizeof(cidr.addr.in6) : sizeof(cidr.addr.in));

	if (cidr.mask != (aflen * 8))
		return false;

	rta_gateway = nla_reserve(msg, RTA_GATEWAY, sizeof(*rtnh));

	if (!rta_gateway)
		return false;

	rtnh = nla_data(rta_gateway);
	rtnh->rtnh_len = sizeof(*rtnh);

	if (rtm->rtm_family == AF_UNSPEC)
		rtm->rtm_family = cidr.family;

	if (cidr.family == rtm->rtm_family) {
		nla_put(msg, RTA_GATEWAY, aflen, &cidr.addr.in6);
		rtnh->rtnh_len += nla_total_size(aflen);
	}
	else {
		via.family = cidr.family;
		memcpy(via.addr, &cidr.addr.in6, aflen);
		nla_put(msg, RTA_VIA, sizeof(via.family) + aflen, &via);
		rtnh->rtnh_len += nla_total_size(sizeof(via.family) + aflen);
	}

	v = ucv_object_get(val, "dev", NULL);
	s = ucv_string_get(v);

	if (s) {
		rtnh->rtnh_ifindex = if_nametoindex(s);

		if (rtnh->rtnh_ifindex == 0)
			return false;
	}

	v = ucv_object_get(val, "weight", NULL);

	if (v) {
		if (!uc_nl_parse_u32(v, &u) || u == 0 || u > 256)
			return false;

		rtnh->rtnh_hops = u - 1;
	}

	if (ucv_is_truish(ucv_object_get(val, "onlink", NULL)))
		rtnh->rtnh_flags |= RTNH_F_ONLINK;

	v = ucv_object_get(val, "realm", NULL);

	if (v) {
		if (!uc_nl_parse_u32(v, &u))
			return false;

		nla_put_u32(msg, RTA_FLOW, u);
		rtnh->rtnh_len += nla_total_size(sizeof(uint32_t));
	}

	v = ucv_object_get(val, "as", NULL);

	if (v) {
		if (!uc_nl_parse_cidr(vm, v, &cidr) || cidr.family != rtm->rtm_family)
			return false;

		if (cidr.mask != cidr.bitlen)
			return false;

		nla_put(msg, RTA_NEWDST, cidr.alen, &cidr.addr.in6);
		rtnh->rtnh_len += nla_total_size(cidr.alen);
	}

	/* XXX: nla_nest_end(rta_gateway) ? */

	return true;
}

static bool
uc_nl_parse_rta_multipath(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	struct nlattr *rta_multipath = nla_nest_start(msg, spec->attr);
	size_t i;

	if (!rta_multipath)
		return nla_parse_error(spec, vm, val, "netlink message is full");

	for (i = 0; i < ucv_array_length(val); i++)
		if (!uc_nl_parse_rta_nexthop(msg, vm, ucv_array_get(val, i)))
			return false;

	nla_nest_end(msg, rta_multipath);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_encap(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm);

static uc_value_t *
uc_nl_convert_rta_multipath(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	uc_nl_attr_spec_t encap_spec = { .attr = RTA_ENCAP };
	struct rtnexthop *nh = nla_data(tb[spec->attr]);
	struct nlattr *multipath_tb[RTA_MAX + 1];
	size_t len = nla_len(tb[spec->attr]);
	uc_value_t *nh_obj, *nh_arr;
	char buf[INET6_ADDRSTRLEN];
	struct rtvia *via;
	int af;

	nh_arr = ucv_array_new(vm);

	while (len >= sizeof(*nh)) {
		if ((size_t)NLA_ALIGN(nh->rtnh_len) > len)
			break;

		nh_obj = ucv_object_new(vm);
		ucv_array_push(nh_arr, nh_obj);

		nla_parse(multipath_tb, RTA_MAX, (struct nlattr *)RTNH_DATA(nh), nh->rtnh_len - sizeof(*nh), NULL);

		if (multipath_tb[RTA_GATEWAY]) {
			switch (nla_len(multipath_tb[RTA_GATEWAY])) {
			case 4: af = AF_INET; break;
			case 16: af = AF_INET6;	break;
			default: af = AF_UNSPEC; break;
			}

			if (inet_ntop(af, nla_data(multipath_tb[RTA_GATEWAY]), buf, sizeof(buf)))
				ucv_object_add(nh_obj, "via", ucv_string_new(buf));
		}

		if (multipath_tb[RTA_VIA]) {
			if (nla_len(multipath_tb[RTA_VIA]) > (ssize_t)sizeof(*via)) {
				via = nla_data(multipath_tb[RTA_VIA]);
				af = via->rtvia_family;

				if ((af == AF_INET &&
				     nla_len(multipath_tb[RTA_VIA]) == sizeof(*via) + sizeof(struct in_addr)) ||
					(af == AF_INET6 &&
				     nla_len(multipath_tb[RTA_VIA]) == sizeof(*via) + sizeof(struct in6_addr))) {
					if (inet_ntop(af, via->rtvia_addr, buf, sizeof(buf)))
						ucv_object_add(nh_obj, "via", ucv_string_new(buf));
				}
			}
		}

		if (if_indextoname(nh->rtnh_ifindex, buf))
			ucv_object_add(nh_obj, "dev", ucv_string_new(buf));

		ucv_object_add(nh_obj, "weight", ucv_int64_new(nh->rtnh_hops + 1));
		ucv_object_add(nh_obj, "onlink", ucv_boolean_new(nh->rtnh_flags & RTNH_F_ONLINK));

		if (multipath_tb[RTA_FLOW] && nla_len(multipath_tb[RTA_FLOW]) == sizeof(uint32_t))
			ucv_object_add(nh_obj, "realm", ucv_int64_new(nla_get_u32(multipath_tb[RTA_FLOW])));

		if (multipath_tb[RTA_ENCAP])
			ucv_object_add(nh_obj, "encap",
				uc_nl_convert_rta_encap(&encap_spec, msg, multipath_tb, vm));

		if (multipath_tb[RTA_NEWDST]) {
			switch (nla_len(multipath_tb[RTA_NEWDST])) {
			case 4: af = AF_INET; break;
			case 16: af = AF_INET6; break;
			default: af = AF_UNSPEC; break;
			}

			if (inet_ntop(af, nla_data(multipath_tb[RTA_NEWDST]), buf, sizeof(buf)))
				ucv_object_add(nh_obj, "as", ucv_string_new(buf));
		}

		len -= NLA_ALIGN(nh->rtnh_len);
		nh = RTNH_NEXT(nh);
	}

	return nh_arr;
}

static bool
parse_num(const uc_nl_attr_spec_t *spec, uc_vm_t *vm, uc_value_t *val, void *dst)
{
	int64_t n = ucv_int64_get(val);
	uint32_t *u32;
	uint16_t *u16;
	uint8_t *u8;

	if (spec->flags & DF_MAX_255) {
		if (n < 0 || n > 255)
			return nla_parse_error(spec, vm, val, "number out of range 0-255");

		u8 = dst; *u8 = n;
	}
	else if (spec->flags & DF_MAX_65535) {
		if (n < 0 || n > 65535)
			return nla_parse_error(spec, vm, val, "number out of range 0-65535");

		u16 = dst; *u16 = n;

		if (spec->flags & DF_BYTESWAP)
			*u16 = htons(*u16);
	}
	else if (spec->flags & DF_MAX_16777215) {
		if (n < 0 || n > 16777215)
			return nla_parse_error(spec, vm, val, "number out of range 0-16777215");

		u32 = dst; *u32 = n;

		if (spec->flags & DF_BYTESWAP)
			*u32 = htonl(*u32);
	}
	else {
		if (n < 0 || n > 4294967295)
			return nla_parse_error(spec, vm, val, "number out of range 0-4294967295");

		u32 = dst; *u32 = n;

		if (spec->flags & DF_BYTESWAP)
			*u32 = htonl(*u32);
	}

	return true;
}

static bool
uc_nl_parse_rta_numrange(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	union {
		struct { uint8_t low; uint8_t high; } u8;
		struct { uint16_t low; uint16_t high; } u16;
		struct { uint32_t low; uint32_t high; } u32;
	} ranges = { 0 };

	void *d1, *d2;
	size_t len;

	if (ucv_array_length(val) != 2 ||
	    ucv_type(ucv_array_get(val, 0)) != UC_INTEGER ||
	    ucv_type(ucv_array_get(val, 1)) != UC_INTEGER)
		return nla_parse_error(spec, vm, val, "not a two-element array of numbers");

	if (spec->flags & DF_MAX_255) {
		len = sizeof(ranges.u8);
		d1 = &ranges.u8.low;
		d2 = &ranges.u8.high;
	}
	else if (spec->flags & DF_MAX_65535) {
		len = sizeof(ranges.u16);
		d1 = &ranges.u16.low;
		d2 = &ranges.u16.high;
	}
	else {
		len = sizeof(ranges.u32);
		d1 = &ranges.u32.low;
		d2 = &ranges.u32.high;
	}

	if (!parse_num(spec, vm, ucv_array_get(val, 0), d1) ||
	    !parse_num(spec, vm, ucv_array_get(val, 1), d2))
	    return false;

	nla_put(msg, spec->attr, len, d1);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_numrange(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	union {
		struct { uint8_t low; uint8_t high; } *u8;
		struct { uint16_t low; uint16_t high; } *u16;
		struct { uint32_t low; uint32_t high; } *u32;
	} ranges = { 0 };

	bool swap = (spec->flags & DF_BYTESWAP);
	uc_value_t *arr, *n1, *n2;

	if (spec->flags & DF_MAX_255) {
		if (!nla_check_len(tb[spec->attr], sizeof(*ranges.u8)))
			return NULL;

		ranges.u8 = nla_data(tb[spec->attr]);
		n1 = ucv_int64_new(ranges.u8->low);
		n2 = ucv_int64_new(ranges.u8->high);
	}
	else if (spec->flags & DF_MAX_65535) {
		if (!nla_check_len(tb[spec->attr], sizeof(*ranges.u16)))
			return NULL;

		ranges.u16 = nla_data(tb[spec->attr]);
		n1 = ucv_int64_new(swap ? ntohs(ranges.u16->low) : ranges.u16->low);
		n2 = ucv_int64_new(swap ? ntohs(ranges.u16->high) : ranges.u16->high);
	}
	else {
		if (!nla_check_len(tb[spec->attr], sizeof(*ranges.u32)))
			return NULL;

		ranges.u32 = nla_data(tb[spec->attr]);
		n1 = ucv_int64_new(swap ? ntohl(ranges.u32->low) : ranges.u32->low);
		n2 = ucv_int64_new(swap ? ntohl(ranges.u32->high) : ranges.u32->high);
	}

	arr = ucv_array_new(vm);

	ucv_array_push(arr, n1);
	ucv_array_push(arr, n2);

	return arr;
}


#define LINK_TYPE(name) \
	{ #name, link_##name##_attrs, ARRAY_SIZE(link_##name##_attrs) }

static const struct {
	const char *name;
	const uc_nl_attr_spec_t *attrs;
	size_t nattrs;
} link_types[] = {
	LINK_TYPE(bareudp),
	LINK_TYPE(bond),
	LINK_TYPE(bond_slave),
	LINK_TYPE(bridge),
	LINK_TYPE(bridge_slave),
	LINK_TYPE(can),
	LINK_TYPE(geneve),
	LINK_TYPE(hsr),
	LINK_TYPE(ipoib),
	LINK_TYPE(ipvlan),
	LINK_TYPE(macvlan),
	LINK_TYPE(rmnet),
	LINK_TYPE(vlan),
	LINK_TYPE(vrf),
	LINK_TYPE(vxcan),
	LINK_TYPE(vxlan),
	//LINK_TYPE(xdp),
	//LINK_TYPE(xstats),
	LINK_TYPE(gre),
	LINK_TYPE(gretap),
	LINK_TYPE(erspan),
	LINK_TYPE(ip6gre),
	LINK_TYPE(ip6gretap),
	LINK_TYPE(ip6erspan),
	LINK_TYPE(ip6tnl),
	LINK_TYPE(ipip),
	LINK_TYPE(sit),
	LINK_TYPE(veth),
	LINK_TYPE(vti),
	LINK_TYPE(vti6),
	LINK_TYPE(xfrm),
};

static bool
uc_nl_parse_rta_linkinfo(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	const uc_nl_attr_spec_t *attrs = NULL;
	struct nlattr *li_nla, *info_nla;
	size_t i, nattrs = 0;
	char *kind, *p;
	uc_value_t *k;

	k = ucv_object_get(val, "type", NULL);
	kind = ucv_string_get(k);

	if (!kind)
		return nla_parse_error(spec, vm, val, "linkinfo does not specify kind");

	li_nla = nla_nest_start(msg, spec->attr);

	if (!li_nla)
		return nla_parse_error(spec, vm, val, "netlink message is full");

	nla_put_string(msg, IFLA_INFO_KIND, kind);

	for (i = 0; i < ARRAY_SIZE(link_types); i++) {
		if (!strcmp(link_types[i].name, kind)) {
			attrs = link_types[i].attrs;
			nattrs = link_types[i].nattrs;
			break;
		}
	}

	p = strchr(kind, '_');

	if (!p || strcmp(p, "_slave"))
		info_nla = nla_nest_start(msg, IFLA_INFO_DATA);
	else
		info_nla = nla_nest_start(msg, IFLA_INFO_SLAVE_DATA);

	if (!info_nla)
		return nla_parse_error(spec, vm, val, "netlink message is full");

	if (!uc_nl_parse_attrs(msg, base, attrs, nattrs, vm, val))
		return false;

	nla_nest_end(msg, info_nla);
	nla_nest_end(msg, li_nla);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_linkinfo_data(uc_value_t *obj, size_t attr, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	const uc_nl_attr_spec_t *attrs = NULL;
	size_t i, nattrs = 0;
	uc_value_t *v;
	bool rv;

	if (!tb[attr] || nla_len(tb[attr]) < 1)
		return NULL;

	v = ucv_string_new_length(nla_data(tb[attr]), nla_len(tb[attr]) - 1);

	ucv_object_add(obj, "type", v);

	for (i = 0; i < ARRAY_SIZE(link_types); i++) {
		if (!strcmp(link_types[i].name, ucv_string_get(v))) {
			attrs = link_types[i].attrs;
			nattrs = link_types[i].nattrs;
			break;
		}
	}

	attr = (attr == IFLA_INFO_KIND) ? IFLA_INFO_DATA : IFLA_INFO_SLAVE_DATA;

	if (nattrs > 0 && tb[attr]) {
		rv = uc_nl_convert_attrs(msg, nla_data(tb[attr]), nla_len(tb[attr]), 0, attrs, nattrs, vm, obj);

		if (!rv)
			return NULL;
	}

	return obj;
}

static uc_value_t *
uc_nl_convert_rta_linkinfo(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	struct nlattr *linkinfo_tb[IFLA_INFO_MAX + 1];
	uc_value_t *info_obj, *slave_obj;

	if (!tb[spec->attr])
		return NULL;

	nla_parse(linkinfo_tb, IFLA_INFO_MAX, nla_data(tb[spec->attr]), nla_len(tb[spec->attr]), NULL);

	info_obj = ucv_object_new(vm);

	if (linkinfo_tb[IFLA_INFO_KIND]) {
		if (!uc_nl_convert_rta_linkinfo_data(info_obj, IFLA_INFO_KIND, msg, linkinfo_tb, vm)) {
			ucv_put(info_obj);

			return NULL;
		}
	}

	if (linkinfo_tb[IFLA_INFO_SLAVE_KIND]) {
		slave_obj = ucv_object_new(vm);

		if (!uc_nl_convert_rta_linkinfo_data(slave_obj, IFLA_INFO_SLAVE_KIND, msg, linkinfo_tb, vm)) {
			ucv_put(info_obj);
			ucv_put(slave_obj);

			return NULL;
		}

		ucv_object_add(info_obj, "slave", slave_obj);
	}

	return info_obj;
}

static uc_value_t *
uc_nl_convert_rta_bridgeid(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	char buf[sizeof("ffff.ff:ff:ff:ff:ff:ff")];
	struct ifla_bridge_id *id;

	if (!nla_check_len(tb[spec->attr], sizeof(*id)))
		return NULL;

	id = nla_data(tb[spec->attr]);

	snprintf(buf, sizeof(buf), "%02x%02x.%02x:%02x:%02x:%02x:%02x:%02x",
		id->prio[0], id->prio[1],
		id->addr[0], id->addr[1],
		id->addr[2], id->addr[3],
		id->addr[4], id->addr[5]);

	return ucv_string_new(buf);
}

static bool
uc_nl_parse_rta_srh(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	uc_value_t *mode, *hmac, *segs, *seg;
	struct seg6_iptunnel_encap *tun;
	struct sr6_tlv_hmac *tlv;
	struct ipv6_sr_hdr *srh;
	size_t i, nsegs, srhlen;
	char *s;

	mode = ucv_object_get(val, "mode", NULL);
	hmac = ucv_object_get(val, "hmac", NULL);
	segs = ucv_object_get(val, "segs", NULL);

	if (mode != NULL &&
	    (ucv_type(mode) != UC_INTEGER ||
	     ucv_int64_get(mode) < 0 ||
	     ucv_int64_get(mode) > UINT32_MAX))
		return nla_parse_error(spec, vm, val, "srh mode not an integer in range 0-4294967295");

	if (hmac != NULL &&
	    (ucv_type(hmac) != UC_INTEGER ||
	     ucv_int64_get(hmac) < 0 ||
	     ucv_int64_get(hmac) > UINT32_MAX))
		return nla_parse_error(spec, vm, val, "srh hmac not an integer in range 0-4294967295");

	if (ucv_type(segs) != UC_ARRAY ||
	    ucv_array_length(segs) == 0)
		return nla_parse_error(spec, vm, val, "srh segs array missing or empty");

	nsegs = ucv_array_length(segs);

	if (!mode || !ucv_int64_get(mode))
		nsegs++;

	srhlen = 8 + 16 * nsegs;

	if (hmac && ucv_int64_get(hmac))
		srhlen += 40;


	tun = calloc(1, sizeof(*tun) + srhlen);

	if (!tun)
		return nla_parse_error(spec, vm, val, "cannot allocate srh header");

	tun->mode = (int)ucv_int64_get(mode);

	srh = tun->srh;
	srh->hdrlen = (srhlen >> 3) - 1;
	srh->type = 4;
	srh->segments_left = nsegs - 1;
	srh->first_segment = nsegs - 1;

	if (hmac && ucv_int64_get(hmac))
		srh->flags |= SR6_FLAG1_HMAC;

	for (i = 0; i < ucv_array_length(segs); i++) {
		seg = ucv_array_get(segs, i);
		s = ucv_string_get(seg);

		if (!s || inet_pton(AF_INET6, s, &srh->segments[--nsegs]) != 1) {
			free(tun);

			return nla_parse_error(spec, vm, val, "srh segs array contains invalid IPv6 address");
		}
	}

	if (hmac && ucv_int64_get(hmac)) {
		tlv = (struct sr6_tlv_hmac *)((char *)srh + srhlen - 40);
		tlv->tlvhdr.type = SR6_TLV_HMAC;
		tlv->tlvhdr.len = 38;
		tlv->hmackeyid = htonl((uint32_t)ucv_int64_get(hmac));
	}

	nla_put(msg, spec->attr, sizeof(*tun) + srhlen, tun);
	free(tun);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_srh(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	char buf[INET6_ADDRSTRLEN], *p, *e;
	struct seg6_iptunnel_encap *tun;
	uc_value_t *tun_obj, *seg_arr;
	struct sr6_tlv_hmac *tlv;
	size_t i;

	if (!nla_check_len(tb[spec->attr], sizeof(*tun)))
		return NULL;

	tun = nla_data(tb[spec->attr]);
	tun_obj = ucv_object_new(vm);

	ucv_object_add(tun_obj, "mode", ucv_int64_new(tun->mode));

	seg_arr = ucv_array_new(vm);

	p = (char *)tun->srh->segments;
	e = (char *)tun + nla_len(tb[spec->attr]);

	for (i = tun->srh->first_segment + 1;
	     p + sizeof(struct in6_addr) <= e && i > 0;
	     i--, p += sizeof(struct in6_addr)) {
		if (inet_ntop(AF_INET6, p, buf, sizeof(buf)))
			ucv_array_push(seg_arr, ucv_string_new(buf));
		else
			ucv_array_push(seg_arr, NULL);
	}

	ucv_object_add(tun_obj, "segs", seg_arr);

	if (sr_has_hmac(tun->srh)) {
		i = ((tun->srh->hdrlen + 1) << 3) - 40;
		tlv = (struct sr6_tlv_hmac *)((char *)tun->srh + i);

		ucv_object_add(tun_obj, "hmac", ucv_int64_new(ntohl(tlv->hmackeyid)));
	}

	return tun_obj;
}

#define ENCAP_TYPE(name, type) \
	{ #name, LWTUNNEL_ENCAP_##type, route_encap_##name##_attrs, ARRAY_SIZE(route_encap_##name##_attrs) }

static const struct {
	const char *name;
	uint16_t type;
	const uc_nl_attr_spec_t *attrs;
	size_t nattrs;
} encap_types[] = {
	ENCAP_TYPE(mpls, MPLS),
	ENCAP_TYPE(ip, IP),
	ENCAP_TYPE(ip6, IP6),
	ENCAP_TYPE(ila, ILA),
	//ENCAP_TYPE(bpf, BPF),
	ENCAP_TYPE(seg6, SEG6),
	//ENCAP_TYPE(seg6local, SEG6_LOCAL),
	//ENCAP_TYPE(rpl, RPL),
};

static bool
uc_nl_parse_rta_encap(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	const uc_nl_attr_spec_t *attrs = NULL;
	struct nlattr *enc_nla;
	size_t i, nattrs = 0;
	uint16_t ntype = 0;
	uc_value_t *t;
	char *type;

	t = ucv_object_get(val, "type", NULL);
	type = ucv_string_get(t);

	if (!type)
		return nla_parse_error(spec, vm, val, "encap does not specify type");

	for (i = 0; i < ARRAY_SIZE(encap_types); i++) {
		if (!strcmp(encap_types[i].name, type)) {
			ntype = encap_types[i].type;
			attrs = encap_types[i].attrs;
			nattrs = encap_types[i].nattrs;
			break;
		}
	}

	if (!ntype)
		return nla_parse_error(spec, vm, val, "encap specifies unknown type");

	nla_put_u16(msg, RTA_ENCAP_TYPE, ntype);

	enc_nla = nla_nest_start(msg, spec->attr);

	if (!enc_nla)
		return nla_parse_error(spec, vm, val, "netlink message is full");

	if (!uc_nl_parse_attrs(msg, base, attrs, nattrs, vm, val))
		return false;

	nla_nest_end(msg, enc_nla);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_encap(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	const uc_nl_attr_spec_t *attrs = NULL;
	const char *name = NULL;
	uc_value_t *encap_obj;
	size_t i, nattrs = 0;
	bool rv;

	if (!tb[spec->attr] ||
	    !nla_check_len(tb[RTA_ENCAP_TYPE], sizeof(uint16_t)))
		return NULL;

	for (i = 0; i < ARRAY_SIZE(encap_types); i++) {
		if (encap_types[i].type != nla_get_u16(tb[RTA_ENCAP_TYPE]))
			continue;

		name = encap_types[i].name;
		attrs = encap_types[i].attrs;
		nattrs = encap_types[i].nattrs;

		break;
	}

	if (!name)
		return NULL;

	encap_obj = ucv_object_new(vm);

	rv = uc_nl_convert_attrs(msg,
		nla_data(tb[spec->attr]), nla_len(tb[spec->attr]), 0,
		attrs, nattrs, vm, encap_obj);

	if (!rv) {
		ucv_put(encap_obj);

		return NULL;
	}

	ucv_object_add(encap_obj, "type", ucv_string_new(name));

	return encap_obj;
}

#define IPOPTS_TYPE(name, type, multiple) \
	{ #name, LWTUNNEL_IP_OPTS_##type, multiple, lwtipopt_##name##_attrs, ARRAY_SIZE(lwtipopt_##name##_attrs) }

static const struct {
	const char *name;
	uint16_t type;
	bool multiple;
	const uc_nl_attr_spec_t *attrs;
	size_t nattrs;
} lwtipopt_types[] = {
	IPOPTS_TYPE(erspan, ERSPAN, false),
	IPOPTS_TYPE(geneve, GENEVE, true),
	IPOPTS_TYPE(vxlan, VXLAN, false),
};

static bool
uc_nl_parse_rta_ipopts(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	const uc_nl_attr_spec_t *attrs = NULL;
	struct nlattr *opt_nla, *type_nla;
	bool exists, multiple = false;
	size_t i, j, nattrs = 0;
	uint16_t ntype = 0;
	uc_value_t *item;

	ucv_object_foreach(val, type, v) {
		for (i = 0; i < ARRAY_SIZE(lwtipopt_types); i++) {
			if (!strcmp(lwtipopt_types[i].name, type)) {
				val = v;
				ntype = lwtipopt_types[i].type;
				attrs = lwtipopt_types[i].attrs;
				nattrs = lwtipopt_types[i].nattrs;
				multiple = lwtipopt_types[i].multiple;
				break;
			}
		}
	}

	if (!ntype)
		return nla_parse_error(spec, vm, val, "unknown IP options type specified");

	opt_nla = nla_nest_start(msg, spec->attr);

	if (!opt_nla)
		return nla_parse_error(spec, vm, val, "netlink message is full");

	j = 0;
	item = (ucv_type(val) == UC_ARRAY) ? ucv_array_get(val, j++) : val;

	while (true) {
		type_nla = nla_nest_start(msg, ntype);

		if (!type_nla)
			return nla_parse_error(spec, vm, val, "netlink message is full");

		for (i = 0; i < nattrs; i++) {
			v = ucv_object_get(item, attrs[i].key, &exists);

			if (!exists)
				continue;

			if (!uc_nl_parse_attr(&attrs[i], msg, nla_data(type_nla), vm, v, 0))
				return false;
		}

		nla_nest_end(msg, type_nla);

		if (!multiple || ucv_type(val) != UC_ARRAY || j >= ucv_array_length(val))
			break;

		item = ucv_array_get(val, j++);
	}

	nla_nest_end(msg, opt_nla);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_ipopts(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	struct nlattr *opt_tb[LWTUNNEL_IP_OPTS_MAX + 1];
	const uc_nl_attr_spec_t *attrs = NULL;
	uc_value_t *opt_obj, *type_obj;
	const char *name = NULL;
	size_t i, nattrs = 0;
	uint16_t type = 0;
	bool rv;

	if (!tb[spec->attr] ||
		!nla_parse(opt_tb, LWTUNNEL_IP_OPTS_MAX, nla_data(tb[spec->attr]), nla_len(tb[spec->attr]), NULL))
		return NULL;

	for (i = 0; i < ARRAY_SIZE(lwtipopt_types); i++) {
		if (!opt_tb[lwtipopt_types[i].type])
			continue;

		type = lwtipopt_types[i].type;
		name = lwtipopt_types[i].name;
		attrs = lwtipopt_types[i].attrs;
		nattrs = lwtipopt_types[i].nattrs;

		break;
	}

	if (!name)
		return NULL;

	type_obj = ucv_object_new(vm);

	rv = uc_nl_convert_attrs(msg,
		nla_data(opt_tb[type]), nla_len(opt_tb[type]), 0,
		attrs, nattrs, vm, type_obj);

	if (!rv) {
		ucv_put(type_obj);

		return NULL;
	}

	opt_obj = ucv_object_new(vm);

	ucv_object_add(opt_obj, name, type_obj);

	return opt_obj;
}

static bool
uc_nl_parse_rta_afspec(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	struct rtgenmsg *rtg = nlmsg_data(nlmsg_hdr(msg));
	struct bridge_vlan_info vinfo = { 0 };
	uc_value_t *vlans, *vlan, *vv;
	struct nlattr *nla, *af_nla;
	uint32_t num;
	size_t i;

	nla = nla_reserve(msg, spec->attr, 0);

	if (!nla)
		return nla_parse_error(spec, vm, val, "netlink message is full");

	ucv_object_foreach(val, type, v) {
		if (!strcmp(type, "bridge")) {
			if (rtg->rtgen_family == AF_UNSPEC)
				rtg->rtgen_family = AF_BRIDGE;

			vv = ucv_object_get(v, "bridge_flags", NULL);

			if (vv) {
				if (!uc_nl_parse_u32(vv, &num) || num > 0xffff)
					return nla_parse_error(spec, vm, vv, "field bridge.bridge_flags not an integer or out of range 0-65535");

				nla_put_u16(msg, IFLA_BRIDGE_FLAGS, num);
			}

			vv = ucv_object_get(v, "bridge_mode", NULL);

			if (vv) {
				if (!uc_nl_parse_u32(vv, &num) || num > 0xffff)
					return nla_parse_error(spec, vm, vv, "field bridge.bridge_mode not an integer or out of range 0-65535");

				nla_put_u16(msg, IFLA_BRIDGE_MODE, num);
			}

			vlans = ucv_object_get(v, "bridge_vlan_info", NULL);

			for (vlan = (ucv_type(vlans) == UC_ARRAY) ? ucv_array_get(vlans, 0) : vlans, i = 0;
			     ucv_type(vlan) == UC_OBJECT;
			     vlan = (ucv_type(vlans) == UC_ARRAY) ? ucv_array_get(vlans, ++i) : NULL) {

				vinfo.vid = 0;
				vinfo.flags = 0;

				vv = ucv_object_get(vlan, "flags", NULL);

				if (vv) {
					if (!uc_nl_parse_u32(vv, &num) || num > 0xffff)
						return nla_parse_error(spec, vm, vv, "field bridge.bridge_vlan_info.flags not an integer or out of range 0-65535");

					vinfo.flags = num;
				}

				vv = ucv_object_get(vlan, "vid", NULL);

				if (!uc_nl_parse_u32(vv, &num) || num > 0xfff)
					return nla_parse_error(spec, vm, vv, "field bridge.bridge_vlan_info.vid not an integer or out of range 0-4095");

				vinfo.vid = num;

				vv = ucv_object_get(vlan, "vid_end", NULL);

				if (vv) {
					if (!uc_nl_parse_u32(vv, &num) || num > 0xfff)
						return nla_parse_error(spec, vm, vv, "field bridge.bridge_vlan_info.vid_end not an integer or out of range 0-4095");

					vinfo.flags &= ~BRIDGE_VLAN_INFO_RANGE_END;
					vinfo.flags |= BRIDGE_VLAN_INFO_RANGE_BEGIN;
					nla_put(msg, IFLA_BRIDGE_VLAN_INFO, sizeof(vinfo), &vinfo);

					vinfo.vid = num;
					vinfo.flags &= ~BRIDGE_VLAN_INFO_RANGE_BEGIN;
					vinfo.flags |= BRIDGE_VLAN_INFO_RANGE_END;
					nla_put(msg, IFLA_BRIDGE_VLAN_INFO, sizeof(vinfo), &vinfo);
				}
				else {
					vinfo.flags &= ~(BRIDGE_VLAN_INFO_RANGE_BEGIN|BRIDGE_VLAN_INFO_RANGE_END);
					nla_put(msg, IFLA_BRIDGE_VLAN_INFO, sizeof(vinfo), &vinfo);
				}
			}
		}
		else if (!strcmp(type, "inet")) {
			af_nla = nla_reserve(msg, AF_INET, link_attrs_af_spec_inet_rta.headsize);

			if (!af_nla)
				return nla_parse_error(spec, vm, val,
					"netlink message is full");

			if (!uc_nl_parse_attrs(msg, nla_data(af_nla),
			                       link_attrs_af_spec_inet_rta.attrs,
			                       link_attrs_af_spec_inet_rta.nattrs,
			                       vm, v))
				return false;

			nla_nest_end(msg, af_nla);
		}
		else if (!strcmp(type, "inet6")) {
			af_nla = nla_reserve(msg, AF_INET6, link_attrs_af_spec_inet6_rta.headsize);

			if (!af_nla)
				return nla_parse_error(spec, vm, val,
					"netlink message is full");

			if (!uc_nl_parse_attrs(msg, nla_data(af_nla),
			                       link_attrs_af_spec_inet6_rta.attrs,
			                       link_attrs_af_spec_inet6_rta.nattrs,
			                       vm, v))
				return false;

			nla_nest_end(msg, af_nla);
		}
		else {
			return nla_parse_error(spec, vm, val, "unknown address family specified");
		}
	}

	nla_nest_end(msg, nla);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_afspec(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	struct rtgenmsg *rtg = nlmsg_data(nlmsg_hdr(msg));
	uc_value_t *obj, *bridge, *vlans = NULL, *vlan;
	struct bridge_vlan_info vinfo;
	struct nlattr *nla;
	uint16_t vid = 0;
	int rem;

	if (!tb[spec->attr])
		return NULL;

	obj = ucv_object_new(vm);

	if (rtg->rtgen_family == AF_BRIDGE) {
		bridge = ucv_object_new(vm);

		nla_for_each_attr(nla, nla_data(tb[spec->attr]), nla_len(tb[spec->attr]), rem) {
			switch (nla_type(nla)) {
			case IFLA_BRIDGE_FLAGS:
				if (nla_check_len(nla, sizeof(uint16_t)))
					ucv_object_add(bridge, "bridge_flags", ucv_uint64_new(nla_get_u16(nla)));

				break;

			case IFLA_BRIDGE_MODE:
				if (nla_check_len(nla, sizeof(uint16_t)))
					ucv_object_add(bridge, "bridge_mode", ucv_uint64_new(nla_get_u16(nla)));

				break;

			case IFLA_BRIDGE_VLAN_INFO:
				if (nla_check_len(nla, sizeof(vinfo))) {
					memcpy(&vinfo, nla_data(nla), sizeof(vinfo));

					if (!(vinfo.flags & BRIDGE_VLAN_INFO_RANGE_END))
						vid = vinfo.vid;

					if (vinfo.flags & BRIDGE_VLAN_INFO_RANGE_BEGIN)
						continue;

					if (!vlans) {
						vlans = ucv_array_new(vm);
						ucv_object_add(bridge, "bridge_vlan_info", vlans);
					}

					vlan = ucv_object_new(vm);

					ucv_object_add(vlan, "vid", ucv_uint64_new(vid));

					if (vid != vinfo.vid)
						ucv_object_add(vlan, "vid_end", ucv_uint64_new(vinfo.vid));

					ucv_object_add(vlan, "flags", ucv_uint64_new(vinfo.flags & ~BRIDGE_VLAN_INFO_RANGE_END));

					ucv_array_push(vlans, vlan);
				}

				break;
			}
		}

		ucv_object_add(obj, "bridge", bridge);
	}
	else {
		if (!uc_nl_convert_attrs(msg, nla_data(tb[spec->attr]), nla_len(tb[spec->attr]),
		                         link_attrs_af_spec_rta.headsize, link_attrs_af_spec_rta.attrs,
		                         link_attrs_af_spec_rta.nattrs, vm, obj)) {
			ucv_put(obj);

			return NULL;
		}
	}

	return obj;
}

static bool
uc_nl_parse_rta_u32_or_member(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	uint32_t u32;

	if (!uc_nl_parse_u32(val, &u32))
		return nla_parse_error(spec, vm, val, "not an integer or out of range 0-4294967295");

	if (spec->flags & DF_MAX_255) {
		if (u32 <= 255) {
			uc_nl_put_struct_member_u8(base, spec->auxdata, u32);

			return true;
		}

		uc_nl_put_struct_member_u8(base, spec->auxdata, 0);
	}
	else if (spec->flags & DF_MAX_65535) {
		if (u32 <= 65535) {
			uc_nl_put_struct_member_u16(base, spec->auxdata,
				(spec->flags & DF_BYTESWAP) ? htons((uint16_t)u32) : (uint16_t)u32);

			return true;
		}

		uc_nl_put_struct_member_u16(base, spec->auxdata, 0);
	}
	else if (spec->flags & DF_MAX_16777215) {
		if (u32 <= 16777215) {
			uc_nl_put_struct_member_u32(base, spec->auxdata,
				(spec->flags & DF_BYTESWAP) ? htonl(u32) : u32);

			return true;
		}

		uc_nl_put_struct_member_u32(base, spec->auxdata, 0);
	}

	nla_put_u32(msg, spec->attr,
		(spec->flags & DF_BYTESWAP) ? htonl(u32) : u32);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_u32_or_member(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, struct nlattr **tb, uc_vm_t *vm)
{
	uint32_t u32 = 0;

	if (nla_check_len(tb[spec->attr], sizeof(uint32_t))) {
		if (spec->flags & DF_BYTESWAP)
			u32 = ntohl(nla_get_u32(tb[spec->attr]));
		else
			u32 = nla_get_u32(tb[spec->attr]);
	}
	else if (spec->flags & DF_MAX_255) {
		u32 = uc_nl_get_struct_member_u8(base, spec->auxdata);
	}
	else if (spec->flags & DF_MAX_65535) {
		if (spec->flags & DF_BYTESWAP)
			u32 = ntohs(uc_nl_get_struct_member_u16(base, spec->auxdata));
		else
			u32 = uc_nl_get_struct_member_u16(base, spec->auxdata);
	}
	else if (spec->flags & DF_MAX_16777215) {
		if (spec->flags & DF_BYTESWAP)
			u32 = ntohl(uc_nl_get_struct_member_u32(base, spec->auxdata));
		else
			u32 = uc_nl_get_struct_member_u32(base, spec->auxdata);
	}
	else {
		return NULL;
	}

	return ucv_uint64_new(u32);
}

static bool
uc_nl_parse_rta_nested(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val)
{
	const uc_nl_nested_spec_t *nest = spec->auxdata;
	struct nlattr *nested_nla;

	nested_nla = nla_reserve(msg, spec->attr, nest->headsize);

	if (!nested_nla)
		return false;

	/* nla_reserve() only zeroes the padding, not the payload, so struct
	 * headers would otherwise carry heap garbage into the kernel */
	if (nest->headsize)
		memset(nla_data(nested_nla), 0, nest->headsize);

	if (!uc_nl_parse_attrs(msg, nla_data(nested_nla), nest->attrs, nest->nattrs, vm, val))
		return false;

	nla_nest_end(msg, nested_nla);

	return true;
}

static uc_value_t *
uc_nl_convert_rta_nested(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, struct nlattr **tb, uc_vm_t *vm)
{
	const uc_nl_nested_spec_t *nest = spec->auxdata;
	uc_value_t *nested_obj;
	bool rv;

	nested_obj = ucv_object_new(vm);

	rv = uc_nl_convert_attrs(msg,
		nla_data(tb[spec->attr]), nla_len(tb[spec->attr]), nest->headsize,
		nest->attrs, nest->nattrs,
		vm, nested_obj);

	if (!rv) {
		ucv_put(nested_obj);

		return NULL;
	}

	return nested_obj;
}


static bool
uc_nl_parse_attr(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, uc_vm_t *vm, uc_value_t *val, size_t idx)
{
	uc_nl_cidr_t cidr = { 0 };
	struct ether_addr *ea;
	struct rtgenmsg *rtg;
	uint64_t u64;
	uint32_t u32;
	uint16_t u16;
	size_t attr;
	char *s;

	if (spec->flags & DF_MULTIPLE)
		attr = idx;
	else
		attr = spec->attr;

	switch (spec->type) {
	case DT_U8:
		if (!uc_nl_parse_u32(val, &u32) || u32 > 255)
			return nla_parse_error(spec, vm, val, "not an integer or out of range 0-255");

		if ((spec->flags & DF_MAX_1) && u32 > 1)
			return nla_parse_error(spec, vm, val, "integer must be 0 or 1");

		if (spec->attr == 0)
			uc_nl_put_struct_member_u8(base, spec->auxdata, u32);
		else
			nla_put_u8(msg, attr, u32);

		break;

	case DT_U16:
		if (!uc_nl_parse_u32(val, &u32) || u32 > 65535)
			return nla_parse_error(spec, vm, val, "not an integer or out of range 0-65535");

		u16 = (uint16_t)u32;

		if (spec->flags & DF_BYTESWAP)
			u16 = htons(u16);

		if ((spec->flags & DF_MAX_1) && u32 > 1)
			return nla_parse_error(spec, vm, val, "integer must be 0 or 1");
		else if ((spec->flags & DF_MAX_255) && u32 > 255)
			return nla_parse_error(spec, vm, val, "integer out of range 0-255");

		if (spec->attr == 0)
			uc_nl_put_struct_member_u16(base, spec->auxdata, u16);
		else
			nla_put_u16(msg, attr, u16);

		break;

	case DT_S32:
	case DT_U32:
		if (spec->type == DT_S32 && !uc_nl_parse_s32(val, &u32))
			return nla_parse_error(spec, vm, val, "not an integer or out of range -2147483648-2147483647");
		else if (spec->type == DT_U32 && !uc_nl_parse_u32(val, &u32))
			return nla_parse_error(spec, vm, val, "not an integer or out of range 0-4294967295");

		if (spec->flags & DF_BYTESWAP)
			u32 = htonl(u32);

		if ((spec->flags & DF_MAX_1) && u32 > 1)
			return nla_parse_error(spec, vm, val, "integer must be 0 or 1");
		else if ((spec->flags & DF_MAX_255) && u32 > 255)
			return nla_parse_error(spec, vm, val, "integer out of range 0-255");
		else if ((spec->flags & DF_MAX_65535) && u32 > 65535)
			return nla_parse_error(spec, vm, val, "integer out of range 0-65535");
		else if ((spec->flags & DF_MAX_16777215) && u32 > 16777215)
			return nla_parse_error(spec, vm, val, "integer out of range 0-16777215");

		if (spec->attr == 0)
			uc_nl_put_struct_member_u32(base, spec->auxdata, u32);
		else
			nla_put_u32(msg, attr, u32);

		break;

	case DT_U64:
		assert(spec->attr != 0);

		if (!uc_nl_parse_u64(val, &u64))
			return nla_parse_error(spec, vm, val, "not an integer or negative");

		if (spec->flags & DF_BYTESWAP)
			u64 = htobe64(u64);

		nla_put_u64(msg, attr, u64);
		break;

	case DT_BOOL:
		u32 = (uint32_t)ucv_is_truish(val);

		if (spec->attr == 0)
			uc_nl_put_struct_member_u8(base, spec->auxdata, u32);
		else
			nla_put_u8(msg, attr, u32);

		break;

	case DT_FLAG:
		u32 = (uint32_t)ucv_is_truish(val);

		if (spec->attr == 0)
			uc_nl_put_struct_member_u8(base, spec->auxdata, u32);
		else if (u32 == 1)
			nla_put_flag(msg, attr);

		break;

	case DT_STRING:
		assert(spec->attr != 0);

		if (ucv_type(val) == UC_STRING) {
			nla_put(msg, attr, ucv_string_length(val), ucv_string_get(val));
		}
		else {
			s = ucv_to_string(vm, val);

			if (!s)
				return nla_parse_error(spec, vm, val, "out of memory");

			nla_put_string(msg, attr, s);
			free(s);
		}

		break;

	case DT_NETDEV:
		if (ucv_type(val) == UC_INTEGER) {
			if (ucv_int64_get(val) < 0 ||
			    ucv_int64_get(val) > UINT32_MAX)
				return nla_parse_error(spec, vm, val, "interface index out of range 0-4294967295");

			u32 = (uint32_t)ucv_int64_get(val);
		}
		else {
			s = ucv_to_string(vm, val);

			if (!s)
				return nla_parse_error(spec, vm, val, "out of memory");

			u32 = if_nametoindex(s);

			free(s);
		}

		if (u32 == 0 && !(spec->flags & DF_ALLOW_NONE))
			return nla_parse_error(spec, vm, val, "interface not found");

		if (spec->attr == 0)
			uc_nl_put_struct_member_u32(base, spec->auxdata, u32);
		else
			nla_put_u32(msg, attr, u32);

		break;

	case DT_LLADDR:
		assert(spec->attr != 0);

		s = ucv_to_string(vm, val);

		if (!s)
			return nla_parse_error(spec, vm, val, "out of memory");

		ea = ether_aton(s);

		free(s);

		if (!ea)
			return nla_parse_error(spec, vm, val, "invalid MAC address");

		nla_put(msg, attr, sizeof(*ea), ea);

		break;

	case DT_U64ADDR:
		assert(spec->attr != 0);

		if (ucv_type(val) == UC_INTEGER) {
			u64 = ucv_uint64_get(val);
		}
		else {
			s = ucv_to_string(vm, val);

			if (!s)
				return nla_parse_error(spec, vm, val, "out of memory");

			u16 = addr64_pton(s, &u64);

			free(s);

			if (u16 != 1)
				return nla_parse_error(spec, vm, val, "invalid address");
		}

		nla_put_u64(msg, attr, u64);

		break;

	case DT_INADDR:
	case DT_IN6ADDR:
	case DT_MPLSADDR:
	case DT_ANYADDR:
		assert(spec->attr != 0);

		rtg = nlmsg_data(nlmsg_hdr(msg));

		if (!uc_nl_parse_cidr(vm, val, &cidr))
			return nla_parse_error(spec, vm, val, "invalid IP address");

		if ((spec->type == DT_INADDR && cidr.family != AF_INET) ||
		    (spec->type == DT_IN6ADDR && cidr.family != AF_INET6) ||
		    (spec->type == DT_MPLSADDR && cidr.family != AF_MPLS))
		    return nla_parse_error(spec, vm, val, "wrong address family");

		if (spec->flags & DF_STORE_MASK)
			uc_nl_put_struct_member_u8(base, spec->auxdata, cidr.mask);
		else if (cidr.mask != cidr.bitlen)
			return nla_parse_error(spec, vm, val, "address range given but single address expected");

		nla_put(msg, attr, cidr.alen, &cidr.addr.in6);

		if ((rtg->rtgen_family == AF_UNSPEC) && (spec->flags & DF_FAMILY_HINT))
			rtg->rtgen_family = cidr.family;

		break;

	case DT_MULTIPATH:
		if (!uc_nl_parse_rta_multipath(spec, msg, base, vm, val))
			return nla_parse_error(spec, vm, val, "invalid nexthop data");

		break;

	case DT_NUMRANGE:
		if (!uc_nl_parse_rta_numrange(spec, msg, base, vm, val))
			return false;

		break;

	case DT_FLAGS:
		if (ucv_array_length(val) == 2) {
			if (ucv_type(ucv_array_get(val, 0)) != UC_INTEGER ||
			    ucv_type(ucv_array_get(val, 1)) != UC_INTEGER)
				return nla_parse_error(spec, vm, val, "flag or mask value not an integer");

			if (!uc_nl_parse_u32(ucv_array_get(val, 0), &u32))
				return nla_parse_error(spec, vm, val, "flag value not an integer or out of range 0-4294967295");

			memcpy(&u64, &u32, sizeof(u32));

			if (!uc_nl_parse_u32(ucv_array_get(val, 1), &u32))
				return nla_parse_error(spec, vm, val, "mask value not an integer or out of range 0-4294967295");

			memcpy((char *)&u64 + sizeof(u32), &u32, sizeof(u32));
		}
		else if (ucv_type(val) == UC_INTEGER) {
			if (!uc_nl_parse_u32(val, &u32))
				return nla_parse_error(spec, vm, val, "flag value not an integer or out of range 0-4294967295");

			memcpy(&u64, &u32, sizeof(u32));
			memset((char *)&u64 + sizeof(u32), 0xff, sizeof(u32));
		}
		else {
			return nla_parse_error(spec, vm, val, "value neither an array of flags, mask nor an integer");
		}

		if (spec->attr == 0)
			uc_nl_put_struct_member(base, spec->auxdata, sizeof(u64), &u64);
		else
			nla_put_u64(msg, attr, u64);

		break;

	case DT_LINKINFO:
		if (!uc_nl_parse_rta_linkinfo(spec, msg, base, vm, val))
			return false;

		break;

	case DT_SRH:
		if (!uc_nl_parse_rta_srh(spec, msg, base, vm, val))
			return false;

		break;

	case DT_ENCAP:
		if (!uc_nl_parse_rta_encap(spec, msg, base, vm, val))
			return false;

		break;

	case DT_IPOPTS:
		if (!uc_nl_parse_rta_ipopts(spec, msg, base, vm, val))
			return false;

		break;

	case DT_AFSPEC:
		if (!uc_nl_parse_rta_afspec(spec, msg, base, vm, val))
			return false;

		break;

	case DT_U32_OR_MEMBER:
		if (!uc_nl_parse_rta_u32_or_member(spec, msg, base, vm, val))
			return false;

		break;

	case DT_NESTED:
		if (!uc_nl_parse_rta_nested(spec, msg, base, vm, val))
			return false;

		break;

	default:
		assert(0);
	}

	return true;
}

static uc_value_t *
uc_nl_convert_attr(const uc_nl_attr_spec_t *spec, struct nl_msg *msg, char *base, struct nlattr **tb, uc_vm_t *vm)
{
	union { uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64; size_t sz; } t = { 0 };
	struct { uint32_t flags; uint32_t mask; } flags;
	char buf[sizeof(struct mpls_label) * 16];
	struct nlmsghdr *hdr = nlmsg_hdr(msg);
	struct rtgenmsg *rtg = nlmsg_data(hdr);
	struct ether_addr *ea;
	uc_value_t *v;
	char *s;

	switch (spec->type) {
	case DT_U8:
		if (spec->attr == 0)
			t.u8 = uc_nl_get_struct_member_u8(base, spec->auxdata);
		else if (nla_check_len(tb[spec->attr], sizeof(t.u8)))
			t.u8 = nla_get_u8(tb[spec->attr]);

		return ucv_uint64_new(t.u8);

	case DT_U16:
		if (spec->attr == 0)
			t.u16 = uc_nl_get_struct_member_u16(base, spec->auxdata);
		else if (nla_check_len(tb[spec->attr], sizeof(t.u16)))
			t.u16 = nla_get_u16(tb[spec->attr]);

		if (spec->flags & DF_BYTESWAP)
			t.u16 = ntohs(t.u16);

		return ucv_uint64_new(t.u16);

	case DT_U32:
	case DT_S32:
		if (spec->attr == 0)
			t.u32 = uc_nl_get_struct_member_u32(base, spec->auxdata);
		else if (nla_check_len(tb[spec->attr], sizeof(t.u32)))
			t.u32 = nla_get_u32(tb[spec->attr]);

		if (spec->flags & DF_BYTESWAP)
			t.u32 = ntohl(t.u32);

		if (spec->type == DT_S32)
			return ucv_int64_new((int32_t)t.u32);

		return ucv_uint64_new(t.u32);

	case DT_U64:
		if (spec->attr == 0)
			t.u64 = uc_nl_get_struct_member_u64(base, spec->auxdata);
		else if (nla_check_len(tb[spec->attr], sizeof(t.u64)))
			memcpy(&t.u64, nla_data(tb[spec->attr]), sizeof(t.u64));

		return ucv_uint64_new(t.u64);

	case DT_BOOL:
		if (spec->attr == 0)
			t.u8 = uc_nl_get_struct_member_u8(base, spec->auxdata);
		else if (nla_check_len(tb[spec->attr], sizeof(t.u8)))
			t.u8 = nla_get_u8(tb[spec->attr]);

		return ucv_boolean_new(t.u8 != 0);

	case DT_FLAG:
		if (spec->attr == 0)
			t.u8 = uc_nl_get_struct_member_u8(base, spec->auxdata);
		else if (tb[spec->attr] != NULL)
			t.u8 = 1;

		return ucv_boolean_new(t.u8 != 0);

	case DT_STRING:
		assert(spec->attr != 0);

		if (!nla_check_len(tb[spec->attr], 1))
			return NULL;

		return ucv_string_new_length(
			nla_data(tb[spec->attr]), nla_len(tb[spec->attr]) - 1);

	case DT_NETDEV:
		if (spec->attr == 0)
			t.u32 = uc_nl_get_struct_member_u32(base, spec->auxdata);
		else if (nla_check_len(tb[spec->attr], sizeof(t.u32)))
			t.u32 = nla_get_u32(tb[spec->attr]);

		if (if_indextoname(t.u32, buf))
			return ucv_string_new(buf);
		else if (spec->flags & DF_ALLOW_NONE)
			return ucv_int64_new(0);

		return NULL;

	case DT_LLADDR:
		assert(spec->attr != 0);

		if (!nla_check_len(tb[spec->attr], sizeof(*ea)))
			return NULL;

		ea = nla_data(tb[spec->attr]);

		snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
			ea->ether_addr_octet[0], ea->ether_addr_octet[1],
			ea->ether_addr_octet[2], ea->ether_addr_octet[3],
			ea->ether_addr_octet[4], ea->ether_addr_octet[5]);

		return ucv_string_new(buf);

	case DT_U64ADDR:
		assert(spec->attr != 0);

		if (!nla_check_len(tb[spec->attr], sizeof(uint64_t)) ||
		    !addr64_ntop(nla_data(tb[spec->attr]), buf, sizeof(buf)))
			return NULL;

		return ucv_string_new(buf);

	case DT_INADDR:
	case DT_IN6ADDR:
	case DT_MPLSADDR:
	case DT_ANYADDR:
		assert(spec->attr != 0);

		t.sz = (size_t)nla_len(tb[spec->attr]);

		switch (spec->type) {
		case DT_INADDR:
			if (t.sz < sizeof(struct in_addr) ||
			    !inet_ntop(AF_INET, nla_data(tb[spec->attr]), buf, sizeof(buf)))
				return NULL;

			break;

		case DT_IN6ADDR:
			if (t.sz < sizeof(struct in6_addr) ||
			    !inet_ntop(AF_INET6, nla_data(tb[spec->attr]), buf, sizeof(buf)))
				return NULL;

			break;

		case DT_MPLSADDR:
			if (t.sz < sizeof(struct mpls_label) ||
			    !mpls_ntop(nla_data(tb[spec->attr]), t.sz, buf, sizeof(buf)))
				return NULL;

			break;

		default:
			switch (rtg->rtgen_family) {
			case AF_MPLS:
				if (t.sz < sizeof(struct mpls_label) ||
				    !mpls_ntop(nla_data(tb[spec->attr]), t.sz, buf, sizeof(buf)))
					return NULL;

				break;

			case AF_INET6:
				if (t.sz < sizeof(struct in6_addr) ||
				    !inet_ntop(AF_INET6, nla_data(tb[spec->attr]), buf, sizeof(buf)))
					return NULL;

				break;

			case AF_INET:
				if (t.sz < sizeof(struct in_addr) ||
				    !inet_ntop(AF_INET, nla_data(tb[spec->attr]), buf, sizeof(buf)))
					return NULL;

				break;

			default:
				return NULL;
			}

			break;
		}

		if (spec->flags & DF_STORE_MASK) {
			s = buf + strlen(buf);
			snprintf(s, buf + sizeof(buf) - s, "/%hhu",
				uc_nl_get_struct_member_u8(base, spec->auxdata));
		}

		return ucv_string_new(buf);

	case DT_MULTIPATH:
		return uc_nl_convert_rta_multipath(spec, msg, tb, vm);

	case DT_NUMRANGE:
		return uc_nl_convert_rta_numrange(spec, msg, tb, vm);

	case DT_FLAGS:
		if (spec->attr == 0)
			uc_nl_get_struct_member(base, spec->auxdata, sizeof(flags), &flags);
		else if (nla_check_len(tb[spec->attr], sizeof(flags)))
			memcpy(&flags, nla_data(tb[spec->attr]), sizeof(flags));
		else
			return NULL;

		if (flags.mask == 0)
			return ucv_uint64_new(flags.flags);

		v = ucv_array_new(vm);

		ucv_array_push(v, ucv_uint64_new(flags.flags));
		ucv_array_push(v, ucv_uint64_new(flags.mask));

		return v;

	case DT_LINKINFO:
		return uc_nl_convert_rta_linkinfo(spec, msg, tb, vm);

	case DT_BRIDGEID:
		return uc_nl_convert_rta_bridgeid(spec, msg, tb, vm);

	case DT_SRH:
		return uc_nl_convert_rta_srh(spec, msg, tb, vm);

	case DT_ENCAP:
		return uc_nl_convert_rta_encap(spec, msg, tb, vm);

	case DT_IPOPTS:
		return uc_nl_convert_rta_ipopts(spec, msg, tb, vm);

	case DT_AFSPEC:
		return uc_nl_convert_rta_afspec(spec, msg, tb, vm);

	case DT_U32_OR_MEMBER:
		return uc_nl_convert_rta_u32_or_member(spec, msg, base, tb, vm);

	case DT_NESTED:
		return uc_nl_convert_rta_nested(spec, msg, tb, vm);

	default:
		assert(0);
	}

	return NULL;
}


static struct nl_sock *sock = NULL;
static struct {
	struct nl_sock *evsock;
	struct uloop_fd evsock_fd;
	uint32_t groups[RTNL_GRPS_BITMAP_SIZE];
} nl_conn;

typedef enum {
	STATE_UNREPLIED,
	STATE_CONTINUE,
	STATE_REPLIED,
	STATE_ERROR
} reply_state_t;

typedef struct {
	reply_state_t state;
	uc_vm_t *vm;
	uc_value_t *res;
	int family;
	const uc_nl_nested_spec_t *spec;
} request_state_t;


/**
 * Query error information.
 *
 * Returns a string containing a description of the last occurred error or
 * `null` if there is no error information.
 *
 * @function module:rtnl#error
 *
 * @returns {?string}
 *
 * @example
 * // Trigger rtnl error
 * request('invalid_command', {}, {});
 *
 * // Print error (should yield error description)
 * print(error(), "\n");
 */
static uc_value_t *
uc_nl_error(uc_vm_t *vm, size_t nargs)
{
	uc_stringbuf_t *buf;
	const char *s;

	if (last_error.code == 0)
		return NULL;

	buf = ucv_stringbuf_new();

	if (last_error.code == NLE_FAILURE && last_error.msg) {
		ucv_stringbuf_addstr(buf, last_error.msg, strlen(last_error.msg));
	}
	else {
		s = nl_geterror(last_error.code);

		ucv_stringbuf_addstr(buf, s, strlen(s));

		if (last_error.msg)
			ucv_stringbuf_printf(buf, ": %s", last_error.msg);
	}

	set_error(0, NULL);

	return ucv_stringbuf_finish(buf);
}

/*
 * route functions
 */

static int
cb_done(struct nl_msg *msg, void *arg)
{
	request_state_t *s = arg;

	s->state = STATE_REPLIED;

	return NL_STOP;
}

static int
cb_error(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg)
{
	request_state_t *s = arg;
	int errnum = err->error;

	set_error(NLE_FAILURE, "RTNETLINK answers: %s",
	          strerror(errnum < 0 ? -errnum : errnum));

	s->state = STATE_ERROR;

	return NL_STOP;
}

static int
cb_reply(struct nl_msg *msg, void *arg)
{
	struct nlmsghdr *hdr = nlmsg_hdr(msg);
	request_state_t *s = arg;
	uc_value_t *o;
	bool rv;

	if (RTM_FAM(hdr->nlmsg_type) != s->family)
		return NL_SKIP;

	if (s->spec) {
		if (nlmsg_attrlen(hdr, 0) < (ssize_t)s->spec->headsize)
			return NL_SKIP;

		o = ucv_object_new(s->vm);

		rv = uc_nl_convert_attrs(msg,
			nlmsg_attrdata(hdr, 0),
			nlmsg_attrlen(hdr, 0),
			s->spec->headsize,
			s->spec->attrs, s->spec->nattrs, s->vm, o);

		if (rv) {
			if (hdr->nlmsg_flags & NLM_F_MULTI) {
				if (!s->res)
					s->res = ucv_array_new(s->vm);

				ucv_array_push(s->res, o);
			}
			else {
				s->res = o;
			}
		}
		else {
			ucv_put(o);
		}
	}

	s->state = STATE_CONTINUE;

	return NL_SKIP;
}


static const struct {
	int family;
	const uc_nl_nested_spec_t *spec;
} rtm_families[] = {
	{ RTM_FAM(RTM_GETLINK), &link_msg },
	{ RTM_FAM(RTM_GETROUTE), &route_msg },
	{ RTM_FAM(RTM_GETNEIGH), &neigh_msg },
	{ RTM_FAM(RTM_GETADDR), &addr_msg },
	{ RTM_FAM(RTM_GETRULE), &rule_msg },
	{ RTM_FAM(RTM_GETADDRLABEL), &addrlabel_msg },
	{ RTM_FAM(RTM_GETNEIGHTBL), &neightbl_msg },
	{ RTM_FAM(RTM_GETNETCONF), &netconf_msg },
};

static void
uc_nl_sock_close(void)
{
	if (sock) {
		nl_socket_free(sock);
		sock = NULL;
	}
}

/**
 * Close the cached request socket.
 *
 * Closes and resets the internal netlink socket used by request().
 * Subsequent calls to request() will open a new socket.
 *
 * @function module:rtnl#close
 *
 * @returns {boolean} - Always returns true
 */
static uc_value_t *
uc_nl_close(uc_vm_t *vm, size_t nargs)
{
	uc_nl_sock_close();

	return ucv_boolean_new(true);
}

/**
 * Send a netlink request.
 *
 * Sends a netlink request with the specified command, flags, and payload.
 *
 * @function module:rtnl#request
 *
 * @param {string} cmd - The netlink command to send
 * @param {number} flags - The netlink flags for the request
 * @param {*} payload - The payload data for the request
 *
 * @returns {?*} - The response data or null on error
 *
 * @example
 * // Send a route request
 * let response = request('RTM_GETROUTE', 0, { family: AF_INET });
 */
static uc_value_t *
uc_nl_request(uc_vm_t *vm, size_t nargs)
{
	uc_value_t *cmd = uc_fn_arg(0);
	uc_value_t *flags = uc_fn_arg(1);
	uc_value_t *payload = uc_fn_arg(2);
	request_state_t st = { .vm = vm };
	uint16_t flagval = 0;
	struct nl_msg *msg;
	struct nl_cb *cb;
	socklen_t optlen;
	int enable, err;
	void *buf;
	size_t i;

	if (ucv_type(cmd) != UC_INTEGER || ucv_int64_get(cmd) < 0 ||
	    (flags != NULL && ucv_type(flags) != UC_INTEGER) ||
	    (payload != NULL && ucv_type(payload) != UC_OBJECT))
		err_return(NLE_INVAL, NULL);

	if (flags) {
		if (ucv_int64_get(flags) < 0 || ucv_int64_get(flags) > 0xffff)
			err_return(NLE_INVAL, NULL);
		else
			flagval = (uint16_t)ucv_int64_get(flags);
	}

	for (i = 0; i < ARRAY_SIZE(rtm_families); i++) {
		if (rtm_families[i].family == RTM_FAM(ucv_int64_get(cmd))) {
			st.spec = rtm_families[i].spec;
			st.family = rtm_families[i].family;
			break;
		}
	}

	if (!sock) {
		sock = nl_socket_alloc();

		if (!sock)
			err_return(NLE_NOMEM, NULL);

		err = nl_connect(sock, NETLINK_ROUTE);

		if (err != 0) {
			uc_nl_sock_close();
			err_return(err, NULL);
		}
	}

	optlen = sizeof(enable);

	if (getsockopt(sock->s_fd, SOL_NETLINK, NETLINK_GET_STRICT_CHK, &enable, &optlen) < 0)
		enable = 0;

	if (!!(flagval & NLM_F_STRICT_CHK) != enable) {
		enable = !!(flagval & NLM_F_STRICT_CHK);

		if (setsockopt(sock->s_fd, SOL_NETLINK, NETLINK_GET_STRICT_CHK, &enable, sizeof(enable)) < 0) {
			uc_nl_sock_close();
			err_return(nl_syserr2nlerr(errno), "Unable to toggle NETLINK_GET_STRICT_CHK");
		}
	}

	msg = nlmsg_alloc_simple(ucv_int64_get(cmd), NLM_F_REQUEST | (flagval & ~NLM_F_STRICT_CHK));

	if (!msg)
		err_return(NLE_NOMEM, NULL);

	if (st.spec) {
		if (st.spec->headsize) {
			buf = nlmsg_reserve(msg, st.spec->headsize, 0);

			if (!buf) {
				nlmsg_free(msg);

				return NULL;
			}

			memset(buf, 0, st.spec->headsize);
		}

		if (!uc_nl_parse_attrs(msg, NLMSG_DATA(nlmsg_hdr(msg)), st.spec->attrs, st.spec->nattrs, vm, payload)) {
			nlmsg_free(msg);

			return NULL;
		}
	}

	cb = nl_cb_alloc(NL_CB_DEFAULT);

	if (!cb) {
		nlmsg_free(msg);
		err_return(NLE_NOMEM, NULL);
	}

	nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, cb_reply, &st);
	nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, cb_done, &st);
	nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, cb_done, &st);
	nl_cb_err(cb, NL_CB_CUSTOM, cb_error, &st);

	err = nl_send_auto_complete(sock, msg);

	if (err < 0) {
		nlmsg_free(msg);
		nl_cb_put(cb);
		uc_nl_sock_close();
		err_return(err, NULL);
	}

	do {
		err = nl_recvmsgs(sock, cb);

		if (err && st.state != STATE_ERROR) {
			set_error(err, NULL);

			st.state = STATE_ERROR;
		}
	}
	while (st.state < STATE_REPLIED);

	nlmsg_free(msg);
	nl_cb_put(cb);

	switch (st.state) {
	case STATE_REPLIED:
		return st.res;

	case STATE_UNREPLIED:
		return ucv_boolean_new(true);

	case STATE_ERROR:
		uc_nl_sock_close();
		return ucv_boolean_new(false);

	default:
		uc_nl_sock_close();
		set_error(NLE_FAILURE, "Interrupted reply");

		return ucv_boolean_new(false);
	}
}

static const uc_nl_nested_spec_t *
uc_nl_msg_spec(int type)
{
	switch (type) {
	case RTM_NEWLINK:
	case RTM_DELLINK:
		return &link_msg;
	case RTM_NEWROUTE:
	case RTM_DELROUTE:
		return &route_msg;
	case RTM_NEWNEIGH:
	case RTM_DELNEIGH:
		return &neigh_msg;
	case RTM_NEWADDR:
	case RTM_DELADDR:
		return &addr_msg;
	case RTM_NEWRULE:
	case RTM_DELRULE:
		return &rule_msg;
	case RTM_NEWADDRLABEL:
	case RTM_DELADDRLABEL:
		return &addrlabel_msg;
	case RTM_NEWNEIGHTBL:
		return &neightbl_msg;
	case RTM_NEWNETCONF:
	case RTM_DELNETCONF:
		return &netconf_msg;
	default:
		return NULL;
	}
}

static void
uc_nl_prepare_event(uc_vm_t *vm, uc_value_t *dest, struct nl_msg *msg)
{
	struct nlmsghdr *hdr = nlmsg_hdr(msg);
	const uc_nl_nested_spec_t *spec;
	const uc_nl_attr_spec_t *attrs = NULL;
	size_t nattrs = 0, headsize = 0;
	uc_value_t *o;

	spec = uc_nl_msg_spec(hdr->nlmsg_type);
	if (spec) {
		attrs = spec->attrs;
		nattrs = spec->nattrs;
		headsize = spec->headsize;
	}

	o = ucv_object_new(vm);
	if (!uc_nl_convert_attrs(msg, nlmsg_attrdata(hdr, 0),
		nlmsg_attrlen(hdr, 0), headsize, attrs, nattrs, vm, o)) {
		ucv_put(o);
		return;
	}

	ucv_object_add(dest, "msg", o);
	if (headsize)
		ucv_object_add(dest, "head", ucv_string_new_length(NLMSG_DATA(hdr), headsize));
}

static bool
uc_nl_fill_cmds(uint32_t *cmd_bits, uc_value_t *cmds)
{
	if (ucv_type(cmds) == UC_ARRAY) {
		for (size_t i = 0; i < ucv_array_length(cmds); i++) {
			int64_t n = ucv_int64_get(ucv_array_get(cmds, i));

			if (errno || n < 0 || n >= __RTM_MAX)
				return false;

			cmd_bits[n / 32] |= (1 << (n % 32));
		}
	}
	else if (ucv_type(cmds) == UC_INTEGER) {
		int64_t n = ucv_int64_get(cmds);

		if (errno || n < 0 || n > 255)
			return false;

		cmd_bits[n / 32] |= (1 << (n % 32));
	}
	else if (!cmds)
		memset(cmd_bits, 0xff, RTNL_CMDS_BITMAP_SIZE * sizeof(*cmd_bits));
	else
		return false;

	return true;
}

static int
cb_listener_event(struct nl_msg *msg, void *arg)
{
	struct nlmsghdr *hdr = nlmsg_hdr(msg);
	uc_vm_t *vm = listener_vm;
	int cmd = hdr->nlmsg_type;

	if (!nl_conn.evsock_fd.registered || !vm)
		return NL_SKIP;

	for (size_t i = 0; i < ucv_array_length(listener_registry); i += 2) {
		uc_value_t *this = ucv_array_get(listener_registry, i);
		uc_value_t *func = ucv_array_get(listener_registry, i + 1);
		uc_nl_listener_t *l;
		uc_value_t *o;

		l = ucv_resource_data(this, "rtnl.listener");
		if (!l)
			continue;

		if (cmd > __RTM_MAX || !(l->cmds[cmd / 32] & (1 << (cmd % 32))))
			continue;

		if (!ucv_is_callable(func))
			continue;

		o = ucv_object_new(vm);
		uc_nl_prepare_event(vm, o, msg);
		ucv_object_add(o, "cmd", ucv_int64_new(cmd));

		uc_vm_stack_push(vm, ucv_get(this));
		uc_vm_stack_push(vm, ucv_get(func));
		uc_vm_stack_push(vm, o);

		if (uc_vm_call(vm, true, 1) != EXCEPTION_NONE) {
			uloop_end();
			set_error(NLE_FAILURE, "Runtime exception in callback");

			errno = EINVAL;

			return NL_STOP;
		}

		ucv_put(uc_vm_stack_pop(vm));
	}

	errno = 0;

	return NL_SKIP;
}

static void
uc_nl_listener_cb(struct uloop_fd *fd, unsigned int events)
{
	while (true) {
		errno = 0;

		nl_recvmsgs_default(nl_conn.evsock);

		if (errno != 0)
			break;
	}
}

static void
uc_nl_add_group(unsigned int idx)
{
	if (idx >= __RTNLGRP_MAX)
		return;

	if (nl_conn.groups[idx / 32] & (1 << (idx % 32)))
		return;

	nl_conn.groups[idx / 32] |= (1 << (idx % 32));
	nl_socket_add_membership(nl_conn.evsock, idx);
}

static bool
uc_nl_evsock_init(void)
{
	struct uloop_fd *fd = &nl_conn.evsock_fd;
	struct nl_sock *sock;

	if (nl_conn.evsock)
		return true;

	sock = nl_socket_alloc();

	if (nl_connect(sock, NETLINK_ROUTE))
		goto free;

	fd->fd = nl_socket_get_fd(sock);
	fd->cb = uc_nl_listener_cb;
	uloop_fd_add(fd, ULOOP_READ);

	nl_socket_set_buffer_size(sock, 1024 * 1024, 0);
	nl_socket_disable_seq_check(sock);
	nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, cb_listener_event, NULL);

	nl_conn.evsock = sock;

	return true;

free:
	nl_socket_free(sock);
	return false;
}

/**
 * Represents a netlink listener resource.
 *
 * @class module:rtnl.listener
 * @hideconstructor
 *
 * @see {@link module:rtnl#listener|listener()}
 *
 * @example
 * const nlListener = listener((msg) => {
 *     print('Received netlink message:', msg, '\n');
 * }, [RTM_NEWROUTE, RTM_DELROUTE]);
 *
 * nlListener.set_commands([RTM_GETLINK, RTM_SETLINK]);
 *
 * nlListener.close();
 */

/**
 * Create a netlink listener.
 *
 * Creates a new netlink listener that will receive messages matching the specified
 * commands and multicast groups.
 *
 * @function module:rtnl#listener
 *
 * @param {function} callback - The callback function to invoke when a message is received
 * @param {Array<string>} [commands] - Array of netlink commands to listen for (optional)
 * @param {Array<number>} [groups] - Array of multicast groups to join (optional)
 *
 * @returns {module:rtnl.listener} - A listener object with methods to control the listener
 *
 * @example
 * // Create a listener for route changes
 * let routeListener = listener((msg) => {
 *     print('Received route message:', msg, '\n');
 * }, [RTM_NEWROUTE, RTM_DELROUTE]);
 */
static uc_value_t *
uc_nl_listener(uc_vm_t *vm, size_t nargs)
{
	uc_nl_listener_t *l;
	uc_value_t *cb_func = uc_fn_arg(0);
	uc_value_t *cmds = uc_fn_arg(1);
	uc_value_t *groups = uc_fn_arg(2);
	uc_value_t *rv;
	size_t i;

	if (!ucv_is_callable(cb_func)) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid callback");
		return NULL;
	}

	if (!uc_nl_evsock_init())
		return NULL;

	if (ucv_type(groups) == UC_ARRAY) {
		for (i = 0; i < ucv_array_length(groups); i++) {
			int64_t n = ucv_int64_get(ucv_array_get(groups, i));

			if (errno || n < 0 || n >= __RTNLGRP_MAX)
				err_return(NLE_INVAL, NULL);

			uc_nl_add_group(n);
		}
	} else {
		uc_nl_add_group(RTNLGRP_LINK);
	}

	for (i = 0; i < ucv_array_length(listener_registry); i += 2) {
		if (!ucv_array_get(listener_registry, i))
			break;
	}

	l = xalloc(sizeof(*l));
	l->index = i;

	if (!uc_nl_fill_cmds(l->cmds, cmds)) {
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid command ID");
		free(l);
		return NULL;
	}

	rv = uc_resource_new(listener_type, l);

	ucv_array_set(listener_registry, i, ucv_get(rv));
	ucv_array_set(listener_registry, i + 1, ucv_get(cb_func));

	listener_vm = vm;

	return rv;
}

static void
uc_nl_listener_free(void *arg)
{
	uc_nl_listener_t *l = arg;

	if (!l)
		return;

	ucv_array_set(listener_registry, l->index, NULL);
	ucv_array_set(listener_registry, l->index + 1, NULL);
	free(l);
}

/**
 * Set the commands for a netlink listener.
 *
 * Updates the set of netlink commands that the listener will receive.
 *
 * @function module:rtnl.listener#set_commands
 *
 * @param {Array<string>} commands - Array of netlink commands to listen for
 *
 * @returns {boolean} - true if successful, false on error
 *
 * @example
 * // Update listener to only receive route messages
 * listener.set_commands([RTM_NEWROUTE, RTM_DELROUTE]);
 */
static uc_value_t *
uc_nl_listener_set_commands(uc_vm_t *vm, size_t nargs)
{
	uc_nl_listener_t *l = uc_fn_thisval("rtnl.listener");
	uc_value_t *cmds = uc_fn_arg(0);

	if (!l)
		return NULL;

	memset(l->cmds, 0, sizeof(l->cmds));
	if (!uc_nl_fill_cmds(l->cmds, cmds))
		uc_vm_raise_exception(vm, EXCEPTION_TYPE, "Invalid command ID");

	return NULL;
}

/**
 * Close a netlink listener.
 *
 * Closes the netlink listener and stops receiving messages.
 *
 * @function module:rtnl.listener#close
 *
 * @returns {boolean} - true if successful, false on error
 *
 * @example
 * // Close the listener
 * listener.close();
 */
static uc_value_t *
uc_nl_listener_close(uc_vm_t *vm, size_t nargs)
{
	uc_nl_listener_t **lptr = uc_fn_this("rtnl.listener");
	uc_nl_listener_t *l;

	if (!lptr)
		return NULL;

	l = *lptr;
	if (!l)
		return NULL;

	*lptr = NULL;
	uc_nl_listener_free(l);

	return NULL;
}


static void
register_constants(uc_vm_t *vm, uc_value_t *scope)
{
	uc_value_t *c = ucv_object_new(vm);

#define ADD_CONST(x) ucv_object_add(c, #x, ucv_int64_new(x))

	/**
	 * @typedef
	 * @name Netlink message flags
	 * @property {number} NLM_F_ACK - Request for acknowledgment
	 * @property {number} NLM_F_ACK_TLVS - Request for acknowledgment with TLVs
	 * @property {number} NLM_F_APPEND - Append to existing list
	 * @property {number} NLM_F_ATOMIC - Atomic operation
	 * @property {number} NLM_F_CAPPED - Request capped
	 * @property {number} NLM_F_CREATE - Create if not exists
	 * @property {number} NLM_F_DUMP - Dump request
	 * @property {number} NLM_F_DUMP_FILTERED - Dump filtered request
	 * @property {number} NLM_F_DUMP_INTR - Dump interrupted
	 * @property {number} NLM_F_ECHO - Echo request
	 * @property {number} NLM_F_EXCL - Exclusive creation
	 * @property {number} NLM_F_MATCH - Match request
	 * @property {number} NLM_F_MULTI - Multi-part message
	 * @property {number} NLM_F_NONREC - Non-recursive operation
	 * @property {number} NLM_F_REPLACE - Replace existing
	 * @property {number} NLM_F_REQUEST - Request message
	 * @property {number} NLM_F_ROOT - Root operation
	 * @property {number} NLM_F_STRICT_CHK - Strict checking
	 */
	ADD_CONST(NLM_F_ACK);
	ADD_CONST(NLM_F_ACK);
	ADD_CONST(NLM_F_ACK_TLVS);
	ADD_CONST(NLM_F_APPEND);
	ADD_CONST(NLM_F_ATOMIC);
	ADD_CONST(NLM_F_CAPPED);
	ADD_CONST(NLM_F_CREATE);
	ADD_CONST(NLM_F_DUMP);
	ADD_CONST(NLM_F_DUMP_FILTERED);
	ADD_CONST(NLM_F_DUMP_INTR);
	ADD_CONST(NLM_F_ECHO);
	ADD_CONST(NLM_F_EXCL);
	ADD_CONST(NLM_F_MATCH);
	ADD_CONST(NLM_F_MULTI);
	ADD_CONST(NLM_F_NONREC);
	ADD_CONST(NLM_F_REPLACE);
	ADD_CONST(NLM_F_REQUEST);
	ADD_CONST(NLM_F_ROOT);
	ADD_CONST(NLM_F_STRICT_CHK); /* custom */

	/**
	 * @typedef
	 * @name IPv6 address generation modes
	 * @property {number} IN6_ADDR_GEN_MODE_EUI64 - EUI-64 mode
	 * @property {number} IN6_ADDR_GEN_MODE_NONE - No mode
	 * @property {number} IN6_ADDR_GEN_MODE_STABLE_PRIVACY - Stable privacy mode
	 * @property {number} IN6_ADDR_GEN_MODE_RANDOM - Random mode
	 */
	ADD_CONST(IN6_ADDR_GEN_MODE_EUI64);
	ADD_CONST(IN6_ADDR_GEN_MODE_NONE);
	ADD_CONST(IN6_ADDR_GEN_MODE_STABLE_PRIVACY);
	ADD_CONST(IN6_ADDR_GEN_MODE_RANDOM);

	/**
	 * @typedef
	 * @name MACVLAN modes
	 * @property {number} MACVLAN_MODE_PRIVATE - Private mode
	 * @property {number} MACVLAN_MODE_VEPA - VEPA mode
	 * @property {number} MACVLAN_MODE_BRIDGE - Bridge mode
	 * @property {number} MACVLAN_MODE_PASSTHRU - Pass-through mode
	 * @property {number} MACVLAN_MODE_SOURCE - Source mode
	 */
	ADD_CONST(MACVLAN_MODE_PRIVATE);
	ADD_CONST(MACVLAN_MODE_VEPA);
	ADD_CONST(MACVLAN_MODE_BRIDGE);
	ADD_CONST(MACVLAN_MODE_PASSTHRU);
	ADD_CONST(MACVLAN_MODE_SOURCE);

	/**
	 * @typedef
	 * @name MACVLAN MAC address commands
	 * @property {number} MACVLAN_MACADDR_ADD - Add MAC address
	 * @property {number} MACVLAN_MACADDR_DEL - Delete MAC address
	 * @property {number} MACVLAN_MACADDR_FLUSH - Flush MAC addresses
	 * @property {number} MACVLAN_MACADDR_SET - Set MAC address
	 */
	ADD_CONST(MACVLAN_MACADDR_ADD);
	ADD_CONST(MACVLAN_MACADDR_DEL);
	ADD_CONST(MACVLAN_MACADDR_FLUSH);
	ADD_CONST(MACVLAN_MACADDR_SET);

	/**
	 * @typedef
	 * @name MACsec validation levels
	 * @property {number} MACSEC_VALIDATE_DISABLED - Disabled validation
	 * @property {number} MACSEC_VALIDATE_CHECK - Check validation
	 * @property {number} MACSEC_VALIDATE_STRICT - Strict validation
	 * @property {number} MACSEC_VALIDATE_MAX - Maximum validation
	 */
	ADD_CONST(MACSEC_VALIDATE_DISABLED);
	ADD_CONST(MACSEC_VALIDATE_CHECK);
	ADD_CONST(MACSEC_VALIDATE_STRICT);
	ADD_CONST(MACSEC_VALIDATE_MAX);

	/**
	 * @typedef
	 * @name MACsec offload modes
	 * @property {number} MACSEC_OFFLOAD_OFF - Offload off
	 * @property {number} MACSEC_OFFLOAD_PHY - Physical offload
	 * @property {number} MACSEC_OFFLOAD_MAC - MAC offload
	 * @property {number} MACSEC_OFFLOAD_MAX - Maximum offload
	 */
	ADD_CONST(MACSEC_OFFLOAD_OFF);
	ADD_CONST(MACSEC_OFFLOAD_PHY);
	ADD_CONST(MACSEC_OFFLOAD_MAC);
	ADD_CONST(MACSEC_OFFLOAD_MAX);

	/**
	 * @typedef
	 * @name IPVLAN modes
	 * @property {number} IPVLAN_MODE_L2 - Layer 2 mode
	 * @property {number} IPVLAN_MODE_L3 - Layer 3 mode
	 * @property {number} IPVLAN_MODE_L3S - Layer 3 symmetric mode
	 */
	ADD_CONST(IPVLAN_MODE_L2);
	ADD_CONST(IPVLAN_MODE_L3);
	ADD_CONST(IPVLAN_MODE_L3S);

	/**
	 * @typedef
	 * @name VXLAN data frame flags
	 * @property {number} VXLAN_DF_UNSET - Data frame unset
	 * @property {number} VXLAN_DF_SET - Data frame set
	 * @property {number} VXLAN_DF_INHERIT - Data frame inherit
	 * @property {number} VXLAN_DF_MAX - Maximum data frame
	 */
	ADD_CONST(VXLAN_DF_UNSET);
	ADD_CONST(VXLAN_DF_SET);
	ADD_CONST(VXLAN_DF_INHERIT);
	ADD_CONST(VXLAN_DF_MAX);

	/**
	 * @typedef
	 * @name Geneve data frame flags
	 * @property {number} GENEVE_DF_UNSET - Data frame unset
	 * @property {number} GENEVE_DF_SET - Data frame set
	 * @property {number} GENEVE_DF_INHERIT - Data frame inherit
	 * @property {number} GENEVE_DF_MAX - Maximum data frame
	 */
	ADD_CONST(GENEVE_DF_UNSET);
	ADD_CONST(GENEVE_DF_SET);
	ADD_CONST(GENEVE_DF_INHERIT);
	ADD_CONST(GENEVE_DF_MAX);

	/**
	 * @typedef
	 * @name GTP roles
	 * @property {number} GTP_ROLE_GGSN - GGSN role
	 * @property {number} GTP_ROLE_SGSN - SGSN role
	 */
	ADD_CONST(GTP_ROLE_GGSN);
	ADD_CONST(GTP_ROLE_SGSN);

	/**
	 * @typedef
	 * @name Port request types
	 * @property {number} PORT_REQUEST_PREASSOCIATE - Pre-associate request
	 * @property {number} PORT_REQUEST_PREASSOCIATE_RR - Pre-associate round-robin request
	 * @property {number} PORT_REQUEST_ASSOCIATE - Associate request
	 * @property {number} PORT_REQUEST_DISASSOCIATE - Disassociate request
	 */
	ADD_CONST(PORT_REQUEST_PREASSOCIATE);
	ADD_CONST(PORT_REQUEST_PREASSOCIATE_RR);
	ADD_CONST(PORT_REQUEST_ASSOCIATE);
	ADD_CONST(PORT_REQUEST_DISASSOCIATE);

	/**
	 * @typedef
	 * @name Port VDP responses
	 * @property {number} PORT_VDP_RESPONSE_SUCCESS - Success response
	 * @property {number} PORT_VDP_RESPONSE_INVALID_FORMAT - Invalid format response
	 * @property {number} PORT_VDP_RESPONSE_INSUFFICIENT_RESOURCES - Insufficient resources response
	 * @property {number} PORT_VDP_RESPONSE_UNUSED_VTID - Unused VTID response
	 * @property {number} PORT_VDP_RESPONSE_VTID_VIOLATION - VTID violation response
	 * @property {number} PORT_VDP_RESPONSE_VTID_VERSION_VIOALTION - VTID version violation response
	 * @property {number} PORT_VDP_RESPONSE_OUT_OF_SYNC - Out of sync response
	 */
	ADD_CONST(PORT_VDP_RESPONSE_SUCCESS);
	ADD_CONST(PORT_VDP_RESPONSE_INVALID_FORMAT);
	ADD_CONST(PORT_VDP_RESPONSE_INSUFFICIENT_RESOURCES);
	ADD_CONST(PORT_VDP_RESPONSE_UNUSED_VTID);
	ADD_CONST(PORT_VDP_RESPONSE_VTID_VIOLATION);
	ADD_CONST(PORT_VDP_RESPONSE_VTID_VERSION_VIOALTION);
	ADD_CONST(PORT_VDP_RESPONSE_OUT_OF_SYNC);

	/**
	 * @typedef
	 * @name Port profile responses
	 * @property {number} PORT_PROFILE_RESPONSE_SUCCESS - Success response
	 * @property {number} PORT_PROFILE_RESPONSE_INPROGRESS - In progress response
	 * @property {number} PORT_PROFILE_RESPONSE_INVALID - Invalid response
	 * @property {number} PORT_PROFILE_RESPONSE_BADSTATE - Bad state response
	 * @property {number} PORT_PROFILE_RESPONSE_INSUFFICIENT_RESOURCES - Insufficient resources response
	 * @property {number} PORT_PROFILE_RESPONSE_ERROR - Error response
	 */
	ADD_CONST(PORT_PROFILE_RESPONSE_SUCCESS);
	ADD_CONST(PORT_PROFILE_RESPONSE_INPROGRESS);
	ADD_CONST(PORT_PROFILE_RESPONSE_INVALID);
	ADD_CONST(PORT_PROFILE_RESPONSE_BADSTATE);
	ADD_CONST(PORT_PROFILE_RESPONSE_INSUFFICIENT_RESOURCES);
	ADD_CONST(PORT_PROFILE_RESPONSE_ERROR);

	/**
	 * @typedef
	 * @name IPoIB modes
	 * @property {number} IPOIB_MODE_DATAGRAM - Datagram mode
	 * @property {number} IPOIB_MODE_CONNECTED - Connected mode
	 */
	ADD_CONST(IPOIB_MODE_DATAGRAM);
	ADD_CONST(IPOIB_MODE_CONNECTED);

	/**
	 * @typedef
	 * @name HSR protocols
	 * @property {number} HSR_PROTOCOL_HSR - HSR protocol
	 * @property {number} HSR_PROTOCOL_PRP - PRP protocol
	 */
	ADD_CONST(HSR_PROTOCOL_HSR);
	ADD_CONST(HSR_PROTOCOL_PRP);

	/**
	 * @typedef
	 * @name CAN controller modes
	 * @description
	 * Flag bits used in the `mask` and `flags` members of the `ctrlmode`
	 * attribute of `can` type links.
	 * @property {number} CAN_CTRLMODE_LOOPBACK - Loopback mode
	 * @property {number} CAN_CTRLMODE_LISTENONLY - Listen-only mode
	 * @property {number} CAN_CTRLMODE_3_SAMPLES - Triple sampling mode
	 * @property {number} CAN_CTRLMODE_ONE_SHOT - One-shot mode
	 * @property {number} CAN_CTRLMODE_BERR_REPORTING - Bus error reporting
	 * @property {number} CAN_CTRLMODE_FD - CAN FD mode
	 * @property {number} CAN_CTRLMODE_PRESUME_ACK - Ignore missing CAN ACKs
	 * @property {number} CAN_CTRLMODE_FD_NON_ISO - CAN FD in non-ISO mode
	 * @property {number} CAN_CTRLMODE_CC_LEN8_DLC - Classic CAN DLC option
	 */
	ADD_CONST(CAN_CTRLMODE_LOOPBACK);
	ADD_CONST(CAN_CTRLMODE_LISTENONLY);
	ADD_CONST(CAN_CTRLMODE_3_SAMPLES);
	ADD_CONST(CAN_CTRLMODE_ONE_SHOT);
	ADD_CONST(CAN_CTRLMODE_BERR_REPORTING);
	ADD_CONST(CAN_CTRLMODE_FD);
	ADD_CONST(CAN_CTRLMODE_PRESUME_ACK);
	ADD_CONST(CAN_CTRLMODE_FD_NON_ISO);
#ifdef CAN_CTRLMODE_CC_LEN8_DLC
	ADD_CONST(CAN_CTRLMODE_CC_LEN8_DLC);
#endif
#ifdef CAN_CTRLMODE_TDC_AUTO
	ADD_CONST(CAN_CTRLMODE_TDC_AUTO);
	ADD_CONST(CAN_CTRLMODE_TDC_MANUAL);
#endif

	/**
	 * @typedef
	 * @name CAN operational states
	 * @description
	 * Values of the `state` attribute of `can` type links.
	 * @property {number} CAN_STATE_ERROR_ACTIVE - RX/TX error count < 96
	 * @property {number} CAN_STATE_ERROR_WARNING - RX/TX error count < 128
	 * @property {number} CAN_STATE_ERROR_PASSIVE - RX/TX error count < 256
	 * @property {number} CAN_STATE_BUS_OFF - RX/TX error count >= 256
	 * @property {number} CAN_STATE_STOPPED - Device is stopped
	 * @property {number} CAN_STATE_SLEEPING - Device is sleeping
	 */
	ADD_CONST(CAN_STATE_ERROR_ACTIVE);
	ADD_CONST(CAN_STATE_ERROR_WARNING);
	ADD_CONST(CAN_STATE_ERROR_PASSIVE);
	ADD_CONST(CAN_STATE_BUS_OFF);
	ADD_CONST(CAN_STATE_STOPPED);
	ADD_CONST(CAN_STATE_SLEEPING);

	/**
	 * @typedef
	 * @name Link extended statistics types
	 * @property {number} LINK_XSTATS_TYPE_UNSPEC - Unspecified type
	 * @property {number} LINK_XSTATS_TYPE_BRIDGE - Bridge type
	 * @property {number} LINK_XSTATS_TYPE_BOND - Bond type
	 */
	ADD_CONST(LINK_XSTATS_TYPE_UNSPEC);
	ADD_CONST(LINK_XSTATS_TYPE_BRIDGE);
	ADD_CONST(LINK_XSTATS_TYPE_BOND);

	/**
	 * @typedef
	 * @name XDP attach types
	 * @property {number} XDP_ATTACHED_NONE - Not attached
	 * @property {number} XDP_ATTACHED_DRV - Driver attached
	 * @property {number} XDP_ATTACHED_SKB - SKB attached
	 * @property {number} XDP_ATTACHED_HW - Hardware attached
	 * @property {number} XDP_ATTACHED_MULTI - Multi attached
	 */
	ADD_CONST(XDP_ATTACHED_NONE);
	ADD_CONST(XDP_ATTACHED_DRV);
	ADD_CONST(XDP_ATTACHED_SKB);
	ADD_CONST(XDP_ATTACHED_HW);
	ADD_CONST(XDP_ATTACHED_MULTI);

	/**
	 * @typedef
	 * @name FDB notification bits
	 * @property {number} FDB_NOTIFY_BIT - Notify bit
	 * @property {number} FDB_NOTIFY_INACTIVE_BIT - Inactive notify bit
	 */

	ADD_CONST(FDB_NOTIFY_BIT);
	ADD_CONST(FDB_NOTIFY_INACTIVE_BIT);

	/**
	 * @typedef
	 * @name Route commands
	 * @property {number} RTM_BASE - Base command
	 * @property {number} RTM_NEWLINK - New link
	 * @property {number} RTM_DELLINK - Delete link
	 * @property {number} RTM_GETLINK - Get link
	 * @property {number} RTM_SETLINK - Set link
	 * @property {number} RTM_NEWADDR - New address
	 * @property {number} RTM_DELADDR - Delete address
	 * @property {number} RTM_GETADDR - Get address
	 * @property {number} RTM_NEWROUTE - New route
	 * @property {number} RTM_DELROUTE - Delete route
	 * @property {number} RTM_GETROUTE - Get route
	 * @property {number} RTM_NEWRULE - New rule
	 * @property {number} RTM_DELRULE - Delete rule
	 * @property {number} RTM_GETRULE - Get rule
	 * @property {number} RTM_NEWQDISC - New queue discipline
	 * @property {number} RTM_DELQDISC - Delete queue discipline
	 * @property {number} RTM_GETQDISC - Get queue discipline
	 * @property {number} RTM_NEWTCLASS - New traffic class
	 * @property {number} RTM_DELTCLASS - Delete traffic class
	 * @property {number} RTM_GETTCLASS - Get traffic class
	 * @property {number} RTM_NEWTFILTER - New traffic filter
	 * @property {number} RTM_DELTFILTER - Delete traffic filter
	 * @property {number} RTM_GETTFILTER - Get traffic filter
	 * @property {number} RTM_NEWACTION - New action
	 * @property {number} RTM_DELACTION - Delete action
	 * @property {number} RTM_GETACTION - Get action
	 * @property {number} RTM_NEWPREFIX - New prefix
	 * @property {number} RTM_GETMULTICAST - Get multicast
	 * @property {number} RTM_GETANYCAST - Get anycast
	 * @property {number} RTM_NEWNEIGHTBL - New neighbor table
	 * @property {number} RTM_GETNEIGHTBL - Get neighbor table
	 * @property {number} RTM_SETNEIGHTBL - Set neighbor table
	 * @property {number} RTM_NEWNDUSEROPT - New neighbor user option
	 * @property {number} RTM_NEWADDRLABEL - New address label
	 * @property {number} RTM_DELADDRLABEL - Delete address label
	 * @property {number} RTM_GETADDRLABEL - Get address label
	 * @property {number} RTM_GETDCB - Get DCB
	 * @property {number} RTM_SETDCB - Set DCB
	 * @property {number} RTM_NEWNETCONF - New network configuration
	 * @property {number} RTM_DELNETCONF - Delete network configuration
	 * @property {number} RTM_GETNETCONF - Get network configuration
	 * @property {number} RTM_NEWMDB - New multicast database
	 * @property {number} RTM_DELMDB - Delete multicast database
	 * @property {number} RTM_GETMDB - Get multicast database
	 * @property {number} RTM_NEWNSID - New network namespace ID
	 * @property {number} RTM_DELNSID - Delete network namespace ID
	 * @property {number} RTM_GETNSID - Get network namespace ID
	 * @property {number} RTM_NEWSTATS - New statistics
	 * @property {number} RTM_GETSTATS - Get statistics
	 * @property {number} RTM_NEWCACHEREPORT - New cache report
	 * @property {number} RTM_NEWCHAIN - New chain
	 * @property {number} RTM_DELCHAIN - Delete chain
	 * @property {number} RTM_GETCHAIN - Get chain
	 * @property {number} RTM_NEWNEXTHOP - New next hop
	 * @property {number} RTM_DELNEXTHOP - Delete next hop
	 * @property {number} RTM_GETNEXTHOP - Get next hop
	 * @property {number} RTM_NEWLINKPROP - New link property
	 * @property {number} RTM_DELLINKPROP - Delete link property
	 * @property {number} RTM_GETLINKPROP - Get link property
	 * @property {number} RTM_NEWVLAN - New VLAN
	 * @property {number} RTM_DELVLAN - Delete VLAN
	 * @property {number} RTM_GETVLAN - Get VLAN
	 */
	ADD_CONST(RTM_BASE);
	ADD_CONST(RTM_NEWLINK);
	ADD_CONST(RTM_DELLINK);
	ADD_CONST(RTM_GETLINK);
	ADD_CONST(RTM_SETLINK);
	ADD_CONST(RTM_NEWADDR);
	ADD_CONST(RTM_DELADDR);
	ADD_CONST(RTM_GETADDR);
	ADD_CONST(RTM_NEWROUTE);
	ADD_CONST(RTM_DELROUTE);
	ADD_CONST(RTM_GETROUTE);
	ADD_CONST(RTM_NEWNEIGH);
	ADD_CONST(RTM_DELNEIGH);
	ADD_CONST(RTM_GETNEIGH);
	ADD_CONST(RTM_NEWRULE);
	ADD_CONST(RTM_DELRULE);
	ADD_CONST(RTM_GETRULE);
	ADD_CONST(RTM_NEWQDISC);
	ADD_CONST(RTM_DELQDISC);
	ADD_CONST(RTM_GETQDISC);
	ADD_CONST(RTM_NEWTCLASS);
	ADD_CONST(RTM_DELTCLASS);
	ADD_CONST(RTM_GETTCLASS);
	ADD_CONST(RTM_NEWTFILTER);
	ADD_CONST(RTM_DELTFILTER);
	ADD_CONST(RTM_GETTFILTER);
	ADD_CONST(RTM_NEWACTION);
	ADD_CONST(RTM_DELACTION);
	ADD_CONST(RTM_GETACTION);
	ADD_CONST(RTM_NEWPREFIX);
	ADD_CONST(RTM_GETMULTICAST);
	ADD_CONST(RTM_GETANYCAST);
	ADD_CONST(RTM_NEWNEIGHTBL);
	ADD_CONST(RTM_GETNEIGHTBL);
	ADD_CONST(RTM_SETNEIGHTBL);
	ADD_CONST(RTM_NEWNDUSEROPT);
	ADD_CONST(RTM_NEWADDRLABEL);
	ADD_CONST(RTM_DELADDRLABEL);
	ADD_CONST(RTM_GETADDRLABEL);
	ADD_CONST(RTM_GETDCB);
	ADD_CONST(RTM_SETDCB);
	ADD_CONST(RTM_NEWNETCONF);
	ADD_CONST(RTM_DELNETCONF);
	ADD_CONST(RTM_GETNETCONF);
	ADD_CONST(RTM_NEWMDB);
	ADD_CONST(RTM_DELMDB);
	ADD_CONST(RTM_GETMDB);
	ADD_CONST(RTM_NEWNSID);
	ADD_CONST(RTM_DELNSID);
	ADD_CONST(RTM_GETNSID);
	ADD_CONST(RTM_NEWSTATS);
	ADD_CONST(RTM_GETSTATS);
	ADD_CONST(RTM_NEWCACHEREPORT);
	ADD_CONST(RTM_NEWCHAIN);
	ADD_CONST(RTM_DELCHAIN);
	ADD_CONST(RTM_GETCHAIN);
	ADD_CONST(RTM_NEWNEXTHOP);
	ADD_CONST(RTM_DELNEXTHOP);
	ADD_CONST(RTM_GETNEXTHOP);
	ADD_CONST(RTM_NEWLINKPROP);
	ADD_CONST(RTM_DELLINKPROP);
	ADD_CONST(RTM_GETLINKPROP);
	ADD_CONST(RTM_NEWVLAN);
	ADD_CONST(RTM_DELVLAN);
	ADD_CONST(RTM_GETVLAN);

	/**
	 * @typedef
	 * @name Route types
	 * @property {number} RTN_UNSPEC - Unspecified route
	 * @property {number} RTN_UNICAST - Unicast route
	 * @property {number} RTN_LOCAL - Local route
	 * @property {number} RTN_BROADCAST - Broadcast route
	 * @property {number} RTN_ANYCAST - Anycast route
	 * @property {number} RTN_MULTICAST - Multicast route
	 * @property {number} RTN_BLACKHOLE - Blackhole route
	 * @property {number} RTN_UNREACHABLE - Unreachable route
	 * @property {number} RTN_PROHIBIT - Prohibited route
	 * @property {number} RTN_THROW - Throw route
	 * @property {number} RTN_NAT - NAT route
	 * @property {number} RTN_XRESOLVE - External resolve route
	 */
	ADD_CONST(RTN_UNSPEC);
	ADD_CONST(RTN_UNICAST);
	ADD_CONST(RTN_LOCAL);
	ADD_CONST(RTN_BROADCAST);
	ADD_CONST(RTN_ANYCAST);
	ADD_CONST(RTN_MULTICAST);
	ADD_CONST(RTN_BLACKHOLE);
	ADD_CONST(RTN_UNREACHABLE);
	ADD_CONST(RTN_PROHIBIT);
	ADD_CONST(RTN_THROW);
	ADD_CONST(RTN_NAT);
	ADD_CONST(RTN_XRESOLVE);

	/**
	 * @typedef
	 * @name Route scopes
	 * @property {number} RT_SCOPE_UNIVERSE - Universe scope
	 * @property {number} RT_SCOPE_SITE - Site scope
	 * @property {number} RT_SCOPE_LINK - Link scope
	 * @property {number} RT_SCOPE_HOST - Host scope
	 * @property {number} RT_SCOPE_NOWHERE - Nowhere scope
	 */
	ADD_CONST(RT_SCOPE_UNIVERSE);
	ADD_CONST(RT_SCOPE_SITE);
	ADD_CONST(RT_SCOPE_LINK);
	ADD_CONST(RT_SCOPE_HOST);
	ADD_CONST(RT_SCOPE_NOWHERE);

	/**
	 * @typedef
	 * @name Route tables
	 * @property {number} RT_TABLE_UNSPEC - Unspecified table
	 * @property {number} RT_TABLE_COMPAT - Compatibility table
	 * @property {number} RT_TABLE_DEFAULT - Default table
	 * @property {number} RT_TABLE_MAIN - Main table
	 * @property {number} RT_TABLE_LOCAL - Local table
	 * @property {number} RT_TABLE_MAX - Maximum table
	 */
	ADD_CONST(RT_TABLE_UNSPEC);
	ADD_CONST(RT_TABLE_COMPAT);
	ADD_CONST(RT_TABLE_DEFAULT);
	ADD_CONST(RT_TABLE_MAIN);
	ADD_CONST(RT_TABLE_LOCAL);
	ADD_CONST(RT_TABLE_MAX);

	/**
	 * @typedef
	 * @name Route metrics
	 * @property {number} RTAX_MTU - Maximum transmission unit
	 * @property {number} RTAX_HOPLIMIT - Hop limit
	 * @property {number} RTAX_ADVMSS - Advertised MSS
	 * @property {number} RTAX_REORDERING - Reordering
	 * @property {number} RTAX_RTT - Round trip time
	 * @property {number} RTAX_WINDOW - Window size
	 * @property {number} RTAX_CWND - Congestion window
	 * @property {number} RTAX_INITCWND - Initial congestion window
	 * @property {number} RTAX_INITRWND - Initial receive window
	 * @property {number} RTAX_FEATURES - Features
	 * @property {number} RTAX_QUICKACK - Quick acknowledgment
	 * @property {number} RTAX_CC_ALGO - Congestion control algorithm
	 * @property {number} RTAX_RTTVAR - RTT variance
	 * @property {number} RTAX_SSTHRESH - Slow start threshold
	 * @property {number} RTAX_FASTOPEN_NO_COOKIE - Fast open no cookie
	 */
	/* required to construct RTAX_LOCK */
	ADD_CONST(RTAX_MTU);
	ADD_CONST(RTAX_HOPLIMIT);
	ADD_CONST(RTAX_ADVMSS);
	ADD_CONST(RTAX_REORDERING);
	ADD_CONST(RTAX_RTT);
	ADD_CONST(RTAX_WINDOW);
	ADD_CONST(RTAX_CWND);
	ADD_CONST(RTAX_INITCWND);
	ADD_CONST(RTAX_INITRWND);
	ADD_CONST(RTAX_FEATURES);
	ADD_CONST(RTAX_QUICKACK);
	ADD_CONST(RTAX_CC_ALGO);
	ADD_CONST(RTAX_RTTVAR);
	ADD_CONST(RTAX_SSTHRESH);
	ADD_CONST(RTAX_FASTOPEN_NO_COOKIE);

	/**
	 * @typedef
	 * @name Prefix types
	 * @property {number} PREFIX_UNSPEC - Unspecified prefix
	 * @property {number} PREFIX_ADDRESS - Address prefix
	 * @property {number} PREFIX_CACHEINFO - Cache info prefix
	 */
	ADD_CONST(PREFIX_UNSPEC);
	ADD_CONST(PREFIX_ADDRESS);
	ADD_CONST(PREFIX_CACHEINFO);

	/**
	 * @typedef
	 * @name Neighbor discovery user option types
	 * @property {number} NDUSEROPT_UNSPEC - Unspecified option
	 * @property {number} NDUSEROPT_SRCADDR - Source address option
	 */
	ADD_CONST(NDUSEROPT_UNSPEC);
	ADD_CONST(NDUSEROPT_SRCADDR);

	/**
	 * @typedef
	 * @name Multicast groups
	 * @property {number} RTNLGRP_NONE - No group
	 * @property {number} RTNLGRP_LINK - Link group
	 * @property {number} RTNLGRP_NOTIFY - Notify group
	 * @property {number} RTNLGRP_NEIGH - Neighbor group
	 * @property {number} RTNLGRP_TC - Traffic control group
	 * @property {number} RTNLGRP_IPV4_IFADDR - IPv4 interface address group
	 * @property {number} RTNLGRP_IPV4_MROUTE - IPv4 multicast route group
	 * @property {number} RTNLGRP_IPV4_ROUTE - IPv4 route group
	 * @property {number} RTNLGRP_IPV4_RULE - IPv4 rule group
	 * @property {number} RTNLGRP_IPV6_IFADDR - IPv6 interface address group
	 * @property {number} RTNLGRP_IPV6_MROUTE - IPv6 multicast route group
	 * @property {number} RTNLGRP_IPV6_ROUTE - IPv6 route group
	 * @property {number} RTNLGRP_IPV6_IFINFO - IPv6 interface info group
	 * @property {number} RTNLGRP_DECnet_IFADDR - DECnet interface address group
	 * @property {number} RTNLGRP_NOP2 - No operation 2
	 * @property {number} RTNLGRP_DECnet_ROUTE - DECnet route group
	 * @property {number} RTNLGRP_DECnet_RULE - DECnet rule group
	 * @property {number} RTNLGRP_NOP4 - No operation 4
	 * @property {number} RTNLGRP_IPV6_PREFIX - IPv6 prefix group
	 * @property {number} RTNLGRP_IPV6_RULE - IPv6 rule group
	 * @property {number} RTNLGRP_ND_USEROPT - Neighbor discovery user option group
	 * @property {number} RTNLGRP_PHONET_IFADDR - Phonet interface address group
	 * @property {number} RTNLGRP_PHONET_ROUTE - Phonet route group
	 * @property {number} RTNLGRP_DCB - Data Center Bridging group
	 * @property {number} RTNLGRP_IPV4_NETCONF - IPv4 network configuration group
	 * @property {number} RTNLGRP_IPV6_NETCONF - IPv6 network configuration group
	 * @property {number} RTNLGRP_MDB - Multicast database group
	 * @property {number} RTNLGRP_MPLS_ROUTE - MPLS route group
	 * @property {number} RTNLGRP_NSID - Network namespace ID group
	 * @property {number} RTNLGRP_MPLS_NETCONF - MPLS network configuration group
	 * @property {number} RTNLGRP_IPV4_MROUTE_R - IPv4 multicast route reverse group
	 * @property {number} RTNLGRP_IPV6_MROUTE_R - IPv6 multicast route reverse group
	 * @property {number} RTNLGRP_NEXTHOP - Next hop group
	 * @property {number} RTNLGRP_BRVLAN - Bridge VLAN group
	 */
	ADD_CONST(RTNLGRP_NONE);
	ADD_CONST(RTNLGRP_LINK);
	ADD_CONST(RTNLGRP_NOTIFY);
	ADD_CONST(RTNLGRP_NEIGH);
	ADD_CONST(RTNLGRP_TC);
	ADD_CONST(RTNLGRP_IPV4_IFADDR);
	ADD_CONST(RTNLGRP_IPV4_MROUTE);
	ADD_CONST(RTNLGRP_IPV4_ROUTE);
	ADD_CONST(RTNLGRP_IPV4_RULE);
	ADD_CONST(RTNLGRP_IPV6_IFADDR);
	ADD_CONST(RTNLGRP_IPV6_MROUTE);
	ADD_CONST(RTNLGRP_IPV6_ROUTE);
	ADD_CONST(RTNLGRP_IPV6_IFINFO);
	ADD_CONST(RTNLGRP_DECnet_IFADDR);
	ADD_CONST(RTNLGRP_NOP2);
	ADD_CONST(RTNLGRP_DECnet_ROUTE);
	ADD_CONST(RTNLGRP_DECnet_RULE);
	ADD_CONST(RTNLGRP_NOP4);
	ADD_CONST(RTNLGRP_IPV6_PREFIX);
	ADD_CONST(RTNLGRP_IPV6_RULE);
	ADD_CONST(RTNLGRP_ND_USEROPT);
	ADD_CONST(RTNLGRP_PHONET_IFADDR);
	ADD_CONST(RTNLGRP_PHONET_ROUTE);
	ADD_CONST(RTNLGRP_DCB);
	ADD_CONST(RTNLGRP_IPV4_NETCONF);
	ADD_CONST(RTNLGRP_IPV6_NETCONF);
	ADD_CONST(RTNLGRP_MDB);
	ADD_CONST(RTNLGRP_MPLS_ROUTE);
	ADD_CONST(RTNLGRP_NSID);
	ADD_CONST(RTNLGRP_MPLS_NETCONF);
	ADD_CONST(RTNLGRP_IPV4_MROUTE_R);
	ADD_CONST(RTNLGRP_IPV6_MROUTE_R);
	ADD_CONST(RTNLGRP_NEXTHOP);
	ADD_CONST(RTNLGRP_BRVLAN);

	/**
	 * @typedef
	 * @name Route flags
	 * @property {number} RTM_F_CLONED - Cloned route
	 * @property {number} RTM_F_EQUALIZE - Equalize route
	 * @property {number} RTM_F_FIB_MATCH - FIB match
	 * @property {number} RTM_F_LOOKUP_TABLE - Lookup table
	 * @property {number} RTM_F_NOTIFY - Notify
	 * @property {number} RTM_F_PREFIX - Prefix
	 */
	ADD_CONST(RTM_F_CLONED);
	ADD_CONST(RTM_F_EQUALIZE);
	ADD_CONST(RTM_F_FIB_MATCH);
	ADD_CONST(RTM_F_LOOKUP_TABLE);
	ADD_CONST(RTM_F_NOTIFY);
	ADD_CONST(RTM_F_PREFIX);

	/**
	 * @typedef
	 * @name Address families
	 * @property {number} AF_UNSPEC - Unspecified address family
	 * @property {number} AF_INET - IPv4 address family
	 * @property {number} AF_INET6 - IPv6 address family
	 * @property {number} AF_MPLS - MPLS address family
	 * @property {number} AF_BRIDGE - Bridge address family
	 */
	ADD_CONST(AF_UNSPEC);
	ADD_CONST(AF_INET);
	ADD_CONST(AF_INET6);
	ADD_CONST(AF_MPLS);
	ADD_CONST(AF_BRIDGE);

	/**
	 * @typedef
	 * @name Generic Routing Encapsulation flags
	 * @property {number} GRE_CSUM - Checksum flag
	 * @property {number} GRE_ROUTING - Routing flag
	 * @property {number} GRE_KEY - Key flag
	 * @property {number} GRE_SEQ - Sequence flag
	 * @property {number} GRE_STRICT - Strict flag
	 * @property {number} GRE_REC - Record flag
	 * @property {number} GRE_ACK - Acknowledgment flag
	 */
	ADD_CONST(GRE_CSUM);
	ADD_CONST(GRE_ROUTING);
	ADD_CONST(GRE_KEY);
	ADD_CONST(GRE_SEQ);
	ADD_CONST(GRE_STRICT);
	ADD_CONST(GRE_REC);
	ADD_CONST(GRE_ACK);

	/**
	 * @typedef
	 * @name Tunnel encapsulation types
	 * @property {number} TUNNEL_ENCAP_NONE - No encapsulation
	 * @property {number} TUNNEL_ENCAP_FOU - Foo over UDP
	 * @property {number} TUNNEL_ENCAP_GUE - Generic UDP Encapsulation
	 * @property {number} TUNNEL_ENCAP_MPLS - MPLS encapsulation
	 */
	ADD_CONST(TUNNEL_ENCAP_NONE);
	ADD_CONST(TUNNEL_ENCAP_FOU);
	ADD_CONST(TUNNEL_ENCAP_GUE);
	ADD_CONST(TUNNEL_ENCAP_MPLS);

	/**
	 * @typedef
	 * @name Tunnel encapsulation flags
	 * @property {number} TUNNEL_ENCAP_FLAG_CSUM - Checksum flag
	 * @property {number} TUNNEL_ENCAP_FLAG_CSUM6 - IPv6 checksum flag
	 * @property {number} TUNNEL_ENCAP_FLAG_REMCSUM - Remote checksum flag
	 */
	ADD_CONST(TUNNEL_ENCAP_FLAG_CSUM);
	ADD_CONST(TUNNEL_ENCAP_FLAG_CSUM6);
	ADD_CONST(TUNNEL_ENCAP_FLAG_REMCSUM);

	/**
	 * @typedef
	 * @name IPv6 tunnel flags
	 * @property {number} IP6_TNL_F_ALLOW_LOCAL_REMOTE - Allow local remote
	 * @property {number} IP6_TNL_F_IGN_ENCAP_LIMIT - Ignore encapsulation limit
	 * @property {number} IP6_TNL_F_MIP6_DEV - Mobile IPv6 device
	 * @property {number} IP6_TNL_F_RCV_DSCP_COPY - Receive DSCP copy
	 * @property {number} IP6_TNL_F_USE_ORIG_FLOWLABEL - Use original flow label
	 * @property {number} IP6_TNL_F_USE_ORIG_FWMARK - Use original firewall mark
	 * @property {number} IP6_TNL_F_USE_ORIG_TCLASS - Use original traffic class
	 */
	ADD_CONST(IP6_TNL_F_ALLOW_LOCAL_REMOTE);
	ADD_CONST(IP6_TNL_F_IGN_ENCAP_LIMIT);
	ADD_CONST(IP6_TNL_F_MIP6_DEV);
	ADD_CONST(IP6_TNL_F_RCV_DSCP_COPY);
	ADD_CONST(IP6_TNL_F_USE_ORIG_FLOWLABEL);
	ADD_CONST(IP6_TNL_F_USE_ORIG_FWMARK);
	ADD_CONST(IP6_TNL_F_USE_ORIG_TCLASS);

	/**
	 * @typedef
	 * @name Interface flags
	 * @property {number} NTF_EXT_LEARNED - Externally learned
	 * @property {number} NTF_MASTER - Master interface
	 * @property {number} NTF_OFFLOADED - Offloaded
	 * @property {number} NTF_PROXY - Proxy
	 * @property {number} NTF_ROUTER - Router
	 * @property {number} NTF_SELF - Self
	 * @property {number} NTF_STICKY - Sticky
	 * @property {number} NTF_USE - Use
	 */
	ADD_CONST(NTF_EXT_LEARNED);
	ADD_CONST(NTF_MASTER);
	ADD_CONST(NTF_OFFLOADED);
	ADD_CONST(NTF_PROXY);
	ADD_CONST(NTF_ROUTER);
	ADD_CONST(NTF_SELF);
	ADD_CONST(NTF_STICKY);
	ADD_CONST(NTF_USE);

	/**
	 * @typedef
	 * @name Neighbor states
	 * @property {number} NUD_DELAY - Delay state
	 * @property {number} NUD_FAILED - Failed state
	 * @property {number} NUD_INCOMPLETE - Incomplete state
	 * @property {number} NUD_NOARP - No ARP
	 * @property {number} NUD_NONE - No state
	 * @property {number} NUD_PERMANENT - Permanent state
	 * @property {number} NUD_PROBE - Probe state
	 * @property {number} NUD_REACHABLE - Reachable state
	 * @property {number} NUD_STALE - Stale state
	 */
	ADD_CONST(NUD_DELAY);
	ADD_CONST(NUD_FAILED);
	ADD_CONST(NUD_INCOMPLETE);
	ADD_CONST(NUD_NOARP);
	ADD_CONST(NUD_NONE);
	ADD_CONST(NUD_PERMANENT);
	ADD_CONST(NUD_PROBE);
	ADD_CONST(NUD_REACHABLE);
	ADD_CONST(NUD_STALE);

	/**
	 * @typedef
	 * @name Address flags
	 * @property {number} IFA_F_DADFAILED - DAD failed
	 * @property {number} IFA_F_DEPRECATED - Deprecated
	 * @property {number} IFA_F_HOMEADDRESS - Home address
	 * @property {number} IFA_F_MANAGETEMPADDR - Manage temporary address
	 * @property {number} IFA_F_MCAUTOJOIN - Multicast auto join
	 * @property {number} IFA_F_NODAD - No DAD
	 * @property {number} IFA_F_NOPREFIXROUTE - No prefix route
	 * @property {number} IFA_F_OPTIMISTIC - Optimistic
	 * @property {number} IFA_F_PERMANENT - Permanent
	 * @property {number} IFA_F_SECONDARY - Secondary
	 * @property {number} IFA_F_STABLE_PRIVACY - Stable privacy
	 * @property {number} IFA_F_TEMPORARY - Temporary
	 * @property {number} IFA_F_TENTATIVE - Tentative
	 */
	ADD_CONST(IFA_F_DADFAILED);
	ADD_CONST(IFA_F_DEPRECATED);
	ADD_CONST(IFA_F_HOMEADDRESS);
	ADD_CONST(IFA_F_MANAGETEMPADDR);
	ADD_CONST(IFA_F_MCAUTOJOIN);
	ADD_CONST(IFA_F_NODAD);
	ADD_CONST(IFA_F_NOPREFIXROUTE);
	ADD_CONST(IFA_F_OPTIMISTIC);
	ADD_CONST(IFA_F_PERMANENT);
	ADD_CONST(IFA_F_SECONDARY);
	ADD_CONST(IFA_F_STABLE_PRIVACY);
	ADD_CONST(IFA_F_TEMPORARY);
	ADD_CONST(IFA_F_TENTATIVE);

	/**
	 * @typedef
	 * @name FIB rule flags
	 * @property {number} FIB_RULE_PERMANENT - Permanent rule
	 * @property {number} FIB_RULE_INVERT - Invert rule
	 * @property {number} FIB_RULE_UNRESOLVED - Unresolved rule
	 * @property {number} FIB_RULE_IIF_DETACHED - Interface detached
	 * @property {number} FIB_RULE_DEV_DETACHED - Device detached
	 * @property {number} FIB_RULE_OIF_DETACHED - Output interface detached
	 */
	ADD_CONST(FIB_RULE_PERMANENT);
	ADD_CONST(FIB_RULE_INVERT);
	ADD_CONST(FIB_RULE_UNRESOLVED);
	ADD_CONST(FIB_RULE_IIF_DETACHED);
	ADD_CONST(FIB_RULE_DEV_DETACHED);
	ADD_CONST(FIB_RULE_OIF_DETACHED);

	/**
	 * @typedef
	 * @name FIB rule actions
	 * @property {number} FR_ACT_TO_TBL - To table action
	 * @property {number} FR_ACT_GOTO - Goto action
	 * @property {number} FR_ACT_NOP - No operation action
	 * @property {number} FR_ACT_BLACKHOLE - Blackhole action
	 * @property {number} FR_ACT_UNREACHABLE - Unreachable action
	 * @property {number} FR_ACT_PROHIBIT - Prohibit action
	 */
	ADD_CONST(FR_ACT_TO_TBL);
	ADD_CONST(FR_ACT_GOTO);
	ADD_CONST(FR_ACT_NOP);
	ADD_CONST(FR_ACT_BLACKHOLE);
	ADD_CONST(FR_ACT_UNREACHABLE);
	ADD_CONST(FR_ACT_PROHIBIT);

	/**
	 * @typedef
	 * @name Network configuration indices
	 * @property {number} NETCONFA_IFINDEX_ALL - All interfaces
	 * @property {number} NETCONFA_IFINDEX_DEFAULT - Default interface
	 */
	ADD_CONST(NETCONFA_IFINDEX_ALL);
	ADD_CONST(NETCONFA_IFINDEX_DEFAULT);

	/**
	 * @typedef
	 * @name Bridge flags
	 * @property {number} BRIDGE_FLAGS_MASTER - Master flag
	 * @property {number} BRIDGE_FLAGS_SELF - Self flag
	 */
	ADD_CONST(BRIDGE_FLAGS_MASTER);
	ADD_CONST(BRIDGE_FLAGS_SELF);

	/**
	 * @typedef
	 * @name Bridge modes
	 * @property {number} BRIDGE_MODE_VEB - Virtual Ethernet Bridge mode
	 * @property {number} BRIDGE_MODE_VEPA - Virtual Ethernet Port Aggregator mode
	 * @property {number} BRIDGE_MODE_UNDEF - Undefined mode
	 * @property {number} BRIDGE_MODE_UNSPEC - Unspecified mode
	 * @property {number} BRIDGE_MODE_HAIRPIN - Hairpin mode
	 */
	ADD_CONST(BRIDGE_MODE_VEB);
	ADD_CONST(BRIDGE_MODE_VEPA);
	ADD_CONST(BRIDGE_MODE_UNDEF);
	ADD_CONST(BRIDGE_MODE_UNSPEC);
	ADD_CONST(BRIDGE_MODE_HAIRPIN);

	/**
	 * @typedef
	 * @name Bridge VLAN information flags
	 * @property {number} BRIDGE_VLAN_INFO_MASTER - Master VLAN info
	 * @property {number} BRIDGE_VLAN_INFO_PVID - Primary VLAN ID
	 * @property {number} BRIDGE_VLAN_INFO_UNTAGGED - Untagged VLAN
	 * @property {number} BRIDGE_VLAN_INFO_RANGE_BEGIN - Range begin
	 * @property {number} BRIDGE_VLAN_INFO_RANGE_END - Range end
	 * @property {number} BRIDGE_VLAN_INFO_BRENTRY - Bridge entry
	 */
	ADD_CONST(BRIDGE_VLAN_INFO_MASTER);
	ADD_CONST(BRIDGE_VLAN_INFO_PVID);
	ADD_CONST(BRIDGE_VLAN_INFO_UNTAGGED);
	ADD_CONST(BRIDGE_VLAN_INFO_RANGE_BEGIN);
	ADD_CONST(BRIDGE_VLAN_INFO_RANGE_END);
	ADD_CONST(BRIDGE_VLAN_INFO_BRENTRY);

	ucv_object_add(scope, "const", c);
};

static const uc_function_list_t global_fns[] = {
	{ "close",		uc_nl_close },
	{ "error",		uc_nl_error },
	{ "request",	uc_nl_request },
	{ "listener",	uc_nl_listener },
};

static const uc_function_list_t listener_fns[] = {
	{ "set_commands",	uc_nl_listener_set_commands },
	{ "close",			uc_nl_listener_close },
};

void uc_module_init(uc_vm_t *vm, uc_value_t *scope)
{
	uc_function_list_register(scope, global_fns);

	listener_type = uc_type_declare(vm, "rtnl.listener", listener_fns, uc_nl_listener_free);
	listener_registry = ucv_array_new(vm);

	uc_vm_registry_set(vm, "rtnl.registry", listener_registry);

	register_constants(vm, scope);
}

/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#define _GNU_SOURCE		// required for strchrnul()

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/socket.h>
#include <libmnl/libmnl.h>
#include <netinet/in.h>
#include <assert.h>
#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#define DP_TEST_MAX_PREFIX_STRING_LEN 100
#define DP_TEST_MAX_ROUTE_STRING_LEN 2048

#define DP_TEST_TMP_BUF 1024

struct rtvia_v6 {
	__kernel_sa_family_t rtvia_family;
	__u8 rtvia_addr[sizeof(struct in6_addr)];
} via;

struct dp_test_addr {
	int family;
	union {
		in_addr_t ipv4;
		struct in6_addr ipv6;
		uint32_t mpls;
	} addr;
};

#define DP_TEST_MAX_NHS 32
#define DP_TEST_MAX_LBLS 8

#define  VRF_DEFAULT_ID      1
#define VRF_ID_MAX 4096

typedef uint32_t label_t;

#ifndef MPLS_LS_LABEL_MASK
#define MPLS_LS_LABEL_MASK      0xFFFFF000
#endif
#ifndef MPLS_LS_LABEL_SHIFT
#define MPLS_LS_LABEL_SHIFT     12
#endif
#ifndef MPLS_LS_TC_MASK
#define MPLS_LS_TC_MASK         0x00000E00
#endif
#ifndef MPLS_LS_TC_SHIFT
#define MPLS_LS_TC_SHIFT        9
#endif
#ifndef MPLS_LS_S_MASK
#define MPLS_LS_S_MASK          0x00000100
#endif
#ifndef MPLS_LS_S_SHIFT
#define MPLS_LS_S_SHIFT         8
#endif
#ifndef MPLS_LS_TTL_MASK
#define MPLS_LS_TTL_MASK        0x000000FF
#endif
#ifndef MPLS_LS_TTL_SHIFT
#define MPLS_LS_TTL_SHIFT       0
#endif

#ifndef MPLS_LABEL_IPV4NULL
#define MPLS_LABEL_IPV4NULL		0	/* RFC3032 */
#endif
#ifndef MPLS_LABEL_RTALERT
#define MPLS_LABEL_RTALERT		1	/* RFC3032 */
#endif
#ifndef MPLS_LABEL_IPV6NULL
#define MPLS_LABEL_IPV6NULL		2	/* RFC3032 */
#endif
#ifndef MPLS_LABEL_IMPLNULL
#define MPLS_LABEL_IMPLNULL		3	/* RFC3032 */
#endif
#ifndef MPLS_LABEL_ENTROPY
#define MPLS_LABEL_ENTROPY		7	/* RFC6790 */
#endif
#ifndef MPLS_LABEL_GAL
#define MPLS_LABEL_GAL			13	/* RFC5586 */
#endif
#ifndef MPLS_LABEL_OAMALERT
#define MPLS_LABEL_OAMALERT		14	/* RFC3429 */
#endif
#ifndef MPLS_LABEL_EXTENSION
#define MPLS_LABEL_EXTENSION		15	/* RFC7274 */
#endif
#ifndef MPLS_LABEL_FIRST_UNRESERVED
#define MPLS_LABEL_FIRST_UNRESERVED	16	/* RFC3032 */
#endif

/* RTMPA_NH_FLAGS - u32 specifying zero or more flags */
enum rtmpls_payload_nh_flags {
	RTMPNF_BOS_ONLY = 0x00000001,
};

#define RTMPNF_ALL (RTMPNF_BOS_ONLY)

enum lwtunnel_encap_types {
	LWTUNNEL_ENCAP_NONE,
	LWTUNNEL_ENCAP_MPLS,
	LWTUNNEL_ENCAP_IP,
	__LWTUNNEL_ENCAP_MAX,
};

enum {
	MPLS_IPTUNNEL_UNSPEC,
	MPLS_IPTUNNEL_DST,
	__MPLS_IPTUNNEL_MAX,
};
#define MPLS_IPTUNNEL_MAX (__MPLS_IPTUNNEL_MAX - 1)

enum rtmpls_payload_attr {
	RTMPA_TYPE,
	RTMPA_NH_FLAGS,
};

/* RTMPA_TYPE - u32 specifying type */
enum rtmpls_payload_type {
	RTMPT_IP = 0x0000,	/* IPv4 or IPv6 */
	RTMPT_IPV4 = 0x0004,
	RTMPT_IPV6 = 0x0006,

	/* Other types not implemented:
	 *  - Pseudo-wire with or without control word (RFC4385)
	 *  - GAL (RFC5586)
	 */
};

enum mpls_rsvlbls_t {
	MPLS_IPV4EXPLICITNULL = 0,
	MPLS_ROUTERALERT = 1,
	MPLS_IPV6EXPLICITNULL = 2,
	MPLS_IMPLICITNULL = 3,
	MPLS_GAC_LABEL = 13,
	MPLS_FIRSTUNRESERVED = 16,
};

struct dp_test_prefix {
	struct dp_test_addr addr;
	uint8_t len;
};

struct dp_test_nh {
	char *nh_int;
	struct dp_test_addr nh_addr;
	uint8_t num_labels;
	label_t labels[DP_TEST_MAX_LBLS];
};

struct dp_test_route {
	struct dp_test_prefix prefix;
	uint32_t vrf_id;
	uint32_t tableid;
	uint32_t scope;
	uint32_t nh_cnt;
	struct dp_test_nh nh[DP_TEST_MAX_NHS];
};

/*
 * Take str and remove start_trim bytes from the start and end_trim bytes from
 * the end.
 */
void dp_test_str_trim(char *str, uint16_t start_trim, uint16_t end_trim)
{
	int i;
	int new_str_len = strlen(str) - start_trim - end_trim;

	assert(new_str_len > 0);

	for (i = 0; i < new_str_len; i++)
		str[i] = str[i + start_trim];
	str[i] = '\0';
}

static uint32_t dp_test_ipv4_addr_mask(uint8_t len)
{
	uint32_t addr = 0xFFFFFFFF >> len;
	return ntohl(~addr);
}

/*
 * Convert an IPv4 interface address into a prefix / network address
 * In: 1.2.3.4/24
 * Out: 1.2.3.0
 *
 * NOTE: Input and output addresses are in *network* byte order
 */
uint32_t dp_test_ipv4_addr_to_network(uint32_t addr, uint8_t prefix_len)
{
	return addr & dp_test_ipv4_addr_mask(prefix_len);
}

/*
 * Convert an IPv4 interface address into a subnet broadcast address
 * In: 1.2.3.4/24
 * Out: 1.2.3.255
 *
 * NOTE: Input and output addresses are in *network* byte order
 */
uint32_t dp_test_ipv4_addr_to_bcast(uint32_t addr, uint8_t prefix_len)
{
	return addr | ~dp_test_ipv4_addr_mask(prefix_len);
}

/*
 * Convert an IPv6 address and prefix length to an IPv6 network address
 *
 * In: 2001:1:1::1/64
 * Out: 2001:1:1::0
 */
void
dp_test_ipv6_addr_to_network(const struct in6_addr *addr,
			     struct in6_addr *network, uint8_t prefix_len)
{
	unsigned int i;
	uint8_t mask, bits = prefix_len;

	for (i = 0; i < 16; i++) {
		if (bits >= 8) {
			mask = 0xFF;
			bits -= 8;
		} else {
			mask = (0xFF >> (8 - bits));
			bits = 0;
		}
		network->s6_addr[i] = addr->s6_addr[i] & mask;
	}
}

bool dp_test_addr_str_to_addr(const char *addr_str, struct dp_test_addr *addr)
{
	char buf[DP_TEST_MAX_PREFIX_STRING_LEN];

	strncpy(buf, addr_str, DP_TEST_MAX_PREFIX_STRING_LEN - 1);
	buf[DP_TEST_MAX_PREFIX_STRING_LEN - 1] = '\0';

	if (inet_pton(AF_INET, buf, &addr->addr.ipv4) == 1) {
		addr->family = AF_INET;
		return true;
	} else if (inet_pton(AF_INET6, buf, &addr->addr.ipv6) == 1) {
		addr->family = AF_INET6;
		return true;
	}

	return false;
}

const char *dp_test_addr_to_str(const struct dp_test_addr *addr, char *addr_str,
				size_t addr_str_size)
{
	return inet_ntop(addr->family, &addr->addr, addr_str, addr_str_size);
}

bool
dp_test_prefix_str_to_prefix(const char *prefix, struct dp_test_prefix *pfx)
{
	char buf[DP_TEST_MAX_PREFIX_STRING_LEN];
	char *end = strchr(prefix, '/');
	bool ret;

	if (!end) {
		char *end;
		unsigned long label = strtoul(prefix, &end, 0);

		if (label >= 1 << 20 || *end)
			return false;

		pfx->addr.family = AF_MPLS;
		pfx->addr.addr.mpls = htonl(label << MPLS_LS_LABEL_SHIFT);
		pfx->len = 20;

		return true;
	}

	strncpy(buf, prefix, end - prefix);
	buf[end - prefix] = '\0';

	pfx->len = atoi(++end);
	if (pfx->len == 0)
		return false;

	ret = dp_test_addr_str_to_addr(buf, &pfx->addr);
	if (ret) {
		switch (pfx->addr.family) {
		case AF_INET:
			if (pfx->len > 32)
				return false;
			break;
		case AF_INET6:
			if (pfx->len > 128)
				return false;
			break;
		}
	}
	return ret;
}

uint8_t dp_test_addr_size(const struct dp_test_addr *addr)
{
	switch (addr->family) {
	case AF_INET:
		return sizeof(addr->addr.ipv4);
	case AF_INET6:
		return sizeof(addr->addr.ipv6);
	case AF_MPLS:
		return sizeof(addr->addr.mpls);
	default:
		return 0;
	}
}

int dp_test_intf_name2index(const char *if_name)
{
	return 1;
}

static char *dp_test_parse_dp_int(const char *int_string, char **nh_int)
{
	char buf[DP_TEST_MAX_ROUTE_STRING_LEN];
	char *start = strchr(int_string, ':');
	char *end = strchrnul(int_string, ' ');

	assert(start);
	start++;
	strncpy(buf, start, end - start);
	buf[end - start] = '\0';
	*nh_int = strdup(buf);
	return end;
}

static const char *dp_test_parse_dp_lbls(const char *lbl_string,
					 struct dp_test_nh *nh)
{
	char buf[DP_TEST_MAX_ROUTE_STRING_LEN];	/* copy for strtok */
	uint8_t num_labels = 0;
	unsigned long label;
	char *lbl_str;
	char *end;

	strncpy(buf, lbl_string, sizeof(buf));
	buf[sizeof(buf) - 1] = '\0';

	lbl_str = strtok(buf, " ");

	if (lbl_str && strncmp(lbl_str, "nh", sizeof("nh")) == 0) {
		/* start of another nexthop - return this */
		return lbl_string;
	}
	assert(!lbl_str || (strncmp(lbl_str, "lbls", sizeof("lbls")) == 0));

	while (NULL != (lbl_str = strtok(NULL, " "))) {
		if (strcmp(lbl_str, "nh") == 0) {
			/* next nh - return a pointer to
			 * its position in the original string
			 */
			nh->num_labels = num_labels;
			return lbl_string + (lbl_str - buf);
		} else if (strcmp(lbl_str, "imp-null") == 0) {
			label = MPLS_IMPLICITNULL;
		} else {
			label = strtoul(lbl_str, &end, 0);
			assert(end != lbl_str);
			assert(label < (1 << 20));
		}
		assert(num_labels < DP_TEST_MAX_LBLS - 1);
		nh->labels[num_labels++] = label;
	}
	nh->num_labels = num_labels;
	/*
	 * If we get here then we have consumed the whole string
	 * return a pointer to the end of the original string.
	 */
	return lbl_string + strlen(lbl_string);
}

static const char *dp_test_parse_dp_nh(const char *nh_string,
				       struct dp_test_nh *nh)
{
	char buf[DP_TEST_MAX_ROUTE_STRING_LEN];
	char const *str = nh_string;

	if (!*str)
		return str;

	assert(*str == 'n');
	str++;

	assert(*str == 'h');
	str++;

	assert(*str == ' ');
	str++;

	/* Looking for either an interface or an address */
	if (*str == 'i') {
		str = dp_test_parse_dp_int(str, &nh->nh_int);
	} else {
		char *end;
		int len;

		end = strchr(str, ' ');
		len = end - str;
		strncpy(buf, str, len);
		buf[len] = '\0';
		if (!dp_test_addr_str_to_addr(buf, &nh->nh_addr))
			assert(0);
		str = strchr(str, ' ');

		if (*str) {
			/* And there may be an interface too. */
			str++;
			if (*str == 'i')
				str = dp_test_parse_dp_int(str, &nh->nh_int);
		}

	}
	/*
	 * Remove any trailing whitespace
	 */
	while (*str && isspace(*str))
		str++;

	/* Be kind to callers, if empty return ->'\0' never NULL */
	assert(str);
	return str;
}

static const char *dp_test_parse_dp_table(const char *route_string,
					  uint32_t *tblid)
{
	const char *rstr = route_string;
	static const char *tblstr = "tbl:";
	uint32_t tid;

	if (strncmp(rstr, tblstr, strlen(tblstr)) != 0)
		tid = RT_TABLE_MAIN;
	else {
		char *e;
		long int v;
		const char *next = strchr(rstr, ' ');
		char buf[DP_TEST_MAX_ROUTE_STRING_LEN];

		rstr += strlen(tblstr);
		strncpy(buf, rstr, next - rstr);
		buf[next - rstr] = '\0';
		v = strtol(buf, &e, 10);
		assert((*e == '\0') && (v <= INT_MAX));
		tid = v;

		rstr = next;
		while (isspace(*rstr))
			rstr++;
	}

	*tblid = tid;
	return rstr;
}

static const char *dp_test_parse_dp_vrf(const char *vrf_string,
					uint32_t *vrf_id)
{
	const char *vrf = vrf_string;
	const char *vrfstr = "vrf:";
	long int id = VRF_DEFAULT_ID;

	/* If no 'vrf' return VRF_DEFAULT_ID */
	if (!strncmp(vrf, vrfstr, strlen(vrfstr))) {
		/* Get the id */
		char *end;

		vrf += strlen(vrfstr);
		assert(vrf);

		id = strtol(vrf, &end, 10);
		if (id <= 0 || id >= VRF_ID_MAX)
			id = VRF_DEFAULT_ID;

		vrf = end;
		while (*vrf && isspace(*vrf))
			vrf++;
		assert(vrf);
	}

	*vrf_id = id;
	return vrf;
}

static const char *dp_test_parse_dp_scope(const char *scope_string,
					  uint32_t *scope)
{
	const char *scp = scope_string;
	const char *scpstr = "scope:";
	long int val = RT_SCOPE_UNIVERSE;

	/* If no 'scope' return RT_SCOPE_UNIVERSE */
	if (!strncmp(scp, scpstr, strlen(scpstr))) {
		/* Get the id */
		char *end;

		scp += strlen(scpstr);
		assert(scp);

		val = strtol(scp, &end, 10);
		if (val < RT_SCOPE_UNIVERSE || val > RT_SCOPE_NOWHERE)
			val = RT_SCOPE_UNIVERSE;

		scp = end;
		while (*scp && isspace(*scp))
			scp++;
		assert(scp);
	}

	*scope = val;
	return scp;
}

static const char *dp_test_parse_dp_prefix(const char *prefix_string,
					   struct dp_test_prefix *prefix)
{
	char buf[DP_TEST_MAX_ROUTE_STRING_LEN];
	/* Find the first space */
	const char *end = strchr(prefix_string, ' ');
	int ret;

	assert(end);
	strncpy(buf, prefix_string, end - prefix_string);

	/* Prefix is now in buf. */
	buf[end - prefix_string] = '\0';
	ret = dp_test_prefix_str_to_prefix(buf, prefix);
	assert(ret);
	end++;

	/*
	 * trim whitespace - we must have at least nh after that so we can't
	 * be at the end.
	 */
	while (isspace(*end))
		end++;
	assert(*end);

	return end;
}

struct dp_test_route *dp_test_parse_route(const char *route_string)
{
	struct dp_test_route *route = calloc(sizeof(*route), 1);
	/* Populate VRF id, if present in string. Otherwise assign default */
	const char *end = dp_test_parse_dp_vrf(route_string, &route->vrf_id);

	/* Populate tabel id, if present in string. Otherwise assign default */
	end = dp_test_parse_dp_table(end, &route->tableid);

	/* Populate prefix */
	end = dp_test_parse_dp_prefix(end, &route->prefix);

	/* Populate scope, if present in string. Otherwise assign default */
	end = dp_test_parse_dp_scope(end, &route->scope);

	/* get nexthops until we reach end of route string. */
	do {
		end = dp_test_parse_dp_nh(end, &route->nh[route->nh_cnt]);
		end = dp_test_parse_dp_lbls(end, &route->nh[route->nh_cnt]);
		route->nh_cnt++;
	} while (*end);
	return route;
}

void dp_test_free_route(struct dp_test_route *route)
{
	unsigned int i;

	for (i = 0; i < route->nh_cnt; i++)
		free(route->nh[i].nh_int);
	free(route);
}

/*
 * Add/delete a route, if verify is set then block until oper-state reflects
 * the requested state.
 */
static struct nlmsghdr *dp_test_netlink_route(const char *route_string,
					      uint16_t nl_type, bool replace,
					      char *buf)
{
	struct dp_test_route *route = dp_test_parse_route(route_string);
	struct rtmsg *rtm;
	struct nlmsghdr *nlh;
	unsigned int i;
	struct dp_test_nh *nh;
	unsigned int route_cnt;
	unsigned int route_idx;

	if (route->prefix.addr.family == AF_INET6)
		route_cnt = route->nh_cnt;
	else
		route_cnt = 1;

	for (route_idx = 0; route_idx < route_cnt; route_idx++) {
		memset(buf, 0, 1024);
		nlh = mnl_nlmsg_put_header(buf);
		switch (nl_type) {
		case RTM_NEWROUTE:
		case RTM_DELROUTE:
			nlh->nlmsg_type = nl_type;
			break;
		default:
			assert(false);
			break;
		}
		nlh->nlmsg_flags = NLM_F_ACK;
		if (route_idx == 0 && replace)
			nlh->nlmsg_flags |= NLM_F_REPLACE;

		rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtmsg));
		rtm->rtm_family = route->prefix.addr.family;
		rtm->rtm_dst_len = route->prefix.len;
		rtm->rtm_src_len = 0;
		rtm->rtm_tos = 0;
		rtm->rtm_table = route->tableid;
		rtm->rtm_protocol = RTPROT_UNSPEC;
		rtm->rtm_scope = route->scope;
		rtm->rtm_type = RTN_UNICAST;
		rtm->rtm_flags = 0;

		/*
		 * RTA_UNSPEC,
		 * RTA_DST,
		 *  ---- RTA_SRC,
		 *  ---- RTA_IIF,
		 * RTA_OIF,      (depending on route type)
		 * RTA_GATEWAY,  (depending on route type)
		 *  ---- RTA_PRIORITY,
		 *  ---- RTA_PREFSRC,
		 *  ---- RTA_METRICS,
		 *  ---- RTA_MULTIPATH,
		 *  ---- RTA_PROTOINFO, // no longer used
		 *  ---- RTA_FLOW,
		 *  ---- RTA_CACHEINFO,
		 *  ---- RTA_SESSION, // no longer used
		 *  ---- RTA_MP_ALGO, // no longer used
		 * RTA_ENCAP_TYPE, (if outlabels present)
		 * RTA_ENCAP,    (if outlabels present)
		 * RTA_TABLE,
		 *  ---- RTA_MARK,
		 *  ---- RTA_MFC_STATS,
		 */
		mnl_attr_put(nlh, RTA_DST,
			     dp_test_addr_size(&route->prefix.addr),
			     &route->prefix.addr.addr);

		if (route->prefix.addr.family != AF_INET6 &&
		    route->nh_cnt > 1) {
			struct nlattr *mpath_start;
			struct rtnexthop *rtnh;

			mpath_start = mnl_attr_nest_start(nlh, RTA_MULTIPATH);
			for (i = 0; i < route->nh_cnt; i++) {
				nh = &route->nh[i];
				/*
				 * insert an rtnh struct - this is not a
				 * netlink attribute so we can't just use the
				 * attr_put func.
				 */
				rtnh = (struct rtnexthop *)
				    mnl_nlmsg_get_payload_tail(nlh);
				nlh->nlmsg_len += MNL_ALIGN(sizeof(*rtnh));
				memset(rtnh, 0, sizeof(*rtnh));
				assert(nh->nh_int);
				rtnh->rtnh_ifindex =
				    dp_test_intf_name2index(nh->nh_int);
				/* if we have a nh insert a gateway attr */
				if (nh->nh_addr.family != AF_UNSPEC) {
					size_t addr_size =
					    dp_test_addr_size(&nh->nh_addr);

					if (route->prefix.addr.family ==
					    nh->nh_addr.family) {
						mnl_attr_put(nlh, RTA_GATEWAY,
							     addr_size,
							     &nh->nh_addr.addr);
					} else {
						via.rtvia_family =
						    nh->nh_addr.family;
						memcpy(via.rtvia_addr,
						       &nh->nh_addr.addr,
						       addr_size);
						mnl_attr_put(nlh, RTA_VIA,
						     offsetof(struct rtvia_v6,
							rtvia_addr[addr_size]),
							     &via);
					}
				}

				if (nh->num_labels == 1 &&
				    nh->labels[0] == MPLS_LABEL_IMPLNULL) {
					/* Nothing to do */
				} else if (nh->num_labels > 0) {
					label_t labels[DP_TEST_MAX_LBLS];
					struct nlattr *encap_start;
					uint8_t i;
					/*
					 * We send labels in network
					 * format - values occupying
					 * top 20 bits, BOS bit set on
					 * the last one, network byte
					 * order.
					 */
					for (i = 0; i < nh->num_labels; i++) {
						labels[i] =
						    htonl(nh->labels[i] <<
							  MPLS_LS_LABEL_SHIFT);
					}
					labels[nh->num_labels - 1] |=
					    htonl(1 << MPLS_LS_S_SHIFT);

					if (route->prefix.addr.family ==
					    AF_MPLS) {
						mnl_attr_put(nlh, RTA_NEWDST,
							     nh->num_labels *
							     sizeof(labels[0]),
							     labels);
					} else {
						mnl_attr_put_u16(nlh,
							 RTA_ENCAP_TYPE,
							 LWTUNNEL_ENCAP_MPLS);

						encap_start =
						    mnl_attr_nest_start(nlh,
								RTA_ENCAP);
						mnl_attr_put(nlh,
							     MPLS_IPTUNNEL_DST,
							     nh->num_labels *
							     sizeof(labels[0]),
							     labels);
						mnl_attr_nest_end(nlh,
								  encap_start);
					}
				}
				/*
				 * length of rtnh includes any gateway
				 * attribute
				 */
				rtnh->rtnh_len =
				    ((char *)mnl_nlmsg_get_payload_tail(nlh) -
				     (char *)rtnh);
			}
			mnl_attr_nest_end(nlh, mpath_start);
		} else {
			nh = &route->nh[route_idx];
			if (nh->nh_int)
				mnl_attr_put_u32(nlh, RTA_OIF,
					 dp_test_intf_name2index(nh->nh_int));
			if (nh->nh_int)
				mnl_attr_put_u32(nlh, RTA_OIF,
					 dp_test_intf_name2index(nh->nh_int));
			if (nh->nh_addr.family != AF_UNSPEC) {
				size_t addr_size =
				    dp_test_addr_size(&nh->nh_addr);

				if (route->prefix.addr.family ==
				    nh->nh_addr.family) {
					mnl_attr_put(nlh, RTA_GATEWAY,
						     addr_size,
						     &nh->nh_addr.addr);
				} else {
					struct rtvia_v6 {
						__kernel_sa_family_t
						    rtvia_family;
						__u8 rtvia_addr[16];
					} via;

					via.rtvia_family = nh->nh_addr.family;
					memcpy(via.rtvia_addr,
					       &nh->nh_addr.addr, addr_size);
					mnl_attr_put(nlh, RTA_VIA,
						     offsetof(struct rtvia_v6,
							rtvia_addr[addr_size]),
						     &via);
				}
			}
			if (nh->num_labels == 1 &&
			    nh->labels[0] == MPLS_LABEL_IMPLNULL) {
				/* Nothing to do */
			} else if (nh->num_labels > 0) {
				label_t labels[DP_TEST_MAX_LBLS];
				struct nlattr *encap_start;
				uint8_t i;
				/*
				 * We send labels in network format - values
				 * occupying top 20 bits, BOS bit set on the
				 * last one, network byte order.
				 */
				for (i = 0; i < nh->num_labels; i++) {
					labels[i] = htonl(nh->labels[i] <<
							  MPLS_LS_LABEL_SHIFT);
				}
				labels[nh->num_labels - 1] |=
				    htonl(1 << MPLS_LS_S_SHIFT);

				if (route->prefix.addr.family == AF_MPLS) {
					mnl_attr_put(nlh, RTA_NEWDST,
						     nh->num_labels *
						     sizeof(labels[0]), labels);
				} else {
					mnl_attr_put_u16(nlh, RTA_ENCAP_TYPE,
							 LWTUNNEL_ENCAP_MPLS);

					encap_start =
					    mnl_attr_nest_start(nlh, RTA_ENCAP);
					mnl_attr_put(nlh, MPLS_IPTUNNEL_DST,
						     nh->num_labels *
						     sizeof(labels[0]), labels);
					mnl_attr_nest_end(nlh, encap_start);
				}
			}
		}

		mnl_attr_put_u32(nlh, RTA_TABLE, route->tableid);
	}

	dp_test_free_route(route);
	return nlh;
}

struct nlmsghdr *netlink_del_route(char *buf, const char *format, ...)
{
	char cmd[1000];
	va_list ap;

	va_start(ap, format);
	vsnprintf(cmd, sizeof(cmd), format, ap);
	va_end(ap);

	return dp_test_netlink_route(cmd, RTM_DELROUTE, false, buf);
}

struct nlmsghdr *netlink_add_route(char *buf, const char *format, ...)
{
	char cmd[1000];
	va_list ap;

	va_start(ap, format);
	vsnprintf(cmd, sizeof(cmd), format, ap);
	va_end(ap);

	return dp_test_netlink_route(cmd, RTM_NEWROUTE, false, buf);
}

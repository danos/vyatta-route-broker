/*
 * Copyright (c) 2018, 2020 AT&T Intellectual Property. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <assert.h>
#include <arpa/inet.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "route_broker.h"
#include "brokerd.h"
#include "fpm.h"

/* Version of FPM we support */
#define BROKER_FPM_VERSION	1

static const char * const nlmsg_type_str[] = {
	[RTM_NEWROUTE] = "newroute",
	[RTM_DELROUTE] = "delroute",
};

static const char *nlmsg_type2str(uint type)
{
	if (type < ARRAY_SIZE(nlmsg_type_str) && nlmsg_type_str[type])
		return nlmsg_type_str[type];
	return "unknown";
}

static const char * const rtm_type_str[] = {
	[RTN_UNSPEC] = "unspec",
	[RTN_UNICAST] = "unicast",
	[RTN_LOCAL] = "local",
	[RTN_BLACKHOLE] = "blackhole",
	[RTN_UNREACHABLE] = "unreachable",
};

static const char *rtm_type2str(uint type)
{
	if (type < ARRAY_SIZE(rtm_type_str) && rtm_type_str[type])
		return rtm_type_str[type];
	return "unknown";
}

static const char * const rtm_table_str[] = {
	[RT_TABLE_UNSPEC] = "unspec",
	[RT_TABLE_COMPAT] = "compat",
	[RT_TABLE_DEFAULT] = "default",
	[RT_TABLE_MAIN] = "main",
	[RT_TABLE_LOCAL] = "local",
};

static const char *rtm_table2str(uint table)
{
	if (table < ARRAY_SIZE(rtm_table_str) && rtm_table_str[table])
		return rtm_table_str[table];
	return "unknown";
}

static const char * const rtm_proto_str[] = {
	[RTPROT_UNSPEC] = "unspecified",
	[RTPROT_KERNEL] = "kernel",
	[RTPROT_STATIC] = "static",
	[RTPROT_ZEBRA] = "zebra",
};

static const char *rtm_proto2str(uint proto)
{
	if (proto < ARRAY_SIZE(rtm_proto_str) && rtm_proto_str[proto])
		return rtm_proto_str[proto];
	return "unknown";
}

static const char * const rtm_scope_str[] = {
	[RT_SCOPE_UNIVERSE] = "universe",
	[RT_SCOPE_LINK] = "link",
	[RT_SCOPE_HOST] = "host",
};

static const char *rtm_scope2str(uint scope)
{
	if (scope < ARRAY_SIZE(rtm_scope_str) && rtm_scope_str[scope])
		return rtm_scope_str[scope];
	return "unknown";
}

static const char * const rtm_af_str[] = {
	[AF_INET] = "ipv4",
	[AF_INET6] = "ipv6",
	[AF_MPLS] = "mpls",
};

static const char *rtm_af2str(uint af)
{
	if (af < ARRAY_SIZE(rtm_af_str) && rtm_af_str[af])
		return rtm_af_str[af];
	return "unknown";
}

static const char * const rtm_attr_str[] = {
	[RTA_DST] = "dst",
	[RTA_SRC] = "src",
	[RTA_OIF] = "oif",
	[RTA_GATEWAY] = "gate",
	[RTA_PRIORITY] = "prio",
	[RTA_MULTIPATH] = "mpath",
};

static const char *rtm_attr2str(uint attr)
{
	if (attr < ARRAY_SIZE(rtm_attr_str) && rtm_attr_str[attr])
		return rtm_attr_str[attr];
	return "unknown";
}

static int
dump_rtattr(const struct nlattr *attr, void *data)
{
	char ipstr[INET6_ADDRSTRLEN];
	const struct rtmsg *rtm = data;
	uint type = mnl_attr_get_type(attr);

	fprintf(stderr, "  %s(%u):\t", rtm_attr2str(type), type);
	switch (type) {
	case RTA_DST:
	case RTA_GATEWAY:
		if (inet_ntop(rtm->rtm_family, mnl_attr_get_payload(attr),
			      ipstr, sizeof(ipstr)))
			fprintf(stderr, "%s/%u\n", ipstr, rtm->rtm_dst_len);
		else
			fprintf(stderr, "\n");
		break;
	case RTA_PRIORITY:
	case RTA_OIF:
		fprintf(stderr, "%u\n", mnl_attr_get_u32(attr));
		break;
	default:
		fprintf(stderr, "\n");
		break;
	}
	return MNL_CB_OK;
}

static void
dump_rtmsg(const struct nlmsghdr *nlh)
{
	struct rtmsg *rtm;
	uint rlen;

	rtm = NLMSG_DATA(nlh);
	rlen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*rtm));
	fprintf(stderr,
		"[%s(%u), len %u]: af %s(%u) type %s(%u), table %s(%u), "
		"proto %s(%u), scope %s(%u)\n",
		nlmsg_type2str(nlh->nlmsg_type), nlh->nlmsg_type, rlen,
		rtm_af2str(rtm->rtm_family), rtm->rtm_family,
		rtm_type2str(rtm->rtm_type), rtm->rtm_type,
		rtm_table2str(rtm->rtm_table), rtm->rtm_table,
		rtm_proto2str(rtm->rtm_protocol), rtm->rtm_protocol,
		rtm_scope2str(rtm->rtm_scope), rtm->rtm_scope);

	mnl_attr_parse(nlh, sizeof(*rtm), dump_rtattr, rtm);
}

static void
process_rtnl(const struct nlmsghdr *nlh)
{
	enum route_priority route_priority;
	struct rtmsg *rtm;

	if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*rtm))) {
		fprintf(stderr, "[%s(%u), len %u]: too short\n",
			nlmsg_type2str(nlh->nlmsg_type), nlh->nlmsg_type,
			nlh->nlmsg_len);
		return;
	}

	if (broker_debug)
		dump_rtmsg(nlh);

	rtm = NLMSG_DATA(nlh);
	if (rtm->rtm_protocol == RTPROT_KERNEL) {
		route_priority = ROUTE_CONNECTED;
		/*
		 * Connected IPv4 routes are received from both kernel and FPM,
		 * but they are link scope from kernel and universe from FPM
		 * which results in 2 entries in the dataplane.
		 * Just make them all universe scope to avoid duplicates.
		 * Not applicable to IPv6 where kernel connected routes are
		 * universe scope too.
		 */
		if (rtm->rtm_family == AF_INET &&
		    rtm->rtm_scope == RT_SCOPE_LINK)
			rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	} else
		route_priority = ROUTE_OTHER;

	if (rtm->rtm_table == RT_TABLE_UNSPEC)
		rtm->rtm_table = RT_TABLE_MAIN;

	route_broker_publish(nlh, route_priority);
}

static void
process_nlmsg(void *buf, size_t len)
{
	struct nlmsghdr *nlh;

	for (nlh = buf; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
		switch (nlh->nlmsg_type) {
		case RTM_NEWROUTE:
			/* Must be a replace or dataplane won't update */
			nlh->nlmsg_flags |= NLM_F_REPLACE;
			/*FALLTHRU*/
		case RTM_DELROUTE:
			process_rtnl(nlh);
			break;
		default:
			break;
		}
	}
}

ssize_t
broker_process_fpm(int fd)
{
	fpm_msg_hdr_t *fpm;
	char buf[FPM_MAX_MSG_LEN];
	ssize_t l, n;

	/* Read the FPM header */
	fpm = (fpm_msg_hdr_t *)buf;
	for (l = 0; l < (ssize_t)sizeof(*fpm); l += n) {
		n = recv(fd, buf + l, sizeof(*fpm) - l, 0);
		if (n <= 0) {
			if (n < 0)
				perror("FPM recv");
			return n;
		}
	}

	if (!fpm_msg_hdr_ok(fpm)) {
		fprintf(stderr, "corrupt FPM header\n");
		return -1;
	}

	if (fpm->version != BROKER_FPM_VERSION) {
		fprintf(stderr, "unknown FPM version %u\n", fpm->version);
		return -1;
	}

	if (fpm->msg_type != FPM_MSG_TYPE_NETLINK) {
		fprintf(stderr, "unexpected FPM message type %u\n",
			fpm->msg_type);
		return -1;
	}

	if (fpm_msg_len(fpm) > sizeof(buf)) {
		fprintf(stderr, "FPM message too big for buffer %lu > %lu\n",
			fpm_msg_len(fpm), sizeof(buf));
		return -1;
	}

	/* Read the rest of the message */
	for (; l < (ssize_t)fpm_msg_len(fpm); l += n) {
		n = recv(fd, buf + l, fpm_msg_len(fpm) - l, 0);
		if (n <= 0) {
			if (n < 0)
				perror("FPM recv");
			return n;
		}
	}

	if (broker_debug)
		fprintf(stderr, "Received %lu bytes from FPM\n", n);

	process_nlmsg(fpm_msg_data(fpm), fpm_msg_data_len(fpm));

	if (broker_debug)
		route_broker_show(broker_log_debug, NULL);

	return n;
}

ssize_t
broker_process_nl(int fd)
{
	char buf[BUFSIZ];
	ssize_t n;

	n = recv(fd, buf, sizeof(buf), 0);
	if (n <= 0) {
		if (n < 0)
			perror("NL recv");
		return n;
	}

	if (broker_debug)
		fprintf(stderr, "Received %ld bytes from NL\n", n);

	process_nlmsg(buf, n);

	if (broker_debug)
		route_broker_show(broker_log_debug, NULL);

	return n;
}

static int
dump_route(const struct nlmsghdr *nlh, void *arg)
{
	struct rtmsg *rtm = NLMSG_DATA(nlh);

	/* Handle kernel routes only - others come from FPM */
	if (nlh->nlmsg_type == RTM_NEWROUTE &&
	    nlh->nlmsg_len >= NLMSG_LENGTH(sizeof(*rtm))) {
		if (rtm->rtm_protocol == RTPROT_KERNEL)
			process_rtnl(nlh);
		else if (broker_debug) {
			fprintf(stderr, "ignore non-kernel dump of %u bytes:\n",
				nlh->nlmsg_len);
			dump_rtmsg(nlh);
		}
	} else if (broker_debug)
		fprintf(stderr, "undecodable dump of type %s(%u), len %u\n",
			nlmsg_type2str(nlh->nlmsg_type), nlh->nlmsg_type,
			nlh->nlmsg_len);
	return MNL_CB_OK;
}

static void
dump_af_routes(struct mnl_socket *nl, int seq, int af)
{
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	struct rtgenmsg *rtg;
	ssize_t len;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_GETROUTE;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = seq;

	rtg = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtg));
	rtg->rtgen_family = af;
	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		perror("mnl_socket_sendto");
		exit(1);
	}

	for (;;) {
		int ret;

		len = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (len < 0) {
			perror("mnl_socket_recvfrom");
			exit(1);
		}

		if (broker_debug)
			fprintf(stderr, "got dump of %lu bytes\n", len);
		ret = mnl_cb_run(buf, len, 0, 0, dump_route, NULL);
		if (ret <= MNL_CB_STOP)
			break;
	}
}

void
broker_dump_routes(void)
{
	struct mnl_socket *nl;
	int seq = 0;

	if (broker_debug)
		fprintf(stderr, "Dumping routes\n");

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (!nl) {
		perror("mnl_socket_open");
		exit(1);
	}

	dump_af_routes(nl, ++seq, AF_INET);
	dump_af_routes(nl, ++seq, AF_INET6);
	mnl_socket_close(nl);

	if (broker_debug) {
		fprintf(stderr, "Dump complete\n");
		route_broker_show(broker_log_debug, NULL);
	}
}

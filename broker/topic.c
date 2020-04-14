/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/socket.h>
#include <libmnl/libmnl.h>
#include <netinet/in.h>
#include <assert.h>
#include <arpa/inet.h>

#ifdef RTNLGRP_RTDMN
#include <linux/rtg_domains.h>
#endif /* RTNLGRP_RTDMN */

#ifdef RTNLGRP_MPLS_ROUTE
#include <linux/mpls.h>
#include <linux/mpls_iptunnel.h>
#include <linux/lwtunnel.h>
#endif /* RTNLGRP_MPLS_ROUTE */

#include "route_broker_internal.h"

/* All zero's in IPv4 or IPv6 */
static const char anyaddr[16];

static int route_attr(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	unsigned int type = mnl_attr_get_type(attr);

	if (type <= RTA_MAX)
		tb[type] = attr;
	return MNL_CB_OK;
}

static const char *mroute_ntop(int af, const void *src,
			       char *dst, socklen_t size)
{
	switch (af) {
	case RTNL_FAMILY_IPMR:
		return inet_ntop(AF_INET, src, dst, size);

	case RTNL_FAMILY_IP6MR:
		return inet_ntop(AF_INET6, src, dst, size);
	}
	return NULL;
}

static int mroute_topic(const struct nlmsghdr *nlh, char *buf, size_t len,
			const struct rtmsg *rtm)
{
	struct nlattr *tb[RTA_MAX + 1] = { NULL };
	int ifindex = 0, oifindex = 0;
	const void *mcastgrp, *origin;
	char b1[INET6_ADDRSTRLEN], b2[INET6_ADDRSTRLEN];
	uint32_t tableid;

	if (rtm->rtm_table == RT_TABLE_LOCAL)
		return -1;

	if (mnl_attr_parse(nlh, sizeof(*rtm), route_attr, tb) != MNL_CB_OK)
		return -1;

	if (tb[RTA_DST])
		mcastgrp = mnl_attr_get_payload(tb[RTA_DST]);
	else
		mcastgrp = anyaddr;

	if (tb[RTA_SRC])
		origin = mnl_attr_get_payload(tb[RTA_SRC]);
	else
		origin = anyaddr;

	if (tb[RTA_IIF])
		ifindex = mnl_attr_get_u32(tb[RTA_IIF]);

	if (tb[RTA_OIF])
		oifindex = mnl_attr_get_u32(tb[RTA_OIF]);

	if (tb[RTA_TABLE])
		tableid = mnl_attr_get_u32(tb[RTA_TABLE]);
	else
		tableid = rtm->rtm_table;

#ifdef RTNLGRP_RTDMN
	unsigned int rd_id = VRF_ID_MAIN;
	if (tb[RTA_RTG_DOMAIN])
		rd_id = mnl_attr_get_u32(tb[RTA_RTG_DOMAIN]);

	return snprintf(buf, len, "route %d %d %s/%u %s/%u %u %u",
			ifindex, oifindex,
			mroute_ntop(rtm->rtm_family, mcastgrp, b1, sizeof(b1)),
			rtm->rtm_dst_len,
			mroute_ntop(rtm->rtm_family, origin, b2, sizeof(b2)),
			rtm->rtm_src_len, rd_id, tableid);
#else
	return snprintf(buf, len, "route %d %d %s/%u %s/%u %u",
			ifindex, oifindex,
			mroute_ntop(rtm->rtm_family, mcastgrp, b1, sizeof(b1)),
			rtm->rtm_dst_len,
			mroute_ntop(rtm->rtm_family, origin, b2, sizeof(b2)),
			rtm->rtm_src_len, tableid);
#endif
}

#ifdef RTNLGRP_MPLS_ROUTE
static inline uint32_t mpls_ls_get_label(uint32_t ls)
{
	return (ntohl(ls) & MPLS_LS_LABEL_MASK) >> MPLS_LS_LABEL_SHIFT;
}

static int mplsroute_topic(const struct nlmsghdr *nlh, char *buf, size_t len,
			   const struct rtmsg *rtm)
{
	struct nlattr *tb[RTA_MAX + 1] = { NULL };
	uint32_t in_label;

	if (mnl_attr_parse(nlh, sizeof(*rtm), route_attr, tb) != MNL_CB_OK)
		return -1;

	if (tb[RTA_DST])
		in_label = mnl_attr_get_u32(tb[RTA_DST]);
	else
		return -1;

	return snprintf(buf, len, "route-mpls %u", mpls_ls_get_label(in_label));
}
#endif /* RTNLGRP_MPLS_ROUTE */

int route_topic(void *obj, char *buf, size_t len, bool *del)
{
	const struct nlmsghdr *nlh = obj;
	const struct rtmsg *rtm = mnl_nlmsg_get_payload(nlh);
	struct nlattr *tb[RTA_MAX + 1] = { NULL };
	const void *dest;
	char b1[INET6_ADDRSTRLEN];
	uint32_t tableid;

	switch (nlh->nlmsg_type) {
	case RTM_NEWROUTE:
		*del = false;
		break;
	case RTM_DELROUTE:
		*del = true;
		break;
	default:
		return -1;
	}

#ifdef RTNLGRP_MPLS_ROUTE
	if (rtm->rtm_family == AF_MPLS)
		return mplsroute_topic(nlh, buf, len, rtm);
#endif /* RTNLGRP_MPLS_ROUTE */

	if (rtm->rtm_type == RTN_MULTICAST)
		return mroute_topic(nlh, buf, len, rtm);

	if (rtm->rtm_type == RTN_BROADCAST)
		return -1;

	/* Ignore cached host routes */
	if (rtm->rtm_flags & RTM_F_CLONED)
		return -1;

	if (mnl_attr_parse(nlh, sizeof(*rtm), route_attr, tb) != MNL_CB_OK)
		return -1;

	if (tb[RTA_DST])
		dest = mnl_attr_get_payload(tb[RTA_DST]);
	else
		dest = anyaddr;

	if (tb[RTA_TABLE])
		tableid = mnl_attr_get_u32(tb[RTA_TABLE]);
	else
		tableid = rtm->rtm_table;

#ifdef RTNLGRP_RTDMN
	unsigned int rd_id = VRF_ID_MAIN;
	if (tb[RTA_RTG_DOMAIN])
		rd_id = mnl_attr_get_u32(tb[RTA_RTG_DOMAIN]);

	return snprintf(buf, len,
			"r %s/%u %u %u %u",
			inet_ntop(rtm->rtm_family, dest, b1, sizeof(b1)),
			rtm->rtm_dst_len, rtm->rtm_scope, tableid, rd_id);
#else
	return snprintf(buf, len,
			"r %s/%u %u %u",
			inet_ntop(rtm->rtm_family, dest, b1, sizeof(b1)),
			rtm->rtm_dst_len, rtm->rtm_scope, tableid);

#endif
}

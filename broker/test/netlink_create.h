/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

struct nlmsghdr *netlink_add_route(char *buf, const char *format, ...);
struct nlmsghdr *netlink_del_route(char *buf, const char *format, ...);

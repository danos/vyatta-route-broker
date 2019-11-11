/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef __ROUTE_BROKER_H__
#define __ROUTE_BROKER_H__

#include <linux/netlink.h>

enum route_priority {
	ROUTE_CONNECTED = 0,
	ROUTE_IGP = 1,
	ROUTE_OTHER = 2,
	ROUTE_PRIORITY_MAX = 3,
};

/*
 * Callback for formatted show/log output
 */
typedef void (*route_broker_fmt_cb) (void *arg, const char *fmt, ...);

/*
 * Callback for kernel publish
 */
typedef int (*route_broker_kernel_publish_cb) (struct nlmsghdr *nlh);

struct route_broker_init {
	/* NULL if no kernel publish required */
	route_broker_kernel_publish_cb kernel_publish;

	/* Debug logging */
	route_broker_fmt_cb log_debug;

	/* Error logging */
	route_broker_fmt_cb log_error;

	/* Argument to provide with log callbacks */
	void *log_arg;
};

/*
 * Take a netlink route message. Parse it to check it is a route, and if it
 * is then update the broker with it. This can be either an add, modify
 * or delete.
 */
void route_broker_publish(const struct nlmsghdr *nlmsg, enum route_priority);

void route_broker_show(route_broker_fmt_cb cli_out, void *cli);
void route_broker_show_summary(route_broker_fmt_cb cli_out, void *cli);

/* Init broker and vplaned broker client */
int route_broker_init_all(const struct route_broker_init *init);
void route_broker_shutdown_all(void);

#endif /* __ROUTE_BROKER_H__ */

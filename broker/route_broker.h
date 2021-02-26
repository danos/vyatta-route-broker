/*-
 * Copyright (c) 2018,2021 AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef __ROUTE_BROKER_H__
#define __ROUTE_BROKER_H__

#include <stdbool.h>
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
typedef void (*route_broker_log_cb) (void *nl, const char *client_name,
				     void *arg, const char *fmt, ...);

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

	/* Detailed log */
	route_broker_log_cb log_dp_detail;

	/* Detailed log is enabled */
	bool *is_log_detail;

	/* Argument to provide with log callbacks */
	void *log_arg;
};

/*
 * Generate topic string for the given object.
 *
 * delete should be set to true if this is a delete of an object
 */
typedef int (*object_broker_topic_gen_cb) (void *obj, char *buf, size_t len,
					bool *delete);

typedef void *(*object_broker_copy_obj_cb) (const void *obj);

typedef void (*object_broker_free_obj_cb) (void *obj);

typedef int (*object_broker_client_publish_cb) (void *obj, void *client_ctx);

struct object_broker_init {
	/* Topic generation */
	object_broker_topic_gen_cb topic_gen;

	/* Make a copy of the object */
	object_broker_copy_obj_cb copy_obj;

	/* Free the object */
	object_broker_free_obj_cb free_obj;

	/* Debug logging */
	route_broker_fmt_cb log_debug;

	/* Error logging */
	route_broker_fmt_cb log_error;

	/* Detailed log */
	route_broker_log_cb log_dp_detail;

	/* Detailed log is enabled */
	bool *is_log_detail;

	/* Argument to provide with log callbacks */
	void *log_arg;
};

enum object_broker_client_type {
	/* Just call back the client upon publish */
	OB_CLIENT_CB,
	/* Connect to dataplane, provide a zsock upon publish */
	OB_CLIENT_DP_ZSOCK,
};

struct object_broker_client_init {
	enum object_broker_client_type type;

	object_broker_client_publish_cb client_publish;

	/* path to config file - required for OB_CLIENT_DP_ZSOCK */
	const char *cfg_file;

	/*
	 * the data format that the client can expect, opaque to the
	 * broker - required for OB_CLIENT_DP_ZSOCK.
	 */
	uint32_t client_data_format;
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

/* Object broker APIs */

int object_broker_init_all(const struct object_broker_init *init,
			   unsigned int num_clients,
			   const struct object_broker_client_init *client);
void object_broker_shutdown_all(void);

void object_broker_publish(void *obj, int route_priority);

#endif /* __ROUTE_BROKER_H__ */

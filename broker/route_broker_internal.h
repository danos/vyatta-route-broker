/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef __ROUTE_BROKER_INTERNAL_H__
#define __ROUTE_BROKER_INTERNAL_H__

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <linux/netlink.h>

#include "broker.h"
#include "route_broker.h"

/* Sized to make the struct rib_route a power of 2 (256) for mem efficiency */
#define ROUTE_TOPIC_LEN 204

#define broker_log_debug(fmt, ...) \
	do { \
		if (route_broker_log_debug) \
			route_broker_log_debug(route_broker_log_arg, fmt, \
					       ##__VA_ARGS__); \
	} while (0)

#define broker_log_err(fmt, ...) \
	do { \
		if (route_broker_log_error) \
			route_broker_log_error(route_broker_log_arg, fmt, \
					       ##__VA_ARGS__); \
	else \
		fprintf(stderr, fmt, ##__VA_ARGS__); \
	} while (0)

struct rib_route {
	struct broker_obj b_obj;
	uint32_t refcount;
	enum route_priority pri;
	char topic[ROUTE_TOPIC_LEN];
	void *data;
};

enum route_broker_types {
	ROUTE_BROKER_ROUTE = 0,
	ROUTE_BROKER_TYPES_MAX = 1,
};

struct route_broker_client {
	CIRCLEQ_ENTRY(route_broker_client) clients_list;
	struct broker_client *client[ROUTE_PRIORITY_MAX];
	pthread_cond_t client_cond;
	uint64_t errors;
};

extern void *route_broker_log_arg;
extern route_broker_fmt_cb route_broker_log_debug;
extern route_broker_fmt_cb route_broker_log_error;
extern object_broker_topic_gen_cb route_broker_topic_gen;
extern object_broker_copy_obj_cb route_broker_copy_obj;
extern object_broker_free_obj_cb route_broker_free_obj;

/*
 * Manage Clients of the broker. A broker can have as many clients
 * as required, and each one reads data at its own speed.
 */
struct route_broker_client *route_broker_client_create(const char *name);
void route_broker_client_delete(struct route_broker_client *client);
void *route_broker_client_get_data(struct route_broker_client *client);
void route_broker_client_free_data(struct route_broker_client *rclient,
				   void *obj);

/* Just the broker */
int route_broker_init(void);
int route_broker_destroy(void);
/* Initialise the broker clients */
int route_broker_dataplane_ctrl_init(const char *cfgfile,
				     object_broker_client_publish_cb publish);
void route_broker_dataplane_ctrl_shutdown(void);
int route_broker_kernel_init(object_broker_client_publish_cb publish);
void route_broker_kernel_shutdown(void);

/*
 * Exposed for tests
 */
void *route_broker_seq_first(int *pri);
void *route_broker_seq_next(void *obj, int *pri);
void *broker_obj_to_rib_route(struct broker_obj *obj);

int route_topic(void *obj, char *buf, size_t len, bool *delete);
void *rib_nl_copy(const void *obj);
void rib_nl_free(void *obj);
int rib_nl_dp_publish_route(void *obj, void *client_ctx);

#endif /* __ROUTE_BROKER_INTERNAL_H__ */

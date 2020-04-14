/*
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2017 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <libmnl/libmnl.h>
#include <czmq.h>
#include <zmq.h>

#include "broker.h"
#include "route_broker_internal.h"

static pthread_t broker_consumer_thread;

static object_broker_client_publish_cb obj_kernel_publish;

static void *broker_consumer(void *arg)
{
	struct route_broker_client *client;
	void *obj;

	client = route_broker_client_create("kernel");

	while (true) {
		while ((obj = route_broker_client_get_data(client))) {
			if (obj_kernel_publish(obj, NULL))
				client->errors++;
			route_broker_client_free_data(client, obj);
		}
	}

	pthread_exit(0);
}

int route_broker_kernel_init(object_broker_client_publish_cb publish)
{
	int rc;

	obj_kernel_publish = publish;
	rc = pthread_create(&broker_consumer_thread, NULL,
			    broker_consumer, NULL);
	return rc;

}

void route_broker_kernel_shutdown(void)
{
	pthread_cancel(broker_consumer_thread);
	pthread_join(broker_consumer_thread, NULL);
}

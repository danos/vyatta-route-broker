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

static route_broker_kernel_publish_cb rib_kernel_publish_route;

static void *broker_consumer(void *arg)
{
	struct nlmsghdr *nl;
	struct route_broker_client *client;

	client = route_broker_client_create("kernel");

	while (true) {
		while ((nl = route_broker_client_get_data(client))) {
			if (rib_kernel_publish_route(nl))
				client->errors++;
			free(nl);
		}
	}

	pthread_exit(0);
}

int route_broker_kernel_init(route_broker_kernel_publish_cb publish)
{
	int rc;

	rib_kernel_publish_route = publish;
	rc = pthread_create(&broker_consumer_thread, NULL,
			    broker_consumer, NULL);
	return rc;

}

void route_broker_kernel_shutdown(void)
{
	pthread_cancel(broker_consumer_thread);
	pthread_join(broker_consumer_thread, NULL);
}

/*
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <czmq.h>

#include "broker.h"
#include "route_broker_internal.h"
#include "netlink_create.h"
#include "cli.h"

int route_broker_kernel_init(object_broker_client_publish_cb publish)
{
	return 0;
}

void route_broker_kernel_shutdown(void)
{
}

struct cli cli;

static void add_routes(int count)
{
	char buf[1024];
	char route[100];
	int i;
	int pri = 1;

	for (i = 0; i < count; i++) {
		snprintf(route, 100, "1.1.%d.0/24 nh 4.4.4.2 int:dp2T0", i);
		netlink_add_route(buf, route);
		route_broker_publish((struct nlmsghdr *)buf, pri);
	}
}

int main(int argc, char **argv)
{
	int rc;
	pid_t pid;
	struct broker_obj *b_obj;
	int pri;
	int i;
	int status;

	rc = route_broker_init();
	assert(rc == 0);
	route_broker_topic_gen = route_topic;
	route_broker_copy_obj = rib_nl_copy;
	route_broker_free_obj = rib_nl_free;

	/*
	 * Create the routing side of it - the broker should open up its control
	 * socket and then wait for the dp.
	 */
	printf("Initialising broker\n ");
	rc = route_broker_dataplane_ctrl_init("test_cfgfile",
					      rib_nl_dp_publish_route, 0);
	assert(rc == 0);

	/* Now create the dp side of it. */
	pid = fork();
	assert(pid >= 0);

	if (pid == 0) {
		/* This is the child */
		execlp("./broker_dp_test", "broker_dp_test",
		       "ipc:///tmp/broker_test_ctrl", "0-0-0-0-1", NULL);
		printf("exec failed\n");
		exit(1);
	}

	/*
	 * Verify there is a dp connected.
	 */
	printf("verifying dp client connected\n");

	/* Wait for 10 sec */
	for (i = 0; i < 10; i++) {
		b_obj = route_broker_seq_first(&pri);
		if (b_obj)
			break;
		sleep(1);
	}

	printf("b_obj is %s\n", b_obj ? "set" : "unset");
	assert(b_obj);

	/* Insert some routes */
	add_routes(10);

	printf("about to wait for pid %d\n", pid);
	if (waitpid(pid, &status, 0) < 0)
		assert(0);

	if (WIFEXITED(status))
		assert(WEXITSTATUS(status) == 0);
	else
		assert(0);

	route_broker_dataplane_ctrl_shutdown();
}

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

static char *connect_to_broker_ctrl(zsock_t **ctrl_sock, const char *ep,
				    const char *uuid)
{
	zmsg_t *msg;
	int rc = 0;
	zframe_t *frame;
	uint32_t prot_version = 0;
	char *uuid_reply;
	char *str;

	*ctrl_sock = zsock_new(ZMQ_DEALER);
	assert(*ctrl_sock);

	if (zsock_connect(*ctrl_sock, "%s", ep) < 0)
		assert(0);

	/* Send CONNECT message */
	msg = zmsg_new();
	assert(msg);

	rc = zmsg_addstr(msg, "CONNECT");
	assert(rc >= 0);

	frame = zframe_new(&prot_version, sizeof(uint32_t));
	assert(frame);
	zmsg_append(msg, &frame);

	rc = zmsg_addstr(msg, uuid);
	assert(rc >= 0);

	rc = zmsg_send(&msg, *ctrl_sock);
	assert(rc >= 0);

	/* Wait for ACCEPT message */
	msg = zmsg_recv(*ctrl_sock);
	assert(msg);

	str = zmsg_popstr(msg);
	assert(strcmp("ACCEPT", str) == 0);
	free(str);

	uuid_reply = zmsg_popstr(msg);
	printf("reply:---%s---  uuid:---%s---\n", uuid_reply, uuid);
	assert(memcmp(uuid_reply, uuid, strlen(uuid)) == 0);
	free(uuid_reply);

	str = zmsg_popstr(msg);
	assert(str);

	zmsg_destroy(&msg);
	return str;
}

static void
connect_to_broker_data(zsock_t **data_sock, const char *data_url,
		       const char *uuid)
{

	*data_sock = zsock_new(ZMQ_PULL);
	assert(*data_sock);

	if (zsock_connect(*data_sock, "%s", data_url) < 0)
		assert(0);
}

/*
 * Pretend we are a dp.
 *
 * Use the control ep passed in in the args
 * use the dp id passed in in the args
 * register with the ctrl channel, and then set up the data channel and
 * pull routes.
 */
int main(int argc, char **argv)
{
	char *ep;
	char *uuid;
	zsock_t *ctrl_sock;
	zsock_t *data_sock;
	char *data_url;
	uint32_t data_msg_count = 0;
	zmsg_t *msg;
	int restart_count = 0;

	ep = argv[1];
	uuid = argv[2];

 init:
	printf("Initialising dp\n");
	printf("dp: EP  : %s\n", ep);
	printf("dp: uuid: %s\n", uuid);

	data_url = connect_to_broker_ctrl(&ctrl_sock, ep, uuid);
	printf("dp: data: %s\n", data_url);

	connect_to_broker_data(&data_sock, data_url, uuid);
	free(data_url);

	/*
	 * Pull data
	 */
	printf("dp: trying to pull data\n");
	while ((msg = zmsg_recv(data_sock))) {
		assert(msg);
		data_msg_count++;
		zmsg_destroy(&msg);
		printf("Message received\n");

		if (data_msg_count == 10)
			break;
	}

	/* Close sockets */
	zsock_destroy(&data_sock);
	zsock_destroy(&ctrl_sock);
	printf("DP shutting down - processed %d messages\n", data_msg_count);

	if (restart_count == 0) {
		restart_count++;
		data_msg_count = 0;
		goto init;
	}

	return 0;
}

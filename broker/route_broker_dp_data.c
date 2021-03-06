/*
 * Copyright (c) 2018-2019,2021 AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#define _GNU_SOURCE		/* Needed to get linux specific pthread APIs */
#include <pthread.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <czmq.h>
#include <zmq.h>

#include "broker.h"
#include "route_broker_internal.h"
#include "route_broker_dp_data.h"

static char *broker_dp_data_init(zsock_t **data_sock, const char *sock_ep)
{
	char ep_dir[PATH_MAX];
	char *actual_ep;

	*data_sock = zsock_new(ZMQ_PUSH);
	if (!*data_sock) {
		broker_log_err("Socket to DP not created");
		assert(0);
	}

	zsock_set_sndhwm(*data_sock, 500);

	if (zsock_bind(*data_sock, "%s", sock_ep) < 0) {
		broker_log_err("Socket to DP not initialised");
		assert(0);
	}

	actual_ep = zsock_last_endpoint(*data_sock);

	/* Fix up permissions of socket and tmp directory (if applicable) */
	if (strncmp(actual_ep, "ipc://", 6) == 0) {
		/* skip over ipc:// */
		if (chmod(actual_ep + 6, 0770) < 0) {
			broker_log_err("Could not chmod DP socket %s: %s",
				       actual_ep + 6, strerror(errno));
			assert(0);
		}

		if (!strcmp(sock_ep, "ipc://*") &&
		    strrchr(actual_ep + 6, '/')) {
			memcpy(ep_dir, actual_ep + 6, strlen(actual_ep + 6));
			*strrchr(ep_dir, '/') = '\0';

			if (chmod(ep_dir, 0770) < 0) {
				broker_log_err(
					"Could not chmod DP socket dir %s: %s",
					ep_dir, strerror(errno));
				assert(0);
			}
		}
	}

	return actual_ep;
}

/* Client needs restarting if we have received the $TERM command on the pipe */
static bool client_needs_restart(zsock_t *pipe)
{
	char *str;
	bool restart = false;

	if (!(zsock_events(pipe) & ZMQ_POLLIN))
		return false;

	str = zstr_recv(pipe);
	if (streq(str, "$TERM"))
		restart = true;

	free(str);
	return restart;
}

void broker_dp_data_client(zsock_t *pipe, void *arg)
{
	void *obj;
	struct route_broker_client *client;
	struct broker_client *bc;
	char *ep;
	static zsock_t *dp_data_sock;
	struct dp_data_client_args *args = arg;
	const char *sock_ep = args->sock_ep;
	object_broker_client_publish_cb client_publish = args->client_publish;

	free(args);

	if (pthread_setname_np(pthread_self(), "ribbroker/dp"))
		broker_log_err("Could not name rib broker dp data thread");

	client = route_broker_client_create("dp");
	ep = broker_dp_data_init(&dp_data_sock, sock_ep);
	broker_log_debug("New broker dataplane client ep: %s\n", ep);

	if (!ep) {
		broker_log_err("Could not create rib broker dp thread ep");
		assert(0);
	}

	zsock_signal(pipe, 0);

	/* Send ep back to ctrl thread so it can send it to DP */
	zstr_send(pipe, ep);
	free(ep);

	while (true) {
		while ((obj = route_broker_client_get_data(client, &bc))) {
 try_sending:
			errno = 0;
			if (client_publish(obj, dp_data_sock)) {
				if (errno != EAGAIN) {
					client->errors++;
					broker_log_err("publish error %s: "
						       "consumed %" PRIu64
						       " behind %" PRIu64
						       " errno (%d) %s\n",
						       bc->name,
						       bc->consumed,
						       bc->broker->id -
						       bc->broker_obj.id,
						       errno, strerror(errno));
				}
				if (client_needs_restart(pipe)) {
					route_broker_client_free_data(
						client, obj);
					goto stop_client;
				}

				usleep(10000);
				goto try_sending;
			}

			broker_log_dp_detail(obj, bc->name,
					     "publish %s: consumed %" PRIu64
					     " behind %" PRIu64 "\n",
					     bc->name, bc->consumed,
					     bc->broker->id - bc->broker_obj.id);

			route_broker_client_free_data(client, obj);

			if (client_needs_restart(pipe))
				goto stop_client;
		}

		if (client_needs_restart(pipe))
			goto stop_client;
	}

 stop_client:
	route_broker_client_delete(client);
	zsock_destroy(&dp_data_sock);
}

/*
 * Copyright (c) 2018-2019, AT&T Intellectual Property.  All rights reserved.
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

static int
rib_dp_publish_route(const struct nlmsghdr *nlmsg, zsock_t *dp_data_sock)
{
	zframe_t *frame;
	int rc;

	frame = zframe_new(nlmsg, nlmsg->nlmsg_len);
	if (!frame)
		return -1;

	rc = zframe_send(&frame, dp_data_sock, ZFRAME_DONTWAIT);
	if (rc < 0)
		zframe_destroy(&frame);

	return rc;
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
	struct nlmsghdr *nl;
	struct route_broker_client *client;
	char *ep;
	static zsock_t *dp_data_sock;
	const char *sock_ep = arg;

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
		while ((nl = route_broker_client_get_data(client))) {
 try_sending:
			if (rib_dp_publish_route(nl, dp_data_sock)) {
				if (client_needs_restart(pipe)) {
					free(nl);
					goto stop_client;
				}

				usleep(10000);
				goto try_sending;
			}
			free(nl);

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

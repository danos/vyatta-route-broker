/*
 * Copyright (c) 2018, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#define _GNU_SOURCE		/* Needed to get linux specific pthread APIs */
#include <pthread.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <czmq.h>
#include <ini.h>
#include <zmq.h>

#include "broker.h"
#include "route_broker_internal.h"
#include "route_broker_dp_data.h"

enum rib_broker_dp_request {
	RIB_BROKER_DP_REQ_ERROR,
	RIB_BROKER_DP_REQ_CONNECT,
	RIB_BROKER_DP_REQ_KEEPALIVE,
};

struct rib_broker_cfg {
	struct in_addr local_ip;	/* local ip of tunnel */
	char *rib_dp_ctrl_url;		/* url of rib broker server */
	char *rib_dp_data_url;		/* url of rib broker server */
};

struct dp_ctrl_client_args {
	const char *cfgfile;
	uint32_t data_format;
};

static struct rib_broker_cfg rib_broker_cfg;

static zactor_t *broker_dp_ctrl_thread;

static object_broker_client_publish_cb broker_dp_client_publish;

/*
 * Hash table of connected vplanes, keyed using the uuid.
 */
static zhash_t *dp_uuid_ht;

struct dp {
	char *uuid;
	zframe_t *envelope;	/* To make sure we send back to correct dp */
	zsock_t *ipc;		/* ipc pipe between ctrl thread and dp thread */
	char *data_url;
};

static int copy_str(char **str_ref, const char *value)
{
	free(*str_ref);
	*str_ref = strdup(value);
	return 1;
}

static int parse_ipaddress(struct in_addr *addr, const char *str)
{
	return inet_pton(AF_INET, str, &addr);
}

/*
 * Callback from inih library for each name value
 * return 0 = error, 1 = ok
 */
static int parse_entry(void *user, const char *section,
		       const char *name, const char *value)
{
	struct rib_broker_cfg *cfg = user;

	if (strcasecmp(section, "rib") == 0) {
		if (strcmp(name, "ip") == 0)
			return parse_ipaddress(&cfg->local_ip, value);
		else if (strcmp(name, "control") == 0)
			return copy_str(&cfg->rib_dp_ctrl_url, value);
		else if (strcmp(name, "data") == 0)
			return copy_str(&cfg->rib_dp_data_url, value);
	}

	return 1;	/* good */
}

static bool
parse_rib_config(const char *cfgfile, struct rib_broker_cfg *rib_broker_cfg)
{
	FILE *f = fopen(cfgfile, "r");

	if (f == NULL)
		return false;

	int rc = ini_parse_file(f, parse_entry, rib_broker_cfg);
	fclose(f);
	return rc == 0;
}

/*
 * Process an actor message. At the moment the only message
 * we expect is the one to say we are terminated.
 */
static int process_actor_message(zloop_t *loop, zsock_t *sock, void *arg)
{
	zmsg_t *msg;
	char *str;
	int restart = 0;

	msg = zmsg_recv(sock);
	if (!msg)
		return 0;

	str = zmsg_popstr(msg);
	if (streq(str, "$TERM"))
		restart = -1;

	free(str);
	zmsg_destroy(&msg);
	return restart;
}

static int zmsg_addu32(zmsg_t *msg, uint32_t u)
{
	zframe_t *frame;

	if (!msg) {
		broker_log_err("addu32: Passed invalid pointer 'msg'\n");
		return -1;
	}

	frame = zframe_new(&u, sizeof(uint32_t));
	if (!frame) {
		broker_log_err("addu32: Failed to create ZMSG frame\n");
		return -1;
	}
	return zmsg_append(msg, &frame);
}

static int zmsg_popu32(zmsg_t *msg, uint32_t *p)
{
	zframe_t *frame = zmsg_pop(msg);
	if (frame == NULL) {
		broker_log_err("popu32: missing message element");
		return -1;
	}

	if (zframe_size(frame) != sizeof(uint32_t)) {
		broker_log_err("popu32: wrong message size %zd",
			       zframe_size(frame));
		zframe_destroy(&frame);
		return -1;
	}

	memcpy(p, zframe_data(frame), sizeof(uint32_t));
	zframe_destroy(&frame);
	return 0;
}

static enum rib_broker_dp_request broker_dp_ctrl_msg_request(char *msg_type)
{
	if (msg_type) {
		if (!strcmp(msg_type, "CONNECT"))
			return RIB_BROKER_DP_REQ_CONNECT;
		if (!strcmp(msg_type, "KEEPALIVE"))
			return RIB_BROKER_DP_REQ_KEEPALIVE;
	}
	return RIB_BROKER_DP_REQ_ERROR;
}

/*
 * Control message should be:
 *   "CONNECT|KEEPALIVE" (string)
 *   <proto version>     (int)
 *   <uuid>              (string)
 */
static enum rib_broker_dp_request broker_dp_ctrl_msg_parse(zmsg_t *msg,
							   char **uuid)
{
	char *msg_type;
	uint32_t proto_version;
	enum rib_broker_dp_request req;

	msg_type = zmsg_popstr(msg);
	req = broker_dp_ctrl_msg_request(msg_type);
	if (req == RIB_BROKER_DP_REQ_ERROR) {
		broker_log_err("broker ctrl expected CONNECT|KEEPALIVE, got %s",
			       msg_type ? msg_type : "NULL");
		free(msg_type);
		return RIB_BROKER_DP_REQ_ERROR;
	}
	free(msg_type);

	if (zmsg_popu32(msg, &proto_version) < 0 || proto_version != 0) {
		broker_log_err("Could not get dataplane proto version");
		return RIB_BROKER_DP_REQ_ERROR;
	}

	*uuid = zmsg_popstr(msg);
	if (*uuid == NULL) {
		broker_log_err("Could not get dataplane uuid");
		return RIB_BROKER_DP_REQ_ERROR;
	}

	return req;
}

static zmsg_t *broker_dp_ctrl_msg_prepare(const char *reply, const char *uuid,
					  zframe_t **envelope)
{
	zmsg_t *reply_msg = zmsg_new();
	int rc;

	if (!reply_msg) {
		broker_log_err("Could not build broker control reply msg");
		return NULL;
	}

	rc = zmsg_addstr(reply_msg, reply);
	if (rc < 0) {
		broker_log_err("Could not add %s to broker control reply msg",
			       reply);
		return NULL;
	}

	rc = zmsg_addstr(reply_msg, uuid);
	if (rc < 0) {
		broker_log_err("Could not add uuid %s to broker control "
			       "reply msg", uuid);
		return NULL;
	}

	rc = zmsg_prepend(reply_msg, envelope);
	if (rc < 0) {
		broker_log_err("Could not add envelope to broker control "
			       "reply msg");
		return NULL;
	}

	return reply_msg;
}

/*
 * Send the ACCEPT to the dataplane
 * The message contains the endpoint to use for the route broker data.
 * Format is:
 * ACCEPT
 * <UUID>
 * <data url>
 * <data format>
 */
static int broker_dp_ctrl_msg_accept(struct dp *dp, zsock_t *sock,
				     const char *url, uint32_t data_format)
{
	zmsg_t *reply_msg;
	int rc;

	reply_msg =
	    broker_dp_ctrl_msg_prepare("ACCEPT", dp->uuid, &dp->envelope);
	if (!reply_msg)
		return -1;

	rc = zmsg_addstr(reply_msg, url);
	if (rc < 0) {
		broker_log_err("Could not add url to broker control reply msg");
		zmsg_destroy(&reply_msg);
		return -1;
	}

	rc = zmsg_addu32(reply_msg, data_format);
	if (rc < 0) {
		broker_log_err("Could not add data format to broker control reply msg");
		zmsg_destroy(&reply_msg);
		return -1;
	}

	broker_log_debug("New broker dataplane reply %s, %s\n", dp->uuid, url);

	return zmsg_send(&reply_msg, sock);
}

/*
 * Send RECONNECT reply to the dataplane
 * Format is:
 * RECONNECT
 * <UUID>
 */
static int broker_dp_ctrl_msg_reconnect(zsock_t *sock, const char *uuid,
					zframe_t *envelope)
{
	zmsg_t *reply_msg;

	reply_msg = broker_dp_ctrl_msg_prepare("RECONNECT", uuid, &envelope);
	if (!reply_msg)
		return -1;

	broker_log_debug("Broker dataplane reconnect reply %s\n", uuid);

	return zmsg_send(&reply_msg, sock);
}

static struct dp *dp_findbyuuid(const char *uuid)
{
	if (uuid != NULL)
		return zhash_lookup(dp_uuid_ht, uuid);

	return NULL;
}

static void dp_insert(struct dp *dp)
{
	zhash_insert(dp_uuid_ht, dp->uuid, dp);
}

static void delete_dp(struct dp *dp)
{
	if (dp->envelope)
		zframe_destroy(&dp->envelope);
	free(dp->uuid);
	free(dp->data_url);
	free(dp);
}

/*
 * Stop the thread by sending the TERM signal down the pipe to the thread.
 */
static void stop_old_dp_thread(struct dp *dp)
{
	zactor_destroy((zactor_t **) &dp->ipc);
}

static void close_dp_session(struct dp *dp)
{
	zhash_delete(dp_uuid_ht, dp->uuid);
	stop_old_dp_thread(dp);
	delete_dp(dp);
}

static void close_all_dp_sessions(void)
{
	struct dp *dp;

	while ((dp = zhash_first(dp_uuid_ht)))
		close_dp_session(dp);
	zhash_destroy(&dp_uuid_ht);
}

/*
 * Start a new data thread, and return the data url it is using.
 */
static char *start_new_dp_data_thread(struct dp *dp)
{
	struct dp_data_client_args *args = calloc(1, sizeof(*args));

	if (!args) {
		broker_log_err("Could not allocate memory for dp data args");
		return NULL;
	}

	args->sock_ep = rib_broker_cfg.rib_dp_data_url;
	args->client_publish = broker_dp_client_publish;

	dp->ipc = (zsock_t *) zactor_new(broker_dp_data_client, args);
	if (dp->ipc == NULL)
		broker_log_err("Could not create new zactor for dp data");

	/* New thread starts, and sends us the ep url on the pipe */
	return zstr_recv(dp->ipc);
}

static int process_connect_message(zsock_t *sock, zframe_t *envelope,
				   char *uuid, uint32_t data_format)
{
	struct dp *dp;

	dp = dp_findbyuuid(uuid);
	if (dp) {
		broker_log_debug("Restart broker dataplane client %s\n", uuid);
		close_dp_session(dp);
	}

	broker_log_debug("New broker dataplane client %s\n", uuid);
	dp = calloc(1, sizeof(*dp));
	if (!dp) {
		broker_log_err("Could not alloc mem for new dp");
		return 0;
	}

	/* New connection, Store it in our db */
	dp->envelope = envelope;
	dp->uuid = uuid;
	dp_insert(dp);

	dp->data_url = start_new_dp_data_thread(dp);

	/* And send the ACCEPT back to the DP */
	broker_dp_ctrl_msg_accept(dp, sock, dp->data_url, data_format);
	return 0;
}

static int
process_keepalive_message(zsock_t *sock, zframe_t *envelope, const char *uuid)
{
	struct dp *dp;

	dp = dp_findbyuuid(uuid);
	if (dp)
		/* DP is known, no need to reply */
		return 0;

	/* unknown DP - tell it to reconnect */
	broker_dp_ctrl_msg_reconnect(sock, uuid, envelope);
	return 0;
}

static int process_ctrl_message(zloop_t *loop, zsock_t *sock, void *arg)
{
	struct dp_ctrl_client_args *args = arg;
	enum rib_broker_dp_request req;
	char *uuid;
	zframe_t *envelope;
	zmsg_t *msg;

	msg = zmsg_recv(sock);

	/*
	 * This is added by the socket and gives us the endpoint to
	 * send back to
	 */
	envelope = zmsg_unwrap(msg);

	req = broker_dp_ctrl_msg_parse(msg, &uuid);
	zmsg_destroy(&msg);
	switch (req) {
	case RIB_BROKER_DP_REQ_CONNECT:
		return process_connect_message(sock, envelope, uuid,
					       args->data_format);
	case RIB_BROKER_DP_REQ_KEEPALIVE:
		return process_keepalive_message(sock, envelope, uuid);
	default:
		break;
	}

	broker_log_err("Could not parse message on broker control socket");
	return 0;
}

/*
 * A new pthread that will control the creation of all the datapane consumers.
 */
static void broker_dp_ctrl(zsock_t *pipe, void *arg)
{
	struct dp_ctrl_client_args *args = arg;
	const char *cfgfile = args->cfgfile;
	zloop_t *zloop;
	zsock_t *broker_dp_ctrl_sock;
	const char *sock_path = NULL;

	if (pthread_setname_np(pthread_self(), "ribb/dp_ctrl"))
		broker_log_err("Could not name rib broker dp ctrl thread");

	parse_rib_config(cfgfile, &rib_broker_cfg);
	zsock_signal(pipe, 0);

	dp_uuid_ht = zhash_new();

	/* Create the control socket */
	broker_dp_ctrl_sock = zsock_new_router(NULL);
	if (!broker_dp_ctrl_sock) {
		broker_log_err("Could not create broker control socket");
		return;
	}

	if (zsock_bind(broker_dp_ctrl_sock, "%s",
		       rib_broker_cfg.rib_dp_ctrl_url) < 0) {
		zsock_destroy(&broker_dp_ctrl_sock);
		broker_log_err("Could not bind broker control socket");
		return;
	}

	if (strncmp(rib_broker_cfg.rib_dp_ctrl_url, "ipc://", 6) == 0) {
		/* skip over ipc:// */
		sock_path = rib_broker_cfg.rib_dp_ctrl_url + 6;
		if (chmod(sock_path, 0770) < 0) {
			broker_log_err("Could not chmod control socket");
			return;
		}
	}

	/* Setup the poller */
	zloop = zloop_new();
	zloop_reader(zloop, pipe, process_actor_message, pipe);
	zloop_reader(zloop, broker_dp_ctrl_sock, process_ctrl_message,
		     args);

	zloop_start(zloop);

	close_all_dp_sessions();
	zloop_destroy(&zloop);
	zsock_destroy(&broker_dp_ctrl_sock);
	free(args);
}

int route_broker_dataplane_ctrl_init(const char *cfgfile,
				     object_broker_client_publish_cb publish,
				     uint32_t data_format)
{
	struct dp_ctrl_client_args *args;

	args = calloc(1, sizeof(*args));
	if (!args)
		return -1;

	args->cfgfile = cfgfile;
	args->data_format = data_format;

	broker_dp_client_publish = publish;

	broker_dp_ctrl_thread = zactor_new(broker_dp_ctrl, args);
	if (broker_dp_ctrl_thread)
		return 0;

	free(args);
	return -1;
}

void route_broker_dataplane_ctrl_shutdown(void)
{
	zactor_destroy(&broker_dp_ctrl_thread);
}

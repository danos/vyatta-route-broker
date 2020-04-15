/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "broker.h"
#include "route_broker_internal.h"

#include <czmq.h>

void *route_broker_log_arg;
route_broker_fmt_cb route_broker_log_debug;
route_broker_fmt_cb route_broker_log_error;
object_broker_topic_gen_cb route_broker_topic_gen;
object_broker_copy_obj_cb route_broker_copy_obj;
object_broker_free_obj_cb route_broker_free_obj;

static uint64_t processed_msg;
static uint64_t ignored_msg;
static uint64_t dropped_msg;

struct broker *route_broker[ROUTE_PRIORITY_MAX];
zhash_t *route_hashtbl;
static pthread_mutex_t route_broker_mutex = PTHREAD_MUTEX_INITIALIZER;

#define container_of(pointer, container, member) \
	((container *)(((unsigned char *)(pointer)) - \
		       offsetof(container, member)))

CIRCLEQ_HEAD(client_list, route_broker_client) client_list_head;

static inline void route_broker_lock(void)
{
	pthread_mutex_lock(&route_broker_mutex);
}

static inline void route_broker_unlock(void)
{
	pthread_mutex_unlock(&route_broker_mutex);
}

static struct rib_route *rib_route_create(void)
{
	return calloc(sizeof(struct rib_route), 1);
}

static void rib_route_delete(struct rib_route *obj)
{
	assert(obj);
	if (--obj->refcount == 0) {
		zhash_delete(route_hashtbl, obj->topic);
		route_broker_free_obj(obj->data);
		free(obj);
	}
}

static struct broker_obj *rib_route_to_broker_obj(void *obj, int type)
{
	struct rib_route *route = obj;
	assert(obj);
	assert(type <= ROUTE_BROKER_TYPES_MAX);

	return &route->b_obj;
}

void *broker_obj_to_rib_route(struct broker_obj *obj)
{
	assert(obj);
	assert(obj->obj_type <= ROUTE_BROKER_TYPES_MAX);

	return container_of(obj, struct rib_route, b_obj);
}

static void rib_route_lock(struct broker_obj *b_obj)
{
	struct rib_route *obj;

	obj = broker_obj_to_rib_route(b_obj);
	obj->refcount++;
}

static void rib_route_unlock(struct broker_obj *b_obj)
{
	struct rib_route *obj;

	obj = broker_obj_to_rib_route(b_obj);
	rib_route_delete(obj);
}

static void route_broker_seq_show(route_broker_fmt_cb cli_out, void *cli,
				  struct broker_obj *b_obj, bool show_obj)
{
	struct rib_route *route;
	struct broker_client *client;

	if (b_obj->flags & BROKER_FLAGS_OBJ) {
		if (show_obj) {
			route = broker_obj_to_rib_route(b_obj);
			cli_out(cli, "ID:%-10" PRIu64 " %s %s\n",
				b_obj->id,
				(b_obj->flags & BROKER_FLAGS_DELETE) ?
				 "D" : " ",
				route->topic);
		}

	} else {
		/* is a client */
		client = container_of(b_obj, struct broker_client, broker_obj);
		cli_out(cli,
			"ID:%-10" PRIu64 "   %s consumed:%" PRIu64 " behind:%"
			PRIu64 "\n", b_obj->id, client->name, client->consumed,
			client->broker->id - b_obj->id);
	}
}

void *route_broker_seq_first(int *pri)
{
	*pri = 0;
	return broker_seq_start(route_broker[*pri]);
}

void *route_broker_seq_next(void *obj, int *pri)
{
	struct broker_obj *b_obj;

	b_obj = broker_seq_next(route_broker[*pri], obj);
	if (!b_obj) {
		/* end of this broker - is there another priority level? */
		if (*pri < (ROUTE_PRIORITY_MAX - 1)) {
			(*pri)++;
			/* TODO - put a separator in here ? */
			return broker_seq_start(route_broker[*pri]);
		}
	}

	return b_obj;
}

static void route_broker_show_internal(route_broker_fmt_cb cli_out, void *cli,
				       bool detail)
{
	uint64_t count = 0;
	int pri = 0;
	int pri_last = 0;

	struct route_broker_client *rclient;

	cli_out(cli, "processed %" PRIu64 "\n", processed_msg);

	if (ignored_msg)
		cli_out(cli, "ignored %" PRIu64 "\n", ignored_msg);
	if (dropped_msg)
		cli_out(cli, "dropped %" PRIu64 "\n", dropped_msg);

	CIRCLEQ_FOREACH(rclient, &client_list_head, clients_list) {
		if (rclient->errors) {
			cli_out(cli, "Client %p: errors:%" PRIu64,
				rclient, rclient->errors);
		}
	}

	cli_out(cli, "\nPriority %d, top: %" PRIu64 "\n", pri,
		route_broker[pri]->id);
	route_broker_lock();

	void *obj = route_broker_seq_first(&pri);
	while (obj) {
		if (pri != pri_last) {
			pri_last = pri;
			cli_out(cli, "\nPriority %d, top: %" PRIu64 "\n", pri,
				route_broker[pri]->id);
		}
		count++;
		route_broker_seq_show(cli_out, cli, obj, detail);
		obj = route_broker_seq_next(obj, &pri);
	}
	cli_out(cli, "Total objects %" PRIu64 "\n", count);

	route_broker_unlock();
}

void route_broker_show(route_broker_fmt_cb cli_out, void *cli)
{
	route_broker_show_internal(cli_out, cli, true);
}

void route_broker_show_summary(route_broker_fmt_cb cli_out, void *cli)
{
	route_broker_show_internal(cli_out, cli, false);
}

const struct broker_ops route_broker_ops = {
	.obj_to_broker_obj = rib_route_to_broker_obj,
	.broker_obj_to_obj = broker_obj_to_rib_route,
	.lock_obj = rib_route_lock,
	.unlock_obj = rib_route_unlock,
};

int route_broker_init(void)
{
	int i;

	CIRCLEQ_INIT(&client_list_head);

	for (i = 0; i < ROUTE_PRIORITY_MAX; i++) {
		route_broker[i] =
		    broker_create(&route_broker_ops, ROUTE_BROKER_TYPES_MAX);
		assert(route_broker[i]);
		if (!route_broker[i])
			return 1;
	}

	route_hashtbl = zhash_new();
	assert(route_hashtbl);

	return route_hashtbl ? 0 : 1;
}

int route_broker_destroy(void)
{
	int rc;
	int i;

	for (i = 0; i < ROUTE_PRIORITY_MAX; i++) {
		rc = broker_delete(route_broker[i]);
		if (rc != 0)
			return rc;
	}

	zhash_destroy(&route_hashtbl);

	return 0;
}

static void *route_broker_client_get(struct broker_obj *b_obj)
{
	struct rib_route *obj;

	obj = broker_obj_to_rib_route(b_obj);

	assert(obj->data);
	return route_broker_copy_obj(obj->data);
}

static struct broker_client_ops route_broker_client_ops = {
	.add_obj = route_broker_client_get,
	.del_obj = route_broker_client_get,
};

static int data_available_for_client(struct route_broker_client *rclient)
{
	int i;

	for (i = 0; i < ROUTE_PRIORITY_MAX; i++) {
		if (rclient->client[i]->broker_obj.id != route_broker[i]->id)
			return i;
	}

	return -1;
}

/*
 * There are possibly multiple underlying brokers (one per priority)
 * being represented to the users as a single one. Check each broker
 * for data in priority order, return data when found. If no data then
 * sleep until data arrives, then start the check again from the highest
 * priority.
 */
void *route_broker_client_get_data(struct route_broker_client *rclient)
{
	void *data;
	int level;
	int rc;
	struct timespec wake_at;

	clock_gettime(CLOCK_REALTIME, &wake_at);
	wake_at.tv_sec += 1;

	route_broker_lock();

	/* Check all levels */

	/* If there is no more data sleep until woken by more data */
	while ((level = data_available_for_client(rclient)) < 0) {
		rc = pthread_cond_timedwait(&rclient->client_cond,
					    &route_broker_mutex, &wake_at);
		if (rc == ETIMEDOUT) {
			route_broker_unlock();
			return NULL;
		}
	}

	data = broker_client_get_data(rclient->client[level]);
	route_broker_unlock();

	return data;
}

void route_broker_client_free_data(struct route_broker_client *rclient,
				   void *obj)
{
	route_broker_free_obj(obj);
}

struct route_broker_client *route_broker_client_create(const char *name)
{
	struct route_broker_client *rclient;
	int i;

	if (!name)
		return NULL;

	rclient = calloc(1, sizeof(*rclient));
	if (!rclient)
		return NULL;

	route_broker_lock();
	for (i = 0; i < ROUTE_PRIORITY_MAX; i++) {
		rclient->client[i] = broker_client_create(route_broker[i],
					     &route_broker_client_ops, name);
		if (!rclient->client) {
			route_broker_unlock();
			goto failed;
		}
	}

	route_broker_unlock();

	if (pthread_cond_init(&rclient->client_cond, NULL))
		goto failed;

	CIRCLEQ_INSERT_HEAD(&client_list_head, rclient, clients_list);

	return rclient;

 failed:
	route_broker_lock();
	for (i = 0; i < ROUTE_PRIORITY_MAX; i++) {
		if (rclient->client[i])
			broker_client_delete(rclient->client[i]);
	}
	free(rclient);
	route_broker_unlock();
	return NULL;
}

void route_broker_client_delete(struct route_broker_client *rclient)
{
	int i;

	route_broker_lock();
	for (i = 0; i < ROUTE_PRIORITY_MAX; i++)
		broker_client_delete(rclient->client[i]);
	pthread_cond_destroy(&rclient->client_cond);
	route_broker_unlock();

	CIRCLEQ_REMOVE(&client_list_head, rclient, clients_list);
	free(rclient);
}

static void route_broker_wake_clients(void)
{
	struct route_broker_client *rclient;

	/* Already have the mutex */
	CIRCLEQ_FOREACH(rclient, &client_list_head, clients_list) {
		if (data_available_for_client(rclient) >= 0)
			pthread_cond_signal(&rclient->client_cond);
	}
}

void object_broker_publish(void *obj, int pri)
{
	struct rib_route *route = NULL;
	struct rib_route *hashed_route = NULL;
	int rc;
	void *data_copy;
	bool del = false;

	processed_msg++;
	route = rib_route_create();
	if (!route) {
		dropped_msg++;
		return;
	}

	data_copy = route_broker_copy_obj(obj);
	if (!data_copy) {
		dropped_msg++;
		free(route);
		return;
	}

	route->data = data_copy;
	route->pri = pri;
	rc = route_broker_topic_gen(route->data, route->topic,
				    ROUTE_TOPIC_LEN, &del);
	if (rc <= 0) {
		/* Some routes such as local broadcast are ignored */
		ignored_msg++;
		route_broker_free_obj(route->data);
		free(route);
		return;
	}

	route_broker_lock();
	hashed_route = zhash_lookup(route_hashtbl, route->topic);

	if (del) {
		/* If we are deleting something it must be there */
		if (hashed_route) {
			if (hashed_route->pri > pri) {
				/*
				 * New route has higher priority:
				 *   - Force it out of existing priority level
				 *   - Add it to new priority level (add then
				 *     delete as we can't add a 'delete')
				 */
				broker_del_obj_now(route_broker
						   [hashed_route->pri],
						   &hashed_route->b_obj);

				broker_add_obj(route_broker[pri], route,
					       ROUTE_BROKER_ROUTE);
				broker_del_obj(route_broker[pri], route,
					       ROUTE_BROKER_ROUTE);
			} else {
				/*
				 * Priority has not changed or new route has
				 * lower priority.
				 * Swap the data to most recent version.
				 */
				route_broker_free_obj(hashed_route->data);
				hashed_route->data = route->data;

				broker_del_obj(route_broker[hashed_route->pri],
					       hashed_route,
					       ROUTE_BROKER_ROUTE);
				free(route);
			}
		} else {
			route_broker_free_obj(route->data);
			free(route);
		}
	} else {
		if (hashed_route) {
			if (hashed_route->pri > pri) {
				/*
				 * New route has higher priority:
				 *   - Force it out of existing priority level
				 *   - Add it to new priority level.
				 */
				broker_del_obj_now(route_broker
						   [hashed_route->pri],
						   &hashed_route->b_obj);

				broker_add_obj(route_broker[pri], route,
					       ROUTE_BROKER_ROUTE);
				zhash_insert(route_hashtbl, route->topic,
					     route);
			} else if (hashed_route->pri < pri
				   || hashed_route->pri == pri) {
				/*
				 * New route has lower priority:
				 *   - Ideally mark it as deleted, where it is
				 *     so that we don't misforward, and put a
				 *     new entry in at the correct priority,
				 *     but that causes issues generating the
				 *     delete, and having an entry in 2 tables
				 *     at once (only really an issue if there
				 *     is a further update).
				 *   - Solution: leave the route in the
				 *     original priority where it can be
				 *     updated quickly. If it gets deleted it
				 *     will come back at the correct priority,
				 *     and if it doesn't get modified further
				 *     then there is little cost to it being
				 *     in the wrong level.
				 *
				 * Updating, so swap data to most recent
				 * version.
				 */
				route_broker_free_obj(hashed_route->data);
				hashed_route->data = route->data;
				free(route);
				broker_upd_obj(route_broker[hashed_route->pri],
					       hashed_route,
					       ROUTE_BROKER_ROUTE);
			}
		} else {
			broker_add_obj(route_broker[pri], route,
				       ROUTE_BROKER_ROUTE);
			zhash_insert(route_hashtbl, route->topic, route);
		}
	}

	route_broker_wake_clients();
	route_broker_unlock();
}

int object_broker_init_all(const struct object_broker_init *init,
			   unsigned int num_clients,
			   const struct object_broker_client_init *client)
{
	int rc;

	if (!init || !init->topic_gen || !init->copy_obj || !init->free_obj)
		return -EINVAL;

	/* Client support currently hardcoded */
	if (num_clients != 1 && num_clients != 2)
		return -EINVAL;

	/* First client must be DP_ZSOCK */
	if (client[0].type != OB_CLIENT_DP_ZSOCK || !client[0].cfg_file ||
	    !client[0].client_publish)
		return -EINVAL;

	/* Second client, if present, must be callback type */
	if (num_clients == 2 && (client[1].type != OB_CLIENT_CB ||
				 !client[1].client_publish))
		return -EINVAL;

	route_broker_log_debug = init->log_debug;
	route_broker_log_error = init->log_error;
	route_broker_log_arg = init->log_arg;
	route_broker_topic_gen = init->topic_gen;
	route_broker_copy_obj = init->copy_obj;
	route_broker_free_obj = init->free_obj;

	rc = route_broker_init();
	assert(rc == 0);

	rc = route_broker_dataplane_ctrl_init(client[0].cfg_file,
					      client[0].client_publish,
					      client[0].client_data_format);
	if (num_clients == 2)
		rc |= route_broker_kernel_init(client[1].client_publish);
	return rc;
}

void object_broker_shutdown_all(void)
{
	route_broker_dataplane_ctrl_shutdown();
	route_broker_kernel_shutdown();
	route_broker_destroy();
}

/* Legacy netlink implementation */

static route_broker_kernel_publish_cb rib_nl_kernel_publish;

static int rib_nl_kernel_publish_wrapper(void *obj, void *client_ctx)
{
	return rib_nl_kernel_publish(obj);
}

int
rib_nl_dp_publish_route(void *obj, void *client_ctx)
{
	const struct nlmsghdr *nlmsg = obj;
	zsock_t *dp_data_sock = client_ctx;
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

void *rib_nl_copy(const void *obj)
{
	const struct nlmsghdr *nl = obj;
	struct nlmsghdr *nl_copy;

	nl_copy = malloc(nl->nlmsg_len);
	if (!nl_copy)
		return NULL;

	memcpy(nl_copy, nl, nl->nlmsg_len);
	return nl_copy;
}

void rib_nl_free(void *obj)
{
	free(obj);
}

int route_broker_init_all(const struct route_broker_init *init)
{
	struct object_broker_init obj_init = { 0 };
	struct object_broker_client_init client[2] = { 0 };
	unsigned int num_clients = 1;
	const char *cfgfile = "/etc/vyatta-routing/rib.conf";

	if (init) {
		obj_init.log_debug = init->log_debug;
		obj_init.log_error = init->log_error;
		obj_init.log_arg = init->log_arg;
	}
	obj_init.topic_gen = route_topic;
	obj_init.copy_obj = rib_nl_copy;
	obj_init.free_obj = rib_nl_free;

	client[0].cfg_file = cfgfile;
	client[0].type = OB_CLIENT_DP_ZSOCK;
	client[0].client_publish = rib_nl_dp_publish_route;

	if (init && init->kernel_publish) {
		rib_nl_kernel_publish = init->kernel_publish;
		client[1].type = OB_CLIENT_CB;
		client[1].client_publish = rib_nl_kernel_publish_wrapper;
		num_clients = 2;
	}
	return object_broker_init_all(&obj_init, num_clients, client);
}

void route_broker_publish(const struct nlmsghdr *nlmsg, enum route_priority pri)
{
	object_broker_publish((void *)nlmsg, pri);
}

void route_broker_shutdown_all(void)
{
	object_broker_shutdown_all();
}

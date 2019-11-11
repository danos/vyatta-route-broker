/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#include "broker.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>

CIRCLEQ_HEAD(broker_list, broker) broker_list_head;

#define BROKER_OBJ_SET_DEL(f) \
	(f |= BROKER_FLAGS_DELETE)
/*
 * type_count: how many different types of object will be stored in this
 *             broker.
 */
struct broker *broker_create(const struct broker_ops *broker_ops,
			     size_t type_count)
{
	struct broker *broker;
	static bool inited;

	if (!broker_ops ||
	    !broker_ops->obj_to_broker_obj ||
	    !broker_ops->broker_obj_to_obj ||
	    !broker_ops->lock_obj || !broker_ops->unlock_obj || type_count == 0)
		return NULL;

	broker = calloc(1, sizeof(*broker));
	if (!broker)
		return NULL;

	if (!inited) {
		inited = true;
		CIRCLEQ_INIT(&broker_list_head);
	}
	CIRCLEQ_INSERT_HEAD(&broker_list_head, broker, brokers_list);

	CIRCLEQ_INIT(&broker->b_obj_list_head);
	CIRCLEQ_INIT(&broker->b_client_list_head);
	broker->ops = *broker_ops;
	broker->type_count = type_count;

	return broker;
}

int broker_delete(struct broker *broker)
{
	if (!broker)
		return -ENOENT;

	if (!CIRCLEQ_EMPTY(&broker->b_client_list_head))
		return -ENOTEMPTY;

	if (!CIRCLEQ_EMPTY(&broker->b_obj_list_head))
		return -ENOTEMPTY;

	free(broker);
	return 0;
}

/*
 * If there are no clients then we can delete now.
 * If all clients have an ID >= obj then we can delete.
 *
 * Note this works for the case where the object is marked as deleted
 * and then is moved to the top. This can not be used to check if an
 * object that has just been marked as deleted can be deleted immediately
 * (that can only be done if there are no clients).
 */
static bool no_clients_need_this(struct broker *broker,
				 struct broker_obj *entry)
{
	struct broker_client *client;
	uint64_t id = ~0;

	if (CIRCLEQ_EMPTY(&broker->b_client_list_head))
		return true;

	CIRCLEQ_FOREACH(client, &broker->b_client_list_head, client_list) {
		if (client->broker_obj.id < id)
			id = client->broker_obj.id;
	}

	if (id >= entry->id)
		return true;

	return false;
}

void broker_add_obj(struct broker *broker, void *obj, int type)
{
	struct broker_obj *new = broker->ops.obj_to_broker_obj(obj, type);

	new->obj_type = type;
	new->flags = BROKER_FLAGS_OBJ;
	new->id = ++broker->id;

	broker->ops.lock_obj(new);

	CIRCLEQ_INSERT_TAIL(&broker->b_obj_list_head, new, b_obj_list);
}

void broker_del_obj_now(struct broker *broker, struct broker_obj *entry)
{
	CIRCLEQ_REMOVE(&broker->b_obj_list_head, entry, b_obj_list);
	broker->ops.unlock_obj(entry);
}

void broker_del_obj(struct broker *broker, void *obj, int type)
{
	struct broker_obj *entry = broker->ops.obj_to_broker_obj(obj, type);

	if (CIRCLEQ_EMPTY(&broker->b_client_list_head)) {
		broker_del_obj_now(broker, entry);
		return;
	}

	BROKER_OBJ_SET_DEL(entry->flags);

	/* Move to the top so update can be picked up */
	CIRCLEQ_REMOVE(&broker->b_obj_list_head, entry, b_obj_list);
	CIRCLEQ_INSERT_TAIL(&broker->b_obj_list_head, entry, b_obj_list);
	entry->id = ++broker->id;
}

void broker_upd_obj(struct broker *broker, void *obj, int type)
{
	struct broker_obj *entry = broker->ops.obj_to_broker_obj(obj, type);

	/* An update of a to-be-deleted object recreates it. */
	if (entry->flags & BROKER_FLAGS_DELETE)
		entry->flags &= ~BROKER_FLAGS_DELETE;

	CIRCLEQ_REMOVE(&broker->b_obj_list_head, entry, b_obj_list);
	CIRCLEQ_INSERT_TAIL(&broker->b_obj_list_head, entry, b_obj_list);
	entry->id = ++broker->id;
}

struct broker_client *broker_client_create(struct broker *broker,
					   const struct broker_client_ops
					   *broker_client_ops, const char *name)
{
	struct broker_client *client;

	if (!broker_client_ops)
		return NULL;

	client = calloc(1, sizeof(*client));
	if (!client)
		return NULL;

	client->name = strdup(name);
	if (!client->name) {
		free(client);
		return NULL;
	}

	client->broker = broker;
	client->client_ops = *broker_client_ops;
	client->broker_obj.flags = BROKER_FLAGS_CLIENT;

	CIRCLEQ_INSERT_HEAD(&broker->b_client_list_head, client, client_list);
	CIRCLEQ_INSERT_HEAD(&broker->b_obj_list_head, &client->broker_obj,
			    b_obj_list);

	/*
	 * If there is no data for this client then make the ID the same as the
	 * broker ID. This is needed for the case when there is another client
	 * with a non 0 ID because that makes the broker think there is more
	 * data for this client as the ID is lower than the broker ID.
	 */
	if (!broker_has_more_data(client))
		client->broker_obj.id = broker->id;

	return client;
}

void broker_client_delete(struct broker_client *client)
{
	struct broker_obj *b_obj;
	struct broker_obj *temp = NULL;
	struct broker *broker = client->broker;

	CIRCLEQ_REMOVE(&client->broker->b_client_list_head,
		       client, client_list);
	CIRCLEQ_REMOVE(&client->broker->b_obj_list_head,
		       &client->broker_obj, b_obj_list);
	free(client->name);
	free(client);

	/* Walk the objects to see if there is stuff we can delete now. */
	b_obj = CIRCLEQ_FIRST(&broker->b_obj_list_head);
	while (b_obj != (struct broker_obj *)&broker->b_obj_list_head) {
		temp = b_obj;
		if ((b_obj->flags & BROKER_FLAGS_OBJ) &&
		    (b_obj->flags & BROKER_FLAGS_DELETE)) {
			if (no_clients_need_this(broker, b_obj)) {
				/* remove it, so keep track of obj before */
				temp = CIRCLEQ_PREV(b_obj, b_obj_list);
				broker_del_obj_now(broker, b_obj);
			}
		}
		b_obj = CIRCLEQ_NEXT(temp, b_obj_list);
	}
}

static struct broker_obj *broker_get_next_data_obj(struct broker *broker,
						   struct broker_obj *b_obj)
{
	while ((b_obj = CIRCLEQ_NEXT(b_obj, b_obj_list))) {
		if (b_obj == (struct broker_obj *)&broker->b_obj_list_head)
			return NULL;

		if (b_obj->flags & BROKER_FLAGS_OBJ)
			return b_obj;
	}
	return NULL;
}

bool broker_has_more_data(struct broker_client *client)
{
	return broker_get_next_data_obj(client->broker, &client->broker_obj);
}

/*
 * Find the next object to be 'passed' to the client, and then call the
 * registered callback func to provide the update to the caller.
 */
void *broker_client_get_data(struct broker_client *client)
{
	struct broker_obj *broker_obj;
	void *data;

	if (!client)
		return NULL;

	broker_obj =
	    broker_get_next_data_obj(client->broker, &client->broker_obj);
	if (!broker_obj) {
		/* No more data. Ensure id up to date so don't keep asking */
		client->broker_obj.id = client->broker->id;
		return NULL;
	}

	if (broker_obj->flags & BROKER_FLAGS_DELETE)
		data = client->client_ops.del_obj(broker_obj);
	else
		data = client->client_ops.add_obj(broker_obj);

	CIRCLEQ_REMOVE(&client->broker->b_obj_list_head, &client->broker_obj,
		       b_obj_list);
	CIRCLEQ_INSERT_AFTER(&client->broker->b_obj_list_head, broker_obj,
			     &client->broker_obj, b_obj_list);
	client->broker_obj.id = broker_obj->id;

	if (broker_obj->flags & BROKER_FLAGS_DELETE) {
		if (no_clients_need_this(client->broker, broker_obj))
			broker_del_obj_now(client->broker, broker_obj);
	}

	client->consumed++;
	return data;
}

struct broker_obj *broker_seq_start(struct broker *broker)
{
	struct broker_obj *broker_obj = CIRCLEQ_LAST(&broker->b_obj_list_head);

	if (broker_obj == (struct broker_obj *)&broker->b_obj_list_head)
		return NULL;

	return broker_obj;
}

struct broker_obj *broker_seq_next(struct broker *broker,
				   struct broker_obj *broker_obj)
{
	broker_obj = CIRCLEQ_PREV(broker_obj, b_obj_list);
	if (broker_obj == (struct broker_obj *)&broker->b_obj_list_head)
		return NULL;

	return broker_obj;
}

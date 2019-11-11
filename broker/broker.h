/*-
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

#ifndef _BROKER_H_
#define _BROKER_H_

#include <sys/queue.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#define BROKER_FLAGS_OBJ     0x1
#define BROKER_FLAGS_CLIENT  0x2
#define BROKER_FLAGS_DELETE  0x4

/*
 * Each object added to a broker must contain one of these structures.
 */
struct broker_obj {
	CIRCLEQ_ENTRY(broker_obj) b_obj_list;
	uint32_t obj_type;
	uint32_t flags;
	uint64_t id;
};

/*
 * struct broker_obj *(*obj_to_broker_obj)(struct broker_obj *obj);
 *     Callback used to convert from an obj to the ca_obj related to the obj.
 * void *(*broker_obj_to_obj)(struct broker_obj *ca_obj);
 *     Callback used to convert from the pointer from a ca_obj to the base obj.
 * void (*lock_obj)(struct broker_obj *);
 *     Callback used to lock an obj, so that the broker can keep a reference to
 *     it for as long as it needs.
 * void (*unlock_obj)(struct broker_obj *);
 *     Callback used to release a lock taken on an object.
 */
struct broker_ops {
	struct broker_obj *(*obj_to_broker_obj)(void *obj, int type);
	void *(*broker_obj_to_obj)(struct broker_obj *ca_obj);
	void (*lock_obj)(struct broker_obj *);
	void (*unlock_obj)(struct broker_obj *);
};

#define BROKER_MAX_NAME_LEN 16
struct broker {
	CIRCLEQ_ENTRY(broker) brokers_list;
	CIRCLEQ_HEAD(b_obj_list, broker_obj) b_obj_list_head;
	struct broker_ops ops;
	size_t type_count;
	CIRCLEQ_HEAD(b_client_list, broker_client) b_client_list_head;
	uint64_t id;
	uint64_t imp_dels;
};

/*
 * type_count: how many different types of object will be stored in this
 *             broker.
 */
struct broker *broker_create(const struct broker_ops *broker_ops,
			     size_t type_count);

/* ret 0 == success */
int broker_delete(struct broker *broker);

void broker_add_obj(struct broker *broker, void *obj, int type);
void broker_del_obj(struct broker *broker, void *obj, int type);
void broker_upd_obj(struct broker *broker, void *obj, int type);
/* Delete this obj without updating clients about it */
void broker_del_obj_now(struct broker *broker, struct broker_obj *entry);

/*
 * void *(*add_obj)(struct broker_obj *);
 *     Called when the client asks for data so that it gets it in a format that
 *     it controls. This callback takes an object that has been added/modified
 *     and formats it appropriately for that client.
 * void *(*del_obj)(struct broker_obj *);
 *     Callback that is called when a client asks for data and the next object
 *     has been marked for deletion.
 */
struct broker_client_ops {
	void *(*add_obj)(struct broker_obj *);
	void *(*del_obj)(struct broker_obj *);
};

struct broker_client {
	struct broker *broker;
	struct broker_client_ops client_ops;
	CIRCLEQ_ENTRY(broker_client) client_list;
	struct broker_obj broker_obj;
	unsigned int flags;
	uint64_t id;
	uint64_t consumed;
	char *name;
};

struct broker_client *broker_client_create(struct broker *broker,
					   const struct broker_client_ops
					   *ca_client_ops, const char *name);

void broker_client_delete(struct broker_client *broker_client);

/* Get the next data from the next object in the list */
void *broker_client_get_data(struct broker_client *broker_client);

/* Is there any more data for this client? */
bool broker_has_more_data(struct broker_client *broker_client);

struct broker_obj *broker_seq_start(struct broker *broker);
struct broker_obj *broker_seq_next(struct broker *broker,
				   struct broker_obj *ca_obj);
#endif /* _BROKER_H_ */

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
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <czmq.h>

#include "broker.h"
#include "route_broker_internal.h"
#include "netlink_create.h"
#include "cli.h"

static pthread_t test_consumer_thread;
static bool finished;
static int consumed_count;
/* use this to slow consumer, will only take them when this is positive */
static int available;
static int client_count;

struct route_broker_client *client;

#define BROKER_TEST_CLIENT 100

struct cli cli;

/*
 * The way the tests work is that we define funcs to add/del objects for
 * each type.  After each change to the contents we invoke a verify function
 * that takes as the first argument a list of the types it should expect
 * as it walks the broker. This list is in show order, which is opposite
 * to the order clients retrieve their data.
 *
 * It then takes a list for each supported type,
 * which has the data for that object.  To minimise typing and to keep it
 * easier to read, we define these lists up front in one place, and then
 * use them through out the code.  The lists are only modified at initial
 * setup time.
 *
 * The naming conventions are that a lower case letter refers
 * to an object that is not being deleted, and an upper case refers to one
 * being deleted. Therefore:
 *
 * object list _r would have one route in it, and is not being deleted.
 * object list _R would have one route in it, and is is being deleted.
 *
 * For each route in a route list, there is an r (lower or upper case) to
 * represent the route state, and then an single digit to represent the
 * route. The data is the same in the route list for lower/upper case apart
 * from the fact it is either RTM_NEWROUTE or RTM_DELROUTE.
 *
 * r1      represents route1, being added
 * R1      represents route1 being deleted.
 * r1R2r3  represents route1, then route2 being deleted, followed by route 3.
 * In the show command this would be along the lines of:
 *
 *    a.b.1.d/24
 *  D a.b.2.d/24
 *    a.b.3.d/24
 *
 * C represents a client/consumer, and it has no data, just a position in the
 * object list, so the object list for the above state is rRr,
 * and if we had a consumer at the top (i.e has had updates for all objects)
 * it would be CrRr.
 */

struct route_verify {
	const char *key;
	const char *data;
};

/*
 * Types is a -1 terminated array of the types/client
 *
 * r_vals is a NULL terminated array of the rotues we expect to find,
 * in the order we expect. As there is copying done, compare by contents
 * not pointers.
 */
void verify_seq(const int *types, const struct route_verify *r_vals)
{
	struct broker_obj *b_obj;
	struct rib_route *route;
	struct nlmsghdr *nl;
	int pri;

	b_obj = route_broker_seq_first(&pri);

	while (*types != -(ROUTE_BROKER_TYPES_MAX + 1)) {
		/* Is the next object the correct type? */
		if (b_obj->flags & BROKER_FLAGS_OBJ) {
			/* -ve value means marked for del */
			if (*types > 0) {
				assert(((*types) - 1) == b_obj->obj_type);
				assert(!(b_obj->flags & BROKER_FLAGS_DELETE));
			} else {
				assert((abs(*types) - 1) == b_obj->obj_type);
				assert(b_obj->flags & BROKER_FLAGS_DELETE);
			}
			switch (b_obj->obj_type) {
			case ROUTE_BROKER_ROUTE:
				route = broker_obj_to_rib_route(b_obj);
				assert(route);
				assert(!strcmp(r_vals->key, route->topic));
				nl = (struct nlmsghdr *)r_vals->data;
				assert(!memcmp(r_vals->data, route->data,
					       nl->nlmsg_len));
				r_vals++;
				break;
			}
		} else {
			assert(*types == BROKER_TEST_CLIENT);
		}
		types++;
		b_obj = route_broker_seq_next(b_obj, &pri);
	}

	assert(b_obj == NULL);
	assert(r_vals->data == NULL);
	assert(r_vals->key == NULL);
}

/*
 * Lower case add, upper case marked as delete, but add 1 as we can't have
 * both 0 and -0
 */
#define r (ROUTE_BROKER_ROUTE + 1)
#define R (-(ROUTE_BROKER_ROUTE + 1))
#define M (ROUTE_BROKER_TYPES_MAX + 1)
#define C BROKER_TEST_CLIENT	/* Client value outside range of normal types */

/* key is prefix, scope, table */
const char *k1 = "r 1.1.1.0/24 0 254";
const char *k2 = "r 1.1.2.0/24 0 254";
const char *k3 = "r 1.1.3.0/24 0 254";

char r1_buf[1024];
char r2_buf[1024];
char r3_buf[1024];

char R1_buf[1024];
char R2_buf[1024];
char R3_buf[1024];

/* Ordered list for ease of viewing with priority: c, r, R */
static int obj_none[] = { -M };
static int obj_r[] = { r, -M };

static int obj_rr[] = { r, r, -M };

static int obj_ccc[] = { C, C, C, -M };
static int obj_rrr[] = { r, r, r, -M };

static int obj_ccrc[] = { C, C, r, C, -M };
static int obj_rccc[] = { r, C, C, C, -M };
static int obj_Rccc[] = { R, C, C, C, -M };

static int obj_ccrrc[] = { C, C, r, r, C, -M };
static int obj_crcrc[] = { C, r, C, r, C, -M };
static int obj_rccrc[] = { r, C, C, r, C, -M };
static int obj_Rccrc[] = { R, C, C, r, C, -M };
static int obj_RRccc[] = { R, R, C, C, C, -M };

static int obj_ccrrrc[] = { C, C, r, r, r, C, -M };
static int obj_crrcrc[] = { C, r, r, C, r, C, -M };
static int obj_crrccr[] = { C, r, r, C, C, r, -M };
static int obj_crrrcc[] = { C, r, r, r, C, C, -M };
static int obj_rcrccr[] = { r, C, r, C, C, r, -M };
static int obj_rcrcrc[] = { r, C, r, C, r, C, -M };
static int obj_rcrrcc[] = { r, C, r, r, C, C, -M };
static int obj_rrcrcc[] = { r, r, C, r, C, C, -M };
static int obj_rrrccc[] = { r, r, r, C, C, C, -M };
static int obj_Rcrrcc[] = { R, C, r, r, C, C, -M };
static int obj_Rccrrc[] = { R, C, C, r, r, C, -M };
static int obj_Rrrccc[] = { R, r, r, C, C, C, -M };
static int obj_RRcrcc[] = { R, R, C, r, C, C, -M };
static int obj_RRrccc[] = { R, R, r, C, C, C, -M };
static int obj_RRRccc[] = { R, R, R, C, C, C, -M };

/* Ordered list for ease of viewing with priority: r, R */
static struct route_verify no_routes[1];
static struct route_verify r1[2];
static struct route_verify R2[2];
static struct route_verify R3[2];

static struct route_verify r1r2[3];
static struct route_verify r2r1[3];
static struct route_verify r3r2[3];
static struct route_verify R2r3[3];
static struct route_verify R2R1[3];
static struct route_verify R3R2[3];

static struct route_verify r1r2r3[4];
static struct route_verify r1r3r2[4];
static struct route_verify r2r1r3[4];
static struct route_verify r2r3r1[4];
static struct route_verify r3r1r2[4];
static struct route_verify r3r2r1[4];

static struct route_verify R1r3r2[4];
static struct route_verify R1R2R3[4];
static struct route_verify R1R3r2[4];
static struct route_verify R2R1r3[4];
static struct route_verify R2R1R3[4];
static struct route_verify R2R3r1[4];
static struct route_verify R3r1r2[4];
static struct route_verify R3R2R1[4];

static void build_route_buffers(void)
{
	netlink_add_route(r1_buf, "1.1.1.0/24 nh 4.4.4.2 int:dp2T0");
	netlink_add_route(r2_buf, "1.1.2.0/24 nh 4.4.4.2 int:dp2T0");
	netlink_add_route(r3_buf, "1.1.3.0/24 nh 4.4.4.2 int:dp2T0");

	netlink_del_route(R1_buf, "1.1.1.0/24 nh 4.4.4.2 int:dp2T0");
	netlink_del_route(R2_buf, "1.1.2.0/24 nh 4.4.4.2 int:dp2T0");
	netlink_del_route(R3_buf, "1.1.3.0/24 nh 4.4.4.2 int:dp2T0");

	no_routes[0].key = NULL;
	no_routes[0].data = NULL;

	r1[0].key = k1;
	r1[0].data = r1_buf;
	r1[1].key = NULL;
	r1[1].data = NULL;

	R2[0].key = k2;
	R2[0].data = R2_buf;
	R2[1].key = NULL;
	R2[1].data = NULL;

	R3[0].key = k3;
	R3[0].data = R3_buf;
	R3[1].key = NULL;
	R3[1].data = NULL;

	r1r2[0].key = k1;
	r1r2[0].data = r1_buf;
	r1r2[1].key = k2;
	r1r2[1].data = r2_buf;
	r1r2[2].key = NULL;
	r1r2[2].data = NULL;

	r2r1[0].key = k2;
	r2r1[0].data = r2_buf;
	r2r1[1].key = k1;
	r2r1[1].data = r1_buf;
	r2r1[2].key = NULL;
	r2r1[2].data = NULL;

	R2r3[0].key = k2;
	R2r3[0].data = R2_buf;
	R2r3[1].key = k3;
	R2r3[1].data = r3_buf;
	R2r3[2].key = NULL;
	R2r3[2].data = NULL;

	R2R1[0].key = k2;
	R2R1[0].data = R2_buf;
	R2R1[1].key = k1;
	R2R1[1].data = R1_buf;
	R2R1[2].key = NULL;
	R2R1[2].data = NULL;

	r3r2[0].key = k3;
	r3r2[0].data = r3_buf;
	r3r2[1].key = k2;
	r3r2[1].data = r2_buf;
	r3r2[2].key = NULL;
	r3r2[2].data = NULL;

	R3R2[0].key = k3;
	R3R2[0].data = R3_buf;
	R3R2[1].key = k2;
	R3R2[1].data = R2_buf;
	R3R2[2].key = NULL;
	R3R2[2].data = NULL;

	r1r2r3[0].key = k1;
	r1r2r3[0].data = r1_buf;
	r1r2r3[1].key = k2;
	r1r2r3[1].data = r2_buf;
	r1r2r3[2].key = k3;
	r1r2r3[2].data = r3_buf;
	r1r2r3[3].key = NULL;
	r1r2r3[3].data = NULL;

	R1R2R3[0].key = k1;
	R1R2R3[0].data = R1_buf;
	R1R2R3[1].key = k2;
	R1R2R3[1].data = R2_buf;
	R1R2R3[2].key = k3;
	R1R2R3[2].data = R3_buf;
	R1R2R3[3].key = NULL;
	R1R2R3[3].data = NULL;

	r1r3r2[0].key = k1;
	r1r3r2[0].data = r1_buf;
	r1r3r2[1].key = k3;
	r1r3r2[1].data = r3_buf;
	r1r3r2[2].key = k2;
	r1r3r2[2].data = r2_buf;
	r1r3r2[3].key = NULL;
	r1r3r2[3].data = NULL;

	R1r3r2[0].key = k1;
	R1r3r2[0].data = R1_buf;
	R1r3r2[1].key = k3;
	R1r3r2[1].data = r3_buf;
	R1r3r2[2].key = k2;
	R1r3r2[2].data = r2_buf;
	R1r3r2[3].key = NULL;
	R1r3r2[3].data = NULL;

	R1R3r2[0].key = k1;
	R1R3r2[0].data = R1_buf;
	R1R3r2[1].key = k3;
	R1R3r2[1].data = R3_buf;
	R1R3r2[2].key = k2;
	R1R3r2[2].data = r2_buf;
	R1R3r2[3].key = NULL;
	R1R3r2[3].data = NULL;

	r2r1r3[0].key = k2;
	r2r1r3[0].data = r2_buf;
	r2r1r3[1].key = k1;
	r2r1r3[1].data = r1_buf;
	r2r1r3[2].key = k3;
	r2r1r3[2].data = r3_buf;
	r2r1r3[3].key = NULL;
	r2r1r3[3].data = NULL;

	r2r3r1[0].key = k2;
	r2r3r1[0].data = r2_buf;
	r2r3r1[1].key = k3;
	r2r3r1[1].data = r3_buf;
	r2r3r1[2].key = k1;
	r2r3r1[2].data = r1_buf;
	r2r3r1[3].key = NULL;
	r2r3r1[3].data = NULL;

	R2R1r3[0].key = k2;
	R2R1r3[0].data = R2_buf;
	R2R1r3[1].key = k1;
	R2R1r3[1].data = R1_buf;
	R2R1r3[2].key = k3;
	R2R1r3[2].data = r3_buf;
	R2R1r3[3].key = NULL;
	R2R1r3[3].data = NULL;

	R2R1R3[0].key = k2;
	R2R1R3[0].data = R2_buf;
	R2R1R3[1].key = k1;
	R2R1R3[1].data = R1_buf;
	R2R1R3[2].key = k3;
	R2R1R3[2].data = R3_buf;
	R2R1R3[3].key = NULL;
	R2R1R3[3].data = NULL;

	R2R3r1[0].key = k2;
	R2R3r1[0].data = R2_buf;
	R2R3r1[1].key = k3;
	R2R3r1[1].data = R3_buf;
	R2R3r1[2].key = k1;
	R2R3r1[2].data = r1_buf;
	R2R3r1[3].key = NULL;
	R2R3r1[3].data = NULL;

	r3r1r2[0].key = k3;
	r3r1r2[0].data = r3_buf;
	r3r1r2[1].key = k1;
	r3r1r2[1].data = r1_buf;
	r3r1r2[2].key = k2;
	r3r1r2[2].data = r2_buf;
	r3r1r2[3].key = NULL;
	r3r1r2[3].data = NULL;

	r3r2r1[0].key = k3;
	r3r2r1[0].data = r3_buf;
	r3r2r1[1].key = k2;
	r3r2r1[1].data = r2_buf;
	r3r2r1[2].key = k1;
	r3r2r1[2].data = r1_buf;
	r3r2r1[3].key = NULL;
	r3r2r1[3].data = NULL;

	R3R2R1[0].key = k3;
	R3R2R1[0].data = R3_buf;
	R3R2R1[1].key = k2;
	R3R2R1[1].data = R2_buf;
	R3R2R1[2].key = k1;
	R3R2R1[2].data = R1_buf;
	R3R2R1[3].key = NULL;
	R3R2R1[3].data = NULL;

	R3r1r2[0].key = k3;
	R3r1r2[0].data = R3_buf;
	R3r1r2[1].key = k1;
	R3r1r2[1].data = r1_buf;
	R3r1r2[2].key = k2;
	R3r1r2[2].data = r2_buf;
	R3r1r2[3].key = NULL;
	R3r1r2[3].data = NULL;
}

static void add_route_1(int pri)
{
	route_broker_publish((struct nlmsghdr *)r1_buf, pri);
}

static void add_route_2(int pri)
{
	route_broker_publish((struct nlmsghdr *)r2_buf, pri);
}

static void add_route_3(int pri)
{
	route_broker_publish((struct nlmsghdr *)r3_buf, pri);
}

static void del_route_1(int pri)
{
	route_broker_publish((struct nlmsghdr *)R1_buf, pri);
}

static void del_route_2(int pri)
{
	route_broker_publish((struct nlmsghdr *)R2_buf, pri);
}

static void del_route_3(int pri)
{
	route_broker_publish((struct nlmsghdr *)R3_buf, pri);
}

/* Starting with 3 routes, and a client at the bottom. */
static void consume1(void)
{
	available = 1;
	while (available == 1)
		usleep(1);
}

static void *test_consumer(void *arg)
{
	struct nlmsghdr *nl;

	client = route_broker_client_create("test");
	client_count++;

	while (true) {
		while ((available > 0)
		       && (nl = route_broker_client_get_data(client))) {
			char buf[ROUTE_TOPIC_LEN];
			bool delete = false;

			route_topic(nl, buf, ROUTE_TOPIC_LEN, &delete);

			consumed_count++;
			available--;
			route_broker_client_free_data(client, nl);
		}
		if (finished)
			break;
	}

	pthread_exit(0);
}

static void new_consumer(void)
{
	int rc;

	rc = pthread_create(&test_consumer_thread, NULL, test_consumer, NULL);
	assert(rc == 0);

	while (client_count == 0)
		usleep(1);
}

static void delete_consumer(void)
{
	int rc;

	finished = true;
	rc = pthread_join(test_consumer_thread, NULL);
	assert(rc == 0);
}

int route_broker_dataplane_ctrl_init(const char *cfgfile,
				     object_broker_client_publish_cb publish)
{
	return 0;
}

void route_broker_dataplane_ctrl_shutdown(void)
{
}

void route_broker_kernel_shutdown(void)
{
}

int route_broker_kernel_init(object_broker_client_publish_cb publish)
{
	return 0;
}

int main(int argc, char **argv)
{
	int rc;

	rc = route_broker_init();
	assert(rc == 0);
	route_broker_topic_gen = route_topic;
	route_broker_copy_obj = rib_nl_copy;
	route_broker_free_obj = rib_nl_free;

	build_route_buffers();

	/*
	 * Start testing.
	 */
	verify_seq(obj_none, no_routes);

	/* add routes */
	add_route_1(ROUTE_CONNECTED);
	verify_seq(obj_r, r1);

	add_route_2(ROUTE_CONNECTED);
	verify_seq(obj_rr, r2r1);

	add_route_1(ROUTE_CONNECTED);
	verify_seq(obj_rr, r1r2);

	add_route_3(ROUTE_CONNECTED);
	verify_seq(obj_rrr, r3r1r2);

	/* Delete routes - no consumers so immediate delete */
	del_route_3(ROUTE_CONNECTED);
	verify_seq(obj_rr, r1r2);

	del_route_2(ROUTE_CONNECTED);
	verify_seq(obj_r, r1);

	del_route_1(ROUTE_CONNECTED);
	verify_seq(obj_none, no_routes);

	/* re-add the routes */
	add_route_1(ROUTE_CONNECTED);
	verify_seq(obj_r, r1);

	add_route_2(ROUTE_CONNECTED);
	verify_seq(obj_rr, r2r1);

	add_route_1(ROUTE_CONNECTED);
	verify_seq(obj_rr, r1r2);

	add_route_3(ROUTE_CONNECTED);
	verify_seq(obj_rrr, r3r1r2);

	available = 0;
	new_consumer();
	/*
	 * Current status is we have a broker with 3 routes to be consumed. If
	 * they get marked as deleted now then they have to stay there until
	 * there are no clients below as we can't tell (in the general case) if
	 * the client below already had an add for this object.
	 *
	 *  1.1.3.0
	 *  1.1.1.0
	 *  1.1.2.0
	 *  client
	 */
	verify_seq(obj_rrrccc, r3r1r2);

	del_route_3(ROUTE_CONNECTED);
	verify_seq(obj_Rrrccc, R3r1r2);

	del_route_2(ROUTE_CONNECTED);
	verify_seq(obj_RRrccc, R2R3r1);
	assert(rc == 0);

	del_route_1(ROUTE_CONNECTED);
	verify_seq(obj_RRRccc, R1R2R3);

	/* Wait for the consumer to finsh (note it did not consume these) */
	delete_consumer();

	/*
	 * this client delete should trigger the delete of the routes marked
	 * as deleted
	 */
	route_broker_client_delete(client);
	client_count--;

	verify_seq(obj_none, no_routes);

	/*
	 * Have tested a consumer can be deleted and we tidy up.
	 * Now check consumption.
	 */
	available = 0;
	finished = false;

	add_route_1(ROUTE_CONNECTED);
	verify_seq(obj_r, r1);

	add_route_2(ROUTE_CONNECTED);
	verify_seq(obj_rr, r2r1);

	add_route_1(ROUTE_CONNECTED);
	verify_seq(obj_rr, r1r2);

	add_route_3(ROUTE_CONNECTED);
	verify_seq(obj_rrr, r3r1r2);

	new_consumer();

	/* Start consuming */
	verify_seq(obj_rrrccc, r3r1r2);

	consume1();
	verify_seq(obj_rrcrcc, r3r1r2);

	consume1();
	verify_seq(obj_rcrrcc, r3r1r2);

	consume1();
	verify_seq(obj_crrrcc, r3r1r2);

	/* mark objects as to be deleted */
	del_route_3(ROUTE_CONNECTED);
	verify_seq(obj_Rcrrcc, R3r1r2);

	del_route_1(ROUTE_CONNECTED);
	verify_seq(obj_RRcrcc, R1R3r2);

	del_route_2(ROUTE_CONNECTED);
	verify_seq(obj_RRRccc, R2R1R3);

	/* And consume */
	consume1();
	verify_seq(obj_RRccc, R2R1);

	consume1();
	verify_seq(obj_Rccc, R2);

	consume1();
	verify_seq(obj_ccc, no_routes);

	/*
	 * Now we have 1 client, but no routes. Test adding routes
	 * at different priority levels, and making sure that any
	 * updates at a high priority are always done before a lower
	 * priority.
	 */
	add_route_1(ROUTE_CONNECTED);
	verify_seq(obj_rccc, r1);

	add_route_2(ROUTE_OTHER);
	verify_seq(obj_rccrc, r1r2);

	consume1();
	verify_seq(obj_crcrc, r1r2);

	add_route_3(ROUTE_CONNECTED);
	verify_seq(obj_rcrcrc, r3r1r2);

	consume1();
	verify_seq(obj_crrcrc, r3r1r2);

	consume1();
	verify_seq(obj_crrccr, r3r1r2);

	/*
	 * Client has proessed everything, update routes, changing priority.
	 */

	/* decrease priority - stays at same level */
	add_route_1(ROUTE_OTHER);
	verify_seq(obj_rcrccr, r1r3r2);

	/* Increase priority - so move to the new priority */
	add_route_2(ROUTE_CONNECTED);
	verify_seq(obj_rrcrcc, r2r1r3);

	/* decrease priority - stays at same level */
	add_route_3(ROUTE_OTHER);
	verify_seq(obj_rrrccc, r3r2r1);

	consume1();
	verify_seq(obj_rrcrcc, r3r2r1);

	consume1();
	verify_seq(obj_rcrrcc, r3r2r1);

	consume1();
	verify_seq(obj_crrrcc, r3r2r1);

	/*
	 * Delete routes, giving them lower priority
	 * (everything is now at priority CONNECTED)
	 */
	del_route_1(ROUTE_OTHER);
	verify_seq(obj_Rcrrcc, R1r3r2);

	del_route_2(ROUTE_OTHER);
	verify_seq(obj_RRcrcc, R2R1r3);

	del_route_3(ROUTE_OTHER);
	verify_seq(obj_RRRccc, R3R2R1);

	/*
	 * Now consume, so that we can put back into priority other,
	 * and then delete with a higher priority.
	 */
	consume1();
	verify_seq(obj_RRccc, R3R2);

	consume1();
	verify_seq(obj_Rccc, R3);

	consume1();
	verify_seq(obj_ccc, no_routes);

	/* Re-add */
	add_route_1(ROUTE_OTHER);
	verify_seq(obj_ccrc, r1);

	add_route_2(ROUTE_OTHER);
	verify_seq(obj_ccrrc, r2r1);

	add_route_3(ROUTE_OTHER);
	verify_seq(obj_ccrrrc, r3r2r1);

	/* Delete at higher priority */
	del_route_1(ROUTE_CONNECTED);
	verify_seq(obj_Rccrrc, R1r3r2);

	consume1();
	verify_seq(obj_ccrrc, r3r2);

	del_route_2(ROUTE_CONNECTED);
	verify_seq(obj_Rccrc, R2r3);

	del_route_3(ROUTE_CONNECTED);
	verify_seq(obj_RRccc, R3R2);

	consume1();
	verify_seq(obj_Rccc, R3);

	consume1();
	verify_seq(obj_ccc, no_routes);

	/* Final tidy */
	route_broker_client_delete(client);
	client_count--;

	rc = route_broker_destroy();
	assert(rc == 0);
	assert(client_count == 0);
	printf("All test passed\n");
	return 0;
}

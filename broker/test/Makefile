all:	build test

build:
	@echo About to copy source
	cp ../broker.h .
	cp ../broker.c .
	cp ../topic.c .
	cp ../route_broker.h .
	cp ../route_broker_internal.h .
	cp ../route_broker.c .
	cp ../route_broker_dp_data.c .
	cp ../route_broker_dp_data.h .
	cp ../route_broker_dp_ctrl.c .
	@echo About to build
	gcc -o broker_test -g -Wall -Werror broker.c route_broker.c \
	topic.c broker_test.c netlink_create.c -lmnl -lpthread -lzmq -lczmq

	gcc -o broker_client_test -g -Wall -Werror broker.c route_broker.c \
	route_broker_dp_ctrl.c broker_client_test.c topic.c  netlink_create.c \
	route_broker_dp_data.c -lmnl -lpthread -lzmq -lczmq -linih

	gcc -o broker_dp_test  -O0 -DDEBUG -g -Wall -Werror dp_test.c \
	netlink_create.c -lmnl -lpthread -lzmq -lczmq -linih

test:
	./broker_test
	./broker_client_test

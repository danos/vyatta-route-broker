# Copyright (c) 2018, AT&T Intellectual Property.  All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-only

# binary name
NAME = brokerd

CFLAGS += -g
CFLAGS += -I../broker
CFLAGS += $(shell pkg-config --cflags libczmq)
CFLAGS += $(shell pkg-config --cflags libmnl)

LIBS += ../broker/libvyatta-route-broker.a
LIBS += $(shell pkg-config --libs libczmq)
LIBS += $(shell pkg-config --libs libmnl)
LIBS += -linih -pthread

OBJS = broker_main.o broker_process.o

all: $(NAME)

$(NAME): $(OBJS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $(NAME) $(OBJS) $(LIBS)

clean:
	rm -f $(OBJS) $(NAME)

install: $(NAME)
	install -D $(NAME) $(DESTDIR)/usr/sbin/$(NAME)

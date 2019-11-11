CFLAGS += $(shell pkg-config --cflags libczmq)

NAME := vyatta-route-broker
OBJS := broker.o route_broker.o route_broker_dp_ctrl.o \
	route_broker_dp_data.o route_broker_kernel.o topic.o

INC := route_broker.h
LIB := lib$(NAME).a
PC := lib$(NAME).pc

all: lib

lib: $(OBJS)
	$(AR) rs $(LIB) $(OBJS)

clean:
	$(RM) $(LIB) $(OBJS)

install: $(INC) $(LIB) $(PC)
	install -D $(INC) $(DESTDIR)/usr/include/$(INC)
	install -D $(LIB) $(DESTDIR)/usr/lib/$(LIB)
	install --mode 0644 -D $(PC) $(DESTDIR)/usr/lib/pkgconfig/$(PC)

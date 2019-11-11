DESTDIR =
SUBDIRS = broker daemon
CHECK_SUBDIRS = broker/test
CFLAGS += -Wall -Werror
CFLAGS += -Wmissing-prototypes -Wredundant-decls

all:
	@set -e; for i in $(SUBDIRS); do $(MAKE) $(MFLAGS) -C $$i; done

check:
	@for i in $(CHECK_SUBDIRS); do $(MAKE) -C $$i; done

install:
	@set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i install; done

clean:
	@for i in $(SUBDIRS); do $(MAKE) $(MFLAGS) -C $$i clean; done

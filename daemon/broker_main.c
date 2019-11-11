/*
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <assert.h>
#include <getopt.h>
#include <grp.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include "route_broker.h"
#include "brokerd.h"
#include "fpm.h"

int broker_debug;

static struct option options[] = {
	{ "debug",	no_argument,		NULL,	'd' },
	{ "user",	required_argument,	NULL,	'u' },
	{ "group",	required_argument,	NULL,	'g' },
	{ 0 }
};

void
broker_log_debug(void *arg, const char *fmt, ...)
{
	va_list ap;

	if (broker_debug) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
}

void
broker_log_error(void *arg, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static int
broker_fpm_socket(void)
{
	struct sockaddr_in sin = {};
	socklen_t slen = sizeof(sin);
	int fpm;
	int val;
	int s;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		perror("socket");
		exit(1);
	}

	val = 1;
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		perror("SO_REUSEADDR");
		exit(1);
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons(FPM_DEFAULT_PORT);
	if (bind(s, (struct sockaddr *)&sin, slen) < 0) {
		perror("bind");
		exit(1);
	}

	if (listen(s, 1) < 0) {
		perror("listen");
		exit(1);
	}
	fprintf(stderr, "Listening for FPM connection\n");

	fpm = accept(s, (struct sockaddr *)&sin, &slen);
	if (fpm < 0) {
		perror("accept");
		exit(1);
	}
	fprintf(stderr, "Connected to FPM\n");
	close(s);

	return fpm;
}

static int
broker_netlink_socket(void)
{
	/* BPF filter to accept only kernel protocol routes */
	struct sock_filter filter[] = {
		/* Load byte value from rtm_protocol */
		BPF_STMT(BPF_LD|BPF_ABS|BPF_B,
			 NLMSG_LENGTH(offsetof(struct rtmsg, rtm_protocol))),
		/* If matches RTPROT_KERNEL, goto next, else next+1 */
		BPF_JUMP(BPF_JMP|BPF_JEQ, RTPROT_KERNEL, 0, 1),
		/* Accept */
		BPF_STMT(BPF_RET, 0xffff),
		/* Reject */
		BPF_STMT(BPF_RET, 0),
	};
	struct sock_fprog fprog = {
		.len = ARRAY_SIZE(filter),
		.filter = filter,
	};
	struct sockaddr_nl snl = {};
	int nl;

	nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (nl < 0) {
		perror("netlink socket");
		exit(1);
	}

	snl.nl_family = AF_NETLINK;
	snl.nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
	if (bind(nl, (struct sockaddr *) &snl, sizeof(snl)) < 0) {
		perror("bind netlink");
		exit(1);
	}

	if (setsockopt(nl, SOL_SOCKET, SO_ATTACH_FILTER, &fprog,
		       sizeof(fprog)) < 0) {
		perror("SO_ATTACH_FILTER");
		exit(1);
	}

	return nl;
}

int main(int argc, char **argv)
{
	struct route_broker_init init = {
		.log_debug = broker_log_debug,
		.log_error = broker_log_error,
	};
	struct pollfd fds[2] = {};
	char *group = NULL;
	char *user = NULL;
	ssize_t n;
	int fpm;
	int opt;
	int nl;
	int p;

	while ((opt = getopt_long(argc, argv, "dg:u:", options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			broker_debug = 1;
			break;
		case 'g':
			group = optarg;
			break;
		case 'u':
			user = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [ARGS]\n", argv[0]);
			fprintf(stderr, "  -d,--debug   debugging\n");
			fprintf(stderr, "  -u,--user    user to run as\n");
			fprintf(stderr, "  -g,--group   additional group\n");
			exit(1);
		}
	}

	if (group) {
		struct group *grp = getgrnam(group);

		if (!grp) {
			fprintf(stderr, "no such group: %s\n", group);
			exit(1);
		}
		if (setgroups(1, &grp->gr_gid) < 0) {
			perror("setgroups");
			exit(1);
		}
	}

	if (user) {
		struct passwd *pw = getpwnam(user);

		if (!pw) {
			fprintf(stderr, "no such user: %s\n", user);
			exit(1);
		}
		if (setreuid(-1, pw->pw_uid) < 0) {
			perror("setreuid");
			exit(1);
		}
	}

	fpm = broker_fpm_socket();
	nl = broker_netlink_socket();
	fds[0].fd = nl;
	fds[1].fd = fpm;
	fds[0].events = fds[1].events = POLLIN;

	route_broker_init_all(&init);

	/* Get a dump of existing kernel routes */
	broker_dump_routes();

	for (;;) {
		fds[0].revents = fds[1].revents = 0;
		p = poll(fds, 2, -1);
		if (p < 0) {
			perror("poll");
			route_broker_shutdown_all();
			exit(1);
		}

		/* Netlink from kernel */
		if (fds[0].revents) {
			if (fds[0].revents != POLLIN) {
				fprintf(stderr, "Bad NL event: 0x%x",
					fds[0].revents);
				route_broker_shutdown_all();
				exit(1);
			}
			n = broker_process_nl(nl);
			if (n < 0) {
				perror("NL recv");
				route_broker_shutdown_all();
				exit(1);
			}
			if (n == 0) {
				fprintf(stderr, "NL connection closed\n");
				break;
			}
		}

		/* FPM from zebra */
		if (fds[1].revents) {
			if (fds[1].revents != POLLIN) {
				fprintf(stderr, "Bad FPM event: 0x%x",
					fds[1].revents);
				route_broker_shutdown_all();
				exit(1);
			}
			n = broker_process_fpm(fpm);
			if (n < 0) {
				route_broker_shutdown_all();
				exit(1);
			}
			if (n == 0) {
				fprintf(stderr, "FPM connection closed\n");
				break;
			}
		}
	}

	route_broker_shutdown_all();
	return 0;
}

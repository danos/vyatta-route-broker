/*
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _BROKERD_H_
#define _BROKERD_H_

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)	(sizeof(a)/sizeof((a)[0]))
#endif

extern int broker_debug;

ssize_t broker_process_nl(int fd);
ssize_t broker_process_fpm(int fd);
void broker_dump_routes(void);

void broker_log_debug(void *arg, const char *fmt, ...);
void broker_log_error(void *arg, const char *fmt, ...);

#endif /* _BROKERD_H_ */

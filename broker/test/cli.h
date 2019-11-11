/*
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* File for test reasons. */
#ifndef __TEST_CLI__
#define __TEST_CLI__

#include <stdio.h>

struct cli {
	int something;
};

#define cli_out(a, b, ARGS...) \
	printf(b, ##ARGS)

#endif /* __TEST_CLI__ */

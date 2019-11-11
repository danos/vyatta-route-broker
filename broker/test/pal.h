/*
 * Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* File for test reasons, including only the pieces needed to compile tests */
#ifndef __TEST_PAL__
#define __TEST_PAL__

#define zlog_debug(a, b, ...) printf(b, __VA_ARGS__)
#define zlog_err(a, b, ...) assert(0)
#define PAL_ZG

#define VRF_ID_MAIN 1

#endif /* __TEST_PAL__ */

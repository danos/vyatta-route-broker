/* Copyright (c) 2018, AT&T Intellectual Property. All rights reserved.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/* File for test reasons, including only the pieces needed to compile tests */
#ifndef __TEST_PLAT_RT_NETLINK_H__
#define __TEST_PLAT_RT_NETLINK_H__

#ifdef HAVE_KERNEL_MPLS
#ifndef AF_MPLS
#define AF_MPLS 28
#endif
#endif /* HAVE_KERNEL_MPLS */

#endif /* __TEST_PLAT_RT_NETLINK_H__ */

/*
 * vr_os.h
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __VR_OS_H__
#define __VR_OS_H__

#define __attribute__packed__open__                     /* do nothing */
#define __attribute__packed__close__                    __attribute__((__packed__))
#define __attribute__format__(...)                      __attribute__((format(__VA_ARGS__)))
#define __attribute__unused__                           __attribute__((unused))

#define vr_sync_sub_and_fetch_16u(a, b)                 __sync_sub_and_fetch((a), (b))
#define vr_sync_sub_and_fetch_32u(a, b)                 __sync_sub_and_fetch((a), (b))
#define vr_sync_sub_and_fetch_32s(a, b)                 __sync_sub_and_fetch((a), (b))
#define vr_sync_sub_and_fetch_64u(a, b)                 __sync_sub_and_fetch((a), (b))
#define vr_sync_sub_and_fetch_64s(a, b)                 __sync_sub_and_fetch((a), (b))
#define vr_sync_add_and_fetch_16u(a, b)                 __sync_add_and_fetch((a), (b))
#define vr_sync_add_and_fetch_32u(a, b)                 __sync_add_and_fetch((a), (b))
#define vr_sync_add_and_fetch_64u(a, b)                 __sync_add_and_fetch((a), (b))
#define vr_sync_fetch_and_add_32u(a, b)                 __sync_fetch_and_add((a), (b))
#define vr_sync_fetch_and_add_64u(a, b)                 __sync_fetch_and_add((a), (b))
#define vr_sync_fetch_and_or_16u(a, b)                  __sync_fetch_and_or((a), (b))
#define vr_sync_and_and_fetch_16u(a, b)                 __sync_and_and_fetch((a), (b))
#define vr_sync_and_and_fetch_32u(a, b)                 __sync_and_and_fetch((a), (b))
#define vr_sync_bool_compare_and_swap_8s(a, b, c)       __sync_bool_compare_and_swap((a), (b), (c))
#define vr_sync_bool_compare_and_swap_8u(a, b, c)       __sync_bool_compare_and_swap((a), (b), (c))
#define vr_sync_bool_compare_and_swap_16u(a, b, c)      __sync_bool_compare_and_swap((a), (b), (c))
#define vr_sync_bool_compare_and_swap_32u(a, b, c)      __sync_bool_compare_and_swap((a), (b), (c))
#define vr_sync_bool_compare_and_swap_p(a, b, c)        __sync_bool_compare_and_swap((a), (b), (c))
#define vr_sync_val_compare_and_swap_16u(a, b, c)       __sync_val_compare_and_swap((a), (b), (c))
#define vr_sync_lock_test_and_set_8u(a, b)              __sync_lock_test_and_set((a), (b))
#define vr_sync_lock_test_and_set_p(a, b)               __sync_lock_test_and_set((a), (b))
#define vr_sync_synchronize                             __sync_synchronize
#define vr_ffs_32(a)                                    __builtin_ffs(a)
#define vr_likely(a)                                    __builtin_expect(!!(a), 1)
#define vr_unlikely(a)                                  __builtin_expect(!!(a), 0)
#define vr_pause                                        __builtin_ia32_pause

#if defined(__linux__)
#ifdef __KERNEL__

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/times.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>

#include <asm/checksum.h>
#include <asm/bug.h>
#include <asm/atomic.h>
#include <asm/unaligned.h>

#include <net/tcp.h>
#include <net/netlink.h>
#include <net/genetlink.h>

#define ASSERT(x) BUG_ON(!(x));

#else /* __KERNEL */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>

#define ASSERT(x) assert((x));

typedef __signed__ char __s8;
typedef unsigned char __u8;

typedef __signed__ short __s16;
typedef unsigned short __u16;

typedef __signed__ int __s32;
typedef unsigned int __u32;

#endif /* __KERNEL__ */
#endif /* __linux__ */

extern int vrouter_dbg;

#endif /* __VR_OS_H__ */

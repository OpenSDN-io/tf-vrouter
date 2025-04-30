/*
 * vr_compat.h - compatibility definitions
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __VRCOMPAT_H__
#define __VRCOMPAT_H__

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))
#if (! (defined(RHEL_MAJOR) && defined(RHEL_MINOR) && \
           (RHEL_MAJOR == 6) && (RHEL_MINOR >= 5)))
typedef u64 netdev_features_t;
#endif
#endif

#define VLAN_CFI_MASK       0x1000
#define VLAN_TAG_PRESENT    VLAN_CFI_MASK

/*
 * As per lxr, skb_get_rxhash exists in 3.13 versions and disappeared in
 * 3.14. We do not know of in between versions. However, the ubuntu
 * sources for 3.13.0-32 does not have it (for which the LINUX_VERSION
 * CODE is 199947, which corresponds to 3.13.11) and hence the following.
 *
 * But then in 3.13.0-36, ubuntu did
 *
 * #define skb_get_rxhash skb_get_hash
 */
#if ((LINUX_VERSION_CODE > KERNEL_VERSION(2,6,32)) && \
            (LINUX_VERSION_CODE < KERNEL_VERSION(3,13,11)))
#if (!defined(RHEL_MAJOR) || (RHEL_MAJOR < 7))
static inline __u32
skb_get_hash(struct sk_buff *skb)
{
    return skb_get_rxhash(skb);
}
#endif
#endif


#endif

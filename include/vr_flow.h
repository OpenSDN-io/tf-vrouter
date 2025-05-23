/*
 * vr_flow.h --
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */
#ifndef __VR_FLOW_H__
#define __VR_FLOW_H__

#include "vr_defs.h"
#include "vr_htable.h"

#define VR_FLOW_ACTION_DROP         0x0
#define VR_FLOW_ACTION_HOLD         0x1
#define VR_FLOW_ACTION_FORWARD      0x2
#define VR_FLOW_ACTION_NAT          0x3

typedef enum {
    FLOW_HELD,
    FLOW_FORWARD,
    FLOW_DROP,
    FLOW_TRAP,
    FLOW_CONSUMED,
    FLOW_EVICT_DROP,
} flow_result_t;

#define VR_FRAG_FLAG_NONE               0x0000
#define VR_FRAG_FLAG_MIRROR             0x0001

#define VR_FLOW_RESP_FLAG_DELETED       0x0001

#define VR_FLOW_FLAG_ACTIVE             0x0001
#define VR_FLOW_FLAG_MODIFIED           0x0100
#define VR_FLOW_FLAG_NEW_FLOW           0x0200
#define VR_FLOW_FLAG_EVICT_CANDIDATE    0x0400
#define VR_FLOW_FLAG_EVICTED            0x0800
#define VR_RFLOW_VALID                  0x1000
#define VR_FLOW_FLAG_MIRROR             0x2000
#define VR_FLOW_FLAG_VRFT               0x4000
#define VR_FLOW_FLAG_LINK_LOCAL         0x8000

/* fe_flags1 */
#define VR_FLOW_FLAG1_HBS_LEFT          0x1000
#define VR_FLOW_FLAG1_HBS_RIGHT         0x2000
#define VR_FLOW_FLAG1_HBS_MASK          (VR_FLOW_FLAG1_HBS_LEFT |\
                                         VR_FLOW_FLAG1_HBS_RIGHT)

#define VR_FLOW_FLAG_DP_FLAGS           (VR_FLOW_FLAG_EVICT_CANDIDATE |\
                                            VR_FLOW_FLAG_EVICTED |\
                                            VR_FLOW_FLAG_NEW_FLOW |\
                                            VR_FLOW_FLAG_MODIFIED)

#define VR_FLOW_FLAG_DP_BITS(fe)        (((fe)->fe_flags) &\
                                            (VR_FLOW_FLAG_DP_FLAGS))
#define VR_FLOW_FLAG_MASK(flag)         ((flag) & ~(VR_FLOW_FLAG_DP_FLAGS))
/* rest of the flags are action specific */

/* for NAT */
#define VR_FLOW_FLAG_SNAT           0x2
#define VR_FLOW_FLAG_SPAT           0x4
#define VR_FLOW_FLAG_DNAT           0x8
#define VR_FLOW_FLAG_DPAT           0x10
#define VR_FLOW_FLAG_NAT_MASK       (VR_FLOW_FLAG_SNAT | VR_FLOW_FLAG_SPAT | \
        VR_FLOW_FLAG_DNAT | VR_FLOW_FLAG_DPAT)

/* for TRAP */
#define VR_FLOW_FLAG_TRAP_ECMP      0x20
#define VR_FLOW_FLAG_TRAP_MASK      (VR_FLOW_FLAG_TRAP_ECMP)

#define VR_FLOW_FLAG_DELETE_MARKED  0x40
#define VR_FLOW_BGP_SERVICE         0x80

#define VR_FLOW_EXT_FLAG_FORCE_EVICT    0x0001
/* Mock src UDP port used to set constant port value
 * for vtest(vrouter simulation framework) */
#define VR_FLOW_EXT_FLAG_MOCK_SRC_UDP   0x0002
#define VR_FLOW_MOCK_SRC_UDP_PORT       0x12b5

/* Flow Action Reason code */
#define VR_FLOW_DR_UNKNOWN                0x00
#define VR_FLOW_DR_UNAVIALABLE_INTF       0x01
#define VR_FLOW_DR_IPv4_FWD_DIS           0x02
#define VR_FLOW_DR_UNAVAILABLE_VRF        0x03
#define VR_FLOW_DR_NO_SRC_ROUTE           0x04
#define VR_FLOW_DR_NO_DST_ROUTE           0x05
#define VR_FLOW_DR_AUDIT_ENTRY            0x06
#define VR_FLOW_DR_VRF_CHANGE             0x07
#define VR_FLOW_DR_NO_REVERSE_FLOW        0x08
#define VR_FLOW_DR_REVERSE_FLOW_CHANGE    0x09
#define VR_FLOW_DR_NAT_CHANGE             0x0a
#define VR_FLOW_DR_FLOW_LIMIT             0x0b
#define VR_FLOW_DR_LINKLOCAL_SRC_NAT      0x0c
#define VR_FLOW_DR_FAILED_VROUTER_INSTALL 0x0d
#define VR_FLOW_DR_INVALID_L2_FLOW        0x0e
#define VR_FLOW_DR_FLOW_ON_TSN            0x0f
#define VR_FLOW_DR_NO_MIRROR_ENTRY        0x10
#define VR_FLOW_DR_SAME_FLOW_RFLOW_KEY    0x11
#define VR_FLOW_DR_PORT_MAP_DROP          0x12
#define VR_FLOW_DR_NO_SRC_ROUTE_L2RPF     0x13
#define VR_FLOW_DR_FAT_FLOW_NAT_CONFLICT  0x14
#define VR_FLOW_DR_POLICY                 0x15
#define VR_FLOW_DR_OUT_POLICY             0x16
#define VR_FLOW_DR_SG                     0x17
#define VR_FLOW_DR_OUT_SG                 0x18
#define VR_FLOW_DR_REVERSE_SG             0x19
#define VR_FLOW_DR_REVERSE_OUT_SG         0x1a
#define VR_FLOW_DR_FW_POLICY              0x1b
#define VR_FLOW_DR_OUT_FW_POLICY          0x1c
#define VR_FLOW_DR_REVERSE_FW_POLICY      0x1d
#define VR_FLOW_DR_REVERSE_OUT_FW_POLICY  0x1e
#define VR_FLOW_DR_FWAAS_POLICY              0x1f
#define VR_FLOW_DR_OUT_FWAAS_POLICY          0x20
#define VR_FLOW_DR_REVERSE_FWAAS_POLICY      0x21
#define VR_FLOW_DR_REVERSE_OUT_FWAAS_POLICY  0x22

#define VR_IP6_ADDRESS_LEN               16

#define VR_FLOW_FAMILY(type) \
        ((type == VP_TYPE_IP6) ? AF_INET6 \
                               : AF_INET)

#define VR_FLOW_KEY_ALL          0x1F
#define VR_FLOW_KEY_NONE         0x00
#define VR_FLOW_KEY_PROTO        0x01
#define VR_FLOW_KEY_SRC_IP       0x02
#define VR_FLOW_KEY_SRC_PORT     0x04
#define VR_FLOW_KEY_DST_IP       0x08
#define VR_FLOW_KEY_DST_PORT     0x10

#define VR_LL_RP_TCP_INDEX     0x0
#define VR_LL_RP_UDP_INDEX     0x1
#define VR_LL_RP_ICMP_INDEX    0x2
#define VR_LL_RP_MAX           0x3

struct vr_forwarding_md;
struct _vr_flow_req;

struct vr_flow_defer_data {
    struct vr_flow_queue *vfdd_flow_queue;
    struct vr_flow_entry *vfdd_fe;
    unsigned int vfdd_fe_index;
    bool vfdd_delete;
    bool vfdd_evict_flow;
};

__attribute__packed__open__
struct vr_common_flow {
    unsigned char  ip_family;
    unsigned char  ip_proto;
    unsigned short ip_unused;
    unsigned short ip_sport;
    unsigned short ip_dport;
    unsigned int   ip_nh_id;
    unsigned char  ip_addr[2 * VR_IP6_ADDRESS_LEN];
} __attribute__packed__close__;

__attribute__packed__open__
struct vr_inet_flow {
    unsigned char  ip4_family;
    unsigned char  ip4_proto;
    unsigned short ip4_unused;
    unsigned short ip4_sport;
    unsigned short ip4_dport;
    unsigned int   ip4_nh_id;
    unsigned int   ip4_sip;
    unsigned int   ip4_dip;
} __attribute__packed__close__;

__attribute__packed__open__
struct vr_inet6_flow {
    unsigned char  ip6_family;
    unsigned char  ip6_proto;
    unsigned short ip6_unused;
    unsigned short ip6_sport;
    unsigned short ip6_dport;
    unsigned int   ip6_nh_id;
    unsigned char  ip6_sip[VR_IP6_ADDRESS_LEN];
    unsigned char  ip6_dip[VR_IP6_ADDRESS_LEN];
} __attribute__packed__close__;

__attribute__packed__open__
struct vr_flow {
    union {
        struct vr_common_flow ip_key;
        struct vr_inet_flow ip4_key;
        struct vr_inet6_flow ip6_key;
    } key_u;
    uint8_t   vr_flow_keylen;
} __attribute__packed__close__;

#define flow_key_len   vr_flow_keylen
#define flow_family    key_u.ip_key.ip_family
#define flow_sport     key_u.ip_key.ip_sport
#define flow_dport     key_u.ip_key.ip_dport
#define flow_nh_id     key_u.ip_key.ip_nh_id
#define flow_proto     key_u.ip_key.ip_proto
#define flow_ip        key_u.ip_key.ip_addr
#define flow_unused    key_u.ip_key.ip_unused
#define flow4_family   key_u.ip4_key.ip4_family
#define flow4_sip      key_u.ip4_key.ip4_sip
#define flow4_dip      key_u.ip4_key.ip4_dip
#define flow4_sport    key_u.ip4_key.ip4_sport
#define flow4_dport    key_u.ip4_key.ip4_dport
#define flow4_nh_id    key_u.ip4_key.ip4_nh_id
#define flow4_proto    key_u.ip4_key.ip4_proto
#define flow4_unused   key_u.ip4_key.ip4_unused
#define flow6_family   key_u.ip6_key.ip6_family
#define flow6_sip      key_u.ip6_key.ip6_sip
#define flow6_dip      key_u.ip6_key.ip6_dip
#define flow6_sport    key_u.ip6_key.ip6_sport
#define flow6_dport    key_u.ip6_key.ip6_dport
#define flow6_nh_id    key_u.ip6_key.ip6_nh_id
#define flow6_proto    key_u.ip6_key.ip6_proto
#define flow6_unused   key_u.ip6_key.ip6_unused

#define VR_FLOW_IPV6_HASH_SIZE           sizeof(struct vr_inet6_flow)
#define VR_FLOW_IPV4_HASH_SIZE           sizeof(struct vr_inet_flow)
#define VR_FLOW_HASH_SIZE(type) \
        ((type == VP_TYPE_IP6) ? VR_FLOW_IPV6_HASH_SIZE \
                               : VR_FLOW_IPV4_HASH_SIZE)

/*
 * Limit the number of outstanding flows in hold state. The flow rate can
 * be much more than what agent can handle. In such cases, to make sure that
 *
 * . pkt0 is not overrun
 * . too many packets are not cached in flow table
 * . too many entries in hold state which will get serviced slowly (of the
 * order of seconds)
 * . and thus starving the table of new entries
 *
 * we limit the number of entries that are in hold state. In a simplistic
 * scenario, all we would need is one variable that is incremented every
 * time a hold entry is added, and decremented when agent changes the
 * state of the entry from hold to any other state (including deletion).
 * However, there will be contention for that variable from all cpus. To
 * avoid the contention, we will make it a per-cpu variable. Once we make
 * a per-cpu variable, there no longer can be a single variable whose
 * value can be decremented. So, to work around that problem, we
 * will have two monotonically incrementing objects, monitoring the hold
 * count and the count of entries that went from hold to active/delete.
 * The single variable that tracks the latter can't be 32 bit, but the
 * former has to be 32 bit since the sum of all of them has to be compared
 * against the latter, and hence can't be each 64bit.
 *
 * How do we solve overflows?
 *
 * We don't care for overflow of the first variable (since it is 64bit).
 * Whenever any of the per-cpu variable (32bit) overflows, our strategy
 * is to decrement the 64 bit variable from the 32 bit, if the former is
 * lesser or to decrement the 64 bit variable by 32bit_max, if the former
 * is greater. Only in those cases, lock is taken. It is guaranteed that
 * no two values will differ by more than hold count.
 */
struct vr_flow_table_info {
    uint64_t vfti_burst_tokens;
    uint64_t vfti_burst_used;
    uint64_t vfti_deleted;
    uint64_t vfti_changed;
    uint64_t vfti_action_count;
    uint64_t vfti_added;
    uint32_t vfti_oflows;
    uint32_t vfti_burst_step_configured;
    uint32_t vfti_burst_interval_configured;
    uint32_t vfti_burst_tokens_configured;
    struct vr_timer *vfti_timer;
    uint32_t vfti_hold_count[];
};

/*
 * flow bytes and packets are of same width. this should be
 * ok since agent really has to take care of overflows. this
 * is also better probably because processor does not have to
 * do bit operations
 */
__attribute__packed__open__
struct vr_flow_stats {
    uint32_t flow_bytes;
    uint32_t flow_packets;
    uint16_t flow_bytes_oflow;
    uint8_t  flow_packets_oflow;
} __attribute__packed__close__;

#define VR_MAX_FLOW_QUEUE_ENTRIES   3U

#define PN_FLAG_LABEL_IS_VXLAN_ID   0x1
#define PN_FLAG_TO_ME               0x2
#define PN_FLAG_FRAGMENT_HEAD       0x4

struct vr_packet_node {
    struct vr_packet *pl_packet;
    uint32_t pl_outer_src_ip;
    uint32_t pl_inner_src_ip;
    uint32_t pl_inner_dst_ip;
    uint32_t pl_label;
    uint32_t pl_vif_idx;
    uint16_t pl_flags;
    int8_t pl_dscp;
    int8_t pl_dotonep;
    uint32_t pl_vrf;
    uint16_t pl_vlan;
    uint16_t pl_mirror_vlan;
    unsigned short pl_custom;
};

struct vr_flow_queue {
    unsigned int vfq_index;
    unsigned int vfq_entries;
    struct vr_packet_node vfq_pnodes[VR_MAX_FLOW_QUEUE_ENTRIES];
};

/*
 * Flow eviction:
 * 1. Requirement
 * --------------
 *
 * Inactive TCP flows (flows that have already seen the closure cycle - FIN/ACK
 * or the RESET flags) should additionally be considered as a free flow entry
 * so that vRouter does not have to wait for agent's aging cycle to accommodate
 * new flows under severe occupancy and provide better service.
 *
 * 2. Problems in datapath initiated flow closure
 * ----------------------------------------------
 *
 * . Simultaneous discovery of the same flow entry by two different CPUs
 * . Simultaneous closure of an entry by both agent as well as from datapath
 * . Handling of packets held in the flow entry when the entry moves from hold to
 *   closed state
 *
 * 3. Implementation
 * -----------------
 *
 * 3.1 Marking
 * -----------
 *
 * Once the TCP state machine determines that a flow can be closed, it updates
 * the tcp flags with a new flag VR_FLOW_TCP_DEAD, since determining whether a
 * tcp flow has seen its end with only the existing TCP flags is a bit more
 * involved. The last packet before exiting the module, marks the flow as a an
 * eviction candidate (VR_FLOW_FLAG_EVICT_CANDIDATE).
 *
 * 3.2 Allocation/Eviction
 * -----------------------
 *
 * Once the last packet exits the flow module, a work is scheduled to mark the
 * flow as inactive. This work will schedule and RCU call back to mark the entry
 * as inactive (this is the same flow for deletion of flow from agent). While
 * deleting the entry, the evicted flow will also be marked as evicted (VR_FLOW_
 * FLAG_EVICTED).
 *
 */
#define VR_FLOW_TCP_FIN             0x0001
#define VR_FLOW_TCP_HALF_CLOSE      0x0002
#define VR_FLOW_TCP_FIN_R           0x0004
#define VR_FLOW_TCP_SYN             0x0008
#define VR_FLOW_TCP_SYN_R           0x0010
#define VR_FLOW_TCP_ESTABLISHED     0x0020
#define VR_FLOW_TCP_ESTABLISHED_R   0x0040
#define VR_FLOW_TCP_RST             0x0080
#define VR_FLOW_TCP_DEAD            0x8000

/* align to 8 byte boundary */
#define VR_FLOW_KEY_PAD ((8 - (sizeof(struct vr_flow) % 8)) % 8)

__attribute__packed__open__
struct vr_dummy_flow_entry {
    vr_hentry_t fe_hentry;
    uint8_t fe_ttl;
    int16_t fe_qos_id;
    struct vr_flow fe_key;
    uint8_t fe_gen_id;
    uint16_t fe_tcp_flags;
    struct vr_flow_queue *fe_hold_list;
    unsigned int fe_tcp_seq;
    int fe_rflow;
    unsigned short fe_flags;
    unsigned short fe_flags1;
    unsigned short fe_action;
    unsigned short fe_vrf;
    unsigned short fe_dvrf;
    uint8_t fe_mirror_id;
    uint8_t fe_sec_mirror_id;
    uint32_t fe_src_nh_index;
    struct vr_flow_stats fe_stats;
    int8_t fe_ecmp_nh_index;
    uint8_t fe_drop_reason;
    uint8_t fe_type;
    unsigned short fe_udp_src_port;
    uint32_t fe_src_info;
    struct vr_mirror_meta_entry *fe_mme;
    unsigned int fe_tcp_ack;
    int8_t fe_underlay_ecmp_index;
    uint8_t fe_bucket_lock;
} __attribute__packed__close__;

/*
 * Flow entry size must be cache line aligned and factor of 4MB page size
 */
#define VR_FLOW_ENTRY_PACK (256 - sizeof(struct vr_dummy_flow_entry))

/* do not change. any field positions as it might lead to incompatibility */
__attribute__packed__open__
struct vr_flow_entry {
    vr_hentry_t fe_hentry;
    uint8_t fe_ttl;
    int16_t fe_qos_id;
    struct vr_flow fe_key;
    uint8_t fe_gen_id;
    uint16_t fe_tcp_flags;
    struct vr_flow_queue *fe_hold_list;
    unsigned int fe_tcp_seq;
    int fe_rflow;
    unsigned short fe_flags;
    unsigned short fe_flags1;
    unsigned short fe_action;
    unsigned short fe_vrf;
    unsigned short fe_dvrf;
    uint8_t fe_mirror_id;
    uint8_t fe_sec_mirror_id;
    uint32_t fe_src_nh_index;
    struct vr_flow_stats fe_stats;
    int8_t fe_ecmp_nh_index;
    uint8_t fe_drop_reason;
    uint8_t fe_type;
    unsigned short fe_udp_src_port;
    /*
     * fe_src_info holds outer source IP if the packet is received on
     * Fabric or the interface index if packet is received on Vmi. This
     * helps, if reverse flow is Ecmp, in choosing the reverse flow's
     * component NH as this source
     */
    uint32_t fe_src_info;
    struct vr_mirror_meta_entry *fe_mme;
    unsigned int fe_tcp_ack;
    int8_t fe_underlay_ecmp_index;
    /* Note: Used only in the first entry of the bucket. May be used even if
     * the whole entry is invalid. This is a spinlock to synchronize
     * vr_add_flow and vr_flow_lookup and avoid duplicate entries. */
    uint8_t fe_bucket_lock;
    unsigned char fe_pack[VR_FLOW_ENTRY_PACK];
} __attribute__packed__close__;

#define VR_FLOW_PROTO_SHIFT             16

#define VR_UDP_DHCP_SPORT   (17 << 16 | htons(67))
#define VR_UDP_DHCP_CPORT   (17 << 16 | htons(68))
#define VR_UDP_DNS_SPORT    (17 << 16 | htons(53))
#define VR_TCP_DNS_SPORT    (6 << 16 | htons(53))

#define VR_DHCP6_SPORT htons(546)
#define VR_DHCP6_DPORT htons(547)

#define VR_DNS_SERVER_PORT  htons(53)

#define VR_DEF_FLOW_ENTRIES   (512 * 1024)

extern unsigned int vr_flow_entries, vr_oflow_entries;

#define VR_FLOW_TABLE_SIZE   (vr_flow_entries * sizeof(struct vr_flow_entry))
#define VR_OFLOW_TABLE_SIZE  (vr_oflow_entries * sizeof(struct vr_flow_entry))

struct vr_flow_md {
    struct vrouter *flmd_router;
    struct vr_defer_data *flmd_defer_data;
    unsigned int flmd_index;
    unsigned short flmd_flags;
};

struct vr_flow_trap_arg {
    unsigned int vfta_index;
    unsigned int vfta_nh_index;
    struct vr_flow_stats vfta_stats;
    uint8_t vfta_gen_id;
};

#define VR_FAT_FLOW_NO_MASK        0x0
#define VR_FAT_FLOW_SRC_PORT_MASK  0x1
#define VR_FAT_FLOW_DST_PORT_MASK  0x2
#define VR_FAT_FLOW_SRC_IP_MASK    0x4
#define VR_FAT_FLOW_DST_IP_MASK    0x8
#define VR_FAT_FLOW_MAX_MASK       0x9

#define VR_HBS_FROM_VMI            0x1
#define VR_HBS_FROM_FABRIC         0x2
#define VR_HBS_L3_PKT              0x4

#define VR_HBS_SMAC_MAGIC          0xCAF0
#define VR_HBS_DMAC_MAGIC          0xC0D0
#define VR_HBS_MAGIC_MASK          0xFFF0

struct vr_packet;
struct vrouter;
struct vr_ip;
struct vr_ip6;

extern int vr_flow_init(struct vrouter *);
extern int vr_flow_mem(struct vrouter *);
extern void vr_flow_exit(struct vrouter *, bool);

extern bool vr_flow_forward(struct vrouter *,
        struct vr_packet *, struct vr_forwarding_md *);

void *vr_flow_get_va(struct vrouter *, uint64_t);

unsigned int vr_flow_table_size(struct vrouter *);

struct vr_flow_entry *vr_flow_get_entry(struct vrouter *, int);
flow_result_t vr_flow_lookup(struct vrouter *, struct vr_flow *,
                             struct vr_packet *, struct vr_forwarding_md *);

flow_result_t vr_inet_flow_lookup(struct vrouter *, struct vr_packet *,
                                  struct vr_forwarding_md *);
flow_result_t vr_inet6_flow_lookup(struct vrouter *, struct vr_packet *,
                                  struct vr_forwarding_md *);
int vr_inet6_form_flow(struct vrouter *, unsigned short, struct vr_packet *,
        uint16_t, struct vr_ip6 *, struct vr_flow *, uint8_t, unsigned short, bool);
int vr_inet6_get_flow_key(struct vrouter *, unsigned short, struct vr_packet *,
        uint16_t, struct vr_flow *, uint8_t, unsigned short);

extern unsigned int vr_inet_flow_nexthop(struct vr_packet *, unsigned short);
extern flow_result_t vr_inet_flow_nat(struct vr_flow_entry *,
        struct vr_packet *, struct vr_forwarding_md *);
extern flow_result_t vr_inet6_flow_nat(struct vr_flow_entry *,
        struct vr_packet *, struct vr_forwarding_md *);
extern void vr_inet_fill_flow(struct vr_flow *, unsigned int,
       uint32_t, uint32_t, uint8_t, uint16_t, uint16_t, uint8_t);
extern void vr_inet6_fill_flow(struct vr_flow *, unsigned int,
       unsigned char *, unsigned char *, uint8_t, uint16_t, uint16_t, uint8_t);
extern void vr_inet6_fill_flow_from_req(struct vr_flow *,
        struct _vr_flow_req *);
extern void vr_inet6_fill_rflow_from_req(struct vr_flow *,
        struct _vr_flow_req *);
extern void vr_fill_flow_common(struct vr_flow *, unsigned int,
                uint8_t, uint16_t, uint16_t, uint8_t, uint8_t);
extern bool vr_inet_flow_is_fat_flow(struct vrouter *, struct vr_packet *,
        struct vr_flow_entry *);
extern bool vr_inet6_flow_is_fat_flow(struct vrouter *, struct vr_packet *,
        struct vr_flow_entry *);
extern bool vr_inet_flow_allow_new_flow(struct vrouter *, struct vr_packet *);
extern int vr_inet_get_flow_key(struct vrouter *, struct vr_packet *,
        struct vr_forwarding_md *, struct vr_flow *, uint8_t, unsigned short);
extern unsigned int vr_reinject_packet(struct vr_packet *,
        struct vr_forwarding_md *);
extern void vr_flow_set_burst_params(struct vrouter *,int,int,int);
extern void vr_flow_get_burst_params(struct vrouter *,int *,int *,int *);


bool vr_valid_link_local_port(struct vrouter *, int, int, int);
int vr_inet_form_flow(struct vrouter *, unsigned short,
                struct vr_packet *, uint16_t, struct vr_flow *, uint8_t,
                unsigned short);
int vr_flow_flush_pnode(struct vrouter *, struct vr_packet_node *,
                struct vr_flow_entry *, struct vr_forwarding_md *);
void vr_flow_fill_pnode(struct vr_packet_node *, struct vr_packet *,
        struct vr_forwarding_md *);
uint16_t vr_flow_fat_flow_lookup(struct vrouter *router, struct vr_packet *pkt,
              uint16_t l4_proto, uint16_t sport, uint16_t dport,
              unsigned int *saddr, unsigned int *daddr,
              unsigned char *ip6_src, unsigned char *ip6_dst);
extern int16_t vr_flow_get_qos(struct vrouter *, struct vr_packet *,
        struct vr_forwarding_md *);
unsigned int vr_flow_table_used_oflow_entries(struct vrouter *);
unsigned int vr_flow_table_used_total_entries(struct vrouter *);
int vr_flow_update_ecmp_index(struct vrouter *, struct vr_flow_entry *,
        unsigned int, struct vr_forwarding_md *);
uint32_t vr_flow_get_rflow_src_info(struct vrouter *, struct
        vr_flow_entry *);
unsigned int vr_flow_table_burst_step_configured(struct vrouter *);
unsigned int vr_flow_table_burst_tokens_configured(struct vrouter *);
unsigned int vr_flow_table_burst_time_configured(struct vrouter *);

void vr_compute_size_oflow_table(void);

int vr_flow_incr_stats(int , uint32_t , uint16_t, uint32_t, uint8_t);
void
update_flow_entry(vr_htable_t table __attribute__unused__, vr_hentry_t *ent ,
        unsigned int index, void *data __attribute__unused__);

#endif /* __VR_FLOW_H__ */

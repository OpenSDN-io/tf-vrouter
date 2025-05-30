/*
 * vr_proto_ip6.c -- ip6 handler
 *
 * Copyright (c) 2014, Juniper Networks, Inc.
 * All rights reserved
 */
#include <vr_os.h>
#include <vr_types.h>
#include <vr_packet.h>

#include <vr_interface.h>
#include <vr_datapath.h>
#include <vr_ip_mtrie.h>
#include <vr_bridge.h>
#include <vr_fragment.h>

struct vr_nexthop *
vr_inet6_ip_lookup(unsigned short vrf, uint8_t *ip6);
l4_pkt_type_t
vr_ip6_well_known_packet(struct vr_packet *pkt);


static bool
vr_ip6_fragment_tail(struct vr_ip6 *ip6)
{
    struct vr_ip6_frag *v6_frag;
    unsigned short frag, offset;
    bool more;

    if (ip6->ip6_nxt != VR_IP6_PROTO_FRAG)
        return false;

    v6_frag = (struct vr_ip6_frag *)(ip6 + 1);
    frag = ntohs(v6_frag->ip6_frag_offset);
    more = (frag & VR_IP6_MF) ? true : false;
    offset = frag > VR_IP6_FRAG_OFFSET_BITS;

    if (!more && offset)
        return true;

    return false;
}

struct vr_nexthop *
vr_inet6_ip_lookup(unsigned short vrf, uint8_t *ip6)
{
    uint32_t rt_prefix[4];

    struct vr_route_req rt;

    rt.rtr_req.rtr_vrf_id = vrf;
    rt.rtr_req.rtr_prefix = (uint8_t*)&rt_prefix;
    memcpy(rt.rtr_req.rtr_prefix, ip6, 16);
    rt.rtr_req.rtr_prefix_size = 16;
    rt.rtr_req.rtr_prefix_len = IP6_PREFIX_LEN;
    rt.rtr_req.rtr_family = AF_INET6;
    rt.rtr_req.rtr_marker_size = 0;
    rt.rtr_req.rtr_nh_id = 0;

    return vr_inet_route_lookup(vrf, &rt);
}


/*
 * The Icmp6 packet is considered DAD, if the source IP address in the
 * IPV6 header is NULL, and if no source link layer option is specified
 * after the Target address
 */
static bool
vr_icmp6_pkt_dad(struct vr_packet *pkt)
{
    struct vr_ip6 *ip6;

    ip6 = (struct vr_ip6 *)pkt_network_header(pkt);
    if ((!ip6) || (ip6->ip6_version != 6))
        return false;

    if (!vr_v6_prefix_null(ip6->ip6_src))
        return false;

    /* VR_IP6_ADDRESS_LEN is for Target address */
    if (pkt_len(pkt) > (sizeof(struct vr_icmp) + VR_IP6_ADDRESS_LEN))
        return false;

    return true;
}

/* TODO: consolidate all sum calculation routines */
static inline uint16_t
vr_sum(unsigned char *buf, unsigned int length)
{
   int num_words;
   uint32_t total;
   typedef uint16_t __attribute__((__may_alias__)) u16_p;
   u16_p *ptr;

   total = 0;
   ptr = (u16_p *)buf;
   num_words = (length + 1) / 2;

   while (num_words--)
       total += *ptr++;

   while (total & 0xffff0000)
       total = (total >> 16) + (total & 0xffff);

   return (uint16_t)total;
}

static inline uint16_t
vr_ip6_pseudo_header_sum(struct vr_ip6 *ip6)
{
   struct vr_ip6_pseudo ip6_ph;

   memcpy(ip6_ph.ip6_src, ip6->ip6_src, VR_IP6_ADDRESS_LEN);
   memcpy(ip6_ph.ip6_dst, ip6->ip6_dst, VR_IP6_ADDRESS_LEN);
   /*
    * XXX: length should be the length of (l4 header + data). But here, we
    * use the ip6 payload length, assuming that there are no extension
    * headers. This asusmption has to be fixed when we extend ipv6 support.
    */
   ip6_ph.ip6_l4_length = ip6->ip6_plen;
   ip6_ph.ip6_zero = 0;
   ip6_ph.ip6_zero_nh = (ip6->ip6_nxt << 24);

   return vr_sum((unsigned char *)&ip6_ph, sizeof(ip6_ph));
}

uint16_t
vr_icmp6_checksum(struct vr_ip6 *ip6, struct vr_icmp *icmph)
{
    uint16_t sum[2];

    sum[0] = vr_ip6_pseudo_header_sum(ip6);

    icmph->icmp_csum = 0;
    sum[1] = vr_sum((unsigned char *)icmph, ntohs(ip6->ip6_plen));

    return vr_sum((unsigned char *)sum, sizeof(sum));
}

bool
vr_ipv6_nd_input(struct vr_packet *pkt, struct vr_forwarding_md *fmd)
{
    bool handled = true;
    struct vr_icmp *icmp6;
    struct vr_ip6 *ip6;
    unsigned char *data, eth_dmac[VR_ETHER_ALEN];

    ip6 = (struct vr_ip6 *)pkt_data(pkt);
    icmp6 = (struct vr_icmp *) ((unsigned char *)ip6 + sizeof(struct vr_ip6));
    data = pkt_data(pkt);

    switch(icmp6->icmp_type) {
        case VR_ICMP6_TYPE_NEIGH_SOL:
            VR_MAC_COPY(eth_dmac, data);
            handled = vr_neighbor_input(pkt, fmd, eth_dmac);
            break;

        case VR_ICMP6_TYPE_NEIGH_AD:
            handled = vr_neighbor_reply(icmp6, pkt, fmd);
            break;

        case VR_ICMP6_TYPE_ROUTER_SOL:
        case VR_ICMP6_TYPE_ROUTER_AD:
        case VR_ICMP6_TYPE_REDIRECT:
        default:
            handled = false;
            break;
    }

    return handled;
}

static bool
vr_icmp6_input(struct vrouter *router, struct vr_packet *pkt,
               struct vr_forwarding_md *fmd)
{
    bool handled = true;
    struct vr_icmp *icmph;

    icmph = (struct vr_icmp *)pkt_data(pkt);
    switch (icmph->icmp_type) {
    case VR_ICMP6_TYPE_ROUTER_SOL:
        vr_trap(pkt, fmd->fmd_dvrf, AGENT_TRAP_L3_PROTOCOLS, NULL);
        break;

    default:
        handled = false;
        break;
    }

    return handled;
}

void
vr_inet6_fill_flow_from_req(struct vr_flow *flow_p, vr_flow_req *req)
{
    uint64_t *dst;

    vr_fill_flow_common(flow_p, req->fr_flow_nh_id, req->fr_flow_proto,
            req->fr_flow_sport, req->fr_flow_dport, AF_INET6, VR_FLOW_KEY_ALL);

    dst = (uint64_t *)(flow_p->flow6_sip);
    *dst = req->fr_flow_sip_u;
    *(dst + 1) = req->fr_flow_sip_l;
    *(dst + 2) = req->fr_flow_dip_u;
    *(dst + 3) = req->fr_flow_dip_l;

    return;
}

void
vr_inet6_fill_rflow_from_req(struct vr_flow *flow_p, vr_flow_req *req)
{
    uint64_t *dst;

    vr_fill_flow_common(flow_p, req->fr_rflow_nh_id, req->fr_flow_proto,
            req->fr_rflow_sport, req->fr_rflow_dport, AF_INET6, VR_FLOW_KEY_ALL);

    dst = (uint64_t *)(flow_p->flow6_sip);
    *dst = req->fr_rflow_sip_u;
    *(dst + 1) = req->fr_rflow_sip_l;
    *(dst + 2) = req->fr_rflow_dip_u;
    *(dst + 3) = req->fr_rflow_dip_l;

    return;
}

void
vr_inet6_fill_flow(struct vr_flow *flow_p, unsigned int nh_id,
        unsigned char *sip, unsigned char *dip,
        uint8_t proto, uint16_t sport, uint16_t dport,
        uint8_t valid_fkey_params)
{
    memset(flow_p, 0, VR_FLOW_IPV6_HASH_SIZE);
    valid_fkey_params &= VR_FLOW_KEY_ALL;

    vr_fill_flow_common(flow_p, nh_id, proto, sport, dport,
                        AF_INET6, valid_fkey_params);

    if (valid_fkey_params & VR_FLOW_KEY_SRC_IP)
        memcpy(flow_p->flow_ip, sip, VR_IP6_ADDRESS_LEN);

    if (valid_fkey_params & VR_FLOW_KEY_DST_IP)
        memcpy(flow_p->flow_ip + VR_IP6_ADDRESS_LEN, dip, VR_IP6_ADDRESS_LEN);

    return;
}

bool
vr_inet6_flow_is_fat_flow(struct vrouter *router, struct vr_packet *pkt,
        struct vr_flow_entry *fe)
{
    if (!fe->fe_key.flow6_sport || !fe->fe_key.flow6_dport) {
        if ((fe->fe_key.flow6_proto == VR_IP_PROTO_TCP) ||
                (fe->fe_key.flow6_proto == VR_IP_PROTO_UDP) ||
                (fe->fe_key.flow6_proto == VR_IP_PROTO_SCTP)) {
            return true;
        }
    }

    return false;
}


static int
vr_inet6_proto_flow(struct vrouter *router, unsigned short vrf,
        struct vr_packet *pkt, uint16_t vlan, struct vr_ip6 *ip6,
        struct vr_flow *flow_p, uint8_t valid_fkey_params,
        unsigned short custom, bool forward)
{
    int i, ret = 0;
    uint8_t ip6_nxt;
    unsigned char *ip6_src, *ip6_dst, *ip6_addr;
    unsigned char ip6_src_flow[16], ip6_dst_flow[16];
    unsigned short *t_hdr, sport, dport, port,
                   fat_flow_mask = VR_FAT_FLOW_NO_MASK;
    unsigned int nh_id;
    struct vr_ip6_frag *v6_frag;
    struct vr_icmp *icmph;

    t_hdr = (unsigned short *)((char *)ip6 + sizeof(struct vr_ip6));
    ip6_nxt = ip6->ip6_nxt;

    if (ip6_nxt == VR_IP6_PROTO_FRAG) {
        v6_frag = (struct vr_ip6_frag *)(ip6 + 1);
        ip6_nxt = v6_frag->ip6_frag_nxt;
        t_hdr = (unsigned short *)(v6_frag + 1);
    }

    if (ip6_nxt == VR_IP_PROTO_ICMP6) {
        icmph = (struct vr_icmp *)t_hdr;
        if (vr_icmp6_error(icmph)) {
            if ((unsigned char *)ip6 == pkt_network_header(pkt)) {
                ret = vr_inet6_form_flow(router, vrf, pkt, vlan,
                        (struct vr_ip6 *)(icmph + 1), flow_p,
                        valid_fkey_params, custom, false);
                return ret;
            } else {
                return -VP_DROP_ICMP_ERROR;
            }

            return 0;
        } else if ((icmph->icmp_type == VR_ICMP6_TYPE_ECHO_REQ) ||
            (icmph->icmp_type == VR_ICMP6_TYPE_ECHO_REPLY)) {
            sport = icmph->icmp_eid;
            dport = ntohs(VR_ICMP6_TYPE_ECHO_REPLY);
        } else if ((icmph->icmp_type == VR_ICMP6_TYPE_NEIGH_SOL) ||
                (icmph->icmp_type == VR_ICMP6_TYPE_NEIGH_AD)) {
            pkt->vp_flags |= VP_FLAG_FLOW_SET;
            return 0;
        } else {
            sport = 0;
            dport = icmph->icmp_type;
        }
    } else if ((ip6_nxt == VR_IP_PROTO_TCP) ||
            (ip6_nxt == VR_IP_PROTO_UDP) ||
            (ip6_nxt == VR_IP_PROTO_SCTP)) {
        sport = *t_hdr;
        dport = *(t_hdr + 1);
    } else {
        sport = 0;
        dport = 0;
    }

    ip6_src = (unsigned char *)&ip6->ip6_src;
    ip6_dst = (unsigned char *)&ip6->ip6_dst;
    if (!forward) {
        port = sport;
        sport = dport;
        dport = port;

        ip6_addr = ip6_src;
        ip6_src = ip6_dst;
        ip6_dst = ip6_addr;
    }
    /*
     * memcpy src and dst ip and create flow with it
     * as we don't want to change the actual pkt hdr
     * (esp with prefix based fat flow)
     */
    memcpy(ip6_src_flow, ip6_src, 16);
    memcpy(ip6_dst_flow, ip6_dst, 16);

    fat_flow_mask = vr_flow_fat_flow_lookup(router, pkt, ip6_nxt,
            sport, dport, NULL, NULL, ip6_src_flow, ip6_dst_flow);
    for (i = 1; i < VR_FAT_FLOW_MAX_MASK; i = i << 1) {
        switch (fat_flow_mask & i) {
        case VR_FAT_FLOW_SRC_PORT_MASK:
            valid_fkey_params &= ~VR_FLOW_KEY_SRC_PORT;
            break;

        case VR_FAT_FLOW_DST_PORT_MASK:
            valid_fkey_params &= ~VR_FLOW_KEY_DST_PORT;
            break;

        case VR_FAT_FLOW_SRC_IP_MASK:
            valid_fkey_params &= ~VR_FLOW_KEY_SRC_IP;
            break;

        case VR_FAT_FLOW_DST_IP_MASK:
            valid_fkey_params &= ~VR_FLOW_KEY_DST_IP;
            break;

        default:
            break;
        }
    }

    valid_fkey_params &= VR_FLOW_KEY_ALL;

    nh_id = vr_inet_flow_nexthop(pkt, vlan);
    vr_inet6_fill_flow(flow_p, nh_id, ip6_src_flow, ip6_dst_flow,
            ip6_nxt, sport, dport, valid_fkey_params);

    return 0;
}

static int
vr_inet6_fragment_flow(struct vrouter *router, unsigned short vrf,
        struct vr_packet *pkt, uint16_t vlan, struct vr_ip6 *ip6,
        struct vr_flow *flow_p, uint8_t valid_fkey_params,
        unsigned short custom, bool forward)
{
    uint16_t sport, dport, port;
    unsigned char *ip6_src, *ip6_dst, *ip6_addr;
    unsigned int nh_id;
    struct vr_fragment *frag;
    struct vr_ip6_frag *v6_frag;

    frag = vr_fragment_get(router, vrf,
            (struct vr_ip *)(pkt_network_header(pkt)), custom);
    if (!frag) {
        return -VP_DROP_NO_FRAG_ENTRY;
    }

    sport = frag->f_sport;
    dport = frag->f_dport;
    v6_frag = (struct vr_ip6_frag *)(ip6 + 1);

    frag->f_received += (ntohs(ip6->ip6_plen) - sizeof(struct vr_ip6_frag));
    if (vr_ip6_fragment_tail(ip6)) {
        frag->f_expected = (((ntohs(v6_frag->ip6_frag_offset)) >>
                          VR_IP6_FRAG_OFFSET_BITS) * 8) +
                          ntohs(ip6->ip6_plen) - sizeof(struct vr_ip6_frag);
    }
    if (frag->f_received == frag->f_expected) {
        vr_fragment_del(router->vr_fragment_table, frag);
    }

    ip6_src = (unsigned char *)&ip6->ip6_src;
    ip6_dst = (unsigned char *)&ip6->ip6_dst;
    if (!forward) {
        port = sport;
        sport = dport;
        dport = port;

        ip6_addr = ip6_src;
        ip6_src = ip6_dst;
        ip6_dst = ip6_addr;
    }

    nh_id = vr_inet_flow_nexthop(pkt, vlan);
    vr_inet6_fill_flow(flow_p, nh_id, ip6_src, ip6_dst,
            v6_frag->ip6_frag_nxt, sport, dport, valid_fkey_params);

    return 0;
}


int
vr_inet6_form_flow(struct vrouter *router, unsigned short vrf,
        struct vr_packet *pkt, uint16_t vlan, struct vr_ip6 *ip6,
        struct vr_flow *flow_p, uint8_t valid_fkey_params,
        unsigned short custom, bool forward)
{
    int ret = 0;

    if (vr_ip6_transport_header_valid(ip6)) {
        ret = vr_inet6_proto_flow(router, vrf, pkt, vlan, ip6, flow_p,
                valid_fkey_params, custom, forward);
    } else {
        ret = vr_inet6_fragment_flow(router, vrf, pkt, vlan, ip6,
                flow_p, valid_fkey_params, custom, forward);
    }

    return ret;

}

int
vr_inet6_get_flow_key(struct vrouter *router, unsigned short vrf,
        struct vr_packet *pkt, uint16_t vlan, struct vr_flow *flow_p,
        uint8_t valid_fkey_params, unsigned short custom)
{
    int ret;
    struct vr_ip6 *ip6;

    ip6 = (struct vr_ip6 *)pkt_network_header(pkt);
    ret = vr_inet6_form_flow(router, vrf, pkt, vlan, ip6, flow_p,
            valid_fkey_params, custom, true);
    if (ret < 0)
        return ret;

    if (vr_ip6_fragment_head(ip6)) {
        ret = vr_v6_fragment_add(router, vrf, ip6, flow_p->flow6_sport,
                flow_p->flow6_dport, custom);
        if (ret < 0)
            return -VP_DROP_NO_MEMORY;
    }

    return ret;
}

flow_result_t
vr_inet6_flow_lookup(struct vrouter *router, struct vr_packet *pkt,
                    struct vr_forwarding_md *fmd)
{
    int ret;
    bool lookup = false;
    struct vr_flow flow, *flow_p = &flow;
    struct vr_ip6 *ip6 = (struct vr_ip6 *)pkt_network_header(pkt);
    struct vr_packet *pkt_c;

    /*
     * if the packet has already done one round of flow lookup, there
     * is no point in doing it again, eh?
     */
    if (pkt->vp_flags & VP_FLAG_FLOW_SET)
        return FLOW_FORWARD;

    /*
     * if the interface is policy enabled, or if somebody else (eg:nexthop)
     * has requested for a policy lookup, packet has to go through a lookup
     */
    if ((pkt->vp_if->vif_flags & VIF_FLAG_POLICY_ENABLED) ||
            (pkt->vp_flags & VP_FLAG_FLOW_GET)) {
        lookup = true;
    }

    if (!lookup)
        return FLOW_FORWARD;

    if(fmd->fmd_fe)
        flow_p = &fmd->fmd_fe->fe_key;
    else {
        ret = vr_inet6_form_flow(router, fmd->fmd_dvrf, pkt, fmd->fmd_vlan,
                                 ip6, flow_p, VR_FLOW_KEY_ALL, 0, true);
        if (ret < 0) {
            if (!vr_ip6_transport_header_valid(ip6) && vr_enqueue_to_assembler) {
                vr_enqueue_to_assembler(router, pkt, fmd);
            } else {
                PKT_LOG(VP_DROP_MISC, pkt, flow_p, VR_PROTO_IP6_C, __LINE__);
                vr_pfree(pkt, VP_DROP_MISC);
            }
            return FLOW_CONSUMED;
        }
    }

    if (pkt->vp_flags & VP_FLAG_FLOW_SET)
        return FLOW_FORWARD;

    if (vr_ip6_fragment_head(ip6)) {
        ret = vr_v6_fragment_add(router, fmd->fmd_dvrf, ip6, flow_p->flow6_sport,
                flow_p->flow6_dport, 0);
        if (ret < 0)
            return -VP_DROP_NO_MEMORY;
        if (vr_enqueue_to_assembler){
            pkt_c = vr_pclone(pkt);
            if (pkt_c) {
                vr_enqueue_to_assembler(router, pkt_c, fmd);
            }
        }
    }

    return vr_flow_lookup(router, flow_p, pkt, fmd);
}


int
vr_ip6_input(struct vrouter *router, struct vr_packet *pkt,
             struct vr_forwarding_md *fmd)
{
    struct vr_ip6 *ip6;
    unsigned short *t_hdr, sport, dport;

    ip6 = (struct vr_ip6 *)pkt_network_header(pkt);
    if (fmd->fmd_dscp < 0)
        fmd->fmd_dscp = vr_inet6_get_tos(ip6);

    t_hdr = (unsigned short *)((char *)ip6 + sizeof(struct vr_ip6));

    if (!pkt_pull(pkt, sizeof(struct vr_ip6))) {
        PKT_LOG(VP_DROP_PULL, pkt, 0, VR_PROTO_IP6_C, __LINE__);
        vr_pfree(pkt, VP_DROP_PULL);
        return 0;
    }

    switch (ip6->ip6_nxt) {
    case VR_IP_PROTO_ICMP6:
        if (vr_icmp6_input(router, pkt, fmd))
            return 0;
        break;

    case VR_IP_PROTO_UDP:
        sport = *t_hdr;
        dport = *(t_hdr + 1);
        if (vif_is_virtual(pkt->vp_if)) {
            if ((sport == VR_DHCP6_SPORT) && (dport == VR_DHCP6_DPORT))
                return vr_trap(pkt, fmd->fmd_dvrf, AGENT_TRAP_L3_PROTOCOLS, NULL);
        }
        break;

    default:
        break;
    }

    if (!pkt_push(pkt, sizeof(struct vr_ip6))) {
        PKT_LOG(VP_DROP_PUSH, pkt, 0, VR_PROTO_IP6_C, __LINE__);
        vr_pfree(pkt, VP_DROP_PUSH);
        return 0;
    }

    return vr_forward(router, pkt, fmd);
}

static void
vr_neighbor_proxy(struct vr_packet *pkt, struct vr_forwarding_md *fmd,
        unsigned char *dmac)
{
    uint16_t adv_flags = 0;

    struct vr_eth *eth;
    struct vr_ip6 *ip6;
    struct vr_icmp *icmph;
    struct vr_neighbor_option *nopt;


    icmph = (struct vr_icmp *)pkt_data(pkt);
    nopt = (struct vr_neighbor_option *)((unsigned char *)icmph +
            sizeof(*icmph) + VR_IP6_ADDRESS_LEN);

    eth = (struct vr_eth *)pkt_push(pkt, sizeof(*ip6) + sizeof(*eth));
    if (!eth) {
        vr_pfree(pkt, VP_DROP_PUSH);
        return;
    }
    ip6 = (struct vr_ip6 *)((unsigned char *)eth + sizeof(*eth));


    /* Update Ethernet headr */
    VR_MAC_COPY(eth->eth_dmac, nopt->vno_value);
    VR_MAC_COPY(eth->eth_smac, dmac);
    eth->eth_proto = htons(VR_ETH_PROTO_IP6);

    memcpy(ip6->ip6_dst, ip6->ip6_src, sizeof(ip6->ip6_src));
    memcpy(ip6->ip6_src, &icmph->icmp_data, sizeof(ip6->ip6_src));

    /* Update ICMP header and options */
    icmph->icmp_type = VR_ICMP6_TYPE_NEIGH_AD;
    adv_flags |= VR_ICMP6_NEIGH_AD_FLAG_SOLCITED;
    if (fmd->fmd_flags & FMD_FLAG_MAC_IS_MY_MAC)
        adv_flags |= VR_ICMP6_NEIGH_AD_FLAG_ROUTER;

    icmph->icmp_eid = htons(adv_flags);

    /* length in units of 8 octets */
    nopt->vno_type = TARGET_LINK_LAYER_ADDRESS_OPTION;
    nopt->vno_length = (sizeof(struct vr_neighbor_option) + VR_ETHER_ALEN) / 8;
    VR_MAC_COPY(nopt->vno_value, dmac);

    icmph->icmp_csum =
        ~(vr_icmp6_checksum(ip6, icmph));

    vr_mac_reply_send(pkt, fmd);

    return;
}

mac_response_t
vm_neighbor_request(struct vr_interface *vif, struct vr_packet *pkt,
        struct vr_forwarding_md *fmd, unsigned char *dmac)
{
    uint32_t rt6_prefix[4];
    unsigned char mac[VR_ETHER_ALEN];
    struct vr_icmp *icmph;
    struct vr_route_req rt;

    if (fmd->fmd_vlan != VLAN_ID_INVALID)
        return MR_FLOOD;

    /* We let DAD packets bridged */
    if (vr_icmp6_pkt_dad(pkt))
        return MR_NOT_ME;


    memset(&rt, 0, sizeof(rt));
    rt.rtr_req.rtr_vrf_id = fmd->fmd_dvrf;
    rt.rtr_req.rtr_family = AF_INET6;
    rt.rtr_req.rtr_prefix = (uint8_t *)&rt6_prefix;
    rt.rtr_req.rtr_prefix_size = 16;
    rt.rtr_req.rtr_prefix_len = IP6_PREFIX_LEN;
    rt.rtr_req.rtr_mac = mac;

    icmph = (struct vr_icmp *)pkt_data(pkt);
    memcpy(rt.rtr_req.rtr_prefix, icmph->icmp_data, 16);

    vr_inet_route_lookup(fmd->fmd_dvrf, &rt);

    if ((vif->vif_flags & VIF_FLAG_MAC_PROXY) ||
            (rt.rtr_req.rtr_label_flags & VR_RT_ARP_PROXY_FLAG)) {
        return vr_get_proxy_mac(pkt, fmd, &rt, dmac);
    }

    return MR_FLOOD;
}

int
vr_neighbor_reply(struct vr_icmp *icmp6, struct vr_packet *pkt,
                  struct vr_forwarding_md *fmd)
{
    struct vr_interface *vif = pkt->vp_if;
    struct vr_packet *cloned_pkt;
    int handled = 1;

    if (vif_mode_xconnect(vif) || vif->vif_type == VIF_TYPE_HOST) {
        vif_xconnect(vif, pkt, fmd);
        return handled;
    }

    if (vif_is_fabric(vif)) {
        if (fmd->fmd_label >= 0)
            return !handled;

        if (fmd->fmd_dvrf != vif->vif_vrf)
            return !handled;

        cloned_pkt = pkt_cow(pkt, AGENT_PKT_HEAD_SPACE);
        if (cloned_pkt) {
            vr_preset(cloned_pkt);
            vif_xconnect(vif, pkt, fmd);
            vr_trap(cloned_pkt, fmd->fmd_dvrf, AGENT_TRAP_ARP, NULL);
        } else {
           vr_trap(pkt, fmd->fmd_dvrf, AGENT_TRAP_ARP, NULL);
        }

        return handled;
    }

    PKT_LOG(VP_DROP_INVALID_IF, pkt, 0, VR_PROTO_IP6_C, __LINE__);
    vr_pfree(pkt, VP_DROP_INVALID_IF);

    return handled;
}

int
vr_neighbor_input(struct vr_packet *pkt, struct vr_forwarding_md *fmd,
        unsigned char *eth_dmac)
{
    int handled = 1;
    uint32_t pull_len;
    unsigned char dmac[VR_ETHER_ALEN];
    mac_response_t ndisc_result;
    struct vr_ip6 *ip6;
    struct vr_icmp *icmph;
    struct vr_packet *pkt_c;
    struct vr_interface *vif = pkt->vp_if;

    pull_len = sizeof(*ip6);
    if (pkt->vp_len < pull_len) {
        vr_pfree(pkt, VP_DROP_INVALID_PACKET);
        return handled;
    }

    ip6 = (struct vr_ip6 *)pkt_data(pkt);
    if (ip6->ip6_nxt != VR_IP_PROTO_ICMP6)
        return !handled;

    if (pkt->vp_len < pull_len + sizeof(struct vr_icmp)) {
        PKT_LOG(VP_DROP_INVALID_PACKET, pkt, 0, VR_PROTO_IP6_C, __LINE__);
        vr_pfree(pkt, VP_DROP_INVALID_PACKET);
        return handled;
    }

    pkt_pull(pkt, pull_len);
    icmph = (struct vr_icmp *)pkt_data(pkt);
    if (icmph->icmp_type != VR_ICMP6_TYPE_NEIGH_SOL) {
        pkt_push(pkt, pull_len);
        return !handled;
    }

    VR_MAC_COPY(dmac, eth_dmac);

    ndisc_result = vif->vif_mac_request(vif, pkt, fmd, dmac);
    switch (ndisc_result) {
    case MR_PROXY:
        vr_neighbor_proxy(pkt, fmd, dmac);
        break;

    case MR_XCONNECT:
        vif_xconnect(pkt->vp_if, pkt, fmd);
        break;

    case MR_TRAP_X:
        pkt_c = vr_pclone(pkt);
        if (pkt_c) {
            vr_trap(pkt_c, fmd->fmd_dvrf, AGENT_TRAP_ARP, NULL);
            vif_xconnect(pkt->vp_if, pkt, fmd);
        } else {
            vr_trap(pkt, fmd->fmd_dvrf, AGENT_TRAP_ARP, NULL);
        }
        break;

    case MR_MIRROR:
        pkt_c = vr_pclone(pkt);
        if (pkt_c)
            vr_trap(pkt_c, fmd->fmd_dvrf, AGENT_TRAP_ARP, NULL);

        handled = false;
        break;

    case MR_DROP:
        PKT_LOG(VP_DROP_INVALID_ARP, pkt, 0, VR_PROTO_IP6_C, __LINE__);
        vr_pfree(pkt, VP_DROP_INVALID_ARP);
        break;

    case MR_FLOOD:
    default:
        handled = false;
        break;
    }

    if (!handled)
        pkt_push(pkt, pull_len);

    return handled;
}

l4_pkt_type_t
vr_ip6_well_known_packet(struct vr_packet *pkt)
{
    unsigned char *data = pkt_data(pkt);
    struct vr_ip6 *ip6;
    struct vr_udp *udph = NULL;
    struct vr_icmp *icmph = NULL;

    if ((pkt->vp_type != VP_TYPE_IP6) ||
         (!(pkt->vp_flags & VP_FLAG_MULTICAST)))
        return L4_TYPE_UNKNOWN;

    ip6 = (struct vr_ip6 *)data;

    if (vr_v6_prefix_is_ll(ip6->ip6_dst))
        return L4_TYPE_UNKNOWN;

    /* 0xFF02 is the multicast address used for NDP, DHCPv6 etc */
    if (ip6->ip6_dst[0] == 0xFF && ip6->ip6_dst[1] == 0x02) {
        /*
         * Bridge neighbor solicit for link-local addresses
         */
        if (ip6->ip6_nxt == VR_IP_PROTO_ICMP6) {
            icmph = (struct vr_icmp *)((char *)ip6 + sizeof(struct vr_ip6));
            if (icmph && (icmph->icmp_type == VR_ICMP6_TYPE_ROUTER_SOL))
                return L4_TYPE_ROUTER_SOLICITATION;
            if (icmph && (icmph->icmp_type == VR_ICMP6_TYPE_NEIGH_SOL))
                return L4_TYPE_NEIGHBOUR_SOLICITATION;
            if (icmph && (icmph->icmp_type == VR_ICMP6_TYPE_NEIGH_AD))
                return L4_TYPE_NEIGHBOUR_ADVERTISEMENT;
        }

        if (ip6->ip6_nxt == VR_IP_PROTO_UDP) {
            udph = (struct vr_udp *)((char *)ip6 + sizeof(struct vr_ip6));
            if (udph && (udph->udp_sport == htons(VR_DHCP6_SRC_PORT)))
                return L4_TYPE_DHCP_REQUEST;
        }
    }

    return L4_TYPE_UNKNOWN;
}

static void
vr_ip6_update_csum(struct vr_packet *pkt, uint32_t ip_inc, uint32_t port_inc)
{
    struct vr_ip6 *ip6 = NULL;
    struct vr_tcp *tcp = NULL;
    struct vr_udp *udp = NULL;
    uint32_t csum_inc  = 0;
    uint32_t csum = 0;
    uint16_t *csump = NULL;

    ip6 = (struct vr_ip6 *)pkt_network_header(pkt);

    if (ip6->ip6_nxt == VR_IP_PROTO_TCP) {
        tcp = (struct vr_tcp *)((uint8_t *)ip6 + sizeof(struct vr_ip6));
        csump = &tcp->tcp_csum;
    } else if (ip6->ip6_nxt == VR_IP_PROTO_UDP) {
        udp = (struct vr_udp *)((uint8_t *)ip6 + sizeof(struct vr_ip6));
        csump = &udp->udp_csum;
        if (*csump == 0) {
            return;
        }
    } else {
        return;
    }

    if (vr_ip6_transport_header_valid(ip6)) {
        /*
        * for partial checksums, the actual value is stored rather
        * than the complement
        */
        if (pkt->vp_flags & VP_FLAG_CSUM_PARTIAL) {
            csum = (*csump) & 0xffff;
        } else {
            csum = ~(*csump) & 0xffff;
        }

        // adj. ip contr. to csum change
        ip_inc = (ip_inc & 0xffff) + (ip_inc >> 16);
        if (ip_inc >> 16)
            ip_inc = (ip_inc & 0xffff) + 1;

        // adj. port contr. to csum change
        port_inc = (port_inc & 0xffff) + (port_inc >> 16);
        if (port_inc >> 16)
            port_inc = (port_inc & 0xffff) + 1;

        csum_inc = ip_inc + port_inc;

        csum += csum_inc;

        csum = (csum & 0xffff) + (csum >> 16);
        if (csum >> 16)
            csum = (csum & 0xffff) + 1;

        if (pkt->vp_flags & VP_FLAG_CSUM_PARTIAL) {
            *csump = csum & 0xffff;
        } else {
            *csump = ~(csum) & 0xffff;
        }
    }

    return;
}

flow_result_t
vr_inet6_flow_nat(struct vr_flow_entry *fe, struct vr_packet *pkt,
                struct vr_forwarding_md *fmd)
{
    uint32_t ip_inc = 0, port_inc = 0;
    uint16_t *t_sport = NULL, *t_dport = NULL;
    const uint8_t dw_p_ip6 = VR_IP6_ADDRESS_LEN / sizeof(uint32_t);
    uint8_t i = 0;

    struct vrouter *router = pkt->vp_if->vif_router;
    struct vr_flow_entry *rfe = NULL;
    struct vr_ip6 *ip6 = NULL, *icmp_pl_ip6 = NULL;
    struct vr_icmp *icmph = NULL;

    if (fe->fe_rflow < 0)
        goto drop;

    rfe = vr_flow_get_entry(router, fe->fe_rflow);
    if (!rfe)
        goto drop;

    ip6 = (struct vr_ip6 *)pkt_network_header(pkt);
    if (!ip6)
        goto drop;

    if (ip6->ip6_nxt == VR_IP_PROTO_ICMP6) {
        icmph = (struct vr_icmp *)((char *)ip6 + sizeof(struct vr_ip6));

        if (vr_icmp_error(icmph)) {
            icmp_pl_ip6 = (struct vr_ip6 *)(icmph + 1);
            if (fe->fe_flags & VR_FLOW_FLAG_SNAT) {
                memcpy(icmp_pl_ip6->ip6_dst, rfe->fe_key.flow6_dip,
                    VR_IP6_ADDRESS_LEN);
            }

            if (fe->fe_flags & VR_FLOW_FLAG_DNAT) {
                memcpy(icmp_pl_ip6->ip6_src, rfe->fe_key.flow6_sip,
                    VR_IP6_ADDRESS_LEN);
            }

            t_sport = (uint16_t *)((uint8_t *)icmp_pl_ip6 +
                sizeof(struct vr_ip6));
            t_dport = t_sport + 1;

            if (fe->fe_flags & VR_FLOW_FLAG_SPAT) {
                *t_dport = rfe->fe_key.flow6_dport;
            }
            if (fe->fe_flags & VR_FLOW_FLAG_DPAT) {
                *t_sport = rfe->fe_key.flow6_sport;
            }
        }
    }

    if ((fe->fe_flags & VR_FLOW_FLAG_SNAT) &&
            (memcmp(ip6->ip6_src, fe->fe_key.flow6_sip, VR_IP6_ADDRESS_LEN) == 0)) {
        for (i = 0; i < VR_IP6_ADDRESS_LEN; i += dw_p_ip6) {
            vr_incremental_diff( *((uint32_t*)(ip6->ip6_src + i)),
                *((uint32_t*)(rfe->fe_key.flow6_dip + i)), &ip_inc);
        }
        memcpy(ip6->ip6_src, rfe->fe_key.flow6_dip,
            VR_IP6_ADDRESS_LEN);
    }

    if (fe->fe_flags & VR_FLOW_FLAG_DNAT) {
        for (i = 0; i < VR_IP6_ADDRESS_LEN; i += dw_p_ip6) {
            vr_incremental_diff( *((uint32_t*)(ip6->ip6_dst + i)),
                *((uint32_t*)(rfe->fe_key.flow6_sip + i)), &ip_inc);
        }
        memcpy(ip6->ip6_dst, rfe->fe_key.flow6_sip,
            VR_IP6_ADDRESS_LEN);
    }

    if (vr_ip6_transport_header_valid(ip6)) {
        t_sport = (uint16_t *)((uint8_t *)ip6 +
                sizeof(struct vr_ip6));
        t_dport = t_sport + 1;

        if (fe->fe_flags & VR_FLOW_FLAG_SPAT) {
            vr_incremental_diff(*t_sport,
                rfe->fe_key.flow6_dport, &port_inc);
            *t_sport = rfe->fe_key.flow6_dport;
        }

        if (fe->fe_flags & VR_FLOW_FLAG_DPAT) {
            vr_incremental_diff(*t_dport,
                rfe->fe_key.flow6_sport, &port_inc);
            *t_dport = rfe->fe_key.flow6_sport;
        }
    }

    if (!vr_pkt_is_diag(pkt))
        vr_ip6_update_csum(pkt, ip_inc, port_inc);

    if ((ip6->ip6_nxt == VR_IP_PROTO_ICMP6) &&
        ((fe->fe_flags & VR_FLOW_FLAG_DNAT)||
        (fe->fe_flags & VR_FLOW_FLAG_SNAT))){
        icmph->icmp_csum = ~(vr_icmp6_checksum(ip6, icmph));
    }

    if ((fe->fe_flags & VR_FLOW_FLAG_VRFT) && pkt->vp_nh &&
            ((pkt->vp_nh->nh_vrf != fmd->fmd_dvrf) ||
            (pkt->vp_nh->nh_flags & NH_FLAG_ROUTE_LOOKUP))) {
        /* only if pkt->vp_nh was set before... */
        pkt->vp_nh = vr_inet6_ip_lookup(fmd->fmd_dvrf, ip6->ip6_dst);
    }

    return FLOW_FORWARD;

drop:
    PKT_LOG(VP_DROP_FLOW_NAT_NO_RFLOW, pkt, 0, VR_PROTO_IP6_C, __LINE__);
    vr_pfree(pkt, VP_DROP_FLOW_NAT_NO_RFLOW);
    return FLOW_CONSUMED;
}

/*
 * flow.c -- flow handling utility
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <inttypes.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <asm/types.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_ether.h>
#endif

#include <net/if.h>
#if defined(__linux__)
#include <netinet/ether.h>
#endif

#include "vr_types.h"
#include "vr_qos.h"
#include "vr_flow.h"
#include "vr_mirror.h"
#include "vr_genetlink.h"
#include "nl_util.h"
#include "vr_os.h"
#include "ini_parser.h"
#include "vr_packet.h"
#include "vr_message.h"
#include "vr_mem.h"

#define TABLE_FLAG_VALID        0x1

#define MAX_FLOW_NL_MSG_BUNCH   15
#define MAX_FLOWS               4000000

static int mem_fd;

static int dvrf_set, mir_set, show_evicted_set, sock_dir_set;
static int help_set, match_set, get_set, force_evict_set;
static unsigned short dvrf;
static int list, flow_cmd, mirror = -1;
static unsigned long flow_index;
static int rate, stats, perf, flush, bunch = 1;
static bool more = false;

#define FLOW_GET_FIELD_LENGTH   30
#define FLOW_COMPONENT_NH_COUNT 16

char src_vif_name[IFNAMSIZ];
char src_l3_vif_name[IFNAMSIZ], dst_l3_vif_name[IFNAMSIZ];
char src_l2_vif_name[IFNAMSIZ], dst_l2_vif_name[IFNAMSIZ];
vr_interface_req *src_vif, *src_l3_vif, *dst_l3_vif, *resp_vif;
vr_interface_req *src_l2_vif, *dst_l2_vif;

vr_nexthop_req *src_nh, *resp_nh;
vr_nexthop_req *src_l3_nh, *dst_l3_nh;
vr_nexthop_req *src_l2_nh, *dst_l2_nh;
vr_nexthop_req *component_nh[FLOW_COMPONENT_NH_COUNT];
vr_nexthop_req *mirror_nh, *mirror1_nh;

vr_route_req *resp_rt;
vr_route_req *src_l3_rt, *dst_l3_rt;
vr_route_req *src_l2_rt, *dst_l2_rt;

uint8_t req_prefix[VR_IP6_ADDRESS_LEN];
uint8_t req_mac[VR_ETHER_ALEN];

vr_mirror_req *resp_mirror;

vr_drop_stats_req *resp_ds, *global_ds;

unsigned char addr_string[INET6_ADDRSTRLEN];
int ecmp_index = -1;

/* match variables */
static unsigned int match_family, match_family_size;
static int32_t match_port1 = -1, match_port2 = -1;
static int32_t match_proto = -1, match_vrf = -1;
static char *match_ip1, *match_ip2;

/* to accommodate '*' */
bool match_ip1_set, match_ip2_set;

struct flow_table {
    struct vr_flow_entry *ft_entries;
    u_int64_t ft_entries_p;
    u_int64_t ft_span;
    u_int64_t ft_processed;
    u_int64_t ft_created;
    u_int64_t ft_added;
    u_int64_t ft_deleted;
    u_int64_t ft_changed;
    u_int64_t ft_total_entries;
    unsigned int ft_burst_free_tokens;
    unsigned int ft_hold_entries;
    unsigned int ft_num_entries;
    unsigned int ft_flags;
    unsigned int ft_cpus;
    unsigned int ft_hold_oflows;
    unsigned int ft_hold_stat_count;
    unsigned int ft_oflow_entries;
    u_int32_t ft_hold_stat[128];
    char flow_table_path[256];
} main_table;

struct flow_md {
    unsigned int fmd_index;
    unsigned int fmd_gen_id;
};


struct nl_client *cl;
vr_flow_req flow_req;
vr_flow_table_data ftable;
static struct flow_md flow_md_mem[MAX_FLOWS];
static int array_index;

static void flow_dump_nexthop(vr_nexthop_req *, vr_interface_req *,
        char *, bool);
static vr_nexthop_req *flow_get_nexthop(int);
static int flow_table_map(vr_flow_table_data *);
static int flow_table_get(void);

static void
response_process(void *sresp)
{
    vr_response *resp = (vr_response *)sresp;

    if (resp->resp_code < 0) {
        printf("%s: %s\n", __func__, strerror(-resp->resp_code));
        exit(-2);
    }

    return;
}

static void
flow_response_process(void *sresp)
{
    if (array_index == -1)
        return;

    vr_flow_response *resp = (vr_flow_response *)sresp;
    flow_md_mem[array_index].fmd_index = resp->fresp_index;
    flow_md_mem[array_index].fmd_gen_id = resp->fresp_gen_id;
    return;
}


static void
flow_table_data_process(void *sreq)
{
    vr_flow_table_data *ftable = (vr_flow_table_data *)sreq;

    flow_table_map(ftable);

    return;
}

static void
interface_req_process(void *arg)
{
    vr_interface_req *req = (vr_interface_req *)arg;

    resp_vif = vr_interface_req_get_copy(req);

    return;
}

static void
nexthop_req_process(void *arg)
{
    vr_nexthop_req *req = (vr_nexthop_req *)arg;

    resp_nh = vr_nexthop_req_get_copy(req);

    return;
}

static void
route_req_process(void *arg)
{
    vr_route_req *req = (vr_route_req *)arg;

    resp_rt = vr_route_req_get_copy(req);

    return;
}

static void
drop_stats_req_process(void *arg)
{
    vr_drop_stats_req *req = (vr_drop_stats_req *)arg;

    resp_ds = vr_drop_stats_req_get_copy(req);

    return;
}

static void
flow_fill_nl_callbacks()
{
    nl_cb.vr_response_process = response_process;
    nl_cb.vr_flow_response_process = flow_response_process;
    nl_cb.vr_flow_table_data_process = flow_table_data_process;
    nl_cb.vr_interface_req_process = interface_req_process;
    nl_cb.vr_nexthop_req_process = nexthop_req_process;
    nl_cb.vr_route_req_process = route_req_process;
    nl_cb.vr_drop_stats_req_process = drop_stats_req_process;
}

struct vr_flow_entry *
flow_get(unsigned long flow_index)
{
    if (flow_index >= main_table.ft_num_entries)
        return NULL;

    return &main_table.ft_entries[flow_index];
}

static vr_drop_stats_req *
flow_get_dropstats(void)
{
    int ret;

    ret = vr_send_drop_stats_get(cl, 0, 0);
    if (ret < 0)
        return NULL;

    ret = vr_recvmsg(cl, false);
    if (ret <= 0)
        return NULL;

    return resp_ds;
}

static vr_nexthop_req *
flow_get_nexthop(int id)
{
    int ret;

    resp_nh = NULL;

    ret = vr_send_nexthop_get(cl, 0, id);
    if (ret < 0)
        return NULL;

    ret = vr_recvmsg(cl, false);
    if (ret <= 0)
        return NULL;

    return resp_nh;
}

static vr_nexthop_req *
flow_get_mirror_nh(int id)
{
    int ret;

    ret = vr_send_mirror_get(cl, 0, id);
    if (ret < 0)
        return NULL;

    ret = vr_recvmsg(cl, false);
    if (ret <= 0)
        return NULL;

    return flow_get_nexthop(resp_mirror->mirr_nhid);
}

static vr_interface_req *
flow_get_vif(int vif_index)
{
    int ret;

    ret = vr_send_interface_get(cl, 0, vif_index, -1, 0, 0);
    if (ret < 0)
        return NULL;

    ret = vr_recvmsg(cl, false);
    if (ret <= 0)
        return NULL;

    return resp_vif;
}

static vr_route_req *
flow_get_route(unsigned int family, unsigned int vrf, uint8_t *prefix)
{
    int ret;
    unsigned int prefix_size;

    switch (family) {
    case AF_INET:
        prefix_size = VR_IP_ADDRESS_LEN;
        break;

    case AF_INET6:
        prefix_size = VR_IP6_ADDRESS_LEN;
        break;

    case AF_BRIDGE:
        prefix_size = VR_ETHER_ALEN;
        memcpy(req_mac, prefix, prefix_size);
        break;

    default:
        return NULL;
    }

    memcpy(req_prefix, prefix, prefix_size);
    if (family == AF_BRIDGE) {
        ret = vr_send_route_get(cl, 0, vrf, family, NULL, 0, req_mac);
    } else {
        ret = vr_send_route_get(cl, 0, vrf, family, req_prefix,
                prefix_size * 8, req_mac);
    }

    if (ret < 0)
        return NULL;

    ret = vr_recvmsg(cl, false);
    if (ret <= 0)
        return NULL;

    return resp_rt;
}

const char *
flow_get_drop_reason(uint8_t drop_code)
{
    switch (drop_code) {
    case VR_FLOW_DR_UNKNOWN:
        return "Unknown";
    case VR_FLOW_DR_UNAVIALABLE_INTF:
        return "IntfErr";
    case VR_FLOW_DR_IPv4_FWD_DIS:
        return "Ipv4Dis";
    case VR_FLOW_DR_UNAVAILABLE_VRF:
        return "VrfErr";
    case VR_FLOW_DR_NO_SRC_ROUTE:
        return "NoSrcRt";
    case VR_FLOW_DR_NO_DST_ROUTE:
        return "NoDstRt";
    case VR_FLOW_DR_AUDIT_ENTRY:
        return "Audit";
    case VR_FLOW_DR_VRF_CHANGE:
        return "VrfChange";
    case VR_FLOW_DR_NO_REVERSE_FLOW:
        return "NoRevFlow";
    case VR_FLOW_DR_REVERSE_FLOW_CHANGE:
        return "RevFlowChng";
    case VR_FLOW_DR_NAT_CHANGE:
        return "NatChng";
    case VR_FLOW_DR_FLOW_LIMIT:
        return "FlowLim";
    case VR_FLOW_DR_LINKLOCAL_SRC_NAT:
        return "LinkSrcNatErr";
    case VR_FLOW_DR_FAILED_VROUTER_INSTALL:
        return "VrouterInstallFail";
    case VR_FLOW_DR_INVALID_L2_FLOW:
        return "InvalidL2Flow";
    case VR_FLOW_DR_FLOW_ON_TSN:
        return "TSNFlow";
    case VR_FLOW_DR_PORT_MAP_DROP:
        return "NoFipPortMap";
    case VR_FLOW_DR_NO_SRC_ROUTE_L2RPF:
        return "NoSrcRtRpfNh";
    case VR_FLOW_DR_FAT_FLOW_NAT_CONFLICT:
        return "FatFlowNatConflict";
    case VR_FLOW_DR_POLICY:
        return "Policy";
    case VR_FLOW_DR_OUT_POLICY:
        return "OutPolicy";
    case VR_FLOW_DR_SG:
        return "SG";
    case VR_FLOW_DR_OUT_SG:
        return "OutSG";
    case VR_FLOW_DR_REVERSE_SG:
        return "RevSG";
    case VR_FLOW_DR_REVERSE_OUT_SG:
        return "RevOutSG";
    case VR_FLOW_DR_FW_POLICY:
        return "FwPolicy";
    case VR_FLOW_DR_OUT_FW_POLICY:
        return "OutFwPolicy";
    case VR_FLOW_DR_REVERSE_FW_POLICY:
        return "RevFwPolicy";
    case VR_FLOW_DR_REVERSE_OUT_FW_POLICY:
        return "RevOutFwPolicy";
    case VR_FLOW_DR_SAME_FLOW_RFLOW_KEY:
        return "SameFlowRflowKey";
    case VR_FLOW_DR_NO_MIRROR_ENTRY:
        return "NoMirrorentry";
    case VR_FLOW_DR_FWAAS_POLICY:
        return "FWAASPolicy";
    case VR_FLOW_DR_OUT_FWAAS_POLICY:
        return "OutFWAASPolicy";
    case VR_FLOW_DR_REVERSE_FWAAS_POLICY:
        return "RevFWAASPolicy";
    case VR_FLOW_DR_REVERSE_OUT_FWAAS_POLICY:
        return "RevOutFWAASPolicy";
    default:
        break;
    }
    return NULL;
}

static void
flow_dump_legend(void)
{
    printf("Action:F=Forward, D=Drop ");
    printf("N=NAT(S=SNAT, D=DNAT, Ps=SPAT, Pd=DPAT, ");
    printf("L=Link Local Port)\n");

    printf(" Other:K(nh)=Key_Nexthop, S(nh)=RPF_Nexthop\n");
    printf(" Flags:E=Evicted, Ec=Evict Candidate, N=New Flow, M=Modified Dm=Delete Marked\n");
    printf("TCP(r=reverse):S=SYN, F=FIN, R=RST, C=HalfClose, E=Established, D=Dead\n");
    printf("\n");

    return;
}

static bool
flow_match_dest(struct vr_flow_entry *fe, uint8_t *addr, int32_t port)
{
    if (!addr && (port < 0))
        return false;

    if (!memcmp(&fe->fe_key.flow_ip[VR_IP_ADDR_SIZE(fe->fe_type)],
                addr, match_family_size)) {
        if (port < 0)
            return true;
        if (ntohs(fe->fe_key.flow_dport) == port)
            return true;
    }

    return false;
}

static bool
flow_match_source(struct vr_flow_entry *fe, uint8_t *addr, int32_t port)
{
    if (!addr && (port < 0))
        return false;

    if (!addr || !memcmp(fe->fe_key.flow_ip, addr, match_family_size)) {
        if (port < 0)
            return true;
        if (ntohs(fe->fe_key.flow_sport) == port)
            return true;
    }

    return false;
}

static void
flow_print_spaces(void)
{
    unsigned int i;

    for (i = 0; i < FLOW_GET_FIELD_LENGTH; i++)
        printf("%c", ' ');
    return;
}

static void
flow_print_field_name(const char *field)
{
    unsigned int printed = 0, i;

    printed = printf("%s:", field);
    if (printed < FLOW_GET_FIELD_LENGTH) {
        for (i = printed; i < FLOW_GET_FIELD_LENGTH; i++)
            printf(" ");
    }

    return;
}

static void
flow_print_nh_header(vr_nexthop_req *nh)
{
    printf("NextHop(Index, VRF, Type): %u, %u, ",
            nh->nhr_id, nh->nhr_vrf);
    printf("%s", vr_nexthop_type_string(nh));
    printf("\n");
    return;
}

static void
flow_print_vif(vr_interface_req *vif, char *vif_name, bool ingress)
{
    if (vif) {
        flow_print_spaces();
        if (ingress)
            printf("Ingress ");
        else
            printf("Egress ");
        printf("Interface(Index, VRF, OS): vif0/%u, %d, %s\n",
                vif->vifr_idx, vif->vifr_vrf, vif_name);

        flow_print_spaces();
        printf("Interface Statistics(Out, In, Errors): %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
                vif->vifr_opackets, vif->vifr_ipackets,
                vif->vifr_ierrors + vif->vifr_oerrors);
    }

    return;
}

static void
flow_dump_tunnel(vr_nexthop_req *nh, vr_interface_req *vif,
       char *vif_name, bool ingress)
{
    if (nh) {
        flow_print_nh_header(nh);
        flow_print_spaces();
        printf("Tunnel Source: ");
        if (nh->nhr_family == AF_INET) {
            printf("%s\n",
                    inet_ntop(nh->nhr_family, &nh->nhr_tun_dip,
                        addr_string, sizeof(addr_string)));
        } else if (nh->nhr_family == AF_INET6) {
            printf("%s\n",
                    inet_ntop(nh->nhr_family, nh->nhr_tun_dip6,
                        addr_string, sizeof(addr_string)));
        }
    }

    flow_print_vif(vif, vif_name, ingress);
    return;
}

static void
flow_dump_composite(vr_nexthop_req *nh, vr_interface_req *vif,
        char *vif_name, bool ingress)
{
    unsigned int i;

    if (nh) {
        flow_print_nh_header(nh);
        flow_print_spaces();
        if (nh->nhr_flags & NH_FLAG_COMPOSITE_ECMP) {
            printf("ECMP\n");
            flow_print_spaces();
            for (i = 0; i < nh->nhr_nh_list_size; i++) {
                if (i >= FLOW_COMPONENT_NH_COUNT)
                    break;

                printf("%d", nh->nhr_nh_list[i]);
                if (ecmp_index == i)
                    printf("*");
                printf(", ");
            }
            printf("\n");
            flow_print_spaces();
            if (ecmp_index >= 0) {
                nh = flow_get_nexthop(nh->nhr_nh_list[ecmp_index]);
                flow_dump_nexthop(nh, vif, vif_name, ingress);
            }
        }
    }

    return;
}

static void
flow_dump_encap(vr_nexthop_req *nh, vr_interface_req *vif,
        char *vif_name, bool ingress)
{
    flow_print_nh_header(nh);
    flow_print_vif(vif, vif_name, ingress);

    return;
}

static void
flow_dump_nexthop(vr_nexthop_req *src, vr_interface_req *vif,
        char *vif_name, bool ingress)
{
    if (src) {
        switch (src->nhr_type) {
        case NH_ENCAP:
        case NH_RCV:
            flow_dump_encap(src, vif, vif_name, ingress);
            break;

        case NH_COMPOSITE:
            flow_dump_composite(src, vif, vif_name, ingress);
            break;

        case NH_TUNNEL:
            flow_dump_tunnel(src, vif, vif_name, ingress);
            break;

        default:
            break;
        }
    }

    return;
}

static void
flow_dump_source(vr_nexthop_req *src)
{
    flow_print_field_name("Expected Source");
    flow_dump_nexthop(src, src_vif, src_vif_name, true);

    return;
}

static void
flow_dump_mirror(vr_nexthop_req *req)
{
    if (req->nhr_type == NH_TUNNEL) {
        printf("NextHop %u\n", req->nhr_id);
        flow_print_spaces();
        printf("To Destination ");
        if (req->nhr_family == AF_INET) {
            printf("%s ", inet_ntop(req->nhr_family, &req->nhr_tun_dip,
                        addr_string, sizeof(addr_string)));
        } else if (req->nhr_family == AF_INET6) {
            printf("%s ", inet_ntop(req->nhr_family, req->nhr_tun_dip6,
                        addr_string, sizeof(addr_string)));
        }

        if (req->nhr_vrf < 0)
            printf("in the same VRF (%d)\n", req->nhr_vrf);
        else
            printf("in VRF %d\n", req->nhr_vrf);
        flow_print_spaces();
        if (req->nhr_flags & NH_FLAG_TUNNEL_SIP_COPY) {
            printf("Tunnel Source IP from packet\n");
        } else {
            printf("Tunnel Source IP ");
            if (req->nhr_family == AF_INET) {
                printf("%s ", inet_ntop(req->nhr_family, &req->nhr_tun_sip,
                        addr_string, sizeof(addr_string)));
            } else if (req->nhr_family == AF_INET6) {
                printf("%s ", inet_ntop(req->nhr_family, req->nhr_tun_sip6,
                            addr_string, sizeof(addr_string)));
            }
        }
    } else {
        printf("NextHop %u of type %u", req->nhr_id, req->nhr_type);
    }

    return;
}


static uint64_t
flow_sum_drops_stats(vr_drop_stats_req *req)
{
    uint64_t sum = 0;

    sum += req->vds_flow_queue_limit_exceeded;
    sum += req->vds_flow_no_memory;
    sum += req->vds_flow_invalid_protocol;
    sum += req->vds_flow_nat_no_rflow;
    sum += req->vds_flow_action_drop;
    sum += req->vds_flow_action_invalid;
    sum += req->vds_flow_unusable;
    sum += req->vds_flow_table_full;
    sum += req->vds_drop_new_flow;

    return sum;
}

static void
flow_dump_entry(struct vr_flow_entry *fe)
{
    unsigned int j;
    char in_src[INET6_ADDRSTRLEN], in_dest[INET6_ADDRSTRLEN];
    char in_rsrc[INET6_ADDRSTRLEN], in_rdest[INET6_ADDRSTRLEN];
    char in_rt[INET6_ADDRSTRLEN];

    struct vr_flow_entry *rfe;

    system(CLEAN_SCREEN_CMD);
    flow_print_field_name("Flow Index");
    printf("%lu\n", flow_index);

    flow_print_field_name("Flow Generation ID");
    printf("%u\n", fe->fe_gen_id);

    flow_print_field_name("Reverse Flow Index");
    if ((fe->fe_flags & VR_RFLOW_VALID) && (fe->fe_rflow >= 0)) {
        printf("%u", fe->fe_rflow);
        rfe = flow_get(fe->fe_rflow);
        if (rfe) {
            if ((rfe->fe_type == VP_TYPE_IP) || (rfe->fe_type == VP_TYPE_IP6)) {
                inet_ntop(VR_FLOW_FAMILY(rfe->fe_type), rfe->fe_key.flow_ip,
                        in_rsrc, sizeof(in_rsrc));
                inet_ntop(VR_FLOW_FAMILY(rfe->fe_type),
                        &rfe->fe_key.flow_ip[VR_IP_ADDR_SIZE(rfe->fe_type)],
                        in_rdest, sizeof(in_rdest));
            }
        }

    } else {
        printf("-1");
    }
    printf("\n");

    if ((fe->fe_type == VP_TYPE_IP) || (fe->fe_type == VP_TYPE_IP6)) {
        inet_ntop(VR_FLOW_FAMILY(fe->fe_type), fe->fe_key.flow_ip,
                in_src, sizeof(in_src));
        inet_ntop(VR_FLOW_FAMILY(fe->fe_type),
                &fe->fe_key.flow_ip[VR_IP_ADDR_SIZE(fe->fe_type)],
                in_dest, sizeof(in_dest));
    }


    flow_print_field_name("VRF");
    printf("%d\n", fe->fe_vrf);

    flow_print_field_name("Destination VRF");
    if (fe->fe_flags & VR_FLOW_FLAG_VRFT) {
        printf("%d", fe->fe_dvrf);
    } else {
        printf("%d", fe->fe_vrf);
    }
    printf("\n");

    flow_print_field_name("Flow Source");
    printf("[%s]:%-5u\n", in_src, ntohs(fe->fe_key.flow_sport));

    flow_print_field_name("Flow Destination");
    printf("[%s]:%-5u\n", in_dest, ntohs(fe->fe_key.flow_dport));

    flow_print_field_name("Flow Protocol");
    printf("%s\n", vr_proto_string(fe->fe_key.flow_proto));

    if(fe->fe_underlay_ecmp_index >= 0) {
        flow_print_field_name("Flow Underlay ECMP Index");
        printf("%d\n", fe->fe_underlay_ecmp_index);
    }

    flow_print_field_name("Flow Action");
    switch (fe->fe_action) {
    case VR_FLOW_ACTION_HOLD:
        printf("HOLD");
        break;

    case VR_FLOW_ACTION_FORWARD:
        printf("FORWARD");
        break;

    case VR_FLOW_ACTION_DROP:
        printf("DROP:");
        flow_print_spaces();
        printf("%s", flow_get_drop_reason(fe->fe_drop_reason));
        break;

    case VR_FLOW_ACTION_NAT:
        printf("NAT: ");
        for (j = 0; j < (sizeof(fe->fe_flags) * 8); j++) {
            switch ((1 << j) & fe->fe_flags) {
            case VR_FLOW_FLAG_SNAT:
                printf("SourceNAT, ");
                break;
            case VR_FLOW_FLAG_DNAT:
                printf("DestinationNAT, ");
                break;
            case VR_FLOW_FLAG_SPAT:
                printf("SourcePortNAT, ");
                break;
            case VR_FLOW_FLAG_DPAT:
                printf("DestinationPortNAT, ");
                break;
            case VR_FLOW_FLAG_LINK_LOCAL:
                printf("LinkLocalNAT, ");
                break;
            }
        }
        break;

    default:
        printf("Unknown");
        break;
    }
    printf("\n");

    if (fe->fe_action == VR_FLOW_ACTION_NAT) {
        flow_print_spaces();
        printf("NAT(Source, Destination): ");
        if (rfe) {
            if (fe->fe_flags & VR_FLOW_FLAG_SNAT)
                printf("[%s]:", in_rdest);
            else
                printf("[%s]:", in_src);

            if (fe->fe_flags & VR_FLOW_FLAG_SPAT)
                printf("%d", ntohs(rfe->fe_key.flow_dport));
            else
                printf("%d", ntohs(fe->fe_key.flow_sport));
            printf(", ");

            if (fe->fe_flags & VR_FLOW_FLAG_DNAT)
                printf("[%s]:", in_rsrc);
            else
                printf("[%s]:", in_dest);

            if (fe->fe_flags & VR_FLOW_FLAG_DPAT)
                printf("%d", ntohs(rfe->fe_key.flow_sport));
            else
                printf("%d", ntohs(fe->fe_key.flow_dport));
        }
        printf("\n");
    }

    if (src_nh) {
        flow_dump_source(src_nh);
    }


    if (src_l3_rt) {
        flow_print_field_name("Source Information");
        printf("VRF: %u\n", src_l3_rt->rtr_vrf_id);
        address_mask(src_l3_rt->rtr_prefix, src_l3_rt->rtr_prefix_len,
                src_l3_rt->rtr_family);
        flow_print_spaces();
        printf("Layer 3 Route Information\n");
        flow_print_spaces();
        printf("Matching Route: %s/%-2d\n",
                inet_ntop(src_l3_rt->rtr_family, src_l3_rt->rtr_prefix, in_rt,
                    sizeof(in_rt)), src_l3_rt->rtr_prefix_len);
        if (src_l3_nh) {
            flow_print_spaces();
            flow_dump_nexthop(src_l3_nh, src_l3_vif, src_l3_vif_name, true);
        }

        if (src_l3_rt->rtr_mac) {
            printf("\n");
            flow_print_spaces();
            printf("Layer 2 Route Information\n");
            flow_print_spaces();
            printf("SourceMAC: ");
            printf("%s\n", ether_ntoa((struct ether_addr *)(src_l3_rt->rtr_mac)));
            if (src_l2_rt) {
                flow_print_spaces();
                flow_dump_nexthop(src_l2_nh, src_l2_vif, src_l2_vif_name, true);
            }
        }

    }

    if (dst_l3_rt) {
        flow_print_field_name("Destination Information");
        printf("VRF: %u\n", dst_l3_rt->rtr_vrf_id);
        address_mask(dst_l3_rt->rtr_prefix, dst_l3_rt->rtr_prefix_len,
                dst_l3_rt->rtr_family);
        flow_print_spaces();
        printf("Layer 3 Route Information\n");
        flow_print_spaces();
        printf("Matching Route: %s/%-2d\n",
                inet_ntop(dst_l3_rt->rtr_family, dst_l3_rt->rtr_prefix, in_rt,
                    sizeof(in_rt)), dst_l3_rt->rtr_prefix_len);
        if (dst_l3_nh) {
            flow_print_spaces();
            flow_dump_nexthop(dst_l3_nh, dst_l3_vif, dst_l3_vif_name, false);
        }

        if (dst_l3_rt->rtr_mac) {
            printf("\n");
            flow_print_spaces();
            printf("Layer 2 Route Information\n");
            flow_print_spaces();
            printf("DestinationMAC: ");
            printf("%s\n", ether_ntoa((struct ether_addr *)(dst_l3_rt->rtr_mac)));
            if (dst_l2_rt) {
                flow_print_spaces();
                flow_dump_nexthop(dst_l2_nh, dst_l2_vif, dst_l2_vif_name, false);
            }
        }

    }

    printf("\n");
    flow_print_field_name("Flow Flags");
    if (fe->fe_flags & VR_FLOW_FLAG_EVICTED)
        printf("EVICTED ");
    if (fe->fe_flags & VR_FLOW_FLAG_EVICT_CANDIDATE)
        printf("EVICT CANDIDATE ");
    if (fe->fe_flags & VR_FLOW_FLAG_NEW_FLOW)
        printf("NEW ");
    if (fe->fe_flags & VR_FLOW_FLAG_MODIFIED)
        printf("MODIFIED ");
    if (fe->fe_flags & VR_FLOW_FLAG_MIRROR)
        printf("MIRROR ");
    printf("\n");

    if (fe->fe_key.flow_proto == VR_IP_PROTO_TCP) {
        flow_print_field_name("TCP FLAGS");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_SYN)
            printf("SYN, ");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_SYN_R)
            printf("SYN(REVERSE), ");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_ESTABLISHED)
            printf("ESTABLISHED, ");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_ESTABLISHED_R)
            printf("ESTABLISHED(REVERSE), ");

        if (fe->fe_tcp_flags & VR_FLOW_TCP_FIN)
            printf("FIN, ");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_FIN_R)
            printf("FIN(REVERSE), ");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_RST)
            printf("RESET, ");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_HALF_CLOSE)
            printf("HALFCLOSED, ");
        if (fe->fe_tcp_flags & VR_FLOW_TCP_DEAD)
            printf("DEAD, ");
        printf("\n");
    }

    flow_print_field_name("UDP Source Port");
    printf("%u\n", fe->fe_udp_src_port);

    if (fe->fe_flags & VR_FLOW_FLAG_MIRROR) {
        printf("\n");
        flow_print_field_name("Mirror Index");
        if (fe->fe_mirror_id < VR_MAX_MIRROR_INDICES)
            printf("%d", fe->fe_mirror_id);
        if (fe->fe_sec_mirror_id < VR_MAX_MIRROR_INDICES)
            printf(", %d, ", fe->fe_sec_mirror_id);


        if (mirror_nh) {
            flow_print_field_name("Primary Mirror");
            flow_dump_mirror(mirror_nh);
        }

        if (mirror1_nh) {
            flow_print_field_name("Secondary Mirror");
            flow_dump_mirror(mirror1_nh);
        }
    }

    printf("\n");
    flow_print_field_name("Flow Statistics");
    printf("%u/%u\n", fe->fe_stats.flow_packets, fe->fe_stats.flow_bytes);
    flow_print_field_name("System Wide Packet Drops");
    printf("%" PRIu64 "\n", vr_sum_drop_stats(global_ds));
    flow_print_spaces();
    printf("Reverse Path Failures: %" PRIu64 "\n", global_ds->vds_invalid_source);
    flow_print_spaces();
    printf("Flow Block Drops: %" PRId64 "\n", flow_sum_drops_stats(global_ds));

    return;
}

static void
flow_get_routes(struct vr_flow_entry *fe)
{
    unsigned int vrf;
    unsigned int family = VR_FLOW_FAMILY(fe->fe_type);
    struct vr_flow_entry *rfe;

    if (fe->fe_action == VR_FLOW_ACTION_NAT) {
        if (!(fe->fe_flags & VR_RFLOW_VALID))
            return;
        rfe = flow_get(fe->fe_rflow);
        if (!rfe)
            return;

    }

    vrf = fe->fe_vrf;
    src_l3_rt = flow_get_route(family, vrf, fe->fe_key.flow_ip);
    if (src_l3_rt) {
        src_l3_nh = flow_get_nexthop(src_l3_rt->rtr_nh_id);
        if (src_l3_nh) {
            if (vr_nexthop_req_has_vif(src_l3_nh)) {
                src_l3_vif = flow_get_vif(src_l3_nh->nhr_encap_oif_id[0]);
                if_indextoname(src_l3_vif->vifr_os_idx, src_l3_vif_name);
            }
        }

        if (vr_valid_mac_address(src_l3_rt->rtr_mac)) {
            src_l2_rt = flow_get_route(AF_BRIDGE, vrf, src_l3_rt->rtr_mac);
            if (src_l2_rt) {
                src_l2_nh = flow_get_nexthop(src_l2_rt->rtr_nh_id);
                if (vr_nexthop_req_has_vif(src_l2_nh)) {
                    src_l2_vif = flow_get_vif(src_l2_nh->nhr_encap_oif_id[0]);
                    if_indextoname(src_l2_vif->vifr_os_idx, src_l2_vif_name);
                }
            }
        }
    }

    if (fe->fe_flags & VR_FLOW_FLAG_VRFT)
        vrf = fe->fe_dvrf;

    if (fe->fe_flags & VR_FLOW_FLAG_DNAT) {
        dst_l3_rt = flow_get_route(family, vrf, rfe->fe_key.flow_ip);
    } else {
        dst_l3_rt = flow_get_route(family, vrf,
                &fe->fe_key.flow_ip[VR_IP_ADDR_SIZE(fe->fe_type)]);
    }

    if (dst_l3_rt) {
        dst_l3_nh = flow_get_nexthop(dst_l3_rt->rtr_nh_id);
        if (dst_l3_nh) {
            if (vr_nexthop_req_has_vif(dst_l3_nh)) {
                dst_l3_vif = flow_get_vif(dst_l3_nh->nhr_encap_oif_id[0]);
                if_indextoname(dst_l3_vif->vifr_os_idx, dst_l3_vif_name);
            }
        }

        if (vr_valid_mac_address(dst_l3_rt->rtr_mac)) {
            dst_l2_rt = flow_get_route(AF_BRIDGE, vrf, dst_l3_rt->rtr_mac);
            if (dst_l2_rt) {
                dst_l2_nh = flow_get_nexthop(dst_l2_rt->rtr_nh_id);
                if (vr_nexthop_req_has_vif(dst_l2_nh)) {
                    dst_l2_vif = flow_get_vif(dst_l2_nh->nhr_encap_oif_id[0]);
                    if_indextoname(dst_l2_vif->vifr_os_idx, dst_l2_vif_name);
                }
            }
        }
    }

    return;
}

static void
flow_get_source(struct vr_flow_entry *fe)
{
    int i;
    struct vr_flow_entry *rfe;

    if (fe->fe_src_nh_index >= 0) {
        src_nh = flow_get_nexthop(fe->fe_src_nh_index);
        if (src_nh) {
            switch (src_nh->nhr_type) {
            case NH_TUNNEL:
                if (fe->fe_underlay_ecmp_index >= 0)
                    src_vif = flow_get_vif(fe->fe_underlay_ecmp_index);
                else
                    src_vif = flow_get_vif(src_nh->nhr_encap_oif_id[0]);
                if (src_vif->vifr_os_idx > 0)
                    if_indextoname(src_vif->vifr_os_idx, src_vif_name);
                break;
            case NH_ENCAP:
            case NH_RCV:
                src_vif = flow_get_vif(src_nh->nhr_encap_oif_id[0]);
                if (src_vif->vifr_os_idx > 0)
                    if_indextoname(src_vif->vifr_os_idx, src_vif_name);
                break;

            case NH_COMPOSITE:
                if ((src_nh->nhr_flags & NH_FLAG_COMPOSITE_ECMP)) {
                    if (fe->fe_flags & VR_RFLOW_VALID) {
                        rfe = flow_get(fe->fe_rflow);
                        if (rfe && (rfe->fe_flags & VR_FLOW_FLAG_ACTIVE))
                            ecmp_index = rfe->fe_ecmp_nh_index;
                    }

                    if (src_nh->nhr_nh_list) {
                        for (i = 0; i < src_nh->nhr_nh_list_size; i++) {
                            if (i >= FLOW_COMPONENT_NH_COUNT)
                                break;

                            component_nh[i] =
                                flow_get_nexthop(src_nh->nhr_nh_list[i]);
                        }
                    }
                }

                break;

            default:
                break;
            }
        }
    }

    return;
}

/*
 * cleanup - release all the memory that we allocated for
 * the get operation
 */
static void
flow_get_cleanup(void)
{
    unsigned int i;

    vr_interface_req_destroy(src_vif);
    vr_interface_req_destroy(src_l3_vif);
    vr_interface_req_destroy(src_l2_vif);
    vr_interface_req_destroy(dst_l3_vif);
    vr_interface_req_destroy(dst_l2_vif);
    src_vif = src_l3_vif = src_l2_vif = dst_l3_vif = dst_l2_vif = NULL;

    for (i = 0; i < FLOW_COMPONENT_NH_COUNT; i++) {
        vr_nexthop_req_destroy(component_nh[i]);
        component_nh[i] = NULL;
    }
    vr_nexthop_req_destroy(src_nh);
    vr_nexthop_req_destroy(src_l3_nh);
    vr_nexthop_req_destroy(src_l2_nh);
    vr_nexthop_req_destroy(dst_l3_nh);
    vr_nexthop_req_destroy(dst_l2_nh);
    vr_nexthop_req_destroy(mirror_nh);
    vr_nexthop_req_destroy(mirror1_nh);
    src_nh = src_l3_nh = src_l2_nh = dst_l3_nh = dst_l2_nh = NULL;

    vr_route_req_destroy(src_l3_rt);
    vr_route_req_destroy(src_l2_rt);
    vr_route_req_destroy(dst_l3_rt);
    vr_route_req_destroy(dst_l2_rt);
    src_l3_rt = src_l2_rt = dst_l3_rt = dst_l2_rt = NULL;

    vr_drop_stats_req_destroy(global_ds);
    global_ds = NULL;

    return;
}

static void
flow_get_entry(struct vr_flow_entry *fe)
{
    /*
     * first step is to get all the information we need, such as
     * nexthops, routes and interfaces
     */

    /* get the source nexthop information */
    flow_get_source(fe);
    /* if the flow has mirror configuration, get the mirror nexthops */
    if (fe->fe_flags & VR_FLOW_FLAG_MIRROR) {
        mirror_nh = flow_get_mirror_nh(fe->fe_mirror_id);
        mirror1_nh = flow_get_mirror_nh(fe->fe_sec_mirror_id);
    }

    /* routes for flow source and destination */
    flow_get_routes(fe);
    /* get the system wide dropstats */
    global_ds = flow_get_dropstats();
    /* we are now ready to dump the flow entry */
    flow_dump_entry(fe);
    /* ...and finally cleanup the memory we allocated */
    flow_get_cleanup();

    return;
}

static inline int
print_new_line_if_required(int printed, int max_print)
{
    if (printed >= max_print) {
        printf("\n");
        return 0;
    }

    return printed;
}

static void
flow_dump_table(struct flow_table *ft)
{
    unsigned int i, j, k, fi, next_index, need_flag_print = 0, printed = 0;
    struct vr_flow_entry *fe, *ofe;
    char action, flag_string[sizeof(fe->fe_flags) * 8 + 32];
    unsigned int need_drop_reason = 0;
    const char *drop_reason = NULL;
    char in_src[INET6_ADDRSTRLEN], in_dest[INET6_ADDRSTRLEN];
    char addr[INET6_ADDRSTRLEN];
    bool smatch, dmatch;

    printf("Flow table(size %" PRIu64 ", entries %u)\n\n", ft->ft_span,
            ft->ft_num_entries);
    printf("Entries: Created %" PRIu64 " Added %" PRIu64 " Deleted %" PRIu64 " Changed %" PRIu64 "Processed %" PRIu64 " Used Overflow entries %u\n",
            ft->ft_created, ft->ft_added, ft->ft_deleted, ft->ft_changed,
            ft->ft_processed, ft->ft_oflow_entries);

    printf("(Created Flows/CPU: ");
    for (i = 0; i < ft->ft_hold_stat_count; i++) {
        printf("%u", ft->ft_hold_stat[i]);
        if (i != (ft->ft_hold_stat_count - 1))
            printf(" ");
    }
    printf(")(oflows %u)\n\n", ft->ft_hold_oflows);

    flow_dump_legend();

    if (match_family || (match_proto > 0) || (match_vrf > 0)) {
        printf("Listing flows matching (");
        if (match_ip1_set) {
            if (match_ip1) {
                inet_ntop(match_family, match_ip1, addr, INET6_ADDRSTRLEN);
                printed += printf("[%s]", addr);
            } else {
                printed += printf("[*]");
            }

            if (match_port1 >= 0)
                printed += printf(":%u", match_port1);
            else
                printed += printf(":*");
        }

        if (match_ip2_set) {
            if (printed)
                printf(", ");

            if (match_ip2) {
                inet_ntop(match_family, match_ip2, addr, INET6_ADDRSTRLEN);
                printed += printf("[%s]", addr);
            } else {
                printed += printf("[*]");
            }

            if (match_port2 >= 0)
                printed += printf(":%u", match_port2);
            else
                printed += printf(":*");
        }

        if (match_proto >= 0) {
            if (printed)
                printf(", ");
            printed += printf("Protocol %s", vr_proto_string(match_proto));
        }

        if (match_vrf >= 0) {
            if (printed)
                printf(", ");
            printf("VRF %d", match_vrf);
        }

        printf(")");
        printed = 0;
        printf("\n\n");
    }

    printf("    Index            ");
    /* inter field gap */
    printf("%4c", ' ');
    /* 40 byte address field - middled header */
    printf("Source:Port/Destination:Port                  ");
    /* inter field gap */
    printf("%4c", ' ');
    printf("Proto(V)\n");
    printf("-----------------------------------------------------------------");
    printf("------------------\n");
    for (i = 0; i < ft->ft_num_entries; i++) {
        memset(flag_string, 0, sizeof(flag_string));
        need_flag_print = 0;
        need_drop_reason = 0;
        fe = (struct vr_flow_entry *)((char *)ft->ft_entries + (i * sizeof(*fe)));
        if (fe->fe_flags & VR_FLOW_FLAG_ACTIVE) {

            if ((fe->fe_flags & VR_FLOW_FLAG_EVICTED) &&
                    !show_evicted_set) {
                continue;
            }


            if (match_vrf >= 0) {
                if (fe->fe_vrf != match_vrf)
                    continue;
            }

            if (match_proto >= 0) {
                if (fe->fe_key.flow_proto != match_proto)
                    continue;
            }

            if (match_family) {
                if (match_family != VR_FLOW_FAMILY(fe->fe_type)) {
                    continue;
                }

                smatch = dmatch = false;
                if (match_ip1_set) {
                    smatch = flow_match_source(fe, match_ip1, match_port1);
                    if (!smatch) {
                        dmatch = flow_match_dest(fe, match_ip1, match_port1);
                    }
                }

                if (match_ip2_set) {
                    if (smatch) {
                        dmatch = flow_match_dest(fe, match_ip2, match_port2);
                        if (!dmatch)
                            continue;
                    } else if (dmatch) {
                        smatch = flow_match_source(fe, match_ip2, match_port2);
                        if (!smatch)
                            continue;
                    } else {
                        smatch = flow_match_source(fe, match_ip2, match_port2);
                        if (!smatch) {
                            dmatch = flow_match_dest(fe, match_ip2, match_port2);
                        }

                    }
                }

                if (!smatch && !dmatch)
                    continue;

                if (match_ip1_set && match_ip2_set) {
                    if (!smatch || !dmatch) {
                        continue;
                    }
                }
            }


            if ((fe->fe_type == VP_TYPE_IP) || (fe->fe_type == VP_TYPE_IP6)) {
                inet_ntop(VR_FLOW_FAMILY(fe->fe_type), fe->fe_key.flow_ip,
                            in_src, sizeof(in_src));
                inet_ntop(VR_FLOW_FAMILY(fe->fe_type),
                      &fe->fe_key.flow_ip[VR_IP_ADDR_SIZE(fe->fe_type)],
                      in_dest, sizeof(in_dest));
            }

            printf("%9d", i);
            if (fe->fe_rflow >= 0)
                printf("<=>%-9d", fe->fe_rflow);
            else
                printf("%12c", ' ');

            printf("%4c", ' ');
            if (fe->fe_type == VP_TYPE_IP) {
                printed = printf("%s:%-5d", in_src, ntohs(fe->fe_key.flow_sport));
                for (k = printed; k < 46; k++)
                    printf(" ");
                printf("%4c", ' ');
                printf("%3d (%d", fe->fe_key.flow_proto, fe->fe_vrf);
                if (fe->fe_flags & VR_FLOW_FLAG_VRFT)
                    printf("->%d", fe->fe_dvrf);
                printf(")\n");
                printf("%25c", ' ');
                printf("%s:%-5d", in_dest, ntohs(fe->fe_key.flow_dport));
            } else if (fe->fe_type == VP_TYPE_IP6) {
                printed = printf("%s:%-5d    ", in_src, ntohs(fe->fe_key.flow_sport));
                for (k = printed; k < 46; k++)
                    printf(" ");
                printf("%4c", ' ');
                printf("%3d (%d", fe->fe_key.flow_proto, fe->fe_vrf);
                if (fe->fe_flags & VR_FLOW_FLAG_VRFT)
                    printf("->%d", fe->fe_dvrf);
                printf(")\n");
                printf("%25c", ' ');
                printf("%s:%-5d    ", in_dest, ntohs(fe->fe_key.flow_dport));
            }

            printf("\n");

            switch (fe->fe_action) {
            case VR_FLOW_ACTION_HOLD:
                action = 'H';
                break;

            case VR_FLOW_ACTION_FORWARD:
                action = 'F';
                break;

            case VR_FLOW_ACTION_DROP:
                action = 'D';
                need_drop_reason = 1;
                drop_reason = flow_get_drop_reason(fe->fe_drop_reason);
                break;

            case VR_FLOW_ACTION_NAT:
                action = 'N';
                need_flag_print = 1;
                fi = 0;
                for (j = 0; (j < (sizeof(fe->fe_flags) * 8)) &&
                        fi < sizeof(flag_string); j++)
                    switch ((1 << j) & fe->fe_flags) {
                    case VR_FLOW_FLAG_SNAT:
                        flag_string[fi++] = 'S';
                        break;
                    case VR_FLOW_FLAG_DNAT:
                        flag_string[fi++] = 'D';
                        break;
                    case VR_FLOW_FLAG_SPAT:
                        flag_string[fi++] = 'P';
                        flag_string[fi++] = 's';
                        break;
                    case VR_FLOW_FLAG_DPAT:
                        flag_string[fi++] = 'P';
                        flag_string[fi++] = 'd';
                        break;
                    case VR_FLOW_FLAG_LINK_LOCAL:
                        flag_string[fi++] = 'L';
                    }

                break;

            default:
                action = 'U';
            }

            printed = printf("(");
            printed += printf("Gen: %u, ", fe->fe_gen_id);
            if ((fe->fe_type == VP_TYPE_IP) || (fe->fe_type == VP_TYPE_IP6))
                printed += printf("K(nh):%u, ", fe->fe_key.flow_nh_id);

            printed += printf("Action:%c", action);
            if (need_flag_print)
                printed += printf("(%s)", flag_string);
            if (need_drop_reason) {
                if (drop_reason != NULL)
                    printed += printf("(%s)", drop_reason);
                else
                    printed += printf("(%u)", fe->fe_drop_reason);
            }

            printed += printf(", ");
            printed += printf("Flags:");
            if (fe->fe_flags & VR_FLOW_FLAG_EVICTED)
                printed += printf("E");
            if (fe->fe_flags & VR_FLOW_FLAG_EVICT_CANDIDATE)
                printed += printf("Ec");
            if (fe->fe_flags & VR_FLOW_FLAG_NEW_FLOW)
                printed += printf("N");
            if (fe->fe_flags & VR_FLOW_FLAG_MODIFIED)
                printed += printf("M");
            if (fe->fe_flags & VR_FLOW_FLAG_DELETE_MARKED)
                printed += printf("Dm");

            printed += printf(", ");
            if (fe->fe_key.flow4_proto == VR_IP_PROTO_TCP) {
                printed += printf("TCP:");

                if (fe->fe_tcp_flags & VR_FLOW_TCP_SYN)
                    printed += printf("S");
                if (fe->fe_tcp_flags & VR_FLOW_TCP_SYN_R)
                    printed += printf("Sr");
                if (fe->fe_tcp_flags & VR_FLOW_TCP_ESTABLISHED)
                    printed += printf("E");
                if (fe->fe_tcp_flags & VR_FLOW_TCP_ESTABLISHED_R)
                    printed += printf("Er");

                if (fe->fe_tcp_flags & VR_FLOW_TCP_FIN)
                    printed += printf("F");
                if (fe->fe_tcp_flags & VR_FLOW_TCP_FIN_R)
                    printed += printf("Fr");
                if (fe->fe_tcp_flags & VR_FLOW_TCP_RST)
                    printed += printf("R");
                if (fe->fe_tcp_flags & VR_FLOW_TCP_HALF_CLOSE)
                    printed += printf("C");
                if (fe->fe_tcp_flags & VR_FLOW_TCP_DEAD)
                    printed += printf("D");

                printed += printf(", ");
            }

            if (fe->fe_ecmp_nh_index >= 0)
                printed += printf("E:%d, ", fe->fe_ecmp_nh_index);

            printed += printf("QOS:%d, ", fe->fe_qos_id);
            printed += printf("S(nh):%u, ", fe->fe_src_nh_index);
            printed = print_new_line_if_required(printed, 70);
            printed += printf(" Stats:%u/%u, ", fe->fe_stats.flow_packets,
                    fe->fe_stats.flow_bytes);
            printed = print_new_line_if_required(printed, 70);

            if (fe->fe_flags & VR_FLOW_FLAG_MIRROR) {
                printed += printf(" Mirror Index :");
                if (fe->fe_mirror_id < VR_MAX_MIRROR_INDICES)
                    printed += printf(" %d", fe->fe_mirror_id);
                if (fe->fe_sec_mirror_id < VR_MAX_MIRROR_INDICES)
                    printed += printf(", %d, ", fe->fe_sec_mirror_id);
                printed = print_new_line_if_required(printed, 70);
            }
            printed += printf(" SPort %d,", fe->fe_udp_src_port);
            printed = print_new_line_if_required(printed, 70);
            printf(" TTL %d,", fe->fe_ttl);
            printed = print_new_line_if_required(printed, 70);
            if(fe->fe_underlay_ecmp_index >= 0) {
                printf(" UnderlayEcmpIdx:%d,", fe->fe_underlay_ecmp_index);
                printed = print_new_line_if_required(printed, 70);
            }
            if (fe->fe_flags1 & VR_FLOW_FLAG1_HBS_LEFT)
                printed += printf(" HbsLeft,");
            else if (fe->fe_flags1 & VR_FLOW_FLAG1_HBS_RIGHT)
                printed += printf(" HbsRight,");
            inet_ntop(AF_INET, &fe->fe_src_info, in_dest, sizeof(in_dest));
            printf(" Sinfo %s", in_dest);
            printf(")");
        }

        j = -1;
        next_index = fe->fe_hentry.hentry_next_index;
        while (next_index != VR_INVALID_HENTRY_INDEX) {
            ofe = (struct vr_flow_entry *)((char *)ft->ft_entries +
                        (next_index * sizeof(*fe)));
            if (j == -1) {
                if (!(fe->fe_flags & VR_FLOW_FLAG_ACTIVE))
                    printf("%6d", i);

                printf("\n     Oflow entries:\n\t");
                j = 0;
            }
            j += printf(" %d", ofe->fe_hentry.hentry_index);
            if (j > 65) {
                printf("\n     ");
                j = 0;
            }

            next_index = ofe->fe_hentry.hentry_next_index;
            /* CEM-24972: memory mapped area for flow is pointing to incorrect memory location so next_index coming as 0 */
            if(!next_index) {
                printf("\nERROR:flow file corrupted,reload vRouter module to recover\n");
                return;
            }
        }

        if ((fe->fe_flags & VR_FLOW_FLAG_ACTIVE) || (j != -1))
            printf("\n\n");
    }

    return;
}

static void
flow_list(void)
{
    flow_dump_table(&main_table);
    return;
}

static void
flow_stats(void)
{
    struct flow_table *ft = &main_table;
    unsigned int i, iteration = 0;
    struct vr_flow_entry *fe;
    struct timeval now;
    struct timeval last_time;
    int active_entries = 0;
    int hold_entries = 0;
    int prev_active_entries = 0;
    int prev_hold_entries = 0;
    int total_entries = 0;
    int prev_total_entries = 0;
    int diff_ms;
    int rate;
    uint64_t avg_setup_rate = 0;
    uint64_t avg_teardown_rate = 0;
    uint64_t setup_time = 0;
    uint64_t teardown_time = 0;
    int total_rate;
    int flow_action_drop = 0;
    int flow_action_fwd = 0;
    int flow_action_nat = 0;

    gettimeofday(&last_time, NULL);
    while (1) {
        active_entries = 0;
        total_entries = 0;
        hold_entries = 0;
        flow_action_drop = 0;
        flow_action_fwd = 0;
        flow_action_nat = 0;
        usleep(500000);
        for (i = 0; i < ft->ft_num_entries; i++) {
            fe = (struct vr_flow_entry *)((char *)ft->ft_entries +
                                          (i * sizeof(*fe)));
            if (fe->fe_flags & VR_FLOW_FLAG_ACTIVE) {
                if (fe->fe_flags & VR_FLOW_FLAG_EVICTED) {
                    continue;
                }
                total_entries++;
                if (fe->fe_action != VR_FLOW_ACTION_HOLD) {
                    active_entries++;
                }
                if (fe->fe_action == VR_FLOW_ACTION_DROP) {
                    flow_action_drop++;
                } else if (fe->fe_action == VR_FLOW_ACTION_FORWARD) {
                    flow_action_fwd++;
                } else if (fe->fe_action == VR_FLOW_ACTION_NAT) {
                    flow_action_nat++;
                }
            }
        }
        hold_entries = ft->ft_hold_entries;
        gettimeofday(&now, NULL);
        /* calc time difference and rate */
        diff_ms = (now.tv_sec - last_time.tv_sec) * 1000;
        diff_ms += (now.tv_usec - last_time.tv_usec) / 1000;
        assert(diff_ms > 0 );
        rate = (active_entries - prev_active_entries) * 1000;
        rate /= diff_ms;
        total_rate = (total_entries - prev_total_entries) * 1000;
        total_rate /= diff_ms;
        if (iteration == 0) {
            rate = 0;
            total_rate = 0;
            iteration = 1;
        }

        if (rate != 0 || total_rate != 0) {
            if (rate < -1000) {
                avg_teardown_rate = avg_teardown_rate * teardown_time -
                    (active_entries - prev_active_entries) * 1000;
                teardown_time += diff_ms;
                avg_teardown_rate /= teardown_time;
            }

            if (rate > 1000) {
                avg_setup_rate = avg_setup_rate * setup_time +
                    (active_entries - prev_active_entries) * 1000;
                setup_time += diff_ms;
                avg_setup_rate /= setup_time;
            }
        }

        /* On Ubuntu system() is declared with warn_unused_result
         * attribute, so we suppress the warning
         */
        if (system(CLEAN_SCREEN_CMD) == -1) {
            printf("Error: system() failed\n");
        }

        time_t secs = now.tv_sec;
        struct tm *tm;
        char fmt[64], buf[64];
        if((tm = localtime(&secs)) != NULL)
        {
            strftime(fmt, sizeof fmt, "%Y-%m-%d %H:%M:%S %z", tm);
            snprintf(buf, sizeof buf, fmt, now.tv_usec);
            printf("%s\n", buf);
        }

        printf("Flow Statistics\n");
        printf("---------------\n");
        printf("    Total  Entries  --- Total = %7d, new = %7d \n",
                total_entries, (total_entries - prev_total_entries));
        printf("    Active Entries  --- Total = %7d, new = %7d \n",
                active_entries, (active_entries - prev_active_entries));
        printf("    Hold   Entries  --- Total = %7d, new = %7d \n",
                hold_entries, (hold_entries - prev_hold_entries));
        printf("    Fwd flow Entries  - Total = %7d\n", flow_action_fwd);
        printf("    drop flow Entries - Total = %7d\n", flow_action_drop);
        printf("    NAT flow Entries  - Total = %7d\n\n", flow_action_nat);
        printf("    Rate of change of Active Entries\n");
        printf("    --------------------------------\n");
        printf("        current rate      = %8d\n", rate);
        printf("        Avg setup rate    = %8zu\n", avg_setup_rate);
        printf("        Avg teardown rate = %8zu\n", avg_teardown_rate);
        printf("    Rate of change of Flow Entries\n");
        printf("    ------------------------------\n");
        printf("        current rate      = %8d\n", total_rate);

        last_time = now;
        prev_active_entries = active_entries;
        prev_total_entries = total_entries;
        prev_hold_entries = hold_entries;
    }
}

static void
flow_rate(void)
{
    struct flow_table *ft = &main_table;
    struct vr_flow_entry *fe;
    struct timeval now;
    struct timeval last_time;
    int diff_ms;
    int hold_count_old = 0;
    int processed_count_old = 0;
    int added_count_old = 0;
    int deleted_count_old = 0;

    gettimeofday(&last_time, NULL);
    while (1) {
        int hold_rate = 0;
        int hold_count = 0;

        int processed_rate = 0;
        int processed_count = 0;

        int added_rate = 0;
        int added_count = 0;

        int deleted_rate = 0;
        int deleted_count = 0;

        int flow_op_rate = 0;
        struct tm *tm;

        usleep(500000);
        flow_table_get();
        gettimeofday(&now, NULL);

        /* calc time difference and rate */
        diff_ms = (now.tv_sec - last_time.tv_sec) * 1000;
        diff_ms += (now.tv_usec - last_time.tv_usec) / 1000;
        assert(diff_ms > 0 );

        hold_count = ft->ft_hold_entries;
        hold_rate = hold_count * 1000 / diff_ms;

        processed_count = ft->ft_processed - processed_count_old;
        processed_rate = processed_count * 1000 / diff_ms;

        added_count = ft->ft_added - added_count_old;
        added_rate = added_count * 1000 / diff_ms;

        deleted_count = ft->ft_deleted - deleted_count_old;
        deleted_rate = deleted_count * 1000 / diff_ms;

        flow_op_rate = processed_rate + added_rate + deleted_rate;

        char fmt[64];
        time_t secs = now.tv_sec;
        tm = localtime(&secs);
        strftime(fmt, sizeof fmt, "%Y-%m-%d %H:%M:%S", tm);

        if (hold_rate || processed_rate || added_rate || deleted_rate) {
            printf ("%s.%03d:  Entries = %8d "
                    "Rate = %6d (Fwd = %8d Rev = %8d Del = %8d) "
                    "Hold = %8d Free Burst Tokens = %8d Total Hold Entries %8d\n",
                    fmt, (int)now.tv_usec/1000,
                    (int)ft->ft_total_entries, flow_op_rate, processed_count,
                    added_count, deleted_count, hold_count,
                    ft->ft_burst_free_tokens,
                    ft->ft_hold_entries);
            fflush(stdout);
        }

        last_time = now;
        hold_count_old = hold_count;
        processed_count_old = ft->ft_processed;
        added_count_old = ft->ft_added;
        deleted_count_old = ft->ft_deleted;
    }
}

static int
get_flow_table_map_counts(vr_flow_table_data *table, struct flow_table *ft)
{
    ft->ft_processed = table->ftable_processed;
    ft->ft_created = table->ftable_created;
    ft->ft_hold_oflows = table->ftable_hold_oflows;
    ft->ft_added = table->ftable_added;
    ft->ft_oflow_entries = table->ftable_oflow_entries;
    ft->ft_deleted = table->ftable_deleted;
    ft->ft_changed = table->ftable_changed;
    ft->ft_total_entries = table->ftable_used_entries;
    ft->ft_burst_free_tokens = table->ftable_burst_free_tokens;
    ft->ft_hold_entries = table->ftable_hold_entries;


    return 0;
}

static int
flow_table_map(vr_flow_table_data *table)
{
    const char *mmap_error_msg;
    int ret;
    unsigned int i;
    struct flow_table *ft = &main_table;
    const char *flow_path;

    if (table->ftable_dev < 0)
        exit(ENODEV);

    if (ft->ft_entries != 0) {
        get_flow_table_map_counts(table, ft);
        return ft->ft_num_entries;
    }

    mmap_error_msg = vr_table_map(table->ftable_dev, VR_MEM_FLOW_TABLE_OBJECT,
        table->ftable_file_path, table->ftable_size, (void **)&ft->ft_entries);

    if (mmap_error_msg) {
        printf("%s\n", mmap_error_msg);
        exit(1);
    }

    ft->ft_span = table->ftable_size;
    ft->ft_num_entries = ft->ft_span / sizeof(struct vr_flow_entry);
    ft->ft_processed = table->ftable_processed;
    ft->ft_created = table->ftable_created;
    ft->ft_hold_oflows = table->ftable_hold_oflows;
    ft->ft_added = table->ftable_added;
    ft->ft_cpus = table->ftable_cpus;
    ft->ft_oflow_entries = table->ftable_oflow_entries;
    ft->ft_deleted = table->ftable_deleted;
    ft->ft_changed = table->ftable_changed;
    ft->ft_burst_free_tokens = table->ftable_burst_free_tokens;
    ft->ft_hold_entries = table->ftable_hold_entries;


    if (table->ftable_hold_stat && table->ftable_hold_stat_size) {
        ft->ft_hold_stat_count = table->ftable_hold_stat_size;
        for (i = 0; i < table->ftable_hold_stat_size; i++) {
            if (i ==
                    (sizeof(ft->ft_hold_stat) / sizeof(ft->ft_hold_stat[0]))) {
                ft->ft_hold_stat_count = i;
                break;
            }

            ft->ft_hold_stat[i] = table->ftable_hold_stat[i];
        }
    } else {
        ft->ft_hold_stat_count = 0;
        memset(ft->ft_hold_stat, 0, sizeof(ft->ft_hold_stat));
    }

    return ft->ft_num_entries;
}

static int
flow_make_flow_req(void *req, char *flow_str)
{
    int ret;

    ret = vr_sendmsg(cl, req, flow_str);
    if (ret <= 0)
        return ret;

    ret = vr_recvmsg(cl, false);
    if (ret <= 0)
        return ret;

    cl->cl_buf_offset = 0;
    return ret;
}

static int
flow_table_get(void)
{
    /* get the kernel's view of the flow table */
    memset(&ftable, 0, sizeof(ftable));
    ftable.ftable_op = FLOW_OP_FLOW_TABLE_GET;

    return flow_make_flow_req(&ftable, "vr_flow_table_data");
}

static int
flow_table_setup(void)
{
    int ret = 0;

    if (sock_dir_set) {
        set_platform_vtest();
    }
    cl = vr_get_nl_client(VR_NETLINK_PROTO_DEFAULT);
    if (cl == NULL)
        return -ENOMEM;

    cl->cl_buf_offset = 0;
    return ret;
}

static int
fill_flow_req(vr_flow_req *req, unsigned long flow_index, char action,
        bool flush)
{
    struct vr_flow_entry *fe;

    memset(req, 0, sizeof(*req));
    fe = flow_get(flow_index);
    if (!fe) {
        printf("Invalid flow index value %lu\n", flow_index);
        return -1;
    }

    if (action == 'g') {
        if (!(fe->fe_flags & VR_FLOW_FLAG_ACTIVE)) {
            printf("Flow index %lu is not active\n", flow_index);
            return -1;
        }

        if (!show_evicted_set && (fe->fe_flags & VR_FLOW_FLAG_EVICTED)) {
            printf("Flow at index %lu is EVICTED. Use --show-evicted\n",
                    flow_index);
            return -1;
        }

        flow_get_entry(fe);
        return -1;
    }

    if ((fe->fe_type != VP_TYPE_IP) && (fe->fe_type != VP_TYPE_IP6))
        return -1;

    req->fr_op = FLOW_OP_FLOW_SET;
    req->fr_index = flow_index;
    req->fr_family = VR_FLOW_FAMILY(fe->fe_type);
    req->fr_flags = VR_FLOW_FLAG_ACTIVE;
    if (fe->fe_type == VP_TYPE_IP) {
        req->fr_flow_sip_l = fe->fe_key.flow4_sip;
        req->fr_flow_dip_l = fe->fe_key.flow4_dip;
    } else {
        memcpy(&req->fr_flow_sip_u, fe->fe_key.flow6_sip, sizeof(uint64_t));
        memcpy(&req->fr_flow_sip_l, (fe->fe_key.flow6_sip + sizeof(uint64_t)),
               sizeof(uint64_t));
        memcpy(&req->fr_flow_dip_u, fe->fe_key.flow6_dip, sizeof(uint64_t));
        memcpy(&req->fr_flow_dip_l, (fe->fe_key.flow6_dip + sizeof(uint64_t)),
               sizeof(uint64_t));
    }

    req->fr_flow_proto = fe->fe_key.flow_proto;
    req->fr_flow_sport = fe->fe_key.flow_sport;
    req->fr_flow_dport = fe->fe_key.flow_dport;
    req->fr_flow_nh_id = fe->fe_key.flow_nh_id;
    req->fr_gen_id     = fe->fe_gen_id;

    switch (action) {
    case 'd':
        req->fr_action = VR_FLOW_ACTION_DROP;
        break;

    case 'f':
        req->fr_action = VR_FLOW_ACTION_FORWARD;
        break;

    case 'i':
        if (fe->fe_flags & VR_FLOW_FLAG_EVICTED) {
            if (!flush)
                printf("Flow %lu is evicted!\n", flow_index);
            return -1;
        }
        req->fr_flags = VR_FLOW_FLAG_ACTIVE ^ VR_FLOW_FLAG_ACTIVE;
        req->fr_action = VR_FLOW_ACTION_DROP;
        break;

    case 'e':
        req->fr_extflags = VR_FLOW_EXT_FLAG_FORCE_EVICT;
        break;

    default:
        return -1;
    }

    if (mirror >= 0) {
        req->fr_mir_id = mirror;
        req->fr_flags |= VR_FLOW_FLAG_MIRROR;
    } else
        req->fr_flags &= ~VR_FLOW_FLAG_MIRROR;

    return 0;
}

static void
flow_do_op(unsigned long flow_index, char action)
{
    if (fill_flow_req(&flow_req, flow_index, action, false))
        return;
    flow_make_flow_req(&flow_req, "vr_flow_req");
    return;
}

static void
flow_process_response()
{
    int ret;
    struct nl_response *resp;

    cl->cl_buf_offset = 0;
    if ((ret = nl_recvmsg(cl)) > 0) {
        resp = nl_parse_reply(cl);
        if (resp->nl_op == SANDESH_REQUEST) {
            sandesh_decode(resp->nl_data, resp->nl_len, vr_find_sandesh_info, &ret);
        }
    }
}

static int
flow_make_flow_req_perf(vr_flow_req *req)
{
    int ret, attr_len, error;
    struct nl_response *resp;
    static int count = 0;
    static struct iovec iov[MAX_FLOW_NL_MSG_BUNCH];
    uint8_t *base;

    base = nl_get_buf_ptr(cl);

    if (!count) {
        ret = nl_build_nlh(cl, cl->cl_genl_family_id, NLM_F_REQUEST);
        if (ret)
            return ret;

        ret = nl_build_genlh(cl, SANDESH_REQUEST, 0);
        if (ret)
            return ret;

        attr_len = nl_get_attr_hdr_size();
    } else {
        attr_len = 0;
    }

    error = 0;
    ret = sandesh_encode(req, "vr_flow_req", vr_find_sandesh_info,
                         (nl_get_buf_ptr(cl) + attr_len),
                         (nl_get_buf_len(cl) - attr_len), &error);

    if ((ret <= 0) || error)
        return ret;

    if (!count) {
        nl_build_attr(cl, ret, NL_ATTR_VR_MESSAGE_PROTOCOL);
    } else {
        nl_update_attr_len(cl, ret);
    }

    nl_update_nlh(cl);

    iov[count].iov_base = base;
    iov[count].iov_len = nl_get_buf_ptr(cl) - base;
    count++;
    if (bunch != count && more)
        return 0;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
#if defined (__linux__)
    msg.msg_name = cl->cl_sa;
    msg.msg_namelen = cl->cl_sa_len;
#endif

    msg.msg_iov = iov;
    msg.msg_iovlen = count;

    ret = sendmsg(cl->cl_sock, &msg, 0);
    if (ret <= 0)
        return ret;

    cl->cl_buf_offset = 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        ret = 0;

    while (count != 0) {
        count--;
        flow_process_response();
     }

    cl->cl_buf_offset = 0;

    return ret;
}

void
run_perf(void)
{
    struct vr_flow_entry *fe;
    uint32_t sip = inet_addr("1.1.1.1");
    uint32_t dip = inet_addr("2.2.2.2");
    int ret;
    uint8_t proto = 0xFF;
    uint16_t sport = 1000;
    uint16_t nhid = 1;

    memset(&flow_req, 0, sizeof(flow_req));
    flow_req.fr_family = AF_INET;
    flow_req.fr_op = FLOW_OP_FLOW_SET;
    flow_req.fr_flags = VR_FLOW_FLAG_ACTIVE;
    flow_req.fr_flow_sip_l = sip;
    flow_req.fr_flow_dip_l = dip;
    flow_req.fr_flow_proto = proto;
    flow_req.fr_flow_nh_id = nhid;
    flow_req.fr_gen_id = 0;

    struct timeval last_time;
    gettimeofday(&last_time, NULL);

    int i = 0;
    for (i = 0; i < perf; i++) {
        more = false;
        array_index = i;
        flow_req.fr_action = VR_FLOW_ACTION_HOLD;
        flow_req.fr_flow_sport = htons(sport + (i / 65535));
        flow_req.fr_flow_dport = htons(i % 65535);
        flow_req.fr_index = -1;
        flow_make_flow_req_perf(&flow_req);
    }
    cl->cl_buf_offset = 0;

    struct timeval now;
    gettimeofday(&now, NULL);
    int diff_ms;
    diff_ms = (now.tv_sec - last_time.tv_sec) * 1000;
    diff_ms += (now.tv_usec - last_time.tv_usec) / 1000;
    printf("Created %d HOLD entries in %d msec\n", perf, diff_ms);

    gettimeofday(&last_time, NULL);
    for (i = 0; i < perf; i++) {
        array_index = -1;
        flow_req.fr_flow_sip_l = dip;
        flow_req.fr_flow_dip_l = sip;
        flow_req.fr_flow_sport = htons(i % 65535);
        flow_req.fr_flow_dport = htons(sport + (i / 65535));
        flow_req.fr_index = -1;
        flow_req.fr_action = VR_FLOW_ACTION_FORWARD;
        flow_req.fr_gen_id = 0;
        more = true;
        flow_make_flow_req_perf(&flow_req);

        flow_req.fr_flow_sip_l = sip;
        flow_req.fr_flow_dip_l = dip;
        flow_req.fr_flow_sport = htons(sport + (i / 65535));
        flow_req.fr_flow_dport = htons(i % 65535);
        flow_req.fr_action = VR_FLOW_ACTION_FORWARD;
        flow_req.fr_index = flow_md_mem[i].fmd_index;
        flow_req.fr_gen_id = flow_md_mem[i].fmd_gen_id;
        if (i == (perf - 1)) {
            more = false;
        }
        flow_make_flow_req_perf(&flow_req);
    }

    gettimeofday(&now, NULL);
    diff_ms = (now.tv_sec - last_time.tv_sec) * 1000;
    diff_ms += (now.tv_usec - last_time.tv_usec) / 1000;
    printf("Created %d HOLD and %d FWD entries in %d msec\n",
            perf, perf, diff_ms);

}

void
run_flush(void)
{
    int i;
    int diff_ms;
    int count = 0;
    struct vr_flow_entry *fe;
    struct flow_table *ft = &main_table;
    struct timeval now;
    struct timeval last_time;
    vr_flow_req *table = malloc(sizeof(vr_flow_req) * ft->ft_num_entries);

    printf("Scanning flow table\n");
    gettimeofday(&last_time, NULL);
    for (i = 0; i < ft->ft_num_entries; i++) {
        if (fill_flow_req(&table[count], i, 'i', true))
            continue;
        count++;
    }
    gettimeofday(&now, NULL);
    diff_ms = (now.tv_sec - last_time.tv_sec) * 1000;
    diff_ms += (now.tv_usec - last_time.tv_usec) / 1000;
    printf("Found %d entries in %d msec\n", count, diff_ms);

    printf("Deleting %d entries in flow-table\n", count);
    gettimeofday(&last_time, NULL);
    for (i = 0; i < count; i++) {
        flow_make_flow_req(&table[i], "vr_flow_req");
        flow_process_response();
    }

    gettimeofday(&now, NULL);
    diff_ms = (now.tv_sec - last_time.tv_sec) * 1000;
    diff_ms += (now.tv_usec - last_time.tv_usec) / 1000;
    printf("Deleted %d entries in %d msec\n", count, diff_ms);
    free(table);
}

static void
Usage(void)
{
    printf("Usage:flow [-f flow_index]\n");
    printf("           [-d flow_index]\n");
    printf("           [-i flow_index]\n");
    printf("           [-e flow_index]\n");
    printf("           [--sock-dir <netlink socket dir>\n");
    printf("           [--mirror=mirror table index]\n");
    printf("           [--match \"match_string\"\n");
    printf("           [-l]\n");
    printf("           [--show-evicted]\n");
    printf("           [-r]\n");
    printf("           [-s]\n");
    printf("           [-p flow_count]\n");
    printf("           [-b bunch_count]\n");
    printf("           [-F]\n");
    printf("\n");

    printf("-f <flow_index>  Set forward action for flow at flow_index <flow_index>\n");
    printf("-d <flow_index>  Set drop action for flow at flow_index <flow_index>\n");
    printf("-i <flow_index>  Invalidate flow at flow_index <flow_index>\n");
    printf("-e <flow_index>  force evict flow at flow_index <flow_index>\n");
    printf("-p <flow_count>  Profile time to add/delete flow entries\n");
    printf("-b <bunch_count> Bunch flow messages in one netlink message\n");
    printf("-F               Flush all the flows\n");
    printf("--get            Get and print flow entry in a particular index\n");
    printf("                 e.g.: --get <flow_index>\n");
    printf("--mirror         Mirror index to mirror to\n");
    printf("--match          Match criteria separated by a '&'; IP:PORT separated by a ','\n");
    printf("                 e.g.: --match 1.1.1.1:20\n");
    printf("                       --match \"1.1.1.1:20,2.2.2.2:22\"\n");
    printf("                       --match \"[fe80::225:90ff:fec3:afa]:22\"\n");
    printf("                       --match \"10.204.217.10:56910 & vrf 0 & proto tcp\"\n");
    printf("                       --match \"10.204.217.10:56910,169.254.0.3:22 & vrf 0 & proto tcp\"\n");
    printf("                               proto {tcp, udp, icmp, icmp6, sctp}\n");
    printf("-l               List flows\n");
    printf("--show-evicted   Show evicted flows too\n");
    printf("-r               Start dumping flow setup rate\n");
    printf("-s               Start dumping flow stats\n");
    printf("--help           Print this help\n");

    exit(-EINVAL);
}

enum opt_flow_index {
    DVRF_OPT_INDEX,
    GET_OPT_INDEX,
    MIRROR_OPT_INDEX,
    SHOW_EVICTED_OPT_INDEX,
    MATCH_OPT_INDEX,
    HELP_OPT_INDEX,
    FORCE_EVICT_OPT_INDEX,
    SOCK_DIR_OPT_INDEX,
    MAX_OPT_INDEX
};

static struct option long_options[] = {
    [DVRF_OPT_INDEX]            = {"dvrf",          required_argument, &dvrf_set,           1},
    [GET_OPT_INDEX]             = {"get",           required_argument, &get_set,            1},
    [MIRROR_OPT_INDEX]          = {"mirror",        required_argument, &mir_set,            1},
    [SHOW_EVICTED_OPT_INDEX]    = {"show-evicted",  no_argument,       &show_evicted_set,   1},
    [MATCH_OPT_INDEX]           = {"match",         required_argument, &match_set,          1},
    [HELP_OPT_INDEX]            = {"help",          no_argument,       &help_set,           1},
    [FORCE_EVICT_OPT_INDEX]     = {"force-evict",   required_argument, &force_evict_set,    1},
    [SOCK_DIR_OPT_INDEX]        = {"sock-dir",      required_argument, &sock_dir_set,       1},
    [MAX_OPT_INDEX]             = { NULL,           0,                 0,                   0}
};

static void
validate_options(void)
{
    if (!flow_index && !list && !rate && !stats && !match_set
        && !perf && !flush)
        Usage();

    if (show_evicted_set && !list)
        Usage();

    return;
}

static int
flow_set_family(unsigned int family, char *addr, const char *port)
{
    uint8_t ip[VR_IP6_ADDRESS_LEN];
    uint8_t *mem = NULL, mem_size;

    if (match_ip1_set && match_ip2_set && (addr || port)) {
        printf("match: Why do you specify \"[%s]:%s\" when both ends of "
                "the flow are already specified\n", addr, port ? port : NULL);
        return -EINVAL;
    }

    switch (family) {
    case AF_INET:
        mem_size = VR_IP_ADDRESS_LEN;
        if (!vr_valid_ipv4_address(addr))
            return -EINVAL;
        break;

    case AF_INET6:
        mem_size = VR_IP6_ADDRESS_LEN;
        if (!vr_valid_ipv6_address(addr))
            return -EINVAL;
        break;

    default:
        printf("match: Internal logic failure. Family is not one of inet/inet6\n");
        return -EINVAL;
    }

    if (match_family && (match_family != family)) {
        printf("match: You are trying to match v4 and v6 flow together\n");
        printf("match: It does not make sense to me at this point of time\n");

        return -EINVAL;
    }

    if (!match_family) {
        match_family = family;
        match_family_size = mem_size;
    }

    if (strlen(addr) != strlen("*")) {
        inet_pton(family, addr, ip);
        mem = malloc(mem_size);
        if (!mem) {
            printf("match: Memory Allocation failure. Try again\n");
            return -ENOMEM;
        }
        memcpy(mem, ip, mem_size);
    }

    if (!match_ip1_set) {
        match_ip1 = mem;
        if (port) {
            if (strncmp(port, "*", 1))
                match_port1 = strtoul(port, NULL, 0);
        }
        match_ip1_set = true;
    } else if (!match_ip2_set) {
        match_ip2 = mem;
        if (port) {
            if (strncmp(port, "*", 1))
                match_port2 = strtoul(port, NULL, 0);
        }
        match_ip2_set = true;
    }

    return 0;
}

/*
 * Separate out the individual (ip, port) combination
 *
 * For ipv4, the flow will be specified as
 * a.a.a.a:p OR a.a.a.a
 *
 * For ipv6, the corresponding format will be
 * [a:a::a:a]:p OR a:a::a:a
 */
static int
flow_set_tuple(char *ip_port)
{
    unsigned int len = strlen(ip_port);
    unsigned int address_len;

    char *f_colon_sep, *b_colon_sep, *bracket_sep;

    /* for ipv6 addresses starting with '[' */
    bracket_sep = strchr(ip_port, '[');
    if (bracket_sep) {
        /* ...look for closing bracket */
        bracket_sep = strrchr(ip_port, ']');
        if (!bracket_sep) {
            printf("match: No closing ']'\n");
            return -EINVAL;
        }


        address_len = bracket_sep - ip_port + 1;
        /* post ']', we should have a ':' and a port number */
        if (((len - address_len) < 2) ||
                (ip_port[address_len] != ':')) {
            printf("match: match string should be in "
                    "[aa:aa::aa:aa]:p format\n");
            return -EINVAL;
        }

        /* replace the ']' with NULL */
        ip_port[address_len - 1] = '\0';
        /* the address string is already terminated with NULL */
        if (flow_set_family(AF_INET6, ip_port + 1, ip_port + address_len + 1))
            return -EINVAL;

    } else {
        f_colon_sep = strchr(ip_port, ':');
        /*
         * if it is an ipv6 address, we expect to see at least two different
         * ':'
         */
        if (f_colon_sep) {
            b_colon_sep = strrchr(ip_port, ':');
            if (b_colon_sep != f_colon_sep) {
                /* ...hence ipv6 */
                flow_set_family(AF_INET6, ip_port, NULL);
            } else {
                /*
                 * if they are the same, then it has to be v4 and the
                 * ':' is a port separator
                 */
                ip_port[f_colon_sep - ip_port] = '\0';
                if (flow_set_family(AF_INET, ip_port,
                        ip_port + (f_colon_sep - ip_port) + 1))
                    return -EINVAL;
            }
        } else {
            /* ...and if there are no ':', then the address is an ipv4 one */
            if (flow_set_family(AF_INET, ip_port, NULL))
                return -EINVAL;
        }
    }

    return 0;
}

static int
flow_set_ip(char *match_string)
{
    int ret = 0;
    unsigned int length = strlen(match_string), token_length;

    char *token, *string = match_string;

    do {
        token = vr_extract_token(match_string, ',');
        if (token) {
            token_length = strlen(token) + 1;
            /* ...and use it to set the match tuple */
            if (ret = flow_set_tuple(token))
                return ret;
        } else {
            token = vr_extract_token(match_string, '&');
            if (token) {
                token_length = strlen(token) + 1;
                if (ret = flow_set_tuple(token))
                    return ret;
            }
        }

        if (token)
          match_string = token + token_length;

    } while (!ret && token && ((match_string - string) < length));

    return 0;
}

static int
flow_set_vrf(char *string)
{
    if (!strlen(string))
        return -EINVAL;

    errno = 0;
    match_vrf = strtoul(string, NULL, 0);
    if (errno)
        return -errno;

    return 0;
}

static int
flow_set_proto(char *string)
{
    if (!strlen(string))
        return -EINVAL;

    if (!strncmp(string, "tcp", strlen("tcp"))) {
        match_proto = VR_IP_PROTO_TCP;
    } else if (!strncmp(string, "udp", strlen("udp"))) {
        match_proto = VR_IP_PROTO_UDP;
    } else if (!strncmp(string, "icmp6", strlen("icmp6"))) {
        match_proto =  VR_IP_PROTO_ICMP6;
    } else if (!strncmp(string, "icmp", strlen("icmp"))) {
        match_proto = VR_IP_PROTO_ICMP;
    } else if (!strncmp(string, "sctp", strlen("sctp"))) {
        match_proto = VR_IP_PROTO_SCTP;
    } else {
        printf("Unsupported protocol \"%s\"\n", string);
        return -EINVAL;
    }

    return 0;
}

static int
flow_set_match(char *match_string)
{
    int ret = 0;
    unsigned int length = strlen(match_string), token_length;
    char *token, *string = match_string;

    do {
        token = vr_extract_token(match_string, '&');
        if (token) {
            token_length = strlen(token) + 1;
            if (!strncmp(token, "proto", strlen("proto"))) {
                ret = flow_set_proto(token + strlen("proto") + 1);
            } else if (!strncmp(token, "vrf", strlen("vrf"))) {
                ret = flow_set_vrf(token + strlen("vrf") + 1);
                ret = flow_set_vrf(token + strlen("vrf") + 1);
            } else {
                ret = flow_set_ip(token);
            }
            match_string = token + token_length;
        }
    } while (!ret && token && ((match_string - string) < length));

    return ret;
}

static void
parse_long_opts(int opt_flow_index, char *opt_arg)
{
    errno = 0;
    switch (opt_flow_index) {
    case DVRF_OPT_INDEX:
        dvrf = strtoul(opt_arg, NULL, 0);
        if (errno)
            Usage();
        break;

    case GET_OPT_INDEX:
        flow_index = strtoul(opt_arg, NULL, 0);
        if (errno)
            Usage();

        flow_cmd = 'g';
        break;

    case MIRROR_OPT_INDEX:
        mirror = strtoul(opt_arg, NULL, 0);
        if (errno)
            Usage();
        break;

    case MATCH_OPT_INDEX:
        if (flow_set_match(opt_arg))
            exit(-EINVAL);
        list = 1;
        break;

    case SHOW_EVICTED_OPT_INDEX:
        break;

    case FORCE_EVICT_OPT_INDEX:
        flow_index = strtoul(opt_arg, NULL, 0);
        if (errno)
            Usage();

        flow_cmd = 'e';
        break;

    case SOCK_DIR_OPT_INDEX:
        vr_socket_dir = opt_arg;
        break;

    case HELP_OPT_INDEX:
    default:
        Usage();
    }

    return;
}

int
main(int argc, char *argv[])
{
    char opt;
    int ret;
    int option_index;

    flow_fill_nl_callbacks();

    while ((opt = getopt_long(argc, argv, "d:f:g:i:e:p:u:b:lrsF",
                    long_options, &option_index)) >= 0) {
        switch (opt) {
        case 'f':
        case 'g':
        case 'd':
        case 'i':
        case 'e':
            flow_cmd = opt;
            flow_index = strtoul(optarg, NULL, 0);
            break;

        case 'l':
            list = 1;
            break;

        case 'r':
            rate = 1;
            break;

        case 's':
            stats = 1;
            break;

        case 'F':
            flush = 1;
            break;

        case 'p':
            perf = strtoul(optarg, NULL, 0);
            if (perf > MAX_FLOWS) {
                printf("Invalid perf count %d. Max value %d\n", perf,
                       MAX_FLOWS);
                exit(-1);
            }
            break;

        case 'b' :
            bunch = strtoul(optarg, NULL, 0);
            if (bunch < 1) {
                bunch = 1;
            }
            if (bunch > MAX_FLOW_NL_MSG_BUNCH) {
                printf("Max NETLINK messages in a bunch cannot exceed %u.\n",
                        MAX_FLOW_NL_MSG_BUNCH);
                bunch = MAX_FLOW_NL_MSG_BUNCH;
            }
            break;
        case 'u':
            sock_dir_set = 1;
            parse_long_opts(SOCK_DIR_OPT_INDEX, optarg);
            break;

        case 0:
            parse_long_opts(option_index, optarg);
            break;

        default:
            Usage();
        }
    }

    validate_options();

    ret = flow_table_setup();
    if (ret < 0)
        return ret;

    ret = flow_table_get();
    if (ret < 0)
        return ret;

    if (list) {
        flow_list();
    } else if (rate) {
        flow_rate();
    } else if (stats) {
        flow_stats();
    } else if (perf) {
        run_perf();
    } else if (flush) {
        run_flush();
    } else {
        if (flow_index >= main_table.ft_num_entries) {
            printf("Flow index %lu is greater than available indices (%u)\n",
                    flow_index, main_table.ft_num_entries - 1);
            return -1;
        }

        flow_do_op(flow_index, flow_cmd);
    }

    return 0;
}

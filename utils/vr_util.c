/*
 * vr_util.c -- common functions used by utilities in a library form
 *
 * Copyright (c) 2015, Juniper Networks, Inc.
 * All rights reserved
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <fcntl.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_ether.h>
#include <linux/dcbnl.h>
#endif

#include <net/if.h>
#include <netinet/in.h>

#include "vr_types.h"
#include "nl_util.h"
#include "vr_message.h"
#include "vr_genetlink.h"
#include "vr_interface.h"
#include "vr_packet.h"
#include "vr_nexthop.h"
#include "vr_route.h"
#include "vr_bridge.h"
#include "vr_mem.h"
#include "ini_parser.h"

/* Suppress NetLink error messages */
bool vr_ignore_nl_errors = false;
int genetlink_group_id = 0;
static bool vr_header_include = 0;

char *
vr_extract_token(char *string, char token_separator)
{
    int ret;
    unsigned int length;

    char *sep;

    /* skip over leading white spaces */
    while ((*string == ' ') && string++);

    /* if there is nothing left after the spaces, return */
    if (!(length = strlen(string))) {
        return NULL;
    }

    /* If first character is a separator its a malforned request */
    if (*string == token_separator) {
        printf("Malformed request: separator used incorrectly \n");
        return NULL;
    }

    /* start searching for the token */
    sep = strchr(string, token_separator);
    if (sep) {
        length = sep - string;
        /* terminate the token with NULL */
        string[sep - string] = '\0';
        length = strlen(string);
    }

    /* remove trailing spaces */
    length -= 1;
    while ((*(string + length) == ' ') && --length);
    *(string + length + 1) = '\0';

    /*
     * reset the separator to space, since a space at the beginning
     * will be snipped
     */
    if (sep && (((sep - string)) != strlen(string)))
        string[sep - string] = ' ';

    return string;
}

bool
vr_valid_ipv6_address(const char *addr)
{
    unsigned int i = 0, j = 0, sep_count = 0;

    /* a '*' is treated as a valid address */
    if (!strncmp(addr, "*", 1) && (strlen(addr) == 1))
        return true;

    while (*(addr + i)) {
        if (isalnum(*(addr + i))) {
            j++;
        } else if (*(addr + i) == ':') {
            j = 0;
            sep_count++;
        } else {
            printf("match: \"%s\" is not a valid ipv6 address format\n", addr);
            return false;
        }

        if ((j > 4) || (sep_count > 7)) {
            printf("match: \"%s\" is not a valid ipv6 address format\n", addr);
            return false;
        }

        i++;
    }

    return true;
}

bool
vr_valid_ipv4_address(const char *addr)
{
    unsigned int i = 0, j = 0, sep_count = 0;

    /* a '*' is treated as a valid address */
    if (!strncmp(addr, "*", 1) && (strlen(addr) == 1))
        return true;

    /* every character should be either a digit or a '.' */
    while (*(addr + i)) {
        if (isdigit(*(addr + i))) {
            j++;
        } else if (i && (*(addr + i) == '.')) {
            j = 0;
            ++sep_count;
        } else {
            printf("match: \"%s\" is not a valid ipv4 address format\n", addr);
            return false;
        }

        if ((j > 3) || (sep_count > 3)) {
            printf("match: \"%s\" is not a valid ipv4 address format\n", addr);
            return false;
        }

        i++;
    }

    if (sep_count != 3) {
        printf("match: \"%s\" is not a valid ipv4 address format\n", addr);
        return false;
    }

    return true;
}

bool
vr_valid_mac_address(const char *mac)
{
    uint8_t null_mac[VR_ETHER_ALEN] = { 0 };

    if (!mac || !memcmp(mac, null_mac, VR_ETHER_ALEN))
        return false;

    return true;
}

char *
vr_proto_string(unsigned short proto)
{
    switch (proto) {
    case VR_IP_PROTO_TCP:
        return "TCP";
        break;

    case VR_IP_PROTO_UDP:
        return "UDP";
        break;

    case VR_IP_PROTO_ICMP:
        return "ICMP";
        break;

    case VR_IP_PROTO_SCTP:
        return "SCTP";
        break;

    case VR_IP_PROTO_ICMP6:
        return "ICMPv6";
        break;


    default:
        return "UNKNOWN";
    }

    return "UNKNOWN";
}

/* send and receive */
int
vr_recvmsg_generic(struct nl_client *cl, bool dump, bool msg_wait)
{
    int ret = 0;
    bool pending = true;
    struct nl_response *resp;
    struct nlmsghdr *nlh;

    while (pending) {
        if (msg_wait) {
            ret = nl_recvmsg_waitall(cl);
        } else {
            ret = nl_recvmsg(cl);
        }
        if (ret > 0) {
            if (dump) {
                pending = true;
            } else {
                pending = false;
            }

            resp = nl_parse_reply(cl);
            if (resp->nl_op == SANDESH_REQUEST) {
                sandesh_decode(resp->nl_data, resp->nl_len,
                        vr_find_sandesh_info, &ret);
            } else if (resp->nl_type == NL_MSG_TYPE_DONE) {
                pending = false;
            }
        } else {
            return ret;
        }

        nlh = (struct nlmsghdr *)cl->cl_buf;
        if (!nlh || !nlh->nlmsg_flags)
            break;
    }

    return ret;
}

int
vr_recvmsg(struct nl_client *cl, bool dump)
{
    return vr_recvmsg_generic(cl, dump, false);
}

int
vr_recvmsg_waitall(struct nl_client *cl, bool dump)
{
    return vr_recvmsg_generic(cl, dump, true);
}

int
vr_sendmsg(struct nl_client *cl, void *request,
        char *request_string)
{
    int ret, error, attr_len;

    /* nlmsg header */
    ret = nl_build_nlh(cl, cl->cl_genl_family_id, NLM_F_REQUEST);
    if (ret)
        return ret;

    /* Generic nlmsg header */
    ret = nl_build_genlh(cl, SANDESH_REQUEST, 0);
    if (ret)
        return ret;

    attr_len = nl_get_attr_hdr_size();
    ret = sandesh_encode(request, request_string, vr_find_sandesh_info,
                             (nl_get_buf_ptr(cl) + attr_len),
                             (nl_get_buf_len(cl) - attr_len), &error);
    if (ret <= 0)
        return ret;

    /* Add sandesh attribute */
    nl_build_attr(cl, ret, NL_ATTR_VR_MESSAGE_PROTOCOL);
    nl_update_nlh(cl);

    return nl_sendmsg(cl);
}

struct nl_client *
vr_get_nl_client(int proto)
{
    int ret;
    int sock_proto = proto;
    struct nl_client *cl;

    cl = nl_register_client();
    if (!cl)
        return NULL;

    parse_ini_file();

    if (proto == VR_NETLINK_PROTO_DEFAULT)
        sock_proto = get_protocol();

    ret = nl_socket(cl, get_domain(), get_type(), sock_proto);
    if (ret <= 0)
        goto fail;

    ret = nl_connect(cl, get_ip(), get_port(), genetlink_group_id);
    if (ret < 0)
        goto fail;

    if ((proto == VR_NETLINK_PROTO_DEFAULT) &&
          (get_platform() != VTEST_PLATFORM) &&
            (vrouter_obtain_family_id(cl) <= 0))
        goto fail;

    return cl;

fail:
    if (cl)
        nl_free_client(cl);

    return NULL;
}

int
vr_send_get_bridge_table_data(struct nl_client *cl)
{
    int ret;
    vr_bridge_table_data req;

    memset(&req, 0, sizeof(req));
    req.btable_op = SANDESH_OP_GET;
    req.btable_rid = 0;

    return vr_sendmsg(cl, &req, "vr_bridge_table_data");
}

int
vr_send_set_dcb_state(struct nl_client *cl, uint8_t *ifname, uint8_t state)
{
    int ret;

    ret = nl_build_set_dcb_state_msg(cl, ifname, state);
    if (ret < 0)
        return ret;

    ret = nl_dcb_sendmsg(cl, DCB_CMD_SSTATE, NULL);
    if (ret <= 0)
        return ret;

    if (ret != state) {
        printf("vRouter: Set DCB State failed (Req/Resp: %u/%d)\n",
                state, ret);
        return -1;
    }

    return 0;
}

int
vr_send_get_dcb_state(struct nl_client *cl, uint8_t *ifname)
{
    int ret;

    ret = nl_build_get_dcb_state_msg(cl, ifname);
    if (ret < 0)
        return ret;

    return nl_dcb_sendmsg(cl, DCB_CMD_GSTATE, NULL);
}

int
vr_send_set_dcbx(struct nl_client *cl, uint8_t *ifname, uint8_t dcbx)
{
    int ret;

    ret = nl_build_set_dcbx(cl, ifname, dcbx);
    if (ret < 0)
        return ret;

    ret = nl_dcb_sendmsg(cl, DCB_CMD_SDCBX, NULL);
    if (ret < 0)
        return ret;

    if (ret) {
        printf("vRouter: Set DCBX failed (Req/Resp: %u/%d)\n",
            dcbx, ret);
        return -1;
    }

    return 0;
}

int
vr_send_get_dcbx(struct nl_client *cl, uint8_t *ifname)
{
    int ret;

    ret = nl_build_get_dcbx(cl, ifname);
    if (ret < 0)
        return ret;

    return nl_dcb_sendmsg(cl, DCB_CMD_GDCBX, NULL);
}

int
vr_send_get_priority_config(struct nl_client *cl, uint8_t *ifname,
        struct priority *p)
{
    int ret;

    ret = nl_build_get_priority_config_msg(cl, ifname);
    if (ret < 0)
        return ret;

    ret = nl_dcb_sendmsg(cl, DCB_CMD_PGTX_GCFG, p);
    if (ret < 0)
        return ret;

    return 0;
}

int
vr_send_set_priority_config(struct nl_client *cl, uint8_t *ifname,
        struct priority *p)
{
    int ret;

    ret = nl_build_set_priority_config_msg(cl, ifname, p);
    if (ret < 0)
        return ret;

    ret = nl_dcb_sendmsg(cl, DCB_CMD_PGTX_SCFG, NULL);
    if (ret < 0)
        return ret;

    return 0;
}

int
vr_send_set_dcb_all(struct nl_client *cl, uint8_t *ifname)
{
    int ret;

    ret = nl_build_set_dcb_all(cl, ifname);
    if (ret < 0)
        return ret;

    return nl_dcb_sendmsg(cl, DCB_CMD_SET_ALL, NULL);
}

int
vr_send_get_ieee_ets(struct nl_client *cl, uint8_t *ifname,
        struct priority *p)
{
    int ret;

    ret = nl_build_get_ieee_ets(cl, ifname, p);
    if (ret < 0)
        return ret;

    return nl_dcb_sendmsg(cl, DCB_CMD_IEEE_GET, p);
}

int
vr_send_set_ieee_ets(struct nl_client *cl, uint8_t *ifname,
        struct priority *p)
{
    int ret;

    ret = nl_build_set_ieee_ets(cl, ifname, p);
    if (ret < 0)
        return ret;

    return nl_dcb_sendmsg(cl, DCB_CMD_IEEE_SET, NULL);
}

void
vr_print_drop_stats(vr_drop_stats_req *stats, int core)
{
    int platform = get_platform();

    if (core != (unsigned)-1)
        printf("Statistics for core %u\n\n", core);

    if (stats->vds_pcpu_stats_failure_status)
       printf("Failed to maintain PerCPU stats for this interface\n\n");

    PRINT_DROP_STAT("Invalid IF", stats->vds_invalid_if);
    PRINT_DROP_STAT("Invalid ARP", stats->vds_invalid_arp);
    PRINT_DROP_STAT("Trap No IF", stats->vds_trap_no_if);
    PRINT_DROP_STAT("IF TX Discard", stats->vds_interface_tx_discard);
    PRINT_DROP_STAT("IF Drop", stats->vds_interface_drop);
    PRINT_DROP_STAT("IF RX Discard", stats->vds_interface_rx_discard);
    PRINT_DROP_STAT("Flow Unusable", stats->vds_flow_unusable);
    PRINT_DROP_STAT("Flow No Memory",stats->vds_flow_no_memory);
    PRINT_DROP_STAT("Flow Table Full", stats->vds_flow_table_full);
    PRINT_DROP_STAT("Flow NAT no rflow", stats->vds_flow_nat_no_rflow);
    PRINT_DROP_STAT("Flow Action Drop", stats->vds_flow_action_drop);
    PRINT_DROP_STAT("Flow Action Invalid", stats->vds_flow_action_invalid);
    PRINT_DROP_STAT("Flow Invalid Protocol", stats->vds_flow_invalid_protocol);
    PRINT_DROP_STAT("Flow Queue Limit Exceeded", stats->vds_flow_queue_limit_exceeded);
    PRINT_DROP_STAT("New Flow Drops", stats->vds_drop_new_flow);
    PRINT_DROP_STAT("Flow Unusable (Eviction)", stats->vds_flow_evict);
    PRINT_DROP_STAT("Original Packet Trapped", stats->vds_trap_original);
    PRINT_DROP_STAT("Discards", stats->vds_discard);
    PRINT_DROP_STAT("TTL Exceeded", stats->vds_ttl_exceeded);
    PRINT_DROP_STAT("Mcast Clone Fail", stats->vds_mcast_clone_fail);
    PRINT_DROP_STAT("Invalid NH", stats->vds_invalid_nh);
    PRINT_DROP_STAT("Invalid Label", stats->vds_invalid_label);
    PRINT_DROP_STAT("Invalid Protocol", stats->vds_invalid_protocol);
    PRINT_DROP_STAT("Etree Leaf to Leaf", stats->vds_leaf_to_leaf);
    PRINT_DROP_STAT("Bmac/ISID Mismatch", stats->vds_bmac_isid_mismatch);
    PRINT_DROP_STAT("Rewrite Fail", stats->vds_rewrite_fail);
    PRINT_DROP_STAT("Invalid Mcast Source", stats->vds_invalid_mcast_source);
    PRINT_DROP_STAT("Packet Loop", stats->vds_pkt_loop);
    PRINT_DROP_STAT("Push Fails", stats->vds_push);
    PRINT_DROP_STAT("Pull Fails", stats->vds_pull);
    PRINT_DROP_STAT("Duplicate", stats->vds_duplicated);
    PRINT_DROP_STAT("Head Alloc Fails", stats->vds_head_alloc_fail);
    PRINT_DROP_STAT("PCOW fails", stats->vds_pcow_fail);
    PRINT_DROP_STAT("Invalid Packets", stats->vds_invalid_packet);
    PRINT_DROP_STAT("Misc", stats->vds_misc);
    PRINT_DROP_STAT("Nowhere to go", stats->vds_nowhere_to_go);
    PRINT_DROP_STAT("Checksum errors", stats->vds_cksum_err);
    PRINT_DROP_STAT("No Fmd", stats->vds_no_fmd);
    PRINT_DROP_STAT("Invalid VNID", stats->vds_invalid_vnid);
    PRINT_DROP_STAT("Fragment errors", stats->vds_frag_err);
    PRINT_DROP_STAT("Invalid Source", stats->vds_invalid_source);
    PRINT_DROP_STAT("Jumbo Mcast Pkt with DF Bit", stats->vds_mcast_df_bit);
    PRINT_DROP_STAT("No L2 Route", stats->vds_l2_no_route);
    PRINT_DROP_STAT("Memory Failures", stats->vds_no_memory);
    PRINT_DROP_STAT("Fragment Queueing Failures", stats->vds_fragment_queue_fail);
    PRINT_DROP_STAT("No Encrypt Path Failures", stats->vds_no_crypt_path);
    PRINT_DROP_STAT("Invalid HBS received packet", stats->vds_invalid_hbs_pkt);
    PRINT_DROP_STAT("No Fragment Entries", stats->vds_no_frag_entry);
    PRINT_DROP_STAT("ICMP errors", stats->vds_icmp_error);
    PRINT_DROP_STAT("Clone Failures", stats->vds_clone_fail);
    PRINT_DROP_STAT("Invalid underlay ECMP", stats->vds_invalid_underlay_ecmp);

    if (platform == DPDK_PLATFORM)
    {
        PRINT_DROP_STAT("VLAN fwd intf failed TX", stats->vds_vlan_fwd_tx);
        PRINT_DROP_STAT("VLAN fwd intf failed enq", stats->vds_vlan_fwd_enq);
    }
    return;
}

void
vr_print_drop_dbg_stats(vr_drop_stats_req *stats, int core)
{
    printf("Cloned Original               %" PRIu64 "\n",
            stats->vds_cloned_original);
    return;
}

const char *
vr_pkt_vp_type_rsn(unsigned char vp_type)
{
    switch(vp_type) {
    case VP_TYPE_ARP:    return "ARP";
    case VP_TYPE_IP :    return "IP";
    case VP_TYPE_IP6:    return "IP6";
    case VP_TYPE_IPOIP:  return "IPOIP";
    case VP_TYPE_IP6OIP: return "IP6OIP";
    case VP_TYPE_AGENT:  return "AGENT";
    case VP_TYPE_PBB:    return "PBB";
    default:             return "UNKNOWN";
    }
}

const char *
vr_pkt_droplog_rsn(unsigned short drop_reason)
{
    switch(drop_reason) {
    case VP_DROP_DISCARD:
        return "Discards";
    case VP_DROP_PULL:
        return "Pull Fails";
    case VP_DROP_INVALID_IF:
        return "Invalid IF";
    case VP_DROP_INVALID_ARP:
        return "Invalid ARP";
    case VP_DROP_TRAP_NO_IF:
        return "Trap No IF";
    case VP_DROP_NOWHERE_TO_GO:
        return "Nowhere to go";
    case VP_DROP_FLOW_QUEUE_LIMIT_EXCEEDED:
        return "Flow Queue Limit Exceeded";
    case VP_DROP_FLOW_NO_MEMORY:
        return "Flow No Memory";
    case VP_DROP_FLOW_INVALID_PROTOCOL:
        return "Flow Invalid Protocol";
    case VP_DROP_FLOW_NAT_NO_RFLOW:
        return "Flow NAT no rflow";
    case VP_DROP_FLOW_ACTION_DROP:
        return "Flow Action Drop";
    case VP_DROP_FLOW_ACTION_INVALID:
        return "Flow Action Invalid";
    case VP_DROP_FLOW_UNUSABLE:
        return "Flow Unusable";
    case VP_DROP_FLOW_TABLE_FULL:
        return "Flow Table Full";
    case VP_DROP_INTERFACE_TX_DISCARD:
        return "IF TX Discard";
    case VP_DROP_INTERFACE_DROP:
        return "IF Drop";
    case VP_DROP_DUPLICATED:
        return "Duplicate";
    case VP_DROP_PUSH:
        return "Push Fails";
    case VP_DROP_TTL_EXCEEDED:
        return "TTL Exceeded";
    case VP_DROP_INVALID_NH:
        return "Invalid NH";
    case VP_DROP_INVALID_LABEL:
        return "Invalid Label";
    case VP_DROP_INVALID_PROTOCOL:
        return "Invalid Protocol";
    case VP_DROP_INTERFACE_RX_DISCARD:
        return "IF RX Discard";
    case VP_DROP_INVALID_MCAST_SOURCE:
        return "Invalid Mcast Source";
    case VP_DROP_HEAD_ALLOC_FAIL:
        return "Head Alloc Fails";
    case VP_DROP_PCOW_FAIL:
        return "PCOW fails";
    case VP_DROP_MCAST_DF_BIT:
        return "Jumbo Mcast Pkt with DF Bit";
    case VP_DROP_MCAST_CLONE_FAIL:
        return "Mcast Clone Fail";
    case VP_DROP_NO_MEMORY:
        return "Memory Failures";
    case VP_DROP_REWRITE_FAIL:
        return "Rewrite Fail";
    case VP_DROP_MISC:
        return "Misc";
    case VP_DROP_INVALID_PACKET:
        return "Invalid Packets";
    case VP_DROP_CKSUM_ERR:
        return "Checksum errors";
    case VP_DROP_NO_FMD:
        return "No Fmd";
    case VP_DROP_CLONED_ORIGINAL:
        return "Cloned Original";
    case VP_DROP_INVALID_VNID:
        return "Invalid VNID";
    case VP_DROP_FRAGMENTS:
        return "Fragment errors";
    case VP_DROP_INVALID_SOURCE:
        return "Invalid Source";
    case VP_DROP_L2_NO_ROUTE:
        return "No L2 Route";
    case VP_DROP_FRAGMENT_QUEUE_FAIL:
        return "Fragment Queueing Failures";
    case VP_DROP_VLAN_FWD_TX:
        return "VLAN fwd intf failed TX";
    case VP_DROP_VLAN_FWD_ENQ:
        return "VLAN fwd intf failed enq";
    case VP_DROP_NEW_FLOWS:
        return "New Flow Drops";
    case VP_DROP_FLOW_EVICT:
        return "Flow Unusable (Eviction)";
    case VP_DROP_TRAP_ORIGINAL:
        return "Original Packet Trapped";
    case VP_DROP_LEAF_TO_LEAF:
        return "Etree Leaf to Leaf";
    case VP_DROP_BMAC_ISID_MISMATCH:
        return "Bmac/ISID Mismatch";
    case VP_DROP_PKT_LOOP:
        return "Packet Loop";
    case VP_DROP_NO_CRYPT_PATH:
        return "No Encrypt Path Failures";
    case VP_DROP_INVALID_HBS_PKT:
        return "Invalid HBS received packet";
    case VP_DROP_NO_FRAG_ENTRY:
        return "No Fragment Entries";
    case VP_DROP_ICMP_ERROR:
        return "ICMP errors";
    case VP_DROP_CLONE_FAIL:
        return "Clone Failures";
    case VP_DROP_INVALID_UNDERLAY_ECMP:
        return "Invalid underlay ECMP";
    default:
        return "Unknow";
    }

}
void vr_print_pkt_drop_log_data(vr_pkt_drop_log_req *pkt_log, int i,
                                uint8_t show_pkt_drop_type)
{
    int j = 0;
    vr_pkt_drop_log_t *pkt_log_utils =
        (vr_pkt_drop_log_t*)pkt_log->vdl_pkt_droplog_arr;
    struct tm *ptr_time;
    char ipv6_addr[INET6_ADDRSTRLEN] = "";

    /* String mapping for packet drop log */
    char vr_pkt_droplog_str[][50] = {
        FILE_MAP(string)
    };

    if (!pkt_log_utils[i].vp_type)
        return;

    if ((show_pkt_drop_type != VP_DROP_MAX) &&
        (show_pkt_drop_type != pkt_log_utils[i].drop_reason))
        return;

    if((pkt_log->vdl_log_idx+i < VR_PKT_DROP_LOG_MAX) &&
        (!vr_header_include))
    {
        vr_print_pkt_drop_log_header(pkt_log);
        vr_header_include = 1;
    }

    ptr_time = localtime(&(pkt_log_utils[i].timestamp));

    printf("sl no: %d  ", pkt_log->vdl_log_idx+i);
    printf("Epoch Time: %ld ", pkt_log_utils[i].timestamp);
    printf("Local Time: %s ", asctime(ptr_time));
    printf("Packet Type: %s  ", vr_pkt_vp_type_rsn(pkt_log_utils[i].vp_type));
    if(pkt_log_utils[i].drop_reason)
        printf("Drop reason: %s  ", vr_pkt_droplog_rsn(pkt_log_utils[i].drop_reason));
    PRINT_PKT_LOG("Vif idx:",    pkt_log_utils[i].vif_idx);
    PRINT_PKT_LOG("Nexthop id:", pkt_log_utils[i].nh_id);
    if(pkt_log_utils[i].vp_type == VP_TYPE_IP)
    {
        printf("Src IP: %s  ", inet_ntoa(pkt_log_utils[i].src.ipv4));
        printf("Dst IP: %s  ", inet_ntoa(pkt_log_utils[i].dst.ipv4));
    }
    else if (pkt_log_utils[i].vp_type == VP_TYPE_IP6)
    {
        inet_ntop(AF_INET6, &pkt_log_utils[i].src.ipv6, ipv6_addr, INET6_ADDRSTRLEN);
        printf("Src IPv6: %s  ", ipv6_addr);
        inet_ntop(AF_INET6, &pkt_log_utils[i].dst.ipv6, ipv6_addr, INET6_ADDRSTRLEN);
        printf("Dst IPv6: %s  ", ipv6_addr);
    }

    PRINT_PKT_LOG("Source port:", pkt_log_utils[i].sport);
    PRINT_PKT_LOG("Dest port:",   pkt_log_utils[i].dport);
    if(pkt_log_utils[i].drop_loc.file)
        printf("file: %s  ", vr_pkt_droplog_str[pkt_log_utils[i].drop_loc.file]);
    printf("line no: %d  ", pkt_log_utils[i].drop_loc.line);

    if(pkt_log_utils[i].pkt_len)
    {
        printf("Packet Length: %d  ", pkt_log_utils[i].pkt_len);
        printf("Packet Data: ");

        if(pkt_log_utils[i].pkt_len > 100)
            for(j = 0; j < 100; j++)
                printf("%02X  ", pkt_log_utils[i].pkt_header[j]);
        else
            for(j=0;j<pkt_log_utils[i].pkt_len;j++)
                printf("%02X  ", pkt_log_utils[i].pkt_header[j]);
    }
    printf("\n\n");
}
void vr_print_pkt_drop_log_header(vr_pkt_drop_log_req *pkt_log)
{
    printf("**********PKT DROP LOG**********\n");
    printf("Total No. of CPU's %d\n", pkt_log->vdl_max_num_cores);
    /* When requested for core 0, it will try to log for all cores
     * so here manually printing as core 1 */
    if(pkt_log->vdl_core == 0)
        printf("Pkt Drop Log for Core 1\n\n");
    else
        printf("Pkt Drop Log for Core %d\n\n", pkt_log->vdl_core);
}
void
vr_print_pkt_drop_log(vr_pkt_drop_log_req *pkt_log, uint8_t show_pkt_drop_type)
{
    int i = 0, log_buffer_iter = 0;
    static bool vr_header_include = 0;

    /* When configured pkt buffer size is than MAX_ALLOWED_BUFFER_SIZE */
    if(pkt_log->vdl_pkt_droplog_max_bufsz - pkt_log->vdl_log_idx  <
            VR_PKT_DROPLOG_MAX_ALLOW_BUFSZ)
    {
        log_buffer_iter = pkt_log->vdl_pkt_droplog_max_bufsz - pkt_log->vdl_log_idx;
    }
    else
        log_buffer_iter = VR_PKT_DROPLOG_MAX_ALLOW_BUFSZ;

    for(i = 0; i < log_buffer_iter; i++)
        vr_print_pkt_drop_log_data(pkt_log, i, show_pkt_drop_type);

    /* On Every VR_PKT_DROP_LOG_MAX count time need to check this
     * flag to print PKT DROP header
     */
    if((pkt_log->vdl_log_idx + log_buffer_iter == VR_PKT_DROP_LOG_MAX) &&
        (vr_header_include))
    {
        vr_header_include = 0;
    }
    return;
}

int
vr_response_common_process(vr_response *resp, bool *dump_pending)
{
    int ret = 0;

    if (dump_pending)
        *dump_pending = false;

    if (resp->resp_code < 0) {
        if (!vr_ignore_nl_errors) {
            printf("vRouter(Response): %s (%d)\n", strerror(-resp->resp_code),
                    -resp->resp_code);
        }
        ret = resp->resp_code;
    } else {
        if ((resp->resp_code & VR_MESSAGE_DUMP_INCOMPLETE) &&
                dump_pending)
            *dump_pending = true;
    }

    return ret;
}

/* dropstats start */
uint64_t
vr_sum_drop_stats(vr_drop_stats_req *req)
{
    uint64_t sum = 0;

    sum += req->vds_discard;
    sum += req->vds_pull;
    sum += req->vds_invalid_if;
    sum += req->vds_invalid_arp;
    sum += req->vds_trap_no_if;
    sum += req->vds_nowhere_to_go;
    sum += req->vds_flow_queue_limit_exceeded;
    sum += req->vds_flow_no_memory;
    sum += req->vds_flow_invalid_protocol;
    sum += req->vds_flow_nat_no_rflow;
    sum += req->vds_flow_action_drop;
    sum += req->vds_flow_action_invalid;
    sum += req->vds_flow_unusable;
    sum += req->vds_flow_table_full;
    sum += req->vds_interface_tx_discard;
    sum += req->vds_interface_drop;
    sum += req->vds_duplicated;
    sum += req->vds_push;
    sum += req->vds_ttl_exceeded;
    sum += req->vds_invalid_nh;
    sum += req->vds_invalid_label;
    sum += req->vds_invalid_protocol;
    sum += req->vds_interface_rx_discard;
    sum += req->vds_invalid_mcast_source;
    sum += req->vds_head_alloc_fail;
    sum += req->vds_pcow_fail;
    sum += req->vds_mcast_df_bit;
    sum += req->vds_mcast_clone_fail;
    sum += req->vds_no_memory;
    sum += req->vds_rewrite_fail;
    sum += req->vds_misc;
    sum += req->vds_invalid_packet;
    sum += req->vds_cksum_err;
    sum += req->vds_no_fmd;
    sum += req->vds_cloned_original;
    sum += req->vds_invalid_vnid;
    sum += req->vds_frag_err;
    sum += req->vds_invalid_source;
    sum += req->vds_l2_no_route;
    sum += req->vds_fragment_queue_fail;
    sum += req->vds_vlan_fwd_tx;
    sum += req->vds_vlan_fwd_enq;
    sum += req->vds_drop_new_flow;
    sum += req->vds_trap_original;
    sum += req->vds_pkt_loop;
    sum += req->vds_no_crypt_path;
    sum += req->vds_invalid_underlay_ecmp;

    return sum;
}

void
vr_drop_stats_req_destroy(vr_drop_stats_req *req)
{
    if (!req)
        return;

    free(req);
    return;
}

vr_drop_stats_req *
vr_drop_stats_req_get_copy(vr_drop_stats_req *src)
{
    vr_drop_stats_req *dst;

    if (!src)
        return NULL;

    dst = malloc(sizeof(*dst));
    if (!dst)
        return NULL;

    *dst = *src;
    return dst;
}

int
vr_send_drop_stats_get(struct nl_client *cl, unsigned int router_id,
        short core)
{
    vr_drop_stats_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.vds_rid = router_id;
    req.vds_core = core;

    return vr_sendmsg(cl, &req, "vr_drop_stats_req");
}


int
vr_send_info_dump(struct nl_client *cl, unsigned int router_id,
        int marker, int buff_table_id, vr_info_msg_en msginfo, int buffsz,
        uint8_t *vr_info_inbuf)
{
    vr_info_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.vdu_rid = router_id;
    req.vdu_marker = marker;
    req.vdu_buff_table_id = buff_table_id;
    req.vdu_msginfo = msginfo;
    req.vdu_outbufsz = buffsz;
    if(vr_info_inbuf != NULL) {
        req.vdu_inbuf_size = strlen(vr_info_inbuf);
        req.vdu_inbuf = vr_info_inbuf;
    }
    return vr_sendmsg(cl, &req, "vr_info_req");
}

int
vr_pkt_drop_log_reset(struct nl_client *cl)
{
    vr_pkt_drop_log_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_RESET;

    return vr_sendmsg(cl, &req, "vr_pkt_drop_log_req");
}

int
vr_drop_stats_reset(struct nl_client *cl)
{
    vr_drop_stats_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_RESET;

    return vr_sendmsg(cl, &req, "vr_drop_stats_req");
}

int
vr_drop_type_set(struct nl_client *cl, uint8_t pkt_drop_log_type)
{
    vr_pkt_drop_log_req req;

    memset(&req, 0, sizeof(req));
    req.h_op                 = SANDESH_OP_ADD;
    req.vdl_pkt_droplog_type = pkt_drop_log_type;

    /* this flag is to indentify bettwn min log config and drop type set
       if 1 used for drop type config
       if 0 used for min log config
    */
    req.vdl_pkt_droplog_config = true;

    return vr_sendmsg(cl, &req, "vr_pkt_drop_log_req");
}

int
vr_min_log_enable(struct nl_client *cl, bool min_log)
{
    vr_pkt_drop_log_req req;

    memset(&req, 0, sizeof(req));
    req.h_op                          = SANDESH_OP_ADD;
    req.vdl_pkt_droplog_min_sysctl_en = min_log;

    /* this flag is to indentify between min log config and drop type set
       if 1 used for drop type config
       if 0 used for min log config
    */
    req.vdl_pkt_droplog_config = false;
    return vr_sendmsg(cl, &req, "vr_pkt_drop_log_req");
}

int vr_pkt_drop_log_request(struct nl_client *cl, unsigned int router_id,
        unsigned int core, int log_idx )
{
    int ret = 0;
    vr_pkt_drop_log_req req;

    memset(&req, 0, sizeof(req));

    req.vdl_pkt_droplog_arr = (char *)malloc(1);

    req.h_op = SANDESH_OP_GET;
    req.vdl_rid = router_id;
    req.vdl_core = core;
    req.vdl_log_idx = log_idx;

    ret =  vr_sendmsg(cl, &req, "vr_pkt_drop_log_req");

    free(req.vdl_pkt_droplog_arr);

    return ret;
}
/* dropstats end */

/* Interface start */
void
vr_interface_req_destroy(vr_interface_req *req)
{
    if (!req)
        return;

    if (req->vifr_name) {
        free(req->vifr_name);
        req->vifr_name = NULL;
    }

    if (req->vifr_queue_ierrors_to_lcore_size &&
            req->vifr_queue_ierrors_to_lcore) {
        free(req->vifr_queue_ierrors_to_lcore);
        req->vifr_queue_ierrors_to_lcore = NULL;
        req->vifr_queue_ierrors_to_lcore_size = 0;
    }


    if (req->vifr_mac && req->vifr_mac_size) {
        free(req->vifr_mac);
        req->vifr_mac = NULL;
        req->vifr_mac_size = 0;
    }

    if (req->vifr_src_mac && req->vifr_src_mac_size) {
        free(req->vifr_src_mac);
        req->vifr_src_mac = NULL;
        req->vifr_src_mac_size = 0;
    }

    if (req->vifr_fat_flow_protocol_port_size &&
            req->vifr_fat_flow_protocol_port) {
        free(req->vifr_fat_flow_protocol_port);
        req->vifr_fat_flow_protocol_port = NULL;
        req->vifr_fat_flow_protocol_port_size = 0;
    }

    if (req->vifr_fat_flow_src_prefix_h &&
        req->vifr_fat_flow_src_prefix_h_size) {
        free(req->vifr_fat_flow_src_prefix_h);
        req->vifr_fat_flow_src_prefix_h = NULL;
        req->vifr_fat_flow_src_prefix_h_size = 0;
    }
    if (req->vifr_fat_flow_src_prefix_l &&
        req->vifr_fat_flow_src_prefix_l_size) {
        free(req->vifr_fat_flow_src_prefix_l);
        req->vifr_fat_flow_src_prefix_l = NULL;
        req->vifr_fat_flow_src_prefix_l_size = 0;
    }
    if (req->vifr_fat_flow_src_prefix_mask &&
        req->vifr_fat_flow_src_prefix_mask_size) {
        free(req->vifr_fat_flow_src_prefix_mask);
        req->vifr_fat_flow_src_prefix_mask = NULL;
        req->vifr_fat_flow_src_prefix_mask_size = 0;
    }
    if (req->vifr_fat_flow_src_aggregate_plen &&
        req->vifr_fat_flow_src_aggregate_plen_size) {
        free(req->vifr_fat_flow_src_aggregate_plen);
        req->vifr_fat_flow_src_aggregate_plen = NULL;
        req->vifr_fat_flow_src_aggregate_plen_size = 0;
    }
    if (req->vifr_fat_flow_dst_prefix_h &&
        req->vifr_fat_flow_dst_prefix_h_size) {
        free(req->vifr_fat_flow_dst_prefix_h);
        req->vifr_fat_flow_dst_prefix_h = NULL;
        req->vifr_fat_flow_dst_prefix_h_size = 0;
    }
    if (req->vifr_fat_flow_dst_prefix_l &&
        req->vifr_fat_flow_dst_prefix_l_size) {
        free(req->vifr_fat_flow_dst_prefix_l);
        req->vifr_fat_flow_dst_prefix_l = NULL;
        req->vifr_fat_flow_dst_prefix_l_size = 0;
    }
    if (req->vifr_fat_flow_dst_prefix_mask &&
        req->vifr_fat_flow_dst_prefix_mask_size) {
        free(req->vifr_fat_flow_dst_prefix_mask);
        req->vifr_fat_flow_dst_prefix_mask = NULL;
        req->vifr_fat_flow_dst_prefix_mask_size = 0;
    }
    if (req->vifr_fat_flow_dst_aggregate_plen &&
        req->vifr_fat_flow_dst_aggregate_plen_size) {
        free(req->vifr_fat_flow_dst_aggregate_plen);
        req->vifr_fat_flow_dst_aggregate_plen = NULL;
        req->vifr_fat_flow_dst_aggregate_plen_size = 0;
    }

    if (req->vifr_fat_flow_exclude_ip_list_size &&
           req->vifr_fat_flow_exclude_ip_list) {
        free(req->vifr_fat_flow_exclude_ip_list);
        req->vifr_fat_flow_exclude_ip_list = NULL;
        req->vifr_fat_flow_exclude_ip_list_size = 0;
    }
    if (req->vifr_fat_flow_exclude_ip6_u_list_size &&
           req->vifr_fat_flow_exclude_ip6_u_list) {
        free(req->vifr_fat_flow_exclude_ip6_u_list);
        req->vifr_fat_flow_exclude_ip6_u_list = NULL;
        req->vifr_fat_flow_exclude_ip6_u_list_size = 0;
    }
    if (req->vifr_fat_flow_exclude_ip6_l_list_size &&
           req->vifr_fat_flow_exclude_ip6_l_list) {
        free(req->vifr_fat_flow_exclude_ip6_l_list);
        req->vifr_fat_flow_exclude_ip6_l_list = NULL;
        req->vifr_fat_flow_exclude_ip6_l_list_size = 0;
    }

    free(req);
    return;
}


vr_interface_req *
vr_interface_req_get_copy(vr_interface_req *src)
{
    vr_interface_req *dst;

    dst = malloc(sizeof(*dst));
    if (!dst)
        return NULL;

    *dst = *src;
    dst->vifr_name = NULL;
    dst->vifr_queue_ierrors_to_lcore_size = 0;
    dst->vifr_queue_ierrors_to_lcore = NULL;
    dst->vifr_mac_size = 0;
    dst->vifr_mac = NULL;
    dst->vifr_src_mac_size = 0;
    dst->vifr_src_mac = NULL;
    dst->vifr_fat_flow_protocol_port_size = 0;
    dst->vifr_fat_flow_protocol_port = NULL;
    dst->vifr_fat_flow_src_prefix_h = NULL;
    dst->vifr_fat_flow_src_prefix_h_size = 0;
    dst->vifr_fat_flow_src_prefix_l = NULL;
    dst->vifr_fat_flow_src_prefix_l_size = 0;
    dst->vifr_fat_flow_src_prefix_mask = NULL;
    dst->vifr_fat_flow_src_prefix_mask_size = 0;
    dst->vifr_fat_flow_src_aggregate_plen = NULL;
    dst->vifr_fat_flow_src_aggregate_plen_size = 0;
    dst->vifr_fat_flow_dst_prefix_h = NULL;
    dst->vifr_fat_flow_dst_prefix_h_size = 0;
    dst->vifr_fat_flow_dst_prefix_l = NULL;
    dst->vifr_fat_flow_dst_prefix_l_size = 0;
    dst->vifr_fat_flow_dst_prefix_mask = NULL;
    dst->vifr_fat_flow_dst_prefix_mask_size = 0;
    dst->vifr_fat_flow_dst_aggregate_plen = NULL;
    dst->vifr_fat_flow_dst_aggregate_plen_size = 0;
    dst->vifr_fat_flow_exclude_ip_list_size = 0;
    dst->vifr_fat_flow_exclude_ip_list = NULL;
    dst->vifr_fat_flow_exclude_ip6_u_list_size = 0;
    dst->vifr_fat_flow_exclude_ip6_u_list = NULL;
    dst->vifr_fat_flow_exclude_ip6_l_list_size = 0;
    dst->vifr_fat_flow_exclude_ip6_l_list = NULL;

    if (src->vifr_name) {
        dst->vifr_name = malloc(strlen(src->vifr_name) + 1);
        if (!dst->vifr_name)
            goto free_vif;
        memcpy(dst->vifr_name, src->vifr_name, strlen(src->vifr_name) + 1);
    }

    if (src->vifr_queue_ierrors_to_lcore_size &&
            src->vifr_queue_ierrors_to_lcore) {
        dst->vifr_queue_ierrors_to_lcore =
            malloc(src->vifr_queue_ierrors_to_lcore_size * sizeof(uint64_t));
        if (!dst->vifr_queue_ierrors_to_lcore)
            goto free_vif;

        memcpy(dst->vifr_queue_ierrors_to_lcore,
                src->vifr_queue_ierrors_to_lcore,
                src->vifr_queue_ierrors_to_lcore_size);
        dst->vifr_queue_ierrors_to_lcore_size =
            src->vifr_queue_ierrors_to_lcore_size;
    }

    if (src->vifr_mac && src->vifr_mac_size) {
        dst->vifr_mac = malloc(src->vifr_mac_size);
        if (!dst->vifr_mac)
            goto free_vif;

        memcpy(dst->vifr_mac, src->vifr_mac, src->vifr_mac_size);
        dst->vifr_mac_size = src->vifr_mac_size;
    }

    if (src->vifr_src_mac && src->vifr_src_mac_size) {
        dst->vifr_src_mac = malloc(src->vifr_src_mac_size);
        if (!dst->vifr_src_mac)
            goto free_vif;

        memcpy(dst->vifr_src_mac, src->vifr_src_mac, src->vifr_src_mac_size);
        dst->vifr_src_mac_size = src->vifr_src_mac_size;
    }


    if (src->vifr_fat_flow_protocol_port_size &&
            src->vifr_fat_flow_protocol_port) {
        dst->vifr_fat_flow_protocol_port =
            malloc(src->vifr_fat_flow_protocol_port_size * sizeof(uint32_t));
        if (!dst->vifr_fat_flow_protocol_port)
            goto free_vif;

        memcpy(dst->vifr_fat_flow_protocol_port,
                src->vifr_fat_flow_protocol_port,
                src->vifr_fat_flow_protocol_port_size);
        dst->vifr_fat_flow_protocol_port_size =
            src->vifr_fat_flow_protocol_port_size;
    }

    if (src->vifr_fat_flow_src_prefix_h_size &&
        src->vifr_fat_flow_src_prefix_h) {
        dst->vifr_fat_flow_src_prefix_h =
                   malloc(src->vifr_fat_flow_src_prefix_h_size * sizeof(uint64_t));
        if (!dst->vifr_fat_flow_src_prefix_h)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_src_prefix_h, src->vifr_fat_flow_src_prefix_h,
               src->vifr_fat_flow_src_prefix_h_size);
        dst->vifr_fat_flow_src_prefix_h_size = src->vifr_fat_flow_src_prefix_h_size;
    }

    if (src->vifr_fat_flow_src_prefix_l_size &&
        src->vifr_fat_flow_src_prefix_l) {
        dst->vifr_fat_flow_src_prefix_l =
                  malloc(src->vifr_fat_flow_src_prefix_l_size * sizeof(uint64_t));
        if (!dst->vifr_fat_flow_src_prefix_l)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_src_prefix_l, src->vifr_fat_flow_src_prefix_l,
               src->vifr_fat_flow_src_prefix_l_size);
        dst->vifr_fat_flow_src_prefix_l_size = src->vifr_fat_flow_src_prefix_l_size;
    }
    if (src->vifr_fat_flow_src_prefix_mask_size &&
        src->vifr_fat_flow_src_prefix_mask) {
        dst->vifr_fat_flow_src_prefix_mask =
                   malloc(src->vifr_fat_flow_src_prefix_mask_size * sizeof(uint8_t));
        if (!dst->vifr_fat_flow_src_prefix_mask)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_src_prefix_mask, src->vifr_fat_flow_src_prefix_mask,
               src->vifr_fat_flow_src_prefix_mask_size);
        dst->vifr_fat_flow_src_prefix_mask_size = src->vifr_fat_flow_src_prefix_mask_size;
    }
    if (src->vifr_fat_flow_src_aggregate_plen_size &&
        src->vifr_fat_flow_src_aggregate_plen) {
        dst->vifr_fat_flow_src_aggregate_plen =
                   malloc(src->vifr_fat_flow_src_aggregate_plen_size * sizeof(uint8_t));
        if (!dst->vifr_fat_flow_src_aggregate_plen)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_src_aggregate_plen, src->vifr_fat_flow_src_aggregate_plen,
               src->vifr_fat_flow_src_aggregate_plen_size);
        dst->vifr_fat_flow_src_aggregate_plen_size = src->vifr_fat_flow_src_aggregate_plen_size;
    }
    if (src->vifr_fat_flow_dst_prefix_h_size &&
        src->vifr_fat_flow_dst_prefix_h) {
        dst->vifr_fat_flow_dst_prefix_h =
                malloc(src->vifr_fat_flow_dst_prefix_h_size * sizeof(uint64_t));
        if (!dst->vifr_fat_flow_dst_prefix_h)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_dst_prefix_h, src->vifr_fat_flow_dst_prefix_h,
               src->vifr_fat_flow_dst_prefix_h_size);
        dst->vifr_fat_flow_dst_prefix_h_size = src->vifr_fat_flow_dst_prefix_h_size;
    }
    if (src->vifr_fat_flow_dst_prefix_l_size &&
        src->vifr_fat_flow_dst_prefix_l) {
        dst->vifr_fat_flow_dst_prefix_l =
                 malloc(src->vifr_fat_flow_dst_prefix_l_size * sizeof(uint64_t));
        if (!dst->vifr_fat_flow_dst_prefix_l)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_dst_prefix_l, src->vifr_fat_flow_dst_prefix_l,
               src->vifr_fat_flow_dst_prefix_l_size);
        dst->vifr_fat_flow_dst_prefix_l_size = src->vifr_fat_flow_dst_prefix_l_size;
    }
    if (src->vifr_fat_flow_dst_prefix_mask_size &&
        src->vifr_fat_flow_dst_prefix_mask) {
        dst->vifr_fat_flow_dst_prefix_mask =
                 malloc(src->vifr_fat_flow_dst_prefix_mask_size * sizeof(uint8_t));
        if (!dst->vifr_fat_flow_dst_prefix_mask)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_dst_prefix_mask, src->vifr_fat_flow_dst_prefix_mask,
               src->vifr_fat_flow_dst_prefix_mask_size);
        dst->vifr_fat_flow_dst_prefix_mask_size = src->vifr_fat_flow_dst_prefix_mask_size;
    }
    if (src->vifr_fat_flow_dst_aggregate_plen_size &&
        src->vifr_fat_flow_dst_aggregate_plen) {
        dst->vifr_fat_flow_dst_aggregate_plen =
                  malloc(src->vifr_fat_flow_dst_aggregate_plen_size * sizeof(uint8_t));
        if (!dst->vifr_fat_flow_dst_aggregate_plen)
            goto free_vif;
        memcpy(dst->vifr_fat_flow_dst_aggregate_plen, src->vifr_fat_flow_dst_aggregate_plen,
               src->vifr_fat_flow_dst_aggregate_plen_size);
        dst->vifr_fat_flow_dst_aggregate_plen_size = src->vifr_fat_flow_dst_aggregate_plen_size;
    }

    if (src->vifr_fat_flow_exclude_ip_list_size &&
             src->vifr_fat_flow_exclude_ip_list) {
        dst->vifr_fat_flow_exclude_ip_list = malloc(src->vifr_fat_flow_exclude_ip_list_size * sizeof(uint64_t));
        if (!dst->vifr_fat_flow_exclude_ip_list) {
            goto free_vif;
        }
        memcpy(dst->vifr_fat_flow_exclude_ip_list, src->vifr_fat_flow_exclude_ip_list,
               src->vifr_fat_flow_exclude_ip_list_size * sizeof(uint64_t));
        dst->vifr_fat_flow_exclude_ip_list_size = src->vifr_fat_flow_exclude_ip_list_size;
    }

    if (src->vifr_fat_flow_exclude_ip6_u_list_size &&
            src->vifr_fat_flow_exclude_ip6_u_list) {
        dst->vifr_fat_flow_exclude_ip6_u_list = malloc(src->vifr_fat_flow_exclude_ip6_u_list_size * sizeof(uint64_t));
        if (!dst->vifr_fat_flow_exclude_ip6_u_list) {
            goto free_vif;
        }
        memcpy(dst->vifr_fat_flow_exclude_ip6_u_list, src->vifr_fat_flow_exclude_ip6_u_list,
               src->vifr_fat_flow_exclude_ip6_u_list_size * sizeof(uint64_t));
        dst->vifr_fat_flow_exclude_ip6_u_list_size = src->vifr_fat_flow_exclude_ip6_u_list_size;
    }
    if (src->vifr_fat_flow_exclude_ip6_l_list_size &&
            src->vifr_fat_flow_exclude_ip6_l_list) {
        dst->vifr_fat_flow_exclude_ip6_l_list = malloc(src->vifr_fat_flow_exclude_ip6_l_list_size * sizeof(uint64_t));
        if (!dst->vifr_fat_flow_exclude_ip6_l_list) {
            goto free_vif;
        }
        memcpy(dst->vifr_fat_flow_exclude_ip6_l_list, src->vifr_fat_flow_exclude_ip6_l_list,
               src->vifr_fat_flow_exclude_ip6_l_list_size * sizeof(uint64_t));
        dst->vifr_fat_flow_exclude_ip6_l_list_size = src->vifr_fat_flow_exclude_ip6_l_list_size;
    }

    return dst;

free_vif:
    vr_interface_req_destroy(dst);
    dst = NULL;
    return NULL;
}

int
vr_send_interface_dump(struct nl_client *cl, unsigned int router_id,
        int marker, int core)
{
    vr_interface_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.vifr_rid = router_id;
    req.vifr_marker = marker;
    req.vifr_core = core;
    return vr_sendmsg(cl, &req, "vr_interface_req");
}

int
vr_send_interface_get(struct nl_client *cl, unsigned int router_id,
        int vif_index, int os_index, int core, int get_drops)
{
    vr_interface_req req;

    memset(&req, 0, sizeof(req));

    req.h_op = SANDESH_OP_GET;
    req.vifr_rid = router_id;
    req.vifr_os_idx = os_index;
    req.vifr_idx = vif_index;
    req.vifr_core = core;
    if (get_drops)
        req.vifr_flags |= VIF_FLAG_GET_DROP_STATS;

    return vr_sendmsg(cl, &req, "vr_interface_req");
}

int
vr_send_interface_delete(struct nl_client *cl, unsigned int router_id,
        char *vif_name, int vif_index)
{
    vr_interface_req req;

    memset(&req, 0, sizeof(req));

    req.h_op = SANDESH_OP_DEL;
    req.vifr_rid = router_id;
    req.vifr_name = vif_name;
    req.vifr_idx = vif_index;

    return vr_sendmsg(cl, &req, "vr_interface_req");
}

int
vr_send_interface_add(struct nl_client *cl, int router_id, char *vif_name,
        int os_index, int vif_index, int *vif_xconnect_index, int vif_type,
        unsigned int vrf, unsigned int flags, int8_t *vif_mac, int8_t vif_transport,
        const char *guid)
{
    int platform, i = 0;
    vr_interface_req req;

    platform = get_platform();
    memset(&req, 0, sizeof(req));

    req.h_op = SANDESH_OP_ADD;
    if (vif_name)
        req.vifr_name = vif_name;
    if (vif_mac) {
        req.vifr_mac_size = 6;
        req.vifr_mac = vif_mac;
    }
    req.vifr_vrf = vrf;
    req.vifr_mcast_vrf = (uint16_t)(-1);

    if (os_index > 0)
        req.vifr_os_idx = os_index;

    req.vifr_idx = vif_index;
    req.vifr_rid = router_id;
    req.vifr_type = vif_type;
    req.vifr_flags = flags;
    req.vifr_transport = vif_transport;

    if (vif_type == VIF_TYPE_HOST) {
        for(i = 0; i < VR_MAX_PHY_INF; i++) {
            if(vif_xconnect_index[i] < 0) {
                break;
            }
        }
        req.vifr_cross_connect_idx_size = i;
        req.vifr_cross_connect_idx = vif_xconnect_index;
    }

    return vr_sendmsg(cl, &req, "vr_interface_req");
}

int
vr_send_vif_clear_stats(struct nl_client *cl, unsigned int router_id,
        int vif_idx, int core)
{
    vr_interface_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_RESET;
    req.vifr_rid = router_id;
    req.vifr_idx = vif_idx;
    req.vifr_core = core + 1;

    return vr_sendmsg(cl, &req, "vr_interface_req");
}

/* interface end */


int
vr_send_mem_stats_get(struct nl_client *cl, unsigned int router_id)
{
    vr_mem_stats_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.vms_rid = router_id;

    return vr_sendmsg(cl, &req, "vr_mem_stats_req");
}

/* mirror start */
void
vr_mirror_req_destroy(vr_mirror_req *req)
{
    if (!req)
        return;

    free(req);

    return;
}

vr_mirror_req *
vr_mirror_req_get_copy(vr_mirror_req *req)
{
    vr_mirror_req *dst;

    if (!req)
        return NULL;

    dst = malloc(sizeof(*req));
    if (!dst)
        return NULL;

    *dst = *req;

    return dst;
}

int
vr_send_mirror_dump(struct nl_client *cl, unsigned int router_id,
        int marker)
{
    vr_mirror_req req;

    req.h_op = SANDESH_OP_DUMP;
    req.mirr_rid = router_id;
    req.mirr_marker = marker;

    return vr_sendmsg(cl, &req, "vr_mirror_req");
}

int
vr_send_mirror_get(struct nl_client *cl, unsigned int router_id,
        unsigned int mirror_index)
{
    vr_mirror_req req;

    req.h_op = SANDESH_OP_GET;
    req.mirr_rid = router_id;
    req.mirr_index = mirror_index;

    return vr_sendmsg(cl, &req, "vr_mirror_req");
}

int
vr_send_mirror_delete(struct nl_client *cl, unsigned int router_id,
        unsigned int mirror_index)
{
    vr_mirror_req req;

    req.h_op = SANDESH_OP_DEL;
    req.mirr_rid = router_id;
    req.mirr_index = mirror_index;

    return vr_sendmsg(cl, &req, "vr_mirror_req");
}

int
vr_send_mirror_add(struct nl_client *cl, unsigned int router_id,
        unsigned int mirror_index, int mirror_nh_index,
        unsigned int mirror_flags, int vni_id)
{
    vr_mirror_req req;

    memset(&req, 0, sizeof(req));

    req.h_op = SANDESH_OP_ADD;
    req.mirr_rid = router_id;
    req.mirr_index = mirror_index;
    req.mirr_nhid = mirror_nh_index;
    req.mirr_flags = mirror_flags;
    req.mirr_vni = vni_id;

    return vr_sendmsg(cl, &req, "vr_mirror_req");
}
/* mirror end */

int
vr_send_vrf_dump(struct nl_client *cl, unsigned int router_id,
        int marker)
{
    vr_vrf_req req;

    req.h_op = SANDESH_OP_DUMP;
    req.vrf_rid = router_id;
    req.vrf_marker = marker;

    return vr_sendmsg(cl, &req, "vr_vrf_req");
}

int
vr_send_vrf_get(struct nl_client *cl, unsigned int router_id,
        unsigned int vrf_index)
{
    vr_vrf_req req;

    req.h_op = SANDESH_OP_GET;
    req.vrf_rid = router_id;
    req.vrf_idx = vrf_index;

    return vr_sendmsg(cl, &req, "vr_vrf_req");
}

int
vr_send_vrf_delete(struct nl_client *cl, unsigned int router_id,
        unsigned int vrf_index)
{
    vr_vrf_req req;

    req.h_op = SANDESH_OP_DEL;
    req.vrf_rid = router_id;
    req.vrf_idx = vrf_index;

    return vr_sendmsg(cl, &req, "vr_vrf_req");
}

int
vr_send_vrf_add(struct nl_client *cl, unsigned int router_id,
        unsigned int vrf_index, int hbfl_idx, int hbfr_idx,
        unsigned int vrf_flags)
{
    vr_vrf_req req;

    memset(&req, 0, sizeof(req));

    req.h_op = SANDESH_OP_ADD;
    req.vrf_rid = router_id;
    req.vrf_idx = vrf_index;
    req.vrf_flags = vrf_flags;
    req.vrf_hbfl_vif_idx = hbfl_idx;
    req.vrf_hbfr_vif_idx = hbfr_idx;

    return vr_sendmsg(cl, &req, "vr_vrf_req");
}

int
vr_send_mpls_delete(struct nl_client *cl, unsigned int router_id,
        unsigned int label)
{
    vr_mpls_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DEL;
    req.mr_rid = router_id;
    req.mr_label = label;

    return vr_sendmsg(cl, &req, "vr_mpls_req");
}

int
vr_send_mpls_dump(struct nl_client *cl, unsigned int router_id, int marker)
{
    vr_mpls_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.mr_rid = router_id;
    req.mr_marker = marker;

    return vr_sendmsg(cl, &req, "vr_mpls_req");
}

int
vr_send_mpls_get(struct nl_client *cl, unsigned int router_id, unsigned int label)
{
    vr_mpls_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.mr_rid = router_id;
    req.mr_label = label;

    return vr_sendmsg(cl, &req, "vr_mpls_req");
}

int
vr_send_mpls_add(struct nl_client *cl, unsigned int router_id,
        unsigned int label, unsigned int nh_index)
{
    vr_mpls_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;
    req.mr_rid = router_id;
    req.mr_label = label;
    req.mr_nhid = nh_index;

    return vr_sendmsg(cl, &req, "vr_mpls_req");
}

char *
vr_nexthop_type_string(vr_nexthop_req *nh)
{
    switch (nh->nhr_type) {
    case NH_DEAD:
        return "DEAD";
        break;

    case NH_RCV:
        return "RECEIVE";
        break;

    case NH_ENCAP:
        return "ENCAP";
        break;

    case NH_TUNNEL:
        return "TUNNEL";
        break;

    case NH_RESOLVE:
        return "RESOLVE";
        break;

    case NH_DISCARD:
        return "DISCARD";
        break;

    case NH_COMPOSITE:
        return "COMPOSITE";
        break;

    case NH_VRF_TRANSLATE:
        return "VRF_TRANSLATE";
        break;

    case NH_L2_RCV:
        return "L2_RECEIVE";
        break;

     default:
        return "NONE";
    }

    return "NONE";
}


bool
vr_nexthop_req_has_vif(vr_nexthop_req *req)
{
    switch (req->nhr_type) {
    case NH_ENCAP:
    case NH_TUNNEL:
    case NH_RCV:
    case NH_L2_RCV:
        return true;
        break;

    case NH_COMPOSITE:
    default:
        return false;
        break;
    }

    return false;
}

void
vr_nexthop_req_destroy(vr_nexthop_req *req)
{
    if (!req)
        return;

    if (req->nhr_encap_size && req->nhr_encap) {
        free(req->nhr_encap);
        req->nhr_encap = NULL;
        req->nhr_encap_size = 0;
        req->nhr_encap_len = 0;
    }

    if (req->nhr_nh_list_size && req->nhr_nh_list) {
        free(req->nhr_nh_list);
        req->nhr_nh_list = NULL;
        req->nhr_nh_list_size = 0;
    }

    if (req->nhr_label_list_size && req->nhr_label_list) {
        free(req->nhr_label_list);
        req->nhr_label_list = NULL;
        req->nhr_label_list_size = 0;
    }

    if (req->nhr_tun_sip6_size && req->nhr_tun_sip6) {
        free(req->nhr_tun_sip6);
        req->nhr_tun_sip6 = NULL;
        req->nhr_tun_sip6_size = 0;
    }

    if (req->nhr_tun_dip6_size && req->nhr_tun_dip6) {
        free(req->nhr_tun_dip6);
        req->nhr_tun_dip6 = NULL;
        req->nhr_tun_dip6_size = 0;
    }

    if (req->nhr_encap_oif_id_size && req->nhr_encap_oif_id) {
        free(req->nhr_encap_oif_id);
        req->nhr_encap_oif_id = NULL;
        req->nhr_encap_oif_id_size = 0;
    }

    if (req->nhr_encap_valid_size && req->nhr_encap_valid) {
        free(req->nhr_encap_valid);
        req->nhr_encap_valid = NULL;
        req->nhr_encap_valid_size = 0;
    }

    free(req);

    return;
}

vr_nexthop_req *
vr_nexthop_req_get_copy(vr_nexthop_req *src)
{
    vr_nexthop_req *dst;

    dst = calloc(sizeof(vr_nexthop_req), 1);
    if (!dst)
        return NULL;

    /* first copy the in-built members */
    *dst = *src;

    dst->nhr_encap = NULL;
    dst->nhr_encap_size = 0;
    dst->nhr_nh_list = NULL;
    dst->nhr_nh_list_size = 0;
    dst->nhr_label_list = NULL;
    dst->nhr_label_list_size = 0;
    dst->nhr_tun_sip6 = NULL;
    dst->nhr_tun_sip6_size = 0;
    dst->nhr_tun_dip6 = NULL;
    dst->nhr_tun_dip6_size = 0;
    dst->nhr_encap_oif_id = NULL;
    dst->nhr_encap_oif_id_size = 0;
    dst->nhr_encap_valid = NULL;
    dst->nhr_encap_valid_size = 0;

    /* ...and then the list elements */
    if (src->nhr_encap_size && src->nhr_encap) {
        dst->nhr_encap = malloc(src->nhr_encap_size);
        if (!dst->nhr_encap)
            goto free_nh;
        memcpy(dst->nhr_encap, src->nhr_encap, src->nhr_encap_size);
        dst->nhr_encap_size = src->nhr_encap_size;
    }

    /* component nexthop list */
    if (src->nhr_nh_list_size && src->nhr_nh_list) {
        dst->nhr_nh_list = malloc(src->nhr_nh_list_size * sizeof(uint32_t));
        if (!src->nhr_nh_list)
            goto free_nh;
        memcpy(dst->nhr_nh_list, src->nhr_nh_list,
                src->nhr_nh_list_size * sizeof(uint32_t));
        dst->nhr_nh_list_size = src->nhr_nh_list_size;
    }

    /* label list */
    if (src->nhr_label_list_size && src->nhr_label_list) {
        dst->nhr_label_list = malloc(src->nhr_label_list_size * sizeof(uint32_t));
        if (!src->nhr_label_list)
            goto free_nh;
        memcpy(dst->nhr_label_list, src->nhr_label_list,
                src->nhr_label_list_size * sizeof(uint32_t));
        dst->nhr_label_list_size = src->nhr_label_list_size;
    }

    /* ipv6 tunnel source */
    if (src->nhr_tun_sip6_size && src->nhr_tun_sip6) {
        dst->nhr_tun_sip6 = malloc(src->nhr_tun_sip6_size);
        if (!src->nhr_tun_sip6)
            goto free_nh;
        memcpy(dst->nhr_tun_sip6, src->nhr_tun_sip6, src->nhr_tun_sip6_size);
        dst->nhr_tun_sip6_size = src->nhr_tun_sip6_size;
    }

    /* ipv6 tunnel destination */
    if (src->nhr_tun_dip6_size && src->nhr_tun_dip6) {
        dst->nhr_tun_dip6 = malloc(src->nhr_tun_dip6_size);
        if (!src->nhr_tun_dip6)
            goto free_nh;
        memcpy(dst->nhr_tun_dip6, src->nhr_tun_dip6, src->nhr_tun_dip6_size);
        dst->nhr_tun_dip6_size = src->nhr_tun_dip6_size;
    }

    /* encap oif id list */
    if (src->nhr_encap_oif_id_size && src->nhr_encap_oif_id) {
        dst->nhr_encap_oif_id = malloc(src->nhr_encap_oif_id_size * sizeof(int32_t));
        if (!src->nhr_encap_oif_id)
            goto free_nh;
        memcpy(dst->nhr_encap_oif_id, src->nhr_encap_oif_id,
                src->nhr_encap_oif_id_size * sizeof(int32_t));
        dst->nhr_encap_oif_id_size = src->nhr_encap_oif_id_size;
    }

    /* encap valid list */
    if (src->nhr_encap_valid_size && src->nhr_encap_valid &&
            (src->nhr_flags & NH_FLAG_TUNNEL_UNDERLAY_ECMP)) {
        dst->nhr_encap_valid = malloc(src->nhr_encap_valid_size * sizeof(int32_t));
        if (!src->nhr_encap_valid)
            goto free_nh;
        memcpy(dst->nhr_encap_valid, src->nhr_encap_valid,
                src->nhr_encap_valid_size * sizeof(int32_t));
        dst->nhr_encap_valid_size = src->nhr_encap_valid_size;
    }

    return dst;

free_nh:
    vr_nexthop_req_destroy(dst);
    return NULL;
}

int
vr_send_nexthop_delete(struct nl_client *cl, unsigned int router_id,
        unsigned int nh_index)
{
    vr_nexthop_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DEL;
    req.nhr_rid = router_id;
    req.nhr_id = nh_index;

    return vr_sendmsg(cl, &req, "vr_nexthop_req");
}

int
vr_send_nexthop_dump(struct nl_client *cl, unsigned int router_id,
        int marker)
{
    vr_nexthop_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.nhr_rid = router_id;
    req.nhr_marker = marker;

    return vr_sendmsg(cl, &req, "vr_nexthop_req");
}

int
vr_send_nexthop_get(struct nl_client *cl, unsigned int router_id,
        unsigned int nh_index)
{
    vr_nexthop_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.nhr_rid = router_id;
    req.nhr_id = nh_index;

    return vr_sendmsg(cl, &req, "vr_nexthop_req");
}

int
vr_send_pbb_tunnel_add(struct nl_client *cl, unsigned int router_id, int
        nh_index, unsigned int flags, int vrf_index, int8_t *bmac,
        unsigned int direct_nh_id, unsigned int direct_label)
{
    int ret = 0;
    unsigned int i;
    vr_nexthop_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;
    req.nhr_rid = router_id;
    req.nhr_vrf = vrf_index;
    req.nhr_id = nh_index;
    req.nhr_flags = flags;
    req.nhr_type = NH_TUNNEL;

    req.nhr_encap_oif_id_size = 1;
    req.nhr_encap_oif_id = calloc(req.nhr_encap_oif_id_size, sizeof(unsigned int));
    if (!req.nhr_encap_oif_id) {
        ret = -ENOMEM;
        goto fail;
    }
    req.nhr_encap_oif_id[0] = 0;

    req.nhr_nh_list_size = 1;
    req.nhr_nh_list = calloc(1, sizeof(uint32_t));
    if (!req.nhr_nh_list) {
        ret = -ENOMEM;
        goto fail;
    }

    req.nhr_label_list = calloc(1, sizeof(uint32_t));
    if (!req.nhr_label_list) {
        ret = -ENOMEM;
        goto fail;
    }

    req.nhr_pbb_mac_size = VR_ETHER_ALEN;
    req.nhr_pbb_mac = calloc(VR_ETHER_ALEN, sizeof(uint8_t));
    if (!req.nhr_pbb_mac) {
        ret = -ENOMEM;
        goto fail;
    }
    VR_MAC_COPY(req.nhr_pbb_mac, bmac);

    req.nhr_label_list_size = 1;
    req.nhr_nh_list[0] = direct_nh_id;
    req.nhr_label_list[0] = direct_label;
    req.nhr_family = AF_BRIDGE;

    ret = vr_sendmsg(cl, &req, "vr_nexthop_req");
fail:
    if (req.nhr_nh_list) {
        free(req.nhr_nh_list);
        req.nhr_nh_list = NULL;
    }

    if (req.nhr_label_list) {
        free(req.nhr_label_list);
        req.nhr_label_list = NULL;
    }

    return ret;
}

int
vr_send_nexthop_composite_add(struct nl_client *cl, unsigned int router_id,
        int nh_index, unsigned int flags, int vrf_index,
        unsigned int num_components, unsigned int *component_nh_indices,
        unsigned int *component_labels, unsigned int family)
{
    int ret = 0;
    unsigned int i;
    vr_nexthop_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;
    req.nhr_rid = router_id;
    req.nhr_vrf = vrf_index;
    req.nhr_id = nh_index;
    req.nhr_flags = flags;
    req.nhr_type = NH_COMPOSITE;

    req.nhr_nh_list_size = num_components;
    req.nhr_nh_list = calloc(num_components, sizeof(uint32_t));
    if (!req.nhr_nh_list) {
        ret = -ENOMEM;
        goto fail;
    }

    req.nhr_label_list = calloc(num_components, sizeof(uint32_t));
    if (!req.nhr_label_list) {
        ret = -ENOMEM;
        goto fail;
    }

    req.nhr_label_list_size = num_components;
    for (i = 0; i < num_components; i++) {
        req.nhr_nh_list[i] = component_nh_indices[i];
        req.nhr_label_list[i] = component_labels[i];
    }


    req.nhr_family = family;

    ret = vr_sendmsg(cl, &req, "vr_nexthop_req");
fail:
    if (req.nhr_nh_list) {
        free(req.nhr_nh_list);
        req.nhr_nh_list = NULL;
    }

    if (req.nhr_label_list) {
        free(req.nhr_label_list);
        req.nhr_label_list = NULL;
    }

    return ret;
}


int
vr_send_nexthop_encap_tunnel_add(struct nl_client *cl, unsigned int router_id,
        unsigned int type, int nh_index, unsigned int flags, int vrf_index,
        int *vif_index, int8_t smac[][6], int8_t dmac[][6], struct in_addr sip,
        struct in_addr dip, int sport, int dport, int8_t *l3_vxlan_mac, int family,
        int count)
{
    vr_nexthop_req req;
    int i;

    memset(&req, 0, sizeof(req));

    req.h_op = SANDESH_OP_ADD;
    req.nhr_rid = router_id;
    req.nhr_vrf = vrf_index;
    req.nhr_id = nh_index;
    req.nhr_flags = flags;
    req.nhr_type = type;
    if (req.nhr_flags & NH_FLAG_TUNNEL_UNDERLAY_ECMP) {
        req.nhr_encap_oif_id_size = VR_MAX_PHY_INF;
        req.nhr_encap_valid_size = VR_MAX_PHY_INF;
        req.nhr_encap_valid = malloc(req.nhr_encap_valid_size * sizeof(unsigned int));
    } else {
        req.nhr_encap_oif_id_size = 1;
        req.nhr_encap_valid_size = 0;
        req.nhr_encap_valid = NULL;
    }
    req.nhr_encap_oif_id = malloc(req.nhr_encap_oif_id_size * sizeof(unsigned int));
    if (flags & NH_FLAG_TUNNEL_UNDERLAY_ECMP) {
        for (i = 0; i < VR_MAX_PHY_INF; i++) {
            if (vif_index[i] < 0)
                req.nhr_encap_valid[i] = 0;
            else
                req.nhr_encap_valid[i] = 1;
            req.nhr_encap_oif_id[i] = vif_index[i];
        }
    }
    else
        req.nhr_encap_oif_id[0] = vif_index[0];
    if (count)
        req.nhr_encap_len = 14;
    req.nhr_encap_size = 14 * count;
    req.nhr_encap = malloc(req.nhr_encap_size);
    if (!req.nhr_encap)
        return -ENOMEM;
    for (i = 0; i < count; i++) {
        memcpy(req.nhr_encap + i*14, dmac, 6);
        memcpy(req.nhr_encap + i*14 + 6, smac, 6);
        *(uint16_t *)(&req.nhr_encap[12 + i*14]) = htons(0x0800);
    }

#if defined(__linux__)
    req.nhr_encap_family = ETH_P_ARP;
#endif

    if (type == NH_TUNNEL) {
        req.nhr_tun_sip = sip.s_addr;
        req.nhr_tun_dip = dip.s_addr;
        if ((sport >= 0) && (dport >= 0)) {
            req.nhr_tun_sport = htons(sport);
            req.nhr_tun_dport = htons(dport);
        }
        if (flags & NH_FLAG_L3_VXLAN) {
            req.nhr_rw_dst_mac_size = 6;
            req.nhr_rw_dst_mac = malloc(req.nhr_rw_dst_mac_size);
            memcpy(req.nhr_rw_dst_mac, l3_vxlan_mac, 6);
        }
    }

    if (type == NH_ENCAP)
        req.nhr_family = family;

    return vr_sendmsg(cl, &req, "vr_nexthop_req");
}

int
vr_send_nexthop_add(struct nl_client *cl, unsigned int router_id,
        unsigned int type, int nh_index, unsigned int flags, int vrf_index,
        int *vif_index, int family)
{
    vr_nexthop_req req;

    memset(&req, 0, sizeof(req));

    req.h_op = SANDESH_OP_ADD;
    req.nhr_rid = router_id;
    req.nhr_vrf = vrf_index;
    req.nhr_id = nh_index;
    req.nhr_flags = flags;
    req.nhr_type = type;
    req.nhr_family = family;
    req.nhr_encap_oif_id_size = 1;
    req.nhr_encap_valid_size = 0;
    req.nhr_encap_valid = NULL;
    req.nhr_encap_oif_id = malloc(req.nhr_encap_oif_id_size * sizeof(unsigned int));
    req.nhr_encap_len = 14;
    req.nhr_encap_oif_id[0] = vif_index[0];

    return vr_sendmsg(cl, &req, "vr_nexthop_req");
}

void
vr_route_req_destroy(vr_route_req *req)
{
    if (!req)
        return;

    if (req->rtr_prefix_size && req->rtr_prefix) {
        free(req->rtr_prefix);
        req->rtr_prefix = NULL;
        req->rtr_prefix_size = 0;
    }

    if (req->rtr_mac_size && req->rtr_mac) {
        free(req->rtr_mac);
        req->rtr_mac = NULL;
        req->rtr_mac_size = 0;
    }

    free(req);
    return;
}

void
address_mask(uint8_t *addr, uint8_t plen, unsigned int family)
{
   int i;
    uint8_t address_bits;
    uint8_t mask[VR_IP6_ADDRESS_LEN];

    if (family == AF_INET) {
        address_bits = VR_IP_ADDRESS_LEN * 8;
    } else {
        address_bits = VR_IP6_ADDRESS_LEN * 8;
    }

    memset(mask, 0xFF, sizeof(mask));
    for (i = address_bits - 1; i >= plen; i--) {
        mask[i / 8] ^= (1 << (7 - (i % 8)));
    }

    for (i = 0; i < (address_bits / 8); i++) {
        addr[i] &= mask[i];
    }

    return;
}

vr_route_req *
vr_route_req_get_copy(vr_route_req *src)
{
    vr_route_req *dst;

    dst = malloc(sizeof(*dst));
    if (!dst)
        return NULL;

    *dst = *src;

    dst->rtr_prefix_size = 0;
    dst->rtr_prefix = NULL;

    dst->rtr_marker_size = 0;
    dst->rtr_marker = NULL;

    dst->rtr_mac_size = 0;
    dst->rtr_mac = NULL;

    if (src->rtr_prefix_size && src->rtr_prefix) {
        dst->rtr_prefix = malloc(src->rtr_prefix_size);
        if (!dst->rtr_prefix)
            goto free_rtr_req;
        memcpy(dst->rtr_prefix, src->rtr_prefix, src->rtr_prefix_size);
    }

    if (src->rtr_mac_size && src->rtr_mac) {
        dst->rtr_mac = malloc(src->rtr_mac_size);
        if (!dst->rtr_mac)
            goto free_rtr_req;
        memcpy(dst->rtr_mac, src->rtr_mac, src->rtr_mac_size);
    }

    return dst;

free_rtr_req:
    vr_route_req_destroy(dst);
    return NULL;
}

int
vr_send_route_dump(struct nl_client *cl, unsigned int router_id, unsigned int vrf,
        unsigned int family, uint8_t *marker)
{
    vr_route_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.rtr_rid = router_id;
    req.rtr_vrf_id = vrf;
    req.rtr_family = family;

    if (family == AF_BRIDGE) {
        req.rtr_mac = marker;
        req.rtr_mac_size = VR_ETHER_ALEN;
    } else {
        req.rtr_prefix = marker;
        req.rtr_prefix_size = RT_IP_ADDR_SIZE(family);
        req.rtr_marker = marker;
        req.rtr_marker_size = RT_IP_ADDR_SIZE(family);
    }

    return vr_sendmsg(cl, &req, "vr_route_req");
}

static int
vr_send_route_common(struct nl_client *cl, unsigned int op,
        unsigned int router_id, unsigned int vrf, unsigned int family,
        uint8_t *prefix, unsigned int prefix_len, unsigned int nh_index,
        int label, uint8_t *mac, uint32_t replace_len, unsigned int flags)
{
    vr_route_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = op;
    req.rtr_rid = router_id;
    req.rtr_vrf_id = vrf;
    req.rtr_family = family;

    if ((family == AF_INET) || (family == AF_INET6)) {
        req.rtr_prefix = prefix;
        req.rtr_prefix_size = RT_IP_ADDR_SIZE(family);
        req.rtr_prefix_len = prefix_len;
    } else if (family == AF_BRIDGE) {
        req.rtr_index = VR_BE_INVALID_INDEX;
    }

    if (mac) {
        req.rtr_mac = mac;
        req.rtr_mac_size = VR_ETHER_ALEN;
    }

    req.rtr_replace_plen = replace_len;
    req.rtr_label_flags = flags;
    req.rtr_label = label;
    if (label != -1)
        req.rtr_label_flags |= VR_RT_LABEL_VALID_FLAG;

    req.rtr_nh_id = nh_index;

    return vr_sendmsg(cl, &req, "vr_route_req");
}

int
vr_send_route_get(struct nl_client *cl,
        unsigned int router_id, unsigned int vrf, unsigned int family,
        uint8_t *prefix, unsigned int prefix_len, uint8_t *mac)
{
    return vr_send_route_common(cl, SANDESH_OP_GET, router_id, vrf,
            family, prefix, prefix_len, 0, 0, mac, 0, 0);
}

int
vr_send_route_delete(struct nl_client *cl,
        unsigned int router_id, unsigned int vrf, unsigned int family,
        uint8_t *prefix, unsigned int prefix_len, unsigned int nh_index,
        int label, uint8_t *mac, uint32_t replace_len, unsigned int flags)
{
    return vr_send_route_common(cl, SANDESH_OP_DEL, router_id, vrf,
            family, prefix, prefix_len, nh_index, label,
            mac, replace_len, flags);
}

int
vr_send_route_add(struct nl_client *cl,
        unsigned int router_id, unsigned int vrf, unsigned int family,
        uint8_t *prefix, unsigned int prefix_len, unsigned int nh_index,
        int label, uint8_t *mac, uint32_t replace_len, unsigned int flags)
{
    return vr_send_route_common(cl, SANDESH_OP_ADD, router_id, vrf,
            family, prefix, prefix_len, nh_index, label,
            mac, replace_len,flags);
}

/* vrf assign start */
int
vr_send_vrf_assign_dump(struct nl_client *cl, unsigned int router_id,
        unsigned int vif_index, int marker)
{
    vr_vrf_assign_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.var_rid = router_id;
    req.var_vif_index = vif_index;
    req.var_marker = marker;

    return vr_sendmsg(cl, &req, "vr_vrf_assign_req");
}

int
vr_send_vrf_assign_set(struct nl_client *cl, unsigned int router_id,
        unsigned int vif_index, unsigned int vlan_id, unsigned int vrf_id)
{

    vr_vrf_assign_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;
    req.var_rid = router_id;
    req.var_vif_index = vif_index;
    req.var_vif_vrf = vrf_id;
    req.var_vlan_id = vlan_id;

    return vr_sendmsg(cl, &req, "vr_vrf_assign_req");
}
/* vrf assign end */

int
vr_send_vrf_stats_dump(struct nl_client *cl, unsigned int router_id, int marker)
{
    vr_vrf_stats_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.vsr_rid = router_id;
    req.vsr_marker = marker;
    req.vsr_family = AF_INET;

    return vr_sendmsg(cl, &req, "vr_vrf_stats_req");
}

int
vr_send_vrf_stats_get(struct nl_client *cl, unsigned int router_id,
        unsigned int vrf)
{
    vr_vrf_stats_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.vsr_rid = router_id;
    req.vsr_vrf = vrf;
    req.vsr_family = AF_INET;

    return vr_sendmsg(cl, &req, "vr_vrf_stats_req");
}

int
vr_send_vrouter_get(struct nl_client *cl, unsigned int router_id)
{
    vrouter_ops req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;

    return vr_sendmsg(cl, &req, "vrouter_ops");
}

int
vr_send_vrouter_set_logging(struct nl_client *cl, unsigned int router_id,
        unsigned int log_level, unsigned int *e_log_types, unsigned int e_size,
        unsigned int *d_log_types, unsigned int d_size)
{
    vrouter_ops req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;

    if (log_level > 0)
        req.vo_log_level = log_level;

    if (e_log_types && e_size) {
        req.vo_log_type_enable_size = e_size;
        req.vo_log_type_enable = e_log_types;
    }

    if (d_log_types && d_size) {
        req.vo_log_type_disable_size = d_size;
        req.vo_log_type_disable = d_log_types;
    }

    /*
     * We create request to change logging options only. As we do not change
     * vRouter's runtime parameters here, they need to be set to a meaningless
     * value. They cannot be left zeroed, because 0 means 'feature turned off'.
     */
    req.vo_perfr = -1;
    req.vo_perfs = -1;
    req.vo_from_vm_mss_adj = -1;
    req.vo_to_vm_mss_adj = -1;
    req.vo_perfr1 = -1;
    req.vo_perfr2 = -1;
    req.vo_perfr3 = -1;
    req.vo_perfp = -1;
    req.vo_perfq1 = -1;
    req.vo_perfq2 = -1;
    req.vo_perfq3 = -1;
    req.vo_udp_coff = -1;
    req.vo_flow_hold_limit = -1;
    req.vo_mudp = -1;
    req.vo_packet_dump = -1;

    return vr_sendmsg(cl, &req, "vrouter_ops");
}

int
vr_send_vrouter_set_runtime_opts(struct nl_client *cl, unsigned int router_id,
        int perfr, int perfs, int from_vm_mss_adj, int to_vm_mss_adj,
        int perfr1, int perfr2, int perfr3, int perfp, int perfq1,
        int perfq2, int perfq3, int udp_coff, int flow_hold_limit,
        int mudp, int btokens, int binterval, int bstep,
        unsigned int priority_tagging, int packet_dump)
{
    vrouter_ops req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;

    /*
     * vRouter runtime options. Adjustable by sysctl as well.
     *
     * No real validation is required, as sysctl does not perform any.
     * Variables are only tested to be -1 ('do not change'),
     * 0 ('feature turned off'), or non-zero ('feature turned on').
     */
    req.vo_perfr = perfr;
    req.vo_perfs = perfs;
    req.vo_from_vm_mss_adj = from_vm_mss_adj;
    req.vo_to_vm_mss_adj = to_vm_mss_adj;
    req.vo_perfr1 = perfr1;
    req.vo_perfr2 = perfr2;
    req.vo_perfr3 = perfr3;
    req.vo_perfp = perfp;
    req.vo_perfq1 = perfq1;
    req.vo_perfq2 = perfq2;
    req.vo_perfq3 = perfq3;
    req.vo_udp_coff = udp_coff;
    req.vo_flow_hold_limit = flow_hold_limit;
    req.vo_mudp = mudp;
    req.vo_burst_tokens = btokens;
    req.vo_burst_interval = binterval;
    req.vo_burst_step = bstep;
    req.vo_priority_tagging = priority_tagging;
    req.vo_packet_dump = packet_dump;

    /*
     * We create request to change runtime (sysctl) options only. Log level
     * fields can be left zeroed, because only non-zero values are meaningful
     * in this case.
     */

    return vr_sendmsg(cl, &req, "vrouter_ops");
}

int
vr_send_vxlan_delete(struct nl_client *cl, unsigned int router_id,
        unsigned int vnid)
{
    vr_vxlan_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DEL;
    req.vxlanr_vnid = vnid;

    return vr_sendmsg(cl, &req, "vr_vxlan_req");
}

int
vr_send_vxlan_dump(struct nl_client *cl, unsigned int router_id,
        int marker)
{
    vr_vxlan_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.vxlanr_vnid = marker;

    return vr_sendmsg(cl, &req, "vr_vxlan_req");
}

int
vr_send_vxlan_get(struct nl_client *cl, unsigned int router_id,
        unsigned int vnid)
{
    vr_vxlan_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.vxlanr_vnid = vnid;

    return vr_sendmsg(cl, &req, "vr_vxlan_req");
}

int
vr_send_vxlan_add(struct nl_client *cl, unsigned int router_id,
        unsigned int vnid, unsigned int nh_index)
{
    vr_vxlan_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;
    req.vxlanr_vnid = vnid;
    req.vxlanr_nhid = nh_index;

    return vr_sendmsg(cl, &req, "vr_vxlan_req");
}

int
vr_send_fc_map_get(struct nl_client *cl, unsigned int router_id,
        uint8_t fc_map_id)
{
    vr_fc_map_req req;
    int16_t id = fc_map_id;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.fmr_rid = router_id;
    req.fmr_id = &id;
    req.fmr_id_size = 1;

    return vr_sendmsg(cl, &req, "vr_fc_map_req");
}

int
vr_send_fc_map_dump(struct nl_client *cl, unsigned int router_id,
        int marker)
{
    vr_fc_map_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.fmr_rid = router_id;
    req.fmr_marker = marker;

    return vr_sendmsg(cl, &req, "vr_fc_map_req");
}

int
vr_send_fc_map_delete(struct nl_client *cl, unsigned int router_id,
        uint8_t fc_id)
{
    vr_fc_map_req req;
    int16_t id = fc_id;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DEL;
    req.fmr_rid = router_id;
    req.fmr_id = &id;
    req.fmr_id_size = 1;

    return vr_sendmsg(cl, &req, "vr_fc_map_req");
}

int
vr_send_fc_map_add(struct nl_client *cl, unsigned int router_id,
        int16_t *fc_id, uint8_t fc_id_size,
        uint8_t *dscp, uint8_t *mpls_qos, uint8_t *dotonep, uint8_t *queue)
{
    vr_fc_map_req req;

    memset(&req, 0, sizeof(req));
    req.fmr_rid = router_id;

    req.fmr_id = fc_id;
    req.fmr_id_size = fc_id_size;
    req.fmr_dscp = dscp;
    req.fmr_dscp_size = fc_id_size;
    req.fmr_mpls_qos = mpls_qos;
    req.fmr_mpls_qos_size = fc_id_size;
    req.fmr_dotonep = dotonep;
    req.fmr_dotonep_size = fc_id_size;
    req.fmr_queue_id = queue;
    req.fmr_queue_id_size = fc_id_size;

    return vr_sendmsg(cl, &req, "vr_fc_map_req");
}

int
vr_send_qos_map_get(struct nl_client *cl, unsigned int router_id,
        unsigned int qos_map_id)
{
    vr_qos_map_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_GET;
    req.qmr_rid = router_id;
    req.qmr_id = qos_map_id;

    return vr_sendmsg(cl, &req, "vr_qos_map_req");
}


int
vr_send_qos_map_dump(struct nl_client *cl, unsigned int router_id,
        int marker)
{
    vr_qos_map_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DUMP;
    req.qmr_rid = router_id;
    req.qmr_marker = marker;

    return vr_sendmsg(cl, &req, "vr_qos_map_req");
}

int
vr_send_qos_map_delete(struct nl_client *cl, unsigned int router_id,
        unsigned int qos_map_id)
{
    vr_qos_map_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_DEL;
    req.qmr_rid = router_id;
    req.qmr_id = qos_map_id;

    return vr_sendmsg(cl, &req, "vr_qos_map_req");
}

int
vr_send_qos_map_add(struct nl_client *cl, unsigned int router_id,
        unsigned int qos_id,
        uint8_t *dscp, uint8_t num_dscp, uint8_t *dscp_fc_id,
        uint8_t *mpls_qos, uint8_t num_mpls_qos, uint8_t *mpls_qos_fc_id,
        uint8_t *dotonep, uint8_t num_dotonep, uint8_t *dotonep_fc_id)
{
    vr_qos_map_req req;

    memset(&req, 0, sizeof(req));
    req.h_op = SANDESH_OP_ADD;
    req.qmr_rid = router_id;
    req.qmr_id = qos_id;

    if (num_dscp) {
        req.qmr_dscp = dscp;
        req.qmr_dscp_size = num_dscp;
        req.qmr_dscp_fc_id = dscp_fc_id;
        req.qmr_dscp_fc_id_size = num_dscp;
    }

    if (num_mpls_qos) {
        req.qmr_mpls_qos = mpls_qos;
        req.qmr_mpls_qos_size = num_mpls_qos;
        req.qmr_mpls_qos_fc_id = mpls_qos_fc_id;
        req.qmr_mpls_qos_fc_id_size = num_mpls_qos;
    }

    if (num_dotonep) {
        req.qmr_dotonep = dotonep;
        req.qmr_dotonep_size = num_dotonep;
        req.qmr_dotonep_fc_id = dotonep_fc_id;
        req.qmr_dotonep_fc_id_size = num_dotonep;
    }

    return vr_sendmsg(cl, &req, "vr_qos_map_req");
}

int
vr_send_ddp_req(struct nl_client *cl, vr_info_msg_en msginfo, uint8_t *vr_info_inbuf)
{
    vr_info_req req;

    memset(&req, 0, sizeof(req));

    req.h_op        = SANDESH_OP_DUMP;
    req.vdu_msginfo = msginfo;

    if (vr_info_inbuf != NULL) {
        req.vdu_inbuf_size = strlen(vr_info_inbuf);
        req.vdu_inbuf = vr_info_inbuf;
    }
    return vr_sendmsg(cl, &req, "vr_info_req");
}

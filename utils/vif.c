/*
 * vif.c -- 'vrouter' interface utility
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <getopt.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>

#include "vr_os.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#if defined(__linux__)
#include <asm/types.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_ether.h>
#endif

#if defined(__linux__)
#include <net/if.h>
#include <net/ethernet.h>
#include <netinet/ether.h>
#endif

#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>

#include "vr_types.h"
#include "vr_message.h"
#include "vr_packet.h"
#include "vr_interface.h"
#include "vhost.h"
#include "vr_genetlink.h"
#include "nl_util.h"
#include "ini_parser.h"


#define LISTING_NUM_OF_LINE  3
#define MAX_OUTPUT_IF 32

#define SET_TIMEOUT_MS 1000
#define CORRECT_ERROR_CNT(cur_error_cnt_ptr, prev_error_cnt_ptr) \
    if (*((uint64_t *) cur_error_cnt_ptr) < *((uint64_t *) prev_error_cnt_ptr)) { \
       *((uint64_t *) cur_error_cnt_ptr) = *((uint64_t *) prev_error_cnt_ptr); }

#define COMPUTE_DIFFERENCE(new, old, counter, diff_time_ms) \
    new->counter = ((new->counter - old->counter) * 1000)/diff_time_ms

#define VHOST_TYPE_STRING           "vhost"
#define AGENT_TYPE_STRING           "agent"
#define PHYSICAL_TYPE_STRING        "physical"
#define VIRTUAL_TYPE_STRING         "virtual"
#define XEN_LL_TYPE_STRING          "xenll"
#define GATEWAY_TYPE_STRING         "gateway"
#define VIRTUAL_VLAN_TYPE_STRING    "virtual-vlan"
#define STATS_TYPE_STRING           "stats"
#define MONITORING_TYPE_STRING      "monitoring"

#define ETH_TRANSPORT_STRING        "eth"
#define PMD_TRASPORT_STRING         "pmd"
#define SOCKET_TRANSPORT_STRING     "socket"
#define VIRTUAL_TRANSPORT_STRING    "virtual"

static struct nl_client *cl;
static char flag_string[64], if_name[IFNAMSIZ];
static int if_kindex = -1, vrf_id, vr_ifindex = -1;
static int if_pmdindex = -1, vif_index = -1;
static bool need_xconnect_if = false;
static bool need_vif_id = false;
static int if_xconnect_kindex[VR_MAX_PHY_INF] = {-1, -1, -1};
static short vlan_id = -1;
static int vr_ifflags;
static unsigned int core = (unsigned)-1;
static int8_t vr_transport = 0;

static int add_set, create_set, get_set, list_set;
static int kindex_set, type_set, transport_set, help_set, set_set, vlan_set, dhcp_set;
static int vrf_set, mac_set, delete_set, policy_set, pmd_set, vindex_set, pci_set;
static int xconnect_set, vif_set, vhost_phys_set, core_set, rate_set, drop_set;
static int sock_dir_set, clear_stats_set;

static unsigned int vr_op, vr_if_type;
static bool dump_pending = false;
static bool vr_vrf_assign_dump = false;
static int dump_marker = -1, var_marker = -1;

static int platform;

static int8_t vr_ifmac[6];
static struct ether_addr *mac_opt;

static vr_interface_req prev_req[VR_MAX_INTERFACES];
static struct timeval last_time;


static bool first_rate_iter = false;


/*
 * How many times we partially ignore function call vr_interface_req_process.
 * For more information please read comment description for function:
 *  vr_interface_req_process
 */
static int ignore_number_interface = 0;

/*
 * How many interfaces we will print/count in rate statistics
 */
static int print_number_interface = 0;

static void Usage(void);
static void usage_internal(const char *msg);
static void list_header_print(void);
static void list_get_print(vr_interface_req *);
static void list_rate_print(vr_interface_req *);
static void rate_process(vr_interface_req *req, vr_interface_req *prev_req);
static void rate_stats_diff(vr_interface_req *, vr_interface_req *);
static void rate_stats(struct nl_client *, unsigned int);
static int is_stdin_hit();
static int is_number(const char *nptr);

static struct vr_util_flags flag_metadata[] = {
    {VIF_FLAG_POLICY_ENABLED,   "P",        "Policy"            },
    {VIF_FLAG_XCONNECT,         "X",        "Cross Connect"     },
    {VIF_FLAG_SERVICE_IF,       "S",        "Service Chain"     },
    {VIF_FLAG_MIRROR_RX,        "Mr",       "Receive Mirror"    },
    {VIF_FLAG_MIRROR_TX,        "Mt",       "Transmit Mirror"   },
    {VIF_FLAG_TX_CSUM_OFFLOAD,  "Tc",       "Transmit Checksum Offload"},
    {VIF_FLAG_L3_ENABLED,       "L3",       "Layer 3"           },
    {VIF_FLAG_L2_ENABLED,       "L2",       "Layer 2"           },
    {VIF_FLAG_DHCP_ENABLED,     "D",        "DHCP"              },
    {VIF_FLAG_VHOST_PHYS,       "Vp",       "Vhost Physical"    },
    {VIF_FLAG_PROMISCOUS,       "Pr",       "Promiscuous"       },
    {VIF_FLAG_NATIVE_VLAN_TAG,  "Vnt",      "Native Vlan Tagged"},
    {VIF_FLAG_NO_ARP_PROXY,     "Mnp",      "No MAC Proxy"      },
    {VIF_FLAG_PMD,              "Dpdk",     "DPDK PMD Interface"},
    {VIF_FLAG_FILTERING_OFFLOAD,"Rfl",      "Receive Filtering Offload"},
    {VIF_FLAG_MONITORED,        "Mon",      "Interface is Monitored"},
    {VIF_FLAG_UNKNOWN_UC_FLOOD, "Uuf",      "Unknown Unicast Flood"},
    {VIF_FLAG_VLAN_OFFLOAD,     "Vof",      "VLAN insert/strip offload"},
    {VIF_FLAG_DROP_NEW_FLOWS,   "Df",       "Drop New Flows"},
    {VIF_FLAG_MAC_LEARN,        "L",        "MAC Learning Enabled"},
    {VIF_FLAG_MAC_PROXY,        "Proxy",    "MAC Requests Proxied Always"},
    {VIF_FLAG_ETREE_ROOT,       "Er",       "Etree Root"},
    {VIF_FLAG_MIRROR_NOTAG,     "Mn",       "Mirror without Vlan Tag"},
    {VIF_FLAG_HBS_LEFT,         "HbsL",     "HBS Left Intf"},
    {VIF_FLAG_HBS_RIGHT,        "HbsR",     "HBS Right Intf"},
    {VIF_FLAG_IGMP_ENABLED,     "Ig",       "Igmp Trap Enabled"},
    {VIF_FLAG_MAC_IP_LEARNING,  "Ml",       "MAC-IP Learning Enabled"},
};

static char *
vr_get_if_type_string(int t)
{
    switch (t) {
    case VIF_TYPE_HOST:
        return "Host";
    case VIF_TYPE_AGENT:
        return "Agent";
    case VIF_TYPE_PHYSICAL:
        return "Physical";
    case VIF_TYPE_VIRTUAL:
        return "Virtual";
    case VIF_TYPE_XEN_LL_HOST:
        return "XenLL";
    case VIF_TYPE_GATEWAY:
        return "Gateway";
    case VIF_TYPE_STATS:
        return "Stats";
    case VIF_TYPE_VIRTUAL_VLAN:
        return "Virtual(Vlan)";
    case VIF_TYPE_MONITORING:
        return "Monitoring";
    default:
        return "Invalid";
    }

    return NULL;
}

static unsigned int
vr_get_if_type(char *type_str)
{
    if (!strncmp(type_str, VHOST_TYPE_STRING,
                strlen(VHOST_TYPE_STRING)))
        return VIF_TYPE_HOST;
    else if (!strncmp(type_str, AGENT_TYPE_STRING,
                strlen(AGENT_TYPE_STRING)))
        return VIF_TYPE_AGENT;
    else if (!strncmp(type_str, PHYSICAL_TYPE_STRING,
                strlen(PHYSICAL_TYPE_STRING)))
        return VIF_TYPE_PHYSICAL;
    else if (!strncmp(type_str, VIRTUAL_VLAN_TYPE_STRING,
                strlen(VIRTUAL_VLAN_TYPE_STRING)))
        return VIF_TYPE_VIRTUAL_VLAN;
    else if (!strncmp(type_str, VIRTUAL_TYPE_STRING,
                strlen(VIRTUAL_TYPE_STRING)))
        return VIF_TYPE_VIRTUAL;
    else if (!strncmp(type_str, XEN_LL_TYPE_STRING,
                strlen(XEN_LL_TYPE_STRING)))
        return VIF_TYPE_XEN_LL_HOST;
    else if (!strncmp(type_str, GATEWAY_TYPE_STRING,
                strlen(GATEWAY_TYPE_STRING)))
        return VIF_TYPE_GATEWAY;
    else if (!strncmp(type_str, STATS_TYPE_STRING,
                strlen(STATS_TYPE_STRING)))
        return VIF_TYPE_STATS;
    else if (!strncmp(type_str, MONITORING_TYPE_STRING,
                strlen(MONITORING_TYPE_STRING)))
        return VIF_TYPE_MONITORING;
    else
        Usage();

    return 0;
}

static unsigned int
vr_get_if_transport(char *transport_str)
{
    if (!strncmp(transport_str, ETH_TRANSPORT_STRING,
                strlen(ETH_TRANSPORT_STRING)))
        return VIF_TRANSPORT_ETH;
    else if (!strncmp(transport_str, PMD_TRASPORT_STRING,
                strlen(PMD_TRASPORT_STRING)))
        return VIF_TRANSPORT_PMD;
    else if (!strncmp(transport_str, VIRTUAL_TRANSPORT_STRING,
                strlen(VIRTUAL_TRANSPORT_STRING)))
        return VIF_TRANSPORT_VIRTUAL;
    else if (!strncmp(transport_str, SOCKET_TRANSPORT_STRING,
                strlen(SOCKET_TRANSPORT_STRING)))
        return VIF_TRANSPORT_SOCKET;
    else
        Usage();

    return 0;
}

static char *
vr_if_flags(int flags)
{
    unsigned int i, array_size;
    unsigned int all_len = 0;
    memset(flag_string, 0, sizeof(flag_string));

    array_size = sizeof(flag_metadata) / sizeof(flag_metadata[0]);
    for (i = 0; i < array_size; i++) {
      if (flags & flag_metadata[i].vuf_flag) {
        unsigned int flag_len;
        flag_len = strlen(flag_metadata[i].vuf_flag_symbol);
        if (all_len + flag_len < sizeof(flag_string)) {
          strcat(flag_string, flag_metadata[i].vuf_flag_symbol);
          all_len += flag_len;
        }
        else
          break;
      }
    }

    return flag_string;
}

static void
vr_interface_print_header(void)
{
    unsigned int i, array_size;

    array_size = sizeof(flag_metadata) / sizeof(flag_metadata[0]);

    printf("Vrouter Interface Table\n\n");

    printf("Flags: ");

    for (i = 0; i < array_size; i++) {
        if (i) {
            if (!(i % 4))
                printf("\n       ");
            else
                printf(", ");
        }
        printf("%s=%s", flag_metadata[i].vuf_flag_symbol,
                flag_metadata[i].vuf_flag_string);
    }

    printf("\n\n");
    return;
}

static void
drop_stats_req_process(void *s_req)
{
    vr_drop_stats_req *stats = (vr_drop_stats_req *)s_req;
    vr_print_drop_stats(stats, core);

    return;
}

static void
vrf_assign_req_process(void *s)
{
    vr_vrf_assign_req *req = (vr_vrf_assign_req *)s;

    printf("%d:%d, ", req->var_vlan_id, req->var_vif_vrf);
    var_marker = req->var_vlan_id;

    return;
}

static int
vr_interface_print_head_space(void)
{
    int i;

    for (i = 0; i < 12; i++)
        printf(" ");
    return i;
}

char *
vr_if_transport_string(vr_interface_req *req)
{
    switch (req->vifr_transport) {
    case VIF_TRANSPORT_VIRTUAL:
        return "Virtual";
        break;

    case VIF_TRANSPORT_ETH:
        return "Ethernet";
        break;

    case VIF_TRANSPORT_PMD:
        return "PMD";
        break;

    case VIF_TRANSPORT_SOCKET:
        return "Socket";
        break;

    default:
        break;
    }

    return "Unknown";
}

static void
vr_interface_core_print(void)
{
    if (core != (unsigned)-1) {
        printf("Core %u ", core);
    }
}

static void
vr_interface_nombufs_print(uint64_t nombufs)
{
    if (nombufs)
        printf(" no mbufs:%" PRId64, nombufs);
    printf("\n");
}

static void
vr_interface_pbem_counters_print(const char *title, bool print_always,
            uint64_t packets, uint64_t bytes, uint64_t errors,
            uint64_t nombufs)
{
    if (print_always || packets || bytes || errors) {
        vr_interface_print_head_space();
        vr_interface_core_print();
        printf("%s packets:%" PRId64 "  bytes:%" PRId64 " errors:%" PRId64,
                title, packets, bytes, errors);
        vr_interface_nombufs_print(nombufs);
    }
}

static void
vr_interface_pesm_counters_print(const char *title, bool print_always,
            uint64_t packets, uint64_t errors, uint64_t syscalls,
            uint64_t nombufs)
{
    if (print_always || packets || errors) {
        vr_interface_print_head_space();
        vr_interface_core_print();
        printf("%s packets:%" PRId64 " errors:%" PRId64,
                title, packets, errors);
        if (syscalls)
            printf(" syscalls:%" PRId64, syscalls);
        vr_interface_nombufs_print(nombufs);
    }
}

static void
vr_interface_pe_counters_print(const char *title, bool print_always,
            uint64_t packets, uint64_t errors)
{
    if (print_always || packets || errors) {
        vr_interface_print_head_space();
        vr_interface_core_print();
        printf("%s packets:%" PRId64 " errors:%" PRId64 "\n",
                title, packets, errors);
    }
}

static void
vr_interface_e_per_lcore_counters_print(const char *title, bool print_always,
            uint64_t *errors, uint32_t size)
{
    unsigned int i;

    vr_interface_print_head_space();
    printf("%s errors to lcore", title);
    for (i = 0; i < size; i++) {
        printf(" %" PRId64 , errors[i]);
    }
    printf("\n");
}

/* Display Fabric(Master) & bond Slave information only on DPDK platforms */
static void
vr_interface_fabric_info(vr_interface_req *req)
{

    int i;
    char *p_name, *p_drv_name ;
    const char *fabric_link[] = {"DOWN", "UP"};

    if(req->vifr_type != VIF_TYPE_PHYSICAL)
        return;

    vr_interface_print_head_space();
    printf("Fabric Interface: %s  Status: %s  Driver: %s\n",
            req->vifr_fab_name, fabric_link[(req->vifr_intf_status &
                0x01)], req->vifr_fab_drv_name);

    p_name = req->vifr_bond_slave_name;
    p_drv_name = req->vifr_bond_slave_drv_name;

    for(i = 0; i < req->vifr_num_bond_slave; i++) {
        vr_interface_print_head_space();
        printf("Slave Interface(%d): %s  Status: %s  Driver: %s\n",
                i, p_name, fabric_link[(req->vifr_intf_status >>
                    (i + 1)) & 0x01], p_drv_name);

        p_name = strchr(p_name, '\0'); p_name++;
        p_drv_name = strchr(p_drv_name, '\0'); p_drv_name++;
    }
}

/* Display VLAN ID & VLAN fwd interface only on DPDK platforms */
static void
vr_interface_vlan_info(vr_interface_req *req)
{
    if(req->vifr_type != VIF_TYPE_PHYSICAL)
        return;

    if(req->vifr_vlan_tag != VLAN_ID_INVALID && req->vifr_vlan_name != NULL ) {
        vr_interface_print_head_space();
        printf("Vlan Id: %d",req->vifr_vlan_tag);
        if(strlen(req->vifr_vlan_name))
            printf(" VLAN fwd Interface: %s",req->vifr_vlan_name);
        printf("\n");
    }
}

static void
list_get_print(vr_interface_req *req)
{
    char ip6_addr[INET6_ADDRSTRLEN], ip_addr[INET_ADDRSTRLEN],
         name[50] = {0}, ip6_ip[16];
    bool print_zero = false;
    uint16_t proto, port, port_data;
    int printed = 0, len;
    unsigned int i;
    uint64_t *tmp;
    uint8_t aggr_data;
    uint32_t *ip;

    if (rate_set) {
        print_zero = true;
    }

    printed = printf("vif%d/%d", req->vifr_rid, req->vifr_idx);
    for (; printed < 12; printed++)
        printf(" ");

    if (req->vifr_flags & VIF_FLAG_PMD) {
        printf("PMD: %d", req->vifr_os_idx);
    } else if (platform == DPDK_PLATFORM || platform == VTEST_PLATFORM) {
        switch (req->vifr_type) {
            case VIF_TYPE_PHYSICAL:
                if(req->vifr_flags & VIF_FLAG_MOCK_DEVICE)
                    printf("PCI: Mock");
                else
                    printf("PCI: ""%.4" PRIx16 ":%.2" PRIx8 ":%.2" PRIx8 ".%" PRIx8,
                            (uint16_t)(req->vifr_os_idx >> 16),
                            (uint8_t)(req->vifr_os_idx >> 8) & 0xFF,
                            (uint8_t)(req->vifr_os_idx >> 3) & 0x1F,
                            (uint8_t)(req->vifr_os_idx & 0x7));
                break;

            case VIF_TYPE_MONITORING:
                printf("Monitoring: %s for vif%d/%d", req->vifr_name,
                        req->vifr_rid, req->vifr_os_idx);
                break;

            default:
                if (req->vifr_name)
                    printf("%s: %s", vr_if_transport_string(req),
                            req->vifr_name);
                if (req->vifr_flags & VIF_FLAG_MOCK_DEVICE)
                    printf(" Mock");
                break;
        }

    } else {
        if (req->vifr_os_idx > 0) {
            printf("OS: %s", if_indextoname(req->vifr_os_idx, name));
        } else {
            printf("    %s", req->vifr_name);
        }
    }

    if ((req->vifr_type == VIF_TYPE_PHYSICAL) &&
            (!(req->vifr_flags & VIF_FLAG_MOCK_DEVICE))) {
        if (req->vifr_speed >= 0) {
            printf(" (Speed %d,", req->vifr_speed);
            if (req->vifr_duplex >= 0)
                printf(" Duplex %d", req->vifr_duplex);
            printf(")");
        }
    } else if (req->vifr_type == VIF_TYPE_VIRTUAL_VLAN) {
        printf(" Vlan(o/i)(,S): %d/%d", req->vifr_ovlan_id, req->vifr_vlan_id);
        if (req->vifr_src_mac_size && req->vifr_src_mac) {
            for (i = 0; i < (req->vifr_src_mac_size / VR_ETHER_ALEN); i = i + 1) {
                printf(", "MAC_FORMAT, MAC_VALUE((uint8_t *)
                            (req->vifr_src_mac + (i * VR_ETHER_ALEN))));
                printf(" Bridge Index: %d", req->vifr_bridge_idx[i]);
            }
        }
    }

    if (req->vifr_parent_vif_idx >= 0)
        printf(" Parent:vif0/%d", req->vifr_parent_vif_idx);

    if (req->vifr_nh_id != 0)
        printf(" NH: %d", req->vifr_nh_id);

    printf("\n");

    vr_interface_print_head_space();
    printf("Type:%s HWaddr:"MAC_FORMAT" IPaddr:%s\n",
            vr_get_if_type_string(req->vifr_type),
            MAC_VALUE((uint8_t *)req->vifr_mac), inet_ntop(AF_INET,
                &req->vifr_ip, ip_addr, INET_ADDRSTRLEN));
    if (req->vifr_ip6_u || req->vifr_ip6_l) {
        tmp = (uint64_t *)ip6_ip;
        *tmp = req->vifr_ip6_u;
        *(tmp + 1) = req->vifr_ip6_l;
        vr_interface_print_head_space();
        printf("IP6addr:%s\n", inet_ntop(AF_INET6, ip6_ip, ip6_addr,
                                                    INET6_ADDRSTRLEN));
    }
    vr_interface_print_head_space();
    printf("Vrf:%d Mcast Vrf:%d Flags:%s QOS:%d Ref:%d", req->vifr_vrf,
            req->vifr_mcast_vrf, req->vifr_flags ?
            vr_if_flags(req->vifr_flags) : "NULL" ,
            req->vifr_qos_map_index, req->vifr_ref_cnt);
    if (req->vifr_flags & (VIF_FLAG_MIRROR_TX | VIF_FLAG_MIRROR_RX)) {
        printf(" Mirror index %d\n", req->vifr_mir_id);
    } else {
        printf("\n");
    }

    if (platform == DPDK_PLATFORM || platform == VTEST_PLATFORM) {
        vr_interface_pbem_counters_print("RX device", print_zero,
                req->vifr_dev_ipackets, req->vifr_dev_ibytes,
                req->vifr_dev_ierrors, req->vifr_dev_inombufs);
        vr_interface_pesm_counters_print("RX port  ", print_zero,
                req->vifr_port_ipackets, req->vifr_port_ierrors,
                req->vifr_port_isyscalls, req->vifr_port_inombufs);
        vr_interface_pe_counters_print("RX queue ", print_zero,
                req->vifr_queue_ipackets, req->vifr_queue_ierrors);

        vr_interface_e_per_lcore_counters_print("RX queue", print_zero,
                req->vifr_queue_ierrors_to_lcore,
                req->vifr_queue_ierrors_to_lcore_size);
        /* Bond Master(Fabric)/slave, VLAN info not valid on vtest platforms */
        if(platform != VTEST_PLATFORM) {
            vr_interface_fabric_info(req);
            vr_interface_vlan_info(req);
        }
    }

    vr_interface_pbem_counters_print("RX", true, req->vifr_ipackets,
            req->vifr_ibytes, req->vifr_ierrors, 0);
    vr_interface_pbem_counters_print("TX", true, req->vifr_opackets,
            req->vifr_obytes, req->vifr_oerrors, 0);
    if (req->vifr_isid || req->vifr_pbb_mac_size) {
        vr_interface_print_head_space();
        printf("ISID: %d Bmac: "MAC_FORMAT"\n",
                req->vifr_isid, MAC_VALUE((uint8_t *)req->vifr_pbb_mac));
    }
    vr_interface_print_head_space();
    printf("Drops:%" PRIu64 "\n", req->vifr_dpackets);


    if (req->vifr_in_mirror_md_size) {
        printed = vr_interface_print_head_space();
        len = printf("Ingress Mirror Metadata: ");
        printed += len;
        for (i = 0; i < req->vifr_in_mirror_md_size; i++) {
            printed += printf("%x ", 0xFF & req->vifr_in_mirror_md[i]);
            if (printed > 68) {
                printf("\n");
                printed = vr_interface_print_head_space();
                printf("%*c", len, ' ');
                printed += len;
            }
        }
        printf("\n");
    }

    if (req->vifr_out_mirror_md_size) {
        printed = vr_interface_print_head_space();
        len = printf("Egress Mirror Metadata: ");
        printed += len;
        for (i = 0; i < req->vifr_out_mirror_md_size; i++) {
            printed += printf("%x ", 0xFF & req->vifr_out_mirror_md[i]);
            if (printed > 68) {
                printf("\n");
                printed = vr_interface_print_head_space();
                printf("%*c", len, ' ');
                printed += len;
            }
        }
        printf("\n");

    }

    if (platform == DPDK_PLATFORM || platform == VTEST_PLATFORM) {
        vr_interface_pe_counters_print("TX queue ", print_zero,
                req->vifr_queue_opackets, req->vifr_queue_oerrors);
        vr_interface_pesm_counters_print("TX port  ", print_zero,
                req->vifr_port_opackets, req->vifr_port_oerrors,
                req->vifr_port_osyscalls, 0);
        vr_interface_pbem_counters_print("TX device", print_zero,
                req->vifr_dev_opackets, req->vifr_dev_obytes,
                req->vifr_dev_oerrors, 0);
    }

    if (req->vifr_fat_flow_protocol_port_size) {
        printf("\n");
        vr_interface_print_head_space();
        printf("FatFlow rules: \n");
        for (i = 0; i < req->vifr_fat_flow_protocol_port_size; i++) {
            proto = VIF_FAT_FLOW_PROTOCOL(req->vifr_fat_flow_protocol_port[i]);
            port = VIF_FAT_FLOW_PORT(req->vifr_fat_flow_protocol_port[i]);
            port_data = VIF_FAT_FLOW_PORT_DATA(req->vifr_fat_flow_protocol_port[i]);
            aggr_data = VIF_FAT_FLOW_PREFIX_AGGR_DATA(req->vifr_fat_flow_protocol_port[i]);
            if (!proto) {
                proto = port;
                port = 0;
            }

            vr_interface_print_head_space();
            printf("\t");
            printf("%d:", proto);
            if (port) {
                printf("%d ", port);
            } else {
                printf("%c", '*');
            }
            if (port_data == VIF_FAT_FLOW_PORT_SIP_IGNORE)
                printf(" - Sip");
            if (port_data == VIF_FAT_FLOW_PORT_DIP_IGNORE)
                printf(" - Dip");

            switch (aggr_data) {
                case VR_AGGREGATE_SRC_IPV4:
                     ip = (uint32_t *) &req->vifr_fat_flow_src_prefix_l[i];
                     printf(" AggrSrc %s/%d %d",
                            inet_ntop(AF_INET, ip, ip_addr, INET_ADDRSTRLEN),
                            req->vifr_fat_flow_src_prefix_mask[i],
                            req->vifr_fat_flow_src_aggregate_plen[i]);
                     break;
                case VR_AGGREGATE_DST_IPV4:
                     ip = (uint32_t *) &req->vifr_fat_flow_dst_prefix_l[i];
                     printf(" AggrDst %s/%d %d",
                            inet_ntop(AF_INET, ip, ip_addr, INET_ADDRSTRLEN),
                            req->vifr_fat_flow_dst_prefix_mask[i],
                            req->vifr_fat_flow_dst_aggregate_plen[i]);
                     break;
                case VR_AGGREGATE_SRC_DST_IPV4:
                     ip = (uint32_t *) &req->vifr_fat_flow_src_prefix_l[i];
                     printf(" AggrSrc %s/%d %d",
                            inet_ntop(AF_INET, ip, ip_addr, INET_ADDRSTRLEN),
                            req->vifr_fat_flow_src_prefix_mask[i],
                            req->vifr_fat_flow_src_aggregate_plen[i]);
                     ip = (uint32_t *) &req->vifr_fat_flow_dst_prefix_l[i];
                     printf(" AggrDst %s/%d %d",
                            inet_ntop(AF_INET, ip, ip_addr, INET_ADDRSTRLEN),
                            req->vifr_fat_flow_dst_prefix_mask[i],
                            req->vifr_fat_flow_dst_aggregate_plen[i]);
                     break;
                case VR_AGGREGATE_SRC_IPV6:
                     tmp = (uint64_t *)ip6_ip;
                     *tmp = req->vifr_fat_flow_src_prefix_h[i];
                     *(tmp + 1) = req->vifr_fat_flow_src_prefix_l[i];
                     printf(" AggrSrc %s/%d %d",
                            inet_ntop(AF_INET6, ip6_ip, ip6_addr, INET6_ADDRSTRLEN),
                            req->vifr_fat_flow_src_prefix_mask[i],
                            req->vifr_fat_flow_src_aggregate_plen[i]);
                     break;
                case VR_AGGREGATE_DST_IPV6:
                     tmp = (uint64_t *)ip6_ip;
                     *tmp = req->vifr_fat_flow_dst_prefix_h[i];
                     *(tmp + 1) = req->vifr_fat_flow_dst_prefix_l[i];
                     printf(" AggrDst %s/%d %d",
                            inet_ntop(AF_INET6, ip6_ip, ip6_addr, INET6_ADDRSTRLEN),
                            req->vifr_fat_flow_dst_prefix_mask[i],
                            req->vifr_fat_flow_dst_aggregate_plen[i]);
                     break;
                case VR_AGGREGATE_SRC_DST_IPV6:
                     tmp = (uint64_t *)ip6_ip;
                     *tmp = req->vifr_fat_flow_src_prefix_h[i];
                     *(tmp + 1) = req->vifr_fat_flow_src_prefix_l[i];
                     printf(" AggrSrc %s/%d %d",
                            inet_ntop(AF_INET6, ip6_ip, ip6_addr, INET6_ADDRSTRLEN),
                            req->vifr_fat_flow_src_prefix_mask[i],
                            req->vifr_fat_flow_src_aggregate_plen[i]);
                     tmp = (uint64_t *)ip6_ip;
                     *tmp = req->vifr_fat_flow_dst_prefix_h[i];
                     *(tmp + 1) = req->vifr_fat_flow_dst_prefix_l[i];
                     printf(" AggrDst %s/%d %d",
                            inet_ntop(AF_INET6, ip6_ip, ip6_addr, INET6_ADDRSTRLEN),
                            req->vifr_fat_flow_dst_prefix_mask[i],
                            req->vifr_fat_flow_dst_aggregate_plen[i]);
                     break;
                default:
                     break;
            }

            printf("\n");
        }
    }
    printf("\n");
    if (req->vifr_fat_flow_exclude_ip_list_size) {
        vr_interface_print_head_space();
        printf("FatFlows IPv4 exclude prefix list:\n");
        for (i = 0; i < req->vifr_fat_flow_exclude_ip_list_size; i++) {
             vr_interface_print_head_space();
             printf("\t%s\n", inet_ntop(AF_INET, &req->vifr_fat_flow_exclude_ip_list[i], ip_addr, INET_ADDRSTRLEN));
        }
        printf("\n");
    }
    if (req->vifr_fat_flow_exclude_ip6_u_list_size) {
        vr_interface_print_head_space();
        printf("FatFlows IPv6 exclude prefix list:\n");
        for (i = 0; i < req->vifr_fat_flow_exclude_ip6_u_list_size; i++) {
             tmp = (uint64_t *)ip6_ip;
             *tmp = req->vifr_fat_flow_exclude_ip6_u_list[i];
             *(tmp + 1) = req->vifr_fat_flow_exclude_ip6_l_list[i];
             vr_interface_print_head_space();
             printf("\t%s\n", inet_ntop(AF_INET6, ip6_ip, ip6_addr, INET6_ADDRSTRLEN));
        }
        printf("\n");
    } 

    if (get_set && req->vifr_flags & VIF_FLAG_SERVICE_IF) {
        vr_vrf_assign_dump = true;
        dump_pending = true;
        printf("VRF table(vlan:vrf):\n");
        vr_ifindex = req->vifr_idx;
    }

    return;
}

static void
list_header_print(void)
{
    int printed = 0;

    printed = printf("Interface name");
    for (; printed < 30; printed++)
        printf(" ");

    printed = printf("VIF ID");
    for (; printed < 30; printed++)
        printf(" ");

    printed = printf("RX");
    for (; printed < 30; printed++)
        printf(" ");

    printed = printf("TX");
    for (; printed < 30; printed++)
        printf(" ");

    printf("\n");

    printed = strlen("Errors");
    for (; printed < 30 * 2; printed++)
        printf(" ");

    printed = printf("Errors   Packets");
    for (; printed < 30; printed++)
        printf(" ");

    printf("Errors   Packets");

    printf("\n\n");
}

static void
list_rate_print(vr_interface_req *req)
{
    int printed = 0;
    uint64_t tx_errors = 0;
    uint64_t rx_errors = 0;
    unsigned int i = 0;

    rx_errors = (req->vifr_dev_ierrors + req->vifr_port_ierrors + req->vifr_queue_ierrors
                 + req->vifr_ierrors);
    tx_errors = (req->vifr_dev_oerrors + req->vifr_port_oerrors + req->vifr_queue_oerrors
                 + req->vifr_oerrors);

    printed = printf("%s: %s", vr_get_if_type_string(req->vifr_type),
                        req->vifr_name);
    for (; printed < 30; printed++)
        printf(" ");
    printed = printf("vif%d/%d", req->vifr_rid, req->vifr_idx);
    for (; printed < 24; printed++)
        printf(" ");

    printed = printf("%-7"PRIu64 "  %-7"PRIu64, rx_errors, req->vifr_ipackets);
    for (; printed < 30; printed++)
        printf(" ");

    printed = printf("%-7"PRIu64 "  %-7"PRIu64, tx_errors, req->vifr_opackets);
    for (; printed < 25; printed++)
        printf(" ");
    printf("\n\n\n");
    return;
}

static void
rate_process(vr_interface_req *req, vr_interface_req *prev_req)
{
    vr_interface_req rate_req_temp = {0};
    uint64_t *temp_prev_req_ptr = NULL;

    if (first_rate_iter) {
        temp_prev_req_ptr = prev_req->vifr_queue_ierrors_to_lcore;
        *prev_req = *req;
        prev_req->vifr_queue_ierrors_to_lcore = temp_prev_req_ptr;
        if (!prev_req->vifr_queue_ierrors_to_lcore) {
            prev_req->vifr_queue_ierrors_to_lcore =
                malloc(req->vifr_queue_ierrors_to_lcore_size * sizeof(uint64_t));
            if (!prev_req->vifr_queue_ierrors_to_lcore)
                return;
        }

        memcpy(prev_req->vifr_queue_ierrors_to_lcore,
            req->vifr_queue_ierrors_to_lcore,
            req->vifr_queue_ierrors_to_lcore_size * sizeof(uint64_t));
        rate_stats_diff(req, prev_req);
        return;
    }

    rate_req_temp = *req;
    rate_req_temp.vifr_queue_ierrors_to_lcore =
        calloc(req->vifr_queue_ierrors_to_lcore_size, sizeof(uint64_t));

    if (!rate_req_temp.vifr_queue_ierrors_to_lcore) {
        fprintf(stderr, "Fail, memory allocation. (%s:%d).", __FILE__ , __LINE__);
        exit(1);
    }

    memcpy(rate_req_temp.vifr_queue_ierrors_to_lcore,
            req->vifr_queue_ierrors_to_lcore,
            req->vifr_queue_ierrors_to_lcore_size * sizeof(uint64_t));

    rate_stats_diff(req, prev_req);

    temp_prev_req_ptr = prev_req->vifr_queue_ierrors_to_lcore;
    *prev_req = rate_req_temp;
    prev_req->vifr_queue_ierrors_to_lcore = temp_prev_req_ptr;

    memcpy(prev_req->vifr_queue_ierrors_to_lcore,
            rate_req_temp.vifr_queue_ierrors_to_lcore,
            rate_req_temp.vifr_queue_ierrors_to_lcore_size * sizeof(uint64_t));

    if ((rate_req_temp.vifr_queue_ierrors_to_lcore)) {
        free(rate_req_temp.vifr_queue_ierrors_to_lcore);
        rate_req_temp.vifr_queue_ierrors_to_lcore = NULL;
    }
}

/*
 * The function is called by functions sandesh_decode.
 * In case, when we have sent SANDESH_OP_DUMP (usually --list parameter) msg to nl_client,
 * then sandesh_decode calls vr_interface_req_process in "loop".
 * Variable dump_marker (dump_marker < next_interface.vif_id) sets which
 * interface is successor.
 *
 * For SANDESH_OP_DUMP msg we SHOULD change variable dump_marker;
 * Otherwise we can be in infinity loop.
 */
static void
interface_req_process(void *s)
{
    vr_interface_req *req = (vr_interface_req *)s;

    if(req->h_op == SANDESH_OP_RESET) {
        if(req->vifr_idx == -1) {
            if(req->vifr_core != 0) {
                printf("\nVif stats cleared successfully on core %d for all interfacess \n\n",
                        req->vifr_core-1);
            } else {
                printf("\nVif stats cleared successfully on all cores for all interfaces \n\n");
            }
        } else {
            if(req->vifr_core != 0) {
                printf("\nVif stats cleared successfully for %s on core %d \n\n",
                        req->vifr_name, req->vifr_core-1);
            } else {
                printf("\nVif stats cleared successfully for %s on all cores \n\n",
                        req->vifr_name);
            }
        }
        return;
    }

    if (add_set)
        vr_ifindex = req->vifr_idx;

    if (!get_set && !list_set)
        return;

    if (rate_set) {
        /* Compute for each "current" vif interfaces. */
        rate_process(req, &prev_req[req->vifr_idx % VR_MAX_INTERFACES]);

        if (list_set) {
            /*
             * We are in loop (which cannot be controlled by us)
             * (see function comment)
             *
             * Ignores first interfaces outputs.
             */
            if (ignore_number_interface > 0) {
                ignore_number_interface--;
                /*
                 * How many interface we should print
                 * Value of variable number_interface is computed:
                 * (get_terminal_lines - header_lines)/(lines_per_interface)
                 */
            } else if (print_number_interface >= 1) {
                list_rate_print(req);
                print_number_interface--;
            }
            /* Mandatory, otherwise we can be in infinity loop.*/
            dump_marker = req->vifr_idx;
            return;
        }
    }
    list_get_print(req);
    if (list_set){

        dump_marker = req->vifr_idx;
    }

    return;
}

static void
response_process(void *s)
{
    vr_response_common_process((vr_response *)s, &dump_pending);
    return;
}

static void
vif_fill_nl_callbacks()
{
    nl_cb.vr_drop_stats_req_process = drop_stats_req_process;
    nl_cb.vr_vrf_assign_req_process = vrf_assign_req_process;
    nl_cb.vr_interface_req_process = interface_req_process;
    nl_cb.vr_response_process = response_process;
}

/*
 * create vhost interface in linux
 */
static int
vhost_create(void)
{
    int ret;
#if defined(__linux__)
    struct vn_if vhost;
    struct nl_response *resp;

    memset(&vhost, 0, sizeof(vhost));
    strncpy(vhost.if_name, if_name, sizeof(vhost.if_name) - 1);
    strncpy(vhost.if_kind, VHOST_KIND, sizeof(vhost.if_kind) - 1);
    memcpy(vhost.if_mac, vr_ifmac, sizeof(vhost.if_mac));
    ret = nl_build_if_create_msg(cl, &vhost, 0);
    if (ret)
        return ret;

    ret = nl_sendmsg(cl);
    if (ret <= 0)
        return ret;

    if ((ret = nl_recvmsg(cl)) > 0) {
        resp = nl_parse_reply(cl);
        if (resp && resp->nl_op)
            printf("%s: %s\n", __func__, strerror(resp->nl_op));
    }
#else
#error "Unsupported platform"
#endif
    return ret;
}

static int
vr_intf_op(struct nl_client *cl, unsigned int op)
{
    int ret = -EINVAL, vrf;
    bool dump = false;

    if (create_set)
        return vhost_create();

    if ((op == SANDESH_OP_DUMP &&  !(rate_set)) ||
            ((op == SANDESH_OP_GET) && !(add_set) )) {
        vr_interface_print_header();
    } else if (rate_set) {
       list_header_print();
    }

op_retry:
    switch (op) {
    case SANDESH_OP_ADD:
        if (set_set)
            vrf = -1;
        else
            vrf = vrf_id;

        if (if_kindex < 0)
            if_kindex = 0;

        if (vindex_set)
            vr_ifindex = vif_index;

        if (vr_ifindex < 0)
            vr_ifindex = if_kindex;

        ret = vr_send_interface_add(cl, 0, if_name, if_kindex, vr_ifindex,
                if_xconnect_kindex, vr_if_type, vrf, vr_ifflags, vr_ifmac, vr_transport, NULL);
        break;

    case SANDESH_OP_DEL:
        ret = vr_send_interface_delete(cl, 0, if_name, vr_ifindex);
        break;

    case SANDESH_OP_GET:
        /**
         * Implementation of getting per-core vif statistics is based on this
         * little trick to avoid making changes in how agent makes requests for
         * statistics. From vRouter's and agent's point of view, request for
         * stats for 0th core means a request for stats summed up for all the
         * cores. So cores are enumerated starting with 1.
         * Meanwhile, from user's point of view they are enumerated starting
         * with 0 (e.g. vif --list --core 0 means 'vif statistics for the very
         * first (0th) core'). This is how Linux enumerates CPUs, so it should
         * be more intuitive for the user.
         *
         * Agent is not aware of possibility of asking for per-core stats. Its
         * requests have vifr_core implicitly set to 0. So we need to make a
         * conversion between those enumerating systems. The vif utility
         * increments by 1 the core number user asked for. Then it is
         * decremented back in vRouter.
         */
        if (!vr_vrf_assign_dump) {
            ret = vr_send_interface_get(cl, 0, vr_ifindex, if_kindex,
                    core + 1, drop_set);
        } else {
            dump = true;
            ret = vr_send_vrf_assign_dump(cl, 0, vr_ifindex, var_marker);
        }
        break;

    case SANDESH_OP_DUMP:
        dump = true;
        ret = vr_send_interface_dump(cl, 0, dump_marker, core + 1);
        break;

    case SANDESH_OP_RESET:
        ret = vr_send_vif_clear_stats(cl, 0, vif_index, core);
        break;
    }


    if (ret < 0)
        return ret;


    ret = vr_recvmsg(cl, dump);
    if (ret <= 0)
        return ret;

    if (set_set) {
        ret = vr_send_vrf_assign_set(cl, 0, vr_ifindex, vlan_id, vrf_id);
        if (ret < 0)
            return ret;

        return vr_recvmsg(cl, dump);
    }

    if (dump_pending) {
        goto op_retry;
    }

    return 0;
}

static void
usage_internal(const char *msg)
{
    printf("Invalid arguments for %s \n", msg);
    exit(0);
}

static void
Usage()
{
    printf("Usage: vif [--create <intf_name> --mac <mac>]\n");
    printf("\t   [--add <intf_name> --mac <mac> --vrf <vrf>\n");
    printf("\t   \t--type [vhost|agent|physical|virtual|monitoring]\n");
    printf("\t   \t--transport [eth|pmd|virtual|socket]\n");
    printf("\t   \t--xconnect <physical interface name>\n");
    printf("\t   \t--policy, --vhost-phys, --dhcp-enable]\n");
    printf("\t   \t--vif <vif ID> --id <intf_id> --pmd --pci]\n");
    printf("\t   [--delete <intf_id>|<intf_name>]\n");
    printf("\t   [--get <intf_id>][--kernel][--core <core number>][--rate] [--get-drop-stats]\n");
    printf("\t   [--set <intf_id> --vlan <vlan_id> --vrf <vrf_id>]\n");
    printf("\t   [--list][--core <core number>][--rate]\n");
    printf("\t   [--sock-dir <sock dir>]\n");
    printf("\t   [--clear][--id <intf_id>][--core <core_number>]\n");
    printf("\t   [--help]\n");

    exit(0);
}


enum if_opt_index {
    ADD_OPT_INDEX,
    CREATE_OPT_INDEX,
    GET_OPT_INDEX,
    RATE_OPT_INDEX,
    DROP_OPT_INDEX,
    LIST_OPT_INDEX,
    VRF_OPT_INDEX,
    MAC_OPT_INDEX,
    DELETE_OPT_INDEX,
    POLICY_OPT_INDEX,
    PMD_OPT_INDEX,
    PCI_OPT_INDEX,
    KINDEX_OPT_INDEX,
    TYPE_OPT_INDEX,
    TRANSPORT_OPT_INDEX,
    SET_OPT_INDEX,
    VLAN_OPT_INDEX,
    XCONNECT_OPT_INDEX,
    VIF_OPT_INDEX,
    DHCP_OPT_INDEX,
    VHOST_PHYS_OPT_INDEX,
    HELP_OPT_INDEX,
    VINDEX_OPT_INDEX,
    CORE_OPT_INDEX,
    SOCK_DIR_OPT_INDEX,
    CLEAR_STATS_OPT_INDEX,
    MAX_OPT_INDEX
};

static struct option long_options[] = {
    [ADD_OPT_INDEX]         =   {"add",         required_argument,  &add_set,           1},
    [CREATE_OPT_INDEX]      =   {"create",      required_argument,  &create_set,        1},
    [GET_OPT_INDEX]         =   {"get",         required_argument,  &get_set,           1},
    [RATE_OPT_INDEX]        =   {"rate",        no_argument,        &rate_set,          1},
    [DROP_OPT_INDEX]        =   {"get-drop-stats",no_argument,      &drop_set,          1},
    [LIST_OPT_INDEX]        =   {"list",        no_argument,        &list_set,          1},
    [VRF_OPT_INDEX]         =   {"vrf",         required_argument,  &vrf_set,           1},
    [MAC_OPT_INDEX]         =   {"mac",         required_argument,  &mac_set,           1},
    [DELETE_OPT_INDEX]      =   {"delete",      required_argument,  &delete_set,        1},
    [POLICY_OPT_INDEX]      =   {"policy",      no_argument,        &policy_set,        1},
    [PMD_OPT_INDEX]         =   {"pmd",         no_argument,        &pmd_set,           1},
    [PCI_OPT_INDEX]         =   {"pci",         no_argument,        &pci_set,           1},
    [KINDEX_OPT_INDEX]      =   {"kernel",      no_argument,        &kindex_set,        1},
    [TYPE_OPT_INDEX]        =   {"type",        required_argument,  &type_set,          1},
    [TRANSPORT_OPT_INDEX]   =   {"transport",   required_argument,  &transport_set,     1},
    [SET_OPT_INDEX]         =   {"set",         required_argument,  &set_set,           1},
    [VLAN_OPT_INDEX]        =   {"vlan",        required_argument,  &vlan_set,          1},
    [VHOST_PHYS_OPT_INDEX]  =   {"vhost-phys",  no_argument,        &vhost_phys_set,    1},
    [XCONNECT_OPT_INDEX]    =   {"xconnect",    required_argument,  &xconnect_set,      1},
    [VIF_OPT_INDEX]         =   {"vif",         required_argument,  &vif_set,           1},
    [DHCP_OPT_INDEX]        =   {"dhcp-enable", no_argument,        &dhcp_set,          1},
    [HELP_OPT_INDEX]        =   {"help",        no_argument,        &help_set,          1},
    [VINDEX_OPT_INDEX]      =   {"id",          required_argument,  &vindex_set,        1},
    [CORE_OPT_INDEX]        =   {"core",        required_argument,  &core_set,          1},
    [SOCK_DIR_OPT_INDEX]    =   {"sock-dir",    required_argument,  &sock_dir_set,      1},
    [CLEAR_STATS_OPT_INDEX] =   {"clear",       no_argument,        &clear_stats_set,   1},
    [MAX_OPT_INDEX]         =   { NULL,         0,                  NULL,               0},
};


/* Safer than raw strtoul call that can segment fault with NULL strings.
   sets errno if any addtional errors are detected.*/
static unsigned long
safer_strtoul(const char *nptr, char **endptr, int base)
{
    if (nptr == NULL) {
        errno = EINVAL;
        return 0;
    } else {
        return strtoul(nptr, endptr, base);
    }
}

static int
is_number(const char *nptr)
{
    unsigned int i;
    if (nptr == NULL) {
        errno = EINVAL;
        return 0;
    } else {
        for (i = 0; i < strlen(nptr); i++) {
            if (!isdigit(nptr[i])) {
                return 0;
            }
        }
    }
    return 1;
}

static void
parse_long_opts(int option_index, char *opt_arg)
{
    int i = 0;
    char *sep_arg;
    errno = 0;
   int retVal = -1;
   

    if (!*(long_options[option_index].flag))
        *(long_options[option_index].flag) = 1;

    switch (option_index) {
        case ADD_OPT_INDEX:
            strncpy(if_name, opt_arg, sizeof(if_name) - 1);
            if_kindex = if_nametoindex(opt_arg);
            if (isdigit(opt_arg[0]))
                if_pmdindex = strtol(opt_arg, NULL, 0);
            vr_op = SANDESH_OP_ADD;
            break;

        case CREATE_OPT_INDEX:
            strncpy(if_name, opt_arg, sizeof(if_name) - 1);
            break;

        case VRF_OPT_INDEX:
            if (is_number(opt_arg))
                vrf_id = safer_strtoul(opt_arg, NULL, 0);
            else
                usage_internal("vif set --vrf");

            if (errno)
                Usage();
            break;

        case MAC_OPT_INDEX:
            mac_opt = ether_aton(opt_arg);
            if (mac_opt)
                memcpy(vr_ifmac, mac_opt, sizeof(vr_ifmac));
            break;

        case DELETE_OPT_INDEX:
            vr_op = SANDESH_OP_DEL;
            if (isdigit(opt_arg[0]))
                vr_ifindex = safer_strtoul(opt_arg, NULL, 0);
            else
                strncpy(if_name, opt_arg, sizeof(if_name) - 1);
            if (errno)
                Usage();
            break;

        case GET_OPT_INDEX:
            vr_op = SANDESH_OP_GET;
            if (is_number(opt_arg))
                vr_ifindex = safer_strtoul(opt_arg, NULL, 0);
            else
                usage_internal("vif --get");

            if (errno)
                Usage();
            break;

        case VIF_OPT_INDEX:
            /* we carry monitored vif index in OS index field */
            if (is_number(opt_arg))
                if_kindex = safer_strtoul(opt_arg, NULL, 0);
            else
                usage_internal("vif add --vif");

            if (errno)
                Usage();
            break;

        case VINDEX_OPT_INDEX:
            if (is_number(opt_arg))
                vif_index = safer_strtoul(opt_arg, NULL, 0);
            else
                usage_internal("vif add --id");

            if (errno)
                Usage();
            break;

        case POLICY_OPT_INDEX:
            vr_ifflags |= VIF_FLAG_POLICY_ENABLED;
            break;

        case PMD_OPT_INDEX:
            vr_ifflags |= VIF_FLAG_PMD;
            break;

        case LIST_OPT_INDEX:
            vr_op = SANDESH_OP_DUMP;
            break;

        case CORE_OPT_INDEX:
            core = (unsigned)strtol(opt_arg, NULL, 0);
            if (errno) {
                printf("Error parsing core %s: %s (%d)\n", opt_arg,
                        strerror(errno), errno);
                Usage();
            }
            break;

        case TYPE_OPT_INDEX:
            vr_if_type = vr_get_if_type(optarg);
            if (vr_if_type == VIF_TYPE_HOST)
                need_xconnect_if = true;
            if (vr_if_type == VIF_TYPE_MONITORING) {
                if (platform != DPDK_PLATFORM)
                    Usage();

                need_vif_id = true;
                /* set default values for mac and vrf */
                vrf_id = 0;
                vrf_set = 1;
                vr_ifmac[0] = 0x2; /* locally administered */
                mac_set = 1;
            }
            break;

         case TRANSPORT_OPT_INDEX:
            vr_transport = vr_get_if_transport(optarg);
            break;

        case SET_OPT_INDEX:
            vr_op = SANDESH_OP_ADD;
            if (is_number(opt_arg))
                vr_ifindex = safer_strtoul(opt_arg, NULL, 0);
            else
                usage_internal("vif --set");

            if (errno)
                Usage();
            break;

        case VLAN_OPT_INDEX:
            vr_ifflags |= VIF_FLAG_SERVICE_IF;
            if (is_number(opt_arg))
                vlan_id = safer_strtoul(opt_arg, NULL, 0);
            else
                usage_internal("vif set --vlan");
            if (errno)
                Usage();
            break;

        case XCONNECT_OPT_INDEX:
            opt_arg = strdup(opt_arg);
            while ((sep_arg = strsep(&opt_arg, ",")) != NULL) {
                if(platform == DPDK_PLATFORM) {
                    /* we need to check cross_connect index has be passed or
                     * not for kernel & DPDK based platforms */
                    /*Incase of DPDK platform, vif_idx is passed for xconnect */
                    if_xconnect_kindex[i++] = safer_strtoul(sep_arg, NULL, 0);
                } else {
                    retVal = if_nametoindex(sep_arg);
                    if(0 == retVal) {
                       break;
                    }
                    if_xconnect_kindex[i++] = retVal;
                }
                if (isdigit(sep_arg[0])) {
                    if_pmdindex = strtol(sep_arg, NULL, 0);
                } else if (!if_xconnect_kindex[i]) {
                    printf("%s does not seem to be a valid physical interface name\n",
                           sep_arg);
                    Usage();
                }
                if (if_pmdindex != -1)
                    break;
            }
            break;

        case DHCP_OPT_INDEX:
            vr_ifflags |= VIF_FLAG_DHCP_ENABLED;
            break;

        case VHOST_PHYS_OPT_INDEX:
            vr_ifflags |= VIF_FLAG_VHOST_PHYS;
            break;

        case SOCK_DIR_OPT_INDEX:
            vr_socket_dir = opt_arg;
            break;

        case CLEAR_STATS_OPT_INDEX:
            clear_stats_set = 1;
            vr_op = SANDESH_OP_RESET;
            break;
        default:
            break;
    }

    return;
}

static void
validate_options(void)
{
    unsigned int sum_opt = 0, i;

    for (i = 0; i < (sizeof(long_options) / sizeof(long_options[0])); i++) {
        if (long_options[i].flag)
            sum_opt += *(long_options[i].flag);
    }

    /*
     * Reduce sum_opt by 1 so that rest of the validation logic doesn't take
     * sock_dir_set into account
     */
    if (sock_dir_set) {
        sum_opt -=1;
    }

    if (!sum_opt || help_set)
        Usage();

    if (pmd_set || pci_set) {
        if_kindex = if_pmdindex;
        if_xconnect_kindex[0] = if_pmdindex;
    }

    if (create_set) {
        if ((sum_opt > 1) && (sum_opt != 2 || !mac_set))
            Usage();
        return;
    }
    if (get_set) {
        if ((sum_opt > 1) && (sum_opt != 3) && (sum_opt != 4) &&
                (!kindex_set && !core_set && !rate_set && !drop_set))
            Usage();
        return;
    }
    if (delete_set) {
        if (sum_opt > 1)
            Usage();
        return;
    }
    if (list_set) {
        if (drop_set)
            Usage();
        if (!core_set) {
            if (rate_set && !(sum_opt > 2))
                return;
            if (sum_opt > 1)
                Usage();
        } else {
            if(rate_set && !(sum_opt > 3))
               return;
            if (sum_opt != 2)
                Usage();
        }
        return;
    }

    if (add_set) {
        if (get_set || list_set)
            Usage();
        if (!vrf_set || !mac_set || !type_set)
            Usage();
        if (need_xconnect_if && !xconnect_set)
            Usage();
        if (need_vif_id && !vif_set)
            Usage();
        return;
    }

    if (set_set) {
        if (sum_opt != 3 || !vrf_set || !vlan_set)
            Usage();
        return;
    }

    /**
     * Statistics per CPU core could be requested as an additional parameter
     * to --list or --get.
     */
    if(core_set){
        if (clear_stats_set)
            return;
    }
    if (core_set) {
        if (!list_set || !get_set)
            Usage();
    }
    if (rate_set) {
        if (!get_set) {
            Usage();
        }
    }

    return;
}


static void
rate_stats_diff(vr_interface_req *req, vr_interface_req *prev_req)
{
    struct timeval now;
    int64_t diff_ms = 0;
    unsigned int i = 0;

    gettimeofday(&now, NULL);
    diff_ms = (now.tv_sec - last_time.tv_sec) * 1000;
    diff_ms += (now.tv_usec - last_time.tv_usec) / 1000;
    assert(diff_ms > 0);

    /* TODO:
     * Sometimes error counters have decreasing trend.
     *
     * Workaround:
     * If previous value is bigger than current value, then we assign
     * previous value to current value => difference is equal to 0
     */

    CORRECT_ERROR_CNT(&(req->vifr_dev_ierrors), &(prev_req->vifr_dev_ierrors));
    CORRECT_ERROR_CNT(&(req->vifr_port_ierrors), &(prev_req->vifr_port_ierrors));
    CORRECT_ERROR_CNT(&(req->vifr_queue_ierrors), &(prev_req->vifr_queue_ierrors));
    CORRECT_ERROR_CNT(&(req->vifr_ierrors), &(prev_req->vifr_ierrors));

    CORRECT_ERROR_CNT(&(req->vifr_dev_oerrors), &(prev_req->vifr_dev_oerrors));
    CORRECT_ERROR_CNT(&(req->vifr_port_oerrors), &(prev_req->vifr_port_oerrors));
    CORRECT_ERROR_CNT(&(req->vifr_queue_oerrors), &(prev_req->vifr_queue_oerrors));
    CORRECT_ERROR_CNT(&(req->vifr_oerrors), &(prev_req->vifr_oerrors));

    /* RX */
    COMPUTE_DIFFERENCE(req, prev_req, vifr_dev_ibytes, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_dev_ipackets, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_dev_ierrors, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_dev_inombufs, diff_ms);

    COMPUTE_DIFFERENCE(req, prev_req, vifr_port_isyscalls, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_port_ipackets, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_port_ierrors, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_port_inombufs, diff_ms);

    COMPUTE_DIFFERENCE(req, prev_req, vifr_queue_ierrors, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_queue_ipackets, diff_ms);

    for (i = 0; i < req->vifr_queue_ierrors_to_lcore_size; i++) {
        COMPUTE_DIFFERENCE(req, prev_req, vifr_queue_ierrors_to_lcore[i], diff_ms);
    }

    COMPUTE_DIFFERENCE(req, prev_req, vifr_ibytes, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_ipackets, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_ierrors, diff_ms);

    /* TX */
    COMPUTE_DIFFERENCE(req, prev_req, vifr_obytes, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_opackets, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_oerrors, diff_ms);

    COMPUTE_DIFFERENCE(req, prev_req, vifr_queue_oerrors, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_queue_opackets, diff_ms);

    COMPUTE_DIFFERENCE(req, prev_req, vifr_port_osyscalls, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_port_opackets, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_port_oerrors, diff_ms);

    COMPUTE_DIFFERENCE(req, prev_req, vifr_dev_obytes, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_dev_opackets, diff_ms);
    COMPUTE_DIFFERENCE(req, prev_req, vifr_dev_oerrors, diff_ms);
 }

static void
rate_stats(struct nl_client *cl, unsigned int vr_op)
{
    struct tm *tm;
    time_t secs;
    char fmt[80] = {0};
    int ret = 0;
    char kb_input[2] = {0};
    struct winsize terminal_size = {0};
    int local_ignore_number_interface = ignore_number_interface;
    int local_print_number_interface = print_number_interface;
    first_rate_iter = true;

    while (true) {
        while (!is_stdin_hit() || get_set) {
            ignore_number_interface = local_ignore_number_interface;
            gettimeofday(&last_time, NULL);
            first_rate_iter || usleep(SET_TIMEOUT_MS * 1000);
            /* Get terminal parameters. */
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal_size);

            print_number_interface = (terminal_size.ws_row - 9) / LISTING_NUM_OF_LINE ;
            print_number_interface =
                (print_number_interface > MAX_OUTPUT_IF? MAX_OUTPUT_IF: print_number_interface);
            local_print_number_interface = print_number_interface;
            if (print_number_interface <= 0) {
                printf("Size of terminal is too small.\n");
                first_rate_iter = true;
                continue;
            }
            ret = system(CLEAN_SCREEN_CMD);
            if (ret == -1) {
                fprintf(stderr, "Error: system() failed.\n");
                exit(1);
            }
            printf("Interface rate statistics\n");
            printf("-------------------------\n\n");
            if (vr_intf_op(cl, vr_op)) {
                fprintf(stderr, "Communication problem with vRouter.\n\n");
                exit(1);
            }
            if(list_set) {
                printf("Key 'q' for quit, key 'k' for previous page, key 'j' for next page.\n");
            }
            secs = last_time.tv_sec;
            tm = localtime(&secs);
            if (tm) {
                strftime(fmt, sizeof(fmt), "%Y-%m-%d %H:%M:%S %z", tm);
                printf("%s \n", fmt);
            }

            /* We need reinitialize dump_marker variable, because we are in loop */
            dump_marker = -1;
            first_rate_iter = false;
        }

        /*
         * We must get minimum 2 characters,
         * otherwise we will be in outer loop, always.
         */
        /* To suppress the warning return if EOF. */
        if (fgets(kb_input, 2, stdin) == NULL)
            return;

        switch (tolower(kb_input[0])) {
            case 'q':
                return;

            case 'k':
                local_ignore_number_interface =
                    (local_ignore_number_interface - local_print_number_interface <= 0)?
                     0:
                     (local_ignore_number_interface - local_print_number_interface);
                break;

            case 'j':
                local_ignore_number_interface =
                        (local_ignore_number_interface + local_print_number_interface);
                break;

            default:
                break;
        }
        fflush(NULL);
    }
}

static int
is_stdin_hit()
{
    struct timeval tv;
    fd_set fds;

    tv.tv_sec = 0;
    tv.tv_usec = 0;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    return FD_ISSET(STDIN_FILENO, &fds);
}

int
main(int argc, char *argv[])
{
    int ret, opt, option_index;
    unsigned int i = 0;

    static struct termios old_term_set, new_term_set;

    /*
     * the proto of the socket changes based on whether we are creating an
     * interface in linux or doing an operation in vrouter
     */
    unsigned int sock_proto = NETLINK_GENERIC;

    vif_fill_nl_callbacks();

    parse_ini_file();
    platform = get_platform();

    while ((opt = getopt_long(argc, argv, "ba:c:d:g:klm:t:T:v:p:C:DPi:s:",
                    long_options, &option_index)) >= 0) {
        switch (opt) {
            case 'a':
                add_set = 1;
                parse_long_opts(ADD_OPT_INDEX, optarg);
                break;

            case 'c':
                create_set = 1;
                parse_long_opts(CREATE_OPT_INDEX, optarg);
                break;

            case 'd':
                delete_set = 1;
                parse_long_opts(DELETE_OPT_INDEX, optarg);
                break;

            case 'g':
                get_set = 1;
                parse_long_opts(GET_OPT_INDEX, optarg);
                break;

            case 'k':
                kindex_set = 1;
                parse_long_opts(KINDEX_OPT_INDEX, optarg);
                break;

            case 'l':
            case 'b':
                list_set = 1;
                parse_long_opts(LIST_OPT_INDEX, NULL);
                break;

            case 'm':
                mac_set = 1;
                parse_long_opts(MAC_OPT_INDEX, optarg);
                break;

            case 'v':
                vrf_set = 1;
                parse_long_opts(VRF_OPT_INDEX, optarg);
                break;

            case 'p':
                policy_set = 1;
                parse_long_opts(POLICY_OPT_INDEX, NULL);
                break;

            case 'D':
                pmd_set = 1;
                parse_long_opts(PMD_OPT_INDEX, NULL);
                break;

            case 'P':
                pci_set = 1;
                parse_long_opts(PCI_OPT_INDEX, NULL);
                break;

            case 't':
                type_set = 1;
                parse_long_opts(TYPE_OPT_INDEX, optarg);
                break;

            case 'T':
                transport_set = 1;
                parse_long_opts(TRANSPORT_OPT_INDEX, optarg);
                break;

            case 'i':
                vindex_set = 1;
                parse_long_opts(VINDEX_OPT_INDEX, NULL);
                break;

            case 'C':
                core_set = 1;
                parse_long_opts(CORE_OPT_INDEX, optarg);
                break;
            case 's':
                sock_dir_set = 1;
                parse_long_opts(SOCK_DIR_OPT_INDEX, optarg);
                break;
            case 0:
                parse_long_opts(option_index, optarg);
                break;

            case '?':
            default:
                Usage();
        }
    }

    validate_options();

    sock_proto = VR_NETLINK_PROTO_DEFAULT;
#if defined(__linux__)
    if (create_set)
        sock_proto = NETLINK_ROUTE;
#endif

    if (sock_dir_set) {
        set_platform_vtest();
        /* Reinit platform variable since platform is changed to vtest now */
        platform = get_platform();
    }
    cl = vr_get_nl_client(sock_proto);
    if (!cl) {
        printf("Error registering NetLink client: %s (%d)\n",
                strerror(errno), errno);
        exit(-ENOMEM);
    }
    if (add_set) {
        /*
         * for addition, we need to see whether the interface already
         * exists in vrouter or not. so, get can return error if the
         * interface does not exist in vrouter
         */
        vr_ignore_nl_errors = true;
        vr_intf_op(cl, SANDESH_OP_GET);
        vr_ignore_nl_errors = false;
    }
    if (!rate_set) {
        vr_intf_op(cl, vr_op);

    } else {
        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
        /*
         * tc[get/set]attr functions are for changing terminal behavior.
         * We dont have to write enter (newline) for getting character from terminal.
         *
         */
        tcgetattr(STDIN_FILENO, &old_term_set);
        new_term_set = old_term_set;
        new_term_set.c_lflag &= ~(ICANON);
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term_set);

        rate_stats(cl, vr_op);
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term_set);
        for (i = 0; i < VR_MAX_INTERFACES; i++) {
            if (prev_req[i].vifr_queue_ierrors_to_lcore) {
                free(prev_req[i].vifr_queue_ierrors_to_lcore);
                prev_req[i].vifr_queue_ierrors_to_lcore = NULL;
            }
        }

    }
    return 0;
}

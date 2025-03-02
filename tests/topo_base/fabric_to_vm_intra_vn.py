#!/usr/bin/python3

import os
import sys
sys.path.append(os.getcwd())
sys.path.append(os.getcwd() + '/lib/')
from imports import *  # noqa

'''
vif --list
-----------
Vrouter Interface Table

Flags: P=Policy, X=Cross Connect, S=Service Chain, Mr=Receive Mirror
       Mt=Transmit Mirror, Tc=Transmit Checksum Offload, L3=Layer 3,
       L2=Layer 2, D=DHCP, Vp=Vhost Physical, Pr=Promiscuous,
       Vnt=Native Vlan Tagged, Mnp=No MAC Proxy, Dpdk=DPDK PMD Interface,
       Rfl=Receive Filtering Offload, Mon=Interface is Monitored
       Uuf=Unknown Unicast Flood, Vof=VLAN insert/strip offload,
       Df=Drop New Flows, L=MAC Learning Enabled,
       Proxy=MAC Requests Proxied Always, Er=Etree Root,
       Mn=Mirror without Vlan Tag, Ig=Igmp Trap Enabled

vif0/0      PCI: Mock
            Type:Physical HWaddr:00:1b:21:bb:f9:48 IPaddr:0.0.0.0
            Vrf:0 Mcast Vrf:65535 Flags:L3L2Vp QOS:0 Ref:7
            RX port   packets:1 errors:0 syscalls:1
            RX queue errors to lcore 0 0 0 0 0 0 0 0 0 0 0
            RX packets:1  bytes:88 errors:0
            TX packets:0  bytes:0 errors:0
            Drops:0

vif0/1      PMD: vhost0 Mock
            Type:Host HWaddr:00:1b:21:bb:f9:48 IPaddr:8.0.0.2
            Vrf:0 Mcast Vrf:65535 Flags:L3D QOS:0 Ref:6
            RX queue errors to lcore 0 0 0 0 0 0 0 0 0 0 0
            RX packets:0  bytes:0 errors:0
            TX packets:0  bytes:0 errors:0
            Drops:0

vif0/2      Socket: unix Mock
            Type:Agent HWaddr:00:00:5e:00:01:00 IPaddr:0.0.0.0
            Vrf:65535 Mcast Vrf:65535 Flags:L3 QOS:0 Ref:5
            RX queue errors to lcore 0 0 0 0 0 0 0 0 0 0 0
            RX packets:0  bytes:0 errors:0
            TX packets:0  bytes:0 errors:0
            Drops:0

vif0/5      PMD: tapc2234cd0-55
            Type:Virtual HWaddr:00:00:5e:00:01:00 IPaddr:1.1.1.3
            Vrf:5 Mcast Vrf:5 Flags:PL3L2D QOS:0 Ref:7
            RX queue errors to lcore 0 0 0 0 0 0 0 0 0 0 0
            RX packets:0  bytes:0 errors:0
            TX packets:1  bytes:42 errors:0
            Drops:0
            TX port   packets:1 errors:0 syscalls:1

nh --list
---------
[root@090c8246aecd vtest_py]# $utils/nh --sock-dir $sock --list
Id:0          Type:Drop           Fmly: AF_INET  Rid:0  Ref_cnt:1021  Vrf:0
              Flags:Valid,

Id:10         Type:Receive        Fmly: AF_INET  Rid:0  Ref_cnt:2     Vrf:1
              Flags:Valid, Policy(R), Etree Root,
              Oif:1

Id:21         Type:Tunnel         Fmly: AF_INET  Rid:0  Ref_cnt:1     Vrf:0
              Flags:Valid, MPLSoUDP, Etree Root,
              Oif:0 Len:14 Data:00 1b 21 bb f9 46 00 1b 21 bb f9 48 08 00
              Sip:8.0.0.2 Dip:8.0.0.3

Id:38         Type:Encap          Fmly: AF_INET  Rid:0  Ref_cnt:1     Vrf:5
              Flags:Valid, Policy, Etree Root,
              EncapFmly:0000 Oif:5 Len:14
              Encap Data: 02 c2 23 4c d0 55 00 00 5e 00 01 00 08 00

Id:44         Type:Encap          Fmly:AF_BRIDGE  Rid:0  Ref_cnt:2    Vrf:5
              Flags:Valid, Policy, Etree Root,
              EncapFmly:0000 Oif:5 Len:14
              Encap Data: 02 c2 23 4c d0 55 00 00 5e 00 01 00 08 00

flow -l
-------
[root@090c8246aecd vtest_py]# $utils/flow --sock-dir $sock -l
Flow table(size 80609280, entries 629760)

Entries: Created 0 Added 2 Deleted 0 Changed 1Processed 0 Used Overflow
entries 0
(Created Flows/CPU: 0 0 0 0 0 0 0 0 0 0 0)(oflows 0)

Action:F=Forward, D=Drop N=NAT(S=SNAT, D=DNAT, Ps=SPAT, Pd=DPAT,
L=Link Local Port)
Other:K(nh)=Key_Nexthop, S(nh)=RPF_Nexthop
Flags:E=Evicted, Ec=Evict Candidate, N=New Flow, M=Modified Dm=Delete Marked
TCP(r=reverse):S=SYN, F=FIN, R=RST, C=HalfClose, E=Established, D=Dead

    Index                Source:Port/Destination:Port               Proto(V)
-----------------------------------------------------------------------------
   255616<=>410748       1.1.1.3:4145                                1 (5)
                         1.1.1.5:0
(Gen: 1, K(nh):38, Action:F, Flags:, QOS:-1, S(nh):38,  Stats:0/0,
SPort 60847, TTL 0, Sinfo 0.0.0.0)

   410748<=>255616       1.1.1.5:4145                                 1 (5)
                         1.1.1.3:0
(Gen: 1, K(nh):38, Action:F, Flags:, QOS:-1, S(nh):21,  Stats:1/42,
SPort 50789, TTL 0, Sinfo 8.0.0.3)

rt --dump 0
-----------
[root@090c8246aecd vtest_py]#
$utils/rt --sock-dir $sock --dump 0 --family inet | grep "8.0.0.2 "\\">"
8.0.0.2/32             32            T          -             10        -

mpls --dump
-----------
[root@090c8246aecd vtest_py]# $utils/mpls --sock-dir $sock --dump
MPLS Input Label Map

   Label    NextHop
-------------------
      42        44

'''

class FabricToVmIntraVn(unittest.TestCase):

    @classmethod
    def setup_class(cls):
        ObjectBase.setUpClass()
        ObjectBase.set_auto_features(cleanup=True)

    @classmethod
    def teardown_class(cls):
        ObjectBase.tearDownClass()

    def setup_method(self, method):
        ObjectBase.setUp(method)

        # Add fabric interface
        self.fabric_interface = FabricVif(
            name="eth1",
            mac_str="00:1b:21:bb:f9:48")

        # Add vhost0 vif
        self.vhost0_vif = VhostVif(
            ipv4_str="8.0.0.2",
            mac_str="00:1b:21:bb:f9:48",
            idx=1)

        # Add agent vif
        self.agent_vif = AgentVif(idx=2)

        # Add tenant vif
        self.tenant_vif = VirtualVif(
            name="tapc2234cd0-55",
            ipv4_str="1.1.1.3",
            mac_str="00:00:5e:00:01:00",
            idx=5,
            vrf=5,
            mcast_vrf=5,
            nh_idx=38)
        # Add vif Nexthop
        self.vif_nh = EncapNextHop(
            encap_oif_id=self.tenant_vif.idx(),
            encap="02 c2 23 4c d0 55 00 00 5e 00 01 00 08 00",
            nh_idx=38,
            nh_vrf=5,
            nh_flags=constants.NH_FLAG_POLICY_ENABLED |
            constants.NH_FLAG_ETREE_ROOT)
        # Add underlay Receive NH
        self.underlay_rnh = ReceiveNextHop(
            encap_oif_id=self.vhost0_vif.idx(),
            nh_idx=10,
            nh_vrf=1,
            nh_flags=constants.NH_FLAG_RELAXED_POLICY |
            constants.NH_FLAG_ETREE_ROOT)
        # Add underlay Route
        self.underlay_route = InetRoute(
            vrf=0,
            prefix="8.0.0.2",
            nh_idx=10,
            rtr_label_flags=constants.VR_RT_ARP_TRAP_FLAG)
        # Add Encap L2 Nexthop for overlay
        self.l2_nh = EncapNextHop(
            encap_oif_id=self.tenant_vif.idx(),
            encap="02 c2 23 4c d0 55 00 00 5e 00 01 00 08 00",
            nh_idx=44,
            nh_family=constants.AF_BRIDGE,
            nh_vrf=5,
            nh_flags=constants.NH_FLAG_POLICY_ENABLED |
            constants.NH_FLAG_ETREE_ROOT)
        # Add MPLS entry for overlay
        self.mpls_entry = Mpls(
            mr_label=42,
            mr_nhid=44)
        # Add tunnel NH (for src validation)
        self.tunnel_nh = TunnelNextHopV4(
            encap_oif_id=self.fabric_interface.idx(),
            encap="00 1b 21 bb f9 46 00 1b 21 bb f9 48 08 00",
            tun_sip="8.0.0.2",
            tun_dip="8.0.0.3",
            nh_idx=21,
            nh_flags=constants.NH_FLAG_TUNNEL_UDP_MPLS |
            constants.NH_FLAG_ETREE_ROOT)
        ObjectBase.sync_all()

        # Add forward and reverse flow
        self.f_flow = InetFlow(
            sip='1.1.1.3',
            dip='1.1.1.5',
            sport=4145,
            dport=0,
            proto=constants.VR_IP_PROTO_ICMP,
            flow_nh_idx=38,
            src_nh_idx=38,
            flow_vrf=5,
            rflow_nh_idx=21)

        self.r_flow = InetFlow(
            sip='1.1.1.5',
            dip='1.1.1.3',
            sport=4145,
            dport=0,
            proto=constants.VR_IP_PROTO_ICMP,
            flags=constants.VR_RFLOW_VALID,
            flow_nh_idx=38,
            src_nh_idx=21,
            flow_vrf=5,
            rflow_nh_idx=21)
        self.f_flow.sync_and_link_flow(self.r_flow)
        self.assertGreater(self.f_flow.get_fr_index(), 0)

    def teardown_method(self, method):
        ObjectBase.tearDown()

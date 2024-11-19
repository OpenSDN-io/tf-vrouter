#!/usr/bin/python

import os
import sys
sys.path.append(os.getcwd())
sys.path.append(os.getcwd() + '/lib/')
from imports import *  # noqa

# anything with *test* will be assumed by pytest as a test


class VmToVmNat66(unittest.TestCase):

    @classmethod
    def setup_class(cls):
        ObjectBase.setUpClass()
        ObjectBase.set_auto_features(cleanup=True)

    @classmethod
    def teardown_class(cls):
        ObjectBase.tearDownClass()

    def setup_method(self, method):
        ObjectBase.setUp(method)

        # Add tenant vif3
        self.vif3 = VirtualVif(
            name="tap88670c2e-11",
            ipv4_str="1.0.0.4",
            ipv6_str="fd99::4",
            mac_str="00:00:5e:00:01:00",
            idx=3,
            vrf=2,
            mcast_vrf=2,
            nh_idx=23)

        # Add tenant vif4
        self.vif4 = VirtualVif(
            name="tape703ea67-f1",
            ipv4_str="1.0.0.5",
            ipv6_str="fd99::5",
            mac_str="00:00:5e:00:01:00",
            idx=4,
            vrf=2,
            mcast_vrf=2,
            nh_idx=28)

        # Add vif3 Nexthop (inet)
        self.vif3_nh = EncapNextHop(
            encap_oif_id=self.vif3.idx(),
            encap="02 88 67 0c 2e 11 00 00 5e 00 01 00 08 00",
            nh_idx=23,
            nh_vrf=2,
            nh_flags=constants.NH_FLAG_POLICY_ENABLED |
            constants.NH_FLAG_ETREE_ROOT)

        # Add vif4 NextHop (inet)
        self.vif4_nh = EncapNextHop(
            encap_oif_id=self.vif4.idx(),
            encap="02 e7 03 ea 67 f1 00 00 5e 00 01 00 08 00",
            nh_idx=28,
            nh_vrf=2,
            nh_flags=constants.NH_FLAG_POLICY_ENABLED |
            constants.NH_FLAG_ETREE_ROOT)

        # Add vif3 Nexthop (bridge)
        self.vif3_nh_bridge = EncapNextHop(
            encap_oif_id=self.vif3.idx(),
            encap="02 88 67 0c 2e 11 00 00 5e 00 01 00 08 00",
            nh_idx=27,
            nh_vrf=2,
            nh_flags=constants.NH_FLAG_POLICY_ENABLED |
            constants.NH_FLAG_ETREE_ROOT,
            nh_family=constants.AF_BRIDGE)

        # Add vif4 Nexthop (bridge)
        self.vif4_nh_bridge = EncapNextHop(
            encap_oif_id=self.vif4.idx(),
            encap="02 e7 03 ea 67 f1 00 00 5e 00 01 00 08 00",
            nh_idx=32,
            nh_vrf=2,
            nh_flags=constants.NH_FLAG_POLICY_ENABLED |
            constants.NH_FLAG_ETREE_ROOT,
            nh_family=constants.AF_BRIDGE)

        # Add bridge route
        self.bridge_route1 = BridgeRoute(
            vrf=2,
            mac_str="02:e7:03:ea:67:f1",
            nh_idx=32)

        # Add bridge route
        self.bridge_route2 = BridgeRoute(
            vrf=2,
            mac_str="02:88:67:0c:2e:11",
            nh_idx=27)

        ObjectBase.sync_all()

        # Add forward and reverse flow
        fflags = constants.VR_FLOW_FLAG_VRFT |\
            constants.VR_FLOW_FLAG_DNAT |\
            constants.VR_FLOW_FLAG_LINK_LOCAL

        self.f_flow = Nat6Flow(sip6="fd99::4", dip6="fd99::6", sport=1136, dport=0,
                        proto=constants.VR_IP_PROTO_UDP, flow_nh_idx=23,
                        src_nh_idx=23, flow_vrf=2, flow_dvrf=2,
                        rflow_sip6="fd99::5", rflow_dip6="fd99::4",
                        rflow_nh_idx=28, rflow_sport=1136, flags=fflags)

        rflags = constants.VR_RFLOW_VALID |\
            constants.VR_FLOW_FLAG_VRFT |\
            constants.VR_FLOW_FLAG_SNAT
        # Add reverse Flow
        self.r_flow = Nat6Flow(
            sip6="fd99::5",
            dip6="fd99::4",
            sport=1136,
            dport=0,
            proto=constants.VR_IP_PROTO_UDP,
            flow_nh_idx=28,
            src_nh_idx=28,
            flow_vrf=2,
            flow_dvrf=2,
            rflow_sip6="fd99::4",
            rflow_dip6="fd99::6",
            rflow_nh_idx=23,
            rflow_sport=1136,
            flags=rflags)

        # self.f_flow = Inet6Flow(
        #     sip6_str="fd99::4",
        #     dip6_str="fd99::5",
        #     sport=1136,
        #     dport=0,
        #     proto=constants.VR_IP_PROTO_UDP,
        #     flow_nh_idx=23,
        #     src_nh_idx=23,
        #     flow_vrf=2,
        #     rflow_nh_idx=28)

        # self.r_flow = Inet6Flow(
        #     sip6_str="fd99::5",
        #     dip6_str="fd99::4",
        #     sport=1136,
        #     dport=0,
        #     proto=constants.VR_IP_PROTO_UDP,
        #     flags=constants.VR_RFLOW_VALID,
        #     flow_nh_idx=28,
        #     src_nh_idx=28,
        #     flow_vrf=2,
        #     rflow_nh_idx=23)

        self.f_flow.sync_and_link_flow(self.r_flow)
        self.assertGreater(self.f_flow.get_fr_index(), 0)

    def teardown_method(self, method):
        ObjectBase.tearDown()

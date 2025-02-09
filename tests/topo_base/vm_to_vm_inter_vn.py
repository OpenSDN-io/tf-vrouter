#!/usr/bin/python3

import os
import sys
sys.path.append(os.getcwd())
sys.path.append(os.getcwd() + '/lib/')
from imports import *  # noqa

# anything with *test* will be assumed by pytest as a test


class VmToVmInterVn(unittest.TestCase):

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
            idx=3,
            name="tap1",
            ipv4_str="1.1.1.4",
            mac_str="00:00:5e:00:01:00",
            vrf=3,
            mcast_vrf=3,
            nh_idx=23)

        # Add tenant vif4
        self.vif4 = VirtualVif(
            idx=4,
            name="tap2",
            ipv4_str="2.2.2.4",
            mac_str="00:00:5e:00:01:00",
            vrf=4,
            mcast_vrf=4,
            nh_idx=28)

        # Add vif3 encap nexthop (inet)
        self.vif3_nh = EncapNextHop(
            encap_oif_id=self.vif3.idx(),
            encap="02 88 67 0c 2e 11 00 00 5e 00 01 00 08 00",
            nh_vrf=3,
            nh_idx=23,
            nh_flags=(
                constants.NH_FLAG_POLICY_ENABLED |
                constants.NH_FLAG_ETREE_ROOT))

        # Add vif4 encap nexthop (inet)
        self.vif4_nh = EncapNextHop(
            encap_oif_id=self.vif4.idx(),
            encap="02 e7 03 ea 67 f1 00 00 5e 00 01 00 08 00",
            nh_vrf=4,
            nh_idx=28,
            nh_flags=(
                constants.NH_FLAG_POLICY_ENABLED |
                constants.NH_FLAG_ETREE_ROOT))

        # Add overlay L2 Receive NH
        self.l2_nh = ReceiveL2NextHop(
            nh_idx=3,
            nh_flags=constants.NH_FLAG_ETREE_ROOT)

        # Add vif3 bridge Route with agent MAC
        self.vif3_bridge_route = BridgeRoute(
            nh_idx=3, vrf=3, mac_str="00:00:5e:00:01:00")

        # Add vif4 bridge Route with agent MAC
        self.vif4_bridge_route = BridgeRoute(
            nh_idx=3, vrf=4, mac_str="00:00:5e:00:01:00")

        # Add vif3 Route (note this is vif4's subnet route)
        self.vif3_inet_route = InetRoute(
            prefix="2.2.2.4",
            vrf=3,
            nh_idx=28)

        # Add vif4 Route (note this is vif3's subnet route)
        self.vif4_inet_route = InetRoute(
            prefix="1.1.1.4",
            vrf=4,
            nh_idx=23)

        ObjectBase.sync_all()

        # Add forward and reverse flow
        self.f_flow = InetFlow(
            sip='1.1.1.4',
            dip='2.2.2.4',
            sport=1136,
            dport=0,
            proto=constants.VR_IP_PROTO_ICMP,
            flow_nh_idx=23,
            src_nh_idx=23,
            flow_vrf=3,
            rflow_nh_idx=28)

        self.r_flow = InetFlow(
            sip='2.2.2.4',
            dip='1.1.1.4',
            sport=1136,
            dport=0,
            proto=constants.VR_IP_PROTO_ICMP,
            flow_nh_idx=28,
            flags=constants.VR_RFLOW_VALID,
            src_nh_idx=28,
            flow_vrf=4,
            rflow_nh_idx=23)
        self.f_flow.sync_and_link_flow(self.r_flow)
        self.assertGreater(self.f_flow.get_fr_index(), 0)

    def teardown_method(self, method):
        ObjectBase.tearDown()

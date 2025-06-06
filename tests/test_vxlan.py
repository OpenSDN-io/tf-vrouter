#!/usr/bin/python3

import os
import sys
sys.path.append(os.getcwd())
sys.path.append(os.getcwd() + '/lib/')
from imports import *  # noqa

# anything with *test* will be assumed by pytest as a test


class TestVxlan(unittest.TestCase):

    @classmethod
    def setup_class(cls):
        ObjectBase.setUpClass()
        ObjectBase.set_auto_features(cleanup=True)

    @classmethod
    def teardown_class(cls):
        ObjectBase.tearDownClass()

    def setup_method(self, method):
        ObjectBase.setUp(method)

        self.vif1 = VirtualVif(
            name="tap_1",
            idx=1,
            ipv4_str=None,
            mac_str="de:ad:be:ef:00:02",
            mtu=2514,
            flags=None)

        self.vif2 = VirtualVif(
            name="tap_2",
            idx=2,
            ipv4_str=None,
            mac_str="de:ad:be:ef:00:01",
            mtu=2514,
            flags=None)

        ObjectBase.sync_all()

    def teardown_method(self, method):
        ObjectBase.tearDown()

    def test_vxlan_bridge(self):

        nh_tunnel = TunnelNextHopV4(
            encap_oif_id=self.vif2.idx(),
            encap="00 22 22 22 22 22 00 11 11 11 11 11 08 00",
            tun_sip="1.1.2.2",
            tun_dip="2.2.1.1",
            nh_idx=12,
            nh_flags=129)

        bridge_route = BridgeRoute(
            vrf=0,
            mac_str="de:ad:be:ef:00:01",
            nh_idx=12,
            rtr_label=128,
            rtr_label_flags=3)
        ObjectBase.sync_all()

        udp = UdpPacket(
            sip="1.1.1.1",
            dip="2.2.2.2",
            smac="de:ad:be:ef:00:02",
            dmac="de:ad:be:ef:00:01",
            sport=0,
            dport=1)
        pkt1 = udp.get_packet()
        pkt1.show()
        self.assertIsNotNone(pkt1)

        rec_pkt = self.vif1.send_and_receive_packet(pkt1, self.vif2)

        self.assertTrue(VXLAN in rec_pkt)
        self.assertEqual('1.1.2.2', rec_pkt[IP].src)
        self.assertEqual('2.2.1.1', rec_pkt[IP].dst)
        self.assertEqual(1, self.vif1.get_vif_ipackets())
        self.assertEqual(1, self.vif2.get_vif_opackets())

    def test_vxlan_route(self):

        nh_tunnel = TunnelNextHopV4(
            encap_oif_id=self.vif2.idx(),
            encap="de ad be ef 00 01 de ad be ef 00 02 08 00",
            tun_sip="1.1.2.2",
            tun_dip="2.2.1.1",
            tun_sport=-18398,
            tun_dport=-18398,
            nh_idx=12,
            nh_flags=129)

        inet_route = InetRoute(
            vrf=0,
            prefix="2.2.2.2",
            nh_idx=12,
            rtr_label=48,
            rtr_label_flags=1)

        nh_l2rcv = ReceiveL2NextHop(
            nh_idx=13,
            nh_family=0)

        bridge_route = BridgeRoute(
            vrf=0,
            mac_str="de:ad:be:ef:00:01",
            nh_idx=13,
            rtr_label_flags=0)
        ObjectBase.sync_all()

        udp = UdpPacket(
            sip="1.1.1.1",
            dip="2.2.2.2",
            smac="de:ad:be:ef:00:02",
            dmac="de:ad:be:ef:00:01",
            sport=7936,
            dport=7936)
        pkt1 = udp.get_packet()
        pkt1.show()
        self.assertIsNotNone(pkt1)

        rec_pkt = self.vif1.send_and_receive_packet(pkt1, self.vif2)

        self.assertTrue(VXLAN in rec_pkt)
        self.assertEqual('1.1.2.2', rec_pkt[IP].src)
        self.assertEqual('2.2.1.1', rec_pkt[IP].dst)
        self.assertEqual(1, self.vif1.get_vif_ipackets())
        self.assertEqual(1, self.vif2.get_vif_opackets())

        self.assertEqual(1, self.vif1.get_vif_ipackets())
        self.assertEqual(1, self.vif2.get_vif_opackets())

#!/usr/bin/python

from topo_base.vm_to_vm_nat66 import VmToVmNat66
import os
import sys
sys.path.append(os.getcwd())
sys.path.append(os.getcwd() + '/lib/')
from imports import *  # noqa

# anything with *test* will be assumed by pytest as a test


class TestVmToVmNat66(VmToVmNat66):

    def test_vm_to_vm_nat66(self):

        self.vif3_nh_bridge.nhr_family = constants.AF_BRIDGE
        self.vif3_nh_bridge.sync()
        self.vif4_nh_bridge.nhr_family = constants.AF_BRIDGE
        self.vif4_nh_bridge.sync()

        # send ping request from vif3
        udpv6 = Udpv6Packet(
            sport=1136,
            dport=0,
            sipv6='fd99::4',
            dipv6='fd99::6',
            smac='02:88:67:0c:2e:11',
            dmac='02:e7:03:ea:67:f1',
            nh=17)
        pkt = udpv6.get_packet()
        pkt.show()

        self.assertIsNotNone(pkt)
        # send packet
        rec_pkt = self.vif3.send_and_receive_packet(pkt, self.vif4)
        rec_pkt.show()
        self.assertEqual(1, self.vif4.get_vif_opackets())

        # check if we got UDP6 packet
        self.assertIsNotNone(rec_pkt)
        self.assertTrue(IPv6 in rec_pkt)
        self.assertEqual("fd99::4", rec_pkt[IPv6].src)
        self.assertEqual("fd99::5", rec_pkt[IPv6].dst)

        # send ping request from vif4
        udpv6 = Udpv6Packet(
            sport=1136,
            dport=0,
            sipv6='fd99::5',
            dipv6='fd99::4',
            smac='02:e7:03:ea:67:f1',
            dmac='02:88:67:0c:2e:11',
            nh=17)
        pkt = udpv6.get_packet()
        pkt.show()

        self.assertIsNotNone(pkt)
        # send packet
        rec_pkt = self.vif4.send_and_receive_packet(pkt, self.vif3)
        rec_pkt.show()
        self.assertEqual(1, self.vif3.get_vif_opackets())

        # check if we got ICMP packet
        self.assertIsNotNone(rec_pkt)
        self.assertTrue(IPv6 in rec_pkt)
        self.assertEqual("fd99::6", rec_pkt[IPv6].src)
        self.assertEqual("fd99::4", rec_pkt[IPv6].dst)

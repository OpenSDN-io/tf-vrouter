#!/usr/bin/python3

import os
import sys
sys.path.append(os.getcwd())
sys.path.append(os.getcwd() + '/lib/')
from imports import *  # noqa


class TestVifL3MH(unittest.TestCase):

    @classmethod
    def setup_class(cls):
        ObjectBase.setUpClass()
        ObjectBase.set_auto_features(cleanup=True)

    @classmethod
    def teardown_class(cls):
        ObjectBase.tearDownClass()

    def setup_method(self, method):
        ObjectBase.setUp(method)
        # Add Fabric vif
        self.fabric_vif = FabricVif(
            name="eth0",
            mac_str="00:1b:21:bb:f9:46",
            ipv4_str="1.1.1.1",
            idx=0)
        self.fabric_vif.vifr_os_idx = self.fabric_vif.vifr_idx
        self.fabric_vif.sync()

        # Add Fabric vif 1
        self.fabric_vif_1 = FabricVif(
            name="eth1",
            mac_str="00:1b:21:bb:f9:50",
            ipv4_str="2.2.2.2",
            idx=1)
        self.fabric_vif_1.vifr_os_idx = self.fabric_vif_1.vifr_idx
        self.fabric_vif_1.sync()

        cross_connect_idx = [self.fabric_vif.vifr_idx,
                             self.fabric_vif_1.vifr_idx]
        # Add vhost0
        self.vhost_vif = VhostVif(
            idx=2,
            ipv4_str='10.1.1.1',
            mac_str='00:00:5e:00:01:00',
            xconnect_idx=cross_connect_idx)
        self.vhost_vif.vifr_os_idx = self.vhost_vif.vifr_idx
        self.vhost_vif.sync()

        # Add fabric vif nexthop
        fabric_vif_nh = EncapNextHop(
            encap_oif_id=self.fabric_vif.idx(),
            encap='90 e2 ba 84 48 88 00 1b 21 bb f9 46 08 00',
            nh_idx=16)

        # Add receive nexthop
        receive_nh = ReceiveNextHop(
            encap_oif_id=self.vhost_vif.idx(),
            nh_vrf=1,
            nh_idx=10)

        # Add fabric route
        fabric_route = InetRoute(
            vrf=0,
            prefix='10.1.1.1',
            nh_idx=receive_nh.idx())

        # Add vhost route
        vhost_route = InetRoute(
            vrf=0,
            prefix='20.1.1.1',
            nh_idx=fabric_vif_nh.idx())

        ObjectBase.sync_all()

        icmp = IcmpPacket(
            sip='10.1.1.1',
            dip='20.1.1.1',
            smac='00:00:5e:00:01:00',
            dmac='90:e2:ba:84:48:88',
            id=1136)
        self.icmp_vhost_to_fabric_pkt = icmp.get_packet()
        self.icmp_vhost_to_fabric_pkt.show()

        icmp = IcmpPacket(
            sip='20.1.1.1',
            dip='10.1.1.1',
            smac='90:e2:ba:84:48:88',
            dmac='00:00:5e:00:01:00',
            id=1137)
        self.icmp_fabric_to_vhost_pkt = icmp.get_packet()
        self.icmp_fabric_to_vhost_pkt.show()

        ether = Ether(src='90:e2:ba:84:48:88', dst='00:1b:21:bb:f9:46',
                      type=0x0806)
        arp = ARP()
        self.arp_fabric_to_tap0_pkt = ether / arp
        self.arp_fabric_to_tap0_pkt.show()

        ether = Ether(src='00:00:5e:00:01:00', dst='90:e2:ba:84:48:88',
                      type=0x0806)
        arp = ARP()
        self.arp_vhost_to_fabric_pkt = ether / arp
        self.arp_vhost_to_fabric_pkt.show()

        icmp2 = IcmpPacket(
            sip='10.1.1.1',
            dip='20.1.1.1',
            smac='00:00:5e:00:01:00',
            dmac='90:e2:ba:84:48:88',
            id=1138)
        self.icmp_vhost_to_fabric_pkt_2 = icmp2.get_packet()
        self.icmp_vhost_to_fabric_pkt_2.show()

    def teardown_method(self, method):
        ObjectBase.tearDown()
        ObjectBase.get_cli_output("vif --delete 4352")
        ObjectBase.get_cli_output("vif --delete 4352")

    def test_l3mh_vif_add(self):
        self.assertEqual("eth0", self.fabric_vif.get_vif_name())
        self.assertEqual("eth1", self.fabric_vif_1.get_vif_name())
        self.assertEqual("vhost0", self.vhost_vif.get_vif_name())

    def test_traffic_l3mh_xconnect(self):
        self.vhost_vif.send_packet(self.icmp_vhost_to_fabric_pkt)
        self.assertEqual(1, self.fabric_vif.get_vif_opackets())

        # In xconnet mode, every packet(both L2 and L3) is transferred
        # via vif_bridge and hence no packet entering into physical
        # interface will ever reach vhost0 in l3mh in dpdk xconnect mode.
        self.fabric_vif.send_packet(self.icmp_fabric_to_vhost_pkt)
        self.assertEqual(0, self.vhost_vif.get_vif_opackets())
        vif_tap0_get = ObjectBase.get_cli_output("vif --get 4352")
        self.assertEqual(1, ("TX packets:1" in vif_tap0_get))

        # clear the stats for tap0 vif before sending L2 packet
        ObjectBase.get_cli_output("vif --clear 4352")

        self.fabric_vif.send_packet(self.arp_fabric_to_tap0_pkt)
        vif_tap0_get = ObjectBase.get_cli_output("vif --get 4352")
        self.assertEqual(1, ("TX packets:1" in vif_tap0_get))

        pkt = self.vhost_vif.send_and_receive_packet(
                self.arp_vhost_to_fabric_pkt, self.fabric_vif)
        pkt.show()
        self.assertTrue(ARP in pkt)

    def test_traffic_l3mh_normal(self):
        # Add agent vif
        agent_vif = AgentVif(idx=3, flags=constants.VIF_FLAG_L3_ENABLED)
        agent_vif.sync()

        self.vhost_vif.send_packet(self.icmp_vhost_to_fabric_pkt)
        self.assertEqual(1, self.fabric_vif.get_vif_opackets())

        self.fabric_vif.send_packet(self.icmp_fabric_to_vhost_pkt)
        self.assertEqual(1, self.vhost_vif.get_vif_opackets())

        self.fabric_vif.send_packet(self.arp_fabric_to_tap0_pkt)
        vif_tap0_get = ObjectBase.get_cli_output("vif --get 4352")
        self.assertEqual(1, ("TX packets:1" in vif_tap0_get))

        pkt = self.vhost_vif.send_and_receive_packet(
                self.arp_vhost_to_fabric_pkt, self.fabric_vif)
        pkt.show()
        self.assertTrue(ARP in pkt)

    def test_vhost_traffic_lb(self):
        self.vhost_vif.delete()
        self.vhost_vif = VhostVif(
            idx=2,
            ipv4_str='10.1.1.1',
            mac_str='00:00:5e:00:01:00',
            flags=constants.VIF_FLAG_XCONNECT)
        self.vhost_vif.vifr_os_idx = self.vhost_vif.vifr_idx
        self.vhost_vif.sync()
        self.vhost_vif.send_packet(self.icmp_vhost_to_fabric_pkt_2)
        self.assertEqual(1, self.fabric_vif.get_vif_opackets())

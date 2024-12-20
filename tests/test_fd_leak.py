#!/usr/bin/python3

import os
import sys
sys.path.append(os.getcwd())
sys.path.append(os.getcwd() + '/lib/')
from imports import *  # noqa

# anything with *test* will be assumed by pytest as a test


class TestFdLeak(unittest.TestCase):

    @classmethod
    def setup_class(cls):
        ObjectBase.setUpClass()
        ObjectBase.set_auto_features(cleanup=True)

    @classmethod
    def teardown_class(cls):
        ObjectBase.tearDownClass()

    def setup_method(self, method):
        ObjectBase.setUp(method)

    def teardown_method(self, method):
        ObjectBase.tearDown()
        new_fd_count = int(os.popen(fd_count_cmd).read())
        print("new_fd_count=" + str(new_fd_count))
        assert (orig_fd_count == new_fd_count)

    def test_fd_leak(self):
        # Wait for 3 sec to calculate number of fd's after launching vrouter
        time.sleep(3)
        global fd_count_cmd, orig_fd_count
        pid_cmd = 'pidof contrail-vrouter-dpdk'
        pid = os.popen(pid_cmd).read()
        print("pid = " + str(pid))

        fd_count_cmd = "ls -al /proc/" + str(pid).strip() + "/fd | wc -l"

        print("fd_count_cmd = " + fd_count_cmd)
        orig_fd_count = int(os.popen(fd_count_cmd).read())
        print("orig_fd_count=" + str(orig_fd_count))

        # Add tap vif
        vif = VirtualVif(
            name="tapc2234cd0-55",
            ipv4_str="1.1.1.3",
            mac_str="00:00:5e:00:01:00",
            idx=5,
            nh_idx=38,
            vrf=5,
            mcast_vrf=5,
            flags=constants.VIF_FLAG_POLICY_ENABLED |
            constants.VIF_FLAG_DHCP_ENABLED)
        vif.sync()

        icmp = IcmpPacket(
            sip='1.1.1.3',
            dip='1.1.1.5',
            smac='02:c2:23:4c:d0:55',
            dmac='02:e7:03:ea:67:f1',
            id=4145)
        pkt = icmp.get_packet()
        pkt.show()
        self.assertIsNotNone(pkt)

        # send packet multiple times
        # each call to this API will simulate VM start + VM stop
        for x in range(3):
            vif.send_packet(pkt)

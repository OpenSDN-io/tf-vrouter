#
# Copyright (c) 2019 Juniper Networks, Inc. All rights reserved.
#

import ipaddress
import netaddr
import socket
import struct
from base import Base

# Add all utility functions here like vt_ipv4, vt_encap etc.


class Common(object):
    """Class for all utility functions like vt_ipv4, vt_encap etc"""

    @classmethod
    def htonll(self, val):
        """Takes host byte order and returns the network byte order"""
        return (socket.htonl(val & 0xFFFFFFFF) << 32) + \
            (socket.htonl(val >> 32))

    @classmethod
    def ntohll(self, val):
        """Takes network byte order and returns the host byte order"""
        return (socket.ntohl(val & 0xFFFFFFFF) << 32) + \
            (socket.ntohl(val >> 32))

    @classmethod
    def vt_encap(self, value):
        """Returns a list from encap hex string value"""
        return list(bytes.fromhex(value.replace(' ', '')))

    @classmethod
    def vt_mac(self, value):
        """Returns list from mac string"""
        return list(bytes.fromhex(value.replace(':', '')))

    @classmethod
    def vt_ipv4(self, value):
        """Returns unsigned int value for corresponding ipv4 string"""
        return socket.htonl(int(ipaddress.IPv4Address(value)))

    @classmethod
    def vt_ipv4_bytes(self, value):
        """Returns decimal list for corresponding ipv4 string"""
        ipv4_sp = value.split(".")
        ipv4_dec = []
        for i in range(len(ipv4_sp)):
            ipv4_dec.append(int(ipv4_sp[i]))
        return ipv4_dec

    @classmethod
    def vt_ipv6(self, value):
        """Returns ipv6 upper and lower value from ipv6 string"""
        ip6_u = int(bin(netaddr.IPAddress(value) >> 64), 2)
        ip6_l = int(bin(netaddr.IPAddress(value) & (1 << 64) - 1), 2)
        return self.htonll(ip6_u), self.htonll(ip6_l)

    @classmethod
    def vt_ipv6_bytes(self, value):
        """Returns unsigned int value for corresponding ipv6 string"""
        return [x for x in ipaddress.ip_address(value).packed]

    @classmethod
    def vt_oif_id(self, value):
        """Returns oif_id list from corrosponding oif_id string"""
        oif_id = [-1, -1, -1]
        if value:
            oif_str_list = value.split(",")
        else:
            oif_str_list = []
        for i in range(len(oif_str_list)):
            oif_id[i] = int(oif_str_list[i])
        return oif_id

    @classmethod
    def vt_encap_valid(self, list):
        """Returns encap_valid list from oif_id list"""
        encap_valid = [0, 0, 0]
        for i in range(len(list)):
            if list[i] > -1:
                encap_valid[i] = 1
        return encap_valid

#!/usr/bin/python3
#
# Copyright (c) 2020 Juniper Networks, Inc. All rights reserved.
#

"""

# pkt_droplog.py
This script is used to display the pkt drop log contents in descending order
based on timestamp from all cores.

"""

import os
import time

os.system("dropstats -l 0 > file_tmp_1")
os.system("awk '$6>0' file_tmp_1 > file_tmp_2")
os.system("sort -nrk6 file_tmp_2 > file_tmp_3")
os.system("sed -i.bak '/^Pkt Drop Log/d' file_tmp_3")
os.system("cp file_tmp_3 dropstats_sorted")
timestr = time.strftime("%Y%m%d-%H%M%S")
file_dest = 'dropstats_sorted_%s' % timestr
os.system('cat dropstats_sorted')
os.rename('dropstats_sorted', file_dest)
os.system("rm -rf file_tmp_*")


#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

netlist = test.obj_dir + '/netlist.v'
test.compile(verilator_flags2=[
    '--struct-only-output', netlist, '--struct-only-xilinx-rtl', '--struct-only-report',
    '--struct-only-wide-packed-mem', '--struct-only-full-case', '--struct-only-blackbox', 'leaf'
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

# Vivado RTL_* cell names, and the blackboxed module left as an instance with no contents
test.file_grep(netlist, r'RTL_')
test.file_grep(netlist, r'module leaf')
test.file_grep_not(netlist, r'RTL_ADD')

test.passes()

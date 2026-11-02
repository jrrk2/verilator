#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

netlist = test.obj_dir + '/netlist.v'

test.compile(verilator_flags2=['--struct-only-output', netlist],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

# Two latches: always_latch with a hold, and the incomplete always @*
test.file_grep_count(netlist, r'latch_en ', 2)
test.file_grep(netlist, r'latch inferred for y_star')
test.file_grep(netlist, r'always_latch assigns y_comb_in_latch on every path')
# The unreachable default value never reaches the netlist
test.file_grep_not(netlist, r"8'hee")
test.file_grep_not(netlist, r'not decidable')

test.passes()

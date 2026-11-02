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
mapped = test.obj_dir + '/mapped.json'

test.compile(verilator_flags2=['--struct-only-output', netlist, '--map-only-output', mapped],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

# y_if and y_cse share one adder each; the ALU's four arms one adder_carry
test.file_grep_count(netlist, r'^\s*adder #', 2)
test.file_grep_count(netlist, r'^\s*adder_carry #', 1)
test.file_grep_not(netlist, r'^\s*subtractor #')
# Constant case labels steer by selector bits
test.file_grep_not(netlist, r'^\s*comparator_eq #')
# a < b and a >= b: one chain of 32 bits (8 CARRY4); y_if, y_alu and the CSE
# sum one 32-bit chain each
test.file_grep_count(mapped, r'"type": "CARRY4"', 32)
test.file_grep_count(mapped, r'"type": "MUXF7"', 8)

test.passes()

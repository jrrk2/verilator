#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
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

# the mapped netlist elaborates (the formal is bound, not an undeclared name)
test.file_grep(mapped, r'"type": "LUT')
test.file_grep(mapped, r'"type": "FDRE"')

test.passes()

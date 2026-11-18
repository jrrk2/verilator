#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

# A never-written memory wider than 64 bits cannot be lowered to logic: its image would be lost
test.compile(verilator_flags2=['--struct-only-output', test.obj_dir + '/netlist.v'],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False,
             fails=True,
             expect_filename=test.golden_filename)

test.passes()

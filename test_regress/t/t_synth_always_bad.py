#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

# A multi-edge always block without a reset-style if chain cannot be synthesised
test.compile(verilator_flags2=['--struct-only-output', test.obj_dir + '/netlist.v'],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False,
             fails=True,
             expect_filename=test.golden_filename)

test.passes()

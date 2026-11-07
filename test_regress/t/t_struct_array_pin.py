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


test.file_grep(netlist, r'\.d\(pinarr_')
test.file_grep(netlist, r'\.q\(pinarr_')

test.passes()

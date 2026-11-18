#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

mapped = test.obj_dir + '/mapped.json'
test.compile(verilator_flags2=[
    '--map-only-output', mapped, '--map-only-partition-min', '1', '--struct-only-report'
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

# every instance mapped on its own keeps its name in the netlist
test.file_grep(mapped, r'"type": "LUT')
test.file_grep(mapped, r'u0\.')
test.file_grep(mapped, r'u1\.')

test.passes()

#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

test.top_filename = "t/t_synth_map_timing.v"

mapped = test.obj_dir + '/mapped.json'
report = test.obj_dir + '/timing.txt'
test.compile(verilator_flags2=[
    '--map-only-output', mapped, '--map-only-period', '4.0', '--map-only-timing-report', report
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

test.file_grep(mapped, r'"type": "LUT')
test.file_grep(report, r'clk')

test.passes()

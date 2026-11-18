#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

mapped = test.obj_dir + '/mapped.json'
report = test.obj_dir + '/timing.txt'
test.compile(verilator_flags2=[
    '--map-only-output', mapped, '--map-only-sdc', test.t_dir + '/t_synth_map_timing.sdc',
    '--map-only-timing-report', report, '--map-only-timing-iterations', '1',
    '--map-only-timing-paths', '3', '--map-only-timing-margin', '0.2', '--map-only-max-fanout',
    '16'
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

test.file_grep(mapped, r'"type": "LUT')
test.file_grep(report, r'clk')

test.passes()

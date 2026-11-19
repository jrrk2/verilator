#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

mapped = test.obj_dir + '/mapped.v'
test.compile(verilator_flags2=[
    '--map-only-output', test.obj_dir + '/mapped.json', '--map-only-verilog', mapped,
    '--map-only-partition-min', '1', '--map-only-no-iobuf', '--struct-only-report'
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

# registers are stored inverted, and the netlist still behaves as the source
test.file_grep(test.compile_log_filename, r'map-regs: \d+ register bits stored inverted')

test.compile(top_filename='t/t_synth_map_regflip_tb.v',
             verilator_flags2=['--binary', '--top-module', 'tb', mapped])

test.execute()

test.passes()

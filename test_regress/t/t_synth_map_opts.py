#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

mapped = test.obj_dir + '/mapped.json'
verilog = test.obj_dir + '/mapped.v'
edif = test.obj_dir + '/mapped.edf'
meminit = test.obj_dir + '/meminit.json'
test.compile(verilator_flags2=[
    '--map-only-output', mapped, '--map-only-verilog', verilog, '--map-only-edif', edif,
    '--map-only-edif-flat', '--map-only-meminit', meminit, '--map-only-keep-temps',
    '--map-only-no-iobuf', '--map-only-no-wide-lut', '--map-only-keep-equivalent-registers',
    '--map-only-keep-sum-chains', '--map-only-whole-program', '--map-only-max-fanout', '8',
    '--map-only-bram-cascade', '--map-only-abc-script', "'strash; dc2; if'"
],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

test.file_grep(mapped, r'"type": "LUT')
test.file_grep(mapped, r'"type": "FDRE"')
test.file_grep(mapped, r'"type": "RAMB')
test.file_grep_not(mapped, r'"type": "(IBUF|OBUF|MUXF7)"')
test.file_grep(verilog, r'^module ')
test.file_grep(edif, r'\(edif ')
test.file_grep(meminit, r'mem')
# --map-only-keep-temps: ABC's input stays next to the output
test.file_grep(mapped + '.aig', r'aig')

test.passes()

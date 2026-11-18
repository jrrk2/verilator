#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('vlt')

bir = test.obj_dir + '/bir.json'
test.compile(verilator_flags2=['--bir-only-output', bir],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)

# the module, its register and its instance are in the IR
test.file_grep(bir, r'"name": *"t"')
test.file_grep(bir, r'"kind":')
test.file_grep(bir, r'"leaf"')

test.passes()

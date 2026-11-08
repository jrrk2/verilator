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

mapped = test.obj_dir + '/mapped.json'
test.compile(verilator_flags2=['--map-only-output', mapped],
             verilator_make_gmake=False,
             make_top_shell=False,
             make_main=False)
test.file_grep_not(mapped, r'"type": "DSP48E1"')
test.file_grep_count(mapped, r'"type": "LUT6"', 32)
test.passes()

# DESCRIPTION: Verilator: SDC constraints for t_synth_map_timing
#
# SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
# SPDX-License-Identifier: CC0-1.0

# one 200 MHz clock, late inputs
create_clock -period 5.0 -name clk [get_ports {clk}]
set_input_delay -clock clk 1.0 [get_ports {a b}]
set_output_delay -clock clk 1.0 [get_ports {y}]

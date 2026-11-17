libsdcparse, from https://github.com/verilog-to-routing/libsdcparse at commit
219021e (the last flex/bison parser; later versions are a Tcl interpreter
with a Python build step).  MIT, see LICENSE.txt.  Built by src/Makefile_obj
(bison -d / flex) for the mapper's --map-only-sdc (V3MapSdc.cpp).

Local change: create_clock also takes its targets as `[get_ports ...]` /
`[get_pins ...]`, the form Vivado XDC files use (sdc_parser.y, and the
matching assertion in sdc_common.cpp), and a netlist clock may be given a
-name (sdc_common.cpp).

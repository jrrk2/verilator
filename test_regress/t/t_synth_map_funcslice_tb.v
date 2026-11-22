// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

// The mapped netlist of t_synth_map_funcslice.v against its behaviour, with
// models of the cells it is made of

module LUT1 #(parameter [1:0] INIT = 0) (input I0, output O);
  assign O = INIT[I0];
endmodule

module tb;
  reg [7:0] a = 0;
  wire [7:0] r;
  wire [15:0] s;
  t dut(.a(a), .r(r), .s(s));
  initial begin
    for (int i = 0; i < 256; ++i) begin
      a = 8'(i);
      #1;
      if (r !== {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]} || s !== {8'hff, ~a}) begin
        $write("%%Error: a=%h gives r=%h s=%h\n", a, r, s);
        $stop;
      end
    end
    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule

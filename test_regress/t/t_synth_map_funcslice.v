// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

// Functions that build their result a part at a time: bit by bit in a loop, and
// whole then a slice on top.  The netlist gives every part.
module t(input [7:0] a, output [7:0] r, output [15:0] s);
  function automatic logic [7:0] rev(input logic [7:0] x);
    for (int i = 0; i < 8; i++) rev[i] = x[7 - i];
  endfunction
  function automatic logic [15:0] top_ones(input logic [7:0] x);
    top_ones = '1;
    top_ones[7:0] = ~x;
  endfunction
  assign r = rev(a);
  assign s = top_ones(a);
endmodule

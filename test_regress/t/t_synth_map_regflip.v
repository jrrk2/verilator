// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

// A register whose input another instance reads in the other polarity: both
// are outputs of the partition, and abc makes one an inverter of the other
// (p is deeper, so that node is not the critical one, which abc would
// duplicate).  The mapper then stores the register inverted, and the LUTs
// reading it must take the complement, in its own partition and in another.
module sub(input clk, input a, input b, input c, input [7:0] d,
           output n, output p, output reg q = 1'b0, output y);
  assign n = a | b;
  assign p = ^{d, a, b, c};
  always @(posedge clk) q <= n;
  assign y = q & c;
endmodule
module t(input clk, input a, input b, input c, input [7:0] d,
         output p, output y, output z);
  wire n, q;
  sub u(.clk(clk), .a(a), .b(b), .c(c), .d(d), .n(n), .p(p), .q(q), .y(y));
  assign z = (n ^ c) & (q | a);
endmodule

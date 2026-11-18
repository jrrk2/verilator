// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module sub(input clk, input [7:0] a, input [7:0] b, output reg [7:0] y);
  always @(posedge clk) y <= (a & b) | (a ^ {b[6:0], b[7]});
endmodule
module t(input clk, input [7:0] a, input [7:0] b, output [7:0] y0, output [7:0] y1);
  sub u0(.clk(clk), .a(a), .b(b), .y(y0));
  sub u1(.clk(clk), .a(b), .b(a), .y(y1));
endmodule

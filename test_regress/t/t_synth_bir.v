// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module leaf(input [3:0] a, output [3:0] y);
  assign y = ~a;
endmodule
module t(input clk, input [3:0] d, output reg [3:0] q);
  wire [3:0] n;
  leaf u(.a(d), .y(n));
  always @(posedge clk) q <= n;
endmodule

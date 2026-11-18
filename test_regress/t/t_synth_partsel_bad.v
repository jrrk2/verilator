// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input clk, input [1:0] i, input [1:0] j, input [3:0] d, input [1:0] ra, output [15:0] o);
  reg [15:0] mem [0:3];
  always @(posedge clk) mem[i][j*4+:4] <= d;
  assign o = mem[ra];
endmodule

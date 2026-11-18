// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input clk, input rst, input d, output reg q);
  // two edges but no "if (rst) ... else ..." chain: not a register with an async reset
  always @(posedge clk or posedge rst) q <= d;
endmodule

// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input clk, input clk2, input a, input b, output reg q);
  always @(posedge clk) q <= a;
  always @(posedge clk2) q <= b;
endmodule

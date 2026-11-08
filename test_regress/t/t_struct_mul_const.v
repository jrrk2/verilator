// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

// Multiplies by a constant of few signed digits are shifts and adds, not DSP48E1s: an index
// 32 * (p * W + l) keeps its 5 low bits visibly 0, so the read below selects one of 4 words
// (32 LUT6), not any of 1024 bit positions (tens of thousands of LUTs)
module t (
    input logic clk,
    input logic [1023:0] q,
    input logic [31:0] p32,
    input logic [31:0] x,
    output logic [31:0] d,
    output logic [31:0] y8,
    output logic [31:0] y7,
    output logic [31:0] y5,
    output logic [31:0] yn3
);
  localparam int W = 8;
  int p;
  int sx;
  assign sx = x;
  always_ff @(posedge clk) p <= p32;
  always_ff @(posedge clk) d <= q[32*(p*W+3)+:32];
  assign y8 = x * 8;
  assign y7 = x * 7;
  assign y5 = x * 5;
  assign yn3 = 32'(sx * -3);
endmodule

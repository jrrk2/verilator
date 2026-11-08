// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module sub (
    input logic clk,
    input logic [31:0] d[8],
    output logic [31:0] q[8]
);
  always_ff @(posedge clk)
    for (int k = 0; k < 8; k++) q[k] <= d[k] + 32'(k);
endmodule

module t (
    input logic clk,
    input logic [255:0] b,
    input logic [2:0] i,
    output logic [31:0] y
);
  // Local arrays connected whole to the child's array ports
  logic [31:0] in[8];
  logic [31:0] out[8];
  always_comb
    for (int k = 0; k < 8; k++) in[k] = b[32 * k+:32];
  sub u_sub (.clk, .d(in), .q(out));
  assign y = out[i];
endmodule

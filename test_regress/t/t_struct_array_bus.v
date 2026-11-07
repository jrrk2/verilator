// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t (
    input logic clk,
    input logic [7:0] a,
    input logic [2:0] i,
    input logic [255:0] b,
    output logic [31:0] y_comb,
    output logic [31:0] y_seq
);
  // An earlier initial block does not make the writes below part of it
  initial if (0) $display("never");

  // Written combinationally, at more bits than the memory threshold: a bus, not a RAM
  logic [31:0] bus[8];
  always_comb
    for (int k = 0; k < 8; k++) bus[k] = b[32 * k+:32] + 32'(a);
  assign y_comb = bus[i];

  // Written whole: a bank of registers
  logic [31:0] copy[8];
  always_ff @(posedge clk) copy <= bus;
  assign y_seq = copy[i];
endmodule

// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t (
    input logic en,
    input logic [1:0] sel,
    input logic [31:0] instr,
    input logic [7:0] a,
    input logic [7:0] b,
    input logic [7:0] c,
    input logic [7:0] d,
    output logic [7:0] y_full,
    output logic [7:0] y_unreach,
    output logic [7:0] y_latch,
    output logic [7:0] y_star,
    output logic [7:0] y_comb_in_latch,
    output logic [7:0] y_decode
);
  // Full without a default: no hold, so always_comb is satisfied
  always_comb
    case (sel)
      2'd0: y_full = a;
      2'd1: y_full = b;
      2'd2: y_full = c;
      2'd3: y_full = d;
    endcase

  // Full labels make the default unreachable
  always_comb
    unique case (sel)
      2'd0: y_unreach = a;
      2'd1: y_unreach = b;
      2'd2: y_unreach = c;
      2'd3: y_unreach = d;
      default: y_unreach = 8'hee;
    endcase

  // A real latch
  always_latch if (en) y_latch = a;

  // always @* that misses a path infers a latch
  always @* if (en) y_star = b;

  // always_latch assigning on every path is combinational
  always_latch y_comb_in_latch = en ? a : b;

  // Wide selector, few tested bits: decided by enumerating those bits
  always_comb
    casez (instr)
      32'b?????????????????????????????0?: y_decode = a;
      32'b?????????????????????????????10: y_decode = b;
      32'b?????????????????????????????11: y_decode = c;
    endcase
endmodule

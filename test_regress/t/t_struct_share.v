// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t (
    input logic c,
    input logic [1:0] op,
    input logic [2:0] sel,
    input logic [31:0] a,
    input logic [31:0] b,
    input logic [31:0] d,
    input logic [31:0] e,
    input logic [31:0] f,
    input logic [31:0] g,
    input logic [7:0] m0,
    input logic [7:0] m1,
    input logic [7:0] m2,
    input logic [7:0] m3,
    input logic [7:0] m4,
    input logic [7:0] m5,
    input logic [7:0] m6,
    input logic [7:0] m7,
    output logic [31:0] y_if,
    output logic [31:0] y_alu,
    output logic [31:0] y_cse1,
    output logic [31:0] y_cse2,
    output logic y_lt,
    output logic y_ge,
    output logic [7:0] y_mux
);
  // Opposite arms of an if/else: one adder behind an operand mux
  always_comb if (c) y_if = a + b; else y_if = d + a;

  // Exclusive case arms, adds and subtracts: one adder_carry
  always_comb
    case (op)
      2'd0: y_alu = e + f;
      2'd1: y_alu = e - f;
      2'd2: y_alu = e + g;
      2'd3: y_alu = e - g;
    endcase

  // The same sum twice is one adder
  assign y_cse1 = b + d;
  assign y_cse2 = (d + b) ^ a;

  // a < b and a >= b are one carry chain
  assign y_lt = a < d;
  assign y_ge = a >= d;

  // Constant labels: steered by the selector bits, no comparators; 8:1 muxes
  // map to two LUTs and a MUXF7
  always_comb
    case (sel)
      3'd0: y_mux = m0;
      3'd1: y_mux = m1;
      3'd2: y_mux = m2;
      3'd3: y_mux = m3;
      3'd4: y_mux = m4;
      3'd5: y_mux = m5;
      3'd6: y_mux = m6;
      3'd7: y_mux = m7;
    endcase
endmodule

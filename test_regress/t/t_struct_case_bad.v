// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t (
    input logic [1:0] sel,
    input logic [7:0] a,
    input logic [7:0] b,
    input logic [7:0] c,
    output logic [7:0] y_comb,
    output logic [7:0] y_overlap,
    output logic [7:0] y_notfull
);
  // Value 3 is not covered: always_comb would need a latch
  always_comb
    case (sel)
      2'd0: y_comb = a;
      2'd1: y_comb = b;
      2'd2: y_comb = c;
    endcase

  // 2'b11 matches the first two items
  always_comb begin
    y_overlap = '0;
    unique casez (sel)
      2'b1?: y_overlap = a;
      2'b?1: y_overlap = b;
      2'b00: y_overlap = c;
    endcase
  end

  // 2'b11 matches nothing and there is no default
  always_comb begin
    y_notfull = '0;
    priority case (sel)
      2'd0: y_notfull = a;
      2'd1: y_notfull = b;
      2'd2: y_notfull = c;
    endcase
  end
endmodule

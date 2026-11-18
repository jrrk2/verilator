// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input clk, input [15:0] a, input [15:0] b, output reg [31:0] y);
  reg [15:0] ra, rb;
  always @(posedge clk) begin
    ra <= a;
    rb <= b;
    y <= {ra + rb, ra ^ rb} + {16'd0, ra & rb};
  end
endmodule

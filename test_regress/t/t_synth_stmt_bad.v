// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input clk, input a, input d, output reg q);
  always @(posedge clk) begin
    wait (a);
    q <= d;
  end
endmodule

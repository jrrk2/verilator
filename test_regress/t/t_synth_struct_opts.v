// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module leaf(input [3:0] a, output [3:0] y);
  assign y = a + 4'd1;
endmodule
module t(input clk, input [1:0] sel, input [3:0] d, output reg [3:0] q, output reg [3:0] m);
  wire [3:0] n;
  leaf u(.a(d), .y(n));
  always @(posedge clk) q <= n;
  always @* begin
    case (sel)  // synopsys full_case parallel_case
      2'd0: m = d;
      2'd1: m = n;
      2'd2: m = q;
    endcase
  end
endmodule

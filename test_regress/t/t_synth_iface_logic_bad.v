// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

interface ifc(input clk);
  logic [3:0] cnt;
  always @(posedge clk) cnt <= cnt + 1;
endinterface
module t(input clk, output [3:0] o);
  ifc i(.clk(clk));
  assign o = i.cnt;
endmodule

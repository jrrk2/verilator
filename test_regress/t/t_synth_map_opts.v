// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input clk, input we, input [9:0] wa, input [9:0] ra, input [7:0] wd,
         input [7:0] a, input [7:0] b, input [7:0] c, input [7:0] d,
         output reg [7:0] rd, output reg [9:0] sum, output reg [7:0] q1, output reg [7:0] q2);
  reg [7:0] mem [0:1023];
  initial $readmemh("t/t_synth_map_opts.mem", mem);
  always @(posedge clk) begin
    if (we) mem[wa] <= wd;
    rd <= mem[ra];
    sum <= {2'd0, a} + {2'd0, b} + {2'd0, c} + {2'd0, d};  // a sum chain, kept as a chain
    q1 <= a ^ b;  // two registers with the same logic, kept apart
    q2 <= a ^ b;
  end
endmodule

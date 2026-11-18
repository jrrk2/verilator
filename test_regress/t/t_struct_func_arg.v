// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

// A function's formals and locals (Verilator keeps them apart from its
// statements): a call binds the argument and the body's loop variable is the
// function's own.  The result is the address pattern of a memory test.
module t (
    input logic clk,
    input logic [7:0] addr,
    output logic [31:0] q
);
  function automatic [31:0] pattern(input [7:0] a);
    reg [31:0] w;
    integer i;
    begin
      w = {~a, a, 16'h9e37} ^ 32'h9e3779b9;
      for (i = 0; i < 3; i = i + 1) w = {w[30:0], w[31]} ^ {24'd0, a};
      pattern = w;
    end
  endfunction
  always_ff @(posedge clk) q <= pattern(addr);
endmodule

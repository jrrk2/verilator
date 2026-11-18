// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input x, input y, output o);
  wire a, b;
  assign a = b & x;
  assign b = a | y;
  assign o = a;
endmodule

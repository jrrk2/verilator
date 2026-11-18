// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t(input [1:0] a, output [71:0] o);
  reg [71:0] rom [0:3];
  initial $readmemh("t/t_synth_rom_bad.hex", rom);
  assign o = rom[a];
endmodule

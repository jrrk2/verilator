// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

// The mapped netlist of t_synth_map_regflip.v against its behaviour, with
// models of the cells it is made of

module LUT1 #(parameter [1:0] INIT = 0) (input I0, output O);
  assign O = INIT[I0];
endmodule
module LUT2 #(parameter [3:0] INIT = 0) (input I0, input I1, output O);
  assign O = INIT[{I1, I0}];
endmodule
module LUT3 #(parameter [7:0] INIT = 0) (input I0, input I1, input I2, output O);
  assign O = INIT[{I2, I1, I0}];
endmodule
module LUT4 #(parameter [15:0] INIT = 0) (input I0, input I1, input I2, input I3, output O);
  assign O = INIT[{I3, I2, I1, I0}];
endmodule
module LUT5 #(parameter [31:0] INIT = 0) (input I0, input I1, input I2, input I3, input I4, output O);
  assign O = INIT[{I4, I3, I2, I1, I0}];
endmodule
module LUT6 #(parameter [63:0] INIT = 0)
  (input I0, input I1, input I2, input I3, input I4, input I5, output O);
  assign O = INIT[{I5, I4, I3, I2, I1, I0}];
endmodule
module FDRE #(parameter [0:0] INIT = 0) (input C, input CE, input D, input R, output reg Q = INIT);
  always @(posedge C) if (R) Q <= 1'b0; else if (CE) Q <= D;
endmodule
module FDSE #(parameter [0:0] INIT = 1) (input C, input CE, input D, input S, output reg Q = INIT);
  always @(posedge C) if (S) Q <= 1'b1; else if (CE) Q <= D;
endmodule

module tb;
  reg clk = 0, a = 0, b = 0, c = 0;
  reg [7:0] d = 0;
  wire p, y, z;
  t dut(.clk(clk), .a(a), .b(b), .c(c), .d(d), .p(p), .y(y), .z(z));
  reg q = 0;
  always @(posedge clk) q <= a | b;
  initial begin
    for (int i = 0; i < 64; ++i) begin
      {a, b, c} = 3'(i ^ (i >> 3));
      d = 8'(i * 37);
      #1;
      if (p !== ^{d, a, b, c} || y !== (q & c) || z !== (((a | b) ^ c) & (q | a))) begin
        $write("%%Error: step %0d: a=%b b=%b c=%b d=%h q=%b gives p=%b y=%b z=%b\n", i, a, b,
               c, d, q, p, y, z);
        $stop;
      end
      clk = 1;
      #1 clk = 0;
    end
    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule

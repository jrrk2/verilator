// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module t (
    input logic en,
    input logic [3:0] a,
    input logic [1:0] m,
    output logic [3:0] y_if,
    output logic [3:0] y_nest
);
  // The index is assigned only when en is 1, but y_if is complete: no latch
  always_comb begin
    y_if = '0;
    if (en) for (int i = 0; i < 4; i++) y_if[i] = a[3 - i];
  end

  // The inner index is assigned on a path of the outer loop's body
  always_comb begin
    y_nest = '0;
    for (int k = 0; k < 2; k++)
      if (m[k])
        for (int l = 0; l < 2; l++) y_nest[2 * k + l] = a[l];
  end
endmodule

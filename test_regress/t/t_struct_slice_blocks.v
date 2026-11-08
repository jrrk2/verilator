// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

// One block per element of a packed array, each writing its element and then bits of it:
// no block may drive another's bits
module t (
    input logic [3:0][3:0] req,
    input logic [3:0][7:0] data,
    output logic [3:0][3:0] pv,
    output logic [3:0][3:0][7:0] pd,
    input logic [1:0] at,
    output logic [3:0][3:0] mk,
    output logic [7:0] part
);
  for (genvar p = 0; p < 4; p++) begin : g_p
    always_comb begin
      pv[p] = '0;
      pd[p] = '0;
      for (int b = 0; b < 4; b++)
        if (req[p][b]) begin
          pv[p][b] = 1'b1;
          pd[p][b] = pd[p][b] | data[b];
        end
    end
  end
  // a bit at a dynamic index, within an element just written
  always_comb
    for (int g = 0; g < 4; g++) begin
      mk[g] = '0;
      if (req[g][0]) mk[g][at] = 1'b1;
      if (req[g][1]) mk[g][at + 2'(g)] = 1'b1;
    end
  // a write that overlaps part of an earlier one on each side
  always_comb begin
    part[3:0] = data[0][3:0];
    part[7:4] = data[0][7:4];
    part[5:2] = data[1][3:0];
  end
endmodule

// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: CC0-1.0

module sub (
    input logic clk,
    input logic [7:0] d[3],
    output logic [7:0] q[3]
);
  always_ff @(posedge clk)
    for (int k = 0; k < 3; k++) q[k] <= d[k] + 8'(k);
endmodule

module sub2 (
    input logic [7:0] d[3],
    output logic [7:0] st[2][3]
);
  always_comb
    for (int s = 0; s < 2; s++)
      for (int l = 0; l < 3; l++) st[s][l] = d[l] ^ 8'(s);
endmodule

module t (
    input logic clk,
    input logic rst,
    input logic [7:0] in,
    input logic [1:0] i,
    input logic [1:0] j,
    output logic [7:0] y0,
    output logic [7:0] y1,
    output logic [7:0] y2,
    output logic [7:0] y3,
    output logic [7:0] y4,
    output logic [7:0] y5,
    output logic [7:0] y6,
    output logic [7:0] y7
);
  // registers written and read at dynamic indexes in both dimensions, with a dimension of three
  logic [7:0] r[3][4];
  // constant in one dimension, dynamic in the other
  logic [7:0] c[2][4];
  // bits of an element at a dynamic index
  logic [3:0] p[4];
  // a range with a nonzero low bound
  logic [7:0] o[1:2][3:4];
  // whole arrays and a sub-array
  logic [7:0] a[3];
  logic [7:0] b[3];
  logic [7:0] m[2][3];
  logic [7:0] sq[3];
  logic [7:0] pq[3];
  logic [7:0] st[2][3];
  // one dimension with a nonzero low bound: read and written at constant and dynamic indexes
  logic [7:0] z[1:3];
  logic [7:0] rz[3:1];

  function automatic logic [7:0] sum2(input logic [7:0] v[3], input logic [1:0] n);
    sum2 = v[n == 2'd3 ? 2'd0 : n] + v[0];
  endfunction

  always_ff @(posedge clk) begin
    if (rst) begin
      for (int x = 0; x < 3; x++) for (int w = 0; w < 4; w++) r[x][w] <= '0;
      for (int x = 0; x < 2; x++) for (int w = 0; w < 4; w++) c[x][w] <= '0;
      for (int w = 0; w < 4; w++) p[w] <= '0;
      for (int x = 0; x < 3; x++) a[x] <= '0;
      for (int x = 1; x <= 3; x++) z[x] <= '0;
      for (int x = 1; x <= 3; x++) rz[x] <= '0;
      for (int x = 0; x < 3; x++) b[x] <= '0;
      for (int x = 1; x <= 2; x++) for (int w = 3; w <= 4; w++) o[x][w] <= '0;
    end
    else begin
      if (i != 2'd3) r[i][j] <= in;
      c[0][j] <= in;
      c[1][i] <= c[0][j] + 8'd1;
      p[i][2:1] <= in[1:0];
      p[j][3] <= in[1];
      a[j == 2'd3 ? 2'd0 : j] <= in;
      b <= a;
      o[i[0] ? 1 : 2][j[0] ? 3 : 4] <= in;
      if (i != 2'd0) z[i] <= in;
      z[2] <= z[2] + 8'd1;
      if (j != 2'd0) rz[j] <= in;
      rz[1] <= rz[3];
    end
  end

  assign m[0] = a;
  assign m[1][0] = b[2];
  assign m[1][1] = b[1];
  assign m[1][2] = b[0];

  sub u_sub (.clk, .d(m[1]), .q(sq));
  sub u_sub2 (.clk, .d(st[1]), .q(pq));
  sub2 u_st (.d(a), .st(st));

  assign y0 = i == 2'd3 ? 8'd0 : r[i][j];
  assign y1 = c[j[0]][i] ^ c[1][j];
  assign y2 = {p[i], p[j]};
  assign y3 = sum2(m[i[0]], j) + sum2(b, i);
  assign y4 = sq[j == 2'd3 ? 2'd1 : j] ^ pq[i == 2'd3 ? 2'd2 : i];
  assign y5 = o[j[0] ? 1 : 2][i[0] ? 3 : 4] ^ st[i[0]][j == 2'd3 ? 2'd0 : j];
  assign y6 = (j == 2'd0 ? 8'd7 : z[j]) ^ z[1];
  assign y7 = (i == 2'd0 ? 8'd9 : rz[i]) ^ rz[3];
endmodule

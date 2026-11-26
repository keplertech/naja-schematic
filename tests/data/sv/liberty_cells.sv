// Instances of the Liberty cells in ../liberty/cells.lib (loaded first).
module cells_top(input a, b, clk, output q, y);
  wire n;
  NAND2X1 u_nand(.A(a), .B(b), .Y(n));
  INVX1 u_inv(.A(n), .Y(y));
  DFFX1 u_ff(.CK(clk), .D(n), .Q(q));
endmodule

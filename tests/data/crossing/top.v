// The design of tests/data/crossing_assign_protocol_cases.json: nets
// crossing hierarchy through an assign inside a module (a feed-through
// whose two sides are different nets) and through bus pins.
module DRV(input A, output Y);
endmodule

module RCV(input A, output Y);
endmodule

module thru(input i, output o);
  assign o = i;
endmodule

module busmod(input [1:0] d, output [1:0] q);
  RCV g0(.A(d[0]), .Y(q[0]));
  RCV g1(.A(d[1]), .Y(q[1]));
endmodule

module top(input a, output y, output [1:0] z);
  wire n;
  wire m;
  wire [1:0] b;
  DRV  drv(.A(a), .Y(n));
  thru u_thru(.i(n), .o(m));
  RCV  sink(.A(m), .Y(y));
  DRV  d0(.A(a), .Y(b[0]));
  DRV  d1(.A(a), .Y(b[1]));
  busmod u_bus(.d(b), .q(z));
endmodule

// The design of tests/data/top_view_protocol_cases.json: a small top with
// a bus port, an assign joining two nets, a hierarchical instance and
// unconnected ports, and an instance with an unconnected pin (u4's Y is
// alone on its net).
module LEAF(input A, output Y);
endmodule

module sub(input i, output o);
  LEAF u0(.A(i), .Y(o));
endmodule

module top(input a, input [1:0] b, output y, output z, output nc);
  wire w;
  wire w2;
  LEAF u1(.A(a), .Y(w));
  sub  u2(.i(w), .o(w2));
  assign y = w2;
  LEAF u3(.A(b[0]), .Y(z));
  wire dangling;
  LEAF u4(.A(a), .Y(dangling));
endmodule

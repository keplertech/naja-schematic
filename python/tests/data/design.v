module sub(input a, input b, output y);
  wire n;
  LUT2 #(.INIT(4'h8)) u_and (.I0(a), .I1(b), .O(n));
  assign y = n;
endmodule
module top(input clk, input [1:0] d, output q);
  wire s;
  sub u_sub (.a(d[0]), .b(d[1]), .y(s));
  FDRE r (.C(clk), .CE(1'b1), .R(1'b0), .D(s), .Q(q));
endmodule

// A high-fanout net split by duplicating its driver: three copies of the
// buffer on `a`, each driving two of the six flops, all enabled by `en`.
// What a fanout-fix report colors: en, each copy's net, and the sinks.
module top(input clk, input en, input a, output [5:0] q);
  wire n0, n1, n2;
  LUT1 #(.INIT(2'h2)) drv_0 (.I0(a), .O(n0));
  LUT1 #(.INIT(2'h2)) drv_1 (.I0(a), .O(n1));
  LUT1 #(.INIT(2'h2)) drv_2 (.I0(a), .O(n2));
  FDRE s0 (.C(clk), .CE(en), .R(1'b0), .D(n0), .Q(q[0]));
  FDRE s1 (.C(clk), .CE(en), .R(1'b0), .D(n0), .Q(q[1]));
  FDRE s2 (.C(clk), .CE(en), .R(1'b0), .D(n1), .Q(q[2]));
  FDRE s3 (.C(clk), .CE(en), .R(1'b0), .D(n1), .Q(q[3]));
  FDRE s4 (.C(clk), .CE(en), .R(1'b0), .D(n2), .Q(q[4]));
  FDRE s5 (.C(clk), .CE(en), .R(1'b0), .D(n2), .Q(q[5]));
endmodule

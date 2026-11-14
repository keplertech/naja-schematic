// After a fanout fix: the enable register duplicated three times, each copy
// driving four of the twelve flops (fanout 12 -> 3 x 4).
module top(input clk, input en, input [11:0] d, output [11:0] q);
  wire en_q, en_q_fo1, en_q_fo2;
  FDRE en_reg (.C(clk), .CE(1'b1), .R(1'b0), .D(en), .Q(en_q));
  FDRE en_reg_fo1 (.C(clk), .CE(1'b1), .R(1'b0), .D(en), .Q(en_q_fo1));
  FDRE en_reg_fo2 (.C(clk), .CE(1'b1), .R(1'b0), .D(en), .Q(en_q_fo2));
  FDRE s0 (.C(clk), .CE(en_q), .R(1'b0), .D(d[0]), .Q(q[0]));
  FDRE s1 (.C(clk), .CE(en_q), .R(1'b0), .D(d[1]), .Q(q[1]));
  FDRE s2 (.C(clk), .CE(en_q), .R(1'b0), .D(d[2]), .Q(q[2]));
  FDRE s3 (.C(clk), .CE(en_q), .R(1'b0), .D(d[3]), .Q(q[3]));
  FDRE s4 (.C(clk), .CE(en_q_fo1), .R(1'b0), .D(d[4]), .Q(q[4]));
  FDRE s5 (.C(clk), .CE(en_q_fo1), .R(1'b0), .D(d[5]), .Q(q[5]));
  FDRE s6 (.C(clk), .CE(en_q_fo1), .R(1'b0), .D(d[6]), .Q(q[6]));
  FDRE s7 (.C(clk), .CE(en_q_fo1), .R(1'b0), .D(d[7]), .Q(q[7]));
  FDRE s8 (.C(clk), .CE(en_q_fo2), .R(1'b0), .D(d[8]), .Q(q[8]));
  FDRE s9 (.C(clk), .CE(en_q_fo2), .R(1'b0), .D(d[9]), .Q(q[9]));
  FDRE s10 (.C(clk), .CE(en_q_fo2), .R(1'b0), .D(d[10]), .Q(q[10]));
  FDRE s11 (.C(clk), .CE(en_q_fo2), .R(1'b0), .D(d[11]), .Q(q[11]));
endmodule

module top(input a, input b, input clk, output y, output q);
  wire n;
  PY_AND2 u_and (.A(a), .B(b), .Y(n));
  PY_AND2 u_and2 (.A(n), .B(a), .Y(y));
  PY_DFF u_ff (.D(y), .C(clk), .Q(q));
endmodule

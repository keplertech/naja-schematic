(* src = "rtl/alu.v:1.1-3.9" *)
module alu(a, y);
  (* src = "rtl/alu.v:1.12-1.24" *)
  input [1:0] a;
  (* src = "rtl/alu.v:1.27-1.34" *)
  output y;
  wire n1;
  wire [1:0] s;
  (* src = "rtl/alu.v:2.14-2.24|rtl/alu.v:1.12-1.24" *)
  and g0 (y, a[0], a[1]);
  (* src = "rtl/alu.v:2.14" *)
  and (n1, a[0], a[1]);
  (* src = "/synth/machine/rtl/alu.v:2.14-2.24" *)
  and g2 (s[0], a[0], a[1]);
endmodule

module top(x, z);
  input [1:0] x;
  output z;
  (* src = "rtl/top.v:2.3-2.30" *)
  alu u_alu (.a(x), .y(z));
endmodule

module alu(input [1:0] a, output y);
  assign y = a[0] & a[1];
endmodule

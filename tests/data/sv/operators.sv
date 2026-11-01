module ops(input logic [3:0] a, b, input logic [1:0] n, input logic s, input logic x, y, z,
           output logic [3:0] sum, diff, prod, shl, shr, sra, m, q, r,
           output logic m1);
  assign sum = a + b;
  assign diff = a - b;
  assign prod = a * b;
  assign shl = a << n;
  assign shr = a >> n;
  assign sra = $signed(a) >>> n;
  assign m = s ? a : b;
  assign m1 = x ? y : z;
  assign q = a / b;
  assign r = a % b;
endmodule

// Small gate-level example exercising one instance of every PrimitiveType
// the standard shapes library (SchematicView::drawInstance()) recognizes --
// AND/NAND/OR/NOR/XOR/XNOR at a few input arities, INV/BUF, and a DFF --
// using cell names from the Nangate library (NangateOpenCellLibrary_typical.lib)
// in this directory. Load with:
//   naja-schematic-standalone scripts/gate_shapes_example.v \
//     --liberty scripts/NangateOpenCellLibrary_typical.lib

module top(
  input  a, b, c, d,
  input  clk, rst_n,
  output y_and2, y_and3, y_and4,
  output y_nand2, y_nand3,
  output y_or2, y_nor2,
  output y_xor2, y_xnor2,
  output y_inv, y_buf,
  output y_q, y_qn
);

  AND2_X1  u_and2  (.A1(a), .A2(b), .ZN(y_and2));
  AND3_X1  u_and3  (.A1(a), .A2(b), .A3(c), .ZN(y_and3));
  AND4_X1  u_and4  (.A1(a), .A2(b), .A3(c), .A4(d), .ZN(y_and4));

  NAND2_X1 u_nand2 (.A1(a), .A2(b), .ZN(y_nand2));
  NAND3_X1 u_nand3 (.A1(a), .A2(b), .A3(c), .ZN(y_nand3));

  OR2_X1   u_or2   (.A1(a), .A2(b), .ZN(y_or2));
  NOR2_X1  u_nor2  (.A1(a), .A2(b), .ZN(y_nor2));

  XOR2_X1  u_xor2  (.A(a), .B(b), .Z(y_xor2));
  XNOR2_X1 u_xnor2 (.A(a), .B(b), .ZN(y_xnor2));

  INV_X1   u_inv   (.A(a), .ZN(y_inv));
  BUF_X1   u_buf   (.A(a), .Z(y_buf));

  DFFR_X1  u_dff   (.D(a), .CK(clk), .RN(rst_n), .Q(y_q), .QN(y_qn));

endmodule

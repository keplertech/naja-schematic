-- One level of the VHDL test design: a 2:1 mux on bits.
entity leaf is
  port (a, b, s : in bit; y : out bit);
end entity;

architecture rtl of leaf is
begin
  y <= a when s = '1' else b;
end architecture;

-- Top of the VHDL test design (tests/data/vhdl): instantiates leaf.vhd.
entity top is
  port (a, b, s : in bit; y, n : out bit);
end entity;

architecture rtl of top is
  signal m : bit;
begin
  u_leaf : entity work.leaf port map (a => a, b => b, s => s, y => m);
  y <= m;
  n <= not (a and b);
end architecture;

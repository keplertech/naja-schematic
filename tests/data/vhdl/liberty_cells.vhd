-- Instances of the Liberty cells in ../liberty/cells.lib (loaded first),
-- declared as components in lower case: VHDL names are case-insensitive.
library ieee;
use ieee.std_logic_1164.all;

entity cells_top is
  port (a, b, clk : in std_logic; q, y : out std_logic);
end entity;

architecture rtl of cells_top is
  component nand2x1 port (a, b : in std_logic; y : out std_logic); end component;
  component invx1 port (a : in std_logic; y : out std_logic); end component;
  component dffx1 port (ck, d : in std_logic; q : out std_logic); end component;
  signal n : std_logic;
begin
  u_nand : nand2x1 port map (a => a, b => b, y => n);
  u_inv : invx1 port map (a => n, y => y);
  u_ff : dffx1 port map (ck => clk, d => n, q => q);
end architecture;

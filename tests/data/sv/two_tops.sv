// Two uninstantiated modules: --top picks which one is the top.
module first(input a, output y);
  assign y = a;
endmodule

module second(input a, output y);
  assign y = ~a;
endmodule

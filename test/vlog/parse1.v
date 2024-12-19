module parse1;
  wire [8:0] x, y, z;
  reg        z;
  assign y = x;
  always begin : foo
    $display("hello");
    $finish;
    if (x);
    if (x) z <= 1;
    if (x) $display("yes");
    else if (z);
    else $display("no");
    z = 0;
    z = ~x;
    z = !x;
  end
  assign x = x | y;
  pulldown (supply0) p1 (x);
  pulldown p2 (x);
  pullup p3 (y);
  pullup (supply0, supply1) p4 (y);
  pullup (y);
  always @(x or y or (posedge z)) begin
  end
  initial begin
    while (z);
    repeat (x) x = ~x;
    do x = y; while (1);
  end
  initial begin
    x = x << 1;
    x = x >> 1;
    x = y <<< x;
    x = x >>> y;
  end
  int r1 = 5;
  wire w1 = 0;
  task task1;
    @(x) y = 2;
  endtask // task1
  task task2(input int x, output logic y);
    y = !x;
  endtask // task2
  function sum(input int a, b);
    sum = a + b;
  endfunction // sum
  initial begin
    wait (x) y = 1;
    x = x ? 1 : y;
    x = x - y * x + 1 % y / x;
  end
  parameter x = 6;
  integer   r2 = 66;
  real      r3 = 1.0;
  shortreal r4 = 6.7;
  realtime  r5 = 1.0;
  assign r2 = {x, y};
  assign {r1, r2} = {x, y};
endmodule // parse1

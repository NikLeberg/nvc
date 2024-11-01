package foo_pkg is 
end foo_pkg;

package body foo_pkg is
  shared variable var : integer; 

  package bar_pkg is
  end bar_pkg;

  package body bar_pkg is
    shared variable var : integer; 
  end package body bar_pkg;
end package body foo_pkg;

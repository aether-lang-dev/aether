- **An array bound in an `if`'s branches can be read after it.** The
  branch hoist declared the name with its C type before it, and an array's
  C type is `E[N]`, so `if c { a = [1, 2] } else { a = [3, 4] }` followed
  by a read of `a` failed in C. The hoist now declares it as a loop's
  does, `E a[N]` (#2877).

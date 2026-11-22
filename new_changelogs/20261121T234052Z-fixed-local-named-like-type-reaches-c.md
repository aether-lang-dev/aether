- **A local named like a struct, enum, message or actor no longer hides
  the type from the generated C.** `Other = n` and then `o = Other { q: "x" }`
  type-checked and stopped in the C compiler ("must use 'struct' tag to refer
  to type 'Other'"): C put the variable in scope over the typedef, so every
  later spelling of the type, a compound literal or a local of an enum type a
  call returns, named the variable. Such a local (a parameter or pattern
  variable too) now has a C name of its own from its binding on, as a local
  shadowing a function has (#2683); a use of the type before the binding
  keeps it. The checker and inference also took the local for the type a
  struct literal names and a field read reads, so `o.q` "had no type": the
  type a value has is now found past a local of its name.

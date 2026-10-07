- **A const may name a const declared after it (#2495).** Codegen wrote each
  top-level const as a C definition in source order, and a C initializer can
  only name a definition above it, so `const HIGH = LOW << 4` ahead of
  `const LOW = 3` stopped the C compiler with "'ae_const_LOW' undeclared".
  Consts, module `var`s and the consts merged in from modules are now
  emitted in dependency order; a program whose consts were already in order
  is emitted as before. A const that depends on itself, directly or through
  others (`const A = B + 1`, `const B = A * 2`), is reported at its
  declaration: `const 'A' depends on itself: A -> B -> A`.
- **A struct argument of the wrong struct type is a type error (#2491).**
  Passing a `Narrow` where the parameter takes a `Wide` went through to gcc as
  "incompatible type for argument", one error per compile, while the same
  values in an assignment were already rejected. Each such argument is now
  reported at the argument (`Argument 1 'a' of 'length2': expected Wide, got
  Narrow`), all of them in one pass. `*T` and `ptr` parameters, and a variant
  struct passed for its sum type, are unchanged.
- **Assigning to a call's result is a type error (#2481).** A call's value is
  a temporary: `copy(q).x = 0.0` and `copy(q) = v` stopped the C compiler at
  "lvalue required" against generated code, `grid().cells[0] = 9` compiled
  and lost the write, and `next_int(1) += 4` stopped the parser with
  "Expected statement in block". Each is now reported at the assignment as
  `cannot assign to the result of 'copy(...)': a call's result is a
  temporary`. A field or element reached through a pointer the call returned
  (`holder(&q).x = 5.0` with a `*P` result, an element of a returned slice)
  is real storage and still assigns.

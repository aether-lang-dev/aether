- **E0900 sees slices that dangle through a closure.** A closure storing its
  own array literal into a variable of the frame it is written in (directly
  or through a local), a bare binding inside a closure carrying a view of
  the frame's storage, and `call(get)` of a closure returning one were
  accepted and read freed memory (#2846).

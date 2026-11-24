- **A local named like an imported function no longer breaks the build.**
  After `import m (*)` (or `import m (button)`), `button = button(t)` in a
  loop called an undeclared `m_button` in the C: the dead-code prune read
  the right-hand call as a call through the local and dropped the
  function. A call before the local's binding is the function again, and
  one after it, which the checker used to take for the import as well, is
  now E0301 "'button' is a local here, which hides the imported function
  'm.button'", as for a program's own function (#2683).

- **`quickjs.rethrow(q)`: a host function can pass on the exact exception a
  nested call raised.** A host function that calls back into JavaScript
  (`quickjs.call`) and sees it fail could only throw a new error carrying
  the text, so a script catching it lost the type (a `RangeError` became an
  `Error`) and the stack. The runtime now keeps the exception value beside
  its text, and `return quickjs.rethrow(q)` throws that same value again;
  with none kept (a full heap, a host-side refusal) it throws an
  `InternalError` with the text. The kept value is freed on the next
  failure and at `quickjs.dispose`; leak-clean under valgrind.

- **A panic in a library export no longer dies of SIGILL or SIGSEGV when the
  library's C and the runtime were compiled differently for control-flow
  protection.** On x86 an export stub saves its context with
  `__builtin_setjmp` and a panic jumped back with `__builtin_longjmp` from the
  runtime, but the builtins' buffer layout depends on `-fcf-protection`: with
  it slot 2 holds the shadow-stack pointer and slot 3 the stack pointer. A
  release runtime is built by Ubuntu's gcc, which protects by default, and a
  library built with Arch's gcc 16 is not, so on CachyOS (an N100, which has
  CET with shadow stacks off) the runtime read the stack pointer as a
  shadow-stack pointer and ran `incsspq`, which faults: RexxAE's
  `test_resource_cleanup.py` died of SIGILL. The other way round it loaded a
  stack pointer of 0. The stub now defines the jump itself and hands it to
  `aether_exec_enter_at` with the buffer, so one compiler with one set of flags
  writes and reads it; nothing changes for the program's own CET settings.
  Runtime ABI revision 5.

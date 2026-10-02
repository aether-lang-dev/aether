- **A binary library's actors run in a program that has none.** A program
  with no actors of its own never initialized the scheduler. A library it
  imported in binary form that spawned more than one actor then crashed
  with an integer division by zero. Now:
  - The first spawn initializes the scheduler and starts it, for an Aether
    program and for a C host alike. A second `scheduler_init()` while one is
    live is a no-op, so it cannot wipe the tables of actors already running.
  - A library's catalog says whether it runs actors (`actors`, schema 1.7;
    `ae lib-info` prints `Actors:`). That includes a library that only
    calls one that does.
  - A program importing such a library has `main()` run the scheduler and
    drain it on the way out, so a message the library sent fire-and-forget
    is still delivered. On macOS and Windows that takes the shared runtime;
    for a library built static, `ae` warns that in-flight messages are lost
    at exit (#2297).

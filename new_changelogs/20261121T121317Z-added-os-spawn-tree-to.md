- **`os.spawn_tree_to`: a child whose whole process tree ends with the
  process that started it.** `spawn_proc_to`'s child leads a process group and
  `os.kill` signals the group, which ends the tree only while the parent lives
  and asks, and misses a descendant that left the group with `setsid`. Asked
  for at spawn time, `os.spawn_tree_to(prog, argv, env, out_path, grace_ms:
  2000)` returns a handle the caller owns, and the tree ends when the caller's
  process ends however it ends (SIGKILL included), when the handle asks
  (`os.tree_kill`, or `os.tree_free` while it runs), and when the program
  exits on its own, so once `os.tree_token(tree)` reaps nothing it started is
  running: SIGTERM, the grace period, then SIGKILL until none is left. Linux
  and FreeBSD use a supervisor process that is a child subreaper and watches a
  pipe only the caller holds; macOS, which has no subreaper, lists the
  descendants every 50 ms instead, so one that leaves the group and is
  orphaned within a poll is missed; Windows uses a Job Object with
  kill-on-close. The default spawns are unchanged. Asked for by selaenium,
  which can drop its own supervisor.

- **`std.guest`: the host calls the guest.** A contained guest's script
  can now leave functions behind for the host to call later: it ends with
  `READY` and their names instead of `DONE`, and serves. The host calls
  one by name with `guest.invoke` (`INVOKE`, answered with `RETURN` or
  `RAISE`, which `guest.result` turns into `(value, err)`) and finishes
  with `guest.end` (`END`, answered with `DONE 0`); `guest.run_script` and
  `guest.call_export` run a script or an invocation to its end while a
  handler answers the guest's own calls. Host and guest strictly
  alternate: `invoke` is refused unless the guest is idle, `next` on an
  idle guest returns `IDLE` without running the deadline down, and the
  deadline, `max_frame` and the kill hold inside an invocation as in a
  script. The `READY` list is bounded (1 to 256 distinct names of letters,
  digits, `_` and `.`, at most 128 bytes each), and a frame from an idle
  guest, or one answering what was not asked, hangs up. `invoke` refuses
  an `INVOKE` frame longer than the 64 KiB a worker reads, saying why and
  sending nothing, where it used to send it and lose the worker. The
  worker's side is `child.ready`, `child.serve`, `child.returned` and
  `child.raised`. A worker that exports nothing behaves exactly as before
  on the wire; `guest.Guest` gains `phase`, `exported` and `pending`. The
  ways of calling across that divide (a direct call with its arguments,
  notify then fetch as a suggested pattern, and the zero-change pull
  pattern), side by side, and the questions still open are in
  `docs/design/guest-host-calls-guest.md`.

- **A core stepping one actor many times in a row puts its step context
  aside once for the run, not once per message (#3858).** A burst to one
  actor fills its mailbox, and the core makes room by stepping it, eight
  messages at a time; each step paid the whole barrier: the reply slot, the
  step actor and the in-step flags saved and restored, the caps word read,
  on macOS a `_tlv_get_addr` call for each. Those runs, and the drain of a
  backlog after delivery, now share one put-aside and one give-back. Each
  message keeps its own try frame and `sigsetjmp`, its unanswered ask, its
  death check and its after-step work, so a panic ends the run exactly as it
  ended a single step; and a run goes on only while the step would have put
  nothing aside itself (no spawn capture, account or sandbox level, no
  execution running on the thread). A single step (a send's, a ping-pong's)
  is the barrier it was. The core also puts a run of one actor's messages
  into its mailbox as many at once as fit, with one store of the count,
  where it stored it once per message, when nothing waits in the actor's
  overflow. The cross-language counting benchmark (one actor, messages from
  main; Apple M5 Pro, median of 15 interleaved runs) went from 55.3 to 73.3 M
  msg/s; bench-core's `fanin_main_burst` from 16.4-17.0 to 12.5-13.3 ns/msg
  and `fanin_8_actors` from 69.0-69.7 to 61.2-64.8, the send, ask and
  ping-pong rows unchanged.

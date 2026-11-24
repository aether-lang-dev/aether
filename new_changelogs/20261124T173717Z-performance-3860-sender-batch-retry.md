- **A batch sent from main that finds a core's channel partly full waits
  for room instead of sending the rest one message at a time (#3860).**
  The rest was given back its counts and sent again, one message each:
  counted and routed again, the channel's lock taken and the core told for
  every message, a yield every 1000 spins. A sender that outruns one actor
  lived in that path, and its lock traffic slowed the receiving core too.
  The batch now keeps its counts (which hold each actor on the core it was
  routed to) and retries the rest under the lock. A batch whose messages
  all go to one core is enqueued from the batch buffer as it stands, with
  no sorted copy. Cross-language counting on an Apple M5 Pro: +5.2% and
  +6.8% in two interleaved rounds, with less CPU.

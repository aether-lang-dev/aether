- **The cross-language benchmark ranks a language within its tier, not in
  one list of eleven (FAIRNESS.md rule 12, "Two tiers").** Under the suite's
  rules C, C++, Rust and Zig are OS threads joined by the suite's bounded
  single-producer ring, and Java and Scala threads with
  `java.util.concurrent` queues, while Aether, Go, Erlang, Elixir and Pony
  are runtimes with unbounded mailboxes, scheduling and failure isolation.
  On counting the single list read as "Aether 5th" behind a hand-written
  two-thread ring; among the runtimes it is first. The runner writes each
  language's `tier` into the results, and the dashboard ranks, computes
  Relative and Spread, and summarizes Aether within the tier, with the
  threads tier shown as the floor and Aether's place overall beside its
  rank. Results written before this have no `tier` and are placed by name.

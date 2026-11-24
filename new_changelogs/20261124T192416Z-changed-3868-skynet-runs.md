- **The cross-language skynet takes its median over 25 runs (#3868).**
  One skynet run creates 1,111 units in about 0.3 ms, short enough that a
  median of five runs was mostly scheduling jitter (cv 14-27%, Pony's lead
  inside it). The runner now times skynet 25 times for every language
  (`BENCH_SKYNET_RUNS`, never fewer than `BENCH_RUNS`) and records the
  count in the results; the other patterns keep five.

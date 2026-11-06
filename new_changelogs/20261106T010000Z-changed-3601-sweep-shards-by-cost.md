- **CI's split test legs divide the sweep by cost, not by position (#3601).** The halves of a split leg took every other test of the list and finished minutes apart:
  - macOS x86_64 took 13 and 21 to 26 minutes;
  - Windows GCC took 18.6 and 22 to 28 minutes, splitting by thirds;
  - ASan and the macOS leaks gate were 2 to 7 minutes apart.

  `tests/scripts/sweep_shard.sh` now reads `tests/sweep_weights_<os>.txt`, each test's time on that system's CI. It deals the tests out heaviest first, each to the shard with the lowest total so far, and every job computes the same partition. A test the file does not name costs the median, and without the file the split is by position as before. One file per system, because a test can cost minutes on one and be skipped on another (`cross_ios`, 182 s on macOS).

  On the measured costs, the halves now come out at 963.9 / 964.2 s (Linux), 2818 / 2818 s (macOS) and 2336 / 2335 s (Windows). Windows GCC's `make ci` half takes 8 of 20 shares, since it also runs `make ci`'s other stages.

  The weights come from `AE_SWEEP_TIMINGS_OUT`, which writes every test's time; the main matrix and Windows GCC legs upload it. `tests/scripts/sweep_weights.sh` turns those uploads into the files. macOS's sweep timings now count whole seconds, where BSD `date` gave none.

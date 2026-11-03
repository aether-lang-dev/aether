- **`-DAETHER_ACCT_FILTER_LOG2=<n>` sizes the execution-account address
  filter (#3410) for small-RAM targets.** The filter is a fixed 32 KiB of
  static memory (2^18 bits) in every program, used or not: on an ESP32-S3
  that is about a tenth of the internal DRAM. A target can now build the
  runtime with 2^n bits (6..24); 14 gives 2 KiB at 16 times the
  false-positive rate (a false positive costs one table probe under the
  lock). The default is unchanged.

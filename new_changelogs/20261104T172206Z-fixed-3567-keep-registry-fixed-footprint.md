- **The record of kept strings has a fixed footprint from the first keep
  (#3567).** The record of the references `string.keep` and a C field's
  store hand over is 16 shards, chosen by the string's address, and each
  made its table at its own first record and kept it. On a heap that does
  not hand the same blocks back, a round of keeps that were all given back
  could reach a shard for the first time and grow the heap by its table,
  so a steady-heap check of such a program failed (seen on Windows UCRT64).
  Every shard's first table is now one block, made at the first record.

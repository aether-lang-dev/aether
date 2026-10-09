- **The Windows release archive no longer extracts files dated in the
  future.** A zip entry's main timestamp is a DOS time with no zone, written
  in the packer's zone (UTC on the runner) and read in the extractor's, so on
  a machine west of UTC the installed files were hours newer than the clock
  for an extractor that reads only that field, which can confuse any check of
  modification times. The archive is now packed with its DOS times in UTC-12,
  so they read at or before the real time everywhere; the exact UTC time
  stays in the extended field beside it (#2621).

- **An allocation refused because a parent account ran out trips the account
  in use too.** `aether_exec_account_tripped` marked only the ancestor whose
  allowance the allocation would pass, while `libaether.h` and
  `docs/emit-lib.md` say the account in use has
  `AETHER_ACCOUNT_TRIPPED_MEMORY`. A library that checks its own account
  before handing back what it built (RexxAE's create does) saw 0 under a
  host's session account, returned its handle, and the interrupted export's
  result was discarded with the handle in it: about 1.3 KB to 9 KB of the
  script's context leaked per refusal. Every account from the one in use up
  to the one that ran out is now marked; those above it are not.

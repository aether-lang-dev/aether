- **`seal except` and `hide` name a module as the source writes it.** A
  module sharing its last segment with another loaded one (`services.mq`
  beside `engines.mq`) is merged under its full path (`services_mq`), and a
  module imported `as engine` under its own namespace; the merge rewrites
  `mq.bump` and `engine.low` to those names. A seal admitted the module
  only under the internal name (`seal except mq` refused `mq.bump`, E0304
  'services_mq.bump'), and `hide mq` or `hide engine` hid none of its
  members. A list entry naming the written prefix now covers the module,
  for a call, a constant and a member taken as a value. E0304 names such a
  member as written, with its module (`'mq.bump' of module 'services.mq'`),
  and a selectively imported constant by its own name (`'K_SHAPE'`, was
  `'aevg_K_SHAPE'`).

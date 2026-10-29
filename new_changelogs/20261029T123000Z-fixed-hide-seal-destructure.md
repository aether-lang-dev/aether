- **Tuple destructuring now respects lexical containment.** A slot can no
  longer re-bind an outer name excluded by `hide` or `seal except`; visible
  locals and new locals inside a sealed block keep their existing behavior.

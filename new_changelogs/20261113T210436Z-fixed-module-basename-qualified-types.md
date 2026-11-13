- **Qualified types resolve when two modules share a last segment.** With
  `a.timing` and `b.timing` in one program, every `timing.T` was refused
  ("unknown module 'timing' in the type 'timing.Call'"), even in a file that
  imports only one of them: the check of a type's qualifier matched it
  against the module's namespace, which the collision had made `a_timing`,
  not against what the file writes. Qualified calls were fixed in #2268
  (nib-code ask module-basename-collision).

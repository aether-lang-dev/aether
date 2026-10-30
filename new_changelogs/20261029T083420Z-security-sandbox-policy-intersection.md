- **Embedded and preload sandbox policies now intersect.** An allowing
  in-process checker no longer bypasses a stricter shared-memory or file grant
  list; an operation must satisfy every active containment layer.

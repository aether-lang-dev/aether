- **Two modules whose paths end in the same segment no longer collide, and
  `import X as Y` works inside a library module.** `mine.vk` and
  `contrib.vulkan.vk` used to share the namespace `vk` program-wide, so
  whichever was imported first answered for both and the other's exports were
  reported missing; an alias written in a library was ignored and failed with
  `Undefined variable`. A module's imports now resolve in that module, by
  their full path or alias: when two loaded modules share a last segment each
  gets a namespace built from its full path (`mine_vk`, `contrib_vulkan_vk`),
  and the merger rewrites every `vk.`/alias prefix a program or library wrote
  to the module it meant. An alias-only import (`import m as x`) also merges
  `m`'s functions now, so a program-level alias reaches a local package
  (#2209).

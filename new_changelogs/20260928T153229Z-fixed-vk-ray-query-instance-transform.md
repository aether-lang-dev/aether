- **The ray-query test writes its instance's whole transform.** It set only
  the diagonal of the 3x4 matrix, and `vkAllocateMemory` does not clear
  memory: lavapipe hands back host memory that held earlier data, so the
  instance sometimes carried a stray translation and the ray through the
  origin missed. Every element is written now, and the test passes on every
  run at every llvmpipe thread count, where it had failed about half of them.

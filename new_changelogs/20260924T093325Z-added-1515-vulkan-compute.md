- **`contrib.vulkan` runs compute shaders (#1515).** `vulkan.compute_create`
  builds a compute pipeline from SPIR-V with storage buffers, uniforms,
  textures and push constants. `vulkan.dispatch` runs it and waits;
  `dispatch_async` / `compute_wait` let the CPU work in the meantime.
  Buffers (`vulkan.buffer_create`) are mapped, zeroed and readable by element
  (`buffer_float`, `buffer_int`).

  The same buffer binds to a graphics pipeline too (`bindings_storage`,
  `set_buffer`), and a pipeline made with an empty vertex layout takes no
  vertex input. So a compute pass can write vertices that a draw pulls by
  `gl_VertexIndex`, with no copy in between. A dispatch that is larger than
  the device allows, or that has a declared binding left unset, is refused
  with the binding or limit named. An unset binding is undefined behaviour,
  and a software rasteriser crashes on one.

  The test checks every element of a compute transform against the same
  computation on the CPU: floats to float precision, integers exactly. It
  also draws a triangle whose corners a compute pass placed.

/*
 * Stand-in for <mach/mach_vm.h> where the toolchain has none: zig's
 * bundled macOS headers (`ae build --target=*-macos`) leave this MIG
 * header out, though libSystem exports the calls. Vendored libfyaml's
 * fy-utils.c includes it for two of them, declared here exactly as the
 * macOS SDK declares them.
 *
 * Only std/yaml/aether_libfyaml_vendored.c's build reaches this directory,
 * through -idirafter, which searches it after the system headers: with a
 * real macOS SDK its own mach/mach_vm.h is found first and this file is
 * never read.
 */
#ifndef AETHER_LIBFYAML_COMPAT_MACH_VM_H
#define AETHER_LIBFYAML_COMPAT_MACH_VM_H

#include <mach/mach.h>

extern kern_return_t mach_vm_deallocate(vm_map_t target,
                                        mach_vm_address_t address,
                                        mach_vm_size_t size);

extern kern_return_t mach_vm_map(vm_map_t target_task,
                                 mach_vm_address_t *address,
                                 mach_vm_size_t size,
                                 mach_vm_offset_t mask,
                                 int flags,
                                 mem_entry_name_port_t object,
                                 memory_object_offset_t offset,
                                 boolean_t copy,
                                 vm_prot_t cur_protection,
                                 vm_prot_t max_protection,
                                 vm_inherit_t inheritance);

#endif

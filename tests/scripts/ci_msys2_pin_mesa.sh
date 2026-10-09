#!/bin/sh
# Pin MSYS2's MINGW64 Mesa to 26.1.2 when the installed build has the #2256
# free() bug. Run by the CI's Windows jobs that install Mesa (ci.yml: the
# MSYS2 suite and the contrib series), right after setup-msys2.
#
# #2256: Mesa 26.2.3's lavapipe frees every vkAllocateMemory block with
# the wrong free on Windows. llvmpipe_allocate_memory takes the block from
# os_malloc_aligned, which without posix_memalign returns a pointer INTO a
# larger malloc, and llvmpipe_free_memory handed that to plain free()
# (Mesa 2d794898097, first shipped in 26.2.3). ntdll detects the damaged
# heap and ends the process with STATUS_HEAP_CORRUPTION (0xC0000374),
# which bash reports as exit 127. So every Vulkan entry that allocates
# device memory dies at its first vkFreeMemory, after the specs before it
# passed, and vulkan/raw, which allocates none, passes. Linux is not
# affected: posix_memalign memory is free()-able. Mesa fixed it in
# 8ace865d958b ("llvmpipe: use os_free_aligned for non-vma case",
# 2026-09-17), nominated for 26.2.4.
#
# Only a Mesa in the broken range is replaced with 26.1.2, the last MSYS2
# build before it (same dependencies and DLL imports, libLLVM-22 included;
# pacman checks the package's .sig). Any other version runs as installed,
# so the first MSYS2 package carrying the fix runs the Vulkan entries
# unpinned and this script becomes a no-op to delete. Should 26.2.4 ship
# without the fix, widen the upper bound.
set -e
ver="$(pacman -Q mingw-w64-x86_64-mesa | awk '{print $2}')"
if [ "$(vercmp "$ver" 26.2.3)" -ge 0 ] && [ "$(vercmp "$ver" 26.2.4)" -lt 0 ]; then
  echo "Mesa $ver frees aligned device memory with free() (#2256); installing 26.1.2-1"
  # Try every mirror pacman itself would use, not repo.msys2.org
  # alone: a URL to `pacman -U` gets no mirror fallback, so that one
  # host being down (2026-10-03) failed the leg before it built.
  pkg=mingw-w64-x86_64-mesa-26.1.2-1-any.pkg.tar.zst
  for base in https://repo.msys2.org/mingw/mingw64 \
      $(sed -n 's/^[[:space:]]*Server[[:space:]]*=[[:space:]]*//p' /etc/pacman.d/mirrorlist.mingw \
        | sed 's/\$repo/mingw64/; s/\$arch/x86_64/; s#/*$##'); do
    pacman -U --noconfirm "$base/$pkg" && break
    echo "Mesa pin: $base did not serve $pkg; trying the next mirror"
  done
  pacman -Q mingw-w64-x86_64-mesa | grep -qx 'mingw-w64-x86_64-mesa 26.1.2-1' || {
    echo "Mesa pin did not take:"; pacman -Q mingw-w64-x86_64-mesa; exit 1; }
else
  echo "Mesa $ver is outside the #2256 range; running it as installed"
fi
pacman -Q mingw-w64-x86_64-mesa

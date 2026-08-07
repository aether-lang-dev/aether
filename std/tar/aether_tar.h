#ifndef AETHER_TAR_H
#define AETHER_TAR_H

#include <stdint.h>

// Set the modification time of a file, directory, or symlink.
// If is_symlink is true, prevent following the symlink if possible (using utimensat with AT_SYMLINK_NOFOLLOW).
// Returns 1 on success, 0 on failure.
int aether_tar_set_mtime_raw(const char* path, int64_t mtime, int is_symlink);

// Retrieve the file mode / permissions of a path.
// Returns the st_mode as an integer, or 0 on failure.
int aether_tar_get_mode_raw(const char* path);

// Slices a substring from a data pointer (which might be an AetherString) safely.
// Returns a freshly allocated AetherString.
char* aether_tar_slice_string(const char* data, int offset, int len);

// Join a decoded ustar prefix and name with '/'. Returns an AetherString.
void* aether_tar_join_name(const void* prefix, const void* name);

#endif // AETHER_TAR_H

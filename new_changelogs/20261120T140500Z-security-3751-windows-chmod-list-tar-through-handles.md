- **On Windows `fs.chmod`, the listings and `std.tar`'s times act on what
  their check saw (#3751).** Inside an enforce block `fs.chmod` changed the
  DACL and the read-only attribute by name (`SetNamedSecurityInfoW`,
  `SetFileAttributesW`), and `fs.list_dir`, `fs.walk` and `fs.glob` listed
  by name (`FindFirstFileA`), after the check: a junction swapped in
  between was followed out of the grant. `fs.chmod` now opens the file once,
  verifies its final path, and changes both through that handle
  (`SetSecurityInfo`, `SetFileInformationByHandle`); the listings read a
  verified directory handle (`NtQueryDirectoryFile`, a glob's pattern
  matched by the file system as `FindFirstFileA` matches it), each
  subdirectory opened relative to its parent with no link followed;
  `std.tar` sets times through a verified handle on Windows and with
  `utimensat` from the anchor on POSIX. Outside a block the listings are
  unchanged. The containment document now says exactly what holds: a file
  an open creates through a swapped-in junction exists outside the grant
  until the verification deletes it.

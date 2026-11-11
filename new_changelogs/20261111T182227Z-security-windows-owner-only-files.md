- **An owner-only mode is now owner-only on Windows.** `fs.chmod` there only
  toggled the read-only attribute, so a file a program made 0o600 (a key, a
  token) stayed as readable to other users as its folder let it be, and
  `fs.create_dir_with_mode` ignored its mode. A mode with no group or other
  bit (0o600, 0o700, 0o400) now gives the file or directory a protected DACL
  admitting only the current user, SYSTEM and Administrators (OpenSSH for
  Windows' rule for a private key), inherited by what is created in such a
  directory, and set as `create_dir_with_mode` creates it; a mode with a
  group or other bit puts back what the folder grants. The read-only toggle
  is unchanged. On a volume without ACLs an owner-only mode fails with
  `KIND_IO` instead of quietly leaving the file readable.

- **On Windows a checked call acts on the name a Win32 call would, and
  `NUL` is a device (#3751).** The calls that make a name through
  `NtCreateFile` or `NtSetInformationFile` (mkdir, rename, hard links,
  `fs.write_atomic`) took the last name as written, so "new." and "sp "
  were made literally and a file called "NUL" was made in a directory,
  none of which a Win32 call reaches again. The whole path now goes through
  `GetFullPathNameW` first, which drops a trailing dot or space and turns a
  device name into the device. A device ("\.\NUL") is its own resource,
  covered by a grant that names it or by `grant_all`, as `/dev/null` is on
  POSIX: `fs.write("NUL")` was refused even under `grant_all`. "sub \x" is
  also resolved one way by every check now.

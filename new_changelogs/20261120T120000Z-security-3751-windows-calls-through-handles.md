- **On Windows a sandboxed file call acts through a handle it verified
  first (#3751).** An open for writing through a junction swapped in after
  the check truncated the file it reached before the second check refused
  it. Opens now never create or truncate by name: the file is opened, the
  final path of its handle checked, and only then emptied (`SetEndOfFile`);
  a file the call created and then refused is deleted through its handle.
  Unlink, rmdir and remove set the disposition of a handle opened on the
  name itself; rename and hard links go through `NtSetInformationFile`
  with the verified destination directory as its root; mkdir, `mkdir -p`,
  `fs.write_atomic`'s temporary and `fs.make_temp_file`/`make_temp_dir`
  are made by `NtCreateFile` relative to a verified directory; `fs.copy`
  copies between two checked handles; the lookups, `fs.readlink`,
  `fs.realpath` and `fs.watch` verify the handle they use. A new symlink is
  verified after it is made, and removed again if a swap sent it elsewhere.

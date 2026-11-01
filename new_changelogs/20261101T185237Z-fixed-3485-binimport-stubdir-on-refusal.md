- **A refused binary import cleans up its stub directory.** On Windows,
  exiting while the stub file was still open prevented `atexit` directory
  cleanup from unlinking the file, leaving an `ae-binimport-*` temporary
  directory behind. Stub generation now returns a refusal code, closes the
  file, and removes the stub tree before exiting.

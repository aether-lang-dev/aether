- **`script_gateway.mount` refuses a script that carries a runtime of its
  own on Linux, FreeBSD and macOS too (#3501).** Since #3481 a script built
  the default way (`ae build --emit=lib`) keeps its own runtime on every
  platform, and on POSIX it was mounted unchecked: two runtimes, each with
  its own caps, config and panic frames, while the script filled objects
  the host's runtime frees. It is now refused at mount with `KIND_IO` and a
  message naming `--shared-runtime`, as Windows refuses it. A
  `--shared-runtime` script mounts on Linux and FreeBSD whatever the host
  was built with; on macOS the host is built with `--shared-runtime` too.
  The module's documentation now builds scripts with `--shared-runtime`.

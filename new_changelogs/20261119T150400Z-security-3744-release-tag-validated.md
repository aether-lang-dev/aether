- **`ae install`, `ae upgrade` and `ae version` refuse a release tag that is
  not a version (#3744).** The latest release's tag, read from GitHub's
  JSON, went unchecked into curl, wget, cp and rm command lines (PowerShell
  scripts on Windows), and `ae upgrade` is sometimes run as root; a tag
  holding shell syntax would have run. Every tag and version, whether from
  GitHub or from the command line (`ae version install`, `use` and
  `remove`), is now checked against the rule `ae add` already applies
  (letters, digits, `.`, `-`, `_` and `+`) before it reaches a URL, a path
  or a command line, and `ae version list` skips a tag that fails it. The
  downloaded JSON and the Windows download and extract scripts no longer sit
  at predictable names in the shared temp directory (`/tmp/ae_latest_<pid>.json`
  and the like), where another local user could make them first: they are
  made in a private directory of the process's own (mode 0700, a
  `mkdtemp` name), removed when `ae` exits. The release list is read from
  `AE_GITHUB_API_BASE` when it is set, as release assets already were.

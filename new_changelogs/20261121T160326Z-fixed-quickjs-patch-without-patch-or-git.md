- **The QuickJS engine's carried patches apply where neither `patch` nor
  `git` is installed.** `scripts/fetch-quickjs-amalgamation.sh` applied
  `contrib/quickjs/patches/*.patch` with `patch(1)`, or `git apply` without
  it, and stopped where there was neither: a bare MSYS2 shell, which is
  where the Windows jobs stage the release archive. It now falls back to
  `scripts/apply-unified-patch.awk`, which applies a plain unified diff
  exactly (a hunk fits at the line it names or the patch is refused and
  nothing changes). The three ways write the same bytes.

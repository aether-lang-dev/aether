#!/usr/bin/env python3
"""
Build the GitHub release body for a given Aether version.

Usage: python3 build_release_body.py <version>
  e.g. python3 build_release_body.py 0.13.0

Reads CHANGELOG.md from the repository root, extracts the section for the
given version, and writes /tmp/release_body.md containing:
  - Changelog notes for that release
  - Download table with direct filenames
  - Quick install instructions

Note: The release pipeline replaces `## [current]` with `## [<version>]` in
CHANGELOG.md before this script runs, so the version match should always
succeed.  As a fallback the script also tries the `[current]` section.
"""

import sys
import os
import re


def extract_changelog_section(changelog_path: str, version: str) -> str:
    """Return the body of the changelog section for `version` (without the header line)."""
    with open(changelog_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    header_pattern = re.compile(r"^## \[" + re.escape(version) + r"\]")
    next_section_pattern = re.compile(r"^## \[")

    in_section = False
    section_lines = []

    for line in lines:
        if header_pattern.match(line):
            in_section = True
            continue  # skip the header line itself
        if in_section:
            if next_section_pattern.match(line):
                break
            section_lines.append(line)

    return "".join(section_lines).strip()


def build_download_table(version: str) -> str:
    targets = [
        ("Linux",   "x86_64",  f"aether-{version}-linux-x86_64.tar.gz", None),
        ("macOS",   "x86_64",  f"aether-{version}-macos-x86_64.tar.gz", None),
        ("macOS",   "arm64",   f"aether-{version}-macos-arm64.tar.gz", None),
        ("Windows", "x86_64",  f"aether-{version}-windows-x86_64.zip", None),
        # FreeBSD is CROSS-built (no native runner) and best-effort, so it may
        # be absent from a given release. List it only when the artifact is
        # actually present in the publish working directory, and flag that it
        # was not run through the test suite on FreeBSD.
        ("FreeBSD", "x86_64",  f"aether-{version}-freebsd-x86_64.tar.gz",
         "cross-built, not tested on FreeBSD"),
    ]
    lines = [
        "| Platform | Architecture | File | Notes |",
        "|----------|:------------:|------|-------|",
    ]
    for platform, arch, filename, note in targets:
        # Optional (note-bearing) targets are only listed if the file exists.
        if note is not None and not os.path.exists(filename):
            continue
        lines.append(f"| {platform} | {arch} | `{filename}` | {note or ''} |")
    return "\n".join(lines)


def build_quickstart(version: str) -> str:
    return f"""\
**Linux / macOS**

```bash
# Download the archive for your platform, then:
tar -xzf aether-{version}-<platform>.tar.gz
export PATH="$PWD/bin:$PATH"
ae version
```

**Windows**

Extract the `.zip` archive, then run `bin\\ae.exe version` from the extracted folder.

---

Documentation: https://github.com/aether-lang-org/aether#readme"""


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: build_release_body.py <version>", file=sys.stderr)
        sys.exit(1)

    version = sys.argv[1]

    # CHANGELOG.md lives at the repository root; the workflow runs from there.
    repo_root = os.environ.get("GITHUB_WORKSPACE", os.getcwd())
    changelog_path = os.path.join(repo_root, "CHANGELOG.md")

    changelog_notes = ""
    if os.path.exists(changelog_path):
        changelog_notes = extract_changelog_section(changelog_path, version)
        if not changelog_notes:
            # No exact version match -- try [current] section as fallback
            # (the release pipeline should have already replaced [current] with
            # the version, but this handles edge cases like manual tag pushes)
            changelog_notes = extract_changelog_section(changelog_path, "current")
    else:
        print(f"CHANGELOG.md not found at {changelog_path}", file=sys.stderr)

    if not changelog_notes:
        changelog_notes = f"See [CHANGELOG.md](CHANGELOG.md) for details."

    body = f"""\
## What's new in {version}

{changelog_notes}

## Downloads

{build_download_table(version)}

## Quick install

{build_quickstart(version)}
"""

    output_path = "/tmp/release_body.md"
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(body)

    print(f"Release body written to {output_path}")


if __name__ == "__main__":
    main()

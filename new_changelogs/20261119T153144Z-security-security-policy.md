- **SECURITY.md says how to report a vulnerability privately** (GitHub's
  private reporting on the Security tab), which versions get fixes, what
  counts as a vulnerability, and how the project protects its CI and
  releases. The repository now requires SHA-pinned actions, scans for secrets
  with push protection, takes Dependabot security updates, and no longer lets
  a workflow approve a pull request (#3753, #3746).

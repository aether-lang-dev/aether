- **Shared-map host tests now exercise provisioned language runtimes.** The
  Lua, JavaScript, Python, Perl, and Ruby bridges now compile with the
  sandbox ABI enabled and receive their detected runtime library paths,
  instead of failing before their grant-map checks run.

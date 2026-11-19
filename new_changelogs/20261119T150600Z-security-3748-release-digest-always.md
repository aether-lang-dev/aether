- **`ae install`, `ae upgrade` and get.sh verify every release archive
  against its sha256 digest before unpacking it (#3748).** Both checked the
  digest GitHub's API lists for an asset only when the archive came through
  the API fallback; fetched from the release URL, the normal way, it was
  checked for an archive's magic and size and then unpacked. The digest is
  now looked up and checked on every path, and the archive is refused when it
  does not match. A digest that cannot be had (the API unreachable or
  rate-limited, or a listing with none) is a refusal too, with a message
  saying so, not a silent pass; there is no opt-out, since no offline
  install path needs one. `GITHUB_TOKEN` (or `GH_TOKEN`) raises the API's
  rate limit of 60 requests an hour. get.sh no longer falls back to a
  source build after a refusal, and finds a sha256 tool among `sha256sum`,
  `shasum`, FreeBSD's `sha256` and `openssl`.

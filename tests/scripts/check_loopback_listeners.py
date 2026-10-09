#!/usr/bin/env python3
"""What the suites run listens on loopback only (#2639).

The std listeners default to every interface (`http_server_create` binds
0.0.0.0, `tcp.listen` binds INADDR_ANY, `udp.bind("")` the wildcard), and
on Windows each NEW executable that listens there opens a Windows Defender
Firewall dialog and leaves two inbound block rules behind. One overnight
sweep left 56 dialogs (about 3.4 GB) and 112 rules, all for test binaries;
CI runners do not show it. Binding 127.0.0.1 does not prompt, and nothing
these suites start has a reason to be reachable from another machine.

This gate reads what the suites build and run, and fails on a listener that
does not name loopback:

  * tests/** (the .ae sweep, the C unit tests, the integration drivers and
    the fixtures they start, Aether embedded in the .sh drivers, Python);
  * std/**/test_*.ae (swept) and contrib/**/test_*.ae (`contrib-check`);
  * examples/**/*.ae (`make examples-run` starts each one);
  * the ```aether,run blocks `check_doc_blocks.py` runs.

The rules, each on code with comments stripped:

  * an HTTP server assigned from `server_create(` in a file that starts or
    binds one must get `127.0.0.1` through `set_host` / `server_bind` /
    `bind_raw` on that variable before it is reassigned;
  * `set_host` / `server_bind` / `bind_raw`, `tcp.listen_on`, `udp.bind`
    and the tinyweb / std.http.server.lb constructors that take a host
    must be given loopback: `"127.0.0.1"` (or `"::1"` where IPv6 is taken);
  * `tcp.listen` / `tcp_listen_raw` always take every interface: use
    `tcp.listen_on("127.0.0.1", port)`;
  * C: no `INADDR_ANY` / `in6addr_any`, and a file that calls bind() names
    a loopback address;
  * Python: a `bind((...))` / `create_server((...))` host and a
    `-m http.server` must name loopback.

A call that is refused before anything is bound (an invalid port or
address, asserted as a failure) says so with a `loopback-ok: <why>` comment
on its line or the line above. Run from the repository root; exit 1 listing
every offender.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

LOOPBACK = {"127.0.0.1", "::1"}
WAIVER = "loopback-ok:"
SELF = {"check_loopback_listeners.py", "test_check_loopback_listeners.py"}

# The first argument of a call: a string literal or anything up to , or ).
ARG = r'\s*\(\s*("(?:[^"\\\n]|\\.)*"|[^,)\n]*)'
HOST_SETTER = re.compile(r'\w*(?:set_host|server_bind|bind_raw)\s*\(\s*([A-Za-z_]\w*)\s*,\s*'
                         r'("(?:[^"\\\n]|\\.)*"|[^,)\n]*)')
HOST_FIRST = re.compile(r'(?<![\w.])(?:(?:\w+\.)?(?:udp_bind_raw|tcp_listen_on_raw|listen_on)'
                        r'|udp\.bind|(?:\w+\.)?(?:web_server_host|web_server_with_ws|web_server_full)'
                        r'|lb\.serve(?:_at)?)' + ARG)
ANY_LISTEN = re.compile(r'(?<![\w.])(?:(?:\w+\.)?tcp_listen_raw|tcp\.listen)\s*\(')
CREATE = re.compile(r'\b([A-Za-z_]\w*)\s*=\s*(?:\(\s*\w+\s*\*?\s*\)\s*)?[\w.]*server_create\s*\(')
# Any spelling: http.server_start, http_server_start_raw, http.server_bind_raw...
STARTS = re.compile(r'\w*(?:server_start|server_start_raw|server_start_background'
                    r'|server_start_background_raw|server_bind|server_bind_raw)\s*\(')
C_ANY = re.compile(r'\b(?:INADDR_ANY|in6addr_any)\b')
C_BIND = re.compile(r'(?<![\w.>])bind\s*\(')
C_LOOPBACK = re.compile(r'\bINADDR_LOOPBACK\b|\bin6addr_loopback\b|"127\.0\.0\.1"|"::1"')
PY_HOST = re.compile(r'\b(?:bind|create_server|HTTPServer|TCPServer|ThreadingHTTPServer)\s*\(\s*\(\s*'
                     r'([\'"])(.*?)\1')
PY_HTTP_SERVER = re.compile(r'-m\s+http\.server\b')
PY_HTTP_BIND = re.compile(r'(?:--bind|-b)\s+(?:127\.0\.0\.1|::1)\b')


def strip_line_comment(line):
    """Drop a `//` comment, leaving a `//` inside a string literal alone."""
    in_str = False
    for i, c in enumerate(line):
        if c == '"' and (i == 0 or line[i - 1] != '\\'):
            in_str = not in_str
        elif not in_str and line.startswith('//', i):
            return line[:i]
    return line


def code_lines(src, hash_comments):
    """(lineno, code, waived) per line: comment lines blanked, `//` tails cut.

    `waived` is a `loopback-ok:` marker on the line, or on a comment line
    right above it.
    """
    out = []
    in_block = False
    above = False   # the previous line is a comment carrying the marker
    for n, raw in enumerate(src.split('\n'), 1):
        s = raw.strip()
        waived = WAIVER in raw or above
        above = WAIVER in raw and s.startswith(('//', '/*', '*', '#'))
        if in_block:
            if '*/' in s:
                in_block = False
            out.append((n, "", waived))
            continue
        if s.startswith('/*'):
            in_block = '*/' not in s
            out.append((n, "", waived))
            continue
        # A comment, or an `extern` declaration (a parameter list, not a call).
        if (s.startswith('//') or s.startswith('*') or s.startswith('extern ')
                or (hash_comments and s.startswith('#'))):
            out.append((n, "", waived))
            continue
        out.append((n, strip_line_comment(raw), waived))
    return out


def literal(arg):
    """The text of a string literal argument, None for anything else."""
    arg = arg.strip()
    if len(arg) >= 2 and arg[0] == '"' and arg[-1] == '"':
        return arg[1:-1]
    return None


def calls(regex, code):
    """Matches of `regex` that start outside a string literal: a message such
    as "FAIL: tcp_listen_on_raw(127.0.0.1, 0)" names a call, it is not one."""
    for m in regex.finditer(code):
        quotes = 0
        i = 0
        while i < m.start():
            if code[i] == '\\':
                i += 2
                continue
            if code[i] == '"':
                quotes += 1
            i += 1
        if quotes % 2 == 0:
            yield m


def check_source(src, kind):
    """Offences in one source as (lineno, message). `kind`: ae, c, sh, py."""
    lines = code_lines(src, hash_comments=kind in ("sh", "py"))
    found = []

    def flag(n, waived, msg):
        if not waived:
            found.append((n, msg))

    if kind in ("ae", "c", "sh"):
        # A start or bind marked `loopback-ok:` is one this gate was told about.
        starts = any(any(calls(STARTS, code)) and not waived for _, code, waived in lines)
        pending = {}   # server variable -> line it was created on
        for n, code, waived in lines:
            for m in calls(HOST_SETTER, code):
                var, host = m.group(1), literal(m.group(2))
                if host in LOOPBACK or waived:
                    pending.pop(var, None)
                if host not in LOOPBACK:
                    flag(n, waived, "binds %s, not loopback: pass \"127.0.0.1\"" % m.group(2).strip())
            for m in calls(HOST_FIRST, code):
                if literal(m.group(1)) not in LOOPBACK:
                    flag(n, waived, "listens on %s, not loopback: pass \"127.0.0.1\""
                         % (m.group(1).strip() or "a default host"))
            if any(calls(ANY_LISTEN, code)):
                flag(n, waived, "listens on every interface: use tcp.listen_on(\"127.0.0.1\", port)"
                     " / tcp_listen_on_raw(\"127.0.0.1\", port)")
            for m in calls(CREATE, code):
                var = m.group(1)
                if starts and var in pending:
                    flag(*pending[var])
                pending[var] = (n, waived, "HTTP server `%s` listens on the default host (every "
                                "interface): http.server_set_host(%s, \"127.0.0.1\")" % (var, var))
        if starts:
            for n, waived, msg in pending.values():
                flag(n, waived, msg)

    if kind == "c":
        binds = False
        loopback = False
        for n, code, waived in lines:
            if C_ANY.search(code):
                flag(n, waived, "binds INADDR_ANY / in6addr_any: bind INADDR_LOOPBACK")
            if C_BIND.search(code):
                binds = binds or not waived
            if C_LOOPBACK.search(code):
                loopback = True
        if binds and not loopback:
            found.append((0, "calls bind() and never names a loopback address"))

    if kind in ("py", "sh"):
        for n, code, waived in lines:
            for m in PY_HOST.finditer(code):
                if m.group(2) not in LOOPBACK and m.group(2) != "localhost":
                    flag(n, waived, "Python listener on %r, not loopback" % m.group(2))
            if PY_HTTP_SERVER.search(code) and not PY_HTTP_BIND.search(code):
                flag(n, waived, "python -m http.server without --bind 127.0.0.1")

    return sorted(found)


KINDS = {".ae": "ae", ".c": "c", ".h": "c", ".sh": "sh", ".py": "py"}


def swept_sources(root):
    """Every file the suites build or run that can start a listener."""
    out = []
    for dirpath, _, names in os.walk(os.path.join(root, "tests")):
        for n in names:
            ext = os.path.splitext(n)[1]
            if ext in KINDS and n not in SELF:
                out.append(os.path.join(dirpath, n))
    for sub in ("std", "contrib"):
        for dirpath, _, names in os.walk(os.path.join(root, sub)):
            for n in names:
                if n.startswith("test_") and n.endswith(".ae"):
                    out.append(os.path.join(dirpath, n))
    for dirpath, _, names in os.walk(os.path.join(root, "examples")):
        for n in names:
            if n.endswith(".ae"):
                out.append(os.path.join(dirpath, n))
    return sorted(out)


def run_blocks(root):
    """(path, first line, code) of every ```aether,run block the doc check runs."""
    import check_doc_blocks
    for path in check_doc_blocks.doc_files(root):
        for line, label, code, _expected in check_doc_blocks.blocks_in(path):
            if label == "run":
                yield path, line, code


def main():
    offences = []
    for path in swept_sources(ROOT):
        with open(path, encoding="utf-8", errors="replace") as f:
            src = f.read()
        rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
        for n, msg in check_source(src, KINDS[os.path.splitext(path)[1]]):
            offences.append("%s:%d: %s" % (rel, n, msg))
    for path, line, code in run_blocks(ROOT):
        rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
        for n, msg in check_source(code, "ae"):
            offences.append("%s:%d: %s" % (rel, line + n, msg))
    if offences:
        print("loopback listeners: %d listener(s) the suites start are not on loopback (#2639):"
              % len(offences))
        for o in offences:
            print("  " + o)
        print("  on Windows each new executable listening on every interface opens a firewall")
        print("  prompt; bind 127.0.0.1, or mark a call refused before any bind `loopback-ok: <why>`.")
        return 1
    print("loopback listeners: every listener the suites start binds loopback")
    return 0


if __name__ == "__main__":
    sys.exit(main())

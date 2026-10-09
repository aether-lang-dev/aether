#!/usr/bin/env python3
"""Unit tests for check_loopback_listeners.py (#2639): each rule on a small
source, flagged and clean, so a rule change here is a red test rather than a
green `make check-tests` that quietly stopped seeing a listener.

Run from anywhere: `python3 tests/scripts/test_check_loopback_listeners.py`.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_loopback_listeners as cll  # noqa: E402


def lines(src, kind="ae"):
    return [n for n, _ in cll.check_source(src, kind)]


class HttpServerTest(unittest.TestCase):
    def test_started_without_a_host_is_flagged(self):
        src = 'main() {\n    raw = http.server_create(8080)\n    http.server_start(raw)\n}\n'
        self.assertEqual(lines(src), [2])

    def test_loopback_host_is_clean(self):
        src = ('main() {\n    raw = http.server_create(0)\n'
               '    http.server_set_host(raw, "127.0.0.1")\n    http.server_start(raw)\n}\n')
        self.assertEqual(lines(src), [])

    def test_bind_on_loopback_is_clean(self):
        src = ('main() {\n    s = http.server_create(0)\n'
               '    if http_server_bind_raw(s, "127.0.0.1", 0) < 0 { exit(1) }\n'
               '    http_server_start_raw(s)\n}\n')
        self.assertEqual(lines(src), [])

    def test_started_in_an_actor_under_another_name_is_still_flagged(self):
        src = ('actor Srv { receive { Go(raw) -> { http.server_start(raw) } } }\n'
               'main() {\n    srv = http.server_create(18101)\n    a = spawn(Srv())\n'
               '    a ! Go { raw: srv }\n}\n')
        self.assertEqual(lines(src), [3])

    def test_created_and_never_started_is_clean(self):
        src = 'main() {\n    srv = http.server_create(0)\n    http.server_free(srv)\n}\n'
        self.assertEqual(lines(src), [])

    def test_every_interface_host_is_flagged(self):
        src = ('main() {\n    s = http.server_create(0)\n'
               '    http.server_set_host(s, "0.0.0.0")\n    http.server_start(s)\n}\n')
        self.assertEqual(lines(src), [2, 3])

    def test_a_reused_variable_needs_a_host_each_time(self):
        src = ('main() {\n    s = http.server_create(1)\n    http.server_set_host(s, "127.0.0.1")\n'
               '    http.server_start_background(s)\n    s = http.server_create(2)\n'
               '    http.server_start_background(s)\n}\n')
        self.assertEqual(lines(src), [5])

    def test_c_server_is_checked_too(self):
        src = ('void f(void) {\n    HttpServer* s = http_server_create(0);\n'
               '    http_server_start_raw(s);\n}\n')
        self.assertEqual(lines(src, "c"), [2])


class TcpUdpTest(unittest.TestCase):
    def test_tcp_listen_is_flagged(self):
        self.assertEqual(lines('main() {\n    s, e = tcp.listen(8080)\n}\n'), [2])
        self.assertEqual(lines('main() {\n    s = tcp_listen_raw(8080)\n}\n'), [2])

    def test_tcp_listen_on_loopback_is_clean(self):
        self.assertEqual(lines('main() {\n    s, e = tcp.listen_on("127.0.0.1", 0)\n}\n'), [])

    def test_tcp_listen_on_every_interface_is_flagged(self):
        self.assertEqual(lines('main() {\n    s, e = tcp.listen_on("0.0.0.0", 0)\n}\n'), [2])

    def test_udp_hosts(self):
        self.assertEqual(lines('main() {\n    s, e = udp.bind("", 0)\n}\n'), [2])
        self.assertEqual(lines('main() {\n    s, e = udp.bind(host, 0)\n}\n'), [2])
        self.assertEqual(lines('main() {\n    s, e = udp.bind("::1", 0)\n}\n'), [])

    def test_extern_declaration_is_not_a_call(self):
        self.assertEqual(lines('extern tcp_listen_raw(port: int) -> ptr\n'), [])
        self.assertEqual(lines('extern udp_bind_raw(host: string, port: int) -> ptr\n'), [])

    def test_waiver_on_the_line_or_a_comment_above(self):
        self.assertEqual(lines('    s = tcp_listen_raw(-1)  // loopback-ok: refused\n'), [])
        self.assertEqual(lines('    // loopback-ok: refused before any bind\n'
                               '    s = tcp_listen_raw(-1)\n'), [])

    def test_waiver_does_not_reach_past_one_line(self):
        src = ('    a = udp.bind("bad host", 0)  // loopback-ok: refused\n'
               '    b, e = udp.bind("", 0)\n')
        self.assertEqual(lines(src), [2])

    def test_comments_are_not_calls(self):
        self.assertEqual(lines('// server, err = tcp.listen(8080)\n'), [])

    def test_a_call_named_in_a_string_is_not_a_call(self):
        self.assertEqual(lines('    println("FAIL: tcp.listen(8080) returned null")\n'), [])
        self.assertEqual(lines('    println("bad \\"x\\" tcp_listen_on_raw(0.0.0.0, 0)")\n'), [])
        self.assertEqual(lines('    println("x"); s, e = tcp.listen(1)\n'), [1])


class ConstructorTest(unittest.TestCase):
    def test_tinyweb_and_lb_hosts(self):
        self.assertEqual(lines('    s = tinyweb.web_server_host("localhost", 8080) {\n'), [1])
        self.assertEqual(lines('    s = tinyweb.web_server_with_ws("127.0.0.1", 1, 2) {\n'), [])
        self.assertEqual(lines('    e = lb.serve("0.0.0.0", 9000, "rr", "", h, b, c)\n'), [1])
        self.assertEqual(lines('    e = lb.serve("127.0.0.1", 0, "rr", "", h, b, c)\n'), [])


class CSocketTest(unittest.TestCase):
    def test_inaddr_any_is_flagged(self):
        src = ('int f(void) {\n    a.sin_addr.s_addr = INADDR_ANY;\n'
               '    return bind(fd, (struct sockaddr*)&a, sizeof(a));\n}\n')
        self.assertIn(2, lines(src, "c"))

    def test_bind_without_loopback_is_flagged(self):
        src = 'int f(void) {\n    return bind(fd, (struct sockaddr*)&a, sizeof(a));\n}\n'
        self.assertEqual(lines(src, "c"), [0])

    def test_loopback_bind_is_clean(self):
        src = ('int f(void) {\n    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);\n'
               '    return bind(fd, (struct sockaddr*)&a, sizeof(a));\n}\n')
        self.assertEqual(lines(src, "c"), [])


class PythonTest(unittest.TestCase):
    def test_python_listeners(self):
        self.assertEqual(lines('s.bind(("", 0))\n', "py"), [1])
        self.assertEqual(lines('s.bind(("127.0.0.1", 0))\n', "py"), [])
        self.assertEqual(lines('python3 -m http.server 8000 &\n', "sh"), [1])
        self.assertEqual(lines('python3 -m http.server "$PORT" --bind 127.0.0.1 &\n', "sh"), [])


if __name__ == "__main__":
    unittest.main()

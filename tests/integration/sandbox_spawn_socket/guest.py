"""sandbox.spawn_socket's Python guest: stock python3, standard library only,
started with grant_runtime("python3") and a read grant on this file. It
reports on the socket at AETHER_IPC_FD what it could do:

  started    the interpreter came up under the kernel layer
  open       open('/etc/passwd') through libc (the preload's to refuse)
  raw        openat('/etc/passwd') through libc's own syscall(), by ctypes:
             past the preload, so only Landlock can refuse it
  connect    a TCP connect to 127.0.0.1:9 (seccomp refuses the socket
             without a tcp grant)
"""
import ctypes
import os
import platform
import socket
import sys

ch = socket.socket(fileno=int(os.environ["AETHER_IPC_FD"]))


def say(line):
    ch.sendall((line + "\n").encode())


def probe_open():
    try:
        with open("/etc/passwd") as f:
            f.read(1)
        return "ALLOWED"
    except OSError as e:
        return "refused (%s)" % e.strerror


def real_libc():
    # libc by its path: the preload refuses dlopen("libc.so.6") by name,
    # which is exactly the escape a guest would try next.
    for p in ("/usr/lib/libc.so.6", "/lib/x86_64-linux-gnu/libc.so.6",
              "/lib/aarch64-linux-gnu/libc.so.6", "/usr/lib64/libc.so.6",
              "/lib64/libc.so.6"):
        if os.path.exists(p):
            return ctypes.CDLL(p, use_errno=True)
    return None


def probe_raw():
    nr = {"x86_64": 257, "aarch64": 56}.get(platform.machine())
    libc = real_libc()
    if nr is None or libc is None:
        return "skipped (no libc path or syscall number here)"
    fd = libc.syscall(nr, -100, b"/etc/passwd", os.O_RDONLY)
    if fd >= 0:
        os.close(fd)
        return "ALLOWED"
    return "refused (%s)" % os.strerror(ctypes.get_errno())


def probe_connect():
    try:
        socket.create_connection(("127.0.0.1", 9), timeout=2).close()
        return "ALLOWED"
    except ConnectionRefusedError:
        return "ALLOWED (connection refused by the peer)"
    except OSError as e:
        return "refused (%s)" % (e.strerror or e)


say("started python %s" % sys.version.split()[0])
say("open: " + probe_open())
say("raw: " + probe_raw())
say("connect: " + probe_connect())

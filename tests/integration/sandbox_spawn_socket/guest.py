"""sandbox.spawn_socket's Python guest: stock python3, standard library only,
started with grant_runtime("python3") and a read grant on this file. It
reports on the socket at AETHER_IPC_FD what it could do:

  started    the interpreter came up under the kernel layer
  open       open('/etc/passwd') through libc (the preload's to refuse)
  dlopen     ctypes.CDLL of libc by its absolute path: the preload refuses
             a handle on libc however it is named
  raw        openat('/etc/passwd') through libc's own syscall(), its address
             read from libc's symbol table in memory and called by ctypes: past
             the preload, so only Landlock can refuse it
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


class DlInfo(ctypes.Structure):
    _fields_ = [("dli_fname", ctypes.c_char_p), ("dli_fbase", ctypes.c_void_p),
                ("dli_sname", ctypes.c_char_p), ("dli_saddr", ctypes.c_void_p)]


def libc_file():
    """libc's path and load address, from dladdr on its strlen (CDLL(None)
    is the program's own handle, which the preload lets through)."""
    me = ctypes.CDLL(None)
    info = DlInfo()
    addr = ctypes.cast(me.strlen, ctypes.c_void_p)
    if not me.dladdr(addr, ctypes.byref(info)) or not info.dli_fname:
        return None, None
    return info.dli_fname.decode(), info.dli_fbase


def probe_dlopen():
    path, _ = libc_file()
    if path is None or not path.startswith("/"):
        return "skipped (libc's path not found)"
    try:
        ctypes.CDLL(path)
        return "ALLOWED (%s)" % path
    except OSError:
        return "refused (%s)" % path


def symbol_offset(base, name):
    """The value of `name` in the dynamic symbol table of the ELF64 object
    loaded at `base`, read from memory (the guest may not read libc's file):
    the program headers lead to PT_DYNAMIC, and that to DT_SYMTAB and
    DT_STRTAB, which glibc lays out with the strings straight after the
    symbols."""
    def u16(a): return ctypes.c_uint16.from_address(a).value
    def u32(a): return ctypes.c_uint32.from_address(a).value
    def u64(a): return ctypes.c_uint64.from_address(a).value

    if ctypes.string_at(base, 4) != b"\x7fELF":
        return None
    phoff, phentsize, phnum = u64(base + 0x20), u16(base + 0x36), u16(base + 0x38)
    dyn = None
    for i in range(phnum):
        ph = base + phoff + i * phentsize
        if u32(ph) == 2:   # PT_DYNAMIC
            dyn = base + u64(ph + 0x10)
    if dyn is None:
        return None
    tags = {}
    while u64(dyn) != 0:
        tags[u64(dyn)] = u64(dyn + 8)
        dyn += 16
    # Relocated in place by the loader on most targets, not on some: an
    # address below the base is still an offset.
    def addr(v): return v if v >= base else base + v
    if 5 not in tags or 6 not in tags:
        return None
    strtab, symtab = addr(tags[5]), addr(tags[6])
    want = name.encode()
    for at in range(symtab, strtab, 24):
        st_name, shndx, value = u32(at), u16(at + 6), u64(at + 8)
        if value and shndx and ctypes.string_at(strtab + st_name, len(want) + 1) == want + b"\0":
            return value
    return None


def probe_raw():
    nr = {"x86_64": 257, "aarch64": 56}.get(platform.machine())
    path, base = libc_file()
    if nr is None or path is None or not base:
        return "skipped (no libc or syscall number here)"
    off = symbol_offset(base, "syscall")
    if off is None:
        return "skipped (no syscall symbol in %s)" % path
    raw = ctypes.CFUNCTYPE(ctypes.c_long, ctypes.c_long, ctypes.c_long,
                           ctypes.c_char_p, ctypes.c_long, use_errno=True)(base + off)
    fd = raw(nr, -100, b"/etc/passwd", os.O_RDONLY)
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
say("dlopen: " + probe_dlopen())
say("raw: " + probe_raw())
say("connect: " + probe_connect())

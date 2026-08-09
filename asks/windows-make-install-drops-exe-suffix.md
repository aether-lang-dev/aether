# Windows: `make install` installs `ae`/`aetherc` without `.exe`, so `ae` can never find its compiler

**From:** the aether-ui line (2026-08-08) · **Where it bit:** winbaz
(Windows 11 / MSYS2 MINGW64). A freshly built, successfully installed
toolchain cannot compile anything, and the whole aether-ui spec matrix comes
back red.

## Symptom

`make install` reports success:

```
===================================
Installing Aether to /usr/local
===================================
✓ Installed successfully
```

`ae --version` works. Any actual compile does not:

```
$ ae run hello.ae
Error: Aether compiler not found.

If you downloaded a release ZIP, make sure to:
  1. Extract the ZIP (e.g. to C:\aether)
  ...
Or set AETHER_HOME to the extraction folder:
  set AETHER_HOME=C:\aether
```

The AETHER_HOME advice is a red herring — setting it changes nothing, because
the problem is a filename, not a search root.

## Cause

`Makefile` lines 1699–1700 read the built artifacts **with** the extension and
install them **without** it:

```make
@install -m 755 build/ae$(EXE_EXT)               $(PREFIX)/bin/ae
@install -m 755 build/aetherc-release$(EXE_EXT)  $(PREFIX)/bin/aetherc
```

So on MINGW the install directory ends up holding:

```
-rwxr-xr-x  585744 ae          <- no .exe
-rwxr-xr-x 1535476 aetherc     <- no .exe
```

But `tools/ae.c` looks for the compiler **with** `EXE_EXT`, which is `".exe"`
on Windows (`tools/ae.c:30`):

```c
snprintf(tc.compiler, sizeof(tc.compiler), "%s/aetherc" EXE_EXT, exe_dir);
if (path_exists(tc.compiler)) goto found_root;
```

Every sibling-of-`ae` strategy therefore misses. The one fallback that would
match, Strategy 5, hardcodes POSIX paths with no extension:

```c
const char* standard_paths[] = {
    "/usr/local/bin/aetherc",
    "/usr/bin/aetherc",
    NULL
};
```

`/usr/local/bin` is an MSYS mount, not a native Windows path, so `path_exists`
does not resolve it from a native-Windows binary either. Result: `ae` is
installed next to `aetherc` and still cannot find it.

## Why it is worse than it looks

`ae --version` keeps working throughout, because that path never needs the
compiler. So the box reports a healthy, correctly-versioned toolchain while
every compile fails. On this line it presented as **57 spec suites red with
0 pass / 0 fail each** — every suite dying at launch — which looks like a
harness or environment failure, not a packaging one.

It also invites exactly the wrong repair. Copying the extensionless binaries
to `.exe` names appears to fix it, and then *blocks the next real install*:

```
install: cannot create regular file '/usr/local/bin/ae': File exists
make: *** [Makefile:1699: install] Error 1
```

— which itself reports through `make` as a failure that is easy to miss if
the output is piped.

## Fix

Install with the extension, matching what `ae.c` looks for:

```make
@install -m 755 build/ae$(EXE_EXT)               $(PREFIX)/bin/ae$(EXE_EXT)
@install -m 755 build/aetherc-release$(EXE_EXT)  $(PREFIX)/bin/aetherc$(EXE_EXT)
```

Worth also making Strategy 5 in `tools/ae.c` append `EXE_EXT`, so a
hand-placed install still resolves.

A smoke check that would have caught it: the install target already runs
`ae version` (Makefile ~1043) as verification. `ae version` does not need
the compiler. Running `ae run` on a trivial file instead — or `ae build` —
would have failed loudly at install time on Windows.

## Not the same as

`aeb`'s Windows fan-out bug (`aetherc` invoked with no arguments) — that is
downstream of this and separate.

#!/bin/bash
# Detect available host language dev packages and output compile flags.
# Source this file: . tests/sandbox/detect_host_langs.sh

# Lua
LUA_AVAILABLE=0
for v in lua5.4 lua5.3 lua5.2; do
    if pkg-config --exists $v 2>/dev/null; then
        LUA_CFLAGS=$(pkg-config --cflags $v)
        LUA_LIBS=$(pkg-config --libs $v)
        LUA_LIBNAME=$(pkg-config --libs-only-l $v | sed -n 's/.*-l\([^ ]*\).*/lib\1.so/p')
        AETHER_LUA_SONAME=$(ldconfig -p 2>/dev/null | awk -v lib="$LUA_LIBNAME" '$1 == lib { print $NF; exit }')
        export AETHER_LUA_SONAME
        LUA_CMD=$(echo $v | tr -d '.')  # lua54, lua53, etc — some systems use this
        # Try common command names
        for cmd in $v lua; do
            if command -v $cmd >/dev/null 2>&1; then LUA_CMD=$cmd; break; fi
        done
        LUA_AVAILABLE=1
        break
    fi
done

# Python
PY_AVAILABLE=0
for v in python3 python3.12 python3.11 python3.10; do
    cfg="${v}-config"
    if command -v $cfg >/dev/null 2>&1; then
        PY_CFLAGS=$($cfg --includes 2>/dev/null)
        PY_LIBS="-l$(echo $v | tr -d .)"  # -lpython311 etc doesn't work, try embed
        PY_LIBS=$($cfg --ldflags --embed 2>/dev/null || $cfg --ldflags 2>/dev/null)
        AETHER_PYTHON_SONAME=$($v -c 'import os, sysconfig; print(os.path.join(sysconfig.get_config_var("LIBDIR") or "", sysconfig.get_config_var("INSTSONAME") or ""))' 2>/dev/null)
        export AETHER_PYTHON_SONAME
        PY_AVAILABLE=1
        break
    fi
done
# Fallback: try pkg-config
if [ $PY_AVAILABLE -eq 0 ]; then
    for v in python-3.12-embed python-3.11-embed python-3.10-embed python3-embed; do
        if pkg-config --exists $v 2>/dev/null; then
            PY_CFLAGS=$(pkg-config --cflags $v)
            PY_LIBS=$(pkg-config --libs $v)
            AETHER_PYTHON_SONAME=$(python3 -c 'import os, sysconfig; print(os.path.join(sysconfig.get_config_var("LIBDIR") or "", sysconfig.get_config_var("INSTSONAME") or ""))' 2>/dev/null)
            export AETHER_PYTHON_SONAME
            PY_AVAILABLE=1
            break
        fi
    done
fi

# Perl
PERL_AVAILABLE=0
if perl -MExtUtils::Embed -e ccopts >/dev/null 2>&1; then
    PERL_CFLAGS=$(perl -MExtUtils::Embed -e ccopts 2>/dev/null)
    # Find libperl — may need versioned .so
    PERL_LIBS=""
    PERL_LIBDIR=$(perl -MConfig -e 'print $Config{libperl}' 2>/dev/null)
    if [ -n "$PERL_LIBDIR" ]; then
        PERL_SO=$(find /usr/lib -name "$PERL_LIBDIR" 2>/dev/null | head -1)
        if [ -n "$PERL_SO" ]; then
            PERL_LIBS="$PERL_SO -ldl -lm -lpthread -lcrypt"
            AETHER_PERL_SONAME="$PERL_SO"
            export AETHER_PERL_SONAME
        fi
    fi
    # Fallback: try -lperl
    if [ -z "$PERL_LIBS" ]; then
        PERL_LIBS="-lperl -ldl -lm -lpthread -lcrypt"
    fi
    PERL_AVAILABLE=1
fi

# Ruby
RUBY_AVAILABLE=0
for v in ruby-3.3 ruby-3.2 ruby-3.1 ruby-3.0 ruby; do
    if pkg-config --exists $v 2>/dev/null; then
        RUBY_CFLAGS=$(pkg-config --cflags $v)
        RUBY_LIBS=$(pkg-config --libs $v)
        if command -v ruby >/dev/null 2>&1; then
            AETHER_RUBY_SONAME=$(ruby -rrbconfig -e 'print File.join(RbConfig::CONFIG["libdir"], RbConfig::CONFIG["LIBRUBY_SO"])' 2>/dev/null)
            export AETHER_RUBY_SONAME
        fi
        RUBY_AVAILABLE=1
        break
    fi
done

# Duktape (JS)
JS_AVAILABLE=0
if [ -f /usr/include/duktape.h ]; then
    JS_CFLAGS=""
    JS_LIBS="-lduktape"
    AETHER_DUKTAPE_SONAME=$(ldconfig -p 2>/dev/null | awk '/libduktape\.so/ { print $NF; exit }')
    export AETHER_DUKTAPE_SONAME
    JS_AVAILABLE=1
fi

# Java
JAVA_AVAILABLE=0
if command -v java >/dev/null 2>&1 && command -v javac >/dev/null 2>&1; then
    JAVA_AVAILABLE=1
fi

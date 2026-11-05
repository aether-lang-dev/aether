# Aether builder image — compile .ae source to Linux x86_64 binaries
# inside the container, extract the binary on completion.
#
# Two stages:
#   1. builder — has gcc + make + git + the Aether stdlib's -dev
#                dependencies. Builds and installs Aether from /src
#                (the build context root). Throw-away after stage 2
#                pulls /usr/local + /opt/aether out.
#   2. runtime — debian:bookworm-slim + gcc + the runtime -dev packages
#                Aether-emitted C links against. Has /usr/local
#                populated from stage 1. With no WITH_* toggles, ~314 MB.
#
# Build-time toggles (--build-arg WITH_<LANG>=1):
#   WITH_JAVA       — build aether-sandbox.jar; ship JAR, drop JDK
#   WITH_PERL       — capture <EXTERN.h>; drop libperl-dev
#   WITH_RUBY       — capture <ruby.h>; drop ruby-dev
#   WITH_JS         — capture <duktape.h>; drop duktape-dev
#
# Strategy A (per docs): install the dev package, capture exactly what
# the host-bridge .c needs (header file or JAR), purge the rest. Image
# growth per toggle is in the low MB; the .h/.jar artifacts ride in
# /opt/aether/ (headers) and /usr/local/share/aether/contrib/host/
# (Java JAR).
#
# Usage:
#   docker build -t aether-builder:slim -f tools/docker/Dockerfile .
#
#   docker build -t aether-builder:polyglot \
#       --build-arg WITH_JAVA=1 \
#       --build-arg WITH_RUBY=1 \
#       --build-arg WITH_PERL=1 --build-arg WITH_JS=1 \
#       -f tools/docker/Dockerfile .
#
# Run-time usage (rootless podman example):
#   podman run --rm --userns=keep-id \
#       -v "$PWD:/work:ro" \
#       -v "$PWD/out:/out" \
#       aether-builder:slim \
#       myprog.ae
#
# Reasonable for: a Bazzite / Silverblue / CoreOS / similar immutable-
# host workflow where the user has Aether source on disk but no
# toolchain installed. The container IS the toolchain.

# =============================================================
# Stage 1 — builder
# =============================================================
FROM debian:bookworm AS builder

ARG WITH_JAVA=0
ARG WITH_PERL=0
ARG WITH_RUBY=0
ARG WITH_JS=0

# Base toolchain + Aether's own -dev dependencies (always-on).
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make ca-certificates git pkg-config \
        libssl-dev zlib1g-dev libnghttp2-dev libpcre2-dev \
        libexpat1-dev libsqlite3-dev \
 && rm -rf /var/lib/apt/lists/*

# Build & install Aether from the local source tree (the build
# context root). Aether's `make install` copies the stdlib + contrib
# source layout (with host-bridge .c files preserved per the
# carve-out at Makefile:1257-1261) into /usr/local/share/aether/.
COPY . /src
WORKDIR /src
RUN make && make install

# Drop scripts into the image. Per-language install scripts now
# just `cp` from the pinned hosted-language-headers checkout (see
# clone-host-headers.sh) into /opt/aether/include/<lang>/.
COPY tools/docker/scripts/ /usr/local/lib/docker-build-scripts/
RUN chmod +x /usr/local/lib/docker-build-scripts/*.sh

# Clone the pinned hosted-language-headers repo ONCE if any
# host-language toggle is on. Skipped entirely for the slim
# (no-toggle) build path. Header pin (commit SHA) and source repo
# URL are env-vars inside the script — bump there to upgrade.
RUN if [ "$WITH_PERL$WITH_RUBY$WITH_JS" != "000" ]; then \
        /usr/local/lib/docker-build-scripts/clone-host-headers.sh; \
    fi

# Per-language conditional installs. One RUN per language so each
# is its own layer — readable, individually skippable when WITH_X=0.
# Java's a special case (JAR build, not C-bridge) and doesn't read
# the hosted-language-headers repo.
RUN if [ "$WITH_JAVA"   = "1" ]; then /usr/local/lib/docker-build-scripts/install-java.sh;            fi
RUN if [ "$WITH_PERL"   = "1" ]; then /usr/local/lib/docker-build-scripts/install-perl-headers.sh;    fi
RUN if [ "$WITH_RUBY"   = "1" ]; then /usr/local/lib/docker-build-scripts/install-ruby-headers.sh;    fi
RUN if [ "$WITH_JS"     = "1" ]; then /usr/local/lib/docker-build-scripts/install-js-headers.sh;      fi

# Ensure /opt/aether exists even when no host-language toggle ran,
# so stage 2's unconditional COPY --from=builder /opt/aether ... has
# something to copy (Docker rejects COPY of a non-existent source).
RUN mkdir -p /opt/aether

# =============================================================
# Stage 2 — runtime
# =============================================================
FROM debian:bookworm-slim

# ARGs don't cross stage boundaries — re-declare them here so the
# stage-2 RUN blocks can branch on them too. Caller's --build-arg
# values flow through.
ARG WITH_JAVA=0
ARG WITH_PERL=0
ARG WITH_RUBY=0
ARG WITH_JS=0

# Runtime image: gcc + libc6-dev (for gcc's standard headers) + the
# -dev packages Aether-emitted C wants at link time. NOT
# build-essential — we don't need g++ or dpkg-dev.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev make \
        libssl-dev zlib1g-dev libnghttp2-dev libpcre2-dev \
        libexpat1-dev libsqlite3-dev \
 && rm -rf /var/lib/apt/lists/*

# Pull the installed Aether toolchain + (any captured) host-bridge
# headers from the builder stage.
COPY --from=builder /usr/local/bin/      /usr/local/bin/
COPY --from=builder /usr/local/lib/      /usr/local/lib/
COPY --from=builder /usr/local/include/  /usr/local/include/
COPY --from=builder /usr/local/share/    /usr/local/share/
# Host-bridge headers (one dir per WITH_* the builder enabled, plus
# the Java JAR which lives under /usr/local/share/aether/). Copy the
# whole /opt/aether/ if it exists — it's a no-op when no toggle ran.
COPY --from=builder /opt/aether          /opt/aether

# Per-language runtime packages — what end-user binaries dlopen.
# Builder stage already removed the -dev versions; here we install
# the runtime-only ones on the same conditional flag.
RUN apt-get update \
 && if [ "$WITH_PERL"   = "1" ]; then apt-get install -y --no-install-recommends perl-base; fi \
 && if [ "$WITH_RUBY"   = "1" ]; then apt-get install -y --no-install-recommends ruby; fi \
 && if [ "$WITH_JS"     = "1" ]; then apt-get install -y --no-install-recommends libduktape207; fi \
 && rm -rf /var/lib/apt/lists/*
# WITH_JAVA intentionally NOT installed at runtime — end-user binaries
# spawn `java` as a subprocess, so the JRE belongs on the target host
# (where the binary is deployed), not in the toolchain image.

# Bind-mount points: source comes in at /work, products land at /out.
RUN mkdir -p /work /out
WORKDIR /work

# Entrypoint dispatcher.
COPY tools/docker/build-entry.sh /usr/local/bin/build-entry.sh
RUN chmod +x /usr/local/bin/build-entry.sh
ENTRYPOINT ["/usr/local/bin/build-entry.sh"]

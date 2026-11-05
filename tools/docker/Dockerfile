# Aether builder image — compile .ae source to Linux x86_64 binaries
# inside the container, extract the binary on completion.
#
# Two stages:
#   1. builder — has gcc + make + git + the Aether stdlib's -dev
#                dependencies. Builds and installs Aether from /src
#                (the build context root). Throw-away after stage 2
#                pulls /usr/local out.
#   2. runtime — debian:bookworm-slim + gcc + the runtime -dev packages
#                Aether-emitted C links against. Has /usr/local
#                populated from stage 1. ~314 MB.
#
# The WITH_JAVA / WITH_PERL / WITH_RUBY / WITH_JS build args went with the
# contrib/host bridges they captured headers for; the bridges live in
# https://github.com/aether-lang-dev/aether-guest-languages now.
#
# Usage:
#   docker build -t aether-builder:slim -f tools/docker/Dockerfile .
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

# Base toolchain + Aether's own -dev dependencies (always-on).
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc make ca-certificates git pkg-config \
        libssl-dev zlib1g-dev libnghttp2-dev libpcre2-dev \
        libexpat1-dev libsqlite3-dev \
 && rm -rf /var/lib/apt/lists/*

# Build & install Aether from the local source tree (the build
# context root). Aether's `make install` copies the stdlib + contrib
# source layout into /usr/local/share/aether/.
COPY . /src
WORKDIR /src
RUN make && make install

# =============================================================
# Stage 2 — runtime
# =============================================================
FROM debian:bookworm-slim

# Runtime image: gcc + libc6-dev (for gcc's standard headers) + the
# -dev packages Aether-emitted C wants at link time. NOT
# build-essential — we don't need g++ or dpkg-dev.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev make \
        libssl-dev zlib1g-dev libnghttp2-dev libpcre2-dev \
        libexpat1-dev libsqlite3-dev \
 && rm -rf /var/lib/apt/lists/*

# Pull the installed Aether toolchain from the builder stage.
COPY --from=builder /usr/local/bin/      /usr/local/bin/
COPY --from=builder /usr/local/lib/      /usr/local/lib/
COPY --from=builder /usr/local/include/  /usr/local/include/
COPY --from=builder /usr/local/share/    /usr/local/share/

# Bind-mount points: source comes in at /work, products land at /out.
RUN mkdir -p /work /out
WORKDIR /work

# Entrypoint dispatcher.
COPY tools/docker/build-entry.sh /usr/local/bin/build-entry.sh
RUN chmod +x /usr/local/bin/build-entry.sh
ENTRYPOINT ["/usr/local/bin/build-entry.sh"]

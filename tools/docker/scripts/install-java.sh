#!/bin/sh
# Strategy A for Java: install default-jdk-headless, build the
# aether-sandbox.jar that contrib.host.java needs, capture it,
# purge the JDK. Layer cost: ~50 KB (the JAR) instead of ~250 MB.
#
# Unlike the in-process C-bridge hosts (perl/ruby/js),
# contrib.host.java spawns `java` as a subprocess from end-user
# programs. So:
#   - BUILD-TIME (here): javac + jar to produce aether-sandbox.jar
#   - END-USER RUN-TIME (their target host): needs `java` on PATH
#
# We capture the JAR so the toolchain image can ship it via the
# contrib install layout, but the JDK/JRE themselves don't ride
# along. Documented in the image's README — end-user binaries that
# import contrib.host.java need a JRE installed wherever they're
# deployed.

set -eu

# javac defaults to the platform's default-charset; bookworm-slim has
# no UTF-8 locale installed, so source files with UTF-8 content
# (em-dashes etc.) fail. Force UTF-8 explicitly via the JAVA_TOOL_OPTIONS
# env var — picked up by javac without needing a generated locale.
export JAVA_TOOL_OPTIONS="-Dfile.encoding=UTF-8"

apt-get update
apt-get install -y --no-install-recommends default-jdk-headless

# Build the sandbox JAR. The contrib/host/java/build.sh wrapper
# writes to ./build/aether-sandbox.jar relative to the repo root,
# so cd into the source tree first.
cd /src
contrib/host/java/build.sh

# Capture: copy the JAR into the contrib install layout so it
# rides alongside the other host-bridge artifacts. The Aether
# install root we wrote in stage 1 is /usr/local/share/aether/.
mkdir -p /usr/local/share/aether/contrib/host/java
cp /src/build/aether-sandbox.jar /usr/local/share/aether/contrib/host/java/

# Purge the JDK + autoremove transitive deps (ca-certificates-java,
# libpcsclite, libnss3, etc.) that came in for the full JDK.
apt-get purge -y 'openjdk-*' 'default-jdk*'
apt-get autoremove -y --purge
rm -rf /var/lib/apt/lists/* /var/cache/apt /usr/lib/jvm

test -f /usr/local/share/aether/contrib/host/java/aether-sandbox.jar || {
    echo "install-java: aether-sandbox.jar missing after build" >&2
    exit 1
}

echo "install-java: $(du -sh /usr/local/share/aether/contrib/host/java/aether-sandbox.jar | cut -f1) at /usr/local/share/aether/contrib/host/java/"

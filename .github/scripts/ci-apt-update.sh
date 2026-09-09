#!/bin/sh
# `apt-get update` for CI, minus the repositories we do not use.
#
# apt refreshes EVERY repository configured on the runner, and GitHub's Ubuntu
# images ship third-party ones we never install from. When one of those serves
# an index whose hash does not match its own Release file, `apt-get update`
# exits 100, the `&&` in the caller short-circuits, and the job fails before
# running a line of our code:
#
#   E: Failed to fetch https://dl.google.com/linux/chrome-stable/.../Packages.gz
#      Hash Sum mismatch
#   ##[error]Process completed with exit code 100
#
# That took down ten CI jobs across four workflows and a release build on
# 2026-09-09, none of which touch a browser (#1981). Everything our jobs
# install -- gcc, clang, make, bc, pkg-config, libgtk-4-dev, curl, xz-utils,
# valgrind, qemu, the arm-none-eabi toolchain -- comes from Ubuntu's own
# archives, so dropping these costs nothing and removes an upstream that can
# veto our builds.
#
# Deliberately NOT `apt-get update || true`: a genuine failure to reach the
# Ubuntu archives must still fail the job, or the install that follows fails
# later with a worse message.
#
# Non-Debian hosts: `apt-get` is absent, the script exits non-zero, and callers
# that support both (memory-check.yml's `|| brew ...`) fall through as before.
set -e

sudo rm -f /etc/apt/sources.list.d/google-chrome.list \
           /etc/apt/sources.list.d/microsoft-prod.list \
           /etc/apt/sources.list.d/microsoft.list 2>/dev/null || true

sudo apt-get update

# Load-balancer comparison bench: aether against nginx and haproxy.
#
# The comparison in #1719 was run from an untracked directory on one
# contributor's box, so its numbers could not be reproduced or re-run by
# anyone else. This image is that harness, committed.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        build-essential ca-certificates curl git pkg-config \
        libssl-dev zlib1g-dev \
        nginx haproxy wrk procps linux-tools-generic strace gawk \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /bench
COPY run.sh profile.sh syscalls.sh workers.sh backends.conf nginx-lb.conf haproxy-lb.cfg ./
RUN chmod +x run.sh profile.sh syscalls.sh workers.sh
ENTRYPOINT ["/bench/run.sh"]

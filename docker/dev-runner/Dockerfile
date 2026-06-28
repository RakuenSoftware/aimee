# Ephemeral verify runner — autonomous-dev-execution-substrate §1.
#
# Carries the CI toolchain so a local mechanical/integration-tier verify run
# reproduces .github/workflows/ci.yml exactly (gcc/make + the same -dev libs +
# clang-format-19 + postgresql-client). The autonomous driver runs a work-item
# worktree's build/unit/lint steps inside a throwaway container started from this
# image (see scripts/dev-verify-runner.sh) — never on the host, never with the
# host's secrets or network.
#
# Pinned: the resolved toolchain versions + this image's id are recorded in
# scripts/dev-runner-pin.json; dev-verify-runner.sh refuses to run if the running
# image drifts from that manifest (so a local pass that CI would fail cannot ship).
# The base is pinned by tag (the homelab base registry exposes no content digest);
# the pin-check binds the toolchain versions, which is what actually gates parity.
FROM ubuntu:24.04

# Mirror ci.yml's apt sets: the lint job installs clang-format-19; the build /
# build-integrity jobs install the gcc/make + -dev libs (+ postgresql-client).
RUN apt-get update -q && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
        gcc make pkg-config clang-format-19 \
        libsqlite3-dev libcurl4-openssl-dev libpam0g-dev libssl-dev libpq-dev \
        libzstd-dev \
        postgresql-client \
        python3 python3-yaml \
        git ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# Non-root build user. The work-item tree is copied in at run time (no bind
# mount, no .git), owned by this user; the container is started --cap-drop=ALL
# --network=none with resource limits by the wrapper.
RUN useradd --create-home --uid 1001 runner
USER runner
WORKDIR /work

# Record the resolved toolchain at build time for audit + the pin manifest.
LABEL org.aimee.runner="dev-verify" \
      org.aimee.toolchain="gcc make clang-format-19 libsqlite3 libcurl libpam libssl libpq postgresql-client"

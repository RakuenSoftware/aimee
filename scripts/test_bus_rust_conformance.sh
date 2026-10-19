#!/usr/bin/env bash
# test_bus_rust_conformance.sh — the Rust reference client's conformance legs.
#
# The third independent client of the single in-source C host. Each leg is the
# Rust counterpart of a leg in test_bus_conformance.sh:
#   1. Vectors and units: the Rust codec produces and accepts the exact
#      committed wire bytes; ring, region, AMOD and runtime logic unit tests;
#      rustfmt and clippy with warnings as errors.
#   2. Interop: the C host, exposed on a Unix socket, driven by the Rust client
#      across a process boundary (notification, request/reply,
#      capability_absent, cancel, backpressure, reaped-client recovery).
#   3. Module runtime interop: the C host and core caller drive a Rust module
#      process through dispatch, fragmented payloads, deadline, cancellation
#      and SIGTERM shutdown, admitting it by executable path and peer creds.
#   4. Single-host: the mechanical D8 check, which also holds Rust to
#      client-only and to no link edge with either reference implementation.
#
# Bounded by timeouts so it cannot hang. Run via `make -C src rust-bus-tests`.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

command -v cargo >/dev/null || {
   echo "FAIL: cargo not found; install the toolchain pinned in rust/rust-toolchain.toml" >&2
   exit 1
}

# Sockets live under TMPDIR, and sun_path is 108 bytes; keep the base short.
work="$(mktemp -d "${TMPDIR:-/tmp}/abrs.XXXXXX")"
trap 'rm -rf "$work"' EXIT

echo "== 1. Rust: format, lint, unit tests and wire vectors =="
(
   cd rust
   cargo fmt --all --check
   cargo clippy --locked --all-targets -- -D warnings
   cargo test --locked
)

echo "== 2. cross-language interop: C host <-> Rust client =="
make -C src --no-print-directory bus-conformance-host
harness="$repo_root/src/build/obj/tests/bus-conformance-host"
if [ ! -x "$harness" ]; then
   harness=$(find "$repo_root/src" -name bus-conformance-host -type f -perm -u+x 2>/dev/null | head -1)
fi
[ -x "$harness" ] || { echo "FAIL: conformance host harness not built" >&2; exit 1; }
(
   cd rust
   BUS_CONFORMANCE_HOST="$harness" TMPDIR="$work" timeout 120s \
      cargo test --locked -p aimee-bus --test c_host_conformance -- --ignored --test-threads=1
)

echo "== 3. module runtime interop: C host/core caller <-> Rust module process =="
make -C src --no-print-directory "build/obj/tests/unit-test-module-runtime"
module_harness="$repo_root/src/build/obj/tests/unit-test-module-runtime"
( cd rust && cargo build --locked --release -p aimee-bus --bin aimee-bus-module-helper )
rust_module="$repo_root/rust/target/release/aimee-bus-module-helper"
TMPDIR="$work" timeout 60s "$module_harness" "$rust_module"

echo "== 4. single host, Rust client-only =="
"$repo_root/scripts/check_bus_single_host.sh"

echo "test_bus_rust_conformance: ok"

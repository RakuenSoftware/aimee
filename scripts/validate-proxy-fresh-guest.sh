#!/usr/bin/env bash
# Run only inside a newly provisioned disposable Debian guest.
set -euo pipefail
ROOT=${1:-/opt/aimee-proxy-validation}
export DEBIAN_FRONTEND=noninteractive
export PATH="/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin:$PATH"
if ! { apt-get update -qq &&
       apt-get install -y -qq --no-install-recommends python3 openssl ca-certificates \
         nodejs npm git make libpq5 libsqlite3-0 libssl3t64 libzstd1 zlib1g; } >/tmp/aimee-proxy-provision.log 2>&1; then
  tail -80 /tmp/aimee-proxy-provision.log >&2
  exit 1
fi
npm install --global @openai/codex@0.153.4
export PATH="$(npm prefix --global)/bin:$PATH"
install -m 0755 "$ROOT/aimee" /usr/local/bin/aimee
hostname
cat /etc/os-release
codex --version
sha256sum /usr/local/bin/aimee
cd "$ROOT"
# Preserve the build's separate test-binary directory: the agent suite creates
# fake clients on PATH, which a real client beside the test binary would shadow
# through the runtime's executable-directory fallback.
"$ROOT/tests/unit-test-openai-shape"
"$ROOT/tests/unit-test-cli-profile"
"$ROOT/tests/unit-test-server-dispatch"
"$ROOT/tests/unit-test-util"
"$ROOT/tests/unit-test-agent"
AIMEE_TEST_MODULE_BIN="$ROOT/aimee-module" "$ROOT/tests/unit-test-guardrails"
AIMEE_TEST_REQUIRE_CODEX=1 AIMEE_TEST_PROXY_BINARY=/usr/local/bin/aimee \
  python3 "$ROOT/scripts/tests/test_thin_client_proxy.py" -v

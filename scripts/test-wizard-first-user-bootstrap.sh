#!/usr/bin/env bash
# Fresh-install proof for the setup wizard's first remote administrator.
set -euo pipefail

if [[ $(id -u) -ne 0 ]]; then
  echo "wizard-bootstrap-e2e: SKIP (root is required to exercise root-owned UDS webuser attestation)"
  exit 0
fi

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
server_bin="$repo_dir/aimee-server"
client_bin="$repo_dir/aimee"
[[ -x "$server_bin" && -x "$client_bin" ]] || {
  echo "wizard-bootstrap-e2e: build aimee and aimee-server first" >&2
  exit 1
}

e2e_tmp=$(mktemp -d -p /tmp aimee-wizard-bootstrap.XXXXXX)
e2e_server_pid=""
cleanup() {
  local status=$?
  if [[ -n "$e2e_server_pid" ]]; then
    kill -TERM "$e2e_server_pid" 2>/dev/null || true
    wait "$e2e_server_pid" 2>/dev/null || true
  fi
  if (( status != 0 )) && [[ -f "$e2e_tmp/server/server.log" ]]; then
    tail -80 "$e2e_tmp/server/server.log" >&2 || true
  fi
  [[ "$e2e_tmp" == /tmp/aimee-wizard-bootstrap.* ]] && rm -rf -- "$e2e_tmp"
  exit "$status"
}
trap cleanup EXIT INT TERM

mkdir -p "$e2e_tmp/server" "$e2e_tmp/client"
e2e_tls_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
python3 - "$e2e_tmp/server/aimee.yaml" "$e2e_tls_port" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
port = int(sys.argv[2])
path.write_text(
    "aimee:\n"
    "  api:\n"
    "    http_port: 0\n"
    f"    tls_port: {port}\n"
    "    mtls: optional\n"
    "    remote_writes: off\n"
)
PY

AIMEE_HOME="$e2e_tmp/server" AIMEE_DEPLOY_ENABLED=1 AIMEE_API_REMOTE_WRITES=off \
  "$server_bin" --foreground --log-level=debug >"$e2e_tmp/server.stdout" 2>&1 &
e2e_server_pid=$!
for _ in $(seq 1 100); do
  [[ -S "$e2e_tmp/server/aimee-http.sock" ]] && \
    curl --silent --fail --cacert "$e2e_tmp/server/tls/server.crt" \
      "https://127.0.0.1:$e2e_tls_port/v1/health" >/dev/null && break
  sleep 0.05
done
[[ -S "$e2e_tmp/server/aimee-http.sock" ]] || {
  echo "wizard-bootstrap-e2e: server UDS did not start" >&2
  exit 1
}
[[ ! -e "$e2e_tmp/server/tls/server.key" ]]
[[ ! -e "$e2e_tmp/server/server.token" ]]
! grep -Eq 'bearer(_token)?:' "$e2e_tmp/server/aimee.yaml"

curl --silent --show-error --unix-socket "$e2e_tmp/server/aimee-http.sock" \
  -H 'X-Aimee-Webuser: alice' \
  -H 'Content-Type: application/json' -X POST -d '{}' \
  -o "$e2e_tmp/apply.json" -w '%{http_code}' http://localhost/v1/deploy/apply \
  >"$e2e_tmp/apply.status"
python3 - "$e2e_tmp/apply.status" "$e2e_tmp/apply.json" <<'PY'
import json
import pathlib
import sys

assert pathlib.Path(sys.argv[1]).read_text() == "200"
data = json.loads(pathlib.Path(sys.argv[2]).read_text())
enrollment = data["enrollment"]
assert enrollment["state"] == "ready"
assert enrollment["principal"] == "webuser:alice"
assert enrollment["tier"] == "full" and enrollment["mtls"] is True
assert len(enrollment["bearer_token"]) == 64
PY
e2e_bearer=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["enrollment"]["bearer_token"])' "$e2e_tmp/apply.json")

# The pending bearer authenticates enrollment but has no write tier by itself.
# Use kb.build because CAP_INDEX_ADMIN sits outside CAPS_AUTHENTICATED: an
# ordinary mutation such as persona PUT can pass without proving that the
# certificate-bound `full` tier survived capability derivation.
e2e_body='{}'
curl --silent --show-error --cacert "$e2e_tmp/server/tls/server.crt" \
  -H "Authorization: Bearer $e2e_bearer" -H 'Content-Type: application/json' \
  -X POST -d "$e2e_body" -o "$e2e_tmp/bearer-write.json" -w '%{http_code}' \
  "https://127.0.0.1:$e2e_tls_port/v1/kb/build" \
  >"$e2e_tmp/bearer-write.status"
[[ $(<"$e2e_tmp/bearer-write.status") == 403 ]]

# Reaching and pinning the right TLS peer is not a successful setup when that
# peer rejects the supplied credential. The command used to exit zero here and
# leave `remote status` to reveal the 401 on the next invocation.
mkdir -p "$e2e_tmp/invalid-client"
set +e
AIMEE_HOME="$e2e_tmp/invalid-client" AIMEE_NO_CLIENT_INTEGRATIONS=1 \
  "$client_bin" --json remote set "https://127.0.0.1:$e2e_tls_port" invalid-bearer \
  >"$e2e_tmp/invalid-remote-set.json"
e2e_invalid_remote_rc=$?
set -e
[[ $e2e_invalid_remote_rc -ne 0 ]]
python3 - "$e2e_tmp/invalid-remote-set.json" <<'PY'
import json
import pathlib
import sys

result = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert result["ok"] is False and result["verified"] is True
assert result["mtls_enrolled"] is False
PY

AIMEE_HOME="$e2e_tmp/client" AIMEE_NO_CLIENT_INTEGRATIONS=1 \
  "$client_bin" --json remote set "https://127.0.0.1:$e2e_tls_port" "$e2e_bearer" \
  >"$e2e_tmp/remote-set.json"
python3 - "$e2e_tmp/remote-set.json" <<'PY'
import json
import pathlib
import sys

result = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert result["ok"] is True and result["verified"] is True
assert result["mtls_enrolled"] is True
PY

curl --silent --show-error --cacert "$e2e_tmp/server/tls/server.crt" \
  --cert "$e2e_tmp/client/tls/client.crt" --key "$e2e_tmp/client/tls/client.key" \
  -H "Authorization: Bearer $e2e_bearer" -H 'Content-Type: application/json' \
  -X POST -d "$e2e_body" -o "$e2e_tmp/mtls-write.json" -w '%{http_code}' \
  "https://127.0.0.1:$e2e_tls_port/v1/kb/build" \
  >"$e2e_tmp/mtls-write.status"
[[ $(<"$e2e_tmp/mtls-write.status") == 200 ]]

# Re-entry is idempotent for alice and cannot transfer ownership to bob.
curl --silent --show-error --unix-socket "$e2e_tmp/server/aimee-http.sock" \
  -H 'X-Aimee-Webuser: alice' \
  -H 'Content-Type: application/json' -X POST -d '{}' -o "$e2e_tmp/reapply.json" \
  -w '%{http_code}' http://localhost/v1/deploy/apply >"$e2e_tmp/reapply.status"
curl --silent --show-error --unix-socket "$e2e_tmp/server/aimee-http.sock" \
  -H 'X-Aimee-Webuser: bob' \
  -H 'Content-Type: application/json' -X POST -d '{}' -o "$e2e_tmp/bob.json" \
  -w '%{http_code}' http://localhost/v1/deploy/apply >"$e2e_tmp/bob.status"
python3 - "$e2e_tmp/server/aimee.db" "$e2e_tmp/reapply.status" "$e2e_tmp/reapply.json" \
  "$e2e_tmp/bob.status" "$e2e_bearer" <<'PY'
import hashlib
import json
import pathlib
import sqlite3
import sys

assert pathlib.Path(sys.argv[2]).read_text() == "200"
reapply = json.loads(pathlib.Path(sys.argv[3]).read_text())
assert reapply["enrollment"]["state"] == "paired"
assert "bearer_token" not in reapply["enrollment"]
assert pathlib.Path(sys.argv[4]).read_text() == "403"
db = sqlite3.connect(sys.argv[1])
assert db.execute("select principal from remote_first_user").fetchone() == ("webuser:alice",)
grant = db.execute(
    "select principal,tier,cert_serial is not null,bearer_sha256 from remote_client_grants"
).fetchone()
assert grant[:3] == ("webuser:alice", "full", 1)
assert grant[3] == hashlib.sha256(sys.argv[5].encode()).hexdigest()
columns = {row[1] for row in db.execute("pragma table_info(remote_client_grants)")}
assert "bearer_token" not in columns and "bearer" not in columns
PY

# Revocation is checked again on every request, after the TLS handshake. A
# certificate that was valid moments ago loses the bound grant immediately.
e2e_serial=$(openssl x509 -in "$e2e_tmp/client/tls/client.crt" -noout -serial | cut -d= -f2)
curl --silent --show-error --unix-socket "$e2e_tmp/server/aimee-http.sock" \
  -H 'Content-Type: application/json' \
  -X POST -d "{\"serial\":\"$e2e_serial\"}" -o "$e2e_tmp/revoke.json" \
  -w '%{http_code}' http://localhost/v1/cert/revoke >"$e2e_tmp/revoke.status"
[[ $(<"$e2e_tmp/revoke.status") == 200 ]]
set +e
curl --silent --show-error --cacert "$e2e_tmp/server/tls/server.crt" \
  --cert "$e2e_tmp/client/tls/client.crt" --key "$e2e_tmp/client/tls/client.key" \
  -H "Authorization: Bearer $e2e_bearer" -H 'Content-Type: application/json' \
  -X PUT -d "$e2e_body" -o "$e2e_tmp/revoked-write.json" -w '%{http_code}' \
  "https://127.0.0.1:$e2e_tls_port/v1/personas/wizard-bootstrap-e2e-revoked" \
  >"$e2e_tmp/revoked-write.status" 2>"$e2e_tmp/revoked-write.stderr"
e2e_revoked_rc=$?
set -e
if [[ $e2e_revoked_rc -eq 0 ]]; then
  [[ $(<"$e2e_tmp/revoked-write.status") == 403 ]]
else
  # Once the automatic mTLS ramp is required, OpenSSL rejects the revoked leaf
  # during the handshake and there is intentionally no HTTP status at all.
  grep -qi 'certificate revoked' "$e2e_tmp/revoked-write.stderr"
fi

echo "wizard-bootstrap-e2e: OK (bearer-only denied; bound mTLS accepted; revoked cert denied)"

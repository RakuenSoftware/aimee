#!/usr/bin/env bash
# CT260-only P6c Bedrock HTTPS composition gate. This script deliberately maps the
# production-derived Bedrock hostname to loopback; no socket override enters C code.
set -euo pipefail

readonly gate_name='run-p6c-egress-ct260'
readonly bedrock_host='bedrock-runtime.us-east-1.amazonaws.com'
readonly team_id='960260'
readonly live_target='src/build/obj/tests/unit-test-kb-bedrock-live'

die() {
  printf '%s: %s\n' "$gate_name" "$1" >&2
  exit 1
}

[[ ${EUID:-$(id -u)} -eq 0 ]] || die 'must run as root inside CT260'
[[ -n ${AIMEE_TEST_PG_URL:-} ]] || die 'AIMEE_TEST_PG_URL is required (CT103 PostgreSQL)'

for command_name in make openssl psql python3; do
  command -v "$command_name" >/dev/null 2>&1 || die "missing required command: $command_name"
done
[[ -x scripts/p6c_bedrock_mock.py ]] || die 'scripts/p6c_bedrock_mock.py is missing or not executable'
[[ -f src/Makefile ]] || die 'run from the CT260 aimee checkout root'

umask 077
scratch_dir=$(mktemp -d /tmp/aimee-p6c-egress.XXXXXX)
[[ -n $scratch_dir && -d $scratch_dir && $scratch_dir == /tmp/aimee-p6c-egress.* ]] ||
  die 'failed to create bounded scratch directory'

hosts_backup="$scratch_dir/hosts.original"
mock_ready="$scratch_dir/mock.ready"
mock_counter="$scratch_dir/mock.accepted"
mock_log="$scratch_dir/mock.log"
case_log="$scratch_dir/case.log"
ca_key="$scratch_dir/ca.key"
ca_cert="$scratch_dir/ca.crt"
server_key="$scratch_dir/server.key"
server_csr="$scratch_dir/server.csr"
server_cert="$scratch_dir/server.crt"
wrong_server_key="$scratch_dir/wrong-server.key"
wrong_server_csr="$scratch_dir/wrong-server.csr"
wrong_server_cert="$scratch_dir/wrong-server.crt"
wrong_ca_key="$scratch_dir/wrong-ca.key"
wrong_ca_cert="$scratch_dir/wrong-ca.crt"
cert_ext="$scratch_dir/server.ext"
wrong_cert_ext="$scratch_dir/wrong-server.ext"

mock_pid=''
hosts_changed=0
db_seeded=0

stop_mock() {
  if [[ -n $mock_pid ]]; then
    if kill -0 "$mock_pid" 2>/dev/null; then
      kill "$mock_pid" 2>/dev/null || true
    fi
    wait "$mock_pid" 2>/dev/null || true
    mock_pid=''
  fi
  rm -f -- "$mock_ready"
}

cleanup() {
  local rc=$?
  trap - EXIT
  set +e
  stop_mock
  if (( hosts_changed )); then
    cp -p -- "$hosts_backup" /etc/hosts || rc=1
    hosts_changed=0
  fi
  if (( db_seeded )); then
    psql -X -q -v ON_ERROR_STOP=1 "$AIMEE_TEST_PG_URL" >/dev/null 2>&1 <<'SQL' || rc=1
BEGIN;
DELETE FROM org_model_entitlement
 WHERE team_id = 960260 OR model_id IN ('model','p6c-unentitled','p6c-unsupported');
DELETE FROM org_model_catalog
 WHERE model_id IN ('model','p6c-unentitled','p6c-unsupported');
DELETE FROM kb_team_membership
 WHERE team = 960260 AND identity_key = 'oidc:test:p6c_member_a';
DELETE FROM kb_team WHERE id = 960260;
COMMIT;
SQL
    db_seeded=0
  fi
  find "$scratch_dir" -mindepth 1 -maxdepth 1 -type f -delete 2>/dev/null || rc=1
  rmdir -- "$scratch_dir" 2>/dev/null || rc=1
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

cp -p -- /etc/hosts "$hosts_backup"
if awk -v wanted="$bedrock_host" '
  /^[[:space:]]*#/ { next }
  { for (i = 2; i <= NF; i++) if ($i == wanted) found = 1 }
  END { exit found ? 0 : 1 }
' /etc/hosts; then
  die 'Bedrock test hostname already exists in /etc/hosts; refusing ambiguous mutation'
fi
hosts_marker="# aimee-p6c-egress-ct260 pid=$$"
printf '\n%s\n127.0.0.1 %s\n' "$hosts_marker" "$bedrock_host" >>/etc/hosts
hosts_changed=1

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
  -subj '/CN=aimee-p6c-ct260-test-ca' -keyout "$ca_key" -out "$ca_cert" \
  -addext 'basicConstraints=critical,CA:TRUE' \
  -addext 'keyUsage=critical,keyCertSign,cRLSign' >/dev/null 2>&1

printf 'subjectAltName=DNS:%s\nextendedKeyUsage=serverAuth\nkeyUsage=digitalSignature,keyEncipherment\n' \
  "$bedrock_host" >"$cert_ext"
openssl req -new -newkey rsa:2048 -nodes -sha256 -subj "/CN=$bedrock_host" \
  -keyout "$server_key" -out "$server_csr" >/dev/null 2>&1
openssl x509 -req -sha256 -days 1 -in "$server_csr" -CA "$ca_cert" -CAkey "$ca_key" \
  -CAcreateserial -extfile "$cert_ext" -out "$server_cert" >/dev/null 2>&1

printf 'subjectAltName=DNS:wrong.invalid\nextendedKeyUsage=serverAuth\nkeyUsage=digitalSignature,keyEncipherment\n' \
  >"$wrong_cert_ext"
openssl req -new -newkey rsa:2048 -nodes -sha256 -subj '/CN=wrong.invalid' \
  -keyout "$wrong_server_key" -out "$wrong_server_csr" >/dev/null 2>&1
openssl x509 -req -sha256 -days 1 -in "$wrong_server_csr" -CA "$ca_cert" -CAkey "$ca_key" \
  -CAcreateserial -extfile "$wrong_cert_ext" -out "$wrong_server_cert" >/dev/null 2>&1

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
  -subj '/CN=aimee-p6c-wrong-test-ca' -keyout "$wrong_ca_key" -out "$wrong_ca_cert" \
  -addext 'basicConstraints=critical,CA:TRUE' \
  -addext 'keyUsage=critical,keyCertSign,cRLSign' >/dev/null 2>&1

psql -X -q -v ON_ERROR_STOP=1 "$AIMEE_TEST_PG_URL" >/dev/null <<'SQL'
BEGIN;
DO $guard$
BEGIN
  IF EXISTS (SELECT 1 FROM kb_team WHERE id = 960260)
     OR EXISTS (SELECT 1 FROM kb_team_membership
                 WHERE identity_key = 'oidc:test:p6c_member_a')
     OR EXISTS (SELECT 1 FROM org_model_catalog
                 WHERE model_id IN ('model','p6c-unentitled','p6c-unsupported')) THEN
    RAISE EXCEPTION 'P6c CT fixture identifiers already exist';
  END IF;
END $guard$;
SELECT set_config('aimee.principal', 'owner', true);
INSERT INTO kb_team(id, name) VALUES (960260, 'p6c_ct260_egress');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:p6c_member_a', 960260, 1);
SELECT org_catalog_bedrock_upsert(
  'model', 'P6c CT model', 'converse', 'anthropic', 'foundation',
  'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
SELECT org_catalog_bedrock_upsert(
  'p6c-unentitled', 'P6c CT unentitled', 'converse', 'anthropic', 'foundation',
  'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
SELECT org_catalog_upsert(
  'p6c-unsupported', 'P6c CT unsupported', 'test-provider', 'anthropic', '', true);
SELECT org_model_entitle('model', 960260);
SELECT org_model_entitle('p6c-unsupported', 960260);
COMMIT;
SQL
db_seeded=1

make -C src -j"$(nproc)" build/obj/tests/unit-test-kb-bedrock-live >/dev/null

start_mock() {
  local case_id=$1
  local cert=${2:-$server_cert}
  local key=${3:-$server_key}
  stop_mock
  : >"$mock_log"
  python3 scripts/p6c_bedrock_mock.py --cert "$cert" --key "$key" \
    --bind 127.0.0.1 --port 443 --case "$case_id" \
    --counter-file "$mock_counter" --ready-file "$mock_ready" 2>"$mock_log" &
  mock_pid=$!
  local attempt=0
  while (( attempt < 100 )); do
    [[ -f $mock_ready ]] && break
    kill -0 "$mock_pid" 2>/dev/null || die "mock failed to start for case $case_id"
    sleep 0.05
    ((attempt += 1))
  done
  [[ -f $mock_ready ]] || die "mock readiness timeout for case $case_id"
}

accepted_count() {
  local value
  [[ -f $mock_counter ]] || die 'mock accepted counter is missing'
  IFS= read -r value <"$mock_counter"
  [[ $value =~ ^[0-9]+$ ]] || die 'mock accepted counter is malformed'
  printf '%s' "$value"
}

assert_count() {
  local expected=$1
  local actual
  actual=$(accepted_count)
  [[ $actual == "$expected" ]] || die "accepted counter mismatch (want $expected, got $actual)"
}

run_success() {
  local label=$1
  local mode=$2
  if ! SSL_CERT_FILE="$ca_cert" AIMEE_TEST_TEAM_ID="$team_id" \
      "$live_target" model "$mode" >"$case_log" 2>&1; then
    sed 's/^/live: /' "$case_log" >&2
    sed 's/^/mock: /' "$mock_log" >&2
    die "$label unexpectedly failed"
  fi
}

run_failure() {
  local label=$1
  local model=$2
  local mode=$3
  local trust=${4:-$ca_cert}
  if SSL_CERT_FILE="$trust" AIMEE_TEST_TEAM_ID="$team_id" \
      "$live_target" "$model" "$mode" >"$case_log" 2>&1; then
    sed 's/^/live: /' "$case_log" >&2
    sed 's/^/mock: /' "$mock_log" >&2
    die "$label unexpectedly succeeded"
  fi
}

start_mock nonstream-success
run_success buffered-success buffered
assert_count 1
stop_mock

start_mock fragmented-stream-success
run_success fragmented-stream-success stream
assert_count 1
stop_mock

start_mock nonstream-success
if AIMEE_TEST_WRONG_SECRET=1 SSL_CERT_FILE="$ca_cert" AIMEE_TEST_TEAM_ID="$team_id" \
    "$live_target" model buffered >"$case_log" 2>&1; then
  die 'wrong-secret unexpectedly succeeded'
fi
assert_count 0
stop_mock

for case_id in wrong-media non-2xx malformed-framing; do
  start_mock "$case_id"
  run_failure "$case_id" model buffered
  assert_count 1
  stop_mock
done

for case_id in bad-crc complete-frame-semantic-truncation; do
  start_mock "$case_id"
  run_failure "$case_id" model stream
  assert_count 1
  stop_mock
done

start_mock nonstream-success
run_failure tls-wrong-ca model buffered "$wrong_ca_cert"
assert_count 0
stop_mock

start_mock nonstream-success "$wrong_server_cert" "$wrong_server_key"
run_failure tls-wrong-hostname model buffered
assert_count 0
stop_mock

start_mock nonstream-success
for model_id in p6c-missing p6c-unentitled p6c-unsupported; do
  run_failure "pre-network-$model_id" "$model_id" buffered
  assert_count 0
done
stop_mock

printf '%s: all CT260 TLS/PG gates passed\n' "$gate_name"

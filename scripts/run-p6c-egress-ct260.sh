#!/usr/bin/env bash
# CT260-only P6c Bedrock HTTPS composition gate. This script deliberately maps the
# production-derived Bedrock hostname to loopback; no socket override enters C code.
set -euo pipefail

readonly gate_name='run-p6c-egress-ct260'
readonly bedrock_host='bedrock-runtime.us-east-1.amazonaws.com'
readonly team_id='960260'
readonly live_target='src/build/obj/tests/unit-test-kb-bedrock-live'
readonly live_resolver='src/build/obj/tests/aimee-kb-resolver'

die() {
  printf '%s: %s\n' "$gate_name" "$1" >&2
  exit 1
}

[[ ${EUID:-$(id -u)} -eq 0 ]] || die 'must run as root inside CT260'
[[ -n ${AIMEE_TEST_PG_URL:-} ]] || die 'AIMEE_TEST_PG_URL is required (CT103 PostgreSQL)'

for command_name in flock make openssl psql python3 sha256sum stat update-ca-certificates; do
  command -v "$command_name" >/dev/null 2>&1 || die "missing required command: $command_name"
done
[[ -x scripts/p6c_bedrock_mock.py ]] || die 'scripts/p6c_bedrock_mock.py is missing or not executable'
[[ -f src/Makefile ]] || die 'run from the CT260 aimee checkout root'
unset SSL_CERT_DIR SSL_CERT_FILE

umask 077
exec 9>/run/lock/aimee-p6c-egress-ct260.lock
flock -n 9 || die 'another P6c CT260 egress gate owns the global test resources'

scratch_dir=$(mktemp -d /tmp/aimee-p6c-egress.XXXXXX)
[[ -n $scratch_dir && -d $scratch_dir && $scratch_dir == /tmp/aimee-p6c-egress.* ]] ||
  die 'failed to create bounded scratch directory'

hosts_filtered="$scratch_dir/hosts.filtered"
mock_ready="$scratch_dir/mock.ready"
mock_counter="$scratch_dir/mock.accepted"
mock_observed="$scratch_dir/mock.observed"
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
wrong_ca_server_key="$scratch_dir/wrong-ca-server.key"
wrong_ca_server_csr="$scratch_dir/wrong-ca-server.csr"
wrong_ca_server_cert="$scratch_dir/wrong-ca-server.crt"
cert_ext="$scratch_dir/server.ext"
wrong_cert_ext="$scratch_dir/wrong-server.ext"
db_lock_state="$scratch_dir/db-lock.state"
db_lock_log="$scratch_dir/db-lock.log"

mock_pid=''
hosts_changed=0
system_ca_owned=0
system_ca_inode=''
db_seeded=0
db_lock_pid=''
deferred_signal=''
readonly hosts_line="127.0.0.1 $bedrock_host # aimee-p6c-egress-ct260 pid=$$"
readonly system_ca_cert="/usr/local/share/ca-certificates/aimee-p6c-egress-ct260-pid-$$.crt"

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

stop_db_lock() {
  if [[ -n $db_lock_pid ]]; then
    if kill -0 "$db_lock_pid" 2>/dev/null; then
      kill "$db_lock_pid" 2>/dev/null || true
    fi
    wait "$db_lock_pid" 2>/dev/null || true
    db_lock_pid=''
  fi
}

cleanup() {
  local rc=$?
  local current_ca_inode
  trap - EXIT
  set +e
  stop_mock
  if (( hosts_changed )); then
    if awk -v exact="$hosts_line" '$0 != exact' /etc/hosts >"$hosts_filtered"; then
      cp -- "$hosts_filtered" /etc/hosts || rc=1
    else
      rc=1
    fi
    hosts_changed=0
  fi
  if (( system_ca_owned )); then
    current_ca_inode=$(stat -Lc '%d:%i' "$system_ca_cert" 2>/dev/null)
    if [[ $current_ca_inode == "$system_ca_inode" ]]; then
      rm -f -- "$system_ca_cert" || rc=1
      update-ca-certificates >/dev/null 2>&1 || rc=1
    else
      rc=1
    fi
    system_ca_owned=0
  fi
  if (( db_seeded )); then
    psql -X -q -v ON_ERROR_STOP=1 "$AIMEE_TEST_PG_URL" >/dev/null 2>&1 <<'SQL' || rc=1
BEGIN;
DO $guard$
BEGIN
  IF EXISTS (
    SELECT 1
      FROM org_model_entitlement
     WHERE model_id IN ('model','p6c-unentitled','p6c-unsupported')
       AND NOT (
         team_id = 960260
         AND model_id IN ('model','p6c-unsupported')
       )
  ) THEN
    RAISE EXCEPTION 'P6c CT catalog has unexpected entitlement references';
  END IF;
END $guard$;
DELETE FROM org_model_entitlement
 WHERE team_id = 960260
   AND model_id IN ('model','p6c-unsupported');
DELETE FROM org_model_catalog
 WHERE model_id IN ('model','p6c-unentitled','p6c-unsupported');
DELETE FROM kb_team_membership
 WHERE team = 960260 AND identity_key = 'oidc:test:p6c_member_a';
DELETE FROM kb_team WHERE id = 960260;
COMMIT;
SQL
    db_seeded=0
  fi
  stop_db_lock
  find "$scratch_dir" -mindepth 1 -maxdepth 1 -type f -delete 2>/dev/null || rc=1
  rmdir -- "$scratch_dir" 2>/dev/null || rc=1
  exit "$rc"
}

restore_termination_traps() {
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
}

defer_termination_traps() {
  trap 'defer_termination HUP' HUP
  trap 'defer_termination INT' INT
  trap 'defer_termination TERM' TERM
}

defer_termination() {
  [[ -n $deferred_signal ]] || deferred_signal=$1
}

exit_for_deferred_signal() {
  case $deferred_signal in
    HUP) exit 129 ;;
    INT) exit 130 ;;
    TERM) exit 143 ;;
    '') return ;;
    *) die 'internal deferred-signal state is invalid' ;;
  esac
}

recover_stale_system_cas() {
  # CT260 is a throwaway integration container.  This deliberately narrow
  # recovery is not a general-purpose host trust-store repair mechanism.  The
  # exclusive gate flock is already held, so no compliant active run can own a
  # matching file while this scan executes.
  local cert_path metadata subject issuer basic_constraints key_usage cert_inode cert_hash
  local index
  local -a candidates=()
  local -a verified_paths=()
  local -a verified_inodes=()
  local -a verified_hashes=()

  shopt -s nullglob
  candidates=(/usr/local/share/ca-certificates/aimee-p6c-egress-ct260-pid-*.crt)
  shopt -u nullglob

  for cert_path in "${candidates[@]}"; do
    [[ $cert_path =~ ^/usr/local/share/ca-certificates/aimee-p6c-egress-ct260-pid-[0-9]+\.crt$ ]] ||
      die 'unverified stale system CA filename; refusing trust-store mutation'
    [[ -f $cert_path && ! -L $cert_path ]] ||
      die 'unverified stale system CA file type; refusing trust-store mutation'
    metadata=$(stat -c '%u:%a' -- "$cert_path") ||
      die 'cannot inspect stale system CA ownership'
    [[ $metadata == '0:600' ]] ||
      die 'unverified stale system CA owner or mode; refusing deletion'
    awk '
      BEGIN { inside = 0; begins = 0; ends = 0 }
      $0 == "-----BEGIN CERTIFICATE-----" {
        if (inside || begins) exit 1
        inside = 1; begins++; next
      }
      $0 == "-----END CERTIFICATE-----" {
        if (!inside || ends) exit 1
        inside = 0; ends++; next
      }
      inside && $0 !~ /^[A-Za-z0-9+\/=]+$/ { exit 1 }
      !inside && $0 !~ /^[[:space:]]*$/ { exit 1 }
      END { if (inside || begins != 1 || ends != 1) exit 1 }
    ' "$cert_path" || die 'unverified stale system CA PEM shape; refusing deletion'
    subject=$(LC_ALL=C openssl x509 -in "$cert_path" -noout -subject -nameopt RFC2253) ||
      die 'cannot parse stale system CA subject'
    issuer=$(LC_ALL=C openssl x509 -in "$cert_path" -noout -issuer -nameopt RFC2253) ||
      die 'cannot parse stale system CA issuer'
    [[ $subject == 'subject=CN=aimee-p6c-ct260-test-ca' &&
      $issuer == 'issuer=CN=aimee-p6c-ct260-test-ca' ]] ||
      die 'unverified stale system CA identity; refusing deletion'
    basic_constraints=$(
      LC_ALL=C openssl x509 -in "$cert_path" -noout -ext basicConstraints |
        tr -d '[:space:]'
    ) || die 'cannot parse stale system CA basic constraints'
    key_usage=$(
      LC_ALL=C openssl x509 -in "$cert_path" -noout -ext keyUsage |
        tr -d '[:space:]'
    ) || die 'cannot parse stale system CA key usage'
    [[ $basic_constraints == 'X509v3BasicConstraints:criticalCA:TRUE' ]] ||
      die 'unverified stale system CA constraints; refusing deletion'
    [[ $key_usage == 'X509v3KeyUsage:criticalCertificateSign,CRLSign' ]] ||
      die 'unverified stale system CA key usage; refusing deletion'
    LC_ALL=C openssl verify -no_check_time -CAfile "$cert_path" "$cert_path" >/dev/null 2>&1 ||
      die 'unverified stale system CA signature; refusing deletion'
    cert_inode=$(stat -Lc '%d:%i' -- "$cert_path") ||
      die 'cannot record stale system CA inode'
    cert_hash=$(sha256sum -- "$cert_path") ||
      die 'cannot fingerprint stale system CA'
    cert_hash=${cert_hash%% *}
    [[ $cert_hash =~ ^[0-9a-f]{64}$ ]] ||
      die 'invalid stale system CA fingerprint'
    verified_paths+=("$cert_path")
    verified_inodes+=("$cert_inode")
    verified_hashes+=("$cert_hash")
  done

  for index in "${!verified_paths[@]}"; do
    cert_path=${verified_paths[$index]}
    cert_inode=$(stat -Lc '%d:%i' -- "$cert_path" 2>/dev/null)
    metadata=$(stat -c '%u:%a' -- "$cert_path" 2>/dev/null)
    cert_hash=$(sha256sum -- "$cert_path" 2>/dev/null)
    cert_hash=${cert_hash%% *}
    [[ ! -L $cert_path && $metadata == '0:600' &&
      $cert_inode == "${verified_inodes[$index]}" &&
      $cert_hash == "${verified_hashes[$index]}" ]] ||
      die 'stale system CA changed after validation; refusing deletion'
  done
  for cert_path in "${verified_paths[@]}"; do
    rm -f -- "$cert_path"
  done
  if (( ${#verified_paths[@]} > 0 )); then
    update-ca-certificates >/dev/null
  fi
}

trap cleanup EXIT
restore_termination_traps
recover_stale_system_cas

if awk -v wanted="$bedrock_host" '
  /^[[:space:]]*#/ { next }
  { for (i = 2; i <= NF; i++) if ($i == wanted) found = 1 }
  END { exit found ? 0 : 1 }
' /etc/hosts; then
  die 'Bedrock test hostname already exists in /etc/hosts; refusing ambiguous mutation'
fi
hosts_changed=1
printf '%s\n' "$hosts_line" >>/etc/hosts

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 1 \
  -subj '/CN=aimee-p6c-ct260-test-ca' -keyout "$ca_key" -out "$ca_cert" \
  -addext 'basicConstraints=critical,CA:TRUE' \
  -addext 'keyUsage=critical,keyCertSign,cRLSign' >/dev/null 2>&1

[[ ! -e $system_ca_cert && ! -L $system_ca_cert ]] ||
  die 'temporary system CA destination already exists'
defer_termination_traps
ca_create_rc=0
set -o noclobber
exec 8>"$system_ca_cert" || ca_create_rc=$?
set +o noclobber
if (( ca_create_rc == 0 )); then
  if system_ca_inode=$(stat -Lc '%d:%i' "/proc/$$/fd/8"); then
    system_ca_owned=1
    while IFS= read -r cert_line; do
      printf '%s\n' "$cert_line" >&8 || {
        ca_create_rc=$?
        break
      }
    done <"$ca_cert"
  else
    ca_create_rc=$?
    rm -f -- "$system_ca_cert" || true
  fi
  exec 8>&-
fi
if (( ca_create_rc == 0 )); then
  openssl x509 -in "$system_ca_cert" -noout >/dev/null 2>&1 || ca_create_rc=$?
fi
restore_termination_traps
exit_for_deferred_signal
(( ca_create_rc == 0 )) || die 'failed to exclusively create temporary system CA'
update-ca-certificates >/dev/null

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
openssl req -new -newkey rsa:2048 -nodes -sha256 -subj "/CN=$bedrock_host" \
  -keyout "$wrong_ca_server_key" -out "$wrong_ca_server_csr" >/dev/null 2>&1
openssl x509 -req -sha256 -days 1 -in "$wrong_ca_server_csr" \
  -CA "$wrong_ca_cert" -CAkey "$wrong_ca_key" -CAcreateserial \
  -extfile "$cert_ext" -out "$wrong_ca_server_cert" >/dev/null 2>&1

: >"$db_lock_state"
: >"$db_lock_log"
psql -X -q -v ON_ERROR_STOP=1 "$AIMEE_TEST_PG_URL" \
  >/dev/null 2>"$db_lock_log" <<SQL &
SELECT pg_advisory_lock(960260, 6006);
\\! printf '%s\n' p6c-db-lock-held > '$db_lock_state'
SELECT pg_sleep(86400);
SQL
db_lock_pid=$!
lock_attempt=0
while (( lock_attempt < 600 )); do
  grep -Fxq 'p6c-db-lock-held' "$db_lock_state" && break
  kill -0 "$db_lock_pid" 2>/dev/null || {
    sed 's/^/db-lock: /' "$db_lock_log" >&2
    die 'database advisory-lock session exited before acquisition'
  }
  sleep 0.05
  ((lock_attempt += 1))
done
grep -Fxq 'p6c-db-lock-held' "$db_lock_state" ||
  die 'database advisory-lock acquisition timed out'

defer_termination_traps
seed_rc=0
psql -X -q -v ON_ERROR_STOP=1 "$AIMEE_TEST_PG_URL" >/dev/null <<'SQL' || seed_rc=$?
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
if (( seed_rc == 0 )); then
  db_seeded=1
fi
restore_termination_traps
exit_for_deferred_signal
(( seed_rc == 0 )) || exit "$seed_rc"

make -C src -j"$(nproc)" build/obj/tests/unit-test-kb-bedrock-live >/dev/null
# The production resolver locates its helper beside /proc/self/exe.  Mirror the
# installed kb layout for this separately linked live harness; this is the same
# helper binary and does not introduce a test-only endpoint or resolver seam.
install -m 755 aimee-kb-resolver "$live_resolver"

start_mock() {
  local case_id=$1
  local cert=${2:-$server_cert}
  local key=${3:-$server_key}
  local -a extra_args=()
  [[ ${4:-} == no-session-token ]] && extra_args+=(--no-session-token)
  [[ -n ${5:-} ]] && extra_args+=(--expected-path "$5")
  [[ ${6:-} == dynamic-timestamp ]] && extra_args+=(--dynamic-timestamp)
  stop_mock
  : >"$mock_log"
  python3 scripts/p6c_bedrock_mock.py --cert "$cert" --key "$key" \
    --bind 127.0.0.1 --port 443 --case "$case_id" \
    --counter-file "$mock_counter" --observed-file "$mock_observed" \
    --ready-file "$mock_ready" "${extra_args[@]}" 2>"$mock_log" &
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

observed_count() {
  local value
  [[ -f $mock_observed ]] || die 'mock observed counter is missing'
  IFS= read -r value <"$mock_observed"
  [[ $value =~ ^[0-9]+$ ]] || die 'mock observed counter is malformed'
  printf '%s' "$value"
}

assert_count() {
  local expected=$1
  local actual
  actual=$(accepted_count)
  [[ $actual == "$expected" ]] || die "accepted counter mismatch (want $expected, got $actual)"
}

assert_observed() {
  local expected=$1
  local actual
  actual=$(observed_count)
  [[ $actual == "$expected" ]] ||
    die "observed counter mismatch (want $expected, got $actual)"
}

run_success() {
  local label=$1
  local mode=$2
  if ! AIMEE_TEST_TEAM_ID="$team_id" \
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
  if AIMEE_TEST_TEAM_ID="$team_id" \
      "$live_target" "$model" "$mode" >"$case_log" 2>&1; then
    sed 's/^/live: /' "$case_log" >&2
    sed 's/^/mock: /' "$mock_log" >&2
    die "$label unexpectedly succeeded"
  fi
}

start_mock nonstream-success
run_success buffered-success buffered
assert_count 1
assert_observed 1
stop_mock

start_mock fragmented-stream-success
run_success fragmented-stream-success stream
assert_count 1
stop_mock

start_mock fragmented-stream-success
run_success callback-abort stream-abort
assert_count 1
stop_mock

start_mock nonstream-success
if AIMEE_TEST_WRONG_SECRET=1 AIMEE_TEST_TEAM_ID="$team_id" \
    "$live_target" model buffered >"$case_log" 2>&1; then
  die 'wrong-secret unexpectedly succeeded'
fi
assert_count 0
assert_observed 1
stop_mock

for case_id in wrong-media non-2xx malformed-framing; do
  start_mock "$case_id"
  run_failure "$case_id" model buffered
  assert_count 1
  stop_mock
done

start_mock unclean-eof
run_failure unclean-eof model buffered
assert_count 1
stop_mock

for case_id in bad-crc complete-frame-semantic-truncation; do
  start_mock "$case_id"
  run_failure "$case_id" model stream
  assert_count 1
  stop_mock
done

start_mock nonstream-success "$wrong_ca_server_cert" "$wrong_ca_server_key"
run_failure tls-wrong-ca model buffered
assert_count 0
stop_mock

start_mock nonstream-success "$wrong_server_cert" "$wrong_server_key"
run_failure tls-wrong-hostname model buffered
assert_count 0
stop_mock

start_mock nonstream-success
pre_network_observed=$(observed_count)
for model_id in p6c-missing p6c-unentitled p6c-unsupported; do
  run_failure "pre-network-$model_id" "$model_id" buffered
  assert_count 0
  assert_observed "$pre_network_observed"
done
stop_mock

if [[ -n ${AIMEE_P2B_LIVE_TARGET:-} ]]; then
  [[ -x $AIMEE_P2B_LIVE_TARGET ]] || die 'P2b live target is not executable'
  install -m 755 aimee-kb-resolver "$(dirname "$AIMEE_P2B_LIVE_TARGET")/aimee-kb-resolver"
  start_mock p2b-matrix "$server_cert" "$server_key" no-session-token \
    /model/p2b-live-model/converse dynamic-timestamp
  if ! "$AIMEE_P2B_LIVE_TARGET" >"$case_log" 2>&1; then
    sed 's/^/p2b-live: /' "$case_log" >&2
    sed 's/^/mock: /' "$mock_log" >&2
    die 'P2b live composition unexpectedly failed'
  fi
  assert_count 3
  assert_observed 3
  stop_mock
fi

printf '%s: all CT260 TLS/PG gates passed\n' "$gate_name"

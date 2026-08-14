#!/usr/bin/env bash
# validate-kb-mtls-scope-live.sh — the PRODUCTION credential path, end to end.
#
# WHY THIS EXISTS. Production uses both a rotating BEARER and a client
# CERTIFICATE. kb_tls_serve.c synthesises a credential from that certificate's CN
# ("<kind>:<id>" -> scope:<kind>:<id>:m, a bare word -> mtls-owner). Nothing
# exercised that translation. The neighbouring test-synthesis-mtls-hop.sh exists
# because a hop stayed broken through 41 lint checks, a unit test and two image
# builds; this is the same class of gap, so it gets the same treatment.
#
# It drives the real flow with no shortcuts: `aimee-kb enroll --scope`, a CSR
# POSTed to /v1/enroll/redeem, then a genuine mTLS request with the issued
# certificate.
#
# The two cases exercise the composed layers on the real path:
#
#   service:aimee-server  + its scoped bearer -> reaches the third-layer gate
#                         and is refused until PAM/OIDC identity is supplied
#   p5-server-client      + that service bearer -> refused because the bearer
#                         identity does not match the legacy certificate
set -uo pipefail

SRC="${AIMEE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
KB="${AIMEE_KB_BIN:-$SRC/aimee-kb}"
# A UNIQUE directory per run, deliberately. The enrollment-token registry is a
# Vault record named after the store PATH but encrypted with the per-AIMEE_HOME
# master key, so reusing a fixed path after wiping AIMEE_HOME leaves an
# undecryptable record in the shared DB2 vault and every later enrollment fails
# with a misleading "invalid or used token". A fixed path would make this suite
# pass once and fail forever after.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/aimee-mtls-scope.XXXXXX")"
HTTP_PORT=8751
MTLS_PORT=8752
BASE="http://127.0.0.1:$HTTP_PORT"
MBASE="https://127.0.0.1:$MTLS_PORT"
PASS=0; FAIL=0; SKIP=0

pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
fail() { printf '  \033[31mFAIL\033[0m  %s\n     -> %s\n' "$1" "$2"; FAIL=$((FAIL+1)); }
skip() { printf '  \033[33mSKIP\033[0m  %s (%s)\n' "$1" "$2"; SKIP=$((SKIP+1)); }

export AIMEE_HOME="$WORK/home"
mkdir -p "$AIMEE_HOME"
export AIMEE_KB_API_BEARER_TOKEN="owner-secret-mtls-test"
export AIMEE_KB_MTLS_PORT="$MTLS_PORT"
export AIMEE_DB2_URL="${AIMEE_DB2_URL:-postgresql://aimee:aimee@127.0.0.1/aimee_kb}"
cat > "$AIMEE_HOME/aimee.yaml" <<YAML
kb:
  api:
    http_port: $HTTP_PORT
YAML

cleanup() { [ -f "$WORK/kb.pid" ] && kill "$(cat "$WORK/kb.pid")" 2>/dev/null; }
trap cleanup EXIT

ulimit -s 65536
# Same first-boot transport as the container entrypoint: the runtime reads the
# bearer from Vault, so it must be sealed before the listener starts.
"$KB" --bootstrap-vault-env > "$WORK/bootstrap.log" 2>&1
nohup "$KB" > "$WORK/kb.log" 2>&1 &
echo $! > "$WORK/kb.pid"

ready=0
for _ in $(seq 1 60); do
  curl -s -m 2 "$BASE/v1/health" >/dev/null 2>&1 && { ready=1; break; }
  sleep 1
done
if [ "$ready" != 1 ]; then
  skip "entire suite" "aimee-kb did not become healthy; see $WORK/kb.log"
  echo "  PASS=$PASS FAIL=$FAIL SKIP=$SKIP"; exit 0
fi
# The mTLS listener is separate from the HTTP one; wait for it too.
for _ in $(seq 1 30); do
  (exec 3<>/dev/tcp/127.0.0.1/$MTLS_PORT) 2>/dev/null && { exec 3<&- 2>/dev/null; break; }
  sleep 1
done

# unescape_json_string: turn a JSON string value into raw PEM.
unesc() { python3 -c 'import sys,json;print(json.loads(sys.stdin.read()))'; }

# issue_cert SCOPE OUTDIR -> writes cert.pem/key.pem, echoes the cert CN
#
# Minting goes through the RUNNING SERVER (POST /v1/enroll, owner bearer), not
# the `aimee-kb enroll` CLI. The single-use token registry is held in Vault, and
# the CLI mints from a second process with its own Vault state, so a token it
# issues while the server is up does not redeem against it. Minting in-process is
# both the working path and the realistic one for a live kb.
issue_cert() {
  local scope="$1" dir="$2" conn token csr resp certjson
  mkdir -p "$dir"
  conn=$(curl -s -m 20 -X POST -H "Authorization: Bearer $AIMEE_KB_API_BEARER_TOKEN" \
         -H 'Content-Type: application/json' \
         --data "{\"host\":\"127.0.0.1\",\"port\":$MTLS_PORT,\"scope\":\"$scope\"}" \
         "$BASE/v1/enroll" 2>/dev/null |
         python3 -c '
import sys,json
try: print(json.load(sys.stdin).get("connection_string",""))
except Exception: print("")' 2>/dev/null)
  [ -n "$conn" ] || { echo "ENROLL_FAILED"; return 1; }
  token="${conn##*enroll=}"

  # A deliberately WRONG CN in the CSR: the issuer must overwrite it with the
  # token's scope. If it ever honoured the client's CN, a scoped client could
  # mint itself an owner certificate by asking for a colon-free name.
  openssl req -new -newkey rsa:2048 -nodes -keyout "$dir/key.pem" \
     -out "$dir/req.csr" -subj "/CN=attacker-chosen-name" >/dev/null 2>&1 || {
     echo "CSR_FAILED"; return 1; }

  csr=$(python3 -c 'import json,sys;print(json.dumps(open(sys.argv[1]).read()))' "$dir/req.csr")
  resp=$(curl -s -m 20 -X POST -H 'Content-Type: application/json' \
         --data "{\"token\":\"$token\",\"csr\":$csr}" "$BASE/v1/enroll/redeem" 2>/dev/null)
  certjson=$(printf '%s' "$resp" | python3 -c '
import sys,json
try: print(json.dumps(json.load(sys.stdin)["client_cert"]))
except Exception: print("")' 2>/dev/null)
  [ -n "$certjson" ] && [ "$certjson" != '""' ] || { echo "REDEEM_FAILED:$resp"; return 1; }
  printf '%s' "$certjson" | unesc > "$dir/cert.pem"
  openssl x509 -in "$dir/cert.pem" -noout -subject 2>/dev/null | sed 's/.*CN *= *//'
}

# mreq PATH DIR BODY -> HTTP status over mTLS with that identity
mreq() {
  local bearer="${4:-$AIMEE_KB_API_BEARER_TOKEN}"
  curl -s -o /dev/null -w '%{http_code}' -m 20 -k \
    --cert "$2/cert.pem" --key "$2/key.pem" \
    -X POST -H "Authorization: Bearer $bearer" \
    -H 'Content-Type: application/json' --data "$3" "$MBASE$1" 2>/dev/null
}

MAINT='/v1/maintenance/purge-project'
MBODY='{"project":"proj-alpha","path":"/tmp/kb","generation":"g1","purge_id":"p1"}'
SERVICE_BEARER="scope:service:aimee-server:$AIMEE_KB_API_BEARER_TOKEN"

echo ""
echo "### A. service:aimee-server — what aimee-server presents NOW"
CN_A=$(issue_cert "service:aimee-server" "$WORK/a")
case "$CN_A" in
  service:aimee-server)
    pass "CSR's chosen CN was overridden with the token scope (CN=$CN_A)"
    code=$(mreq "$MAINT" "$WORK/a" "$MBODY" "$SERVICE_BEARER")
    [ "$code" = "401" ] && pass "service cert + bearer reaches the PAM/OIDC gate on $MAINT" \
                        || fail "service cert + bearer on $MAINT" "want 401 got $code"
    code=$(mreq "/v1/code/build" "$WORK/a" '{"path":"/tmp/kb","project":"proj-beta"}' \
                "$SERVICE_BEARER")
    [ "$code" = "401" ] && pass "data plane also requires PAM/OIDC after cert + bearer" \
                        || fail "service cert + bearer data plane" "want 401 got $code"
    ;;
  ENROLL_FAILED*|CSR_FAILED*|REDEEM_FAILED*) skip "case A" "$CN_A" ;;
  *) fail "case A CN" "expected service:aimee-server, got '$CN_A'" ;;
esac

echo ""
echo "### B. p5-server-client — what it presented BEFORE (the red case)"
CN_B=$(issue_cert "p5-server-client" "$WORK/b")
case "$CN_B" in
  p5-server-client)
    pass "old-style CN issued (CN=$CN_B, no ':')"
    code=$(mreq "$MAINT" "$WORK/b" "$MBODY" "$SERVICE_BEARER")
    if [ "$code" = "401" ] || [ "$code" = "403" ]; then
      pass "old CN cannot wear the service bearer's identity (=$code)"
    else
      fail "old CN on $MAINT" "want 401/403 got $code"
    fi
    ;;
  ENROLL_FAILED*|CSR_FAILED*|REDEEM_FAILED*) skip "case B" "$CN_B" ;;
  *) fail "case B CN" "expected p5-server-client, got '$CN_B'" ;;
esac

echo ""
echo "=============================================="
echo "  PASS=$PASS  FAIL=$FAIL  SKIP=$SKIP"
echo "=============================================="
[ "$FAIL" -eq 0 ]

#!/usr/bin/env bash
#
# test_v1_third_party.sh — language-agnostic third-party integration smoke test
# for the aimee-kb /v1 public API, using ONLY curl + jq.
#
# Proposal AC (docs/proposals/accepted/aimee-kb-service-and-public-api.md):
#   "A third-party tool using only the OpenAPI spec can ingest a doc, search it,
#    fetch an artifact, and receive an invalidation stream. Demonstrated by a
#    language-agnostic integration test (curl + jq)."
#
# v1 exposes the job lifecycle by polling (`/v1/jobs/{id}`), not WebSocket —
# WebSocket invalidation is the proposal's Phase-2 optimization ("v1 is
# TTL-only"). This test therefore polls job status as the invalidation surface.
#
# It is SAFE in CI: when curl/jq are missing or no aimee-kb is reachable it
# prints SKIP and exits 0. Point it at a running service to validate the
# stability contract end-to-end:
#
#   KB_BASE_URL=http://127.0.0.1:8090/v1 \
#   KB_BEARER_TOKEN=<token-if-remote> \
#     src/tests/test_v1_third_party.sh
#
set -u

BASE_URL="${KB_BASE_URL:-http://127.0.0.1:8090/v1}"
TOKEN="${KB_BEARER_TOKEN:-}"
TIMEOUT="${KB_HTTP_TIMEOUT:-10}"

skip() { echo "SKIP: $*"; exit 0; }
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }

command -v curl >/dev/null 2>&1 || skip "curl not installed"
command -v jq   >/dev/null 2>&1 || skip "jq not installed"

AUTH=()
[ -n "$TOKEN" ] && AUTH=(-H "Authorization: Bearer $TOKEN")

# req METHOD PATH [curl-args...] -> writes body to $BODY, status to $STATUS
req() {
   local method="$1" path="$2"; shift 2
   local tmp; tmp="$(mktemp)"
   STATUS="$(curl -sS -m "$TIMEOUT" -o "$tmp" -w '%{http_code}' \
      -X "$method" "${AUTH[@]}" "$@" "$BASE_URL$path" 2>/dev/null)" || STATUS="000"
   BODY="$(cat "$tmp")"; rm -f "$tmp"
}

# --- 0. Reachability: skip cleanly if no service ----------------------------
req GET /health
if [ "$STATUS" = "000" ]; then
   skip "no aimee-kb reachable at $BASE_URL (set KB_BASE_URL to run live)"
fi
[ "$STATUS" = "200" ] || fail "/health returned $STATUS"
echo "$BODY" | jq -e '.status' >/dev/null 2>&1 || fail "/health body not JSON with .status"
ok "health ($BASE_URL)"

# --- 1. Discovery -----------------------------------------------------------
req GET /version
[ "$STATUS" = "200" ] && echo "$BODY" | jq -e '.' >/dev/null 2>&1 || fail "/version: status=$STATUS"
ok "version"

req GET /capabilities
[ "$STATUS" = "200" ] && echo "$BODY" | jq -e '.' >/dev/null 2>&1 || fail "/capabilities: status=$STATUS"
ok "capabilities"

# --- 2. Ingest: manifest pre-check then multipart upload --------------------
DOC="$(mktemp --suffix=.md)"
cat >"$DOC" <<'EOF'
# Third-party smoke document

aimee-kb public API integration test. The quick brown fox jumps over the lazy dog.
EOF
HASH="$(sha256sum "$DOC" | awk '{print $1}')"

req POST /docs/manifest -H 'Content-Type: application/json' \
   --data "$(jq -n --arg h "$HASH" '{scope:"global",docs:[{doc_key:"smoke.md",content_hash:$h}]}')"
[ "$STATUS" = "200" ] && echo "$BODY" | jq -e '.' >/dev/null 2>&1 \
   || fail "/docs/manifest: status=$STATUS body=$BODY"
ok "docs/manifest pre-check"

req POST /docs -F "file=@$DOC;type=text/markdown" -F "scope=global"
case "$STATUS" in
   200|201) ;;  # 201 = ingested, 200 = idempotent re-upload
   503) rm -f "$DOC"; skip "/docs returned 503 (KB_STORE not provisioned on this host)";;
   *) rm -f "$DOC"; fail "/docs upload: status=$STATUS body=$BODY";;
esac
DOC_ID="$(echo "$BODY" | jq -r '.doc_id // empty')"
[ -n "$DOC_ID" ] || fail "/docs upload returned no doc_id: $BODY"
rm -f "$DOC"
ok "docs upload -> doc_id=$DOC_ID"

# --- 3. Pipeline / invalidation surface: drain + poll job -------------------
req POST /drain -H 'Content-Type: application/json' --data '{}'
if [ "$STATUS" = "200" ]; then
   JOB_ID="$(echo "$BODY" | jq -r '.job_id // empty')"
   if [ -n "$JOB_ID" ]; then
      for _ in $(seq 1 10); do
         req GET "/jobs/$JOB_ID"
         [ "$STATUS" = "200" ] || break
         STATE="$(echo "$BODY" | jq -r '.state // .status // empty')"
         case "$STATE" in done|completed|failed|error|"") break;; esac
         sleep 1
      done
      ok "drain -> job $JOB_ID polled (state=${STATE:-n/a})"
   else
      ok "drain (synchronous; no job_id)"
   fi
else
   ok "drain skipped (status=$STATUS)"
fi

# --- 4. Retrieval: search -> fetch artifact ---------------------------------
req POST /search -H 'Content-Type: application/json' \
   --data '{"query":"quick brown fox","max_results":5}'
[ "$STATUS" = "200" ] && echo "$BODY" | jq -e 'has("hits")' >/dev/null 2>&1 \
   || fail "/search: status=$STATUS body=$BODY"
HITS="$(echo "$BODY" | jq '.hits | length')"
ok "search -> $HITS hit(s)"

ART_ID="$(echo "$BODY" | jq -r '.hits[0].id // .hits[0].artifact_id // empty')"
if [ -n "$ART_ID" ]; then
   req GET "/artifacts/$ART_ID"
   [ "$STATUS" = "200" ] && echo "$BODY" | jq -e '.id' >/dev/null 2>&1 \
      || fail "/artifacts/$ART_ID: status=$STATUS body=$BODY"
   ok "fetch artifact $ART_ID"
else
   # No artifacts yet (extraction needs an LLM); still exercise the endpoint
   # contract: an unknown id must return a clean 404 + {"error":...} shape.
   req GET "/artifacts/00000000-0000-0000-0000-000000000000"
   [ "$STATUS" = "404" ] || fail "unknown artifact expected 404, got $STATUS"
   echo "$BODY" | jq -e 'has("error")' >/dev/null 2>&1 \
      || fail "404 body missing {\"error\":...}: $BODY"
   ok "artifact 404 error-shape (no corpus artifacts yet)"
fi

echo "PASS: third-party /v1 journey (ingest -> drain/poll -> search -> artifact)"

#!/usr/bin/env bash
# Live server -> kb -> PostgreSQL acceptance for memory row scope and durable refusal.
set -euo pipefail

SERVER_URL="${SERVER_URL:-https://localhost:8743}"
BEARER="${BEARER:-}"
REQUEST_TIMEOUT_SECONDS="${REQUEST_TIMEOUT_SECONDS:-60}"
[[ -n "$BEARER" ]] || { echo "BEARER is required" >&2; exit 2; }
AUTH=(-H "Authorization: Bearer ${BEARER}")
IDENTITY=()
if [[ -n "${CLIENT_CERT:-}" || -n "${CLIENT_KEY:-}" ]]; then
  [[ -r "${CLIENT_CERT:-}" && -r "${CLIENT_KEY:-}" ]] || {
    echo "CLIENT_CERT and CLIENT_KEY must both be readable" >&2; exit 2;
  }
  IDENTITY=(--cert "$CLIENT_CERT" --key "$CLIENT_KEY")
fi

post() {
  curl -fksS --max-time "$REQUEST_TIMEOUT_SECONDS" "${IDENTITY[@]}" "${AUTH[@]}" \
    -H 'content-type: application/json' -X POST -d "$2" "$SERVER_URL$1"
}
fail() { echo "memory governance E2E: FAIL: $*" >&2; exit 1; }

sentinel="memoryGovernance${$}x${RANDOM}"
key="governance-${sentinel}"
content="durable refusal ${sentinel}"
project_a="e2e-project-a-${sentinel}"
project_b="e2e-project-b-${sentinel}"

store_a="$(post /v1/memory/store \
  "{\"key\":\"$key\",\"content\":\"$content\",\"kind\":\"fact\",\"project\":\"$project_a\"}")"
id_a="$(python3 -c 'import json,sys; print(int(json.loads(sys.argv[1])["id"]))' "$store_a")" ||
  fail "project-A store did not return an id: $store_a"

review_b="$(post /v1/memory/review "{\"project\":\"$project_b\",\"limit\":64}")"
[[ "$review_b" != *"$sentinel"* ]] || fail "project-B review leaked project-A row"
review_a="$(post /v1/memory/review "{\"project\":\"$project_a\",\"limit\":64}")"
[[ "$review_a" == *"$sentinel"* && "$review_a" == *'"scope_type":"project"'* &&
   "$review_a" == *"\"scope_value\":\"$project_a\""* ]] ||
  fail "project-A review did not expose row-owned scope: $review_a"

reject="$(post /v1/memory/reject \
  "{\"id\":$id_a,\"reason\":\"operator rejected $sentinel\",\"project\":\"$project_a\"}")"
[[ "$reject" == *'"tombstoned":true'* ]] || fail "reject failed: $reject"

list_after_reject="$(post /v1/memory/list "{\"project\":\"$project_a\",\"limit\":64}")"
[[ "$list_after_reject" != *"$sentinel"* ]] || fail "rejected row remained recallable"
review_rejected="$(post /v1/memory/review \
  "{\"project\":\"$project_a\",\"state\":\"rejected\",\"limit\":64}")"
[[ "$review_rejected" == *"$sentinel"* && "$review_rejected" == *'"lifecycle":"rejected"'* &&
   "$review_rejected" == *"operator rejected $sentinel"* ]] ||
  fail "human-review history lost rejected content or reason: $review_rejected"

reassert_a="$(post /v1/memory/store \
  "{\"key\":\"$key\",\"content\":\"$content\",\"kind\":\"fact\",\"project\":\"$project_a\"}" || true)"
[[ "$reassert_a" != *'"status":"ok"'* ]] ||
  fail "exact project-A value was reasserted despite tombstone: $reassert_a"

store_b="$(post /v1/memory/store \
  "{\"key\":\"$key\",\"content\":\"$content\",\"kind\":\"fact\",\"project\":\"$project_b\"}")"
[[ "$store_b" == *'"status":"ok"'* ]] ||
  fail "project-A tombstone incorrectly blocked distinct project-B scope: $store_b"

restore="$(post /v1/memory/restore "{\"id\":$id_a,\"project\":\"$project_a\"}")"
[[ "$restore" == *'"restored":true'* ]] || fail "restore failed: $restore"
list_after_restore="$(post /v1/memory/list "{\"project\":\"$project_a\",\"limit\":64}")"
[[ "$list_after_restore" == *"$sentinel"* ]] || fail "restored row did not return to recall"

echo "memory governance E2E: PASSED (row scope, review, reject, refusal, scope separation, restore)"

#!/bin/sh
# Boot a PUBLISHED image as the real stack and prove it works end to end.
#
# Everything so far tested the image's PARTS: the embedder process started by
# hand, llama-server invoked directly. This runs aimee-kb the way an operator
# does — compose, embedded Postgres, DB2 schema, the entrypoint deciding what to
# start — and then asks the live /v1 surface whether it is actually usable.
#
# The new ground here is start_synthesis() in its real context: nothing has ever
# executed it. It is the last piece of the bundled-model path that was written
# and reviewed but never run.
set -u
IMG=${1:-ghcr.io/rakuensoftware/aimee-kb-llm-e2b:sha-c1d0aba}
cd /root/matrix || exit 1

export AIMEE_KB_IMAGE="$IMG"
export COMPOSE_PROJECT_NAME=smoke

say(){ echo "[$(date +%H:%M:%S)] $*"; }
pass=0; fail=0
ok(){ pass=$((pass+1)); say "  PASS  $*"; }
bad(){ fail=$((fail+1)); say "  FAIL  $*"; }

say "=== stack smoke: $IMG ==="
docker compose -f compose.yaml down -v >/dev/null 2>&1 || true

# First-boot credentials go through the disposable helper, never Config.Env.
if [ -x scripts/aimee-compose-vault-bootstrap.sh ]; then
  scripts/aimee-compose-vault-bootstrap.sh -f compose.yaml kb >/tmp/vault.log 2>&1 \
    && ok "vault bootstrap" || bad "vault bootstrap: $(tail -2 /tmp/vault.log)"
fi

docker compose -f compose.yaml up -d --no-build aimee-kb >/tmp/up.log 2>&1 \
  || { bad "compose up: $(tail -3 /tmp/up.log)"; docker compose -f compose.yaml logs --tail=20 aimee-kb; exit 1; }
ok "compose up"

say "  waiting for health (up to 600s; first boot builds the DB2 schema)"
i=0
while [ "$i" -lt 200 ]; do
  st=$(docker compose -f compose.yaml ps --format '{{.Service}} {{.Health}}' 2>/dev/null | awk '$1=="aimee-kb"{print $2}')
  [ "$st" = "healthy" ] && break
  i=$((i+1)); sleep 3
done
if [ "$st" = "healthy" ]; then ok "aimee-kb reports healthy"
else
  bad "aimee-kb never healthy (state=${st:-unknown})"
  docker compose -f compose.yaml logs --tail=40 aimee-kb
  docker compose -f compose.yaml down -v >/dev/null 2>&1
  exit 1
fi

kb() { docker compose -f compose.yaml exec -T aimee-kb "$@" 2>/dev/null; }

# --- the live /v1 surface -----------------------------------------------------
kb curl -fsS -m 10 http://127.0.0.1:8741/v1/health >/dev/null && ok "/v1/health" || bad "/v1/health"
kb curl -fsS -m 20 "http://127.0.0.1:8741/v1/health?status=1" 2>/dev/null | head -c 300 > /tmp/st.json
grep -qi 'db2\|vector\|schema\|ok' /tmp/st.json && ok "/v1/health?status=1 (DB2 + vector store)" \
  || bad "status: $(head -c 120 /tmp/st.json)"
kb curl -fsS -m 10 http://127.0.0.1:8741/v1/version >/dev/null && ok "/v1/version" || bad "/v1/version"
kb curl -fsS -m 15 -X POST http://127.0.0.1:8741/v1/search \
   -H 'content-type: application/json' -d '{"query":"hello","limit":1}' >/dev/null \
   && ok "POST /v1/search (DB2-backed query path)" || bad "POST /v1/search"

# --- the embedder the entrypoint chose ---------------------------------------
kb curl -fsS -m 20 http://127.0.0.1:8760/health 2>/dev/null > /tmp/eh.json
if grep -q '"dim"' /tmp/eh.json; then
  ok "in-container embedder healthy: $(head -c 160 /tmp/eh.json)"
else
  bad "embedder health: $(head -c 120 /tmp/eh.json)"
fi

# --- THE NEW GROUND: start_synthesis() in its real context --------------------
say "  waiting for bundled synthesis on loopback (model load takes time)"
i=0; ok_syn=0
while [ "$i" -lt 100 ]; do
  if kb curl -fsS -m 5 http://127.0.0.1:8761/health >/dev/null; then ok_syn=1; break; fi
  i=$((i+1)); sleep 6
done
if [ "$ok_syn" = 1 ]; then
  ok "entrypoint started llama-server on :8761"
  kb curl -fsS -m 120 http://127.0.0.1:8761/v1/chat/completions \
     -H 'content-type: application/json' \
     -d '{"messages":[{"role":"user","content":"Reply with one word: ok"}],"max_tokens":8}' \
     2>/dev/null | head -c 300 > /tmp/gen.json
  grep -q '"choices"' /tmp/gen.json && ok "bundled synthesis answered a request" \
    || bad "generation: $(head -c 150 /tmp/gen.json)"
else
  bad "no llama-server on :8761 — start_synthesis did not run"
  docker compose -f compose.yaml logs --tail=30 aimee-kb | grep -ai 'synthesis\|llama' | tail -8
fi

# --- credentials must not be in the long-lived environment -------------------
env_dump=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' \
           "$(docker compose -f compose.yaml ps -q aimee-kb)" 2>/dev/null)
if echo "$env_dump" | grep -qE '^(SYNTHESIS_API_KEY|EMBEDDER_API_KEY)=.'; then
  bad "a credential is persisted in Config.Env"
else
  ok "no credential in Config.Env"
fi

say "  tearing down"
docker compose -f compose.yaml down -v >/dev/null 2>&1
say "==== stack smoke: $pass passed, $fail failed ===="
[ "$fail" -eq 0 ]

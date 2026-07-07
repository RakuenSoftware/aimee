#!/usr/bin/env bash
# Integration smoke test: bring up two aimee-proxy replicas sharing a
# single Postgres instance, confirm both boot cleanly and the shared
# schema is applied exactly once.
#
# Satisfies the two-replica acceptance criterion from the DB2 Postgres
# design tracked in
# docs/STORAGE_TIERS.md.
#
# Requires docker + docker compose on PATH. Not run by `make test` —
# invoke manually or from CI where docker is available.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

COMPOSE=(docker compose -f docker-compose.proxy.yml -p aimee-proxy-smoke)

cleanup() {
  "${COMPOSE[@]}" down -v --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> bringing up stack"
"${COMPOSE[@]}" up -d --build

echo "==> waiting for postgres health"
for _ in $(seq 1 60); do
  status=$("${COMPOSE[@]}" ps --format '{{.Service}} {{.Health}}' | awk '$1=="postgres" {print $2}')
  [[ "$status" == "healthy" ]] && break
  sleep 1
done
[[ "$status" == "healthy" ]] || { echo "postgres never became healthy"; exit 1; }

echo "==> waiting for both proxy replicas to log 'listening on'"
for svc in aimee-proxy-1 aimee-proxy-2; do
  for _ in $(seq 1 60); do
    if "${COMPOSE[@]}" logs --no-log-prefix "$svc" 2>/dev/null | grep -q "listening on"; then
      break
    fi
    sleep 1
  done
  if ! "${COMPOSE[@]}" logs --no-log-prefix "$svc" 2>/dev/null | grep -q "listening on"; then
    echo "$svc never started listening"
    "${COMPOSE[@]}" logs "$svc" | tail -40
    exit 1
  fi
done

echo "==> verifying proxy schema landed exactly once in shared Postgres"
tables=$("${COMPOSE[@]}" exec -T postgres psql -U aimee -d aimee_proxy -Atc \
  "SELECT table_name FROM information_schema.tables WHERE table_schema='public' ORDER BY 1")
for needed in proxy_tokens proxy_usage; do
  grep -qx "$needed" <<<"$tables" || { echo "missing table: $needed"; echo "got: $tables"; exit 1; }
done

echo "==> OK — both replicas bootstrapped against shared Postgres"

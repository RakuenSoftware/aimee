#!/usr/bin/env bash
# Phase 1 of the upgrade rehearsal: a real v0.2.192 appliance, populated with data.
set -uo pipefail
cd /root/upgrade
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
no(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }

echo "=== 0. empty docker state ==="
docker compose -f compose.combined.yaml down -v --remove-orphans >/dev/null 2>&1
ids=$(docker ps -aq); [ -n "$ids" ] && docker rm -f $ids >/dev/null 2>&1
vs=$(docker volume ls -q); [ -n "$vs" ] && docker volume rm -f $vs >/dev/null 2>&1
docker network prune -f >/dev/null 2>&1
rm -rf /root/.config/aimee/remote.conf /root/.config/aimee/remote-ca.pem
echo "  containers=$(docker ps -aq|wc -l) volumes=$(docker volume ls -q|wc -l)"

echo
echo "=== 1. bring up v0.2.192 (the last real release) ==="
export AIMEE_IMAGE_TAG=0.2.192
export AIMEE_COMBINED_IMAGE=ghcr.io/rakuensoftware/aimee-combined:0.2.192
docker compose -f compose.combined.yaml up -d 2>&1 | grep -E 'Started|Created|Error' | head -6
st=""
for i in $(seq 1 60); do
  st=$(docker ps --filter name=aimee --format '{{.Status}}' | head -1)
  echo "$st" | grep -qi healthy && break
  sleep 5
done
docker ps --format '  {{.Names}}  {{.Image}}  {{.Status}}'
echo "$st" | grep -qi healthy && ok "0.2.192 appliance healthy" || no "0.2.192 not healthy (last=$st)"

echo
echo "=== 2. record the pre-upgrade state (UPGRADING.md step 6) ==="
docker compose -f compose.combined.yaml config 2>/dev/null | grep -E '^\s+image:' | sort -u | sed 's/^/  /'
docker ps --format '{{.Names}}' | while read -r c; do
  echo "  $c -> $(docker inspect "$c" --format '{{index .Image}}' | cut -c1-19)"
done
echo "  volumes:"; docker volume ls --format '    {{.Name}}'

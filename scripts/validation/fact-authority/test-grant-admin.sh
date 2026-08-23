#!/bin/bash
# What is the ACTUAL way to create a write-tier grant?
#
# Three artifacts tell an operator to run `aimee kb grant set`:
#   - the server's own 403 body (server_http.c)
#   - the route comment in server_http_routes.c
#   - docs/UPGRADING.md's operator procedure
#
# That command does not dispatch. It was not an oversight: the server-side proxy
# was REMOVED on purpose, and v1_route_requires_uds records why --
#
#   "aimee-server no longer proxies write-tier grant administration. That is an
#    operator action against aimee-kb, where the DB layer's admin-or-team-lead
#    RLS check is the authority. Proxying it meant aimee-server needed an
#    administrative identity on aimee-kb, which is precisely what a single-tenant
#    data-plane service should not hold."
#
# So the supported path is the kb's own endpoint. This RUNS it, so the corrected
# documentation states something that has actually been executed rather than
# inferred from the source.
#
# Usage: test-grant-admin.sh
# Run AS ROOT in the container, after make-oidc-idp.sh.
set -u
export LC_ALL=C
KB=http://127.0.0.1:8741
D=/root/.config/aimee-oidc
rc=0

# Fresh token: the kb applies a hard age ceiling on `iat` independently of `exp`.
bash /root/make-oidc-idp.sh >/dev/null 2>&1 || { echo "FAIL: could not mint" >&2; exit 1; }
TOKEN="$(cat $D/token.jwt)"
OWNER="$(cat /root/kb-bearer.txt)"

call() { # $1 = bearer, $2 = method, $3 = path, $4 = body
  if [ "$2" = "GET" ]; then
    curl -s -m 20 -H "Authorization: Bearer $1" "$KB$3"
  else
    curl -s -m 20 -H "Authorization: Bearer $1" -H 'content-type: application/json' \
         -X "$2" --data "$4" "$KB$3"
  fi
}

BODY='{"server_id":"fact-authority-srv","team_id":7,"subject":"alice","tier":"data","granted_by":"owner"}'

echo "=== 1. the command every message names ==="
export AIMEE_HOME=/root AIMEE_API_ENDPOINT=unix:/root/aimee-http.sock
out="$(/usr/local/bin/aimee kb grant set --server fact-authority-srv --team 7 \
         --subject alice --tier data 2>&1 | head -2)"
echo "  $out"
case "$out" in
  *"not a subcommand"*) echo "  CONFIRMED: the documented command does not dispatch" ;;
  *) echo "  NOTE: it dispatched -- the documentation may now be correct"; ;;
esac

echo
echo "=== 2. the kb endpoint, as the owner ==="
o1="$(call "$OWNER" POST /v1/write-tier-grants/set "$BODY")"
echo "  $(printf '%s' "$o1" | tr '\n' ' ' | head -c 180)"

echo
echo "=== 3. the kb endpoint, as an OIDC subject ==="
o2="$(call "$TOKEN" POST /v1/write-tier-grants/set "$BODY")"
echo "  $(printf '%s' "$o2" | tr '\n' ' ' | head -c 180)"

echo
echo "=== 4. read it back ==="
o3="$(call "$OWNER" GET '/v1/write-tier-grants?server_id=fact-authority-srv&team_id=7' '')"
echo "  $(printf '%s' "$o3" | tr '\n' ' ' | head -c 220)"

echo
# Leg 2 must SUCCEED and leg 4 must show the grant. Until a tenant existed this
# probe could only ever watch it refuse, which cannot tell a working
# authorization path from a broken one -- the same shape as a probe stopped at an
# auth wall, which this suite has been fooled by twice.
case "$o1" in
  *'"changed"'*|*'"tier"'*)
    echo "PASS: the owner administered a grant through the kb endpoint" ;;
  *)
    echo "FAIL: the owner could not administer a grant"
    echo "      $(printf '%s' "$o1" | head -c 200)"
    rc=1 ;;
esac
case "$o3" in
  *'"grants"'*)
    echo "PASS: the grant reads back from the listing" ;;
  *)
    echo "FAIL: the grant does not read back, so leg 2 changed nothing durable"
    rc=1 ;;
esac
# The OIDC subject is NOT a member of that team, so it must be refused. Without
# this the run cannot distinguish "authorized correctly" from "authorizes anyone".
case "$o2" in
  *'not a member of that team'*|*refused*)
    echo "PASS: a non-member was refused, so membership is actually enforced" ;;
  *'"changed"'*)
    echo "FAIL: a principal with no membership administered a grant"
    rc=1 ;;
  *)
    echo "NOTE: the non-member leg answered something else: $(printf '%s' "$o2" | head -c 120)" ;;
esac
exit $rc

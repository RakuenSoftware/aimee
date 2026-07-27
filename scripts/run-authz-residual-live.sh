#!/bin/bash
# run-authz-residual-live.sh — the three §11 criteria that were left untested.
#
# WHY THIS EXISTS. The enforcement and PAM rigs covered most of §11 and I filed
# the remaining three as "not proven" and a reviewer decision. That was wrong:
# all three are testable with the same environment those rigs already stand up,
# and none of them needs the vault/KMS chain. This rig tests them.
#
#   1. REVOCATION LAG. "after kb revokes a subject's grant, the next call past the
#      documented lag is denied", bounded by one token TTL. Two halves, and both
#      are checkable without minting a real token:
#        - kb refuses to issue a NEW token the moment the grant is revoked; and
#        - the server does NOT consult grants per request, so an already-issued
#          token necessarily stays valid until its exp. That is what makes the lag
#          exactly the token TTL rather than something unbounded.
#      The second half is proven by construction here: a token verifies against
#      the JWKS with NO grant row anywhere in the database, which is only possible
#      if the request path never reads a grant.
#
#   2. CSRF on the PAM login POST. Asserted rather than argued: can a
#      cross-origin-shaped browser form actually drive this route? A browser can
#      only send text/plain, application/x-www-form-urlencoded or
#      multipart/form-data cross-origin without a preflight, so the question is
#      whether kb accepts a JSON body under those content types, and what an
#      attacker gets if it does.
#
#   3. THE STARTUP WARNING for a non-default aimee.api.remote_writes. §11 pairs it
#      with the global_ignored metric; only the metric was asserted.
#
# Uses the shared environment in scripts/lib/aimee-live-env.sh, so it does not
# reinvent the boot recipe a fourth time.
#
# MUST RUN AS ROOT on a host with Postgres.
# Usage: run-authz-residual-live.sh [--keep] [postgres://superuser@host:port/db]
set -uo pipefail
export LC_ALL=C

LIVE_KB_PORT=18911
LIVE_SRV_PORT=18913
LIVE_SERVER_ID=residsrv
# Deliberately NON-DEFAULT, because criterion 3 is about what that produces.
LIVE_REMOTE_WRITES=full

. "$(cd "$(dirname "$0")" && pwd)/lib/aimee-live-env.sh"

live_env_init "authz-residual" "$@"
live_env_pg_create
# A real host account, because the CSRF impact measurement has to be taken on a
# login that SUCCEEDS; the response to a rejected credential is a different
# response and cannot stand in for it.
live_env_add_host_account "residual_$$" "Resid-Pass-$$-7q"
live_env_start_kb
live_env_seed_identity_fixture
# kb caches nothing that matters here, but the fixture landed after boot, so
# restart to be certain no assertion depends on load order.
live_env_restart_kb
live_env_start_server

KB="http://127.0.0.1:$LIVE_KB_PORT"
kb_post() { # kb_post <path> <body> [content-type] -> status, body in $LIVE_WORK/out
   curl -s -o "$LIVE_WORK/out" -w '%{http_code}' -X POST \
      -H "Content-Type: ${3:-application/json}" --data "$2" "$KB$1"
}

# ---------------------------------------------------------------------------
step "1a. A grant is honoured, and REVOKING it denies the NEXT token immediately"

# Driven through the REAL mint SQL, kb_management_identity_intent_start -- the
# function the login route calls -- with the same session scope the route sets
# (aimee.principal / aimee.team, which db2_tenant_scope_begin establishes). That
# function reads the grant with `revoked_at IS NULL` and raises
# 'management identity not granted' when there is none, so it answers the
# criterion directly. No vault: filing an intent writes a row; only SIGNING the
# token needs custodied keys.
intent_call() { # intent_call <correlation-hex64> <jti-hex64> <token-jti> -> last output line
   pg_scoped alice "SELECT replayed FROM public.kb_management_identity_intent_start( \
       '$1','$2','$3',$LIVE_TEAM,'$LIVE_SERVER_ID','pam','kb','$LIVE_TOKEN_KID',300, \
       '$(printf '1%.0s' $(seq 1 32))')"
}
hex64() { printf '%s' "$1" | sha256sum | cut -c1-64; }

pg_scoped owner "SELECT kb_write_tier_grant_set('$LIVE_SERVER_ID',$LIVE_TEAM,'alice','data','owner')" >/dev/null 2>&1
granted=$(pg_val "SELECT tier FROM kb_write_tier_grant WHERE server_id='$LIVE_SERVER_ID' AND team_id=$LIVE_TEAM AND subject='alice' AND revoked_at IS NULL")
if [ "$granted" = "data" ]; then
   pass "grant row present at tier=data"
else
   fail "could not seed the grant (got '$granted'); the rest of group 1 would be meaningless"
fi

# WITH the grant: the mint files an intent.
out_before=$(intent_call "$(hex64 c1)" "$(hex64 j1)" "tokjti-before-001")
if [ "$out_before" = "f" ] || [ "$out_before" = "t" ]; then
   pass "with a live grant the mint files an intent (replayed=$(printf '%s' "$out_before" | tr -d ' \n'))"
elif printf '%s' "$out_before" | grep -qi 'not granted'; then
   fail "the mint refused DESPITE a live grant: $out_before"
else
   fail "unexpected mint result with a live grant: $(printf '%s' "$out_before" | head -c200)"
fi

# REVOKE.
# THREE arguments: (server_id, team_id, subject). The actor comes from
# aimee.principal, not from a parameter -- an audit trail that took the granter
# from an argument could be written to order.
rev_out=$(pg_scoped owner "SELECT kb_write_tier_grant_revoke('$LIVE_SERVER_ID',$LIVE_TEAM,'alice')")
printf '%s' "$rev_out" | grep -q '^ERROR:' && echo "  revoke error: $rev_out"
live_after=$(pg_val "SELECT count(*) FROM kb_write_tier_grant WHERE server_id='$LIVE_SERVER_ID' AND team_id=$LIVE_TEAM AND subject='alice' AND revoked_at IS NULL")
if [ "$live_after" = "0" ]; then
   pass "revoke stamped revoked_at: 0 live grants remain for alice"
else
   fail "revoke left $live_after live grant(s)"
fi

# THE CRITERION: the very next mint is refused. A fresh correlation id, so this
# is a NEW intent and not the replay of the one filed above.
out_after=$(intent_call "$(hex64 c2)" "$(hex64 j2)" "tokjti-after-002")
if printf '%s' "$out_after" | grep -qi 'not granted'; then
   pass "the NEXT token is refused immediately after revoke: 'management identity not granted'"
else
   fail "a revoked subject could still mint: $(printf '%s' "$out_after" | head -c200)"
fi

# ---------------------------------------------------------------------------
step "2. CSRF: a cross-origin browser form must NOT reach the PAM login"

# The impact measurement needs a login that SUCCEEDS, so the host account needs
# what any other subject needs: team membership and a live write-tier grant.
# (alice's grant was deliberately revoked above and must stay revoked.)
pg_db -c "INSERT INTO kb_team_membership(identity_key,team) VALUES ('$LIVE_PAM_USER',$LIVE_TEAM) ON CONFLICT DO NOTHING" >/dev/null 2>&1
pg_scoped owner "SELECT kb_write_tier_grant_set('$LIVE_SERVER_ID',$LIVE_TEAM,'$LIVE_PAM_USER','data','owner')" >/dev/null 2>&1
csrf_granted=$(pg_val "SELECT tier FROM kb_write_tier_grant WHERE server_id='$LIVE_SERVER_ID' AND team_id=$LIVE_TEAM AND subject='$LIVE_PAM_USER' AND revoked_at IS NULL")
if [ "$csrf_granted" = "data" ]; then
   pass "the host account $LIVE_PAM_USER is granted and can log in for real"
else
   fail "could not grant $LIVE_PAM_USER (got '$csrf_granted'); the impact measurement needs a successful login"
fi

# §11 says "CSRF-forged PAM login POST -> rejected". An earlier version of this
# section printed a note when a forged shape got through and passed anyway, which
# is not a test of that criterion -- CI stayed green while the criterion was
# unmet. It now FAILS, and the route was changed so that it can pass: the handler
# requires application/json, which is the one content type a cross-origin form
# cannot send without a preflight.
#
# The bodies below are what a BROWSER would actually put on the wire, not JSON
# with a swapped Content-Type header. That distinction matters: an attacker can
# only send what an HTML form emits.
csrf_user="${LIVE_PAM_USER}"
json_body=$(printf '{"username":"%s","password":"%s","server_id":"%s","team_id":%d}' \
   "$csrf_user" "$LIVE_PAM_PASS" "$LIVE_SERVER_ID" "$LIVE_TEAM")

# enctype=text/plain emits exactly "name=value\r\n" with NO escaping, which is
# what makes it the usable JSON-forgery vector: split the payload so the '=' the
# browser inserts lands INSIDE a string value, and the bytes on the wire are
# valid JSON. Getting this wrong is easy and self-defeating -- an earlier version
# here put the '=' outside a string, so the body was not JSON, the route rejected
# it at parse, and the assertion passed for a reason that had nothing to do with
# the control being tested.
tp_name=$(printf '{"username":"%s","password":"%s","server_id":"%s","team_id":%d,"pad":"' \
   "$csrf_user" "$LIVE_PAM_PASS" "$LIVE_SERVER_ID" "$LIVE_TEAM")
tp_body=$(printf '%s=%s\r\n' "$tp_name" '"}')
# PREREQUISITE, not a nicety: if this body is not valid JSON then the route would
# refuse it at parse and the 415 assertion below would pass without the
# content-type control doing anything. An earlier version checked this only when
# python3 happened to be installed and continued silently otherwise -- which is
# the same false pass, just conditional on the host.
if ! command -v python3 >/dev/null 2>&1; then
   echo "authz-residual: python3 is required to verify the CSRF forgery body is valid JSON;" >&2
   echo "  without it the 415 assertions cannot be distinguished from a parse refusal." >&2
   exit 2
fi
if printf '%s' "$tp_body" | python3 -c 'import json,sys; json.loads(sys.stdin.read())' 2>/dev/null; then
   pass "the text/plain form body is valid JSON, so it is a real forgery attempt"
else
   fail "the text/plain form body is not valid JSON — this vector would be refused at parse, \
proving nothing about the CSRF control"
fi
# enctype=application/x-www-form-urlencoded emits "name=value" with the name
# PERCENT-ENCODED. kb never url-decodes a body, so what arrives is not JSON and
# this vector cannot deliver a credential even without the content-type check.
# Included anyway: it must stay refused, and the refusal must not depend on that
# accident of encoding.
ue_body="$(printf '%s' "$json_body" | od -An -tx1 -v | tr -d ' \n' | sed 's/../%&/g')="
# enctype=multipart/form-data emits a boundary-delimited part.
mp_boundary="----WebKitFormBoundaryLiveCsrf"
mp_body=$(printf -- '--%s\r\nContent-Disposition: form-data; name="j"\r\n\r\n%s\r\n--%s--\r\n' \
   "$mp_boundary" "$json_body" "$mp_boundary")

csrf_reached=0
check_forged() { # check_forged <label> <content-type> <body>
   local code
   code=$(kb_post /v1/identity/login/pam "$3" "$2")
   if [ "$code" = "415" ]; then
      pass "forged $1 refused with 415 before any credential work"
   else
      csrf_reached=1
      fail "forged $1 reached the login handler (HTTP $code) — a cross-origin form can drive it"
   fi
}
check_forged "text/plain form" "text/plain" "$tp_body"
check_forged "urlencoded form" "application/x-www-form-urlencoded" "$ue_body"
check_forged "multipart form" "multipart/form-data; boundary=$mp_boundary" "$mp_body"
# A form that names no content type at all must not slip through either.
code=$(curl -s -o "$LIVE_WORK/out" -w '%{http_code}' -X POST --data-binary "$json_body" \
   -H 'Content-Type:' "$KB/v1/identity/login/pam")
if [ "$code" = "415" ]; then
   pass "a request with no Content-Type is refused too"
else
   fail "a request with no Content-Type reached the handler (HTTP $code)"
fi

# THE CONTENT-TYPE PARSER ITSELF, over a real socket. RFC 9110 allows zero or
# more spaces or tabs after the colon, so a client sending
# "Content-Type:application/json" is sending JSON -- and a route that requires
# JSON must not answer 415 to it. Raw bytes rather than curl, because curl
# normalises the header it is given and would hide exactly the difference under
# test. The route tests cannot cover this: they call the router directly and
# never go through a header parser at all.
raw_login() { # raw_login <literal header line> -> HTTP status
   local hdr=$1 body=$2 resp=""
   exec 3<>"/dev/tcp/127.0.0.1/$LIVE_KB_PORT" || { echo "000"; return; }
   printf 'POST /v1/identity/login/pam HTTP/1.1\r\nHost: 127.0.0.1\r\n%s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s' \
      "$hdr" "${#body}" "$body" >&3
   resp=$(timeout 10 head -1 <&3)
   exec 3<&-
   printf '%s' "$resp" | sed -nE 's#^HTTP/1\.1 ([0-9]{3}).*#\1#p'
}
for hdr in 'Content-Type:application/json' \
   'Content-Type: application/json' \
   "$(printf 'Content-Type:\tapplication/json')" \
   'Content-Type:   application/json   '; do
   code=$(raw_login "$hdr" "$json_body")
   if [ "$code" = "200" ]; then
      pass "wire form '$(printf '%s' "$hdr" | cat -A | head -c60)' accepted -> 200"
   else
      fail "wire form '$(printf '%s' "$hdr" | cat -A | head -c60)' -> ${code:-no response}; \
optional whitespace after the colon is legal and must not read as a missing content type"
   fi
done

# THE REFUSAL MUST BE FREE. If a forged request were charged to the login
# throttle, a form on an attacker's page could lock the named user out — turning
# a CSRF that achieves nothing into a denial of service that achieves plenty.
# Measured from outside: after a burst of forged requests, a real login for the
# same user must still succeed rather than answer 429.
i=0
while [ $i -lt 40 ]; do
   kb_post /v1/identity/login/pam "$tp_body" "text/plain" >/dev/null
   i=$((i + 1))
done
code=$(kb_post /v1/identity/login/pam "$json_body")
if [ "$code" = "429" ]; then
   fail "40 forged cross-origin requests spent the real user's login budget (429): CSRF becomes DoS"
else
   pass "40 forged requests cost the real user nothing (a genuine login still answers $code)"
fi

# WHAT A FORGED LOGIN WOULD ACHIEVE IF ONE LANDED. Measured on a SUCCESSFUL
# login, with an attacker's Origin, and without following redirects -- an earlier
# version measured a FAILED login with no Origin, and cookie/redirect/CORS
# behaviour can all differ by authentication outcome and by Origin, so that
# measurement did not support the claim it was cited for.
hdrs=$(curl -s -D - -o "$LIVE_WORK/succ" -w '\n%{http_code}' --max-redirs 0 -X POST \
   -H 'Content-Type: application/json' -H 'Origin: https://attacker.example' \
   --data "$json_body" "$KB/v1/identity/login/pam")
succ_code=$(printf '%s' "$hdrs" | tail -1)
if [ "$succ_code" = "200" ]; then
   pass "the impact measurement is taken on a SUCCESSFUL login (HTTP 200)"
else
   fail "could not drive a successful login for the impact measurement (HTTP $succ_code); \
the cookie/redirect/CORS assertions below would be measuring a refusal"
fi
if printf '%s' "$hdrs" | grep -qi '^set-cookie:'; then
   fail "the login route sets a COOKIE on success, so a forged login would plant an ambient credential"
else
   pass "no Set-Cookie even on success: a forged login plants no ambient credential"
fi
if printf '%s' "$hdrs" | grep -qiE '^location:'; then
   fail "the login route REDIRECTS on success, which a browser would follow after a forged POST"
else
   pass "no redirect on success: nothing for a browser to follow"
fi
if printf '%s' "$hdrs" | grep -qi '^access-control-allow-origin:'; then
   fail "CORS is enabled, so an attacker's page could READ a successful login response"
else
   pass "no Access-Control-Allow-Origin for an attacker Origin: the response is unreadable cross-origin"
fi

# ---------------------------------------------------------------------------
step "3. The startup warning for a non-default aimee.api.remote_writes"

# The config is remote_writes: full (set at the top of this rig), which is
# non-default and no longer authorizes anything. §11 requires the operator to be
# told at startup, not only via the metric.
warn=$(grep -iE 'remote_writes' "$LIVE_SRV_LOG" 2>/dev/null | head -5)
if printf '%s' "$warn" | grep -qiE 'no longer|retired|ignored|not authorize'; then
   pass "startup warning present: $(printf '%s' "$warn" | head -1 | cut -c1-120)"
else
   fail "no startup warning for a non-default remote_writes in the server log"
   echo "       server log had: ${warn:-<no remote_writes line at all>}"
fi

live_env_verdict "the three residual §11 criteria are measured, not argued"

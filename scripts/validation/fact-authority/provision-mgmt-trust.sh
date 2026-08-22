#!/bin/bash
# Provision the management trust chain so aimee-server can verify a KB-issued
# identity token, and therefore resolve an ACCOUNT from a request that arrives
# over TCP.
#
# This is the piece that was missing when the PAM/OIDC-over-TCP path was left
# unproven. The server does not accept a raw JWKS: it reads
# AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE (a root-owned, single-link, non-group/other
# -writable file pinning an Ed25519 manifest key), then loads a SIGNED
# PUBLICATION ENVELOPE from DB1 and validates it against that bundle. None of
# that can be faked from the shell.
#
# `write-tier-enforce-live provision` does it with the PRODUCTION functions --
# real Ed25519 manifest signing, the real envelope encoder, a real DB1 row -- so
# a change that breaks the real publication path breaks this too. It is the
# tree's own rig for exactly this chain; this script only drives it and records
# the environment the server then needs.
#
# What is stood in for, deliberately and in line with that rig's own note: the
# RSA token key is generated locally rather than custodied in the Vault. Key
# custody is proven separately by run-identity-mint-e2e.sh against a real KMS
# helper. What is NOT stood in for is anything the server does with the token.
#
# Must run BEFORE aimee-server starts: the envelope has to be in the very DB1
# file the server opens, and an absent one denies every token as INVALID --
# indistinguishable from a forged one, which would make a test "pass" for the
# wrong reason.
# Run AS ROOT in the container.
set -u
export LC_ALL=C
DB1=/root/aimee.db
BUNDLE=/root/mgmt-trust.json
TOKEN_KEY=/root/mgmt-token.key
SERVER_ID="${AIMEE_SERVER_ID:-fact-authority-srv}"
TEAM_ID="${AIMEE_SERVER_TEAM_ID:-7}"
RIG=/usr/local/bin/write-tier-enforce-live

[ -x "$RIG" ] || { echo "FAIL: $RIG not installed" >&2; exit 1; }
[ -f "$DB1" ] || { echo "FAIL: DB1 not found at $DB1" >&2; exit 1; }

kid="$("$RIG" provision --db1 "$DB1" --bundle "$BUNDLE" --key "$TOKEN_KEY")" || {
  echo "FAIL: trust chain provisioning failed" >&2; exit 1; }

# The loader refuses an untrusted owner or a writable file, so state the posture
# rather than assuming the rig left it right.
chown root:root "$BUNDLE" && chmod 0644 "$BUNDLE"

cat > /root/mgmt-trust-env.sh <<EOF
export AIMEE_SERVER_ID=$SERVER_ID
export AIMEE_SERVER_TEAM_ID=$TEAM_ID
export AIMEE_SERVER_MGMT_JWKS_TRUST_BUNDLE=$BUNDLE
EOF

echo "kid:          $kid"
echo "trust bundle: $BUNDLE"
ls -la "$BUNDLE" | tr -s ' '
echo "token key:    $TOKEN_KEY"
echo "server id:    $SERVER_ID   (the token AUDIENCE)"
echo "team id:      $TEAM_ID     (the single team this server is enrolled in)"
echo "env:          /root/mgmt-trust-env.sh (sourced by start-server.sh)"

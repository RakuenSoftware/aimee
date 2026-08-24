#!/bin/bash
# Provision a tenant so write-tier grant administration can SUCCEED.
#
# Every earlier run of test-grant-admin.sh saw the same refusal, and that is all
# it could see: the acting principal belonged to no team, so db2_tenant_scope_begin
# returned DB2_ERR_TENANT_DENIED (-104) and the route answered 403 before any
# grant logic ran. A probe that only ever observes a refusal cannot tell a
# working authorization path from a broken one -- it is the same shape as a probe
# refused at an auth wall, which this suite has been caught by twice.
#
# So the tenant is created here: a team, a membership for the acting identity, an
# admin grant, and the (server, team) registration the route also requires.
#
# WHAT IS REAL: the route, the SECURITY DEFINER functions, the RLS scope, and the
# grant table are all the product's. What is seeded is the tenancy an operator
# would otherwise create through the console -- rows, not behaviour.
#
# The acting identity for the kb's own bearer is the install owner, whose
# identity key is the literal `owner` (kb_identity.h: the owner/bearer principal
# in the no-IdP single-org case; the subject grammar reserves that name).
#
# Usage: provision-tenant.sh [TEAM_ID] [SERVER_ID]
# Run AS ROOT in the container.
set -u
export LC_ALL=C
TEAM="${1:-7}"
SERVER="${2:-fact-authority-srv}"
IDENTITY="${IDENTITY:-owner}"
P() { PGPASSWORD=aimee-e2e psql -q -h 127.0.0.1 -U aimee -d aimee_shared -Atc "$1" 2>&1 | grep -v '^perl'; }

echo "team $TEAM, server $SERVER, identity $IDENTITY"

P "INSERT INTO kb_team(id, name) VALUES($TEAM, 'fact-authority-team')
   ON CONFLICT (id) DO NOTHING" >/dev/null
P "INSERT INTO kb_team_membership(identity_key, team, is_default)
   VALUES('$IDENTITY', $TEAM, 1)
   ON CONFLICT (identity_key, team) DO NOTHING" >/dev/null
# Admin authority is what the definer functions check; without it the route
# refuses with the OTHER 403 ("admin or team-lead authority"), which is a
# different answer and worth not confusing with the membership one.
P "INSERT INTO kb_admin_grant(identity_key, source, granted_by)
   VALUES('$IDENTITY', 'fact-authority-validation', 'provision-tenant.sh')
   ON CONFLICT (identity_key) DO NOTHING" >/dev/null
# The (server, team) pair must be registered, or the grant has nowhere to land.
P "INSERT INTO kb_server_registry(server_id, cert_cn, mgmt_cert_cn, team_id, endpoint, status)
   VALUES('$SERVER', '$SERVER-cert', '$SERVER-mgmt', $TEAM, 'https://127.0.0.1:8743', 'active')
   ON CONFLICT (server_id) DO UPDATE SET team_id = EXCLUDED.team_id, status = 'active'" >/dev/null

echo "--- provisioned ---"
echo "  teams:       $(P "select count(*) from kb_team where id=$TEAM")"
echo "  membership:  $(P "select count(*) from kb_team_membership where identity_key='$IDENTITY' and team=$TEAM")"
echo "  admin grant: $(P "select count(*) from kb_admin_grant where identity_key='$IDENTITY' and revoked_at=''")"
echo "  server reg:  $(P "select count(*) from kb_server_registry where server_id='$SERVER' and team_id=$TEAM")"

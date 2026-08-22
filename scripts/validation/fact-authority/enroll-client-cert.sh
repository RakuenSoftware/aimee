#!/bin/bash
# Bind the issued client certificate to a first-user grant at tier `full`, so an
# mTLS request can reach the authority decision instead of stopping at the
# write-tier wall.
#
# WHY THIS IS SEEDED RATHER THAN DRIVEN
#
# The real enrollment is a wizard flow: a webchat user calls /v1/deploy/apply,
# which claims the first user and returns an ENROLLMENT-ONLY bearer, and
# /v1/cert/sign then binds that bearer to the client's CSR-produced certificate
# and activates `full` (remote_client_grant.h: "the standing write grant is
# attached to the mTLS certificate produced by that enrollment"). Driving it end
# to end needs a webchat principal and a docker compose deploy, neither of which
# this container has -- and neither of which is what is under test.
#
# What IS under test is what happens AFTER enrollment: does a verified,
# enrolled client certificate resolve to an account and therefore to user
# authority. So this writes the row enrollment would have left behind, exactly
# as seed-facts.sh writes the entity_edges rows an earlier build would have
# left behind.
#
# The grant is keyed on cert_serial (db1_remote_client_tier: SELECT
# principal,tier FROM remote_client_grants WHERE cert_serial=?1), and tier
# 'full' maps to SERVER_REMOTE_WRITES_FULL.
#
# Usage: enroll-client-cert.sh [PRINCIPAL]
# Run AS ROOT in the container.
set -u
export LC_ALL=C
DB=/root/aimee.db
SERIAL_FILE=/root/.config/aimee-tls/client.serial
PRINCIPAL="${1:-thin-client-a}"

[ -f "$SERIAL_FILE" ] || { echo "no issued serial -- run make-mtls-certs.sh first" >&2; exit 1; }
SERIAL="$(cat "$SERIAL_FILE")"
[ -n "$SERIAL" ] || { echo "issued serial is empty" >&2; exit 1; }
[ -f "$DB" ] || { echo "DB1 not found at $DB" >&2; exit 1; }

echo "serial:    $SERIAL"
echo "principal: $PRINCIPAL"

# A bearer hash is NOT NULL in the schema and is the enrollment credential's
# fingerprint. Nothing in the read path consults it (the tier lookup keys on
# cert_serial alone), so a deterministic placeholder derived from the serial
# keeps the row well-formed without inventing a credential that could be used.
HASH="$(printf 'enrolled-%s' "$SERIAL" | sha256sum | cut -d' ' -f1)"

sqlite3 "$DB" <<SQL
DELETE FROM remote_client_grants WHERE cert_serial='$SERIAL' OR principal='$PRINCIPAL';
INSERT INTO remote_first_user(singleton,principal,created_at)
  VALUES(1,'$PRINCIPAL',strftime('%s','now'))
  ON CONFLICT(singleton) DO NOTHING;
INSERT INTO remote_client_grants
  (bearer_sha256,principal,tier,cert_serial,created_at,bound_at)
  VALUES('$HASH','$PRINCIPAL','full','$SERIAL',strftime('%s','now'),strftime('%s','now'));
SQL
rc=$?
[ $rc -eq 0 ] || { echo "FAIL: could not write the grant (rc=$rc)" >&2; exit 1; }

echo "--- remote_client_grants ---"
sqlite3 -header -column "$DB" \
  "SELECT principal,tier,cert_serial FROM remote_client_grants WHERE cert_serial='$SERIAL'"

n="$(sqlite3 "$DB" "SELECT COUNT(*) FROM remote_client_grants WHERE cert_serial='$SERIAL' AND tier='full'")"
[ "${n:-0}" = "1" ] || { echo "FAIL: expected 1 full-tier grant for this serial, got ${n:-0}" >&2; exit 1; }
echo "enrolled: tier=full bound to $SERIAL"

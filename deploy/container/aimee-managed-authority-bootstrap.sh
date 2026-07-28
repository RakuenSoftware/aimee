#!/bin/sh
# One-shot, wizard-orchestrated management trust bootstrap for a local managed KB.
set -eu

authority=/var/lib/aimee-authority
trust_dir=/run/aimee-trust
admin_db_url='postgresql:///aimee_shared?host=/var/lib/aimee-kb/run&user=aimee'
authority_db_role=aimee_managed_authority_login
authority_db_url="postgresql:///aimee_shared?host=/var/lib/aimee-kb/run&user=$authority_db_role"
helper="$authority/managed-kms-helper"
private="$authority/hwm-private.pem"
public="$authority/hwm-public.raw"
kek="$authority/kek.raw"

fail() {
    echo "aimee-authority-bootstrap: $1" >&2
    exit 1
}

[ "$(id -u)" = 0 ] || fail "must run as root"
umask 077
install -d -o root -g root -m 0700 "$authority"
install -d -o root -g root -m 0755 "$trust_dir"

# Serialize retries and concurrent Deploy clicks without leaving a stale lock
# after a crash. `flock -o` keeps the lock in its parent supervisor and closes
# the descriptor in this script, so it cannot cross into a custody helper.
if [ "$#" = 0 ]; then
    exec flock -x -o "$authority/bootstrap.lock" "$0" --locked
fi
[ "$#" = 1 ] && [ "$1" = --locked ] || fail "usage"
chmod 0600 "$authority/bootstrap.lock"

private_shape() {
    path=$1
    size=${2:-}
    [ -f "$path" ] && [ ! -L "$path" ] || return 1
    meta=$(stat -c '%u:%a:%h:%s' "$path") || return 1
    case "$meta" in
        0:600:1:*) ;;
        *) return 1 ;;
    esac
    [ -z "$size" ] || [ "${meta##*:}" = "$size" ]
}

if [ ! -e "$private" ]; then
    tmp=$(mktemp "$authority/.hwm-private.XXXXXX")
    trap 'rm -f "$tmp"' EXIT HUP INT TERM
    openssl genpkey -algorithm Ed25519 -out "$tmp" 2>/dev/null || fail "HWM key generation failed"
    chmod 0600 "$tmp"
    mv "$tmp" "$private"
    trap - EXIT HUP INT TERM
fi
private_shape "$private" || fail "HWM private key has an unsafe shape"
openssl pkey -in "$private" -check -noout >/dev/null 2>&1 || fail "HWM private key is invalid"

public_tmp=$(mktemp "$authority/.hwm-public.XXXXXX")
trap 'rm -f "$public_tmp"' EXIT HUP INT TERM
openssl pkey -in "$private" -pubout -outform DER 2>/dev/null | tail -c 32 >"$public_tmp"
[ "$(wc -c <"$public_tmp")" = 32 ] || fail "HWM public key derivation failed"
chmod 0600 "$public_tmp"
if [ -e "$public" ]; then
    private_shape "$public" 32 || fail "HWM public key has an unsafe shape"
    cmp -s "$public_tmp" "$public" || fail "HWM public key does not match its private key"
    rm -f "$public_tmp"
else
    mv "$public_tmp" "$public"
fi
trap - EXIT HUP INT TERM

if [ ! -e "$kek" ]; then
    tmp=$(mktemp "$authority/.kek.XXXXXX")
    trap 'rm -f "$tmp"' EXIT HUP INT TERM
    head -c 32 /dev/urandom >"$tmp"
    chmod 0600 "$tmp"
    mv "$tmp" "$kek"
    trap - EXIT HUP INT TERM
fi
private_shape "$kek" 32 || fail "software KEK has an unsafe shape"

# The helper path is part of the persisted publication binding. Keep the path
# stable while atomically updating only from the trusted image on an upgrade.
tmp=$(mktemp "$authority/.managed-kms-helper.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM
install -o root -g root -m 0700 /usr/libexec/aimee/managed-kms-helper.py "$tmp"
mv "$tmp" "$helper"
trap - EXIT HUP INT TERM

# The embedded cluster starts as the private `aimee` superuser. Install the
# hardened role/grant topology out of band, then make that local deploy identity
# a migration member so the two offline tools can SET ROLE to their isolated
# provisioner compartments. No TCP database credential is created.
psql "$admin_db_url" -X -q -v ON_ERROR_STOP=1 -f /usr/share/aimee/schema_roles.sql >/dev/null
psql "$admin_db_url" -X -q -v ON_ERROR_STOP=1 -f /usr/share/aimee/schema_grants.sql >/dev/null
psql "$admin_db_url" -X -q -v ON_ERROR_STOP=1 <<SQL >/dev/null
DO \$\$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_roles WHERE rolname='$authority_db_role') THEN
    CREATE ROLE $authority_db_role LOGIN NOINHERIT NOBYPASSRLS
      NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
  END IF;
END
\$\$;
ALTER ROLE $authority_db_role LOGIN NOINHERIT NOBYPASSRLS
  NOCREATEDB NOCREATEROLE NOSUPERUSER NOREPLICATION;
GRANT aimee_kb_migrate TO $authority_db_role;
SQL

export AIMEE_VAULT_KMS_HELPER="$helper"
export AIMEE_VAULT_KMS_KEY_ID=managed-software-kek-v1
export AIMEE_VAULT_KMS_HWM_PUBKEY="$public"
export AIMEE_VAULT_KMS_HWM_DOMAIN=aimee-managed-authority-v1
export AIMEE_KB_TOKEN_ROOT_CUSTODY_ID=managed-token-root-v1
export AIMEE_KB_JWKS_MANIFEST_ROOT_CUSTODY_ID=managed-jwks-manifest-root-v1
export AIMEE_KB_JWKS_PUBLICATION_HWM_CUSTODY_ID=managed-jwks-publication-v1
export AIMEE_KB_TOKEN_ROOTS_PROVISION_DSN="$authority_db_url"
export AIMEE_KB_JWKS_PUBLISH_DSN="$authority_db_url"

"$helper" hwm-read "$AIMEE_VAULT_KMS_KEY_ID" >/dev/null || fail "software KMS self-check failed"

# Root provisioning is crash-resumable. Always use the explicit export path for
# the deployment file so a retry cannot replace or invent a trust pin.
aimee-kb-token-roots-provision >/dev/null || fail "management trust-root provisioning failed"
bundle_tmp=$(mktemp "$trust_dir/.jwks-trust-bundle.XXXXXX")
trap 'rm -f "$bundle_tmp"' EXIT HUP INT TERM
aimee-kb-token-roots-provision --export-public >"$bundle_tmp" || fail "trust-bundle export failed"
chmod 0644 "$bundle_tmp"
chown root:root "$bundle_tmp"

# Export succeeds only for a finalized publication. When it does not exist yet,
# resume any staged validity tuple exactly; otherwise create generation 1 for the
# current bounded publication window.
if ! aimee-kb-jwks-publish --export-public >/dev/null 2>&1; then
    times=$(psql "$authority_db_url" -X -qAt -v ON_ERROR_STOP=1 \
        -c "SELECT valid_from||' '||valid_until FROM kb_management_jwks_publication_candidate WHERE generation=1")
    if [ -n "$times" ]; then
        set -- $times
        valid_from=$1
        valid_until=$2
    else
        valid_from=$(date -u +%s)
        valid_until=$((valid_from + 82800))
    fi
    aimee-kb-jwks-publish --valid-from "$valid_from" --valid-until "$valid_until" >/dev/null \
        || fail "signed JWKS publication failed"
fi
aimee-kb-jwks-publish --export-public >/dev/null || fail "signed JWKS verification failed"

mv "$bundle_tmp" "$trust_dir/jwks-trust-bundle.json"
trap - EXIT HUP INT TERM
bundle_digest=$(sha256sum "$trust_dir/jwks-trust-bundle.json" | cut -d' ' -f1)
valid_until=$(psql "$authority_db_url" -X -qAt -v ON_ERROR_STOP=1 \
    -c 'SELECT valid_until FROM kb_management_jwks_publication_generation WHERE generation=1')
echo "aimee-authority-bootstrap: ready generation=1 valid_until=$valid_until trust_sha256=$bundle_digest"

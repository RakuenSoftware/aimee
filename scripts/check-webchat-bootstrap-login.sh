#!/usr/bin/env bash
# Vault-only webchat migration checks without touching host accounts.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -p /tmp aimee-webchat-vault.XXXXXX)
trap 'find "$test_dir" -depth -delete 2>/dev/null || true' EXIT

export AIMEE_HOME="$test_dir/home"
mkdir -p "$AIMEE_HOME/webchat"

sealed_dir="$test_dir/sealed-vault-fixture"
mkdir -p "$sealed_dir"
cleared_users="$test_dir/cleared-users"
: > "$cleared_users"

runuser() {
  [[ ${1:-} == -u && ${2:-} == aimee && ${3:-} == -- &&
     ${4:-} == aimee-server && ${5:-} == --webchat-vault-seal ]]
  case ${6:-} in
    legacy_primary | legacy_hashes | tls_key)
      # Test-only fake Vault. Production's C helper encrypts this stdin directly.
      tee "$sealed_dir/${6}" >/dev/null
      ;;
    *) return 1 ;;
  esac
}

id() { return 0; }
getent() {
  case ${1:-}:${2:-} in
    group:aimee) printf 'aimee:x:999:operator,legacy\n' ;;
    *) return 1 ;;
  esac
}
usermod() {
  [[ ${1:-} == --password && ${2:-} == '!' && -n ${3:-} ]]
  printf '%s\n' "$3" >> "$cleared_users"
}

# shellcheck source=../deploy/container/runtime-web-lib.sh
source "$repo_dir/deploy/container/runtime-web-lib.sh"

legacy_user=aimee-012345abcdef
legacy_pass=$(printf 'a%.0s' {1..64})
legacy_hash='$6$legacy$verifier'
legacy_key='-----BEGIN EC PRIVATE KEY-----
test-only-key-material
-----END EC PRIVATE KEY-----'
printf 'username=%s\npassword=%s\n' "$legacy_user" "$legacy_pass" > "$WEBCHAT_BOOTSTRAP_CREDENTIALS"
printf '%s:%s\noperator:%s\n' "$legacy_user" "$legacy_hash" '$6$operator$verifier' > "$WEBCHAT_LOGIN_STORE"
printf '%s\n' "$legacy_key" > "$WEBCHAT_LEGACY_TLS_KEY"

migration_log=$(webchat_migrate_legacy_credentials)
[[ ! -e $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
[[ ! -e $WEBCHAT_LOGIN_STORE ]]
[[ ! -e $WEBCHAT_LEGACY_TLS_KEY ]]
[[ $(<"$sealed_dir/legacy_primary") == "$legacy_user:$legacy_pass" ]]
grep -Fq "$legacy_hash" "$sealed_dir/legacy_hashes"
grep -Fq 'BEGIN EC PRIVATE KEY' "$sealed_dir/tls_key"
for user in "$legacy_user" operator legacy aimee; do
  grep -Fxq "$user" "$cleared_users"
done
for secret in "$legacy_pass" "$legacy_hash" test-only-key-material; do
  ! grep -Fq "$secret" <<<"$migration_log"
done

# A corrupt legacy plaintext record fails closed and remains available for an
# operator-assisted recovery; it is never silently deleted.
printf 'not-a-valid-record\n' > "$WEBCHAT_BOOTSTRAP_CREDENTIALS"
set +e
corrupt_log=$(webchat_migrate_legacy_credentials)
corrupt_rc=$?
set -e
[[ $corrupt_rc -ne 0 ]]
[[ -f $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
grep -Fq 'invalid' <<<"$corrupt_log"

# Headless mode still performs custody migration before it disables the UI.
find "$AIMEE_HOME/webchat" -type f -delete
export AIMEE_RUNTIME_WEB_ENABLED=0
disabled_log=$(webchat_prepare)
grep -Fq 'browser UI disabled' <<<"$disabled_log"

echo "webchat-vault-migration-check: ok"

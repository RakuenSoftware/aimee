#!/usr/bin/env bash
# Webchat credential-custody checks without touching host accounts.
#
# The contract these assert changed with PAM login: a host password is not one of
# aimee's own secrets, so logins are NOT sealed into the Vault and their shadow
# verifiers are NOT erased. The plaintext bootstrap file is still removed — after
# the account is provisioned from it — and the TLS key, which IS aimee's secret,
# still goes to the Vault.
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
# Violations are recorded as FILES, not `exit`: these stubs run inside pipelines
# and command substitutions, where an exit only kills the subshell and the check
# would pass while printing its own failure.
violations="$test_dir/violations"
: > "$violations"

runuser() {
  [[ ${1:-} == -u && ${2:-} == aimee && ${3:-} == -- &&
     ${4:-} == aimee-server && ${5:-} == --webchat-vault-seal ]]
  case ${6:-} in
    tls_key)
      # Test-only fake Vault. Production's C helper encrypts this stdin directly.
      tee "$sealed_dir/${6}" >/dev/null
      ;;
    legacy_primary | legacy_hashes)
      # Sealing a login into the Vault is the behaviour this check now forbids.
      printf 'sealed a host login into the Vault (%s)\n' "$6" >> "$violations"
      cat >/dev/null
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
  # Erasing a verifier would delete the credential PAM authenticates with.
  if [[ ${1:-} == --password ]]; then
    printf 'erased a shadow verifier (%s)\n' "$*" >> "$violations"
    return 0
  fi
  printf '%s\n' "$*" >> "$cleared_users"
}
useradd() { printf 'useradd %s\n' "$*" >> "$cleared_users"; }
groupadd() { :; }
chpasswd() { cat >/dev/null; printf 'chpasswd\n' >> "$cleared_users"; }

# shellcheck source=../deploy/container/runtime-web-lib.sh
source "$repo_dir/deploy/container/runtime-web-lib.sh"

legacy_user=aimee-012345abcdef
legacy_pass=$(printf 'a%.0s' {1..64})
legacy_hash='$6$legacy$verifier'
legacy_key='-----BEGIN EC PRIVATE KEY-----
test-only-key-material
-----END EC PRIVATE KEY-----'
printf 'username=%s\npassword=%s\n' "$legacy_user" "$legacy_pass" > "$WEBCHAT_BOOTSTRAP_CREDENTIALS"
printf '%s\n' "$legacy_key" > "$WEBCHAT_LEGACY_TLS_KEY"

migration_log=$(webchat_migrate_legacy_credentials)
# The plaintext bootstrap file is removed once its account exists; the TLS key is
# sealed. No login record reaches the Vault (the fake would have failed above).
[[ ! -e $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
[[ ! -e $WEBCHAT_LEGACY_TLS_KEY ]]
[[ ! -e $sealed_dir/legacy_primary ]]
[[ ! -e $sealed_dir/legacy_hashes ]]
grep -Fq 'BEGIN EC PRIVATE KEY' "$sealed_dir/tls_key"
# The account was provisioned before its plaintext source was deleted.
grep -q 'chpasswd' "$cleared_users"
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

# Fail on anything the stubs recorded. Last, so every check has run first.
if [[ -s $violations ]]; then
  echo "webchat-vault-migration-check: FAILED" >&2
  cat "$violations" >&2
  exit 1
fi

#!/usr/bin/env bash
# Container-entrypoint credential generation without mutating host accounts.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
test_dir=$(mktemp -d -p /tmp aimee-webchat-bootstrap.XXXXXX)
trap 'rm -rf -- "$test_dir"' EXIT

export AIMEE_HOME="$test_dir/generated"
mock_users="$test_dir/users"
: > "$mock_users"

# shellcheck source=../deploy/container/runtime-web-lib.sh
. "$repo_dir/deploy/container/runtime-web-lib.sh"

id() {
  [[ ${1:-} == aimee ]] && return 0
  grep -Fxq -- "${1:-}" "$mock_users"
}
getent() {
  if [[ ${1:-} == group ]]; then return 0; fi
  if [[ ${1:-} == shadow ]]; then printf '%s:$6$test$hash:0:0:0:0:0:0:\n' "$2"; return 0; fi
  return 1
}
groupadd() { return 0; }
usermod() { return 0; }
useradd() {
  local last=""
  for last in "$@"; do :; done
  printf '%s\n' "$last" >> "$mock_users"
}
chpasswd() {
  local line user
  IFS= read -r line
  user=${line%%:*}
  grep -Fxq -- "$user" "$mock_users" || printf '%s\n' "$user" >> "$mock_users"
}

unset AIMEE_WEBCHAT_USER AIMEE_WEBCHAT_PASSWORD AIMEE_WEBCHAT_USERS
first_log=$(webchat_bootstrap_user)
[[ ! -e $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
generated_user=$(sed -n 's/^\[webchat\]   username: //p' <<<"$first_log")
generated_pass=$(sed -n 's/^\[webchat\]   password: //p' <<<"$first_log")
[[ $generated_user =~ ^aimee-[0-9a-f]{12}$ ]]
[[ $generated_pass =~ ^[0-9a-f]{64}$ ]]
grep -Fq "username: $generated_user" <<<"$first_log"
grep -Fq "password: $generated_pass" <<<"$first_log"

# A recreate restores the PAM verifier but never persisted or reprints the
# generated plaintext password.
second_log=$(webchat_bootstrap_user)
grep -Fq 'restored the existing temporary browser login' <<<"$second_log"
! grep -Fq "$generated_pass" <<<"$second_log"
[[ ! -e $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]

# Onboarding retirement keeps the plaintext absent and no longer logs it.
mkdir -p "$(dirname "$WEBCHAT_BOOTSTRAP_REPLACED")"
printf 'operator\n' > "$WEBCHAT_BOOTSTRAP_REPLACED"
retired_log=$(webchat_bootstrap_user)
[[ ! -e $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
! grep -Fq "$generated_pass" <<<"$retired_log"

# The retirement marker blocks only the old published bootstrap. A later custom
# env pair is an operator-managed account and must still be provisioned.
export AIMEE_WEBCHAT_USER=siteadmin
export AIMEE_WEBCHAT_PASSWORD=post-onboarding-secret
post_onboarding_log=$(webchat_bootstrap_user)
grep -Fxq siteadmin "$mock_users"
grep -Fq "login user 'siteadmin' ready" <<<"$post_onboarding_log"
! grep -Fq "$AIMEE_WEBCHAT_PASSWORD" <<<"$post_onboarding_log"

# Rolling upgrades may still inject the retired image default; never resurrect it.
export AIMEE_WEBCHAT_USER=aimee
export AIMEE_WEBCHAT_PASSWORD=aimee-local-dev
legacy_log=$(webchat_bootstrap_user)
grep -Fq 'retired legacy bootstrap login remains disabled' <<<"$legacy_log"

# Pair validation applies even after onboarding.
unset AIMEE_WEBCHAT_PASSWORD
set +e
marker_partial_log=$(webchat_bootstrap_user)
marker_partial_rc=$?
set -e
[[ $marker_partial_rc -ne 0 ]]
grep -Fq 'must be set together' <<<"$marker_partial_log"

# A corrupt plaintext file must not be deleted while an unknown PAM login could
# still be active, even if the onboarding marker already exists.
export AIMEE_HOME="$test_dir/corrupt-marker"
WEBCHAT_HOME="$AIMEE_HOME"
WEBCHAT_LOGIN_STORE="$WEBCHAT_HOME/webchat/logins"
WEBCHAT_BOOTSTRAP_REPLACED="$WEBCHAT_HOME/webchat/bootstrap-replaced"
WEBCHAT_BOOTSTRAP_CREDENTIALS="$WEBCHAT_HOME/webchat/bootstrap-credentials"
mkdir -p "$WEBCHAT_HOME/webchat"
printf 'operator\n' > "$WEBCHAT_BOOTSTRAP_REPLACED"
printf 'not-a-credential\n' > "$WEBCHAT_BOOTSTRAP_CREDENTIALS"
unset AIMEE_WEBCHAT_USER AIMEE_WEBCHAT_PASSWORD
set +e
corrupt_log=$(webchat_bootstrap_user)
corrupt_rc=$?
set -e
[[ $corrupt_rc -ne 0 ]]
[[ -f $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
grep -Fq 'refusing to leave an unknown login active' <<<"$corrupt_log"

# An explicit pair wins, is never copied to the plaintext generated store, and
# does not print the supplied password.
export AIMEE_HOME="$test_dir/explicit"
WEBCHAT_HOME="$AIMEE_HOME"
WEBCHAT_LOGIN_STORE="$WEBCHAT_HOME/webchat/logins"
WEBCHAT_BOOTSTRAP_REPLACED="$WEBCHAT_HOME/webchat/bootstrap-replaced"
WEBCHAT_BOOTSTRAP_CREDENTIALS="$WEBCHAT_HOME/webchat/bootstrap-credentials"
export AIMEE_WEBCHAT_USER=operator
export AIMEE_WEBCHAT_PASSWORD=operator-secret
explicit_log=$(webchat_bootstrap_user)
[[ ! -e $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
! grep -Fq "$AIMEE_WEBCHAT_PASSWORD" <<<"$explicit_log"
grep -Fq "login user 'operator' ready" <<<"$explicit_log"

# A partial override is rejected rather than inventing an unknown half.
unset AIMEE_WEBCHAT_PASSWORD
set +e
partial_log=$(webchat_bootstrap_user)
partial_rc=$?
set -e
[[ $partial_rc -ne 0 ]]
grep -Fq 'must be set together' <<<"$partial_log"

echo "webchat-bootstrap-login-check: ok"

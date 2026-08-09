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

# Minimal containers do not have to export USER.  Resolve the account before the
# fixture replaces id() below so set -u cannot turn an environment detail into a
# release-check failure.
fixture_process_user=${USER:-$(id -un 2>/dev/null || printf '%s' aimee)}

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

# A realistic account table, not "every user exists". The old stub returned 0
# unconditionally, so provisioning always took the already-exists branch and the
# useradd path — the one a FRESH appliance actually walks — was never exercised.
existing_users="$test_dir/existing-users"
printf '%s\n' operator legacy aimee "$fixture_process_user" > "$existing_users"
id() {
  case ${1:-} in
    -nG|-gn|-u) shift ;;
  esac
  [[ -n ${1:-} ]] || return 0
  grep -Fxq "$1" "$existing_users"
}
# The account table the guard now reads. `getent group` alone was never enough to
# answer "can a human sign in?": a wizard-retired account keeps its group
# membership and loses only its shadow verifier, and a container recreate drops
# the accounts while the group definition ships in the image.
group_members="$test_dir/group-members"
# A FRESH appliance. The group does NOT ship in the image -- verified against
# ghcr.io/rakuensoftware/aimee-server:testing, which has no aimee-webchat group and
# no admin account. The old fixture asserted the opposite (getent group always
# succeeded), which is exactly why it could not see a restore path whose usermod
# failed for want of the group. Absent marker = no group yet; groupadd creates it.
printf '' > "$group_members"
group_exists="$test_dir/group-exists"
rm -f "$group_exists"
shadow_hashes="$test_dir/shadow-hashes"   # "<user> <hash>"; absent => a usable hash
: > "$shadow_hashes"
getent() {
  case ${1:-} in
    group)
      [[ ${2:-} == "${WEBCHAT_LOGIN_GROUP:-aimee}" ]] || return 1
      [[ -f $group_exists ]] || return 1
      printf '%s:x:999:%s\n' "$2" "$(cat "$group_members")"
      ;;
    passwd)
      grep -Fxq "${2:-}" "$existing_users" || return 1
      printf '%s:x:1001:1001::/home/%s:/bin/sh\n' "$2" "$2"
      ;;
    shadow)
      grep -Fxq "${2:-}" "$existing_users" || return 1
      _h=$(awk -v u="${2:-}" '$1==u{print $2}' "$shadow_hashes")
      printf '%s:%s:19000:0:99999:7:::\n' "$2" "${_h:-\$6\$salt\$usableverifier}"
      ;;
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
  # -aG <group> <user>: reflect the membership so getent group agrees. Real usermod
  # FAILS on an unknown group; modelling that is what exposes a restore that runs
  # before the group is created.
  if [[ ${1:-} == -aG ]]; then
    [[ -f $group_exists ]] || return 1
    printf '%s,%s\n' "$(cat "$group_members")" "${3:-}" > "$group_members"
  fi
}
useradd() {
  printf 'useradd %s\n' "$*" >> "$cleared_users"
  # the last argument is the account name
  for _u in "$@"; do :; done
  printf '%s\n' "$_u" >> "$existing_users"
  # Provisioning adds the supplementary group separately (usermod -aG); model
  # that here so group membership reflects what actually happened.
}
groupadd() { printf 'x' > "$group_exists"; }
chpasswd() {
  # -e means the payload is a hash, not a plaintext password (the restore path).
  if [[ ${1:-} == -e ]]; then
    printf 'chpasswd -e %s\n' "$(cat)" >> "$cleared_users"
  else
    cat >/dev/null; printf 'chpasswd\n' >> "$cleared_users"
  fi
}
userdel() { :; }

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

migration_log=$(webchat_migrate_legacy_credentials 2>&1)
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
corrupt_log=$(webchat_migrate_legacy_credentials 2>&1)
corrupt_rc=$?
set -e
[[ $corrupt_rc -ne 0 ]]
[[ -f $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
grep -Fq 'invalid' <<<"$corrupt_log"

# Headless mode still performs custody migration before it disables the UI.
find "$AIMEE_HOME/webchat" -type f -delete
export AIMEE_RUNTIME_WEB_ENABLED=0
disabled_log=$(webchat_prepare 2>&1)
grep -Fq 'browser UI disabled' <<<"$disabled_log"

# --- first-boot login generation -------------------------------------------
#
# A clean install supplies no AIMEE_WEBCHAT_USER/PASSWORD (compose carries no
# credential) and has no generated file, because nothing used to write one. That
# left a healthy appliance with no account a human could sign in with — measured
# on a fresh box. aimee must generate one and PRINT it, exactly once.
rm -rf "$AIMEE_HOME/webchat"
mkdir -p "$AIMEE_HOME/webchat"
unset AIMEE_WEBCHAT_USER AIMEE_WEBCHAT_PASSWORD
: > "$cleared_users"
# CLEAN install means the accounts too, not just $AIMEE_HOME. The migration
# section above provisioned a real login and put it in the group; leaving it
# there would (correctly) suppress generation and this section would be testing
# nothing. The old stub hid that by never answering for the login group.
printf '%s\n' operator legacy aimee "$fixture_process_user" > "$existing_users"
printf '' > "$group_members"
: > "$shadow_hashes"

gen_log=$(webchat_provision_bootstrap_account 2>&1)

# It announces the credential, and the credential is a real pair.
# Exactly one credential is announced. More than one would mean more than one
# account was provisioned, and the operator would not know which is live.
[[ $(grep -c 'FIRST-BOOT DASHBOARD LOGIN' <<<"$gen_log") -eq 1 ]]
gen_user=$(sed -n 's/.*username: //p' <<<"$gen_log" | tr -d ' ' | head -1)
gen_pass=$(sed -n 's/.*password: //p' <<<"$gen_log" | tr -d ' ' | head -1)
[[ $gen_user =~ ^aimee-[0-9a-f]{12}$ ]]
[[ ${#gen_pass} -eq 64 ]]

# It provisioned an actual account with that password.
grep -q 'useradd' "$cleared_users"
grep -q 'chpasswd' "$cleared_users"

# Supplementary group membership, not just primary: `getent group` lists only
# supplementary members, so a primary-only account is invisible to
# UserManager.List() and the dashboard's user list comes back empty.
grep -Fq -- "-aG $WEBCHAT_LOGIN_GROUP $gen_user" "$cleared_users"

# The marker records the NAME only; the plaintext never lands on disk.
[[ -f $WEBCHAT_BOOTSTRAP_USER ]]
grep -Fq "generated:$gen_user" "$WEBCHAT_BOOTSTRAP_USER"
[[ ! -e $WEBCHAT_BOOTSTRAP_CREDENTIALS ]]
! grep -rFq "$gen_pass" "$AIMEE_HOME" 2>/dev/null

# The marker must be readable by the C server, which runs as a different user
# and resolves the appliance administrator from it.
marker_mode=$(stat -c %a "$WEBCHAT_BOOTSTRAP_USER")
[[ $(( 8#$marker_mode & 8#044 )) -ne 0 ]]

# A second boot neither regenerates nor reprints: rotating the password would
# lock the operator out of the account they were just handed.
second_log=$(webchat_provision_bootstrap_account 2>&1)
! grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"$second_log"

# --- an upgrade must never leave the appliance with no way in -----------------
#
# THE LOCKOUT, reproduced on a live appliance: PAM identities live in the
# container's writable layer (/etc/passwd and /etc/shadow are on no volume) while
# the markers live in $AIMEE_HOME, which is. `up --force-recreate` onto a new
# image therefore destroyed every account and KEPT the markers saying one exists,
# so provisioning declined to mint a replacement and the operator could not sign
# in. The Vault could not help: only session_hmac and tls_key survive there.
#
# Model exactly that: markers intact, accounts and group membership gone.
: > "$cleared_users"
printf '%s\n' operator legacy aimee "$fixture_process_user" > "$existing_users"   # the image's own users
printf '' > "$group_members"                                       # group ships empty
printf 'generated:%s\n' "$gen_user" > "$WEBCHAT_BOOTSTRAP_USER"
printf 'jbailes\n' > "$WEBCHAT_BOOTSTRAP_REPLACED"
upgrade_log=$(webchat_provision_bootstrap_account 2>&1)
grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"$upgrade_log"
upgrade_user=$(sed -n 's/.*username: //p' <<<"$upgrade_log" | tr -d ' ' | head -1)
[[ $upgrade_user =~ ^aimee-[0-9a-f]{12}$ ]]
[[ $upgrade_user != "$gen_user" ]]
# The stale marker must go too: left behind, it tells the wizard setup is already
# finished while the only working credential is the one just printed.
[[ ! -e $WEBCHAT_BOOTSTRAP_REPLACED ]]

# --- an upgrade must give the operator back THEIR account ---------------------
#
# Not being locked out is only half of it. The operator's projects are filed by
# webuser NAME under /var/lib/aimee-workspaces/webusers/<name>, so handing them a
# fresh generated login after an upgrade leaves the whole tree attached to a user
# nobody signs in as — the same disconnect, by a different route. runtime-web
# records the managed accounts and their verifiers; restore them instead.
rm -rf "$AIMEE_HOME/webchat"; mkdir -p "$AIMEE_HOME/webchat"
: > "$cleared_users"
printf '%s\n' operator legacy aimee "$fixture_process_user" > "$existing_users"   # the image's own users
printf '' > "$group_members"                                       # group ships empty
rm -f "$group_exists"          # ...and does NOT ship at all: a replaced container
printf 'admin:$6$salt$operatorverifier\n' > "$AIMEE_HOME/webchat/identities"
printf 'generated:aimee-000000000000\n' > "$WEBCHAT_BOOTSTRAP_USER"
printf 'admin\n' > "$WEBCHAT_BOOTSTRAP_REPLACED"
restore_log=$(webchat_provision_bootstrap_account 2>&1)
# The operator's own account is back, with its own verifier...
grep -Fq 'useradd' "$cleared_users"
grep -Fq -- "-aG $WEBCHAT_LOGIN_GROUP admin" "$cleared_users"
# ...and is actually IN the group. Assert the RESULT, not the invocation: usermod
# records its arguments before it can fail, so the line above passed even while the
# restore ran ahead of any groupadd and every membership silently failed. An
# unmanaged account is invisible to UserManager.List(), so the next
# snapshotManagedIdentities() rewrites this record without it and the following
# container replacement deletes the operator's account outright. Observed on the
# testing appliance: `admin` lived only in this record and was one snapshot from gone.
getent group "$WEBCHAT_LOGIN_GROUP" | grep -Fq admin
grep -Fq 'chpasswd -e admin:$6$salt$operatorverifier' "$cleared_users"
grep -Fxq admin "$existing_users"
# ...so no replacement credential is minted, and the marker naming them stands.
! grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"$restore_log"
[[ -f $WEBCHAT_BOOTSTRAP_REPLACED ]]

# An account that SURVIVED keeps its current password: the record can be older
# than a password change made since, and restoring over it would roll that back.
: > "$cleared_users"
survivor_log=$(webchat_provision_bootstrap_account 2>&1)
! grep -q 'chpasswd' "$cleared_users"
! grep -q 'useradd' "$cleared_users"
! grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"$survivor_log"

# A record naming a LOCKED verifier is not a login: restoring it would recreate
# an account nobody can authenticate as, and then suppress minting a real one.
rm -rf "$AIMEE_HOME/webchat"; mkdir -p "$AIMEE_HOME/webchat"
: > "$cleared_users"
printf '%s\n' operator legacy aimee "$fixture_process_user" > "$existing_users"
printf '' > "$group_members"
printf 'retired:!$y$locked\n' > "$AIMEE_HOME/webchat/identities"
locked_record_log=$(webchat_provision_bootstrap_account 2>&1)
! grep -Fxq retired "$existing_users"
grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"$locked_record_log"

rm -f "$AIMEE_HOME/webchat/identities"

# --- a retired account is not a usable login ----------------------------------
#
# Replacing the bootstrap login through the wizard LOCKS its shadow entry
# ("!$y...") and leaves the name in the group. Trusting membership alone, the
# appliance then refuses to mint a replacement for an account nobody can
# authenticate as — observed after deleting a probe account left exactly this.
rm -rf "$AIMEE_HOME/webchat"; mkdir -p "$AIMEE_HOME/webchat"
: > "$cleared_users"
printf '%s\n' operator legacy aimee "$fixture_process_user" retired-acct > "$existing_users"
printf 'retired-acct\n' > "$group_members"
printf 'retired-acct !$y$locked\n' > "$shadow_hashes"
locked_log=$(webchat_provision_bootstrap_account 2>&1)
grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"$locked_log"

# ...but a member with a real verifier still suppresses generation, or every
# restart would rotate the password out from under the operator.
rm -rf "$AIMEE_HOME/webchat"; mkdir -p "$AIMEE_HOME/webchat"
: > "$shadow_hashes"
usable_log=$(webchat_provision_bootstrap_account 2>&1)
! grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"$usable_log"

# Restore the baseline table for the checks below.
printf '%s\n' operator legacy aimee "$fixture_process_user" > "$existing_users"
printf '' > "$group_members"
: > "$shadow_hashes"

# An EXISTING account named by an explicit pair must still join the managed
# group. This is the live case: the image ships `aimee` as its service account,
# the documented compose names that same user, so provisioning takes the
# already-exists branch — and without the supplementary add it authenticates but
# never appears in the dashboard's user list.
rm -rf "$AIMEE_HOME/webchat"; mkdir -p "$AIMEE_HOME/webchat"
: > "$cleared_users"
AIMEE_WEBCHAT_USER=aimee AIMEE_WEBCHAT_PASSWORD=aimee-local-dev \
  webchat_provision_bootstrap_account >/dev/null 2>&1
grep -Fq -- "-aG $WEBCHAT_LOGIN_GROUP aimee" "$cleared_users"
! grep -q 'useradd' "$cleared_users"

# An explicit pair short-circuits generation entirely.
rm -rf "$AIMEE_HOME/webchat"; mkdir -p "$AIMEE_HOME/webchat"
AIMEE_WEBCHAT_USER=operator AIMEE_WEBCHAT_PASSWORD=operator-pw \
  explicit_log=$(webchat_provision_bootstrap_account 2>&1) || true
! grep -Fq 'FIRST-BOOT DASHBOARD LOGIN' <<<"${explicit_log:-}"
unset AIMEE_WEBCHAT_USER AIMEE_WEBCHAT_PASSWORD

echo "webchat-vault-migration-check: first-boot generation ok"

# --- stdout stays clean ------------------------------------------------------
#
# server-entrypoint.sh sources this library and then execs whatever command the
# operator passed (`docker run ... sh -c '...'`). Anything the library prints to
# stdout is prepended to that command's output. That is how the build-integrity
# credential-override check broke: the provisioning diagnostics landed in the
# captured output alongside the child's, so it no longer read as "unset".
# Diagnostics belong on stderr; container runtimes collect both as logs anyway.
rm -rf "$AIMEE_HOME/webchat"; mkdir -p "$AIMEE_HOME/webchat"
stray_stdout=$(webchat_provision_bootstrap_account 2>/dev/null) || true
if [[ -n $stray_stdout ]]; then
  echo "webchat-vault-migration-check: provisioning wrote to stdout, which corrupts" >&2
  echo "  an entrypoint command override. Route it through webchat_log (stderr):" >&2
  printf '%s\n' "$stray_stdout" >&2
  exit 1
fi

echo "webchat-vault-migration-check: stdout clean"

echo "webchat-vault-migration-check: ok"

# Fail on anything the stubs recorded. Last, so every check has run first.
if [[ -s $violations ]]; then
  echo "webchat-vault-migration-check: FAILED" >&2
  cat "$violations" >&2
  exit 1
fi

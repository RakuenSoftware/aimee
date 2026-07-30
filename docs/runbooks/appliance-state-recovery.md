# Appliance state recovery

Use this checklist on SmoothNAS/tierd appliances running the `aimee-server`
plugin. It restores an agent registry or replaces damaged workspace Git
metadata; it does not repair storage.

Run every block below, in order, in the same shell as the account that owns the
appliance state and workspace, not with `sudo`. Each block exits at the first
failed prerequisite. Stop at every `STOP` message. The API checks use the local
filesystem-authenticated Unix socket: no bearer is read, exported, or persisted.

## Set appliance paths

```sh
set -eu
export AIMEE_HOME='/path/to/aimee-home'
export AIMEE_HTTP_SOCKET="$AIMEE_HOME/aimee-http.sock"
stop() { printf 'STOP: %s\n' "$1" >&2; exit 1; }

test -d "$AIMEE_HOME" && test "$AIMEE_HOME" != / &&
  test -w "$AIMEE_HOME" || stop 'AIMEE_HOME is not a writable state directory'
test -S "$AIMEE_HTTP_SOCKET" || stop 'local API socket is unavailable'
printf 'AIMEE_HOME=%s\nAIMEE_HTTP_SOCKET=%s\n' \
  "$AIMEE_HOME" "$AIMEE_HTTP_SOCKET"
```

Unix-socket ownership authenticates the local operator. Do not replace these
commands with TCP requests or copy a bearer out of Vault.

## Missing `agents.json`

**Symptom:** `GET /v1/agents` returns HTTP 502 with `agents backend
unavailable`. Do not use `GET /v1/agent/list`; it masks failure as an empty
array.

List the sibling backups, select a trustworthy one, and run:

```sh
set -eu
ls -lht "$AIMEE_HOME"/agents.json.bak-* || stop 'no registry backups found'
export AGENTS_BACKUP="$AIMEE_HOME/agents.json.bak-YYYYMMDD-HHMMSS"
status=$(curl -sS --unix-socket "$AIMEE_HTTP_SOCKET" -o /dev/null \
  -w '%{http_code}' http://localhost/v1/agents) ||
  stop 'agent diagnostic failed'

test "$status" = 502 && test ! -e "$AIMEE_HOME/agents.json" &&
  test -f "$AGENTS_BACKUP" && test -s "$AGENTS_BACKUP" ||
  stop 'symptom, missing file, or selected backup is invalid'

cp -p -- "$AGENTS_BACKUP" "$AIMEE_HOME/agents.json" &&
  touch "$AIMEE_HOME/agents.json" && test -s "$AIMEE_HOME/agents.json" ||
  stop 'registry restoration failed'

status=$(curl -sS --unix-socket "$AIMEE_HTTP_SOCKET" -o /dev/null \
  -w '%{http_code}' http://localhost/v1/agents) ||
  stop 'restored registry check failed'
test "$status" = 200 || stop "restored registry returned HTTP $status"
curl -fsS --unix-socket "$AIMEE_HTTP_SOCKET" \
  http://localhost/v1/agents || stop 'restored registry could not be read'
```

The backup remains intact. API keys remain in Vault under the agent names and
must not be re-entered or persisted.

## Stale `agents.json`

**Symptom:** agent configuration appears absent although `agents.json` is valid,
and its mtime is behind the appliance clock.

```sh
set -eu
test -f "$AIMEE_HOME/agents.json" && test -s "$AIMEE_HOME/agents.json" ||
  stop 'agents.json is missing or empty'
date -Ins || stop 'appliance clock check failed'
old_mtime=$(stat -c %Y "$AIMEE_HOME/agents.json") || stop 'mtime check failed'
sleep 1
touch "$AIMEE_HOME/agents.json" &&
  test "$(stat -c %Y "$AIMEE_HOME/agents.json")" -gt "$old_mtime" ||
  stop 'agents.json cache identity was not refreshed'

status=$(curl -sS --unix-socket "$AIMEE_HTTP_SOCKET" -o /dev/null \
  -w '%{http_code}' http://localhost/v1/agents) ||
  stop 'refreshed registry check failed'
test "$status" = 200 || stop "refreshed registry returned HTTP $status"
curl -fsS --unix-socket "$AIMEE_HTTP_SOCKET" \
  http://localhost/v1/agents || stop 'refreshed registry could not be read'
```

## Corrupt or missing workspace Git metadata

**Symptoms:** proposal polling reports `ls-tree failed ... rc=128`, and forge
operations report `resolve https origin: no origin remote`.

The block below resolves the forge-neutral symbolic `HEAD`, creates and checks a
single-branch clone on the workspace volume, retains the damaged repository,
and installs the checked clone. Do not guess `main`. A private remote must
already be reachable through the appliance's Vault-backed Git credential path;
never add a credential to the URL or environment.

```sh
set -eu
export WORKSPACE='/path/on/tier-bound-volume/to/workspace'
export CANONICAL_URL='https://forge.example/owner/repository.git'
WORKSPACE_PARENT=$(dirname "$WORKSPACE") || stop 'workspace parent lookup failed'

test "${WORKSPACE#/}" != "$WORKSPACE" && test "$WORKSPACE" != / &&
  test -e "$WORKSPACE" && test "$WORKSPACE_PARENT" != / &&
  test -d "$WORKSPACE_PARENT" && test -w "$WORKSPACE_PARENT" ||
  stop 'workspace path or parent is unsafe'
case "$CANONICAL_URL" in
  https://*) ;;
  *) stop 'canonical HTTPS URL required' ;;
esac

remote_head=$(git ls-remote --symref "$CANONICAL_URL" HEAD) ||
  stop 'remote HEAD lookup failed'
DEFAULT_BRANCH=$(printf '%s\n' "$remote_head" |
  sed -n 's#^ref: refs/heads/\([^[:space:]]*\)[[:space:]]*HEAD$#\1#p')
test -n "$DEFAULT_BRANCH" &&
  git ls-remote --exit-code "$CANONICAL_URL" \
    "refs/heads/$DEFAULT_BRANCH" >/dev/null ||
  stop 'authoritative default branch is not readable'

STAGING=$(mktemp -d "$WORKSPACE_PARENT/.aimee-recovery.XXXXXX") ||
  stop 'staging directory creation failed'
test "$(stat -c %d "$WORKSPACE_PARENT")" = "$(stat -c %d "$STAGING")" ||
  stop 'staging directory is not on the workspace volume'
git clone --single-branch --branch "$DEFAULT_BRANCH" \
  "$CANONICAL_URL" "$STAGING/repository" || stop 'health clone failed'

test "$(git -C "$STAGING/repository" remote get-url origin)" = "$CANONICAL_URL" &&
  test "$(git -C "$STAGING/repository" branch --show-current)" = "$DEFAULT_BRANCH" &&
  test "$(git -C "$STAGING/repository" rev-parse --abbrev-ref '@{upstream}')" = \
    "origin/$DEFAULT_BRANCH" &&
  test -z "$(git -C "$STAGING/repository" status --porcelain)" &&
  git -C "$STAGING/repository" fsck --no-dangling &&
  git -C "$STAGING/repository" ls-tree HEAD >/dev/null ||
  stop 'health clone validation failed'

DAMAGED_WORKSPACE="${WORKSPACE}.damaged-$(date +%Y%m%d-%H%M%S)"
test ! -e "$DAMAGED_WORKSPACE" &&
  mv -- "$WORKSPACE" "$DAMAGED_WORKSPACE" ||
  stop 'damaged repository was not retained; clone not installed'
if ! mv -- "$STAGING/repository" "$WORKSPACE"
then
  if test ! -e "$WORKSPACE" && mv -- "$DAMAGED_WORKSPACE" "$WORKSPACE"
  then
    stop 'replacement failed; original workspace restored'
  fi
  stop "replacement failed; retained repository remains at $DAMAGED_WORKSPACE"
fi
rmdir -- "$STAGING" || stop 'staging directory cleanup failed'

test "$(git -C "$WORKSPACE" remote get-url origin)" = "$CANONICAL_URL" &&
  test "$(git -C "$WORKSPACE" branch --show-current)" = "$DEFAULT_BRANCH" &&
  test "$(git -C "$WORKSPACE" rev-parse --abbrev-ref '@{upstream}')" = \
    "origin/$DEFAULT_BRANCH" &&
  test -z "$(git -C "$WORKSPACE" status --porcelain)" &&
  git -C "$WORKSPACE" fsck --no-dangling &&
  git -C "$WORKSPACE" ls-tree HEAD >/dev/null ||
  stop 'installed repository validation failed'
printf 'default branch: %s\ndamaged repository retained at: %s\n' \
  "$DEFAULT_BRANCH" "$DAMAGED_WORKSPACE"
```

Resume normal operation. If either Git error recurs, retain both repositories
and escalate. Do not trigger proposal or forge workflows as tests because they
create external changes. Preserve `$DAMAGED_WORKSPACE` for manual disposition;
do not perform storage repair, migration, or automated recovery here.

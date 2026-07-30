# Appliance state recovery

Use this checklist on SmoothNAS/tierd appliances running the `aimee-server`
plugin. It restores an agent registry or replaces damaged workspace Git
metadata; it does not repair storage.

Run commands as the account that owns the appliance state and workspace, not
with `sudo`. Each block exits the shell at the first failed prerequisite. Stop
at every `STOP` message. Use only the appliance's normal Vault-backed
authentication; never put tokens in commands or environment variables.

## Set appliance paths

```sh
export AIMEE_URL='http://127.0.0.1:8080'
export AIMEE_HOME='/path/to/aimee-home'
stop() { printf 'STOP: %s\n' "$1" >&2; exit 1; }

test -d "$AIMEE_HOME" && test "$AIMEE_HOME" != / &&
  test -w "$AIMEE_HOME" || stop 'AIMEE_HOME is not a writable state directory'
printf 'AIMEE_URL=%s\nAIMEE_HOME=%s\n' "$AIMEE_URL" "$AIMEE_HOME"
```

Add the appliance's normal Vault-backed authorization option to each `curl`
command when the API requires authentication.

## Missing `agents.json`

**Symptom:** `GET /v1/agents` returns HTTP 502 with `agents backend
unavailable`. Do not use `GET /v1/agent/list`; it masks failure as an empty
array.

List the sibling backups, select a trustworthy one, and run:

```sh
ls -lht "$AIMEE_HOME"/agents.json.bak-* || stop 'no registry backups found'
export AGENTS_BACKUP="$AIMEE_HOME/agents.json.bak-YYYYMMDD-HHMMSS"
status=$(curl -sS -o /dev/null -w '%{http_code}' "$AIMEE_URL/v1/agents") ||
  stop 'agent diagnostic failed'

test "$status" = 502 && test ! -e "$AIMEE_HOME/agents.json" &&
  test -f "$AGENTS_BACKUP" && test -s "$AGENTS_BACKUP" ||
  stop 'symptom, missing file, or selected backup is invalid'

cp -p -- "$AGENTS_BACKUP" "$AIMEE_HOME/agents.json" &&
  touch "$AIMEE_HOME/agents.json" && test -s "$AIMEE_HOME/agents.json" ||
  stop 'registry restoration failed'

status=$(curl -sS -o /dev/null -w '%{http_code}' "$AIMEE_URL/v1/agents") ||
  stop 'restored registry check failed'
test "$status" = 200 || stop "restored registry returned HTTP $status"
curl -fsS "$AIMEE_URL/v1/agents" || stop 'restored registry could not be read'
```

The backup remains intact. API keys remain in Vault under the agent names and
must not be re-entered or persisted.

## Stale `agents.json`

**Symptom:** agent configuration appears absent although `agents.json` is valid,
and its mtime is behind the appliance clock.

```sh
test -f "$AIMEE_HOME/agents.json" && test -s "$AIMEE_HOME/agents.json" ||
  stop 'agents.json is missing or empty'
date -Ins || stop 'appliance clock check failed'
old_mtime=$(stat -c %Y "$AIMEE_HOME/agents.json") || stop 'mtime check failed'
sleep 1
touch "$AIMEE_HOME/agents.json" &&
  test "$(stat -c %Y "$AIMEE_HOME/agents.json")" -gt "$old_mtime" ||
  stop 'agents.json cache identity was not refreshed'

status=$(curl -sS -o /dev/null -w '%{http_code}' "$AIMEE_URL/v1/agents") ||
  stop 'refreshed registry check failed'
test "$status" = 200 || stop "refreshed registry returned HTTP $status"
curl -fsS "$AIMEE_URL/v1/agents" || stop 'refreshed registry could not be read'
```

## Corrupt or missing workspace Git metadata

**Symptoms:** proposal polling reports `ls-tree failed ... rc=128`, and forge
operations report `resolve https origin: no origin remote`.

The block below obtains GitHub's authoritative `default_branch`, creates and
checks a single-branch clone on the workspace volume, retains the damaged
repository, and installs the checked clone. Do not guess `main`.

```sh
export WORKSPACE='/path/on/tier-bound-volume/to/workspace'
export CANONICAL_URL='https://github.com/owner/repository.git'
WORKSPACE_PARENT=$(dirname "$WORKSPACE") || stop 'workspace parent lookup failed'

test "${WORKSPACE#/}" != "$WORKSPACE" && test "$WORKSPACE" != / &&
  test -e "$WORKSPACE" && test "$WORKSPACE_PARENT" != / &&
  test -d "$WORKSPACE_PARENT" && test -w "$WORKSPACE_PARENT" ||
  stop 'workspace path or parent is unsafe'
case "$CANONICAL_URL" in
  https://github.com/*/*.git) ;;
  *) stop 'canonical GitHub HTTPS URL required' ;;
esac

DEFAULT_BRANCH=$(gh repo view "$CANONICAL_URL" --json defaultBranchRef \
  --jq '.defaultBranchRef.name') || stop 'GitHub default_branch lookup failed'
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
mv -- "$STAGING/repository" "$WORKSPACE" ||
  stop "install failed; damaged repository remains at $DAMAGED_WORKSPACE"
rmdir -- "$STAGING" || stop 'staging directory cleanup failed'

test "$(git -C "$WORKSPACE" remote get-url origin)" = "$CANONICAL_URL" &&
  test "$(git -C "$WORKSPACE" branch --show-current)" = "$DEFAULT_BRANCH" &&
  test "$(git -C "$WORKSPACE" rev-parse --abbrev-ref '@{upstream}')" = \
    "origin/$DEFAULT_BRANCH" || stop 'installed repository validation failed'
printf 'default branch: %s\ndamaged repository retained at: %s\n' \
  "$DEFAULT_BRANCH" "$DAMAGED_WORKSPACE"
```

Resume normal operation. If either Git error recurs, retain both repositories
and escalate. Do not trigger proposal or forge workflows as tests because they
create external changes. Preserve `$DAMAGED_WORKSPACE` for manual disposition;
do not perform storage repair, migration, or automated recovery here.

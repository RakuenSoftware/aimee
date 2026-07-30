# Appliance state recovery

Use this checklist on SmoothNAS/tierd appliances running the `aimee-server`
plugin. It restores existing state after a lost or stale agent registry, or
after workspace Git metadata is damaged. It does not repair storage.

Run commands as the account that owns the appliance state and workspace, not
with `sudo`. Each block enables fail-closed shell behavior; stop at the first
error. Authentication must come from the appliance's normal Vault-backed
integration. Do not put tokens or API keys in these commands or environment
variables.

## Set and verify appliance paths

```sh
set -eu
export AIMEE_URL='http://127.0.0.1:8080'
export AIMEE_HOME='/path/to/aimee-home'

test -d "$AIMEE_HOME"
test "$AIMEE_HOME" != /
test -w "$AIMEE_HOME"
printf 'AIMEE_URL=%s\nAIMEE_HOME=%s\n' "$AIMEE_URL" "$AIMEE_HOME"
```

If the API requires authentication, use the appliance's normal Vault-backed
authorization mechanism with each `curl` command.

## Missing `agents.json`

**Symptom:** `GET /v1/agents` returns HTTP 502 with
`agents backend unavailable`. Do not use `GET /v1/agent/list` as the
diagnostic; it masks the failure as an empty array.

- [ ] Confirm the failure and the missing file, select a sibling backup, restore
  it without deleting the backup, and verify recovery.

```sh
set -eu
status=$(curl -sS -o /dev/null -w '%{http_code}' "$AIMEE_URL/v1/agents")
test "$status" = 502
test ! -e "$AIMEE_HOME/agents.json"

ls -lht "$AIMEE_HOME"/agents.json.bak-*
export AGENTS_BACKUP="$AIMEE_HOME/agents.json.bak-YYYYMMDD-HHMMSS"
test -f "$AGENTS_BACKUP"
test -s "$AGENTS_BACKUP"

cp -p -- "$AGENTS_BACKUP" "$AIMEE_HOME/agents.json" &&
  touch "$AIMEE_HOME/agents.json" &&
  test -s "$AIMEE_HOME/agents.json"
status=$(curl -sS -o /dev/null -w '%{http_code}' "$AIMEE_URL/v1/agents")
test "$status" = 200
curl -fsS "$AIMEE_URL/v1/agents"
```

API keys remain in Vault under the agent names. Restoring `agents.json` must
not require re-entering or persisting credentials.

## Stale `agents.json`

**Symptom:** agent configuration appears absent although `agents.json` is
present and valid, and its mtime is behind the appliance clock.

- [ ] Verify the file and clock, touch the file to invalidate the
  mtime+size+inode cache identity, and verify recovery.

```sh
set -eu
test -f "$AIMEE_HOME/agents.json"
test -s "$AIMEE_HOME/agents.json"
date -Ins
old_mtime=$(stat -c %Y "$AIMEE_HOME/agents.json")
sleep 1
touch "$AIMEE_HOME/agents.json"
new_mtime=$(stat -c %Y "$AIMEE_HOME/agents.json")
test "$new_mtime" -gt "$old_mtime"
status=$(curl -sS -o /dev/null -w '%{http_code}' "$AIMEE_URL/v1/agents")
test "$status" = 200
curl -fsS "$AIMEE_URL/v1/agents"
```

## Corrupt or missing workspace Git metadata

**Symptoms:** proposal polling reports `ls-tree failed ... rc=128`, and forge
operations report `resolve https origin: no origin remote`.

The replacement is staged and checked on the same volume before the damaged
repository is moved. The remote's symbolic `HEAD` supplies the authoritative
default branch; do not guess `main`.

- [ ] Set the workspace and canonical HTTPS remote, resolve the default branch,
  and stage a healthy single-branch clone beside the workspace.

```sh
set -eu
export WORKSPACE='/path/on/tier-bound-volume/to/workspace'
export CANONICAL_URL='https://github.com/owner/repository.git'

test "${WORKSPACE#/}" != "$WORKSPACE"
test "$WORKSPACE" != /
test -e "$WORKSPACE"
WORKSPACE_PARENT=$(dirname "$WORKSPACE")
test "$WORKSPACE_PARENT" != /
test -d "$WORKSPACE_PARENT"
test -w "$WORKSPACE_PARENT"
case "$CANONICAL_URL" in https://*) ;; *) false ;; esac

remote_head=$(git ls-remote --symref "$CANONICAL_URL" HEAD)
DEFAULT_BRANCH=$(printf '%s\n' "$remote_head" |
  sed -n 's#^ref: refs/heads/\([^[:space:]]*\)[[:space:]]*HEAD$#\1#p')
test -n "$DEFAULT_BRANCH"
git ls-remote --exit-code "$CANONICAL_URL" \
  "refs/heads/$DEFAULT_BRANCH" >/dev/null

STAGING=$(mktemp -d "$WORKSPACE_PARENT/.aimee-recovery.XXXXXX")
test "$(stat -c %d "$WORKSPACE_PARENT")" = "$(stat -c %d "$STAGING")"
git clone --single-branch --branch "$DEFAULT_BRANCH" \
  "$CANONICAL_URL" "$STAGING/repository"

test "$(git -C "$STAGING/repository" remote get-url origin)" = "$CANONICAL_URL"
test "$(git -C "$STAGING/repository" branch --show-current)" = "$DEFAULT_BRANCH"
test "$(git -C "$STAGING/repository" rev-parse --abbrev-ref '@{upstream}')" = \
  "origin/$DEFAULT_BRANCH"
test -z "$(git -C "$STAGING/repository" status --porcelain)"
git -C "$STAGING/repository" fsck --no-dangling
git -C "$STAGING/repository" ls-tree HEAD >/dev/null
```

- [ ] Only after the block above succeeds, retain the damaged repository and
  install the staged clone. If the final move fails, the guarded rollback
  restores the original workspace path.

```sh
set -eu
test -d "$STAGING/repository/.git"
DAMAGED_WORKSPACE="${WORKSPACE}.damaged-$(date +%Y%m%d-%H%M%S)"
test ! -e "$DAMAGED_WORKSPACE"

mv -- "$WORKSPACE" "$DAMAGED_WORKSPACE"
if mv -- "$STAGING/repository" "$WORKSPACE"
then
  rmdir -- "$STAGING"
else
  if test ! -e "$WORKSPACE" && mv -- "$DAMAGED_WORKSPACE" "$WORKSPACE"
  then
    printf '%s\n' 'replacement failed; original workspace restored' >&2
  else
    printf 'replacement failed; retained repository remains at %s\n' \
      "$DAMAGED_WORKSPACE" >&2
  fi
  false
fi

test "$(git -C "$WORKSPACE" remote get-url origin)" = "$CANONICAL_URL"
test "$(git -C "$WORKSPACE" branch --show-current)" = "$DEFAULT_BRANCH"
test "$(git -C "$WORKSPACE" rev-parse --abbrev-ref '@{upstream}')" = \
  "origin/$DEFAULT_BRANCH"
test -z "$(git -C "$WORKSPACE" status --porcelain)"
git -C "$WORKSPACE" fsck --no-dangling
git -C "$WORKSPACE" ls-tree HEAD >/dev/null
printf 'damaged repository retained at %s\n' "$DAMAGED_WORKSPACE"
```

- [ ] Resume normal operation. If either Git error recurs, retain both
  repositories and escalate. Do not trigger a proposal or forge workflow merely
  as a recovery test because those workflows create external changes.

Preserve `$DAMAGED_WORKSPACE` for manual disposition. Do not perform storage
repair, migration, or automated recovery as part of this procedure.

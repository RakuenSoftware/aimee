# Appliance state recovery

Use this checklist on SmoothNAS/tierd appliances running the `aimee-server` plugin when agent state is missing or stale, or when an affected workspace has corrupt or missing Git metadata. It restores existing state; it does not repair the underlying tier or storage.

## Prerequisites

- [ ] Log in as the account that owns the appliance state and workspace files. Do not run these commands as another account or with `sudo`.
- [ ] Confirm the appliance clock is correct and the tier-bound volume containing appliance state is mounted and writable.
- [ ] Set the appliance API address and state directory. Do not continue until both values are correct.

`AIMEE_HOME` is the appliance state directory.

```sh
export AIMEE_URL='http://127.0.0.1:8080'
export AIMEE_HOME='/path/to/aimee-home'

printf 'AIMEE_URL=%s\nAIMEE_HOME=%s\n' "$AIMEE_URL" "$AIMEE_HOME"
test -d "$AIMEE_HOME"
```

If the API requires authentication, add the appliance's normal authorization option to each `curl` command.

## Missing `agents.json`

**Distinguishing symptom:** `GET /v1/agents` returns HTTP 502 with `agents backend unavailable`. `GET /v1/agent/list` masks this failure as an empty array and is not a valid diagnostic.

- [ ] Reproduce the authoritative symptom and confirm the file is absent.

```sh
curl -i "$AIMEE_URL/v1/agents"
test ! -e "$AIMEE_HOME/agents.json"
```

- [ ] List sibling backups, inspect their timestamps, and explicitly select the intended backup. Stop if no trustworthy backup exists.

```sh
ls -lht "$AIMEE_HOME"/agents.json.bak-*
export AGENTS_BACKUP="$AIMEE_HOME/agents.json.bak-YYYYMMDD-HHMMSS"
test -f "$AGENTS_BACKUP"
test -s "$AGENTS_BACKUP"
ls -l "$AGENTS_BACKUP"
```

- [ ] Restore the selected backup without removing it, then touch the restored file. `touch` invalidates the server's mtime+size+inode cache identity.

```sh
cp -p "$AGENTS_BACKUP" "$AIMEE_HOME/agents.json"
touch "$AIMEE_HOME/agents.json"
ls -li "$AIMEE_HOME/agents.json"
curl -i "$AIMEE_URL/v1/agents"
```

- [ ] Verify `GET /v1/agents` now returns HTTP 200 and the expected agents.

API keys remain in the vault under the agent names and do not need to be re-entered after `agents.json` is restored.

## Stale `agents.json`

**Distinguishing symptom:** agent configuration appears absent even though
`$AIMEE_HOME/agents.json` is present and valid, and the file's mtime is in the
past relative to the appliance clock.

- [ ] Confirm the file is present and non-empty, compare the appliance clock with
  its timestamp, record the old timestamp, touch the file, and confirm its
  timestamp changed.

```sh
test -f "$AIMEE_HOME/agents.json"
test -s "$AIMEE_HOME/agents.json"
date -Ins
stat "$AIMEE_HOME/agents.json"
old_mtime=$(stat -c %Y "$AIMEE_HOME/agents.json")
sleep 1
touch "$AIMEE_HOME/agents.json"
new_mtime=$(stat -c %Y "$AIMEE_HOME/agents.json")
printf 'old_mtime=%s new_mtime=%s\n' "$old_mtime" "$new_mtime"
test "$new_mtime" -gt "$old_mtime"
stat "$AIMEE_HOME/agents.json"
```

- [ ] Recheck the authoritative endpoint and verify HTTP 200 with current agent state.

```sh
curl -i "$AIMEE_URL/v1/agents"
```

## Corrupt or missing workspace Git metadata

**Distinguishing symptoms:** proposal polling repeatedly logs `ls-tree failed ... rc=128`, and forge operations log `resolve https origin: no origin remote`.

Do not replace the workspace until a fresh clone has passed all checks on the same tier-bound volume.

- [ ] Confirm Git credentials can read the canonical HTTPS repository, then set and validate the repository-specific values.

`WORKSPACE` is the affected repository path on the tier-bound volume; `CANONICAL_URL` is its canonical HTTPS repository URL; and `DEFAULT_BRANCH` is the branch the appliance should track.

```sh
export WORKSPACE='/path/on/tier-bound-volume/to/workspace'
export CANONICAL_URL='https://forge.example/owner/repository.git'
export DEFAULT_BRANCH='main'

printf 'WORKSPACE=%s\nCANONICAL_URL=%s\nDEFAULT_BRANCH=%s\n' \
  "$WORKSPACE" "$CANONICAL_URL" "$DEFAULT_BRANCH"
test "${WORKSPACE#/}" != "$WORKSPACE"
case "$CANONICAL_URL" in https://*) ;; *) false ;; esac
test -n "$DEFAULT_BRANCH"
git ls-remote --exit-code "$CANONICAL_URL" "refs/heads/$DEFAULT_BRANCH" >/dev/null
```

- [ ] Recheck the configured values and establish a temporary path beside the workspace so the test uses the same volume.

```sh
printf 'WORKSPACE=%s\nCANONICAL_URL=%s\nDEFAULT_BRANCH=%s\n' \
  "$WORKSPACE" "$CANONICAL_URL" "$DEFAULT_BRANCH"
WORKSPACE_PARENT=$(dirname "$WORKSPACE")
test -d "$WORKSPACE_PARENT" && test -w "$WORKSPACE_PARENT"
TEST_CLONE=$(mktemp -d "$WORKSPACE_PARENT/.aimee-recovery.XXXXXX")
export TEST_CLONE
printf 'TEST_CLONE=%s\n' "$TEST_CLONE"
test "$(stat -c %d "$WORKSPACE_PARENT")" = "$(stat -c %d "$TEST_CLONE")"
```

- [ ] Clone only the default branch and verify the working tree, canonical `origin`, and upstream tracking branch.

```sh
git clone --single-branch --branch "$DEFAULT_BRANCH" "$CANONICAL_URL" "$TEST_CLONE/repository" &&
  git -C "$TEST_CLONE/repository" status --short --branch &&
  test "$(git -C "$TEST_CLONE/repository" remote get-url origin)" = "$CANONICAL_URL" &&
  test "$(git -C "$TEST_CLONE/repository" branch --show-current)" = "$DEFAULT_BRANCH" &&
  test "$(git -C "$TEST_CLONE/repository" rev-parse --abbrev-ref '@{upstream}')" = "origin/$DEFAULT_BRANCH" &&
  git -C "$TEST_CLONE/repository" fsck --no-dangling &&
  git -C "$TEST_CLONE/repository" ls-tree HEAD >/dev/null
```

- [ ] Only after every test above succeeds, remove the temporary clone. The guarded sequence below must confirm that the damaged repository was moved and the original path is absent before cloning. If this final clone fails, it removes the destination only when the clone created it, then immediately restores the retained repository to the configured workspace path.

```sh
rm -rf "$TEST_CLONE"
DAMAGED_WORKSPACE="${WORKSPACE}.damaged-$(date +%Y%m%d-%H%M%S)"
export DAMAGED_WORKSPACE
if test -e "$WORKSPACE" &&
   test ! -e "$DAMAGED_WORKSPACE" &&
   mv "$WORKSPACE" "$DAMAGED_WORKSPACE" &&
   test -e "$DAMAGED_WORKSPACE" &&
   test ! -e "$WORKSPACE"
then
  if git clone --single-branch --branch "$DEFAULT_BRANCH" "$CANONICAL_URL" "$WORKSPACE"
  then
    printf 'damaged repository retained at %s\n' "$DAMAGED_WORKSPACE"
  else
    # The workspace was confirmed absent immediately before git clone, so any
    # destination now at that path was created by the failed clone.
    if { test ! -e "$WORKSPACE" || rm -rf "$WORKSPACE"; } &&
       test -e "$DAMAGED_WORKSPACE" &&
       mv "$DAMAGED_WORKSPACE" "$WORKSPACE"
    then
      printf '%s\n' 'clone failed; retained repository restored to workspace path' >&2
    else
      printf 'clone failed; automatic rollback failed; retained repository remains at %s\n' \
        "$DAMAGED_WORKSPACE" >&2
    fi
    false
  fi
else
  printf '%s\n' 'repository retention failed; clone was not attempted' >&2
  false
fi
```

- [ ] Verify the replacement repository.

```sh
test "$(git -C "$WORKSPACE" remote get-url origin)" = "$CANONICAL_URL" &&
  test "$(git -C "$WORKSPACE" branch --show-current)" = "$DEFAULT_BRANCH" &&
  test "$(git -C "$WORKSPACE" rev-parse --abbrev-ref '@{upstream}')" = "origin/$DEFAULT_BRANCH" &&
  git -C "$WORKSPACE" status --short --branch &&
  git -C "$WORKSPACE" fsck --no-dangling &&
  git -C "$WORKSPACE" ls-tree HEAD >/dev/null
```

- [ ] Confirm the direct checks above completed without either reported Git error:
  `git ls-tree HEAD` must not report `ls-tree failed ... rc=128`, and
  `git remote get-url origin` must return the canonical HTTPS URL rather than
  `resolve https origin: no origin remote`. Do not trigger a proposal or forge
  workflow as a recovery test; those workflows can create external changes such
  as branches and pull requests.

- [ ] Preserve `$DAMAGED_WORKSPACE` for later manual disposition after recording
  its path. Do not delete it as part of this recovery procedure. If any direct
  check fails or either Git error recurs during normal operation, retain both
  repositories and escalate.

Do not attempt storage repair, migration, or automated recovery as part of this procedure.

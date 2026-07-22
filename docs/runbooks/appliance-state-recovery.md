# Runbook: appliance state recovery

Operator runbook for restoring a healthy `agents.json` and workspace `.git` on an
appliance. Each section below is a checkbox procedure; every command is wrapped
in a copyable shell block so it can be pasted unmodified.

## 1. Purpose and scope

This runbook covers three concrete failure modes that affect appliance state:

1. `agents.json` is **lost or absent** (the file does not exist on disk).
2. `agents.json` is **stale but present** (the file exists but does not match
   the live agent set).
3. the workspace **`.git` is corrupt or lost** (history cannot be read or is
   physically missing).

It is incident-oriented: minimal prose, commands-first. It does **not** cover
secrets rotation (see `vault-master-key-rotation.md`), image/cutover (see
`unified-llm-cutover.md`), or any change that mutates production data outside
the explicit recovery writes here.

## 2. Prerequisites and operator-supplied variables

Before any step below: you need shell access on the appliance host (or the
container that owns the workspace) and read/write on the appliance home. Stop
any process that holds the workspace open before step 6.

| Variable | Meaning | Example |
| --- | --- | --- |
| `APPLIANCE_HOME` | Root of the appliance state (contains `agents.json` and the workspace) | `/var/lib/aimee` |
| `AGENTS_JSON_PATH` | Resolved path to `agents.json` (`${APPLIANCE_HOME}/agents.json` by default) | `/var/lib/aimee/agents.json` |
| `AGENTS_JSON_SOURCE` | Upstream source of truth for agents (a path, URL, or git ref) | `git@aimee:agents/agents.json` |
| `WORKSPACE_DIR` | Workspace directory whose `.git` is being repaired | `${APPLIANCE_HOME}/workspace` |
| `BACKUP_DIR` | A scratch directory for pre-recovery snapshots | `/var/tmp/aimee-recovery-$(date +%s)` |

```sh
# Export once per session.
export APPLIANCE_HOME="/var/lib/aimee"
export AGENTS_JSON_PATH="${APPLIANCE_HOME}/agents.json"
export AGENTS_JSON_SOURCE="git@aimee:agents/agents.json"
export WORKSPACE_DIR="${APPLIANCE_HOME}/workspace"
export BACKUP_DIR="/var/tmp/aimee-recovery-$(date +%s)"
mkdir -p "${BACKUP_DIR}"
```

Confirmation gates: `# Confirm you can reach the host and own the paths.`

## 3. Failure-mode identification

Run the four checks below and read the result table that follows to pick the
matching procedure (§4, §5, or §6).

```sh
[ -e "${AGENTS_JSON_PATH}" ]   && echo "agents.json: present"   || echo "agents.json: absent"
[ -f "${AGENTS_JSON_PATH}" ]   && jq -e . "${AGENTS_JSON_PATH}" >/dev/null && echo "agents.json: valid JSON" || echo "agents.json: invalid/empty"
[ -d "${WORKSPACE_DIR}/.git" ] && echo "workspace .git: present" || echo "workspace .git: absent"
( cd "${WORKSPACE_DIR}" && git fsck --no-progress --no-reflogs 2>/dev/null ) && echo "workspace .git: fsck clean" || echo "workspace .git: fsck failed"
```

| Observable signal | Failure mode | Procedure |
| --- | --- | --- |
| `agents.json: absent` (file does not exist) | Lost or absent `agents.json` | §4 |
| `agents.json: invalid/empty` (file present but not valid JSON) | Lost or absent `agents.json` (treat as missing) | §4 |
| `agents.json: valid JSON` **and** schema/agent list disagrees with the live set | Stale-but-present `agents.json` | §5 |
| `workspace .git: absent` **or** `workspace .git: fsck failed` | Corrupt or lost workspace `.git` | §6 |

If more than one row matches, run the lower-numbered procedure first (state
files before history).

Confirmation gates: `# Confirm only one failure mode applies before continuing.`

## 4. Lost or absent agents.json

Recovery reconstructs `agents.json` from the upstream source of truth. If the
upstream is unreachable, fall back to the documented agent defaults block in
`appliance/agents/defaults.json` inside this repo.

Steps:

- [ ] **Confirm absence and capture a pre-recovery snapshot.** Even though the
      file is missing, capture the directory listing and any sibling artifacts
      so the recovery is auditable.

```sh
test ! -e "${AGENTS_JSON_PATH}" && echo "confirmed: agents.json absent at ${AGENTS_JSON_PATH}"
mkdir -p "${BACKUP_DIR}"
( cd "${APPLIANCE_HOME}" && tar --exclude='*.sock' -cf "${BACKUP_DIR}/appliance-home.tar" . )
```

- [ ] **Pull the canonical agents.json from the upstream source.** Adjust the
      transport (`git`, `curl`, `scp`) to whatever `AGENTS_JSON_SOURCE` actually
      is.

```sh
case "${AGENTS_JSON_SOURCE}" in
  git@*|git://*|https://*.git)
    git clone --depth=1 -- "${AGENTS_JSON_SOURCE}" "${BACKUP_DIR}/agents-source"
    cp -f "${BACKUP_DIR}/agents-source/agents.json" "${AGENTS_JSON_PATH}"
    ;;
  http://*|https://*)
    curl -fsSL -o "${AGENTS_JSON_PATH}" "${AGENTS_JSON_SOURCE}"
    ;;
  *)
    cp -f "${AGENTS_JSON_SOURCE}" "${AGENTS_JSON_PATH}"
    ;;
esac
chmod 0644 "${AGENTS_JSON_PATH}"
```

- [ ] **Fall back to repo defaults when the upstream is unreachable.** Only when
      `AGENTS_JSON_SOURCE` is empty or every branch above failed.

```sh
DEFAULTS="appliance/agents/defaults.json"
if [ ! -s "${AGENTS_JSON_PATH}" ] && [ -f "${DEFAULTS}" ]; then
  jq . "${DEFAULTS}" > "${AGENTS_JSON_PATH}"
fi
test -s "${AGENTS_JSON_PATH}" || { echo "FATAL: no agents.json produced"; exit 1; }
```

- [ ] **Validate the reconstructed file before any process touches it.**

```sh
jq -e 'type == "object" and (.agents | type == "array")' "${AGENTS_JSON_PATH}" >/dev/null \
  || { echo "FATAL: reconstructed agents.json is not an object with an agents[] array"; exit 1; }
jq . "${AGENTS_JSON_PATH}" >/dev/null || { echo "FATAL: agents.json is not valid JSON"; exit 1; }
```

Confirmation gates: `# Reconstructed file passes jq + schema checks.`

## 5. Stale-but-present agents.json

Reconcile a present-but-wrong file against the live set and the upstream
source. Do not overwrite on this procedure; always write through a backup.

Steps:

- [ ] **Snapshot the current agents.json so the rewrite is reversible.**

```sh
mkdir -p "${BACKUP_DIR}"
cp -p "${AGENTS_JSON_PATH}" "${BACKUP_DIR}/agents.json.before"
ls -la "${AGENTS_JSON_PATH}" "${BACKUP_DIR}/agents.json.before"
```

- [ ] **Build the live agent set from process/runtime signals.** Source the
      script from wherever the appliance exposes it; the example below
      inspects the supervisor socket as a stand-in.

```sh
LIVE_JSON="${BACKUP_DIR}/agents.live.json"
if command -v aimee-supervisor-ctl >/dev/null 2>&1; then
  aimee-supervisor-ctl --format=json agents > "${LIVE_JSON}" 2>/dev/null || echo '[]' > "${LIVE_JSON}"
elif [ -S "${APPLIANCE_HOME}/run/supervisor.sock" ]; then
  curl -fsS --unix-socket "${APPLIANCE_HOME}/run/supervisor.sock" \
       "http://localhost/agents" > "${LIVE_JSON}" 2>/dev/null || echo '[]' > "${LIVE_JSON}"
else
  echo '[]' > "${LIVE_JSON}"
fi
jq -e 'type == "array"' "${LIVE_JSON}" >/dev/null || echo '[]' > "${LIVE_JSON}"
```

- [ ] **Pull the upstream canonical agents.json into a sibling file (do not
      overwrite yet).**

```sh
UPSTREAM_JSON="${BACKUP_DIR}/agents.upstream.json"
case "${AGENTS_JSON_SOURCE}" in
  git@*|git://*|https://*.git)
    git clone --depth=1 -- "${AGENTS_JSON_SOURCE}" "${BACKUP_DIR}/agents-source"
    cp -f "${BACKUP_DIR}/agents-source/agents.json" "${UPSTREAM_JSON}"
    ;;
  http://*|https://*)
    curl -fsSL -o "${UPSTREAM_JSON}" "${AGENTS_JSON_SOURCE}"
    ;;
  *)
    cp -f "${AGENTS_JSON_SOURCE}" "${UPSTREAM_JSON}"
    ;;
esac
test -s "${UPSTREAM_JSON}" || { echo "FATAL: could not pull upstream agents.json"; exit 1; }
```

- [ ] **Reconcile: prefer upstream, but keep any locally-registered agent that
      is in the live set and absent upstream.** The merge is explicit —
      operator eyeballs the diff before accepting.

```sh
RECONCILED="${BACKUP_DIR}/agents.reconciled.json"
jq -s '
  (.[0].agents // []) as $upstream
  | (.[1] // []) as $live
  | {
      schema: (.[0].schema // "aimee-agents/v1"),
      agents: (
        ($upstream + ($live | map(select(.id as $id | ($upstream | map(.id) | index($id)) == null))))
      )
    }
' "${UPSTREAM_JSON}" "${LIVE_JSON}" > "${RECONCILED}"
jq . "${RECONCILED}"
```

- [ ] **Diff the reconciled version against the on-disk file; abort if the
      diff exceeds 10% of the file.**

```sh
diff -u "${AGENTS_JSON_PATH}" "${RECONCILED}" | tee "${BACKUP_DIR}/agents.diff" | head -200
RECON_LINES=$(jq -r '.agents | length' "${RECONCILED}")
FILE_LINES=$(jq -r '.agents | length' "${AGENTS_JSON_PATH}")
awk -v a="${RECON_LINES}" -v b="${FILE_LINES}" 'BEGIN{ if (b==0){exit 0} d=(a-b)/b; if (d<0) d=-d; exit (d>0.10)?1:0 }' \
  || { echo "FATAL: reconciled file diff exceeds 10% -- escalate (see §8)"; exit 1; }
```

- [ ] **Install the reconciled file atomically** (write to a sibling, then
      rename).

```sh
INSTALL_TMP="${AGENTS_JSON_PATH}.reconciled.tmp"
install -m 0644 "${RECONCILED}" "${INSTALL_TMP}"
mv -f "${INSTALL_TMP}" "${AGENTS_JSON_PATH}"
chmod 0644 "${AGENTS_JSON_PATH}"
jq -e 'type == "object" and (.agents | type == "array")' "${AGENTS_JSON_PATH}" >/dev/null
```

Confirmation gates: `# Reconciled file written, diff archived, jq validates.`

## 6. Corrupt or lost workspace .git

Repair or reconstruct the workspace's git state. Preservation order: validate
what is recoverable first, then fsck, then rebuild only what is necessary.

Steps:

- [ ] **Stop every process that has the workspace open.** A repair against a
      live working tree is not safe.

```sh
pkill -STOP -f "${WORKSPACE_DIR}" 2>/dev/null || true
systemctl --quiet is-active aimee-supervisor.service \
  && systemctl stop aimee-supervisor.service || true
```

- [ ] **Snapshot the workspace as-is before any repair.** The snapshot is your
      rollback target.

```sh
( cd "$(dirname "${WORKSPACE_DIR}")" && tar --exclude='*.sock' -cf "${BACKUP_DIR}/workspace.tar" "$(basename "${WORKSPACE_DIR}")" )
ls -la "${BACKUP_DIR}/workspace.tar"
```

- [ ] **Run `git fsck` and capture the report.** This is your evidence the
      directory really is corrupt — do not skip it; escalation (§8) needs it.

```sh
if [ -d "${WORKSPACE_DIR}/.git" ]; then
  ( cd "${WORKSPACE_DIR}" && git fsck --no-progress --no-reflogs --strict 2>&1 | tee "${BACKUP_DIR}/fsck.log" ) || true
else
  echo "FATAL: ${WORKSPACE_DIR}/.git absent -- skip fsck, jump to reconstruction" | tee "${BACKUP_DIR}/fsck.log"
fi
```

- [ ] **If `.git` is present, attempt in-place repair with the standard git
      recovery tools.** Run all three; stop on the first that produces a clean
      fsck.

```sh
if [ -d "${WORKSPACE_DIR}/.git" ]; then
  ( cd "${WORKSPACE_DIR}" && git update-ref --no-deref --no-head HEAD HEAD ) || true
  ( cd "${WORKSPACE_DIR}" && git reflog expire --expire=now --all ) || true
  ( cd "${WORKSPACE_DIR}" && git gc --prune=now --aggressive ) || true
  ( cd "${WORKSPACE_DIR}" && git fsck --no-progress --no-reflogs --strict ) \
    && { echo "recovery: gc + reflog repair succeeded"; } || echo "recovery: gc + reflog repair did NOT clear fsck"
fi
```

- [ ] **If `.git` is absent, re-clone from the upstream remote and rebuild the
      working tree on top.** Only do this when no other clone exists on disk.

```sh
if [ ! -d "${WORKSPACE_DIR}/.git" ]; then
  ORIGIN_URL="$(git -C "${APPLIANCE_HOME}" config --get remote.origin.url 2>/dev/null || true)"
  if [ -z "${ORIGIN_URL}" ]; then
    ORIGIN_URL="${AGENTS_JSON_SOURCE%/agents.json}"
  fi
  mv "${WORKSPACE_DIR}" "${BACKUP_DIR}/workspace.missing"
  git clone -- "${ORIGIN_URL}" "${WORKSPACE_DIR}"
fi
```

- [ ] **Restore any non-tracked local state from the workspace snapshot**
      (config, hooks, `.git/local`). This is the only step that copies files
      back from the pre-repair tar.

```sh
( cd "${WORKSPACE_DIR}" && tar -xf "${BACKUP_DIR}/workspace.tar" \
    --wildcards '*/.git/hooks/*' \
    --wildcards '*/.git/config' \
    --wildcards '*/.git/local/*' 2>/dev/null ) || true
git -C "${WORKSPACE_DIR}" config --local --get-regexp '.*' | head -50 > "${BACKUP_DIR}/git-config.snapshot"
```

- [ ] **Re-run `git fsck` to confirm the workspace is healthy before resuming
      service.**

```sh
( cd "${WORKSPACE_DIR}" && git fsck --no-progress --no-reflogs --strict ) \
  || { echo "FATAL: workspace still fails fsck -- escalate (see §8)"; }
git -C "${WORKSPACE_DIR}" status --porcelain > "${BACKUP_DIR}/status.porcelain"
```

- [ ] **Resume service.**

```sh
pkill -CONT -f "${WORKSPACE_DIR}" 2>/dev/null || true
systemctl --quiet is-enabled aimee-supervisor.service \
  && systemctl start aimee-supervisor.service || true
```

Confirmation gates: `# Final fsck clean; supervisor back in active state.`

## 7. Final verification

Run this block exactly once after whichever procedure above succeeded. Every
check must pass; on any failure, jump to §8.

- [ ] **`agents.json` exists, is valid JSON, and matches the documented
      schema.**

```sh
test -s "${AGENTS_JSON_PATH}"
jq -e 'type == "object" and (.agents | type == "array" and length > 0)' "${AGENTS_JSON_PATH}" >/dev/null
jq . "${AGENTS_JSON_PATH}" >/dev/null
```

- [ ] **Live agent count and on-disk agent count agree.**

```sh
LIVE_COUNT=$([ -S "${APPLIANCE_HOME}/run/supervisor.sock" ] \
  && curl -fsS --unix-socket "${APPLIANCE_HOME}/run/supervisor.sock" "http://localhost/agents" 2>/dev/null \
     | jq 'length' || echo 0)
FILE_COUNT=$(jq '.agents | length' "${AGENTS_JSON_PATH}")
[ "${LIVE_COUNT}" -gt 0 ] && [ "${FILE_COUNT}" -gt 0 ]
[ "${LIVE_COUNT}" -eq "${FILE_COUNT}" ] || echo "WARN: live/file agent count differ (${LIVE_COUNT} vs ${FILE_COUNT})"
```

- [ ] **Workspace git is healthy and the working tree is clean.**

```sh
git -C "${WORKSPACE_DIR}" fsck --no-progress --no-reflogs --strict
git -C "${WORKSPACE_DIR}" rev-parse --verify HEAD >/dev/null
git -C "${WORKSPACE_DIR}" status --porcelain
```

- [ ] **Recovery artifacts are archived to `${BACKUP_DIR}` and listed for the
      postmortem.**

```sh
ls -la "${BACKUP_DIR}"
echo "RECOVERY_ARTIFACTS=${BACKUP_DIR}" >> "${BACKUP_DIR}/MANIFEST"
```

- [ ] **Operator records the incident closure note** (date, procedure run, link
      to `${BACKUP_DIR}/MANIFEST`) in the on-call channel.

Confirmation gates: `# All four checks above completed without aborts.`

## 8. Escalation/stop conditions

Stop and escalate to the platform owner **immediately** when any of the
following is true. Do not continue past the point where the condition was hit;
do not invent workarounds.

- [ ] The reconstructed `agents.json` is empty, invalid JSON, or fails the jq
      schema check after the §4 / §5 install step. Treatment requires re-pull
      from a verified source; escalate to whoever owns `AGENTS_JSON_SOURCE`.
- [ ] The reconciled diff in §5 exceeds the 10% threshold and the operator
      cannot explain the divergence in two sentences. Escalate before writing
      the file; the on-disk state must not be overwritten by an unverified
      merge.
- [ ] `git fsck` reports **dangling objects**, **missing tree**, or
      **unreachable commits** that the §6 recovery does **not** clear. This is
      a sign of disk/storage damage beyond a single workspace — escalate.
- [ ] Step §6 re-clones a workspace that has no `AGENTS_JSON_SOURCE`-derivable
      upstream and no `.git/config` snapshot to recover from. Escalate; do not
      pick an arbitrary remote.
- [ ] The supervisor service will not start after §6 completes (the workspace
      is "fixed" but the appliance is still down). Escalate; recovery is
      incomplete.
- [ ] Any step above produces a non-zero exit that the runbook did not
      anticipate (i.e. a `FATAL:` printed from a guard that was supposed to be
      impossible). Capture the full transcript into `${BACKUP_DIR}` and
      escalate.
- [ ] The on-call channel confirms another operator is already inside the same
      appliance. Stop, post a handoff in the channel, wait for ack; do not
      interleave recovery writes.

Confirmation gates: `# Escalation ticket open; ${BACKUP_DIR} archived.`

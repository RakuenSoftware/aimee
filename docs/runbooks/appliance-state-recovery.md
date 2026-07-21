# Runbook: appliance state recovery

Operator incident-response runbook for recovering an on-disk appliance whose
**agent registration** (`agents.json`) or **workspace git repository**
(workspace `.git`) has drifted, gone missing, or been corrupted. Three
concrete failure modes are covered; triage in §3 picks one and the matching
section runs.

This is a controlled, on-host recovery. It is **never** the right path for
agent *credentials* — those live in the vault (see
[`vault-master-key-rotation.md`](vault-master-key-rotation.md)) and use the
`aimee agent key import` workflow.

## 1. Purpose and scope

Bring an appliance back to a coherent state when the *registry of agents*
or the *workspace git store* is inconsistent, without rebuilding the host and
without rotating keys. Scope:

- **In scope:** `agents.json` recovery (lost, stale), workspace `.git`
  recovery (corrupt, lost), verification, escalation.
- **Out of scope:** vault credential rotation (use the vault runbook),
  model/capability swaps (use the cutover runbook), schema migration, host
  re-imaging.

## 2. Prerequisites and operator-supplied variables

Before any section below, confirm you have:

- [ ] **SSH/console access** to the host as a user that can read/write
      `AIMEE_HOME` (typically `aimee` or `root`).
- [ ] **Backup medium** mounted read-write (USB / NFS) with at least
      `2 × sizeof($AIMEE_HOME)` free, to take a pre-recovery snapshot.
- [ ] **No live `aimee-server`** writing to the appliance state. Stop it
      first so a mid-recovery write cannot race the operator.
- [ ] **The canonical upstream source** for `agents.json` (the operator's
      known-good copy on the backup medium, or the appliance's documented
      defaults).

Operator-supplied variables used by every section below. **Set them in
your shell once** so the fenced blocks run unchanged:

```sh
# Where the appliance lives on disk.
export AIMEE_HOME="/var/lib/aimee"

# The workspace git repository to recover. Defaults to the canonical
# workspace; override if this host runs a non-default workspace.
export WORKSPACE_DIR="${AIMEE_HOME}/workspace"

# Backup medium (mounted read-write). A pre-recovery snapshot is
# written under this directory.
export BACKUP_DIR="/mnt/incident-backup/$(date -u +%Y%m%dT%H%M%SZ)"

# Canonical source for agents.json (path on the backup medium, or a
# URL the operator has verified out-of-band).
export AGENTS_JSON_SOURCE="/mnt/known-good/agents.json"
```

| Variable            | Purpose                                              |
| ------------------- | ---------------------------------------------------- |
| `AIMEE_HOME`        | Appliance root containing the vault + state files.   |
| `WORKSPACE_DIR`     | Directory whose `.git` is being repaired.            |
| `BACKUP_DIR`        | Destination for the pre-recovery snapshot.           |
| `AGENTS_JSON_SOURCE` | Known-good `agents.json` used by §4.                |

## 3. Failure-mode identification

Run these three checks in order. The first one that fires picks the section.
If two fire (rare), pick the **lower-numbered section first** — agents.json
recovery can change what the workspace refs point at, so fix it before the
git repair.

```sh
# 3.1 — Is agents.json present and parseable?
test -s "${AIMEE_HOME}/agents.json" \
  && jq -e . "${AIMEE_HOME}/agents.json" >/dev/null \
  && echo "agents.json: present+valid" \
  || echo "agents.json: MISSING or INVALID  -> §4"
```

```sh
# 3.2 — Is the agents.json stale (file present, content drifted)?
jq -r '.[].id' "${AIMEE_HOME}/agents.json" \
  | sort -u >/tmp/registered.txt
curl -fsS http://127.0.0.1:8742/v1/agents \
  | jq -r '.[].id' | sort -u >/tmp/live.txt
diff -u /tmp/registered.txt /tmp/live.txt \
  && echo "agents.json: in sync with live set" \
  || echo "agents.json: STALE  -> §5"
```

```sh
# 3.3 — Is the workspace .git healthy?
git -C "${WORKSPACE_DIR}" rev-parse --git-dir >/dev/null 2>&1 \
  || { echo ".git: MISSING  -> §6"; exit 0; }
git -C "${WORKSPACE_DIR}" fsck --no-dangling --no-progress >/dev/null 2>&1 \
  && echo ".git: fsck clean" \
  || echo ".git: CORRUPT  -> §6"
```

Mapping:

- `agents.json` missing or invalid -> **§4**.
- `agents.json` present + parseable, but diff against `/v1/agents` non-empty
  -> **§5**.
- `agents.json` healthy and in sync, but `git rev-parse` fails or `fsck`
  fails -> **§6**.

## 4. Lost or absent agents.json

Symptom: §3.1 reports `MISSING or INVALID`, or the appliance fails to
boot/serve because `agents.json` cannot be loaded.

- [ ] **Stop `aimee-server`** so it does not race the restore:

  ```sh
  systemctl stop aimee-server          # or: docker compose stop aimee-server
  ```

- [ ] **Snapshot the current state** (even if the file is bad — capture
      what is on disk for forensics):

  ```sh
  mkdir -p "${BACKUP_DIR}"
  cp -a "${AIMEE_HOME}/agents.json" "${BACKUP_DIR}/agents.json.bad" 2>/dev/null || true
  find "${AIMEE_HOME}" -maxdepth 2 -name 'agents.json*' \
       -exec cp -a --parents {} "${BACKUP_DIR}/" \;
  ```

- [ ] **Decide the source.** Use `AGENTS_JSON_SOURCE` if the operator has
      a known-good copy; otherwise fall back to the appliance defaults
      bundled with `aimee-server` (only valid for an empty registration —
      it will not restore per-agent overrides):

  ```sh
  # Option A — known-good from backup medium (preferred):
  test -s "${AGENTS_JSON_SOURCE}" \
    || { echo "AGENTS_JSON_SOURCE not found: ${AGENTS_JSON_SOURCE}"; exit 1; }
  jq -e . "${AGENTS_JSON_SOURCE}" >/dev/null \
    || { echo "AGENTS_JSON_SOURCE is not valid JSON: ${AGENTS_JSON_SOURCE}"; exit 1; }

  # Option B — appliance defaults (empty registration). Use ONLY if the
  # operator confirms no per-agent state must be preserved:
  #   : > "${AIMEE_HOME}/agents.json"
  ```

- [ ] **Restore `agents.json`** with safe permissions:

  ```sh
  install -m 0640 -o aimee -g aimee \
    "${AGENTS_JSON_SOURCE}" "${AIMEE_HOME}/agents.json"
  ```

- [ ] **Restart the server** and confirm `/v1/agent/list` returns the
      expected ids:

  ```sh
  systemctl start aimee-server
  curl -fsS http://127.0.0.1:8742/v1/agent/list | jq -r '.[].id' | sort -u
  ```

- [ ] Proceed to **§7 (final verification)**.

## 5. Stale-but-present agents.json

Symptom: `agents.json` parses, but the diff in §3.2 is non-empty. Agents
that exist in the live set are missing from the file, or the file lists
agents that no longer exist.

Recovery is **reconciliation, not overwrite**: take the live set as ground
truth for which ids must be present, then write back the *full per-id
records* from the live source (`/v1/agents`) so capabilities/keys/tiers are
preserved. Never copy individual fields from a stale file into a live
record — that re-introduces drift.

- [ ] **Stop the server** (you are rewriting registration):

  ```sh
  systemctl stop aimee-server
  ```

- [ ] **Snapshot the current state**:

  ```sh
  mkdir -p "${BACKUP_DIR}"
  cp -a "${AIMEE_HOME}/agents.json" "${BACKUP_DIR}/agents.json.pre-reconcile"
  ```

- [ ] **Pull the live agent records as ground truth** (the server must be
      reachable; if it is down, restart it long enough to read `/v1/agents`,
      then stop it again before the write):

  ```sh
  systemctl start aimee-server
  curl -fsS http://127.0.0.1:8742/v1/agents > "${BACKUP_DIR}/live-agents.json"
  systemctl stop aimee-server
  jq -e . "${BACKUP_DIR}/live-agents.json" >/dev/null \
    || { echo "live /v1/agents returned non-JSON"; exit 1; }
  ```

- [ ] **Write the reconciled file**. The live `GET /v1/agents` response
      is the authoritative per-id record set; `agents.json` is just that
      set on disk:

  ```sh
  jq . "${BACKUP_DIR}/live-agents.json" > "${AIMEE_HOME}/agents.json.tmp"
  install -m 0640 -o aimee -g aimee \
    "${AIMEE_HOME}/agents.json.tmp" "${AIMEE_HOME}/agents.json"
  rm -f "${AIMEE_HOME}/agents.json.tmp"
  ```

- [ ] **Diff the disk file against the live source** to confirm the
      reconcile closed the gap:

  ```sh
  diff -u \
    <(jq -S . "${AIMEE_HOME}/agents.json") \
    <(jq -S . "${BACKUP_DIR}/live-agents.json") \
    && echo "reconcile: clean"
  ```

- [ ] Proceed to **§7 (final verification)**.

## 6. Corrupt or lost workspace .git

Symptom: `git rev-parse --git-dir` fails, or `git fsck` reports dangling
or missing objects / bad refs. The workspace tree itself may be intact —
the goal is to get `.git` back to a state where `git status`/`git log` work
and the working tree is not lost.

- [ ] **Stop the server** so workspace writes do not race the repair:

  ```sh
  systemctl stop aimee-server
  ```

- [ ] **Snapshot the workspace** before touching `.git`. Even a corrupt
      `.git` plus an intact working tree is recoverable; throwing it away
      is not:

  ```sh
  mkdir -p "${BACKUP_DIR}"
  rsync -aHAX --numeric-ids "${WORKSPACE_DIR}/" "${BACKUP_DIR}/workspace.snapshot/"
  ```

- [ ] **Diagnose**. Capture the exact `fsck` and `rev-parse` output for
      the ticket — they determine whether the next step is repair or
      rebuild:

  ```sh
  ( git -C "${WORKSPACE_DIR}" rev-parse --git-dir || echo "NO_GIT" ) \
    | tee    "${BACKUP_DIR}/rev-parse.txt"
  git -C "${WORKSPACE_DIR}" fsck --no-progress --no-dangling \
    | tee    "${BACKUP_DIR}/fsck.txt" || true
  ```

- [ ] **Repair path — `.git` exists but `fsck` reports errors.** Use the
      safe, non-destructive options first; if they do not clear the
      errors, fall back to repacking from the object database. Never run
      `git fsck --lost-found` with auto-checkout; review what it finds
      before restoring:

  ```sh
  # 1. Reflog + unreachable-object salvage (review, do not auto-checkout).
  git -C "${WORKSPACE_DIR}" fsck --no-progress --unreachable \
    | tee "${BACKUP_DIR}/fsck.unreachable.txt"

  # 2. Repack to coalesce good objects and surface the bad ones.
  git -C "${WORKSPACE_DIR}" repack -ad

  # 3. Re-check; if still dirty, the object database is partially lost —
  #    skip to the rebuild path below.
  git -C "${WORKSPACE_DIR}" fsck --no-dangling --no-progress
  ```

- [ ] **Rebuild path — `.git` is missing or `fsck` cannot be cleared.**
      Re-initialize `.git` in place over the working tree, then rebuild
      the working-tree refs from the snapshot's working files (the tree
      is the source of truth; the new history is whatever `git add` +
      a single bootstrap commit captures):

  ```sh
  # 1. Move the broken .git aside (do NOT delete — keep it under BACKUP_DIR).
  if [ -d "${WORKSPACE_DIR}/.git" ]; then
    mv "${WORKSPACE_DIR}/.git" "${BACKUP_DIR}/workspace.snapshot/.git.broken"
  fi

  # 2. Re-init.
  git -C "${WORKSPACE_DIR}" init -q

  # 3. Re-stage and bootstrap-commit the recovered tree.
  git -C "${WORKSPACE_DIR}" -c user.name="recovery" \
                            -c user.email="recovery@localhost" \
      add -A
  git -C "${WORKSPACE_DIR}" -c user.name="recovery" \
                            -c user.email="recovery@localhost" \
      commit -q -m "recovered working tree (history not preserved)"
  ```

- [ ] **Repair refs / remote tracking** if the rebuild path was used, by
      re-adding the upstream and re-fetching. Without the original
      remote URL the history *cannot* be restored — this is a deliberate
      stop condition; see §8:

  ```sh
  # Only run if the operator knows the original remote URL.
  git -C "${WORKSPACE_DIR}" remote add origin <ORIGIN_URL>
  git -C "${WORKSPACE_DIR}" fetch --prune origin
  git -C "${WORKSPACE_DIR}" reset --hard origin/<BRANCH>   # only if appropriate
  ```

- [ ] **Sanity-check** the repaired/rebuilt repo:

  ```sh
  git -C "${WORKSPACE_DIR}" status
  git -C "${WORKSPACE_DIR}" fsck --no-dangling --no-progress
  ```

- [ ] Proceed to **§7 (final verification)**.

## 7. Final verification

Run **every** box below. A passing final verification means the recovery
is done; a single failing box is a stop condition (§8).

- [ ] **`agents.json` parses and is in sync with the live set**:

  ```sh
  jq -e . "${AIMEE_HOME}/agents.json" >/dev/null
  jq -r '.[].id' "${AIMEE_HOME}/agents.json" | sort -u >/tmp/r.txt
  curl -fsS http://127.0.0.1:8742/v1/agents \
    | jq -r '.[].id' | sort -u >/tmp/l.txt
  diff -u /tmp/r.txt /tmp/l.txt
  ```

- [ ] **Server is up and a smoke-test agent responds**:

  ```sh
  systemctl status aimee-server --no-pager
  aimee delegate <smoke-agent-id> "ping" --persona engineer
  ```

- [ ] **Workspace `.git` is clean (only required if §6 was run)**:

  ```sh
  git -C "${WORKSPACE_DIR}" fsck --no-dangling --no-progress
  git -C "${WORKSPACE_DIR}" status --porcelain
  ```

- [ ] **Pre-recovery snapshot is retained** under `${BACKUP_DIR}` until the
      operator confirms the recovery in a follow-up window (24h minimum):

  ```sh
  ls -la "${BACKUP_DIR}"
  ```

- [ ] **Incident log entry written** (short — what broke, which section
      ran, the diff/snapshot paths):

  ```sh
  printf 'incident=%s host=%s section=%s snapshot=%s\n' \
    "appliance-state-recovery" "$(hostname)" "<§4|§5|§6>" "${BACKUP_DIR}" \
    >> "${AIMEE_HOME}/var/log/recovery.log"
  ```

## 8. Escalation/stop conditions

Stop the runbook and escalate (page the on-call owner of the `aimee-server`
service and open a sev-2 incident) if **any** of the following is true:

- [ ] **`AGENTS_JSON_SOURCE` is missing AND** there is no appliance-default
      path the operator can stand on. Do **not** invent an empty
      `agents.json` on a host that previously had registered agents —
      that silently orphans every per-agent record. Stop and escalate.
- [ ] **`git fsck` reports "bad" objects after §6's repair path.** Partial
      object loss is unrecoverable from the host alone; it requires the
      last known-good snapshot from off-host backup or the upstream
      remote. Stop and escalate.
- [ ] **The original remote URL for `WORKSPACE_DIR` is unknown.** The
      rebuild path can salvage the working tree but cannot restore
      history; without the remote, the repo is permanently degraded.
      Stop and escalate before any further write.
- [ ] **`diff` in §5 (or §7) does not converge after two attempts.**
      The reconcile loop is not closing — likely a schema or endpoint
      change upstream. Stop, escalate with the captured diff, do not
      force-write.
- [ ] **The pre-recovery snapshot (§4/§5/§6) cannot be written** (backup
      medium full, read-only, or missing). Do **not** proceed: every
      recovery step here is reversible only via that snapshot. Mount a
      writable backup medium, then resume from §2.
- [ ] **The server fails to start after any restore.** A clean restart is
      the verification gate (§7); if it does not start, the recovered
      state is not safe to bring online. Stop and escalate with the
      server journal:

  ```sh
  journalctl -u aimee-server -n 200 --no-pager
  ```

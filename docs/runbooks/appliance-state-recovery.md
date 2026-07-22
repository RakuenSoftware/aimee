# Runbook: recover Aimee appliance state

A running Aimee appliance (`aimee-server`) can lose two pieces of live
state on its tier-bound volumes:

- the agent config file at `$AIMEE_HOME/agents.json`
- a workspace repo's git metadata at
  `$AIMEE_HOME/workspaces/<user>/<repo>/.git`

The symptoms are specific and the recoveries are mechanical, but until
this runbook exists each incident is rediscovered from scratch. Pick the
section that matches your symptom; do not apply one scenario's recovery
to a different scenario.

> **Read your section before running anything.** Each scenario has a
> short diagnosis block. The recovery command is only correct when the
> diagnosis matches.

---

## Before you start

- You need shell access on the appliance host as a user that can read
  `$AIMEE_HOME` (the directory the Aimee server was started with).
- Locate `$AIMEE_HOME`. The default on a deployed appliance is
  `/var/lib/aimee`. A development checkout may use `~/.config/aimee`.
  Substitute the actual value in every command below.

The remainder of this runbook uses `"$AIMEE_HOME"` literally. Run the
commands from a shell where that variable is exported, or inline the
path.

---

## Scenario 1 — Lost or absent `agents.json`

### Symptom

- `GET /v1/agents` returns **502 "agents backend unavailable."**
- `GET /v1/agent/list` returns an **empty array** (it masks the failure
  as "no agents" rather than the upstream 502 — do not be fooled by the
  empty list).
- The UI shows "backend unavailable" or "no agents configured" and will
  not let you chat, run a delegate, or list models.

### Diagnosis

Confirm the file (or its contents) is genuinely gone:

```sh
ls -l "$AIMEE_HOME/agents.json" 2>&1
test -s "$AIMEE_HOME/agents.json" && echo NONEMPTY || echo MISSING_OR_EMPTY
```

If the file is missing or zero-bytes, you are in this scenario. If it
exists and parses, jump to Scenario 2 instead — the recovery commands
are different and Scenario 2's `touch` will not help here.

Confirm a sibling backup exists (the appliance keeps timestamped
backups next to the live file):

```sh
ls -1 "$AIMEE_HOME"/agents.json.bak-* 2>&1
```

Pick the most recent non-empty backup.

### Recovery

Restore the chosen backup to the live path. API keys do not live in
this file — they live in the vault, keyed by agent name — so a
restored file needs **no secrets re-entered.** That is also why you
must not edit the restored file by hand: a hand-edited partial copy
will leave the vault out of sync with what the loader sees.

```sh
BAK="$(ls -1t "$AIMEE_HOME"/agents.json.bak-* | head -n1)"
cp -p "$BAK" "$AIMEE_HOME/agents.json"
chown aimee:aimee "$AIMEE_HOME/agents.json"   # or your server user:group
chmod 0640 "$AIMEE_HOME/agents.json"
```

Then bust the identity cache (see Scenario 2 for the rationale):

```sh
touch "$AIMEE_HOME/agents.json"
```

### Verification

1. File exists, is non-empty, and parses as JSON:

   ```sh
   python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); \
     assert d.get("agents"), d' "$AIMEE_HOME/agents.json" && echo OK
   ```

2. `GET /v1/agents` returns 200 with the expected agent list. If it
   still returns 502, the loader has not invalidated — re-run the
   `touch` and confirm the file's mtime moved forward.

3. The UI lets you select a model and run a chat / delegate. The
   "backend unavailable" message is gone.

### If the problem persists

Stop. If no `agents.json.bak-*` sibling exists, the recovery above is
not available on this appliance — escalate before attempting to
hand-author the file. If the restored file parses but `/v1/agents`
still returns 502, the fault is upstream of the file (the credential
vault referenced by the agents is empty or sealed — see the
vault-master-key-rotation runbook). Investigate the vault state and the
Aimee server log; do not loop on `touch`.

---

## Scenario 2 — Stale-but-present `agents.json`

### Symptom

- `GET /v1/agents` returns 502 "agents backend unavailable."
- `agents.json` clearly exists on disk, parses as JSON, and contains
  the expected agent entries — yet the loader is acting as if the file
  has not changed since boot.

### Diagnosis

Run each of the following in order. Stop as soon as one fails — that
identifies which scenario you are actually in.

1. **File exists and is readable.**

   ```sh
   test -r "$AIMEE_HOME/agents.json" && echo OK || echo MISSING_OR_UNREADABLE
   ```

2. **File is valid JSON.**

   ```sh
   python3 -c 'import json,sys; json.load(open(sys.argv[1]))' \
     "$AIMEE_HOME/agents.json" && echo OK || echo INVALID_JSON
   ```

3. **File contains the expected agent entries.**

   ```sh
   python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); \
     assert isinstance(d.get("agents"), list) and d["agents"], d' \
     "$AIMEE_HOME/agents.json" && echo OK || echo NO_AGENTS
   ```

   `NO_AGENTS` here means the JSON parsed but the `agents` list is
   missing or empty — that is **not** this scenario; treat it as
   Scenario 1.

4. **Compare mtime to the appliance clock.** The Aimee server caches
   the config identity as `(mtime, size, inode)`; a file whose mtime
   is in the past relative to the running clock looks unchanged to
   the loader even though the cache is stale.

   ```sh
   stat -c '%Y' "$AIMEE_HOME/agents.json" \
     | awk '{printf "file_mtime_epoch=%s\n", $1}'
   date -u +%Y-%m-%dT%H:%M:%SZ
   date -u +%s | awk '{printf "clock_epoch=%s\n", $1}'
   ```

   If `file_mtime_epoch` is meaningfully in the past relative to
   `clock_epoch` (typically hours or more), and steps 1–3 all
   returned `OK`, you are in the Stale-but-present scenario. A
   difference of a few seconds is normal (process startup, journal
   flush). A difference of minutes or more — or an mtime that is
   *future-dated* because the clock was rolled back — is the failure
   pattern.

### Failure pattern

The configuration is correct on disk, but the loader's
`(mtime, size, inode)` cache was populated before the file's mtime
became current (typical after a clock correction, a snapshot restore
whose mtimes predate the restored clock, or a long-running server
whose initial scan pre-dates the present file). The next reload sees
no identity change and the loader keeps serving the cached "no
config" view.

### Recovery

```sh
touch "$AIMEE_HOME/agents.json"
```

That is the entire recovery. `touch` updates the mtime without
modifying the file contents, so the loader sees a fresh identity and
re-reads the existing (correct) configuration from disk.

**Do not re-enter agents, keys, or other configuration in response to
this symptom.** When `agents.json` already exists, is readable,
parses as valid JSON, and contains the expected entries, the content
is not the problem. Re-entering configuration risks overwriting a
working file with an incomplete one. API keys live in the vault
keyed by agent name; they are not stored in `agents.json`.

### Verification

1. **mtime now reflects the appliance clock.**

   ```sh
   stat -c '%y' "$AIMEE_HOME/agents.json"
   date -u +%Y-%m-%dT%H:%M:%SZ
   ```

   The two should be within a second of each other.

2. **The agents endpoint returns the expected agents.**

   ```sh
   curl -fsS http://127.0.0.1:<api-port>/v1/agents
   ```

   Confirm the returned list matches the `agents` array from
   Diagnosis step 3.

3. **The backend-unavailable symptom is gone.** Reload the UI, or
   re-run the probe that originally surfaced the symptom (a delegate
   call, a model-list call, a chat). The "backend unavailable"
   message must no longer appear.

If all three checks pass, recovery is complete. The file content is
unchanged; the loader's cache was simply stale.

### If the problem persists

**Stop. Do not run `touch` again or modify `agents.json` further.**
Repeatedly bumping mtime on a file that is already fresh will not
change the outcome and risks masking the real fault. Investigate the
following in order:

1. **Clock synchronization.** If the box clock is drifting, rolling
   back, or being reset by NTP on every boot, mtime comparisons are
   unreliable. Check `timedatectl status` and the NTP source.
   Stabilize the clock before touching any state files.
2. **File ownership and permissions.** Confirm the Aimee server
   process user can read `$AIMEE_HOME/agents.json`:

   ```sh
   ls -l "$AIMEE_HOME/agents.json"
   stat -c '%U:%G %a' "$AIMEE_HOME/agents.json"
   ```

   The server user (often `aimee`) must have at least `r--`; if not,
   the loader sees an empty or unreadable configuration regardless
   of mtime. Fix ownership with `chown`/`chmod`; do not edit the
   file.
3. **Service logs.** Check the Aimee server log for config-load
   errors, parse failures, or repeated reload attempts. The log
   entry will name the actual fault.
4. **Service-level issues.** If config loads cleanly but the endpoint
   still returns "unavailable," the fault is in the running service
   (process crash loop, port conflict, downstream dependency down) —
   not in `agents.json`. Restart the service only after diagnosing
   the log entry; a restart of a broken service will not fix a
   broken service.

---

## Scenario 3 — Corrupt or lost workspace repo `.git`

### Symptom

- Opening or polling a specific workspace in the UI fails.
- The proposals-trigger log line repeats every poll:

  ```
  ls-tree failed ... rc=128
  ```

- The forge log shows:

  ```
  resolve https origin: no origin remote
  ```

- Other workspaces on the same appliance open normally; only one (or
  a small set) is affected.

### Diagnosis

```sh
ls -la "$AIMEE_HOME/workspaces/<user>/<repo>/.git" 2>&1
GIT_DIR="$AIMEE_HOME/workspaces/<user>/<repo>/.git" \
  git -C "$AIMEE_HOME/workspaces/<user>/<repo>" fsck --no-progress
```

`git fsck` reports "missing or corrupt object" or "bad reference," or
`ls` shows `.git` absent — you are in this scenario. Before
proceeding, rule out storage failure on the tier-bound volume:

```sh
git clone --depth 1 <remote-url> /tmp/repo-healthcheck
git -C /tmp/repo-healthcheck fsck --no-progress
rm -rf /tmp/repo-healthcheck
```

A healthy clone confirms the volume and network are fine; the fault
is local to the workspace's `.git`. A failing clone means the
recovery below will also fail — stop and investigate storage and
network before continuing.

### Recovery

The workspace working tree is content-preserved; only its git
metadata needs repair. Replace `.git` from a fresh clone of the same
remote, then re-attach:

```sh
REPO="$AIMEE_HOME/workspaces/<user>/<repo>"
cd "$REPO"

# Stash any uncommitted work before moving .git aside.
# If uncommitted work matters, copy the working tree to a safe
# location first; the recovery below does not touch working files.

mv .git .git.corrupt.$(date -u +%Y%m%dT%H%M%SZ)

git clone --no-checkout --single-branch --branch <default-branch> \
  <remote-url> .git

# Re-attach the remote name and tracking branch so the forge can
# resolve `origin` again.
git remote remove origin 2>/dev/null || true
git remote add origin <remote-url>
git branch --set-upstream-to=origin/<default-branch> <default-branch>
```

Use the canonical HTTPS URL the appliance is configured to reach
(typically the forge's clone URL). Set `origin` to that URL
explicitly so the forge log no longer reports "no origin remote."

### Verification

1. `git fsck` against the new `.git` reports no errors.
2. `git -C "$REPO" remote -v` shows the canonical `origin`.
3. The forge log stops emitting `resolve https origin: no origin
   remote`.
4. The proposals-trigger log stops emitting `ls-tree failed ... rc=128`.
5. The workspace opens in the UI and lists the expected files.
6. `git -C "$REPO" status` is clean, or shows only the expected
   uncommitted changes — not a flood of "deleted/untracked" entries
   that would indicate the working tree was not re-attached
   correctly.

### If the problem persists

Stop. If the remote is unreachable, the workspace's tracked branch has
been force-pushed since the corrupt `.git` was last updated, or the
storage healthcheck at the top of this section fails, the recovery
above will not produce the expected working tree. Investigate network
reachability, remote branch state, and tier-volume health before
attempting further recovery.

---

## Stop guidance (all scenarios)

If any scenario's recovery action runs cleanly but verification fails:

- Do not loop on the recovery action.
- Do not modify the file further to "force" recovery.
- Stop and collect:
  - The exact symptom (UI message, API response, log entry).
  - The output of the diagnosis checks for the scenario you were in.
  - The Aimee server log covering the recovery attempt.
- Escalate to the appliance support channel with that bundle.

Two failure modes this runbook explicitly guards against:

- **Re-entering configuration or secrets when the file is already
  correct.** Scenario 2 is a false "missing config" presentation; the
  file content is fine and `touch` is the only action needed. Editing
  `agents.json` to "fix" this symptom risks overwriting a working
  file. API keys live in the vault and never need to be re-entered
  here.
- **Looping on `touch` when the fault is elsewhere.** A persistent
  symptom after a single correct recovery action indicates clock
  drift, permissions, vault state, or a service-level issue — none
  of which additional `touch` calls will resolve.

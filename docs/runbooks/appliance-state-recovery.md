# Appliance state recovery

Two pieces of live state live on tier-bound volumes for an `aimee-server`
appliance: the agent config (`$AIMEE_HOME/agents.json`) and the workspace
repo's git metadata (`$WS/.git`). Both can be lost or corrupted by a missing
volume, a partial restore, or a botched edit. The symptoms are specific and
the recovery is mechanical; this runbook is the checklist.

Throughout this runbook:

- `$AIMEE_HOME` is the server's data directory (e.g. `/var/lib/aimee`).
- `$WS` is the affected workspace repo path (e.g.
  `/var/lib/aimee/workspaces/<user>/<repo>`).
- `$AIMEE_BASE_URL` is the daemon's v1 API base URL (e.g.
  `http://127.0.0.1:8740`). Before running any `curl` probe, set it
  from the actual daemon bind address and port shown in the systemd unit
  (`Environment=`/`ExecStart=`), container environment, or startup logs;
  do not assume the appliance is bound on loopback.
- `$CANONICAL_HTTPS_URL` is the canonical HTTPS clone URL of that repo
  (the same URL the forge uses; e.g. `https://example.com/<user>/<repo>.git`).

All commands assume shell expansion of these variables. Run them as the
server runtime user.

## Failure Mode 1 -- Lost or absent `agents.json`

### Symptom

`GET /v1/agents` returns `502 agents backend unavailable`.

> Current builds report this failure explicitly from `GET /v1/agent/list`.
> The masking path is `server_agent_list_json`, which still returns `[]`
> for summary consumers such as `GET /v1/server/state`; older builds also
> masked the error at `/v1/agent/list`. Probe with `/v1/agents` (the
> strict endpoint) to see the backend error.

### Confirm

`agents.json` is missing or zero-bytes; one or more sibling backups exist:

```bash
ls -l "$AIMEE_HOME/agents.json"
ls -1 "$AIMEE_HOME"/agents.json.bak-*
```

If the second command prints the literal `agents.json.bak-*` pattern (i.e.
no siblings exist), there is no `agents.json` to restore in this runbook.
**Stop and escalate** -- recovering the agent configuration without an
existing sibling backup is out of scope for this runbook. Only proceed
with the Restore step below when at least one `agents.json.bak-*` sibling
is present.

### Recover

1. Pick the latest backup so the operator can eyeball the choice before
   overwriting the canonical file:

   ```bash
   LATEST=$(ls -1t "$AIMEE_HOME"/agents.json.bak-* | head -n1)
   [ -n "$LATEST" ] && [ -f "$LATEST" ] || {
     echo "No agents.json backup selected; aborting." >&2
     exit 1
   }
   ls -l "$LATEST"
   ```

2. Restore it in place preserving mode/timestamps, bust the in-process
   identity cache (mtime + size + inode) with `touch`, then probe the
   strict endpoint:

   ```bash
   cp -p "$LATEST" "$AIMEE_HOME/agents.json"
   touch "$AIMEE_HOME/agents.json"
   curl -fsS "${AIMEE_BASE_URL}/v1/agents"
   ```

   Success is HTTP 200 with a JSON object containing `default` and a
   non-empty `agents` array.

API keys live in the vault keyed by agent name, not in `agents.json`. A
restored config needs no secrets re-entered -- the vault lookups continue
to resolve unchanged.

## Failure Mode 2 -- Stale but present `agents.json`

### Symptom

The daemon reports the agent config as absent even though the file is
valid on disk.

### Confirm

`agents.json` exists with non-zero size, but its mtime is in the past
relative to the box clock. Size and inode are unchanged across reads.
The cache key is the (mtime, size, inode) tuple; print it in a single
structured line so the operator can compare two snapshots directly:

```bash
stat -c '%Y %s %i %n' "$AIMEE_HOME/agents.json"
```

The four whitespace-separated columns are, in order:

1. `%Y` -- mtime in epoch seconds (the freshness signal the cache keys on).
2. `%s` -- size in bytes.
3. `%i` -- inode number.
4. `%n` -- file name.

Stale-mtime symptom: column 1 predates the current `date +%s` while
columns 2 and 3 are unchanged between repeated `stat` calls.

### Recover

Refresh mtime so the cache tuple changes (the inode and size may stay the
same), then probe:

```bash
touch "$AIMEE_HOME/agents.json"
curl -fsS "${AIMEE_BASE_URL}/v1/agents"
```

Success is HTTP 200 with a JSON object containing `default` and a non-empty
`agents` array.

## Failure Mode 3 -- Corrupt or lost workspace repo git dir

### Symptom

- Proposals trigger logs report `ls-tree failed ... rc=128` on every poll.
- The `wfe-forge` subsystem logs
  `resolve https origin: no origin remote` for the affected workspace repo.

### Confirm

Run the clone probe on a sibling path **on the same volume** as `$WS` so
storage faults are visible. `/tmp` would mask a tier-bound volume fault;
a path under the same parent as `$WS` keeps the device-id check meaningful.
Use a shallow (`--depth 1`) probe so the diagnostic clone does not
consume more of the workspace tier than necessary; keep only the final
recovery clone full.

```bash
WS_DEV=$(stat -c '%d' "$WS")
PROBE="${WS%/*}/ws-probe-$$"
git clone --depth 1 --single-branch "$CANONICAL_HTTPS_URL" "$PROBE"
git -C "$PROBE" rev-parse HEAD
git -C "$PROBE" ls-remote origin HEAD
```

Sanity checks before you proceed with the real recovery:

- `[ "$(stat -c '%d' "$PROBE")" = "$WS_DEV" ]` -- the probe and the
  broken repo share a filesystem (same device id). This audit is the whole
  point of the probe: if it fails, the workspace storage itself is
  the fault, not `$WS/.git`.
- `git rev-parse HEAD` resolves to a commit hash on the probe.
- `git ls-remote origin HEAD` returns a matching remote HEAD.

Then clean up the probe so it does not accumulate on tier-bound storage:

```bash
rm -rf "$PROBE"
```

If the probe clone succeeds and the canonical remote answers, the
storage layer is fine; the on-disk `$WS/.git` is the fault.

### Recover

Move the broken repo aside to preserve any uncommitted state, then clone
a clean single-branch repo from the canonical HTTPS URL into `$WS`. Do
**not** edit `$WS/.git` in place and do **not** `rm -rf` the workspace
root -- `mv` keeps the broken copy recoverable:

```bash
ts=$(date +%s)
mv "$WS" "${WS}.bak.${ts}"
git clone --single-branch "$CANONICAL_HTTPS_URL" "$WS"
```

If this clone fails, stop immediately. Preserve `${WS}.bak.${ts}`, do not
try to repair a partial new `$WS` in place, and either escalate or restore
the backup before attempting any further recovery on the canonical path.

Verify the fresh clone has its `origin` set correctly and that HEAD
resolves:

```bash
git -C "$WS" remote -v
git -C "$WS" rev-parse HEAD
```

The fresh clone uses the canonical `origin` URL and tracks the default
branch by virtue of `--single-branch`; no `remote set-url` or
`symbolic-ref` step is required.

# Runbook: appliance state recovery

Recover the two pieces of live state a SmoothNAS/tierd appliance running the
`aimee-server` plugin can lose on its tier-bound volumes: the agent config
(`$AIMEE_HOME/agents.json`) and a workspace repo's git metadata
(`.../workspaces/<user>/<repo>/.git`). Each section is a self-contained
playbook an operator can follow under incident pressure.

## Scope

This runbook applies to SmoothNAS/tierd appliances running the aimee-server plugin.

## Preconditions

Before beginning recovery, confirm that you have:

- Shell access to the affected appliance.
- `$AIMEE_HOME` set correctly for the aimee-server installation.
- The path to the affected workspace.
- The canonical HTTPS origin URL for the workspace repository.
- The repository's default branch identified.

> **Warning:** Preserve all damaged files until recovery has been completed and verified. Do not delete, overwrite, or otherwise modify them; they may be required for validation or forensic analysis.

## 1. Lost/absent `agents.json`

(Drafted stub — placeholder for the implementer. Mirrors the shape of §2:
Symptom / Recovery / Verification, each with an operator-checkable signal
or command. The proposal calls out `GET /v1/agents` returning 502
"agents backend unavailable" (and `GET /v1/agent/list` masking the failure
as an empty array), restoring from a sibling `agents.json.bak-*` backup,
then `touch $AIMEE_HOME/agents.json` to bust the (mtime+size+inode)
identity config cache.)

## 2. Stale-but-present `agents.json`

The file exists, parses, and is on disk, but the appliance treats its
mtime as older than its own clock and behaves as if no configuration is
present. Distinct from §1: there is no missing-file 502 to grep for, so
the operator has to read the file's own metadata before guessing.

### Symptoms

- **`agents.json` is present and parses.** `cat "$AIMEE_HOME/agents.json"`
  returns valid JSON describing configured agents; the file is not zero
  bytes; no `agents.json.bak-*` is being shadowed. Yet configuration
  appears absent from the appliance's perspective (agents do not show
  up in `aimee` listings, delegates cannot resolve).
- **File mtime is unexpectedly behind the appliance clock.** Run
  `stat -c '%y %n' "$AIMEE_HOME/agents.json"` and compare to the box
  clock (`date -u`); the file's mtime should match recent configuration
  activity. A mtime hours or days in the past against a current clock is
  the staleness tell.
- **API tell.** `GET /v1/agents` returns the persisted agents list when
  the file is fresh, and returns an empty list (not 502) while the file
  is stale — operators can read the API directly to distinguish this
  failure mode from §1.

### Recovery

1. Inspect the appliance time and the file timestamp side by side:

   ```sh
   date -u
   stat -c '%y  %s  %n' "$AIMEE_HOME/agents.json"
   ```

   Confirm the file exists, parses, and its mtime is genuinely behind
   the box clock before touching.

2. Bust the staleness with a touch:

   ```sh
   touch "$AIMEE_HOME/agents.json"
   ```

   No file content changes; only the mtime advances.

### Verification

- `GET /v1/agents` reads and returns the existing configuration (the
  agents list that was already on disk in `$AIMEE_HOME/agents.json`),
  not an empty array and not a 502.

## 3. Corrupt/lost workspace repo git dir

(Drafted stub — placeholder for the implementer. Mirrors the shape of §2:
Symptom / Recovery / Verification, each with an operator-checkable signal
or command. The proposal calls out the proposals trigger logging
`ls-tree failed ... rc=128` every poll and the forge logging
`resolve https origin: no origin remote`, with recovery via a clean
single-branch clone that sets `origin` to the canonical HTTPS URL and
tracks the default branch.)

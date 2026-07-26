# Runbook: appliance state-recovery

A SmoothNAS/tierd appliance running the `aimee-server` plugin can lose pieces
of live state on its tier-bound volumes. The symptoms are specific, the
recovery is mechanical, but the steps are not written down anywhere — so each
incident is rediscovered from scratch. This runbook is the operator-facing
checklist.

**Operator-gated, offline maintenance op**: apply the recovery steps under
incident pressure, never as a background job.

## Lost or absent `agents.json`

`$AIMEE_HOME/agents.json` defines the delegate roster; without it the server
cannot enumerate agents. The two `GET` routes diverge deliberately — one
fails loud, the other swallows the failure — which is the diagnostic trap.

### Symptoms

- `GET /v1/agents` returns HTTP **502** with `agents backend unavailable`.
- Operators commonly reach for `GET /v1/agent/list` as a quick health check:
  **do not**. When the agents backend is unwired or the file is absent,
  `GET /v1/agent/list` **masks the failure as an empty array** (HTTP 200,
  body `"agents": []`). It is not a reliable diagnostic — a "no agents"
  response and a "backend unavailable" response look identical from the
  dashboard. Always trust `GET /v1/agents`.

### Recovery

1. **Confirm `$AIMEE_HOME/agents.json` is absent** before proceeding. A
   partial or zero-byte file is a different failure (see the agents.json
   mtime-cache footnote in `src/server/agent_config.c`):

   ```sh
   ls -l "$AIMEE_HOME/agents.json" 2>&1
   ```

   A `No such file or directory` reply is the trigger for this runbook;
   anything else means stop and diagnose.

2. **List the sibling `$AIMEE_HOME/agents.json.bak-*` backups** and select the
   appropriate one. Pick the most recent backup whose captured roster
   matches the pre-incident fleet — backup filenames carry a timestamp or
   incremental suffix (e.g. `agents.json.bak-2025-01-15T08-30Z`); pick the
   last good one. If no backup exists, stop and recover from off-host
   storage before restoring.

   ```sh
   ls -lt "$AIMEE_HOME"/agents.json.bak-* 2>&1
   ```

3. **Restore the chosen backup to `$AIMEE_HOME/agents.json`**:

   ```sh
   cp -p "$AIMEE_HOME/agents.json.bak-<chosen>" "$AIMEE_HOME/agents.json"
   ```

   `cp -p` preserves the backup's mode/ownership/timestamps — that is
   intentional for the next step's invariant, but it is also the reason the
   following `touch` is required.

4. **Run `touch "$AIMEE_HOME/agents.json"`** after the restore:

   ```sh
   touch "$AIMEE_HOME/agents.json"
   ```

   The server caches the parsed agents.json in process memory and keys the
   cache on the file's **identity** — `(mtime, size, inode)` together, not
   mtime alone (see the `g_agent_config_mtime` / `_size` / `_ino` triplet in
   `src/server/agent_config.c:919-929`). A restored backup can legitimately
   land with the same mtime as a prior cached read, and on tiered appliance
   filesystems it has been observed landing with an mtime *hours in the
   past*, where the cache treats it as a hit and serves stale (or absent)
   content forever. `touch` updates mtime to the current clock, **which
   invalidates the identity-based configuration cache**, forcing the next
   `agent_load_config` to re-parse the file. Do not skip it.

5. **API keys need no re-entry.** A restored `agents.json` is a roster of
   *definitions*; provider API keys are sealed in the server vault at
   `<AIMEE_HOME>/.vault/` and are keyed by **agent name**, not by
   `agents.json` in-memory state. The restored file references the same
   agent names as before, so the vault resolves the same credentials at
   runtime — there is no plaintext secret in `agents.json` to re-enter, and
   no re-import is required.

### Verification

- `GET /v1/agents` no longer returns the backend-unavailable **502**; the
  response is **200** with the populated roster.
- `GET /v1/agent/list` now lists the restored agents (compare the count and
  the agent names against the chosen backup to confirm completeness).

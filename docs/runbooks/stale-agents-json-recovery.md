# Runbook: recover from a stale-but-present `agents.json`

When the Aimee backend reports "agents backend unavailable" but
`$AIMEE_HOME/agents.json` already exists, is readable, parses as valid JSON,
and contains the expected agent entries, the file is almost certainly stale
on the box clock. This runbook covers that one scenario and the single
recovery action that resolves it.

If any diagnosis check fails, this runbook does not apply. Stop here
and escalate to the Aimee service owner with the diagnosis output rather
than continuing with the steps below. Configuration restoration or secret
re-entry may be appropriate for missing, malformed, or partially-populated
files, but those cases are out of scope for this runbook and must follow
a different procedure; do not apply this runbook to them.

## Symptoms

- The Aimee backend reports "agents backend unavailable" (or an equivalent
  empty/unconfigured agent listing) for an appliance that has previously
  been provisioned and used.
- `$AIMEE_HOME/agents.json` already exists and looks correct on inspection.
- No recent configuration change has been made by an operator.

## Diagnosis

Run the four checks below, in order. Every check must pass before you
continue with the recovery action.

1. **File exists and is readable.**

   ```sh
   ls -l "$AIMEE_HOME/agents.json"
   ```

   Confirm the file is present and the listing shows a non-zero size.

2. **File is valid JSON.**

   ```sh
   python3 -c 'import json,sys; json.load(open(sys.argv[1]))' \
       "$AIMEE_HOME/agents.json"
   ```

   The command exits 0 with no output on success. Any error here means the
   file is malformed; this runbook does not apply — stop and escalate
   to the Aimee service owner.

3. **File contains the expected agent entries.**

   Inspect the parsed object and confirm each expected agent (by name and
   adapter) is present. If any expected agent is missing, this runbook
   does not apply — stop and escalate to the Aimee service owner.

4. **File mtime lags the box clock.**

   ```sh
   stat -c '%Y  %n' "$AIMEE_HOME/agents.json"
   date +%s
   ```

   If the mtime epoch is more than a few seconds behind the box clock
   epoch, the file is stale. This is the only condition in which the
   recovery action below is appropriate.

## Failure pattern

Configuration appears absent even though the file on disk is correct:
valid JSON, complete entries, and the only defect is that the file's mtime
predates the current box clock. The backend's "unavailable" signal is
driven by staleness against the clock, not by file content.

## Recovery

Do not re-enter configuration or secrets. The file on disk is already
correct; re-entering values risks overwriting a known-good file with
unverified input.

Update the file's mtime to the current box clock with a single `touch`:

```sh
touch "$AIMEE_HOME/agents.json"
```

This is the entire recovery action. No service restart is required.

## Verification

Confirm each of the following, in order:

1. **mtime now reflects the box clock.**

   ```sh
   stat -c '%Y  %n' "$AIMEE_HOME/agents.json"
   date +%s
   ```

   The two epochs should match (within a second or two).

2. **The backend now returns the expected agents.** Re-run `GET /v1/agents`:

   ```sh
   curl -fsS http://localhost:8080/v1/agents
   ```

   The response must list every agent you confirmed in the diagnosis step
   above.

3. **The backend-unavailable symptom is gone.**

   Re-run the call that originally surfaced the "unavailable" error and
   confirm it now succeeds without the unavailability message.

## If the problem persists

Stop and investigate. Do **not** repeat the `touch`, and do **not**
re-enter configuration. The condition this runbook addresses is a stale
mtime on an otherwise-valid file; if the symptom persists after a
successful `touch`, the actual fault lies elsewhere. In order:

1. **Clock synchronization.** Verify the box clock is accurate and that
   NTP (or the appliance's configured time source) is healthy. A
   persistently drifting or wrong clock will mask any number of
   upstream issues and will re-trigger this symptom on every reload.
2. **File ownership and permissions.** Confirm the Aimee service can
   read the file. The owning user/group and mode must match what the
   service expects; a permission regression can look identical to a
   missing file from the backend's perspective.
3. **Service logs.** Inspect the Aimee service logs for the time window
   around the `touch` and the verification call. A load failure,
   schema rejection, or dependency error logged there will identify the
   real cause.

If those checks do not identify the fault, escalate rather than
continuing to modify the file or the clock.

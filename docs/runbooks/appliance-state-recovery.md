# Appliance State Recovery

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

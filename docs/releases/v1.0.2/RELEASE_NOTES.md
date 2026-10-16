# Aimee 1.0.2

## Memory setup and Hillock qualification

- First-start setup offers Aimee native memory, Cognee or Hillock. External service
  credentials are sealed in Vault before the non-secret backend selection is saved.
- Memory and egress load the saved selection on startup. Existing installations
  default to native memory; explicit deployment environment settings take precedence.
  Changing backends requires restart and does not install services or migrate records.
- Hillock release-candidate branches are discovered upstream and tested at their exact
  commit through Aimee's owner, recall and governed transport boundaries. Qualification
  covers isolation, corrections, lifecycle operations, authentication, restart,
  concurrency and scale. Application prereleases do not trigger this workflow.

## Audit durability and startup recovery

- Application images use checksum-verified SQLite 3.51.3. Audit admission refuses
  unsupported engines and corrupt ledgers, and reports durability failures clearly.
- Diagnostic captures have a per-file size bound and durable replay-gap markers.
  Readiness fails when audit-volume free space falls below 512 MiB and recovers
  when capacity is restored. The audit ledger itself is not rotated by this policy.
- Failed startup stops child services and remains unhealthy without a crash loop.
  Automatic startup on host boot is retained; repair the fault and restart explicitly.
- Explicit audit recovery writes a separate epoch and preserves the original ledger.
  Historical integrity failures remain visible; recovery does not certify missing history.
- Detached Git waits for acknowledged runner registration and rejects malformed
  acknowledgments before dispatch.

## Upgrade notes

Preserve database, Vault and audit backups before upgrading. Do not delete or rewrite
an audit ledger to clear an integrity error. Memory backend selection remains governed
by administrator access and takes effect after restart. Independent module, Hillock
and native-model plugin versions are unchanged by this application release.

Release publication follows the protected main-merge and release approval workflows.
These notes describe the prepared candidate; artifacts and deployment require those gates.

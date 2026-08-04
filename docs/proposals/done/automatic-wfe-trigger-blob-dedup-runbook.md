# Proposal: Document proposal-trigger blob deduplication

- **State:** DONE — archived after the runbook update shipped.

## Goal

Document the production behavior of the autonomous pending-proposal watcher so
operators can predict when a file is admitted, queued, or deliberately ignored.

## Scope

Update only `docs/wfe-autonomy-runbook.md`. Add a concise section that explains:

- pending proposal identity is based on the complete file bytes, workflow, and
  mode, not the moving branch commit;
- advancing the watched branch without changing a proposal does not start a
  duplicate run;
- changing the proposal bytes makes it eligible for a new run;
- the live trigger admission cap can queue otherwise eligible proposals and is
  edited in Workflows → Run policy; and
- queued proposals remain eligible on later scans without manual firing.

## Acceptance criteria

1. The behavior above is stated precisely in `docs/wfe-autonomy-runbook.md`.
2. The documentation does not imply that a commit SHA alone is a proposal
   identity.
3. No source code, workflow definition, generated documentation, or runtime
   configuration is changed.
4. Existing documentation checks pass.

# Canonical IR migration status

## Scope

Provider model messages use the shared canonical IR inside Aimee. Provider, SDK,
IDE and CLI wire formats are admitted or rendered at their external boundaries.
Session controls, authentication, policy and workflow records retain their domain
contracts. The browser and Discord use the shared thinclient; the thinclient
transports ordinary application requests as well as canonical model messages.

The migration branch is `fix/native-mcp-startup-and-editor`, targeting `testing`.
It contains IR unification, browser/thinclient repairs and removal of replaced or
unused code. Code/test/build changes must be measured separately from prose;
documentation deletion does not establish code reduction.

## Deferred work

ACP host execution/resources, durable delegation authority, container ownership
and lease recovery, audit/readiness draining, and cumulative accounting changes
were removed from this migration. Their implementation is preserved on
`proposal/deferred-execution-and-accounting` for independent review:

- [Delegation execution and authority](proposals/pending/delegation-execution-hardening.md)
- [Audit/readiness shutdown](proposals/pending/audit-readiness-shutdown-drain.md)
- [Cumulative usage accounting](proposals/pending/cumulative-model-usage-accounting.md)

Incoming ACP/OpenCode support remains. The outgoing ACP client had no production
execution path after the host feature was deferred; it and its unused decoder,
resource rendering and private result accumulator were retired. Codex/Claude
native decoders, controls, diagnostics and canonical history remain supported.
Canonical absent-versus-zero usage observations remain intact.

Confirmed unbuilt tests with no build or script driver were retired. Script-backed
bus-plugin tests were retained. Active regression fixture setup was consolidated
without dropping the provider/control cases.

The follow-up caller audit also retired the old terminal editor/input-history
and in-memory conversation-thread implementations, unused native-history shadows,
codec convenience APIs and shadow test substitutes, and obsolete rollout counters.
These had no production consumers; persisted histories were not deleted. A stale
resource-scope requirement that refused active delegates was removed while actual
cancellation, admission and capture checks remain. The legacy primary-session
reader's Go/C column ordering was corrected and checked against the shared reply
contract. Required one-time legacy import and external SDK/provider adapters remain.

## Validation and limits

The final source has a successful server build and focused C normal/sanitizer,
Go normal/race, frontend and ownership/IR boundary checks. The unchanged frontend
also passes the existing production-bundle Chromium loopback harness, 128 focused
frontend tests and 33 runtime-web race fixtures through the shared C thinclient.
These local fixtures do not establish acceptance by a live upstream provider.

The existing QA instance remains on `8fd0eddae`, with its history, SDK caches and
single Claude connection preserved. Actual browser evidence on that revision
covers HTTP model reads/follow-ups, native Codex reads/follow-ups and same-thread
continuation after restart, fresh Claude reads/follow-ups, exact-turn Steer/cancel,
and reload at 1280, 390 and 320 pixels. Per-alias repetition was not completed;
there is no claim that every model was tested live or that QA runs the final
migration commit.

A historical Claude context returned the provider's `reasoning_extraction`
refusal. No codec/history defect was demonstrated and the precise upstream
trigger is unproven. Preserve that negative result and authored history; do not
strip content or retry automatically to evade the refusal. Provider explanations
remain visible in the browser.

The unmerged authority migration was removed and subsequent IR migrations were
renumbered. The old QA migration checksums therefore differ from this branch;
do not upgrade it in place by waiving checksums or deleting history. The target
`testing` base ends at migration 50. A deployment needs the normal schema and
preservation review rather than treating the earlier QA image as final-source
qualification.

## Operational constraints

Do not investigate or modify `.100`, configure a KB instance, or perform WOL work
for this migration. Preserve the existing QA data and credentials. Keep automatic
reviews enabled; use registered Aimee Git commands or the established personally
authenticated fallback, never connector writes or attribution trailers.
Production promotion follows merge and the independent review requirements in
[OWNERS.md](../OWNERS.md). Earlier detailed handoffs remain in Git history.

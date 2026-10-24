# Canonical IR migration continuation

## Objective and constraints

Complete canonical IR use throughout the existing browser GUI, server, model
requests/responses/streams, durable history, delegates, workflows and tools.
External wire translation and opaque SDK caches remain boundaries. Retire
obsolete parallel implementations without removing supported behavior.

Continue on `fix/native-mcp-startup-and-editor` in this worktree. Keep one eventual
PR; PR administration and authentication questions are not implementation
prerequisites. Use parallel agents for independent implementation and validation.
Do not call the migration complete while implementation or required live checks
remain. Historical handoff versions are evidence, not current instructions.

Do not investigate or modify `.100`. No KB instance should be configured: unset
mode/endpoint are intentional. No WOL work belongs here. Keep automatic reviews
and strict attribution. Prefer registered Aimee Git commands, then SSH Git or
personally authenticated gh. No connector writes, coauthor trailers or branding.
Production promotion follows merge to main and the independent review requirement
in `OWNERS.md`.

## Current source and qualification

Committed checkpoint: `8be13dd5020a56b7d6a1b5b0530ef507730bf28f`.
The working tree contains further implementation under qualification; HEAD alone
therefore does not identify current source bytes.

That committed checkpoint passes the full native suite: 702 normal and 702
ASan/UBSan/leak invocations, 699 unique binaries in each lane, with 35 explicit
optional prerequisite notices per lane. All 76 lint gates pass. Compiler scratch
uses owned disk directories under `/var/tmp`; runtime fixtures retain their short
isolated `/tmp` paths. Original failed runs and their logs are retained separately.
Evidence: `/var/tmp/aimee-final-ir-disk-qualification/final-index.json`.

The aggregate-metrics checkpoint passes 510 frontend tests, typecheck and both
production builds; 18 normal and 18 race PostgreSQL assertions pass without skips.
Evidence: `/var/tmp/aimee-metrics-main-05dc/proof.json`.
Whole Go normal/race qualification before the current recovery changes passes
4,183 server tests, 43 control-web tests and 291 runtime-web tests per lane;
337 server tests have explicit optional skips. This does not qualify newer Go
ownership changes. Evidence: `/var/tmp/aimee-current-go-full-metrics/proof.json`.
Make shipping and thin CMake default builds pass at earlier joined checkpoints;
the final composed source still needs fresh shipping builds.

A genuine local Claude SDK 2.1.310 two-turn journey uses the normal private Vault,
registered CLI-owned project, server/native factory and canonical history. The
second prompt omits the random nonce, and the assistant recalls it exactly using
the same opaque native UUID. Both admitted generations complete without retries,
fallback or forced credential refresh; observed usage and the original native
cache archive are retained. Relevant owner inputs join to checkpoint `8be13dd50`.
Evidence: `/var/tmp/aimee-genuine-claude-endpoint-1933-proof/genuine-proof.json`.
This is one real native case, not final-image or all-service qualification.

Claude launch now disables the two bundled SDK plugins through supported
`--settings`; the strict zero-plugin admission guard remains unchanged. Actual
SDK 2.1.177 and 2.1.310 initialization both qualify that setting. No opaque caches
were deleted or synthesized.

## Implemented behavior and current work

Primary history loading and model/persona switching use the durable canonical
session log. Browser permission/question replies bind session, turn and control
identity; reconnect/reload do not resubmit actions. Native execution uses the
host-admitted canonical profile, supervised tools and source-bound replay/cache
owners. Internal disconnected planners/model loops and obsolete wire constructors
have been retired. External adapters remain at protocol boundaries.

Unknown usage remains distinct from observed zero through decoding, streaming,
wire output, retries/fallbacks, token audit, roundtables, cache metadata and metrics.
Metrics show known subtotals together with incomplete-usage counts. Current dirty
changes additionally preserve credential retries and failed handoff-repair usage,
including prior successful generations, cache facts and latency. Focused normal,
sanitizer and causal old-source failures are retained under
`/var/tmp/aimee-credential-usage-retry-proof/`,
`/var/tmp/aimee-handoff-attempt-proof/` and
`/var/tmp/aimee-credential-facts-reuse-proof/`.

The full container-backed ACP owner is implemented, including held Docker
endpoint/CID/image identity, private scoped Unix callbacks, supervised stdio,
bounded raw filesystem IO, canonical capture and actual action/effect policy.
Unknown post-spawn usage is retained on failed exchanges. Inline model-less ACP
admission uses a conservative counter key without inventing model identity.
The actual `.252` worker has progressed through raw filesystem writes/reads,
MCP and Bash-to-shipping-CLI execution; final production action-owner assertions
and frozen CLI entry-order qualification remain in progress. Component successes
are not a completed installed-application claim.

Current working changes preserve producer-issued revision worktree roots and
retain their write policy through container callbacks. The new focused regression
is being combined with the other owner changes. Do not broaden arbitrary worktree
path patterns or disable session isolation to make this pass.

Legacy reapers now exclude private owned containers. Durable restart recovery
uses the actual Docker daemon ID, held creation lock, recorded filesystem identities
and exact-CID retirement; mismatched/legacy/partial evidence stays fail-closed.
Focused recovery qualification passes 743 Go normal and race tests (two explicit
optional skips per lane) and 28 native normal/sanitizer cases. Evidence:
`/var/tmp/aimee-recovery-proof/qualified.json` and
`/var/tmp/aimee-reaper-refreeze-proof/proof.json`.
Actual Docker crash/rebind recovery still needs live qualification.

Two coordinated changes remain under implementation: lexical resource ownership
for every actual dispatch/retry/repair, and a checked ordinary container backend.
Keep request context through repair, retire before completing the session, poison
uncertain retirement, preserve the worktree if execution may still be active, and
only tighten existing deadlines. Do not reset a one-use native source capability.
The ordinary backend must use the held endpoint and actual creation receipt,
preserve image startup/callback behavior and live management/WFE hibernation,
and explicitly distinguish ordinary resource custody from private ACP IO/source
authority. Native delegated container support requires its own actual path proof;
resource ownership alone does not establish native launch support.

## Retained `.252` runtime and required live work

Host `192.168.1.252`, retained VM8168, guest `192.168.0.67`.
Compose project `aimee-internal-ir-fresh-601aff47e`, API8745/web8445.
Installed source remains `fb0d0f82635d6b0a2e20a55696aa35ee83523940`, tree
`36ea38743b25ec9d856c7d21e4c1a75860046e5b`, image
`sha256:540759735c93e4f683331545c33a46fd6cb183cca4defecaf206c35598e6a478`.
Its 87 products and 19 modules match its original source archive. It began as a
genuine empty installation at `601aff47e`; subsequent deployments are upgrades,
not new clean installs.

Preserve this VM, volumes, authority, grants, protected workspace files, 128 IR
records, five native sessions and complete SDK caches/binaries for manual review.
Keep the failed native first-Send history and its actual OAuth-refresh refusal;
do not retry/rewrite it into a success. The task retention controller lives on
host `.252`. Task-only unused build-cache reclamation preserved protected assets;
latest seal: `/var/tmp/aimee-acp-container-proof/after-final-resource-reclamation-seal.json`.
Never print credentials or reset application data to make validation pass.

After the coherent source is qualified, build the final 87-product/19-module
candidate, perform a normal same-volume upgrade and a separate latest clean
installation. Validate installed CLI ACP/MCP/nested read, implicit-cwd project
lookup, browser permission/question replies and replay without duplicate effects,
old-error placement, later success, mobile input and unknown latest usage. Run a
bounded real model/history/recall journey. Qualify configured-test learning grades,
restart and scoped later reuse without a KB; process/model success is not a grade.
Seal original history/cache/authority prefixes and identify actual new records.

## Remaining completion checks

- Finish and jointly review checked ordinary resource custody and per-dispatch
  cleanup, including cancellation, setup deadlines, retries, repairs and uncertain
  teardown. Complete actual fullworker and shipping CLI proofs.
- Run combined normal/sanitizer checks and fresh whole Go/race qualification on
  the final source, all required lint/module gates, and actual shipping builds.
- Complete final-image retained upgrade, separate clean install and the live
  browser/CLI/native/learning checks above. Label service prerequisites, platform
  checks and provider refusals precisely; do not claim all-platform coverage.
- Recompute production size using the existing counting rules. At `ae63ad600`,
  production is +449 lines against cleanup baseline `03d881e0e` and +5,438 against
  testing `251536ddb`. Genuine retirements exist, but the requested overall code
  reduction has not been established. Do not change the census, minify code or
  remove supported guards to improve this number.
- Update this handoff, push the completed work to the same branch, and bind fresh
  CI and live evidence to the actual final source. No more PR/auth detours.

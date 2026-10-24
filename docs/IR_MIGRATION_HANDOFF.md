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

The assembled resource/dispatch checkpoint follows `89829d60a`; its exact commit
and tree are recorded in `/var/tmp/aimee-final-resource-qualification/plan.json`.
Combined broad and live qualification is pending; earlier component receipts do
not establish final-image completion.

Checkpoint `8be13dd5020a56b7d6a1b5b0530ef507730bf28f` passes the full native suite: 702 normal and 702
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
Actual `.252` fullworker component integration passes raw filesystem write/read,
MCP through the built shipping CLI, Bash-to-nested-CLI read, canonical completion
with unknown usage and normal resource cleanup. Four registered file actions
produce real receipts; Bash retains its actual policy/source/result without an
invented fifth file-action receipt. Persistence is an explicit service peer in
this fixture: it is not installed-app/PG grading evidence. Proof:
`/var/tmp/aimee-acp-container-proof/fullworker-positive-proof.json`.

The installed CLI now enters MCP service before ordinary native command dispatch.
Actual built CLI cases pass 38 normal and 38 sanitizer-owner vectors, plus the
same missing-token vectors; the old actual installed binary reproduces the
initialization failure. SDK/subprocess binaries in those owner lanes are normal
builds, not themselves sanitizer-instrumented. Proof:
`/var/tmp/aimee-acp-container-proof/native-mcp-cli-entry-proof-final.json`.

Producer-issued revision worktree roots are retained through callbacks. Read-only
roles now mount even their private checkouts read-only. The actual old worker
allowed an opaque child to truncate its checkout directly; the corrected worker
refuses direct truncate/create while preserving ACP/MCP reads, Bash/nested CLI,
canonical completion and cleanup. The write-capable lane still passes. Original
parent/private bytes stay exact. Proof:
`/var/tmp/aimee-fullworker-readonly-proof/readonly-final-proof.json`.
The published legacy shared-worktree API remains supported; unused unpublished
child/source exporters are retired, with 58 retained function bodies unchanged.

Lexical resource ownership now spans each actual selected dispatch, including
credential/fallback attempts and handoff repair. Request context remains live
through repair; joined retirement precedes canonical completion and parent
application. Uncertain retirement poisons later execution and preserves the
worktree. Setup only tightens existing deadlines; one-use native source grants
are never reset. Normal headed native retry/repair is not blocked by the separate
ATTEST_NONE service-source one-use contract. A new resource epoch forks native
cache scope while retaining canonical history; primary UUID continuity uses its
existing stable owner. Focused lifecycle, real runtime/history/hash-owner and
explicit fake-SDK factory tests pass normally and under sanitizers. These do not
claim a new real native Docker journey. Proofs:
`/var/tmp/aimee-dispatch-container-plan-proof/final-index.json`,
`/var/tmp/aimee-composed-dispatch-898-qualification/proof.json`,
`/var/tmp/aimee-native-container-history-proof/final-index.json` and
`/var/tmp/aimee-native-history-nonempty-proof/final-index.json`.

The checked ordinary backend now retains actual creation receipts and the same
held local Unix Docker endpoint/CID/image for operations and retirement. Its
resource custody is distinct from private ACP IO/model-source authority. Image
ENTRYPOINT, health, environment and ordinary callback behavior remain supported;
legacy management/WFE hibernation remains separate. Unsupported checked transports
and attested nonrecursive controller source layouts refuse explicitly. Existing
VOLUME-extra-mount refusal remains; no volume stripping or added-mount exemption.
Private/ordinary journals use the existing cleanup/recovery owner. Old generic
reapers exclude owned containers; recovery checks durable daemon ID, creator
lock and original assets, retaining ambiguous old evidence. Source exposure of
private journals/helpers is refused before creation, using held kernel mount
identities, bind aliases and reachable recursive mounts for workspace and Git
metadata. Real local bind/tmpfs negatives and benign same-filesystem/nested cases
pass. Privileged external host remounts remain a trusted-host boundary.

The ordinary backend passes 779 Go normal and 779 race tests, with two existing
optional skips per lane, and three native normal/sanitizer targets. Linux-only
kernel fixtures have a Linux filename/build constraint. Independent source review
is clear; Make links, CMake changed-object builds and module gates pass. Proofs:
`/var/tmp/aimee-ordinary-final-proof/qualified.json`,
`/var/tmp/aimee-ordinary-portability-proof/go-proof.json` and
`/var/tmp/aimee-ordinary-resource-independent-review.json`.
Actual Docker crash/rebind recovery and the newly composed worker paths still
need their joint live qualification; earlier private-owner recovery receipts are
retained at `/var/tmp/aimee-recovery-proof/qualified.json`.

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

- Run both actual worker modes on the composed resource/dispatch source with a
  read-only journal observer retaining real CID evidence before successful normal
  cleanup erases it. Qualify actual Docker crash/rebind recovery and the ordinary
  image path; preserve the existing declared-volume refusal and original volumes.
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

# Canonical IR migration continuation

This is the current handoff. Earlier checkpoints are retained in Git, including
`d96dcaa5d9aae25342ad95c71dbff62048a79a3b:docs/IR_MIGRATION_HANDOFF.md`.
Their completion statements, branch restrictions and authentication questions are
historical. They are not current instructions.

## Objective and operating constraints

Complete internal canonical IR use throughout the existing browser GUI, server,
model requests/responses/streams, durable history, delegates, workflows and tools.
Wire translation belongs at external boundaries. Retire obsolete parallel
implementations while preserving supported behavior and opaque native caches.

Continue on `fix/native-mcp-startup-and-editor` in the existing worktree. Keep one
eventual PR; PR administration is not an implementation prerequisite. The user
explicitly instructed continued implementation and no more PR/auth questions.
Use multiple agents for independent implementation, review and validation.

Do not investigate or modify `.100`. Production promotion follows a merge to main
and the independent reviewer requirement in `OWNERS.md`. No KB instance should
be configured: unset mode/endpoint are intentional. No WOL work belongs here.
Keep automatic reviews and strict attribution enabled. Use registered Aimee Git
commands where available, then SSH Git or personally authenticated gh. Connector
writes, branding and coauthor trailers are forbidden.

## Current source and live runtime

Latest compiled and deployed test runtime:
`fb0d0f82635d6b0a2e20a55696aa35ee83523940`, tree
`36ea38743b25ec9d856c7d21e4c1a75860046e5b`.
The one test-only descendant `7925b40c00f865eae3e2ff2aef3f7aaa1a69bf10`
fixes diagnostic-file ownership and does not change production code.
Current branch head `eb4e3803b` additionally retires 1,001 production lines of
disconnected planners, model loops and resume APIs. Five focused normal and five
sanitizer targets plus Make/CMake shipping links pass; full combined checks are
running. Retained active function bodies and plan data layouts are unchanged.
This production delta is not yet deployed or claimed live-qualified.

The current image is
`sha256:540759735c93e4f683331545c33a46fd6cb183cca4defecaf206c35598e6a478`.
Its actual 87 products and 19 modules match the exact source archive. Core SHA256:
`a42652f925dc4fed5727be675ff9b7d884c289afe68d710bd08b5ba33c07e15e`;
SPA: `779856b846c902d0a5253c43b417087c277468be7fa1896586fa1877146669f3`.

The authorized `.252` test environment is retained VM8168, guest `192.168.0.67`,
Compose project `aimee-internal-ir-fresh-601aff47e`, API8745/web8445. The installed
application was genuinely empty at source `601aff47e`; subsequent changes are
normal same-volume upgrades, not additional fresh installations. The final
9c4-to-fb0 upgrade preserved all 125 prior IR records, five native sessions,
complete SDK caches/binaries, authority, grants and protected workspace files.
All three services are healthy; no SDK/native/control work was active at the seal.
The public leaf renewed normally with the same SPKI/issuer/subject; it is not
claimed byte-identical. The database container and mounts stayed intact.

Retain this VM for manual review. The task-owned retention controller lives on
host `.252`, not the local machine. Do not destroy/reset its volumes, credentials,
failed turns or caches. Private credentials belong in their existing files and
must not be printed.

## Implemented ownership

Primary history loading and model/persona switching use the durable canonical
session log. Browser controls and replies bind exact session/turn/control identity;
reload and reconnect do not resubmit actions. Native model calls use the original
host-admitted request/catalog, supervised tools and source-bound replay/cache
owners. SDK caches remain opaque and are never synthesized from a display log.

The outbound configured/inline ACP backend now has an owned canonical constructor,
held executable/cwd descriptors, clean child environment and strict deadlines.
It sends the latest USER once, projects prior admitted messages with explicit
fidelity, and preserves host system context. It refuses unsupported native resume,
hidden-generation/token/cost and no-internal-tools guarantees. Opaque agent tool-status observations
remain unverified diagnostics, never invented host ToolUse/ToolResult or grades.
Admitted ACP text and terminal events form the canonical answer boundary; actual
client filesystem/MCP intent obtains real host tool records only after source
and policy admission.
Usage can be explicitly unobserved; unknown is not observed zero.

Actual ACP filesystem and MCP calls use the shared admission, role, policy,
workspace, action/effect and canonical capture owners. Nested Bash-to-installed-CLI
calls have bounded source-bound scopes and restore enclosing action/effect/outcome
state. Expired, altered and foreign capabilities refuse. Exact valid nested-CLI
nonce/request replay returns its retained receipt without executing another effect. An invalid
scoped helper cannot fall through to ambient UID authority.

The active-project CLI repair resolves the implicit cwd in distinct repositories.
KB-free learning captures authenticated canonical observations and separately
qualified configured-test grades; scoped later requests revalidate their actual
source receipts. Process success alone is never a grade or causal repair claim.

External client wire adapters, opaque SDK cache formats, embeddings, OCR/table
recognition and external cognifier domain protocols remain explicit boundaries.
They are not competing internal model-history owners.

## Qualification and limits

Exact fb0 native suite: 699 normal and 699 ASan/UBSan/leak invocations, 696 unique
binaries per lane, no runtime failures or sanitizer reports. Each lane retains
33 prerequisite notices for PG/KB/KMS/root/cgroup coverage. A fixture diagnostic
initially made the strict Git-clean wrapper exit1; original evidence is retained.
The test-only fix passes all 38 public ACP cases normally and under sanitizers,
including the expired parent capability, without an untracked file.

All 76 lint gates pass on exact fb0. Full server-Go race passes 4,101 tests with
335 explicit optional skips; all three Go module inputs and C/H bridges match
its d96 qualification exactly. Frontend 501 tests, typecheck and both builds pass;
all 147 frontend inputs join exactly to fb0. The integrated bypass guard has 39 mutation tests
and no exceptions. Current Linux Make shipping links and supported thin CMake
links pass; Linux success is not fresh Windows/macOS execution.

Historical real integration at `03d881e0e` passes 173/173, including eight actual
SDK profiles, real Docker verification, canonical proof/grade publication,
restart recovery and later-session scoped reuse. It retains 25 unconfigured
service skips. Seventy separate real-PostgreSQL assertions pass without skips.
Learning owners and assertions are unchanged in the later joined source. These
results are not relabeled as a fresh full fb0 integration run.

Actual earlier live browser question/reply, original-UUID resume, server/NONE
service execution and HTTP Cancel/Allow have positive source-labelled evidence.
A native unknown-history Allow continuation was refused by the actual provider.
Preserve that failure and full cache; do not report it as positive or retry/rewrite
its history to manufacture acceptance.

The same-rule fb0 census removes 1,285 net production lines from cleanup baseline
`03d881e0e`, but the whole follow-up against testing `251536ddb` still adds 3,704.
The local disconnected-executor retirement removes a further 1,001 production
lines: cleanup becomes minus 2,286, whole follow-up plus 2,703.
This does not establish the requested overall reduction. Do not narrow the census,
minify source or remove supported behavior to change the count.

## Active work before completion

- Complete actual installed CLI ACP/MCP/nested-read and browser validation on fb0,
  plus a meaningful bounded native model/history/recall journey. Preserve the
  original history/cache/authority prefix and seal actual additional records.
- Complete combined qualification of the integrated dormant C model-loop,
  planner and resume retirement. Preserve active plan data/storage, job state, prompt
  helpers, voting and directive policy. Review dormant native constructor modes
  separately; absence of callers does not justify dropping supported coverage.
- Fix the confirmed transcript layout defect: errors are currently appended after
  all messages despite carrying their owning turn. Place old diagnostics with
  their own turn, preserve folding and legacy unknown-origin errors, and qualify
  later-success rendering and mobile input readability.
- The installed CLI structural probes so far are two retained prelaunch negatives:
  missing persona, then attempting to use a PAM-owned project as an ordinary UID.
  Both left IR125/SDK0. Use a normally registered CLI-owned shared source; do not
  weaken ownership, forge actor context, or repeat the same wrong-scope request.
- Recompute the production census, run checks appropriate to final deltas, and
  bind the final live image/source and evidence accurately. Any new production
  change requires its actual build and runtime qualification.
- Push completed work to the same branch. Fresh CI/released-platform checks and
  eventual delivery remain distinct from implementation. Do not stop coding to
  ask about PR authentication.

Do not mark the full goal complete while required implementation or live
qualification remains. Report unsupported paths, optional prerequisites and vendor
refusals precisely rather than claiming all-services/all-platform success.

## Evidence entry points

- `/var/tmp/aimee-ir-current-working-checkpoint.json` (working status, not proof)
- `/var/tmp/aimee-ir-fb0-acceptance-in-progress.json`
- `/var/tmp/aimee-final-fb0-native-qualification/final-proof-index.json`
- `/var/tmp/aimee-fb0-final-gates/lint/proof.json`
- `/var/tmp/aimee-fb0-go-race-input-join.json`
- `/var/tmp/aimee-assembled-frontend-final/proof-fb0.json`
- `/var/tmp/aimee-internal-ir-live-8168/candidate-fb0d0f826/producer-completion-index.json`
- `/var/tmp/aimee-internal-ir-live-8168/upgrade-9c4-to-fb0/normal-upgrade-qualification.json`
- `/var/tmp/aimee-fb0-open-acceptance-audit.json`
- `/var/tmp/aimee-final-03-integration-held/final-proof-index.json`
- `/var/tmp/aimee-ir-production-reduction-fb0d0f826.json`

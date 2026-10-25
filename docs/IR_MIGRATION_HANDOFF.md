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

Runtime source is `4e577b87355723e3ec24c003d8040ca7c2305c3b`, tree
`2caf7d88ac2676fb3cf9681df628f50227bd1a9b`, version
`v1.0.2-451-g4e577b873`. It includes current testing `35418e47e` through clean
merge `017e89520`; all eight affected Rust module-runtime tests pass. Later
ownership-ledger/documentation commits do not change application inputs; retain
an explicit byte join rather than relabelling old binaries as a later commit.
This is not yet a completed migration: the metadata repair below needs fresh
whole-source qualification and the remaining installed-image journeys are required.

Exact clean 4e source passes 702 normal and 702 ASan/UBSan/leak invocations,
699 unique binaries per lane, with 35 explicit optional-prerequisite notices per
lane. The lanes ran serially. All 76 lint checks ran: 75 passed and the sole
failure was the old memory-ownership fingerprint for server/agent_runtime.c.
The reviewed change adds only four job wait-phase heartbeat calls; removing
those lines reproduces the previous reviewed file byte for byte. Memory calls,
arguments, scope, budgets and Go ownership are unchanged. The ledger now records
that reviewed hash; the unchanged canonical gate must be rerun. No exemption,
rule relaxation or promotion-baseline refresh is authorized. Review:
`/var/tmp/aimee-final-source-review-coherent/memory-runtime-heartbeat-review.json`.
Native receipt:
`/var/tmp/aimee-final-4e577-native-qualification/qualification.json`.

Fresh whole Go normal and race each pass 4,615 pass events across the three
modules, with 337 explicit server prerequisite skips; all 24 exporter checks
pass. PostgreSQL history/learning/native/selection/metrics tests pass 109 normal
and 109 race cases with no skips. Its restricted database and role were dropped
and PostgreSQL stopped. CMake default-all thin-client build and actual 4e version
pass, as do shipping Make links. Source bytes and modes stayed unchanged during
each frozen qualification. Receipts:
`/var/tmp/aimee-final-go-coherent/final-index.json`,
`/var/tmp/aimee-pg-4e577/proof.json`,
`/var/tmp/aimee-cmake-4e577/proof.json`.

The 48 workflow-equivalent commands initially report 46 passes, the stale
memory fingerprint above, and the intentionally separate promotion-only surface
baseline. The integration branch must not re-freeze that promotion baseline.
The 411 workflow tests, attribution API/workflow cases, 41 IR mutations and actual
11-commit rename/copy history delta join pass. These are local checks, not fresh
GitHub CI or production-promotion approval. Receipt:
`/var/tmp/aimee-final-source-review-coherent/final-index.json`.

A clean locked frontend installation uses SmoothGUI 0.12.3, passes 510 tests,
`npm run check` and both production builds. All 149 tracked frontend inputs
match 4e, and the resulting SPA is byte-identical to the installed f21962de...
bundle. Upstream package metadata reports 0.12.3 at the recorded observation.
Ignored main-worktree node_modules held 0.11.0; only the clean locked receipt and
shipping byte join establish the tested dependency:
`/var/tmp/aimee-frontend-3999/proof.json`,
`/var/tmp/aimee-checkpoint-7db3-root/frontend-shipping-byte-join.json`.

Earlier exact 7db full client/server integration passes 172/172 and subsequent
checks, retaining 27 unavailable-service checks. Actual local Codex 0.163.0 and
Claude 2.1.227 use controlled HTTP fixtures there, not genuine paid inference.
The corresponding exact 4e full integration also passes 172/172 plus postchecks,
with 27 explicit unavailable-service skips. Receipt:
`/var/tmp/aimee-integration-4e577/proof.json`. These controlled provider fixtures
do not establish genuine inference on the installed final image.

## Live session metadata repair, pending final image validation

The first installed 4e one-call check failed before any provider request: job 7
could not admit canonical capture. Its existing parent
`3dad6876-648c-3db9-9256-e920608c798b` contains the accepted 33-byte client label
`cold-default-7db-41d0e11e80bb5766`. The old C full-record DTO rejects that stored
value and also breaks session listing. The parent, label and failed job remain
unchanged; this is not a provider, usage-counter or Rust principal regression.
Evidence: `/var/tmp/aimee-http-count-4e-41d0e11e80bb5766/`.

The repair reuses the measured-length owned session decoder and existing bus
operations, schema, message bound and deadline. Display readers own complete
metadata; authority readers use only exact ID, principal and incarnation fields.
Legacy fixed DTO consumers stay strict. Malformed/partial/extra rows fail
atomically. Focused producer wire and 11 caller fixtures pass normally and with
sanitizers/leak checks; independent source review is clear. The four intersecting
memory-ledger fingerprints record only reviewed owner-projection replacements;
classifications, memory symbols and guard rules are unchanged. Receipts:
`/var/tmp/aimee-session-metadata-proof/qualified.json`,
`/var/tmp/aimee-session-callers-proof/final-join.json`,
`/var/tmp/aimee-session-callers-proof/memory-pin-review.json`.

Next freeze and qualify this repaired source, build its exact installed image,
then load the existing long-label parent losslessly through normal list/get and
run a distinct bounded one-call job using that same unchanged parent. Preserve
job 7 as failed with zero provider sends. Genuine native replay, a new empty
installation and original retained-history upgrade remain required afterward.

## Implemented behavior and current work

Primary history loading and model/persona switching use the durable canonical
session log. Browser permission/question replies bind session, turn and control
identity; reconnect/reload do not resubmit actions. Native execution uses the
host-admitted canonical profile, supervised tools and source-bound replay/cache
owners. Internal disconnected planners/model loops and obsolete wire constructors
have been retired. External adapters remain at protocol boundaries.

Unknown usage remains distinct from observed zero through decoding, streaming,
wire output, retries/fallbacks, token audit, roundtables, cache metadata and metrics.
Metrics show known subtotals together with incomplete-usage counts. The implemented retry accounting additionally preserves credential retries and failed handoff-repair usage,
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

The newly composed a36 worker now passes both write and read-only modes with
actual CID observation: write `bc292a0ca4bf078b14e9627a97172dbc7fffe3aea36b40f91510ae6287b897f9`,
read-only `caae969b1838174a22db73295370b537d11421f85a96334a9aa340e2716426e7`.
Durable creation/removal records and independent absence checks bind both to the
same actual daemon. The first current-source run correctly refused a missing
fixture stop-state service before creation; the fixture now records the worker's
actual spawn and serves exact active/completed lookups. No diagnostic logging or
production decision changes are in the positive image. Original 128 IR records,
five native sessions, SDK caches, authority, grants and products remain exact.
Proof: `/var/tmp/aimee-acp-container-final-proof/a36-both-workers-final-proof.json`;
seal: `/var/tmp/aimee-acp-container-final-proof/post-a36-workers-seal.json`.

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
Actual ordinary-container execution, known-CID retirement and create-response-gap
recovery have checkpoint qualification under
`/var/tmp/aimee-acp-container-final-proof/ordinary-gap-final-proof.json`. Earlier
private-owner recovery receipts remain at
`/var/tmp/aimee-recovery-proof/qualified.json`; neither is a new final-image claim.

## Retained `.252` runtime and live work

Host `192.168.1.252`, retained VM8168, guest `192.168.0.67`. The supported
manual-review hold protects only this VM until explicit user cleanup. Its owned
disk was expanded online from 64 to 84, then 104 GiB with root/EFI identities unchanged;
resource gates remain 9 GiB free disk and 3 GiB MemAvailable. No foreign VM,
volume, cache or reaper policy was removed or weakened.

Original Compose project `aimee-internal-ir-fresh-601aff47e`, API8745/web8445,
remains at d851, image
`sha256:a8198936540f98237f950726a0e7327a905235843f1687b93a08804db3ff24e2`.
Its original 128 IR records, five native sessions, opaque SDK caches/binaries,
authority, grants and protected workspace files remain exact. It began as a
genuinely empty installation at 601aff47e; subsequent deployments are upgrades.
Keep the failed first native OAuth-refresh refusal as failed history.

Separate project `aimee-internal-ir-fresh-d8511ad41`, API8747/web8447, started
with zero sessions/IR/native caches at d851. It is now upgraded to 4e, image
`sha256:935ceaa4aacd5596a743579868b8d53a134e050aa94ca095477af47821ad38ac`.
All 156 prior IR rows and native caches/authority/grants survived the 4e upgrade
exactly. Later live jobs add genuine new history. This is not a clean 4e install.
Both app inventories match 87 products and 19 modules.

Actual d851 browser testing passes normal login/project clone, pending question
reload and one reply, known/error/unknown usage placement, normal Steer and one
permission Allow with one real 36-byte file write. Desktop and 390/320px views
fit, and passive reload/idle does not replay effects. Controlled HTTP model cases
are labelled controlled; the genuine installed Claude 2.1.177 recall case uses
two sends, the same opaque native UUID, two-to-four history messages and observed
cache-write 17385 followed by cache-read 17385, with no retries or tools. Original
closed-permission history also passes zero-Send replay checks. Screenshots and
private canonical/cache seals remain under
`/var/tmp/aimee-internal-ir-live-8168/fresh-d8511ad41/` and
`/var/tmp/aimee-internal-ir-live-8168/upgrade-fb0-to-d851/`.

Actual 7db live validation exposed two defects whose source repairs are now
implemented; the CLI live positive passes and accounting is being validated:

- Relative `workspace add .` still registers the server cwd. Served argument
  metadata bypasses the corrected compiled marshaller. The failed registration
  was removed normally and the exact original registry restored; no index query
  was issued. The shared served/compiled workspace-root finalizer, malformed metadata refusal
  and actual HTTP/UDS regression pass normal/SAN plus all 76 lint checks. Do not claim implicit scope fixed
  until installed same-basename A-to-B-to-A lookup passes without `--project`.
- A tool-free readonly validation job completed and cold-acquired the default
  Ubuntu image through the normal daemon, with actual CID lifecycle/retirement
  proof, but reported api_call_count=0 despite one observed HTTP response. That historical accounting assertion remains failed; do not rewrite it. The
  retained durable-job progress owner now counts observed HTTP-client outcomes
  across plain/tool-loop, retry/fallback and joined native/script callbacks,
  initializes from the real stored baseline and serializes count plus DB writes.
  Actual HTTP focused normal/SAN tests, cache/no-send and scope/lifetime cases
  pass. A new installed one-call case must report one observed call.

Actual normal readonly `validate` training has business DONE, canonical COMPLETED
and a protected configured-test grade on 7db. Normal restart preserves 145 prior rows and native caches/authority/grants;
a new parent successfully reuses the protected grade in actual server-owned
system/developer context, while the foreign-project lookup returns none. The
three controlled HTTP requests (two train, one reuse) omit usage, and cost stays
unknown. The normal provider process is stopped and all actual CIDs retired.
Proof: `/var/tmp/aimee-learning-live-validate-7db-0aa039e8e41cad58/final-learning-seal.json`. Earlier failures are preserved:
mandatory sandbox refusal; Docker optional Healthcheck inspection; absent default
image; missing genuine verification configuration; and the correctly rejected
no-file-changes code-role job. A configured-tests-only grade from that last job
is not business or semantic success. The correct verifier config owner is
Aimee home `projects/<main-repo-basename>/project.yaml`, normally generated from
the actual Makefile, not an invented `.aimee/project.yaml` verification block.

Further review found native aggregation discarded earlier unknown-usage facts
and could retain a stale prior completion after a later preframe failure. The
aggregate repair passes 24 mixed streamed/buffered sequences, normal/SAN and
all lint checks. The failure-boundary wrapper passes focused normal/SAN tests: only
observed model-transport invocations taint missing completion usage; typed
quiesced diagnostics preserve known subtotals without manufacturing a model
response. Auxiliary count_tokens calls do not invalidate a prior completion.
A failed latest admitted generation cannot transfer an earlier final/cache.

## Remaining completion checks

- Finish composition qualification of the native failure/usage projection and
  shared job progress owner; keep original per-generation facts and failure
  history. Preserve metadata precedence, cache/no-send, retry/fallback and full
  callback-scope restoration; add no authority or guard exceptions.
- Preserve the source-bound 7db learning/restart/scoped-reuse positive and all
  failed history; qualify the relevant final-source installation changes.
- Freeze the repaired source, qualify the affected normal/sanitizer and full
  required gates, build its real 87-product/19-module image, then perform the
  retained original-volume upgrade and a separate latest-source empty install.
  Validate installed CLI scope, native recall, controls/replay/mobile and
  learning on the proper source. Keep optional/platform/provider limits visible.
- Requested overall production-code reduction is not established. At 7db the
  unchanged census is +1,929 physical production lines against cleanup baseline
  03d881e0e and +6,918 against testing 251536ddb. At 95c3ffecf they are +1,917
  and +6,906 respectively under the identical counting rules. Subsequent source
  must receive a fresh census. Substantial obsolete paths were
  retired, but ownership and container support added more. Do not change counting
  rules, minify code or delete supported behavior to manufacture a reduction.
- Update this handoff with actual final evidence, keep work on the same branch,
  and obtain source-bound fresh CI when implementation is complete. No PR/auth
  detours, no `.100` actions, no KB instance, no claim of 100% while work remains.

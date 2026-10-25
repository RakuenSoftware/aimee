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

## Current implementation and qualified checkpoint

The user clarified the architectural scope on 2026-10-25: **“A message is an
IR.”** Different task payloads stay in their existing messages. Do not invent a
parallel inference schema, or exempt speech, recognition and embedding messages
merely because their payloads differ from conversation content.

Recovery checkpoint `6afbaf919` adds only the reviewed ownership-ledger update
to `b517daa71`; all other 10,218 tracked entries are identical. Its 76 lint
checks pass and its `.252` producer has all 87 products and 19 modules. It has
not been deployed. The b517 normal and ASan/UBSan lanes each ran 698 invocations:
695 passed inside private temporary storage, and the three binaries sensitive
to root ownership passed unchanged on the host. Both original suite exit-2
receipts are preserved; this is composed coverage, not a uniform full-suite
pass. There were no sanitizer diagnostics and 35 explicit prerequisite notices
per lane. Receipts:
`/var/tmp/aimee-recovered-ir-b517-private-tmp-qualification-r3/final-index.json`,
`/var/tmp/aimee-recovered-ir-6afb-lint/`, and
`/var/tmp/aimee-internal-ir-live-8168/candidate-6afbaf919/`.

The continuation repairs additional active message-consumer defects:

- Speech uploads bytes and decodes complete transcript text instead of passing
  vendor JSON into chat. Local CLI arguments/results and concurrent temporary
  outputs retain their own boundaries. TTS preserves structured request fields
  and complete binary audio rather than truncating JSON or using `strlen`.
- The common buffered HTTP reader refuses incomplete declared/chunked bodies
  before copying or publishing them. Binary requests/responses retain explicit
  lengths. Windows header conversions preserve complete fields and refuse
  conversion/header failures; local Windows checks use the exact function
  source with controlled WinHTTP peers, not a real Windows runtime.
- OCR/TSR messages use strict framing and atomic payload admission, retaining
  documented optional defaults and empty results while refusing invalid or
  silently truncated text, geometry and cell data.
- Embedding consumers preserve the actual dimension separately from row count
  and buffer stride, and honor decline/truncation facts before publishing a
  complete vector. Go batch generation retains the actual bounded width and
  the public client admits the existing batch/dimension payload variants.

These repairs require combined-source qualification and a new exact image
before final installed validation. Focused evidence is under
`/var/tmp/aimee-stt-proof/`, `/var/tmp/aimee-tts-boundary-proof/`,
`/var/tmp/aimee-http-framing-proof/followup/`,
`/var/tmp/aimee-sidecar-admission-proof/`, and
`/var/tmp/aimee-embed-consumer-evidence/`. The 6afb image is a checkpoint,
not evidence that these later repairs were deployed.

Recovered on 2026-10-25 from local session
`01a14bc2-2fb8-7751-95af-afed5428382a`, whose last saved action was at
02:49 UTC. Continue in this worktree and branch; the main repository checkout
is an unrelated branch. The recovered session's full internal IR objective,
code-reduction requirement and `.252` authorization remain in force.

The latest native/lint-qualified checkpoint is `641fe8ea4`: 700 normal and
700 ASan/UBSan invocations (697 unique binaries per lane), with all 76 lint
checks passing. The sanitizer receipt completed before the crash:
`/var/tmp/aimee-final-retirement-native-qualification/san/proof.json`.
This does not qualify subsequent patches or a final installed image. Both
retained `.252` installations were independently found healthy after recovery;
they still run the previously recorded d851 and 199 images below.

The last fully qualified source checkpoint is
`19911237b0cff2f66edf9de4b943be99768bf38c`, tree
`c770deae4f752af62d3057f14f45833b7449a447`, version
`v1.0.2-452-g19911237b`. Current testing remains `35418e47e`; its affected Rust
runtime tests pass. This continuation retires dormant execution/projection code
from that checkpoint. It requires combined-source qualification before its
image is called final. The migration is unfinished.

Exact clean 199 passes 702 normal and 702 ASan/UBSan/leak invocations, 699 unique
binaries per lane, 35 explicit optional-prerequisite notices per lane. Lanes ran
serially; all 76 lint checks pass. Reviewed ownership fingerprints changed only
for host heartbeat and exact session-owner projections; classifications, memory
calls, scope, budgets, Go ownership and guard rules remain intact. Receipt:
`/var/tmp/aimee-final-session-native-qualification/qualification.json`.

Fresh actual PostgreSQL passes 109 normal and 109 race cases with no skips;
the restricted database/role were dropped and PostgreSQL stopped. CMake default
thin-client ALL/version passes. Full composed integration passes 172/172 plus
postchecks with 27 explicit unavailable-service skips. Its actual Claude 2.1.227
and Codex 0.163.0 binaries use controlled HTTP fixtures, not genuine inference.
Receipts: `/var/tmp/aimee-pg-199112/proof.json`,
`/var/tmp/aimee-cmake-199112/proof.json`,
`/var/tmp/aimee-integration-199112/proof.json`.

The 48 workflow-equivalent commands pass 47; only the deliberately separate
production-promotion surface baseline remains red. Do not re-freeze it on this
branch. All 411 workflow tests, attribution checks, 41 IR mutation cases and
actual history/metadata joins pass. These are local checks, not fresh GitHub CI
or production-promotion approval:
`/var/tmp/aimee-final-source-review-19911237/final-index.json`.

Go/exporter inputs (2,269 files), frontend/module inputs (152), and Rust/shared
ABI inputs (40) match the fully qualified 4e source byte for byte, including
modes: `/var/tmp/aimee-final-input-join-19911237/input-joins.json`. That prior Go
normal/race qualification passes 4,615 events each with 337 explicit prerequisites
skipped, plus 24 exporter checks. The clean frontend uses SmoothGUI 0.12.3,
passes 510 tests, type checking and both production builds; its SPA f21962de...
bytes match the installed 199 image. These input joins preserve the actual
source/version of each receipt rather than relabelling binaries.

## Installed session metadata and accounting regression

Installed 4e job 7 failed before any provider request because its existing parent
`3dad6876-648c-3db9-9256-e920608c798b` has the accepted 33-byte client label
`cold-default-7db-41d0e11e80bb5766`. The old C DTO also broke session listing.
The repair reuses the measured-length owned decoder and existing bus operations,
schema, message bound and deadline. Display readers own complete metadata;
authority readers retain exact ID/principal/incarnation bounds. Malformed,
partial and extra rows fail atomically. Focused normal/sanitizer tests and
independent boundary review pass; no input restriction or truncation was added.

The actual 199 QA upgrade preserves all 156 prior IR rows, caches, SDKs,
authority, grants and the entire failed job 7. Normal UID1000 list/get both return
HTTP200 with the unchanged full label and parent identity. A distinct job 8 uses
that same parent and completes canonically with one observed HTTP POST and
api_call_count=1; omitted usage leaves cost unknown. Job 7 stays byte-exact,
failed with zero calls. The ordinary container was observed through creation,
start, destruction and independent absence; provider/observers are closed.
The store now has 164 IR rows. Receipts:
`/var/tmp/aimee-internal-ir-live-8168/upgrade-qa-4e-to-199-final/upgrade-preservation-proof.json`,
`/var/tmp/aimee-internal-ir-live-8168/upgrade-qa-4e-to-199-final/normal-parent-metadata-lossless-proof.json`,
`/var/tmp/aimee-http-parent33-199-cdb483c85d7c34ec/final-index.json`.

Installed 199 same-root A-to-B-to-A index queries pass as UID1000 with implicit
CWD, strict single JSON, distinct canonical project identities and each root's
own C definition. No project override, registration/config/default/KB mutation,
provider prompt or retry was used:
`/var/tmp/aimee-installed-index-199-ready-join/joined-proof.json`.

## Retiring obsolete owners and remaining ingress review

This continuation removes dormant CLI screen-response execution, its unused
isolated credential-copy helpers, an unreferenced mixed-wire pruning helper,
unused native roundtable brief/result projection, and an unreachable quorum-vote
and aggregate-budget facade. Supported PTY forwarding/startup settling, native
HOME/cache custody, OAuth setup/login, active canonical compaction, model/project
budgets and hard directives remain. Retained function bodies and source sections
have exact byte joins; only four obsolete cleanup-field lines change in the
retained CLI destructor. Independent reviews are clear and focused tests pass.
The first focused budget runner omitted its provider fixture; the unchanged old
199 binary reproduces the same assertion. The corrected runner with actual
qualified peers passes both retained targets normally and under sanitizers.
No source/test assertion was weakened to fix that harness error. Receipts:
`/var/tmp/aimee-screen-retirement-exports/qualified.json`,
`/var/tmp/aimee-screen-retirement-exports/independent-review.json`,
`/var/tmp/aimee-roundtable-retirement-proof/independent-final-review.json`,
`/var/tmp/aimee-vote-retirement-proof/independent-review.json`,
`/var/tmp/aimee-vote-retirement-proof/with-provider-fixtures/qualification.json`.
Only obsolete helpers' own test cases are retired; active PTY/native/OAuth,
business description/liveness and prompt-budget targets remain.

The recovered delegate-reply repair removes the unread mailbox and its MCP
advertisement. The retained HTTP compatibility route explicitly refuses instead
of acknowledging an undeliverable answer; supported questions continue through
the canonical operator control endpoint. Independent review and 12 focused
normal/sanitizer invocations pass, including root-namespace control authorization
and duplicate-reply rejection. The old handler probe reproduces false success.
Receipts: `/var/tmp/aimee-delegate-reply-proof/recovered-focused-proof.json` and
`/var/tmp/aimee-delegate-reply-proof/recovered-independent-review.json`.

The unused per-message router and detached classifier are also retired. All
five active routing bodies remain byte-identical; actual router/autonomous tests,
IR bypass guard and mutation tests pass. Receipt:
`/var/tmp/aimee-router-retirement-proof/recovered-review.json`.
The IR guard now refuses reintroduction of those retired entries and mailboxes.
Recovery also retires the uncalled CLI Markdown renderer and unused tool-prompt
collector/embedded-table generator, including their solely owned tests and build
registrations. Current CLI output and streaming code, all 14 retained policy
functions, active registry/catalog behavior and the eight authored tool notes
remain byte-identical. The renderer removes 1,054 production source/header lines;
the collector removes 93. Make/CMake thin-client builds, canonical CLI stream,
three retained policy/tool/ACP targets normally and under ASan/UBSan, module
ownership and registration checks pass. Receipts:
`/var/tmp/aimee-markdown-retirement-proof/review.json` and
`/var/tmp/aimee-tool-prompts-proof/`. The published native-frame parsing API is
retained; its stale live-builder documentation was corrected.
Combined-source qualification remains required. The first recovered integration
attempt refused before test execution because its saved custom OBJDIR did not
match the harness's fixed paths. It is preserved under
`/var/tmp/aimee-delegate-reply-integration/`; the standard-OBJDIR rerun is under
`/var/tmp/aimee-delegate-reply-integration-recovered-defaultobj/`.
Final genuine native replay, separate latest-source empty install, original
retained-history upgrade and fresh source-bound CI remain required.

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
with zero sessions/IR/native caches at d851. It is now upgraded to 199, image
`sha256:6bae300ff30745c9c0001505e6447343846fa7b33f8ae7e126f5a6617ed6756c`.
All 156 prior IR rows and native caches/authority/grants survived that upgrade
exactly; job 8 adds new history for 164 current rows. This is not a clean 199 install.
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
implemented and now have installed 199 live positives above:

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
  pass. Installed 199 job 8 now records one observed call; the earlier failure remains intact.

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

- Complete the registered delegate-reply ingress cutover and final combined-source qualification.
  Preserve the already qualified native failure/usage projection and
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
  and +6,906 respectively under the identical counting rules. At 199 they are
  +2,277 and +7,266. This continuation removes dormant production code, but subsequent source
  must receive a fresh census. Substantial obsolete paths were
  retired, but ownership and container support added more. Do not change counting
  rules, minify code or delete supported behavior to manufacture a reduction.
- Update this handoff with actual final evidence, keep work on the same branch,
  and obtain source-bound fresh CI when implementation is complete. No PR/auth
  detours, no `.100` actions, no KB instance, no claim of 100% while work remains.

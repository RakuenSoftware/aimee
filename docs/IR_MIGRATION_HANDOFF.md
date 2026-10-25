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

The composed source after merge 017e89520 contains the repairs below. Its
final whole-source and installed-image checks remain outstanding. These are
earlier source-specific qualification checkpoints, not a completed migration.

Checkpoint `7db3f9c67e1e5add544ae9b07ab5bf4de2fc8f2e`, tree
`bf73f238948be72daef6458fa290307b171e23d7`, is pushed to the same branch. This
checkpoint is not migration completion. Commit 95c3ffecf adds qualified served-
metadata workspace paths and canonical native tool counts, retiring duplicate SDK
usage/tool display parsers. The branch also includes current testing 35418e47e
through clean merge 017e89520; its eight Rust module-runtime tests pass.
Later fixes must receive source-specific
qualification; do not relabel older binaries or live cases as a new source.

The checkpoint passes 702 normal and 702 ASan/UBSan/leak native invocations
(699 unique binaries per lane), all 76 lint checks, shipping Make linkage and
CMake default-all thin-client build. Each native lane retains 35 explicit
optional-prerequisite notices. Whole Go normal/race passes 4,281 server pass
events, 291 runtime-web and 43 control-web cases per lane; the server retains
337 explicit skips. Exporters pass 24 cases. The private PostgreSQL history,
learning, native, selection and metrics pattern passes 109 normal and 109 race
cases with no skips; its database/role were dropped and server stopped.

Full `make -C src -j2 CC=gcc integration-tests` passes 172/172 and its subsequent
checks, with 27 explicit unavailable-service checks. Actual installed local
Codex 0.163.0 and Claude 2.1.227 binaries use controlled HTTP fixtures in this
suite; it does not establish genuine paid-provider or live Docker coverage.
Independent workflow-equivalent checks pass 47/48 commands; the remaining
promotion-only surface baseline must not be re-frozen on this integration
branch. These are local checks, not fresh GitHub CI or promotion approval.

A clean, locked frontend installation uses SmoothGUI 0.12.3, passes 510 tests,
`npm run check` and both production builds. Its 149 tracked frontend inputs
match this checkpoint. The ignored main-worktree node_modules had 0.11.0;
only the clean-install receipt establishes the tested dependency version.
Upstream package metadata also reports 0.12.3 at the recorded observation.

Hashed source-specific receipts are joined in
`/var/tmp/aimee-checkpoint-7db3-root/index.json`. Native, Go, source-review,
PostgreSQL, integration, CMake and frontend receipts remain separate so their
scope and skips are visible. Failed cases remain retained.

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
disk was expanded online from 64 to 84 GiB with root/EFI identities unchanged;
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
with zero sessions/IR/native caches at d851. It is now upgraded to 7db, image
`sha256:855b36754d0d1e91f923f2a97e1b80cf90f5be400fa5a2a4236eba6e0097c3b1`.
Its prior 118 IR rows and caches/authority/grants survived that upgrade exactly;
later live jobs add genuine new history. This is not a clean 7db installation.
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
implemented; matching-image live positives remain required:

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

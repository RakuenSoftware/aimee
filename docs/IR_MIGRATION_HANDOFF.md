# Aimee overnight continuation handoff

Prepared 17 October 2026, with subsequent continuation checkpoints. The current
user instruction is to keep implementing the unfinished migration; the historical
stop/handoff request below is superseded.

## Current continuation status: live `.252` acceptance follow-up

### Active work and delivery constraint: 22 October 2026

Continue implementation and validation on `fix/native-mcp-startup-and-editor`.
The user's current instruction is one eventual PR for the remaining migration,
with no intermediate PRs or merges. An open PR is not a prerequisite for any
implementation or local/live validation. Earlier follow-up-PR instructions below
are historical. Do not investigate or change `.100`; validation uses the owned
`.252` environment.

### Full internal conversion in progress: 24 October 2026

The internal tool-catalog cutover now uses one admitted canonical owner for
HTTP/native execution, role filtering, respond, schema normalization and current
execution definitions. Provider-shaped builders and unused driver callbacks are
retired; the common model profile clones only tools, so catalog metadata cannot
override host controls. Three valid prior catalog byte vectors remain exact;
canonical respond also repairs the formerly invalid Anthropic wrapped-tool case.
Seven focused targets and the three-profile driver checks pass normally and under
ASan/UBSan/leak checking. This cutover removes 372 net production lines. Combined
retirements remove 2,013 net production lines before the ACP terminal repair.
Full combined qualification and actual updated SDK matrix remain pending.

The standalone ACP classification defect is repaired: admitted MAX/refusal/
unknown, unfinished-tool and reasoning-only responses retain their full canonical
diagnostics but cannot become successful answers. The common standalone adapter
guard uses the same completion owner; host scheduler staging remains intact.
Three focused targets pass normally and under ASan/UBSan/leak checking, with
actual old-code failing regressions. Unused untyped provider-client completion,
build and import helpers are retired; their wire goldens now exercise the actual
canonical completion transport. Together the cleanup removes 2,080 net production
lines before the provider health-probe cutover.

Provider HTTP health probes now own admitted canonical fixed requests and full
canonical diagnostic responses. Their native JSON/SSE bytes remain inert raw
metadata, and shared final-text admission rejects truncation, refusal, tools,
unknown terminals and malformed known usage. The unmanaged native CLI probe
worker and its pre-network-guard callbacks are retired: without a source-bound
native diagnostic owner, probing reports not tested and directs users to a
conversation. Configuration metadata and `--no-run` remain available; actual
native execution still uses the managed factory. The UI shows a neutral result;
CLI requested unsupported inference exits 2, failed HTTP inference 1, metadata 0.
Focused provider normal/race, standalone export, CLI normal/sanitizers and UI
regressions pass. Independent review also closes explicit streamed refusal
overwrite, duplicate closed-text aliases and nested response-identity conflicts;
old-source failures and final focused normal/race results are retained. The integrated bypass gate passes 31 tests.

Measured from `03d881e0e`, this continuation removes **2,106 net production lines**
(969 added / 3,075 deleted, excluding tests, documentation, generated locks and
build tooling). It retires parallel implementations rather than compressing source.
The final CLI probe exit policy now lives in its existing response owner, keeping
the forwarding file below the 2,500-line lint limit without exemptions.
A further 195-line frontend retirement removes the unsupported legacy thread
controls/capability fetch and three unused flat-history render/rewind components.
Archived messages and thread metadata remain intact; active IR rendering and
replies are unchanged. All 494 frontend tests, type checking, both builds and the
31-test bypass suite pass; new old-source failing cases cover fake capability
responses and archive preservation. The combined C baseline
`f74a037ae` passes all 693 normal invocations, 693 ASan/UBSan/leak invocations,
76 lint gates and both CMake native runtime/factory tests. The small later probe
CLI/printer delta has focused normal and sanitizer coverage. Final combined Go,
frontend, SDK, image and live browser acceptance remain pending; these results
must not be relabeled as final-source deployment acceptance.

The prior `.252` VM8167 was stopped and destroyed at 21:38 UTC before the new
question/service/hybrid cases sent any model request. The exact controller-exit
trigger is unconfirmed; no reaper-expiry claim is made. Local exports retain the
173-case integration, 70 real-PostgreSQL checks, both normal upgrades and the real
native resume with original UUID/full cache prefix preserved. The new task-owned
lease controller retains its VM on ordinary errors and requires explicit cleanup.
Replacement VM8168 has built source `601aff47e`: all 87 source-derived products
and 19 modules match. Its actual eight-profile SDK qualification passes with
47 full catalog snapshots across 18 observed history-phase groups, typed refusal
and all four source fences; source entries and all eight products remain exact.
Opening/established catalog differences are limited to the existing native-shell
policy. The independent source/product/catalog recomputation also passes. All
76 lint gates pass on that exact source; Go/exporter and frontend sources remain
identical to their qualified baseline except for the separately tested CLI delta.

A genuinely empty installation of `601aff47e` passes normal PAM/admin/setup,
normal supported credential enrollment, and actual installed `index investigate`
in two distinct repositories using implicit cwd and no explicit project/ambient
scope. No KB instance is configured. Initial startup stopped before launching the
app because pinned PostgreSQL/embedder dependencies were absent; exact-digest
pulls and same-project startup preserve the generated environment/PAM material.
The initial failed attempt remains evidence; no bootstrap or credential reset ran.

The actual browser question case passes: one Send, one request_input and one
answer; pending controls survive reload, and completed controls stay closed after
reload. The 13 canonical records, exact tool/control IDs, completed/checkpoint,
one SDK invocation/two generations, full SDK cache and zero workspace effects
are independently verified. All 22 observed SSE responses are HTTP 200 with zero
EventSource errors; intentional navigation/close aborts remain recorded. This
does not establish the cause of the older failed question case.

The first real registered service trigger correctly refuses the test fixture's
unsupported monetary ceiling before any SDK launch: `delegate CLI execution
cannot enforce a monetary cost limit`. Its real work item, private source scope,
normal owner Stop/removal, unchanged question history/cache and sealed source
records remain negative evidence. The production refusal stays intact. A distinct
supported positive service case must use admitted turn/deadline bounds (and output
bounds where already supported) without claiming dollar-limit enforcement. That case, hybrid permission/fault
recovery, final candidate upgrade/native replay and responsive idle/editor checks
remain unfinished. No final live or CI completion is claimed.

Subsequent `ce789ccf3` qualification passes all 76 lint gates, the frontend
retirement checks and all 87 image products/19 modules. A normal same-volume
`601aff47e` to `ce789ccf3` upgrade preserves all prior Q13 history, native records,
full SDK cache and protected authority. Public TLS leaf renewal retains its SPKI;
the prior full PEM was not exported, so no full prior-PEM equality claim is made.
The actual original-UUID resume then passes: one Send, no tools/replies, original
4159-byte cache prefix preserved and appended to 6471 bytes, unchanged cache/cwd
inodes and 7 new canonical records (current Q20). Independent checks accept it.

The distinct supported service case uses a new normal alias with one admitted
generation, effective native timeout at most 180 seconds, output cap 512, tools
false and no monetary claim. It exposes a real local scope defect: its actual SDK
receives HTTP 403 `native request scope unavailable` before any observed upstream
inference. The constructor has consumed the private host capability, but the
HTTP forwarder still rejects honest server/NONE sources. Its failed child/parent
records and opaque SDK error cache are retained; normal owner Stop/removal leave
Q20 and its full 6471-byte cache untouched.

The fix retains the consumed capability only in the owned runtime and activates
read-only callback-local proof around its dispatch. Forwarding requires matching
canonical profile/model/launch, live capture, actor/session/capabilities,
credentials, cwd and workspace. No wire field can activate it; honest NONE and
trusted=0 remain unchanged. Three focused fixtures pass normally and with
ASan/UBSan/leak checks. The old HTTP guard reproduces the actual 403 regression;
cryptographic constructor admission, callback activation and HTTP policy are
separately qualified layers, not claimed as one mocked end-to-end proof. Actual
service success on the corrected image remains required.

The earlier monetary negative also exposes a real retry defect: its permanent
refusal was labeled transient `runner_unavailable`. It now parks as
`cost_limit_unsupported`, with explicit operator resume and the refusal intact.
Actual PostgreSQL regressions reproduce repeated old scheduler retries and prove
one new park, zero model dispatch/spend, and operator-only retries. The four
changed Go packages pass 902 tests/subtests in each normal and race run; 49
unrelated optional family checks remain explicit skips. Required engine/store
checks use actual PostgreSQL with zero skips. A separate existing fake-plane E2E
fixture now supplies its required explicitly synthetic source issuer; this is
not evidence of C MAC attestation. Final combined qualification, corrected-image
service execution, permission/fault recovery, responsive idle/editor and one
final PR/fresh CI remain unfinished.

The service-forwarding source `4d5ab125c` passes all 693 normal C invocations,
693 ASan/UBSan/leak invocations, all three Go modules normally and under race,
and all 24 exporter checks. Its only full-lint failure was the HTTP file growing
to 2,505 lines. Consolidating the identical Responses/Anthropic typed-stream
wrappers into one handler-parameterized live writer brings it to 2,495 without
an exemption. Focused actual HTTP normal/sanitizer checks cover both private
NONE streaming routes, exact preserved owner context and protocol-owned terminal
events without an OpenAI `[DONE]` trailer. OpenAI chat framing remains separate.
The final image/current-source lint and post-fix live service/hybrid cases still
must qualify; the intermediate `4d5ab125c` image was not deployed.




The user's completion criteria now include a significant reduction in production
code. The first retirement pass deletes 1,439 production lines with no replacement
production code: obsolete CLI event/callback transports and duplicate raw request,
sampling and Anthropic system builders. Useful native history/resume/control and
provider/cache tests now exercise the active canonical adapters. The CLI pass
passes six normal and six ASan/UBSan/leak targets; the request-builder pass passes
five of each. The integrated bypass gate passes 26 tests. Broad combined-source
qualification remains pending; these focused results are not final acceptance.
A second pass retires seven unused migration toggles, body-comparison/build
shims and flat persona helpers: another net 202 production lines removed. Its
six focused targets pass normally and under ASan/UBSan/leak checking. Moving the
persona regression to the actual typed stages exposed a real bug: delivery-state
tracking stopped at the first user message before seeing a later assistant. A
restarted established session could receive its persona again after compaction.
The typed owner now checks all assistant roles first; an old-code failing regression
covers established history, compaction, and a fresh session's one-time delivery.
The full branch still has a net production-code increase, measured separately
from tests and documentation. Further confirmed dead migration APIs are being
retired, while active diagnostics, archives and native replay remain supported.


Generic and unknown CLI registrations are no longer eligible for internal model
planning or execution. The delegate registry accepts Claude/Codex native aliases
only through the authenticated canonical host bridge; unsupported registrations
fail before command execution or workspace processing. Arbitrary stdout and exit
status cannot manufacture a model response, terminal reason, or child history.
The external-text adapter and its private command watchdog are retired. Existing
classic C HTTP model delegation and explicit nonmodel domain/tool command
protocols remain separate boundaries. Regression fixtures exercise the actual
registry, including a write-role/tools-enabled/unbounded command that must never
create its marker, and canonical-host capacity, deadline, cancellation and
turn-cap handling.

The generic retirement passes the full server-Go normal suite and all three Go
race suites. The bypass gate and 24 mutation tests reject both retired symbols
and newly named direct command/synthetic-text model paths. Standalone packaging
also removes the generated watchdog entry point, declares the delegates' actual
egress dependency, removes the deleted text adapter and includes its new tests.
The Aimee descriptor now includes its existing job-observation SQL migration and
live regression. All 23 exporter tests pass with zero skips using the installed
Go/CMake toolchains, including actual standalone builds. Earlier packaging
failures remain retained.

Standard Compose native execution does not configure the optional cgroup
boundary; no whole-tree native crash-recovery proof is claimed for that profile.
The held live permission case therefore uses a real HTTP operator-interruption
receipt, then switches the same owned session to native execution for one
relative-path write and one browser Allow. It must prove absolute resource
binding, one dispatch and one effect. This tests native permission delivery,
not native kernel custody. Actual live execution remains pending final image and
integration qualification; no synthetic receipt or scope repair is authorized.


The active goal is **full internal canonical IR conversion**, including model
requests, results, streams, histories, native execution, delegates, workflows,
curation and learning synthesis. Browser acceptance alone does not complete it.
The earlier recall probe reproduces a refusal in the stock Claude SDK as well;
that diagnostic is separate from the remaining internal conversion work.

The browser, delegate/workflow and curator/learning cutovers are integrated on
this branch. Parallel agents are qualifying the final runtime. Active browser
history comes only from the durable canonical session log; flat records are inert
archives. Provider results own canonical IR and transport status. Refused, partial,
reasoning-only and unfinished tool turns cannot acquire business success or tool
authority. Asynchronous terminal classification and child status/logs use the
owner-checked canonical observation, including the actual current child turn;
legacy stored results cannot substitute for that observation.

Go delegates, workflows and panels exchange complete owned canonical requests and
responses. Native calls cross the authenticated local bus into the C factory;
direct SDK execution and stdout interpretation are retired. Immutable execution
scopes bind source, actor, credentials, role, model and workspace custody.
Registered triggers mint an honest server source only for an admitted candidate,
after deduplication. General completion preserves media and opaque companions;
application text projection separately requires a completed text answer.

Opaque shell model commands are retired as internal model-result transports.
Curator and synthesis consumers use their configured canonical provider; no
endpoint or KB instance is selected automatically. Deterministic parsing, solving,
embedding and external cognifier domain protocols remain explicit boundaries.
KB-free canonical task observation, configured-test grading and scoped reuse do
not require a remote KB or an invented grade.

Fresh acceptance found an additional real setup defect: an explicitly supplied
PAM login was provisioned without recording the initial appliance administrator.
Setup reported complete with an empty username while workflow saving returned
403. Startup now records the usable named supplied account through the existing
non-secret `explicit:<username>` identity contract, preserving existing generated,
replacement and first-seed ownership. Actual Go/C policy readers and shell
negative regressions pass. Neither failed workflow preflight submitted a task or
called a model. Their evidence and stopped fresh-install volumes are retained;
a new empty installation of the final image must qualify the repair.

The shipped memory evaluator also called an unregistered `agent generate`
command, hidden by a fake executable fixture, and consumed only a flat response.
It now uses the registered `model generate` CLI/RPC with bounded canonical JSON
stdin. Its tool-free owner retains caller instructions and retrieved evidence as
unverified context, preserves explicit controls and rejects unsupported controls.
It uses the configured HTTP completion path without hints, hooks, tools, role
cache or feedback. The evaluator admits full canonical responses and derives
answer/judge text and usage only after complete-text admission. Observed failed
IR remains diagnostic; host errors, missing IR and poisoned display fields cannot
become answers. Normal/race real-PostgreSQL evaluator qualification passes all
26 tests without skips. The actual CLI-to-daemon fixture is part of ordinary
configured integration, alongside success/refusal/MAX/tool negative cases.

Source `876a35522` passes 690 native invocations and 690 ASan/UBSan/leak
invocations, all 76 lint gates, all three Go race suites, 483 frontend tests,
type checking and both frontend builds. Configured local integration passes
170/170 with eight actual SDK profiles; unavailable Docker checks were explicitly
skipped there. Independent guest qualification then passes 171/171 with all eight
SDK profiles, actual Docker grading, KB-free learning, recovery and scoped reuse;
its earlier workflow-dependency skip and PostgreSQL readiness failure remain
retained, not counted as full acceptance. The source-derived Docker image matches
all 87 products and 19 server modules. The sole extra product relative to the
older 86-product baseline is the declared private delegates-host grant.

The normal `876a35522` upgrade preserves all 86 existing canonical events,
native records, full cache/SDK bytes, UUID, device identity, workspace/inodes,
Vault ciphertext, models and configuration. Public TLS certificate reissue retains
the same SPKI; the old public PEM bytes were not exported, so no byte-equality or
old SAN claim is made. A later external change to the operator's local Claude
credential file is recorded separately; the task did not restore it or claim
unchanged operator-source bytes. Original application Vault bytes remain exact.

The administrator and evaluator repairs are committed as `a9cdd64b1`. Its image
matches all 87 source-derived products and 19 modules. The normal upgrade keeps
all prior 86 events and full cache/native/authority state exact. Subsequent C
qualification passes 691 registered invocations normally and under
ASan/UBSan/leak checking, with the same 33 conditional prerequisite notices
retained. The isolated fixture version is pinned to its original build metadata;
its application C/Go bytes match `a9cdd64b1`. The only approved test delta adds an
aborting learning-transport seam. Seven updated production C units pass the strict
Cppcheck ratchet and explicit Clang analysis; all 76 lint gates and three Go race
suites pass. The evaluator's 26 real-PostgreSQL tests pass normally and under race,
without skips.

Actual fresh `a9cdd64b1` setup proves the administrator repair, but the first
workflow pauses before model execution: the long-lived Go executor selected
`agents.json` at startup before setup created `models.json`, then never reconsidered
that path. It is stopped normally with no SDK, native cache or canonical model
output. The repair resolves the canonical registry on every load, permits legacy
fallback only when the canonical entry is absent, and binds cached parsing to the
selected path, actual file identity and bytes. First-boot-after-configuration and
stale-cache regressions reproduce the old failure; normal and race tests pass.
The full three-module Go race suites pass after this repair.

The old-install ordinary browser A1 also stops before SDK launch, for a distinct
reason: its preserved credential expired at 13:36 UTC; the normal refresh endpoint
returns HTTP 429 at 15:03 UTC. The actual constructor stage is
`claude_source_lineage`. This is auth-refresh status, not model HTTP status or a
tool-catalog defect. Its three legitimate failed events are retained, so the next
upgrade baseline is 89 events. The old SDK/cache bytes remain exact. The outer
launch diagnostic now correctly names runtime preparation. The old turn is not
retried or repaired by rewriting its credentials. The separate fresh enrollment
is valid until 21:31 UTC and remains the fresh-install acceptance source.

The first `a9cdd64b1` configured integration ends 168/171: the new hand-written
Python fixture omitted required canonical context revision fields and `stream`,
and the new fixture UID could not open a prior UID's global verifier lock. The
repaired Python body passes the actual production C codec unchanged; the next
fixture uses its own nonce-scoped verifier lock. No decoder, ownership rule or
test was weakened. All eight SDK profiles and the refusal oracle passed that
failed run; its receipt is retained as failed, not relabeled green.

The registry/diagnostic repair is committed as `69d9ca7fd`. Its image matches
all 87 products and 19 modules, and normal upgrade preserves all 89 canonical
events and the full cache, native and authority state. Fresh setup again passes;
model configuration created after startup is now discovered without a restart.
However, the first real fresh native workflow fails before SDK launch because
its stored execution scope is empty. It was stopped through the normal owner
route before retry; its failed attempt and empty SDK/native observations remain
retained. The cause is the web gateway's direct Go socket shortcut, which skipped
the C source issuer. Repair routes workflow calls through the C facade and private
bus, preserving existing bare-username workflow ownership while retaining the
canonical actor in the sealed source. The obsolete direct Go HTTP listener is
also removed; the workflow mux serves only its authenticated bus stage. Shutdown
drains that stage before detaching the outbound store caller. Focused Go tests
pass; the combined image and configured ingress/storage regressions remain due.

Honest server-owned source scopes retain `ATTEST_NONE`; the native constructor
previously rejected that transport. The repair composes the actual admitted
private source with factory preparation through a one-shot, thread-local ticket.
It binds the canonical seed, selected model and credentials, owner, actual owned
child session and final native profile. Finite limits can only tighten. Failed
or consumed preparation cannot issue another ticket without fresh source
admission. Raw server strings, bearer transport, unadmitted/foreign scopes and
changed requests remain refused. The real source/HMAC/admitter and constructor
composition passes normal and sanitizer regressions; its positive fixture ends
at an explicit database dependency seam, not a provider call. Fresh native and
service-trigger execution are still required.

Removing the raw control listener also exposed missing cancellation propagation
inside the bus-to-HTTP-mux adapter. Control requests now derive their context from
the process lifetime and bounded bus deadline, preserving the private execution
scope. Active bus cancellation reaches the request and its subprocesses; canceled
handlers return transport failure, never an empty HTTP success. Blocked-handler
regressions reproduce the old failure and pass normally and under Go race.
Supported CMake qualification builds the thin client and the two affected
native runtime/factory fixtures. Their actual Debug/LTO links exposed missing
math-library linkage and section flags at final LTO compilation; both target-local
test-build repairs pass CTest 2/2. CMake intentionally does not build the server;
production server registration and image qualification use Make. No dormant
server source table was revived or claimed as a supported server build.

Standalone deployment validators now use this same C/bus ingress and the actual
generated config/PostgreSQL/DB1 grants and processes. Their old placeholder paths,
raw identity headers, empty-column comparisons and undefined table counts are
replaced by actual registered workspaces and a read-only, schema-bound PostgreSQL
metadata oracle. Fixture syntax and negative schema-oracle cases pass; real
standalone deployment execution remains pending the combined products.

The `69d9ca7fd` configured guest integration ends 170/171. All eight SDK profiles,
typed refusal, real Docker grading, signed proof, KB-free learning, scoped reuse
and daemon-restart recovery pass. The new actual CLI fixture receives the correct
canonical success response, then wrongly expects a `status` field that the CLI
intentionally strips on success. This also exposes the same assumption in the
Go evaluator. The evaluator repair requires a successful process exit, admits the
actual unwrapped canonical response, and strictly checks any explicit status;
failed/partial/refused IR remains diagnostic. Its focused normal/race regressions
pass. The failed integration receipt is retained; a new configured run must
exercise the repaired actual CLI-to-evaluator contract.

The next qualification source also repairs two follow-up control defects.
Native primary chat now enforces the tighter positive bound from the registered
model's `max_turns` and any request-local native limit; an unset TLS override no
longer discards a configured finite cap. Actual adapter configuration, factory
forwarding and runtime-budget regressions pass normally and under sanitizers.
The onboarding metadata writer explicitly restores its dedicated directory to
0755, so a restrictive process umask or earlier 0700 directory cannot hide the
readable administrator marker from the C server. The real marker regression
passes normally and under race with umask 077. Credential file modes are unchanged.

The first `09567b99b` local native run passes all 692 invocations, but its wrapper
correctly fails source equality after the parallel lint generator changes only
stale generated documentation. Lint passes 75/76; docs generation now removes
the retired socket variable (303 runtime environment variables), and its check
passes after regeneration. The race run separately exposes the metadata-directory
permission defect above and an AF_UNIX temporary-path limit in the harness. New
qualification uses independent source worktrees and exclusive ten-character
temporary roots. These failed wrapper/race/lint receipts remain retained; they
are not relabeled final acceptance.

Runtime source `936438a42` now passes 693 native fixture invocations normally
and 693 with ASan/UBSan/leak checking (690 distinct binaries), plus all 76 lint
gates. The sanitizer parent was safely paused before any fixture invocation and
resumed after the normal suite, avoiding confirmed shared test paths; elapsed
sanitizer time includes that recorded pause. All three Go race suites pass. The
server-Go success includes a test-only repair to two observability negative
fixtures: explicit 0644 chmod/stat prevents umask 077 from accidentally creating
safe 0600 files. Production file-permission guards are unchanged. The original
936 two-fixture failure remains retained. This test-only follow-up does not
relabel or require rebuilding the qualified production image.

The canonical 936 image matches all 87 products and 19 server modules. Its
normal upgrade preserves all 89 existing canonical events, native records,
complete SDK/cache bytes, UUID, workspace/device/inodes, Vault, models and
configuration. Public TLS leaf reissue keeps the same private-key SPKI, with both
actual public certificates retained. Fresh empty installation and normal setup
pass. Actual installed CLI implicit-cwd investigation passes in two distinct
normally registered repositories, without explicit project/scope fields or
model calls: Hello-World returns its correct empty project result and Spoon-Knife
returns three results only from its own project. The original no-active-project
failure is absent. Local index availability is measured, not inferred from KB
mode; no KB instance is configured.

One actual fresh browser workflow now crosses authenticated C scope minting,
Go storage/scheduling, the native host, C factory and real Claude SDK. Its private
scope is persisted as 64 valid characters; public ownership keeps the bare PAM
name. One SDK process completes with canonical TEXT and THINKING, child 12 events
and parent two matching child-execution events, zero tools, then the real human
gate (`active` plus `pause_reason=human_gate`). One normal owner stop succeeds.
The original browser harness expected a different parked-state label; its
failure remains retained. A later finisher stopped the task successfully but
failed a background-presence assertion; that blocked attach was a single fresh
UI presence registration after login, not a workflow/model retry. A separate
15-second source-bound forensic observation confirms the distinct UI SID and no
repeat. Neither automation failure is relabeled green. Independent final seals
prove the actual native behavior, stopped state, full canonical history/cache and
quiescence.

Current configured 936 integration passes 173/173, all eight SDK profiles,
typed refusal, actual Docker grading, signed proof, KB-free learning, scoped
reuse and restart recovery. Its mandatory Python primary case runs both the real
C CLI and the real Go evaluator through that CLI, requiring four additional
observed local-provider requests. The outer receipt initially searched for
markers that the shell filtered from successful output; that false receipt is
retained. Corrected evidence is explicitly source-flow inference from the exact
unconditional assertions and later retained success marker, not a captured raw
request transcript. The test-only shell follow-up forwards evaluation markers in
future logs. Production/runtime provenance remains 936.

Actual ordinary native-to-HTTP-to-native switching now passes all three turns
in one fresh SID. The HTTP turn leaves native cache bytes untouched; native return
resumes the original SDK UUID with the exact first prefix, identity, cwd and inode,
then appends valid history. Canonical terminal/checkpoint and business-fact gates
pass; prior human-workflow history/cache stays exact. Final automation fails only
a stale mutation allowlist: three session-metadata POSTs were ordered before the
first send. Their original bodies were not retained; current source sends only
metadata, and the server rejects flat transcript writes. This is source-flow
classification, not a fabricated body observation. All three actual canonical
USER inputs match their frozen prompt bytes. Failed automation and independent
behavior proofs remain separate.

Registered service-trigger configuration is blocked before any candidate/model:
the pinned external config owner turns nested JSON numbers into YAML strings,
rejecting numeric max_spend_usd. The owner repair is now committed on the scoped
dependency branch at `01e83ef1aee91c7b5848ecb58ead5e930fde99b8`, with the exact
previous pin as its parent, retaining temporal-learning defaults. Its normal and
race tests and actual application consumer tests pass; the old handler reproduces
the numeric failures. The application now pins the immutable pseudo-version and
verified module checksum. No budget omission, string workaround, module-cache
patch, extra PR or default-branch mutation is used. Source/authority/cache seals
confirm no service model ran. A new qualified image and actual service-trigger
execution remain required.

The complete server-Go race suite passes with this new config pin. Its first
attempt failed while compiling in the local tmpfs quota; the retained retry uses
an exclusive short filesystem-backed temporary root and passes. Public Go proxy
resolution independently returns the exact immutable owner revision; neither
result is a hosted-CI claim.

Standalone real-PG workflow control passes 32/32 and lifecycle passes 23/23.
Coexistence exposed a test-oracle connection defect: libpq treats PGDATABASE's
value as a literal database name, rather than expanding a whole URI. The test-only
repair supplies URI components through the private child environment, verifies
matching runtime/migration endpoints, retains schema/read-only checks, and rejects
unknown options. Seven pure cases and an actual read of 1,010 columns/121 tables
pass. The fresh committed `1ad2ec549` harness then passes coexistence 15/15 with
zero notes against the same qualified 936 runtime products. Earlier fixture
startup/path errors and oracle failure remain retained. All nonce-scoped fixture
databases, roles, container, volume and cgroups are cleaned; application resources
are preserved.

The next actual native question exposes a separate browser failure. One model
request emits the real canonical question at sequence 6, but the history
EventSource fails and the UI hides its controls. No human reply is submitted;
the request cancels and completes with 13 canonical events. Those records, the
prior 44 events and full SDK/cache/authority bytes remain preserved. This is a
failed question acceptance case. Current public and owner SSE reads return all
13 events and valid replay completion; the original failing HTTP status was not
retained, so a backend cause is not established. A reproducible frontend gap
allows new model sends after a history failure despite hidden controls. Its
repair now preserves the draft and blocks dispatch until canonical replay is
validated, including initial history and explicit reconnect. Synchronous owner
and readiness references fence queued sends, attachment failures and Steer
continuations. A changed watch generation cannot revive old sending intent.
Actual old-production regression also reproduces a separate custody race: a
session switch before passive effects sends old SID `web-one` with new cwd
`/work/two`. Dispatch now captures and rechecks the session/SID/workspace tuple.
Confirmed undispatched text is retained for its own conversation, with bounded
preview and explicit restore; uncertain dispatched requests are not retried.

Primary and delegate reply callbacks synchronously check their actual canonical
history owner, readiness and still-pending control before the shared submission
latch or POST. Nested delegates retain their own log checks; stale buttons cannot
bypass a disconnected or closed history. Four critical regressions fail against
the old actual production components. The final frontend bytes pass 493 tests in
51 files (81 focused), TypeScript and both production builds. A separate actual
390-second unsent browser check receives replay completion and regular keepalives
without UI errors or model calls. This does not explain or relabel the original
failed question; fresh live reply acceptance remains required.

The remaining-consumer audit also finds and repairs a live shadow ingress bypass:
the old HTTP dispatcher copied raw completion bodies before canonical admission.
Runtime-armed shadow delivery now captures the real owner's admitted inbound IR
once, before local policy mutation, and dispatches only after local preparation
succeeds. The peer receives a faithful external rendering and applies its own
ingress policy; local policy is not duplicated. Unsupported options, local
checkpoint references and lossy opaque/cache projections drop the optional mirror
instead of flattening or forwarding ignored fields. Disabled publishing has no
clone/render cost. Scope cleanup covers every HTTP exit and keep-alive request;
internal model calls cannot inherit or consume another request's snapshot.
HTTP 2xx counts only successful delivery, never model or tool success. Atomic
allocation failures cannot omit a model limit and dispatch a changed request.
The old implementation reproduces the invalid-request delivery failure. Six
registered native fixtures pass normally and under ASan/UBSan/leak checking,
including actual OpenAI admission/preparation, three Anthropic paths and terminal
handling; the bypass gate and 22 mutation tests pass. Full combined-source
qualification remains required.

Honest server-triggered native execution, question/permission replies, native
tool effects and final browser acceptance remain outstanding.
Keep these checks separate
from the stock-SDK recall refusal. No current-branch GitHub CI or production
promotion is claimed. Keep one eventual PR, automatic reviews and strict
attribution; do not investigate or change `.100`.

### Earlier runtime qualification: 23 October 2026

Runtime candidate `252ee7707` is committed and pushed on the same branch. Its
canonical Docker and HOST builds and normal upgrades pass. Docker matches all
86 products and 19 server modules; HOST matches all 71 installed products and
19 modules. Normal HOST shutdown completes in 2.357 seconds with zero pending
audit or guardrail records, no dropped records and all threads exited. Normal
recovery and 30-second stability pass. Earlier failures remain retained.

The current candidate passes all 76 lint gates on a clean, unchanged tree.
Combined OAuth parser/refresh regressions pass normally and under ASan, UBSan
and leak checking. Oversized token responses now fail before they can replace
stored credentials. Refresh failures log static categories and bounded numeric
curl/HTTP codes without logging credentials, headers, endpoints or response bodies.

The broader runtime qualification is bound to `64cce353b`: configured integration
170/170, all eight actual SDK profiles, a real Claude 2.1.177 refusal fixture,
T3's seven topology gates and 692 assertions, 678 native sanitizer invocations
covering 675 distinct binaries, and static analysis of 988 production C units.
Integration retains 25 explicitly unconfigured KB/runtime-web checks; real Docker
grading, learning, recovery and scoped reuse ran. Scoped fixture databases, roles,
credentials, containers, volumes and networks are cleaned up. All three Go module
race suites, 478 frontend tests, type checking and both frontend builds pass on
unchanged inputs. Rust/C and Go/C interoperability checks also pass on unchanged
inputs. These are local results, not fresh GitHub CI or a newly published image.

A genuine ordinary release-planning conversation now passes all four live phases
on `252ee7707`: native Claude A1, resumed A2, HTTP Codex B, then native Claude A
return. A2 recalls the earlier release version; B recalls that version and target
platform and adds the release channel; returning A correctly uses all three facts
without their values being repeated in the final prompt. The return completes at
event 40 with native checkpoint 41. Exactly four user Sends occurred, with zero
tools, replies or retries in this workflow.

The return uses the original SDK UUID, identity, cwd and inodes. The full original
5,972-byte SDK prefix and the full pre-B 12,902-byte prefix remain exact; B launches
no native SDK, and the final SDK file is 18,767 bytes. The final response reports
17,355 cache-read tokens. All 147 prior canonical events and their full caches,
original connection entries and authority remain exact. No SDK transcript was
imported into model history. SDK and native execution processes are zero afterward.

The first release turn completed successfully at event 8/checkpoint 9, but the test
helper rejected a legitimate signed-thinking `replay_proof` with its own proof
turn. The original harness failure remains retained. Production source and the
frontend reducer confirm this is cursor metadata, not another execution. The
narrow helper correction checks the exact proof schema, actor, session and proof
ID; negative fixtures still reject foreign actions, controls, messages, deltas,
executions and projections. Read-only validation recovered that same completed A1;
only the three remaining turns were sent. A1 was not repeated or replaced.

Current-build governed tool execution also passes: one real `write_file`, one
successful result, exactly one effect-confirmed dispatch, exact file bytes and
ownership, completed event 10 and checkpoint 11. Normal policy authorized that
write without an approval, so this test does not qualify the Allow UI. The earlier
genuine HOST uncertain-effect recovery remains the permission proof: one
permission, one human Allow, one governed dispatch and independently verified
filesystem effect. Its permission code inputs are unchanged.

Current-build question/reply validation passes with one genuine `request_input`,
a pending-question reload, one mobile-width UI answer, one control response, one
successful tool result and an actual model acknowledgment. It completes at event
12/checkpoint 13. After reload the answered control remains closed. The browser
exits, SDK/native processes are zero, and all 134 preceding events and caches stay
exact. No synthetic question or approval was inserted.

A separate high-entropy label/marker recall probe still fails on the native return
at event 33. Its canonical stop now correctly retains `content_filter`, raw
`refusal`, category `reasoning_extraction` and the provider's explanation. A1, A2
and B in that probe passed with fresh valid credentials. Numeric HTTP status is
unknown: its bounded WORM scan contained no admitted provider receipt. Local SSE
200 and dispatch success must not be represented as upstream HTTP success.

Offline reconstruction with the actual production IR and Anthropic renderer
finds seven alternating text messages, an exact native A2 prefix, and no foreign
reasoning, encrypted blocks or provider extensions in that failed probe's history.
Its final user prompt matches canonical history. Retained screenshots show Standard
and engineer; current configuration has no webchat prompt override or explicit
private-reasoning request. The full historic model-facing request was not retained,
so the classifier's cause remains unproved. Do not call that refusal repaired,
retry its failed turn, reset its cache or introduce an automatic fallback.

The older 54-event and 33-event native-return failures also remain immutable.
Actual stdout and numeric HTTP status were not retained. A further three-event
attempt failed before SDK creation during Claude credential-lineage preparation.
Its retained cloned credential was expired and the local operator credential had
rotated; the old refresh failure cause was not retained. That failure is separate
from the valid-auth provider refusal. A new connection was enrolled through supported routes without
changing original entries; no failed session's auth or cache was rewritten.

Refusal details remain bounded canonical diagnostics through responses, deltas,
snapshots, durable history, caches and browser replay. Provider fallback tokens
and model recommendations are excluded. Refusals cannot trigger synthetic tool
continuations; unsupported wire projections fail explicitly. SDK assistant error
envelopes remain transport diagnostics, and session binding precedes decoding.
Cross-cutting review found no additional ingress bypass or IR consumer defect in
scope. Interactive PTY/auth compatibility is separate from native model execution.

The editor resolves hydrated basenames through the owner's registry to the actual
session worktree. Desktop and 320px README rendering pass. The code-server proxy
preserves encoded paths, queries and credential stripping while PWA and webview
workers register within `/vscode/`; real HTTP, WebSocket and Go race checks pass.
The observation-bus flush race is fixed by publishing processed work after queue
handoff; deterministic old-code failure and normal, sanitizer and ThreadSanitizer
regressions establish the repair. SQLite FULL synchronization remains intact.

Corrected strict replay checks already pass at 1280/390/320 with 15 seconds of
zero idle lifecycle, reconnect or mutation requests at each size. They use
`ignoreHTTPSErrors:false` and only the owned UI certificate SPKI. An earlier audit
ignored certificate errors; retain it as HTTP semantics evidence only. The final
41-event release conversation also passes that audit: the latest release checklist
renders at all three sizes, controls stay closed, Send stays enabled, and no page
errors or horizontal overflow occur. Independent after-state preservation of all
188 events, native metadata, full SDK caches and authority passes.

Evidence exports are local and hash-verified. The exact owned test VM, its disks,
scratch directory, cloud-init snippet and lease records are removed, and its
controller has exited. Original user credentials remain unchanged. Remaining
delivery work is one PR, fresh GitHub CI and postmerge
published fresh-install acceptance. Git over SSH can push the branch; the local
personal `gh` client currently has no authenticated GitHub account. Do not turn
that delivery limitation into `.100` troubleshooting or a prerequisite for work. An open PR is
not required for implementation or validation. Keep automatic reviews, full-history
secret scanning and strict attribution enabled. The verified public OpenAPI
checksum's exact historical fingerprint is the only admitted secret-scan finding.
No production promotion or `.100` operation is authorized by this checkpoint.
The PR timeline below is historical; delivery remains one eventual PR and no
intermediate PRs or merges.

[PR #3052](https://github.com/RakuenSoftware/aimee/pull/3052) merged into `testing`
as `aa8c774edcc324f5defb6c5cea592f0e1dd5314d`. Its final premerge checks and
all 54 postmerge main CI jobs passed, including every configured deployment
tier; the seven-job artifact publisher also passed. Those results qualify the
merged baseline, not the subsequent browser repairs. PR #3057 subsequently merged
into `testing` as `861b5b0ff20d2c24f7fb89a5826198f175f7a685`. The SmoothGUI update
in [PR #3058](https://github.com/RakuenSoftware/aimee/pull/3058) subsequently merged
as `24ecd8b39ea7a6fef78fba838a3bfe1a188831ed`. The fresh-install Claude repairs
continue on `fix/fresh-claude-provider-setup`, requiring their own follow-up PR.
Keep automatic review and the strict attribution gate enabled. No production
promotion is claimed.

### Current published-runtime verification: 22 October 2026

PR #3060 merged as `ee9fa572e` after all 57 checks passed. The published
application digest is `sha256:538ef1bfa7ace46d9dfed5a0580897f645e87d88fc5678fe7893ccce22cc7cfb`;
its registry provenance binds that exact source. PR #3061 subsequently merged as
`251536ddb`, repairing Clang CI package downloads after the Azure Ubuntu mirror
stalled. Its postmerge CI, Audit and Attribution workflows passed. That workflow
change did not publish a new runtime image.

Fresh published-image and canonical HOST launches both encountered Claude's
strict profile refusal: 53 expected tools, zero reported. Neither first failed
turn was retried. A later isolated diagnostic reached the guarded model path
with all upstream HTTPS blocked; this proves profile acceptance in that run,
not a successful provider request or a fix for the original failure. The exact
SDK init frame was not captured in that diagnostic. Startup timing remains under investigation; do not weaken the catalog or
ownership gates or declare this acceptance complete. The retained imported
credential was outside the refresh window, and the isolated trace showed no
multi-second credential-lock wait.

The native control channel now polls the SDK's supported `mcp_status` before
sending its first user message. Only a matching successful response with the
expected connected server releases that message; pending status stays within the
original 60-second initialization deadline. Readiness responses remain private
transport metadata. The later actual system/init frame still must pass the full
catalog, cwd, authentication, permission, plugin and host-attestation checks.
Zero-tool launches retain their existing path. A deterministic regression proves
the old code sent user input while discovery was pending. An isolated, exact
Claude 2.1.177 probe with seven-second MCP delay verifies pending-to-connected
polling before user input and the full 53-tool init afterward. Its immediate-user
control also passed that run, so this does not prove the original failure's cause.
Twenty focused readiness cases pass normally and under ASan/UBSan/leak checks,
including the real 60-second deadline, cancellation, stale/duplicate replies,
malformed metadata, zero-tool launches and continued full-profile refusal after
readiness. Two independent reviews found no guard or lifetime regression.

The next repair batch preserves the editor's owner-resolved session directory
through its dedicated web route, seeds Claude trust for its actual held SDK cwd,
and adds allowlisted MCP status/stage timing diagnostics. The editor regression
and full Go race suite pass. Combined headless, native-runtime and native-factory
fixtures pass normally and under ASan/UBSan/leak checks. These changes are not
published yet. Genuine permission Allow, cross-vendor A/B/A, final published
restart/UI/deployment acceptance and owned-resource cleanup remain outstanding.
Production `.100` remains untouched and KB-free.

### Current PR continuation and audit correction: 22 October 2026

Continue on [PR #3060](https://github.com/RakuenSoftware/aimee/pull/3060), branch
`fix/fresh-claude-provider-setup`. Its first full CI run exposed two independent
causes: native config fixtures still expected Claude's retired `none` default,
and provider normalization converted explicit native Claude/OpenAI host profiles
into an alias and cleared their endpoint on reload. The fixture now checks both
Claude OAuth aliases and preservation of unrelated explicit `none` profiles;
normal and ASan/UBSan/leak runs pass. The normalizer preserves explicit upstream
wire, endpoint and authentication. Ten real store/edit/reload cases reproduce
its baseline failure, and the complete provider race suite passes after repair.
These changes were pushed at `430f5d15c`. The later `f2edf66f8` actual-SDK
integration passes 170/170, including all 16 browser/seeded wire/auth cases and
real configured grading/recovery/scoped reuse. All formerly failing native and
PostgreSQL shards also pass. Final current-source CI remains required.

The remaining-consumer audit corrects earlier broad unfinished statements:
primary durable history, compaction/seeded runs, delegate execution/history and
owner-bound local grading/reuse are already implemented. `check_ir_bypass.py`
passes. The actual configured-test grade/recovery/scoped-reuse integration and
independent signed 16-case semantic oracle are recorded in the later checkpoints
below; their production owners are unchanged. Do not rebuild a learning store,
seed grades, import private SDK transcripts or treat pending observations as
applied knowledge. Linux descriptor-based native server execution is the current
supported contract. macOS/Windows CI exercises thin-client server access; old
historical testing-branch skip notes are superseded by the current workflow.

Normal setup dismissal now has an authenticated, non-secret installation/account
scope. Remembering Later/Close does not change account readiness; a reinstall
with the same bootstrap username receives a new scope. Concurrent first reads
and process restarts retain one stable scope; runtime-web race tests pass.
Frontend work remembers normal dismissal, compacts the mobile rules banner and
groups compatibility notices while retaining every canonical observation. Its
465 tests, both production builds and built-browser checks at 320/390/1280 pass.

Trusted SDK `invalid_request` failures now display a static provider-rejection
message instead of an undifferentiated structured-turn failure. Arbitrary SDK
text remains excluded, the terminal remains failed, and cancellation/capture
failure take precedence. Normal and sanitizer regressions pass. This reporting
change does not assert that the upstream policy rejection has been repaired.
Further offline inspection identified fresh-HOME device identity regeneration.
The host now retains only the nonsecret correlation ID in its checked cache lease,
seeds the SDK's actual config path in each fresh HOME, and binds the version and
actual identity into the launch digest. Four focused fixtures pass normally and
under ASan/UBSan/leak checks. An exact installed-SDK control proves the seeded
device hash remains stable across fresh HOMEs; it does not prove causality for
the earlier provider rejection. Existing cache bytes and canonical history are
preserved; pre-identity handles are not adopted. Live/published-image acceptance
and final current-source CI remain required; production `.100` is untouched.

### Fresh-install acceptance continuation: 22 October 2026

The first published-image failure remains retained separately. Subsequent full
canonical candidate builds exposed OAuth-lineage and MCP-startup defects. The
default installed Claude 2.1.177 emits its initial profile before asynchronous
MCP discovery unless the host explicitly waits. The repaired launch fixes
MCP_CONNECTION_NONBLOCKING=0 and attests it; no discovery timeout or catalog gate
was relaxed. Exact SDK probes with delayed MCP responses reproduced the failure
and verified all 53 tools after blocking startup.

Full candidate source 1c8b55ff73f6915248c841da433e0cf99b05303e passed a genuine
fresh normal PAM/Providers/Models/Vault/project browser journey: one README read,
one pending question surviving reload, one human reply, and one ordinary
`aimee index investigate "README Hello World"` execution. The CLI selected the
approved project without scope overrides; its honest empty symbol hits are not
a retrieval-quality pass. The initial turn completed, and switching the same
conversation from Opus to a normally enrolled Sonnet model completed with correct
prior-answer recall. No live permission waiter occurred in this journey.

Returning to Opus failed with `Claude structured turn failed`. It was not retried.
The original SDK JSONL remained unchanged. A credential-free, network-isolated
probe of copied retained cache bytes reached a local model endpoint under the
original scheduler cwd, but exited before any model request under a different
cwd. The production SDK's raw error was not retained; the probe's exact error
must not be attributed to that production process. Claude locates saved projects
by cwd, while the host had removed and regenerated that temporary cwd each turn.

The repair retains a private scheduler directory within the admitted cache lease
and epoch, with path/inode revalidation. Credential HOME and temporary files stay
per-launch; tool authority remains the operator's approved project. Configured
containment binds only its existing HOME and SDK-state write interiors and leaves
the stable cwd read-only. A versioned launch contract avoids adopting old handles
keyed by deleted temporary directories; their files and canonical history remain
intact. Normal lease/factory/headless/runtime and ASan/UBSan/leak checks pass, as
do all 76 lint gates. The actual configured-host containment fixture also passes on the owned VM
under UID1000 with a delegated cgroup; its unit/cgroup cleanup is verified. The
default Compose application remains ordinary direct launch. Fresh full-image
acceptance is pending at this checkpoint.

A separate browser regression preserves existing conversation titles when only
canonical IR history is populated. Its baseline fails; 449 frontend tests and
both production builds pass. SmoothGUI 0.12.3 was already merged separately in
PR #3058; these later backend/title repairs are on
`fix/fresh-claude-provider-setup`. Fresh CI and a follow-up PR remain required.
Production `.100` has not been changed.

The full canonical `6a790b7f0265` image then passed a new fresh-install first
read/question/reply/CLI turn (completed at 197, checkpoint 198) and Sonnet switch
(completed at 224, checkpoint 225). Returning to Opus used the exact original SDK
UUID, stable cwd pathname and inode, and a new credential HOME. It nevertheless
failed at 231: authenticated model observations include an empty assistant and
`raw_stop_reason=refusal`, with zero output tokens. The retained SDK diagnostic
identifies `invalid_request` and an explicit upstream Usage Policy rejection.
This is a genuine failed acceptance case, preserved without retry; it is distinct
from the repaired missing-directory failure. All earlier canonical prefixes
remain equal. No prompt, model or cache was changed to evade the rejection.

An isolated copied-cache probe using the exact installed SDK and actual host
initialization protocol reaches a local model endpoint after control response
and valid system/init. It preserves the source cache prefix and has no
no-conversation error. It is deliberately terminated after reaching that local
endpoint; it does not qualify successful model inference or reproduce the actual
provider refusal. No launch or canonical-admission gate was relaxed.

Late onboarding overlays and incorrect inferred tab titles caused separate
zero-send harness preparation failures. Those were preserved separately and
corrected using normal visible dialog dismissal and observed tab metadata. The
first 320px completion image is obstructed and is not an unobstructed visual pass.
Restart/persistence, full-image T3 and read-only three-viewport audit continue
against the complete 231-event baseline, including the failed terminal. No live
permission waiter or cross-vendor OAuth switching is claimed by this journey.

The idle Docker restart of all three owned application containers passes. All
231 canonical events, native handles/owner/reservation rows, all four SDK cache
files (inode, bytes and hash), and configuration hashes remain exactly equal.
There are no active native jobs or SDK processes after restart. This qualifies
idle persistence, not successful continuation after the rejected provider turn.

The final candidate's unchanged canonical T3 runner passes all seven topology
gates and 692 family subchecks: 217 local memory, 15 semantic memory, 21 concurrent
owner/exploratory memory, 352 provider boundary, and 87 native asynchronous
execution/refusal/recovery. There are zero failed or skipped checks. The 18
allowlisted injected-failure diagnostic events are reported separately, not
counted as passed assertions. Actual image remains
`sha256:3fb20800584e7533343c9cbda0d38a08502309543a5be373985b79c3c814e238`;
this is a locally built full canonical candidate, not a published registry image.
Its isolated test topology has zero remaining containers, volumes and networks.

The final read-only normal PAM browser audit passes at 1280, 390 and 320px. Each
explicit reload preserves all 231 envelopes and exactly one question response;
resolved controls never become actionable. Each settled 15-second idle interval
has zero attach/detach, primary-history refresh, SSE reconnect or mutation. The
whole audit makes zero send/reply/cancel/configuration requests and has zero page
errors. Root reviewed all three unobstructed screenshots: controls fit, though
the rules banner remains large and two reasoning-omission notices are redundant.
Pending first-boot account setup deliberately reopens onboarding after each
explicit document reload; normal Later dismisses it before idle measurement.
This behavior is a UX finding, not an observed idle replay loop.

Qualification artifacts are retained privately outside the repository. Main
custody archive SHA256 is
`db1e57e7a8aea9e78d5ded52667513ae6dbfdc31aedc486b3cab05a260a631a7`;
T3 archive is
`5f1a73d8135952e0526188e858448b14d973d571ff949604d8c95726a3e006be`;
read-only report is
`8f3070bac36c8d9bd5e6d257a9749df1461bacaba43a52a0178673123c820c48`.
The tested code revision is `6a790b7f0265a1bbdd9cf144c7d8769130c62e5e`;
this later handoff-only commit does not identify a different tested runtime.
Fresh CI/follow-up PR housekeeping remains deferred at the user's request. No
production promotion, cross-vendor OAuth journey, live permission waiter, broader
platform qualification or overall migration completion is asserted.

### Published-image fresh-install failure and candidate repair

The merged revision passed all 54 post-merge main CI jobs. Publisher run
`39747388946` passed all seven jobs. Registry SLSA/BuildKit provenance identifies
that exact revision for application amd64 digest
`68b0a21ae5f3b7e495c0544f07414403bc22e70ed227b38ee0519d70277fa1df`.
Fresh owned VM 8165 on `.252` pulled that image normally and started the documented
managed, KB-free Compose installation with fresh Vault, PostgreSQL and PAM state.
It reports `vtesting-861b5b0`, all 19 module rows and ready owners, and serves SPA
`60756abf`. The default installed SDKs are Claude 2.1.177 and Codex 0.139.0.
The separate published-container T3 suite passed all seven topology gates,
including memory, persistence, provider/tool and native-async recovery checks.

The first genuine browser request through a normally added Claude connection
**failed** with `failed to build request URL`; no read, question or vendor
completion passed. Failure proof SHA256 is
`86d04363fe9eac423dafaf491e719cc37e2508cbf2f42911345411b20d31b633`.
Applying the saved connection had removed the model's native backend and kind,
causing HTTP dispatch with an empty endpoint. The same setup also generated an
inline `--model` command in a field that the attested launcher treats as one
executable. These are actual default-setup defects; the earlier configured-host
acceptance does not qualify this path.

Go normalization now derives native Claude dispatch after applying connection
authority, keeps model selection separate from the executable, and narrowly
repairs matching legacy generated commands. Explicit custom paths and unrelated
API connections retain their meaning. The C owner projects only Claude SDK
aliases into its host-owned Anthropic wire/default endpoint, preserving explicit
endpoints, cross-wire profiles and the original caller configuration.
All three Go regressions and the C alias regression fail on their original
sources. The provider race suite, normal C factory fixture, ASan/UBSan/leak checks
and all 76 lint gates pass. Full candidate-image fresh-install acceptance and
fresh CI remain required at this checkpoint; preserve the immutable-image
failure separately from any later candidate result.

The separately merged PR #3058 vendors SmoothGUI 0.12.3 from upstream main commit
`57d8308f38665d24221cb30b2aa2453926eed464`, with recorded tarball integrity.
All 447 frontend tests, both production builds and Chromium regressions pass.
The published `testing-861b5b0` image contains the preceding dependency, so its
results must not be attributed to this update. Production `.100` stays untouched.

### Current review and local Claude acceptance scope

The user clarified that production `.100` must remain untouched until a merge
to `main`. Its later promotion review is not a blocker for code review or the
current IR acceptance work. Automatic reviews and strict attribution remain
unchanged. No self-approval or production promotion is asserted.

### Current-source deployment and fixture-startup follow-up

Fresh owned `.252` deployment of `60f0f9f4c` built all six C products and five Go
services from clean output directories in 160.94 seconds: 1,341 compiler commands,
zero warnings. Actual server SHA256 is
`2cd4cd0fd24546500a4fb11a6cb133e6806ba518a25a639627d6a27c0560d7e2`.
The canonical Server manifest has 19 module rows. All 25 distribution aliases
are built; the other six belong to KB, which remains disabled. The unchanged
qualified SPA is `60756abf`.

The first fault setup withheld the entire module supervisor and therefore the
required configuration owner. Core correctly failed configuration readiness
before reaching the repaired path; that staging failure is retained, not counted
as startup acceptance. The corrected targeted delay retained configuration and
withheld only the database/families owner. Independent samples observed the same
core PID 53612, no boot-owner mutation and no dispatch before attachment. Restoring
the exact canonical manifest produced `coord-53612-0c27981fe3427d62`, exactly one
dispatcher-start event and `/v1/ready` 200 with database/modules/retrieval ready.
Jobs, canonical events and active native work stayed zero during this fault test;
no fake live rows were seeded to claim cleanup coverage.

A separate isolated standard integration run on those exact `60f0f9f4c` artifacts
passed 171/171 applicable checks again, including all eight pinned SDK/wire profiles,
interruption/denial/child controls and configured-grade to durable learning to
restart/scoped reuse. Its 25 configured-service skips remain explicit. Shipping
PIDs deliberately changed during the separately recorded cold-start test, so this
packet records that concurrent owned qualification rather than claiming unchanged
shipping PIDs. Genuine normal PAM/browser/model-catalog desktop/320 preflight also
passed with zero send/reply/cancel/configuration actions; the single real provider
journey remains held until the last source changes are installed.

Fresh CI then exposed a fixture interaction with the new production admission
fence: `workflowstoretest` writes `agent_jobs` directly for tests, bypassing normal
job creation, and returned after store-family readiness but before the daemon's
cleanup acknowledgement. A real PostgreSQL/daemon reproducer cancelled freshly
seeded job 901 and failed the terminal-batch test. The SQL query itself returns
correct ordered batches and is unchanged. Production now emits one fixed success
marker only when cleanup is acknowledged. The isolated fixture waits for that
actual daemon marker within its unchanged deadline before permitting SQL seeds.
Normal/sanitizer marker assertions refuse unavailable, in-flight and lost-ACK
states; the workflow test also strengthens first/third batch fairness checks.
Five actual PostgreSQL race-test repetitions pass with the corrected owned native
log path. An intermediate collector looked at captured stdout instead, waited
fail-closed and seeded no jobs; that failed packet is retained. The production
SQL query is unchanged. All 76 lint gates pass on the marker/harness repair.

The encrypted T2 CI leg separately failed before executing deployment checks:
Ubuntu's unrelated Microsoft feed returned HTTP403 during package refresh.
The cryptsetup preparation now excludes only that unused third-party feed,
retains signature verification and uses bounded package acquisition. Fresh CI,
final-source installation and the genuine browser journey remain required.

### Full-runtime startup and T2 readiness follow-up

The corrected actual-SDK fixture passed CI on `aa77037f1`; 52 of 53 main
jobs succeeded. T2 alone failed `Typed fixture indexing succeeds`, with
`operation=other`, status 5, class `internal`. Its artifact did not establish
an exact failing reembedding phase. The independent T1 and T2-luks legs passed.

Source review found that the deployment harness admitted indexing after only
application health, although the embedder starts independently after the
application. It now requires both the application and the pinned image's
loaded-model health check within the existing 240-second deadline, before any
index mutation. The maintenance requests, failure assertions and cutover remain
unchanged: no failed-index retry or waiver was added. Fixed diagnostic fields
and literal reembedding phase categories expose future failures without response
text, credentials, caller input or provider URLs. Ten Python readiness/privacy
regressions and focused Go diagnostic tests pass; fresh T2 CI remains required.
This barrier closes a demonstrated ordering gap without claiming that the
previous sparse artifact proves it was the sole cause of that failure.

The fresh owned `.252` VM 8164 built all six shipping C products (1,340 compiler
commands, no warnings/errors), fresh Go services and all 25 built module aliases. The canonical Server
supervisor contains 19 rows; six KB-side aliases are build-only, with no KB
service running. It has actual PostgreSQL 18, restricted roles, encrypted Vault custody,
normal PAM, pinned code-server, workflow service and browser, with no KB configured.
Source checkout advanced from `f7ae75c23` to test-only `aa77037f1`; runtime inputs
and the `60756abf` SPA remained unchanged. A separate isolated standard integration
suite passed 171/171 applicable checks, all eight actual pinned SDK/provider-wire
profiles, crash/permission/child-control journeys and configured-grade to durable
pending learning to restart/scoped reuse. Its 25 configured-service skips are
explicit; the fixture intentionally omits KB and runtime-web, while the separate
shipping GUI stack includes runtime-web. Result SHA256 is
`93e0544f33d44249bfa23a5d0c48f8ce62074d5affb3cd70949162542bad3e5c`.
These results retain those producer revisions and do not qualify later changes.

That full-runtime audit exposed a reachable startup defect: the coordination
dispatcher attempted its durable boot claim before the database module attached,
then returned permanently. A typed runtime-state lookup now distinguishes an
explicit missing row from transport/store/malformed-reply failure. The worker
retries boundedly, recovers prior claims, retains the same generated owner across
lost write acknowledgements and persists it before any sweep. It never treats a
failed read as permission to replace the owner. Normal, ASan/UBSan/leak and CMake
fixtures pass; the old source fails the no-write-after-failed-read regression.
Cold module-delay validation and final-source deployment remain required.

The same audit found that the old one-shot delegate-job cleanup could miss
attachment. Blindly retrying its bulk cancellation later would cancel new work.
The shared process-local admission barrier now holds a mutex across the typed
cleanup, latches only an acknowledged success, and gates the sole production
background-job creation before writing a new row. Server initialization and the
idle main tick use that same latch. No new job is admitted across a failed or
lost acknowledgement, and later ticks cannot cancel this generation. Existing
resume code has no reachable call site. Its production-helper regression covers
unavailable-to-attached storage and twelve concurrent callers with a committed
cleanup/lost acknowledgement. All three startup fixtures pass normal, ASan/UBSan/leak and CMake checks;
affected production objects compile with warnings treated as errors. These
results belong to the new source; the previous 171-check deployment does not
stand in for them.

The first combined lint pass caught test registrations after shard selection;
they were moved before the suite snapshot. The test-execution gate now accounts
for all three new fixtures. The memory ownership gate required fresh review of
the changed server startup file; its memory symbols, disposition and typed calls
are unchanged. Independent review completed, and only that reviewed hash was
updated. All 76 lint gates now pass after the targeted ownership-gate rerun.
Fresh CI and current-source live qualification remain required before declaring
this follow-up complete.

### Native integration beta-route assertion follow-up

Fresh final-head CI caught the actual-SDK synthetic provider fixture matching
`self.path.endswith("/messages")`, rejecting the now-correct retained
`?beta=true` marker and causing cascading provider retries. The fixture now
splits path/query and requires exactly `beta=true` for Claude-to-Anthropic,
including token counts, and an empty query for other host-selected wires.
This changes tests only; the qualified C/Go/frontend products remain unchanged.
Fresh CI must qualify the corrected fixture rather than waive the failed run.

### Latest local Claude acceptance: source `384683799`

The previously unqualified genuine local Claude path now passes. The clean,
unmodified four-product build has 1,297 compile commands with no warnings/errors;
only the owned server/CLI were installed, with KB/gateway build-only. Actual
running server SHA256 is
`eed11404a9d57624ffeedde7765566be6d6b70c21bd88086ff9995a55d20f935`.
The unchanged qualified SPA is `60756abfa7c545434f2771c565c6dc798738c9c44693700cee8606ce8797016e`.

Normal PAM and the actual Max-backed Opus5 profile executed README `read_file`,
then `request_input`. Reload preserved the pending question; one real operator
reply produced the matching tool result and completed at 80/checkpoint 81.
Independent actor/project/call/result and acknowledged read-intent checks agree.
The actual SDK child retained the exact fourteen-entry credential-free environment.
Desktop/320 replay sent no duplicate actions. Session is
`web-0079d51e-760c-43f1-802b-7c10a7978ed5`.

A separate genuine permission journey continued that same session. The retained
SDK session from checkpoint 81 produced the next actual question/source 105;
its JSONL message ID matches the canonical original. Literal resume argv was not
captured and is not claimed. A verified qualified leaf was frozen before killing
only the owned daemon. Normal browser recovery retired the physically quiescent
old execution, appended cancellation 109 and source-bound unknown outcome 110,
and changed the ownership epoch. A real `write_file` call at 139 requested
permission 142; one browser Decline produced DENY 143 and matching error result
144. The target file is absent, completed 226/checkpoint 227 are durable and all
native work/three leaves are gone. Refusal occurred before governed write issue
or dispatch, so no fictional write-effect journal row is claimed.

An ordinary idle restart, without freezing or inference, separately preserved
all 227 canonical/source hashes, both cache-owner rows, four harness rows, three
reservation rows and the valid cache's device/inode/bytes/hash exactly. Actual
PAM desktop/320 replay also matched all 227 envelopes with zero send/reply/cancel
or configuration mutations. Controlled crash recovery is not substituted for
that ordinary restart check.

Two proof-driver assertions were corrected read-only: one selected hidden folded
tool arguments instead of the final assistant paragraph; another assumed a write
call was block zero although thinking preceded it. Their failures remain in the
packets; no model sends, replies, database seeding or product changes repeated
those actions. Six combined normal/sanitizer fixtures and all 76 lint gates pass.
The preceding `d3639f1dc` completed all 53 main CI jobs and all seven auxiliary
workflows. Fresh final-head CI and fresh owned `.252` deployment qualification
remain separate; prior VM 8163 no longer exists and must not be retaken.

This checkpoint supersedes the older local-Claude-unqualified statements below.
It qualifies this pinned Linux SDK/Max/Opus profile, not every subscription or
native platform. Production `.100` remains untouched until a merge to `main`.

Separate fresh reviews of `aa8c774..ff71e5d48` covered browser/history and
cancellation races, execution/security and native grant lifetime, source forks,
project custody, canonical learning ownership and the remaining C/build changes.
No confirmed blocking regression was found. These are code audits, distinct
from a later approving GitHub identity under `OWNERS.md`.

The VM API profile's actual HTTP429 did not establish that local Claude was
unavailable. The workstation already has an authenticated Max login and the
pinned Claude 2.1.227 executable. Local qualification uses an owned user/mount
namespace with an actual private PAM account, a delegated user service, a
separate restricted PostgreSQL database and the exact `ff71e5d48` server/module
build. The qualified SPA is retained with its identical frontend input tree.
The existing local OAuth login is enrolled through the supported credential
owner into an isolated test profile; permission mode remains `default`.
No host accounts, unrelated databases or `.100` state are changed. Actual local
question/reply and permission-denial results must be recorded separately from
the earlier VM API failure; a bare CLI response alone is not integration proof.

The first local browser request admitted the actual operator and pinned SDK but
produced no model/tool/question output before a persistence failure and SDK
shutdown. No upstream status was established. The SDK process tree was proved
empty and its scoped token revoked; no reply or duplicate send occurred. A
controlled replacement retained the original database, namespace and cgroup
identity. Supported canonical browsing preserved running/user records 1/2,
appended one restart cancellation at 3, reconciled the same reservation and
returned replay completion with zero active native reservations. This qualifies
recovery of that failed attempt, not successful Claude execution.

A second normal-UI attempt, without extra replay observers, also failed capture
before upstream HTTP. Supported settled recovery preserved its records 1/2 and
appended exactly one cancellation at 3, with zero active native reservations.
A transient directory-read timeout during its earlier browse exposed a separate
bug: canonical SSE could send HTTP200 and replay without running history recovery.
The route now requires successful owner lookup and history opening before any SSE
headers. The regression reproduces the old bypass when the first lookup times
out and a later lookup would succeed; the fixed full HTTP fixture and formatting
check pass. Normal browser attachment already acknowledges the durable owned row.

The third local request passed credential and canonical-history stages, but the
attached debugger then crashed and terminated the isolated daemon. Its normal
recovery is separately proved; it is diagnostic contamination, not a provider
outcome. The fourth logging-only diagnostic run exposed the secondary defect:
a typed transport ERROR before TURN_START was rejected by the native response
builder, poisoning capture. The fix preserves that host diagnostic without
manufacturing an assistant message, original response or tool source. The actual
pre-frame-error fixture reproduces the old failure; native-runtime and Anthropic
fixtures pass normally and under ASan/UBSan with leak checks.

The fifth real normal-PAM local-Max request on the reviewed production changes
plus a separately hashed logging overlay reached upstream HTTP429. Its original
running/user records 1/2 remain intact, typed diagnostic 3 persisted, and failed
terminal 4 reports the safe trusted SDK rate-limit classification. Independent
readback proved FINISHED ownership, zero active reservations and removal of the
exact native cgroup leaf without restart. Desktop/320 reload repeated no send,
reply or cancellation and showed no stale controls or reconnect error. The SDK's
internal error record is not raw stdout evidence; numeric HTTP429 is separately
proved by the actual transport. No specific quota cause is established.

A bounded pinned-SDK diagnostic with the actual operator's locally successful
Opus5 model returned successfully in 4.7 seconds. This is not Aimee integration.
The isolated model was then updated through the supported owner API to Opus5;
its normal full-catalog browser request also reached upstream429. Question/reply
and permission denial remain pending. Next diagnostics retain only allowlisted
429 metadata and envelope counts, with strict TLS and source guards unchanged.

The replay-fix revision `79b9f97df` completed all 53 main CI jobs. Six auxiliary
workflows passed; attribution verification failed because GitHub returned HTTP503,
not because a policy violation was reported. The read-only API helper now retries
transient transport/500/502/503/504 failures at most three times, with cancellable
backoff; authorization failures, incomplete metadata and exhausted reads still
fail closed. Strict provenance/history checks and automatic review are unchanged.
HTTP fixtures and existing policy/workflow regressions pass. Retain the original
failed-run evidence when replacing the latest unmerged commit with the combined
reviewed corrections; add no historical SHA or provenance exemption.

### Claude gateway protocol compatibility follow-up

The actual operator credential file did not change after supported enrollment;
its expiry remained valid and the supported enrollment audit matched that source.
A further real full-Opus request yielded a complete 114-byte HTTP429 error body
with only `rate_limit_error` and a generic message, without a reset instruction.
Its envelope was 46,911 bytes with 53 tools and one message; the successful direct
SDK diagnostic used about 22K input/cache tokens. No specific quota or context-size
cause is established. Every updated-runtime request failed durably and finished
custody without a restart; successful question/reply/permission execution is pending.

Official gateway guidance and pinned-SDK source exposed two concrete compatibility
gaps. Because the host reshapes system content, its verified native launch now sets
`CLAUDE_CODE_ATTRIBUTION_HEADER=0`, causing the SDK to omit its fingerprint block
rather than having a gateway strip or relocate it. This is unrelated to Git/PR
attribution policy. A real descriptor child and actual pinned SDK against a local
synthetic provider verify omission; neither is live vendor acceptance.

The original SDK's typed `metadata.user_id` was also lost when canonical host
ownership replaced its parsed request. The gateway now preserves this bounded
optional correlation field per callback, retaining host metadata and refusing
conflicts. No SDK system/transcript/tools/raw sidecar or new identity authority
is admitted. The baseline fixture reproduces the loss; twelve normal/sanitizer
scenarios cover the contract, malformed values and unchanged non-Anthropic behavior.
Combined fixtures retain the earlier pre-frame error fix. Independent reviews,
clang-format19, documentation and IR bypass gates pass. Qualify the exact newly
committed unmodified runtime through normal PAM/browser next; do not relabel the
previous logging overlays as final-source vendor acceptance.

The clean, unmodified `2c20c49ec` server and CLI were rebuilt in a fresh object
directory (840 compile commands) and installed only in the owned local runtime.
Independent `/proc` executable hashing matched the candidate, with the original
namespace, delegated cgroup, PAM account and database retained. Unchanged Go,
runtime-web and SPA components have separate input/hash lineage; they are not
represented as newly built components. The actual SDK child had all fourteen
expected environment entries, including fixed attribution-header value `0`, and
no parent OAuth secret. Its single normal-PAM full-Opus browser request still
failed before any tool or question, with the trusted SDK rate-limit classification.
Canonical records 1/2/3/4 persisted, native ownership reached FINISHED at 4, all
reservations became inactive and the exact execution leaf was removed naturally.
Desktop and 320px reload repeated no send, reply, cancel or metadata mutation.
This qualifies clean-source failure handling, not successful Claude execution.

The supported interactive Claude `/usage` command performed no inference and
reported about 2% weekly usage, with current session usage at 0%. Combined with
the earlier successful direct pinned-SDK diagnostic, this does not support a
blanket account-exhaustion explanation for Aimee's opaque upstream failure.
Neither the two protocol corrections nor quota exhaustion is established as its
cause. Continue the source/protocol comparison without blind provider retries,
invented client identities or relaxed canonical ownership. Actual local Claude
read/question/reply/completion and permission denial remain unqualified.

The failed request also exposed presentation noise: the generic stream diagnostic
and clearer terminal explanation appeared as two primary alerts. The browser now
keeps both canonical observations, prioritizes the failed terminal's explanation
and places only the exact generic diagnostic from that same turn under collapsed
Response details. Custom errors, other turns, unknown origins and terminals with
no explanation retain their diagnostics. The standalone generic message uses
plain language. All 445 frontend tests and both SPA/console production builds
pass; replay and closed controls remain unchanged. The newly built SPA has its
own hash/input lineage and must be qualified separately from the previously
deployed frontend.

The native gateway also dropped the pinned SDK's actual User-Agent and x-app
headers. Authenticated native callbacks now retain those original bounded
singletons as transport metadata, without assigning identity or capability
authority. Ordinary ingress and every native exit clear the thread-local values;
ordinary Aimee requests keep their existing User-Agent. The socket writer emits
one User-Agent and preserves complete bounded extra headers instead of its old
512-byte truncation. Baseline socket fixtures reproduce both duplicate-header
and lost-tail failures. Four focused normal and ASan/UBSan/leak fixtures pass,
as do clang-format19 and an independent source review. The actual 53-tool SDK
loopback probe's beta block fits the old limit, so that truncation is not a
proved explanation for the observed rate-limit failure. Neither forwarding
invented SDK identities nor bypassing canonical request ownership is permitted;
live qualification of the exact combined revision remains required.

Source `2c20c49ec` completed all 53 main CI jobs and all seven auxiliary
workflows successfully. The next clean `91355fd35` build passed server/CLI and
build-only KB/gateway compilation without enabling KB services. Its new SPA's
137 tracked frontend inputs matched the tested build. Actual normal-PAM desktop
and 320px replay of the retained failed turn showed one clear terminal alert and
collapsed details, with no repeated actions. A composed actual-pinned-SDK to
production proxy/header capture/C socket loopback test also passed; this is
synthetic-provider protocol evidence, not vendor acceptance. A warning in that
composed LTO context prompted a one-line valid-C-string length-check correction;
normal/sanitizer socket tests and the warning-free composed probe pass again.
Keep that corrected source distinct from the already installed `91355fd35`
producer when rebuilding for the next live request. All 76 lint checks were run;
the sole missing-clang-format19 tooling failure passed on explicit-binary recheck.

The clean corrected `d3639f1dc` four-product build also passed without warnings.
Its one actual full-Opus browser request still failed before tools/questions;
independent readback confirmed failed terminal 4, FINISHED ownership, zero active
reservations and natural removal of the execution leaf. New-SPA desktop/320
replay showed one clear terminal error and repeated no actions. This is another
retained failure, not question/reply or permission acceptance.

Two further independently reviewed protocol losses are repaired in the next
combined source. The actual pinned SDK deliberately prepends its fixed Agent SDK
component identity even when given a custom system prompt. After measuring and
holding that executable, the host now represents this truthful identity as typed
profile evidence before registry/cache copies. Host policy, history, tools and
per-block cache markers remain intact; arbitrary SDK system text is still ignored.
The typed mutation is marked, and Codex profiles remain unchanged. Missing-identity
baseline, malicious incoming-system and repeated-profile cache cases are covered.

The exact admitted SDK `?beta=true` marker now reaches the host-selected Anthropic
model/count endpoint. This is not arbitrary URL forwarding: the capability route
still normalizes to the same owner, and only the fixed marker survives as transport
metadata, cleared on every ordinary request and native exit. URL construction
precedes wire fencing. Official SDK fallback code distinguishes the beta surface;
ordinary beta headers alone do not prove the query irrelevant. A scoped cache
contract revision prevents silent reuse of the earlier stripped-query policy.
Baseline dispatch reproduces query loss; buffered/stream/count, reset, invalid
query, unowned and non-Anthropic cases are covered. These corrections require
fresh exact-source live acceptance; neither is asserted as the cause of the 429.

### Final live qualification: 21 October 2026

Source `230b490627b716b3aa41af30672e6c2b3269d19a` passed all 53 main CI
jobs with no skips and all seven auxiliary workflows. The exact complete GUI
image `sha256:c32b572c592d85032625ef136ae728d6e5a5390479915c5e00ecb5125d6d0f45`
passed all 10,106 source-file comparisons, running artifact hashes, normal PAM
login, managed model TLS discovery and desktop/390/320 replay/reload. The original
28 API recovery records remain unchanged; no send, reply or cancellation was
repeated. PostgreSQL, Vault, provider and TLS state were preserved. Matching
fresh C consumers were deployed together on owned VM 8163; byte-identical Go,
configuration, runtime-web and SPA components retain their explicit input proof.

A genuine native Bash invocation of
`aimee index investigate 'classify.sh zero boundary'` succeeded without project,
endpoint or token overrides. Independent database and filesystem readback
confirmed original call 153, successful result 155 and completion 159 in session
`af360a42-2822-eca5-7332-863c1ed0b26a`, with the actual operator principal,
workspace, local project UUID and checkout revision. The selected project is
`local:92584937-94a1-48e0-84f5-2e7b2c384912`; the actual checkout is
`d940c6640b38331491ce18bf38903f33141d1c31`. The result is a successful scoped
read with empty retrieval, not evidence of corpus hits. Ordinary index reads are
outside the governed-effect journal's eligibility contract; no fabricated nested
model call, mutation receipt or indexed-outcome row is claimed.

The KB-free real-service integration passed 171/171 applicable checks on the
production component at `7138bf6e3`; final-source CI integration also passed.
Five optional HTTP400 cases were qualified separately through actual C-over-UDS
with the runtime-web status owner. The remaining 20 KB-dependent checks are
intentionally excluded because production must not use a KB instance.

One fresh normal-browser Claude request on the exact `230b49062` build reached
Anthropic and received actual HTTP429/rate_limit. Its existing SDK backoff ran
inside that single request; there were no agent retries or quota changes. Session
`web-c766156a-883b-4b5e-84a7-8c33ce39cc5c` contains only running, user and failed
terminal records. Independent database readback confirmed the finished
reservation, zero active work and zero tool/question records. Read-only PAM reload
sent nothing and left no pending controls. Successful genuine Claude questions
and permission execution remain unqualified. The available SDK record does not
establish which upstream quota or rate condition caused the 429.

That actual failure also exposed a final usability gap: the generic canonical
failure concealed the SDK's rate-limit classification. The follow-up
translation admits only the exact SDK API-error marker and the allowlisted
rate-limit category. It emits the static message "Claude request was rate limited
by the provider." only after a failed terminal result; provider diagnostic text
never becomes assistant output. Cancellation, capture failure and later success
retain precedence. Normal, ASan/UBSan/leak fixtures and CMake tests cover this
boundary. The saved SDK conversation log uses an internal camelCase marker;
inspection of the pinned Claude 2.1.227 stdout mapper and its query-loop call site
established the actual emitted marker, `is_api_error_message`. The parser and
fixtures use this snake_case contract; a camelCase-only lookalike cannot classify
an error. The initial log-shaped fixture did not qualify stdout handling and is
superseded by that correction. Old history is unchanged. Fresh CI and the matching
VM deployment qualify this follow-up separately from the green `230b49062`
qualification above; the CT image remains exactly `230b49062`.
Production `.100` remains unchanged, KB-free and unpromoted; the distinct reviewer
and live-ruleset requirements in `OWNERS.md` remain outstanding.

### Live native CLI authority repair

The complete packaged GUI at `25e62c1e0f17c5455dd7b03d245c06d8c825a924`
passed normal PAM, exact running artifact hashes, managed model TLS discovery,
read-only replay/reload and desktop/390/320 screenshots. The Reasoning label no
longer implies completed history is still running; no extra send/reply/cancel
occurred and all 28 original API recovery records remained identical. The
isolated real-PostgreSQL/Docker integration passed 171/171 checks. Five optional
HTTP400 checks also passed separately through the actual C-over-UDS service with
its runtime-web status owner; 20 KB-dependent checks remain intentionally
excluded. Production still has no KB instance.

A single genuine native Bash CLI invocation then exposed HTTP400 before index
admission: the POSIX client omitted the listener port from HTTP Host, while the
private proxy correctly required its exact authority. Its failed canonical turn
and absence of nested index admission are preserved; no project-scope success is
claimed for that attempt. The client repair separates HTTP authority (including
port and IPv6 brackets) from the bare TLS verification/SNI hostname. Proxy checks
are unchanged. The real thin client now passes against the real strict proxy;
the pre-fix client fails the same regression. Actual TLS hostname acceptance,
wrong-hostname refusal, IPv6, normal and sanitizer checks pass. The existing CI
step now requires the whole-binary proxy regression. Fresh CI, matching deployment
and one new genuine native CLI turn must still qualify this repair.

### Live source-handoff result and remaining integration repair

Executable source `38d55ba726e22e11e0a7cae8750200139ae0da87` is deployed on owned
VM 8163 and in complete immutable CT image
`sha256:18b9f69453dfc66b15cce1ca6d7724db6cfdbcc4f6b9289d1e179703f7b4de72`.
The CT image includes runtime-web and code-server; clean-image artifact hashes,
normal PAM login, the served SPA and managed-mTLS E4B probe passed. PostgreSQL
identity/start time/volumes, provider configuration, login HMAC and TLS were
preserved. The VM's first install rolled back when generated runtime composition
was missing; regeneration from exact descriptors subsequently proved all 52
composition files identical before successful installation. No app state was
reconstructed or reset.

A genuine Codex delegate now passed configured verification. Job 2's child
`delegate-d4cd3fcd4dae947e822c4258c4fc1f208a3443b350bcdb6d9fe85fe8ed68de99`
completed turn `bdd9a1c01eeb20e6acc60ed3cac64fbc` at sequence 18. Its actual
git_verify ran once on original task commit
`d940c6640b38331491ce18bf38903f33141d1c31`. Independent database readback
confirmed the canonical inline proof equals stored test-grade event 127,
source-bound delegate experience 128 equals the sole matching learning row 3,
and actor/project/parent/job/child/revision bindings agree. The row remains
pending review with zero confidence and no automatic application. This is a
configured-test verification-only delegate, not a new causal repair grade or
unrestricted semantic-quality claim. The previous failed job 1 remains intact.

A fresh single Claude request on this source again reached Anthropic and failed
canonically after actual HTTP 429/rate_limit, before assistant or tool output.
Existing SDK retry/backoff occurred within that one outer request. No agent retry
loop or quota override was used; successful Claude vendor acceptance remains
blocked. The actual API-model pending-question/crash/continuation journey uses
the full tool catalog. A real request_input became source 4/question 6; the owned
app was killed without resetting PostgreSQL. Normal browser recovery produced
exactly one operator-cancel terminal 7, authenticated unknown-outcome receipt 8,
and one continuation 9/user 10. That continuation executed read_file on README,
with canonical call 13/result 15 and independently confirmed actual file content.
The real model then completed; the final marker was visible and reload showed
no question, no Reconnect state and no retransmission. Exact totals were two
sends, zero question replies and one explicit cancellation. The fixture first
tripped over mount-array ordering after the crash; only that comparison was
normalized, preserving the original failure packet and the same container,
question, source and nonce. No provider response or history row was fabricated.
The E4B validation alias explicitly uses 32768 context, 1800000 ms timeout and
max_output 1024; max_output is capacity metadata, not the generation budget.
The actual upstream generation limit was 8192; use max_tokens for future bounded
CPU runs. Other aliases and the actual provider/model/endpoint remain unchanged.
This is not a default responsiveness qualification.

Scan-metadata-only follow-up `28fb5ca07c8d7c4200662645ed3a593935a1f3cb` records one
exact public OpenAPI checksum false-positive fingerprint; recomputation and the
complete-history secret scan passed. Fresh CI passed 52 of 53 main jobs; only
integration failed. One failure expected the retired raw delegate-learning
publisher; its repair requires positive actual canonical child observations and
keeps historical compatibility in a separate PostgreSQL test. The other exposed
that script RPC had marked direct operator scripts as model execution while
omitting canonical capture handoff. The repair retains/binds/releases real
captures for model-origin workers, keeps all scripts untrusted, and marks only
the original authenticated direct-operator script path with host-only provenance.
Actual model entry clears that provenance; the durable cancellation fence still
applies. Real tools/policy-owner fixtures cover both paths and missing capture.

The native shell CLI bridge uses lifetime-scoped child grants,
normal actor/cwd/capture admission, exact registered family projection and nonce
replay protection. It must use the actual generation's final post-policy catalog,
not the initial request profile. No new model tool-call IDs or alternative
conversation store are created. Container/detached shells do not receive a host
loopback grant. The real thin-client binary dispatches native commands before
operator profiles, remote credentials, local handlers and workspace bootstrap;
it cannot fall back to those paths when its child grant is missing. Real-binary
entry regressions, focused normal/sanitizer checks, CMake fixtures and all 76 lint
gates passed before the final combined run. Fresh combined CI, isolated full
integration and actual shell CLI validation remain required;
production `.100` remains unchanged and KB-free.

### Current qualification checkpoint, 21 October 2026

Committed head `4ad4843b2c7013d8fc37d1c70453bf8bd261ba3c` passed all 53
main CI jobs and all eight workflow runs. On owned `.252` VM 8163, its normal
PAM browser recovered an actual interrupted Codex question produced by the
previous `d0234bc8` build. Recovery proved the exact frozen SDK containment leaf
empty before retiring it, appended canonical cancellation and a source-bound
unknown-outcome receipt, and restored the composer. A subsequent real
write_file request was declined once through the normal browser. The model
completed, reload repeated neither send nor reply, and independent host
inspection found no file and no active native reservation. Default systemd
KillMode was restored. The packet records producer and recovery builds
separately; this is genuine vendor execution, not seeded receipt validation.

That same build's genuine delegate validation failed before producing a child
tool result. The qualified child launched and finished custody, but the SDK
reported an unavailable stream while ingress source assembly failed. This is
retained failure evidence, not a successful delegate grade. Source inspection
found the captured source-release ticket was still bound to the parent's
request ID when the callback minted its required distinct transport ID.

The next changes implement an explicit host-only Go source-release fork: the
same actor/caller must prove the parent binding, and existing local/shared/grade
owners freshly revalidate the sources before the new callback handle is
published. Parent expiry is retained; provider send guards still run separately.
No ticket is cleared to bypass validation. The duplicate legacy delegate-learning
writer is retired; canonical source-bound execution observations remain the sole
learning publisher, while operational exit metrics remain independent.

Generic API execution also gains explicit exact-turn operator cancellation.
This cancels continuation without asserting process or effect quiescence, fences
late writes/action admission, and atomically records unknown-outcome receipts
for actual unmatched API calls. History reload and native switching authenticate
the immutable original calls and privileged cancellation. The browser retains
text on failures and submits one continuation only after the exact canonical
terminal. Actual PostgreSQL race tests and cross-language fixtures cover pending
questions, repeated call IDs, compaction, forged receipts and ownership refusal.
These newer changes require fresh CI and genuine API crash/replay acceptance;
the previous head's green jobs and native recovery do not qualify them.

At that earlier checkpoint, live delegate source-fork, API orphan recovery and
native Bash CLI acceptance were outstanding. The first two have since passed
as recorded above. Commit `606bf3db236a942d935410b56c3329f3b6935854` implements
the child-scoped CLI bridge and both integration repairs; all 76 lint gates pass.
Its exact-source isolated integration and packaged GUI qualification are running.
GitHub has the new branch ref but reports delayed pull-request/webhook delivery;
no new workflow run has started at this checkpoint. Actual native Bash CLI
acceptance still must qualify the new matching server and client. The final
browser display refinement labels retained reasoning as Reasoning, so completed
history does not falsely appear to be thinking again; it changes no execution or
replay state. Claude still reaches the actual vendor but returns HTTP 429;
no successful Claude inference is claimed. Production remains KB-free and
unmodified; independent production review and promotion are outstanding.

### Genuine live-model continuation, 21 October 2026

PR #3057 now includes executable-source commit `e9bde72c703c0b9e6569326d46063401f6ebe369`
and the reviewed memory-boundary ledger follow-up
`4eeee289ed3dd636de1664db4f63fe52d9fa6a86`. The latter changes no executable
source. That head passed both Windows and macOS builds, frontend, lint and
PostgreSQL test execution, but the reliability release gate detected the newly
expanded governed-action fixture fingerprint. Its explicit fixture review now
records the added canonical-actor cases, retaining every existing assertion,
required case, group and release predicate. Fresh CI must qualify the subsequent
combined head. The earlier 58 successful checks qualified `b8c7670a6`, not
these subsequent live repairs.

Actual model validation exposed failures beyond the owned provider fixtures:

- Durable action and exploration consumers used the account principal
  (`uid:0` or `webchat:<user>`) where canonical sessions require the attested
  actor (`webuser:<user>`). Their typed store calls and task projection now
  use the canonical actor; resource and memory-owner calls retain their
  existing account identity. Actor mismatch remains refused, an empty
  attested actor cannot fall back, and complete actors survive the finish
  callback. Three Go bounds now match the native 159-byte actor capacity.
- Normal GUI clones were absent from native workspace custody. The host now
  obtains a held directory descriptor from the actual registered Git project
  owner, requiring the PAM attestation, tool capability, published clone,
  matching registered remote, lifecycle lock and no-symlink traversal.
  Unregistered and foreign roots remain refused; the documented shared
  environment namespace is preserved.
- A real discovered model ID plus provider name exceeded the local alias
  limit. Enrollment now provides an editable Name in Aimee, validates the
  backend UTF-8 byte limit before sending, and preserves the actual model ID
  and provider registration. All 426 frontend tests and the real API
  enrollment check passed.

The four actor C fixtures passed normally and under ASan/UBSan with leak
checking. Actual PostgreSQL owner/race regressions and the execution-policy
race suite passed; clone custody fixtures, their sanitizer runs and the
production server object passed. All 76 local lint gates were exercised; the
reviewed boundary record and explicit installed Go/clang-format tools resolved
the three initial failures without disabling a gate.

Genuine shipped E2B inference completed a full-catalog browser turn, persisted
canonical deltas and its terminal assistant answer, and replayed after reload
without a resend. This low-power CPU took about 599 seconds with explicitly
configured 32768 context and 600000 ms timeout. The original 8192 context
overflow and default-timeout failures are preserved. A sequential, separately
identified E4B image passed actual mTLS discovery, UI enrollment and the normal
Models probe; that probe is not full-tool Chat acceptance. Both use actual
model weights and responses, not the external provider fixtures.

The exact repaired source is deployed in owned VM 8163. A genuine Codex turn
performed an acknowledged read_file and completed canonically; its native
reservation finished under qualified user/mount/PID/cgroup/network/write
containment. The VM uses proper systemd delegation rather than a manually
chowned sibling cgroup. The normal PAM browser also passed actual Codex
selection, streaming, completion and history replay, followed by a genuine
acknowledged file read and request_input across reload: exactly three sends,
one operator reply, final completion and resolved replay without a repeated
send or question. The same session subsequently preserved its entire
canonical prefix across Codex → Claude (HTTP 429 failure) → Codex, performed
another acknowledged file read, recalled the actual operator answer and
replayed without resending. This qualifies continuity through a failed model
switch, not successful Claude inference. An initial genuine coding task produced a real local commit that passed sixteen
independent signed boundary cases without changing the pinned existing tests or
configuration. Its configured verification failed on a root-owned global lock;
that failure remains recorded. After configuring the supported state-local lock,
a fresh genuine Codex task repaired the code, made registered Git commit
`d940c6640b38331491ce18bf38903f33141d1c31`, and ran the private configured tests
successfully. Canonical session `af360a42-2822-eca5-7332-863c1ed0b26a` completed
at sequence 72. Its inline verification proof equals the actual stored grade
proof, bound to the same canonical actor, project, event, turn and revision.
An independent sixteen-case semantic check also passed; trusted tests,
configuration and instructions remained unchanged. This is a configured-test
grade plus a separate independent task oracle, not an unrestricted quality claim.
Actual scoped reuse and delegate learning still require their live checks.

The actual native index_investigate call exposed two scope defects. Unlike HTTP
dispatch, its direct MCP handler omitted active-project injection. The native
context now shares the HTTP resolver on a private argument copy and uses only
the host-installed cwd. Live retesting then revealed that an unrelated indexed
repository prevented fallback to the selected checkout's valid persisted project
identity. Resolution now preserves matching indexed identity/root precedence,
then uses the actual checkout identity even when other repositories are indexed.
Missing identity and unmatched roots still refuse; explicit project arguments
remain unchanged. No KB instance is required or introduced. The combined wrapper
and resolver regressions cover unavailable, empty and unrelated indexes, hostile
model cwd, immutable admitted input and response lifetime. ASan reproduced a
handler-response use-after-free before the independent response-copy repair;
normal, sanitizer and the exact affected Cppcheck shard pass after repair.
Genuine default-scope lookup on the combined new build remains required; the
explicit-project diagnostic and successful file reads do not qualify it. The
user's original regular CLI example was checked read-only from the real wol-lore
checkout: it resolved only wol-lore hits without scope_required.

The real CI LSP providers completed all twenty trials each for gopls and pyright.
The frozen release guard then refused the reviewed extraction of the native MCP
host wrapper. Two exact literal reversals now restore that host table to its
previous reviewed bytes before the existing whole-file frozen comparison.
Callback, advert, LSP, missing-block and duplicate-block mutations still refuse.
All 22 benchmark tests and revalidation of the unchanged real CI artifact pass;
no provider threshold, lineage, fixture or frozen-source exemption was added.
Fresh CI must qualify the combined commit.

Claude now passes GUI clone custody. Its first credential-source refusal was
traced through existing safe Vault audit records: source parsing and atomic
credential storage succeeded, but the enrolled access token had expired and
the workstation login rotated after enrollment. The healthy current
workstation document was a newer snapshot. Supported whole-document
re-enrollment passed, and Claude acquired qualified native custody and
reached the real Anthropic endpoint. The vendor currently returns HTTP 429,
so its browser matrix remains unqualified; the bounded SDK turn failed canonically.
No credential guard or vendor limit is bypassed, and no successful Claude
vendor run is yet claimed. Production remains KB-free and unmodified. Do not call the migration
or live acceptance complete while these cases remain outstanding.

### Genuine interrupted-turn browser recovery follow-up

Source commit `d0234bc8c430c6208a8c6d2d1d6644ad4b0d3c89` passed all
53 main CI jobs, including four deployment tiers, both thin-client platforms,
real-provider LSP, PostgreSQL replay and strict static analysis. Its complete
immutable image `sha256:014a9877e81601128bf59914a82dd742454f09a4268bd99194b82198c4072b55`
was built from the exact source archive and deployed on owned CT 8162. The
image-only upgrade preserved PostgreSQL identity/start time/volumes, normal PAM
login, provider configuration, Vault session HMAC and TLS. Running server,
runtime-web, SPA and provider artifacts match the clean image; code-server is
present. Normal PAM browser login, exact served SPA and actual managed-mTLS
E4B inference probe passed. This is not full-tool E4B Chat acceptance.

The combined project repair also passed genuine native default-scope lookup on
VM 8163: one actual index_investigate call without an explicit project completed
without scope_required. A same-actor HTTP call from the selected worktree also
passed while the only indexed project remained an unrelated repository. The
truthful result was empty/unavailable evidence, not fabricated hits or a KB.
New-session genuine Codex recall reproduced the two actual configured-test
records in the same project and exposed neither in a foreign project. An
ordinary same-build service restart preserved the complete canonical prefix,
completed a new registered file read and reused the exact Codex SDK session ID;
reported cached-input tokens were zero. No vendor token-cache hit is claimed.

A subsequent genuine interrupted-turn test exposed a remaining browser deadlock:
recovery ran only before a new model turn, while stale canonical Running state
left the composer offering only Steer. Browser opening now invokes a separate
registered browse-recovery owner after exact attestation/durable-owner checks,
before legacy import or SSE. It validates all native records, preserves exact
process-local retained writers, and applies the unchanged physical lease,
fresh database authority and whole-tree containment proof to abandoned owners.
Arbitrary busy leases and malformed/foreign records still refuse; strict model
admission is unchanged. Canonical actors up to 159 bytes can browse an empty
owner set without inheriting the native execution profile's narrower bound.
No model or tool turn is started by browsing. Focused regressions and sanitizer
checks cover this seam; fresh CI and genuine same-source recovery/permission
retesting must qualify this later repair.

A separate source audit found the analogous generic API-provider crash gap:
without a native reservation, browsing cannot reconcile the old Running turn,
and process-local cancellation cannot recover a lost registry entry. Graceful
shutdown drains and terminalizes workers; hard interruption is the unresolved
case. This path is source-confirmed but not yet live reproduced. Do not weaken
the existing unfinished-history guard or infer cross-process quiescence from
absence of a local writer. A sound generic durable-owner/cancellation repair
and meaningful regression remain required; the native repair does not cover it.

The VM's separate core/module test services must restart together after killing
the core; a stale module bus initially caused configuration bootstrap refusal.
That test-orchestration correction is distinct from the confirmed browser bug.
The original qualified native leaf/source remains preserved for retesting; no
synthetic recovery, permission reply or effect was seeded. Production `.100`
remains unchanged and KB-free; Claude successful vendor inference remains
unqualified after actual HTTP 429.

### Actual `.252` qualification, 21 October 2026

Disposable CT 8162 and VM 8163 on `pvetest` were created for this work; existing
guests were left alone. The immutable merged application and PostgreSQL images
were used for KB-free T3 deployment. Its seven gates passed, with 217 personal
memory, 15 semantic memory, 21 exploratory memory, 352 provider-boundary and 87
native-async checks. Egress/Vault passed 18 checks. Plain storage passed all 11
checks on an unchanged fresh retry; its initial startup timeout remains recorded
with an undetermined cause. The CT's initial Docker memlock failure was resolved
by raising only its own Docker/containerd limits to the shipped 1 GiB setting.

The Proxmox host lacks Landlock, so native containment was qualified in VM 8163
with its own supported Debian kernel rather than weakening the guard. Actual
server integration passed **171/171**, including all eight pinned SDK profiles,
both Codex/Claude crash recoveries and both interruption/denial journeys. The 25
unconfigured-service skips remain explicit. The persisted-learning audit found
79 ungraded canonical outcomes, two configured-test grades and nine delegate
observations across four principals/project scopes, with no invalid source,
proof, scope, pending-review or duplicate bindings. No learning rows were seeded.

Real Chromium against the deployed GUI exposed defects that the earlier
component fixtures did not: primary selection omitted the authenticated webuser,
fresh presence did not establish its durable canonical owner, and a page reload
could reuse an attachment that unload had detached. The follow-up forwards the
actual actor, admits the exact durable browser owner before attachment success,
and acquires a page-owned attachment before preferences, replay or sends.
Explicit reconnect reacquires a lost surface without retrying the failed action;
background replay no longer clears its send error. Cached Back navigation keeps its
live surface; only a genuine page discard detaches it. Foreign owners remain refused.

The combined derivative passed live first-open model pinning before any chat,
reload with zero sends, real model switching and durable history, a real pending
question across reload, exactly one browser reply and resolved replay, plus
320/390 px layout checks. Visual inspection then found that the mobile project
selector overlapped Channels despite fitting the viewport. Its flex wrapper now
occupies a separate row, with actual control-overlap assertions at desktop,
320 px and 390 px. The source passes 419 frontend tests, both frontend
builds, the Chromium component harness, runtime-web race tests, normal HTTP and
presence fixtures, HTTP ASan/UBSan/LSan, and all 76 lint gates. The published
baseline and manually built test derivatives are separately identified; fresh
follow-up CI and production `.100` promotion/verification are still required.
A second real Chromium pass exercised actual Codex and Claude interruption
permissions. Both deny replies reached the exact native waiter once, remained
resolved after reload and produced no write or repeated prompt. Both final
assistant messages also rendered without protocol/reconnect errors. Its focused
recovery run passed 5/5, separately from the immutable 171-test baseline; the
test-only browser handoff was restored byte-exact afterward. This exposed a
remaining browser consumer mismatch: canonical owned assistant snapshots may
carry a matching outer role, and owned tool-result messages use role `tool`.
The browser now accepts those existing Go/C contracts while retaining exact
owner validation, immutable source custody and assistant-only partial messages.
Actual native snapshots and negative role/owner cases have regression coverage.

The first follow-up CI head also failed the vulnerability gate on Go 1.26.9
(GO-2026-6809). All three module directives and active Go build-image versions
now use Go 1.26.10 with the official production image digest. Actual vulnerability
checks passed across all three modules; affected image/build pin checks passed.
Fresh CI must still qualify the combined follow-up head and rebuild its images;
the previous published baseline is not the production promotion candidate.

Owned provider fixtures establish runtime contracts, not live vendor acceptance
or semantic model quality. Production `.100` must remain KB-free.

The supported browser/Linux cutovers are implemented: canonical primary loading
and model switching, durable session-log history, request/response streaming,
scoped native tools, delegate execution/history, persisted model selection and scoped persona controls,
and canonical permission/question replies. Each child owns its controls; replies
bind the actual durable child session, turn and actor. Replay readiness prevents
historical controls from appearing before their saved replies. Reconnect and
repeated delivery do not resend replies or reopen denied requests. Unsupported
branching is hidden; mobile preferences wrap without clipping. Failed steering
is a durable failed admission and never becomes later model instructions.

Native Linux Codex/Claude launches retain canonical host calls/results and
cache/replay custody. Optional qualified containment uses a delegated cgroup,
pre-exec attachment, private namespaces, restricted filesystem/network access and
an exact durable owner reservation. Recovery proves actual tree quiescence before
fencing an abandoned turn and rotating the shared cache epoch; old SDK files are
preserved. An interrupted tool effect remains unknown, with its first protected
call source and terminal binding retained. A later mutation requires permission
for its exact effective arguments; denial suppresses repeated attempts in that
turn. Ordinary launches without the optional boundary keep their successful
launch path but do not acquire a whole-tree crash-recovery proof.

Legacy import now happens on browser opening, before SSE headers, as well as on
model loading. HostOnly50 locks the actual session/agent/provider source, owner
and raw-source hash. Its explicit browse mode returns `skipped` without appending
when completed canonical primary history wins; model import remains strict.
Pending turns and their reply paths remain live. Empty legacy agent selectors
are preserved. Reports distinguish exact import, normalization, archive and
refusal; source rows remain intact. Terminal text is a passive archive with no
invented message roles or tools. Large canonical messages use bounded protected
fragments instead of imposing a smaller pre-compaction history limit.

The browser shows the acknowledged imported conversation in chronological
position before newer observations, using its exact canonical roles and blocks.
It exposes that base only after its report-bound checkpoint is complete; fragment
hashes are verified before parsing/rendering. Archive count and raw UTF-8 digest
also verify before explicit expansion. Ordinary recurring prompt checkpoints
remain cursor-only, so browsing does not duplicate them. Historical browser rendering grants no execution, control, learning-grade or
model-observation authority.

KB-free learning uses canonical terminal evidence and a source-pointer outbox.
Configured-test grades, pending delegate observations and same-project recall
retain actor, project, parent/job/child and provider-send freshness fences.
Unattended bounded recovery republishes unacknowledged projections after daemon
restart, without reconstructing evicted delivered rows. Ungraded execution stays
ungraded; exit success does not imply independent semantic correctness or causal
procedure success. No KB instance, seeded facts/grades or fabricated successful
outcomes are required. Production `.100` must remain KB-free.

### Qualification recorded before the final boundary repairs

The integrated source has passed the full native suite, all 76 lint gates, the
full Go race suite against a fresh actual PostgreSQL database, nine focused CMake
tests, focused ASan/UBSan/leak checks, all 77 script tests and both frontend builds.
Frontend coverage increased from 396 to 399 tests with the initial-open/imported
conversation follow-up. Actual Chromium checks cover desktop/mobile/dark,
primary/child replies, source-bound unknown effects, passive archives, verified
imported fragments, reconnect and duplicate delivery. The HTTP fixture checks
that ownership/import completes before SSE headers, foreign owners are refused,
and migration failure emits no successful stream or model/network work. The
primary fixture exercises the real importer separately. Actual PostgreSQL race
fixtures prove that a competing completed checkpoint wins under lock, browse
skips without appending, and model import still refuses the stale replacement.

The complete actual-server integration run passed 170/170 checks on the initial
fidelity slice, before the browser-opening follow-up. It includes all eight
pinned SDK wire/credential configurations, real model/tool/resume turns and both
Codex/Claude daemon-crash/interruption journeys. Those journeys freeze the actual
SDK tree, kill only the owned daemon, restart with the same durable store,
recover/fence the old source, preserve old state, complete a fresh turn, load the
unknown effect into later model context, deny a real mutation once and verify
second-attempt suppression plus replay. This is actual pinned SDK execution
against owned provider fixtures; it does not claim live upstream subscription
or vendor acceptance.

The confirmed boundary repairs are implemented: canonical history, browser replay,
interruption sources and delegate-learning scans page before SQL transport framing.
Only the affected source-complete operations select a bounded 30-second budget;
caller deadlines still clamp it and unrelated operations retain two seconds. Legacy
source reads use bounded UTF-8 chunks without changing global wire or cell limits.
Migration 50 validates a 16 MiB retained-source constraint. The original migration
20 file/checksum stays unchanged; one exact deferred-validation variant permits
pre-limit historical rows to reach migration 50. Both known histories replay, and
unknown checksums still refuse. Whole replies retain their existing frame budget,
including metadata overhead; oversized replies fail instead of truncating source.

A final combined PostgreSQL race run exposed cancellation between the last query
and commit dispatch. Commit errors now trigger one bounded, owner-preserving
rollback attempt without retrying commit or changing its original unknown outcome.
Provider transaction handles use cryptographic per-instance seeds and never wrap;
this removes deterministic handle reuse after restart without changing the wire,
open-transaction ceiling or idle TTL. Entropy failure refuses new leases. Actual
SQL-wire tests cover cancellation, lost commit acknowledgment and stale cleanup
arriving after a replacement provider has opened a new transaction. The legacy
source getters share the bounded 30-second history budget, with caller deadlines
still enforced. Required CI replay now includes all SessionIR, primary-model and
native-live regression fixtures.

The final combined Go race suite now passes against a fresh actual PostgreSQL
database, including the complete Aimee-family, shared DB and PostgreSQL provider
packages. Affected native fixtures also pass normal and ASan/UBSan/leak runs.
The final rebuilt-server integration also passes 170/170 checks, including all
eight pinned SDK configurations and both Codex/Claude crash/interruption journeys.
Its 27 unconfigured-service checks remain explicit skips; Docker-dependent paths
are qualified in CI. All 76 lint gates pass on the integrated repair tree.
Published baseline `bd835cbad` passed all 59 checks, including complete script
regressions and all four deployment tiers. Fresh exact-head CI remains pending
for the final transaction cleanup and deadline repairs. Earlier green commits do
not substitute for that final run.
Do not label the migration or production promotion 100% complete while those
checks or the recorded independent review remain outstanding.

### Current completion matrix

| Requirement | Implemented behavior and evidence | Remaining action |
| --- | --- | --- |
| Canonical primary/delegate/browser execution | Durable canonical history, persisted owner-bound model selection, native request/tool/result paths, separate child history, canonical controls and recovery are implemented. Both pinned Linux SDKs pass actual crash/interruption journeys. | Local integrated gates pass; finish fresh exact-head CI. |
| Honest legacy history on opening | Owner-bound browse preflight, atomic fidelity report/checkpoint or passive archive, and verified chronological imported-base display are implemented. Focused native, PostgreSQL lock-race, 399 frontend tests and actual Chromium pass. | Final rebuilt-server integration passes 170/170; finish fresh exact-head CI. |
| Browser controls and usability | Exact primary/child reply ownership, replay readiness, no duplicate/reconnect resend, denial suppression, readable tool output and responsive composer/preferences are implemented and tested. | Local automated checks pass; finish fresh exact-head CI. The browser is ready for manual validation; a later manual pass is not a reason to stop finishing automated work. |
| KB-free project and learning | Approved held repository identity resolves without a KB index. Canonical terminal/outbox owners publish source-bound configured-test grades and observational delegate learning; next-context reuse is actor/project/source fenced. Actual Docker/restart journeys pass. | Finish fresh exact-head CI, then verify real `.100` capture/reuse after authorized promotion. Leave KB mode/endpoint unset; add no fabricated learning rows. |
| Meaning of learned evidence | Observational tool sequences, configured-test results and ungraded outcomes keep distinct labels. No automatic causal/procedure promotion or semantic task-success claim is inferred. | Preserve those labels during production verification. Synthesis is not a prerequisite for the KB-free capture/reuse path. |
| Linux native coverage | Qualified descriptor-based pinned Codex/Claude model/tool/resume and cgroup/Landlock crash recovery have actual SDK and kernel-backed evidence. Genuine local Claude Max/Opus read/question/reply, permission denial, interrupted recovery and ordinary cache-preserving restart pass on `384683799`; owned `.252` Codex evidence retains its own producer revisions. Unqualified launches remain fail-closed. | Finish fresh final-head CI and current-source owned `.252` deployment qualification; do not generalize to untested profiles or native platforms. |
| macOS, Windows and unsupported profiles | macOS/Windows thin clients are supported server-access build/test targets. They are not native server-execution coverage. Arbitrary daemon ACP profiles and non-Linux native tool runtimes remain explicitly unsupported/refused. | Report these boundaries honestly; do not convert thin-client passes into native-server claims. |
| Production review and promotion | `.100` is KB-free and no promotion is claimed. `OWNERS.md` requires Governance to assign/record a second qualified GitHub identity in the live ruleset; the approving reviewer must differ from the author. Agent reviews do not satisfy it. | Obtain the recorded independent review for the final head, retain live-control evidence, deploy through the supported path and verify `.100` without adding a KB. |

The sections below are chronological checkpoints. Their older statements that a
path is missing describe that checkpoint, not an additional current defect when
the matrix and current source establish the cutover. Dead compatibility APIs and
unsupported profiles are not working-runtime coverage. The supported-path audit
found no additional confirmed reachable raw-provider history/tool bypass. Fresh
results must still qualify every later change.

The user confirmed #3038 and its follow-up #3047 merged and requested the remaining
migration and standalone learning work. #3050 was subsequently merged at
`4e0ca19c2` (merge `7e17d58be`). Continuation commits remain on
`fix/browser-ir-native-execution`; its original base was merged #3047 (`fedfa6728`).
The user authorized one follow-up PR after #3050 merged: active work is now
[PR #3052](https://github.com/RakuenSoftware/aimee/pull/3052), targeting testing.
Keep further migration work in #3052. Do not reopen #3036 or mix in WOL work.
Automatic review and the strict attribution gate remain enabled.

The existing browser GUI now has canonical primary loading/model switching,
durable-log history and standard controls. Request/response streaming, delegate
history, tool catalogs/role admission and all three tool-enabled host request
profiles use IR. The Codex browser/delegate launch sites now use the host-owned
canonical scheduler for qualified Linux executable-descriptor launches; this code
has not been promoted to production. Its host model source, original tool calls,
result projection, scoped cache claims and durable-terminal checkpointing replace
SDK conversation observations. The next local continuation also activates Claude
through a source-bound MCP bridge: original host calls, rather than SDK-normalized
arguments, supply tool effects. Both Linux native SDKs pass actual-server model,
tool, durable replay and resumed-turn journeys across all three provider wires.
Its full local regression, sanitizer and pinned-SDK PostgreSQL integration
qualification passed, together with all 76 lint gates. Fresh CI follows publication. Native
launch/platform coverage beyond the qualified Linux executables remains unfinished.

The remaining work includes full native launch/platform coverage, broader real
host-effect/human-control journeys, live Claude subscription qualification and
other unsupported credential custody, remaining ingress/consumer retirement and
independently graded server-local learning with next-run reuse. Production `.100`
must remain KB-free. Explicit receipt-bound feedback/ungraded observations are not
independent task grades or synthesis. Production promotion requires the recorded
independent reviewer under `OWNERS.md`.

Published `c77cc0492` completed all 50 main CI jobs, including all Docker tiers
and identity mint, with two unchanged testing-target platform skips. Export,
audit, fuzz, release and strict attribution workflows also passed. Published
`118f06bb2` adds host-turn generation/source claims and actual-catalog admission;
it subsequently completed all 50 main CI jobs and every auxiliary workflow,
with the same two platform skips. Original-call readback/claim is published
as `a94bd79ea`; it completed all 50 main CI jobs and every auxiliary workflow,
with the same two platform skips. Supervised original-call execution is published
as `2d49add9b`; it completed all 50 main CI jobs and every auxiliary workflow,
with the same two platform skips. These commits do not activate native SDK execution.

Installed CLI probes correctly resolve `aimee` and `wol-lore` from their own
workspaces without an explicit project override; newer native callback cwd binding
is separately pending promotion. The overall migration remains unfinished.

Published `135897989` passed the full native suite, focused ASan/UBSan, Go race tests,
actual PostgreSQL/SDK/browser integration (168 configured checks), 30 proxy tests, CMake cache
fixture, all 76 lint gates and the full-history secret scan. Fresh Windows CI then exposed a
thin-client CMake link defect: the codec target was declared only inside the full-runtime guard.
Published `5fd04df5a` moves that existing portable target into common configuration. Fresh local
thin build/selfcheck and the full-runtime CMake cache fixture pass. That commit subsequently
completed all 59 CI checks, including Windows, macOS, strict attribution and every deployment
tier (T1, T2, T2-luks, T3), with no platform skip or attribution exemption.

## Working continuation: browser replay and KB-free durable verification

Published `c5661a2b4` completed all 59 CI checks, without platform skips or attribution
exemptions. The current continuation remains in PR #3052 and is under fresh qualification.
Three parallel agents handled browser races, scoped recall and the isolated executor while
the primary integrated their production owners. No production promotion has occurred.

Browser model/persona requests and failed-save rollback are now session-bound. Controls have
session-specific state; malformed streams withdraw stale controls and offer durable-cursor
reconnection. A reproduced Send/Steer race came from copied activity state; the composer now
uses canonical activity directly. Repeated Chromium journeys, readable tool-result contrast,
mobile layout, 324 frontend tests and production builds pass. Failed primary/persona lookups
remain errors, focus/visibility refreshes retain confirmed selections and reject stale reads,
and the UI hides unsupported branching instead of offering a silent 501 action.

Configured Git verification now reaches the native host tool catalog through the same owned
Git dispatcher. A qualified run uses a held independent Git snapshot and exact Docker Unix
resource, actual image/profile and immutable exec IDs, machine-observed normal exits, and
acknowledged joined cleanup. Before prerequisites, an actual worker exec must read the exact
random lease marker from the independent snapshot; matching inspect paths alone cannot qualify
source custody. The helper watches its actual server parent and joins cancellation/cleanup if
that parent disappears. Persistent owner identity and per-worker locks let startup cleanup
remove acknowledged same-owner orphan IDs while preserving live and foreign workers. Private
workers stay outside the ordinary delegate prefix/age reaper. Prerequisites run in that worker. No parent-control socket,
network, cached pass, skipped command or opaque process status grants a grade. A proven
preflight refusal with zero configured attempts can continue ordinary verification ungraded;
unknown/started execution cannot be replayed that way. Incomplete configuration cannot qualify.

The capture owner prepares source-bound receipts immediately before completion. Host-only
DB1 operation 36 commits them inside the same canonical terminal event; ordinary append
cannot introduce privileged receipts. Learning validates the typed terminal and proof, and
host-only operation 61 stores an immutable configured-test grade whose complete proof must
match that durable source. Replay rebuilds lost derived grades from the terminal. Operation 62
returns whole bounded receipts only for the authenticated actor and registered project;
primary/delegate context revalidates them through the existing provider-send fence. They are
historical configured-test evidence, not semantic task correctness or causal procedure claims.

The next working increment binds actual selected delegate/job/project metadata to a protected
canonical parent reference before child execution. Ordinary append cannot forge that binding.
Host-only operations 63/64 resolve the exact owned child terminal and project source-bound
pending delegate observations; proof-less exit remains ungraded, and retained proofs require
all matching derived grades before projection. Parent-only replay recovers lost child grades
and observations idempotently, using exact terminal lookup rather than scanning a bounded
prefix. Recall rechecks parent, child, job and action sources through the same freshness fence.
No observation is auto-applied or promoted to causal/procedure success. The project prerequisite
is an approved held workspace and local repository identity, not a KB or code-index service.
Production remains KB-free; no endpoint, seeded learning rows or synthetic successes are added.
The separate legacy goal-only failed-approach injection into write delegates is retired: its
query had no principal/project/source fence. The bypass gate protects this boundary; legacy
rows are not guessed into scoped owners or promoted to trusted procedure learning.

The actual-daemon integration journey now requires real Docker in CI and covers model-invoked
verification, durable proof/grade publication, erased-projection replay recovery and later scoped
model context. Local absence of Docker skips exactly that journey and is not positive evidence.
Published `aee506721` adds the browser/durable verification slice; `9667034f5` records the
exact native verifier integration in the frozen semantic-context gate. Smoke and Linux LSP pass
on that repair. Actual Docker CI then caught a missing native `git_verify` advert: its dispatch
existed but the canonical catalog exposes the multiplexed `git` schema. The working repair
projects a fixed named command only from an actually advertised enum entry and preserves the
remaining schema and cached catalog. The source-bound delegate increment and this catalog
repair are not yet CI-qualified or promoted.
Published `1ec815d36` fixes that advert and publishes source-bound delegate
observations. Its actual-daemon CI journey then exposed a separate harness
omission: the migrated Git process was never installed or started, leaving
verification hashes and snapshots unavailable. The continuation installs its
real generated serving/outbound grants and manages it with the other module
children. A Docker-independent actual-daemon check now verifies configured
commands and both clean/dirty source observations, so local Docker absence
cannot hide a missing Git owner. This grants no private-execution grade;
the separate mandatory actual-Docker journey still qualifies that boundary.
Published `b1c3820ab` passes the new clean/dirty check, but its Docker grade journey
still declines private admission. Learning's project producer was incorrectly
using the index-name resolver, which refuses a valid repository when the KB
index is empty. A dedicated learning resolver now obtains approved held workspace
custody and the repository's canonical identity without consulting KB/index
names. Verification, protected delegate binding and delegate recall share it;
local repository scope retains the existing `local:UUID` spelling. General index
name resolution is unchanged. Focused normal/sanitizer and actual Docker CI
qualification must follow this repair; no production promotion is claimed.
Published `ea7ed891b` removes the KB prerequisite, but its next Docker run
still declined admission. A production-reader reproduction traced this to the
fixture's persisted UUID file: default `0644` was rejected by the existing
owner-only permission contract. Published `4396de22d` creates it exclusively as
`0600` and logs bounded admission reasons. The same production reader accepts
both direct and held-descriptor paths after that correction. A negative-only
local daemon probe now passes identity admission; it has no real Docker and does
not qualify a grade. Its fresh CI then passed private proof/grade publication
but failed later fresh-session reuse: ordinary primary turns only assembled
local history when explicit code-evidence obligations were present. Published
`d734db8e6` adds a local-grade-only assembly path for ordinary turns, retaining
actor/project scope, integrity, and provider-send source freshness without KB
reads. Normal/sanitizer regression reproduces the old failure and passes the
repair. Actual-daemon CI is rerunning.

The unpublished integrated continuation adds mobile footer controls, authenticated
source-bound historical approach reads, canonical session search, and append-atomic
learning delivery tickets with a daemon-owned recovery worker. Search explicitly
separates current reduced history from retained observations, binds scroll selectors
to actor/session/view/source revision, searches joined visible blocks and bounds
whole-session result pages. It refuses classified/tool-input leakage and stale
selectors; unfinished sessions cannot poison discovery. Combined real PostgreSQL
race checks pass for search, protected proofs, delegate recovery and outbox fencing.
Normal/sanitizer/CMake recovery fixtures pass. Full native checks and mandatory
actual-Docker daemon-restart qualification are still being completed. Migration 47
records an explicit automatic-delivery cutover; earlier missing projections require
explicit replay, and intentional retention is not silently undone.

An integrated pre-spawn native cache lease now covers the exact persistent SDK
directory before HOME/configuration/spawn, and remains owned through durable
terminal publication. Crash markers block unsafe restart even if an escaped
descendant has closed every lease descriptor. Normal, sanitizer, and CMake
fixtures pass. This deliberately does not clear abandoned claims; qualified
whole-tree containment and atomic abandoned-turn reconciliation remain in work.
See `docs/NATIVE_CACHE_OWNERSHIP.md`.

Three parallel workers continue independent integration/review, native cache custody
and remaining ingress retirement. The original active-project bug also has a working
repair for valid local identity with an empty index. None of this is a claim of
complete migration, live vendor qualification or production promotion.

Fresh CI, remaining live-vendor/native-profile qualification and independently reviewed production
promotion remain required. The Windows native tool-runtime stub and arbitrary daemon ACP profiles
remain explicit unsupported profiles; thin-client CI does not qualify them as native servers.

Cron, all-failed voting and commit-message output now release owned canonical results. The unused
native-format compactor and its canonical-runtime calls are retired; adjacent canonical source
classifications remain distinct. Do not stop at publication or claim the complete migration is done.

## Active continuation: compatibility native results and fallback retirement

Codex and ACP compatibility executors now retain admitted typed model observations rather than
reconstructing output from joined text. ACP thought/text/media groups survive ownership transfer;
Codex stream prefixes reconcile with immutable complete snapshots without duplication. Incomplete,
contradictory or unidentified observations fail capture, including without a session sink. Nested
capture restores its parent's failure latch. Successful standalone adapters must return admitted
canonical output; the host scheduler remains an intermediate stage until its factory installs the
original response. ACP terminal stop reasons and supported legacy terminal content are decoded at
the native translation boundary.

All four registered adapters already have explicit execution owners. The unused generic child
collector/text-parser fallback is retired; an adapter without a canonical executor refuses before
spawn. The bypass gate rejects reintroduction of those APIs. Published `7f288afff` completed all 59 fresh CI checks, including both thin-client platforms,
strict attribution and all deployment tiers. Its full native regression, focused sanitizers,
CMake targets, actual PostgreSQL/SDK integration and all 76 lint gates passed.

ACP client requests now enter canonical translation before any resource or control action. The
transport performs no filesystem I/O: exact namespaced native resource arguments go to an explicitly
configured resource owner, and its typed result is captured before the RPC reply. Missing owners,
foreign native sessions, duplicate request IDs and failed capture refuse effects. Permission replies
consume the authenticated control owner and can select only a one-time grant. Initialize advertises
only resources actually owned. Dynamic framing preserves opaque session IDs and complete prompts;
nonblocking pipe writes and reads honor cancellation, source liveness and execution budgets.

The daemon always consults the canonical native execution owner. Unsupported native profiles refuse
instead of launching an SDK with ambient credentials/history. A weak standalone seam is eligible only
without canonical history, capture, authenticated principal or request credentials. Private fresh ACP
protocol execution remains available to explicitly configured standalone resource owners; arbitrary
ACP daemon model/credential profiles are not qualified by that compatibility path.

Live subprocess fixtures exercise authenticated permission selection, rejection of another actor,
resource intents/results, a 2 KiB opaque session ID, a 200 KiB prompt and bounded blocked stdin.
Independent local task grading/reuse and remaining deployment qualification still require completion.
Continue those after publication rather than ending the task.

## Active continuation: local learning evidence and legacy injection retirement

ACP owner cutover is published as `af0943cf2`. Its full native regression, focused ASan/UBSan,
CMake targets, actual PostgreSQL/SDK integration (168 configured checks), static analysis, full-history
secret scan and all 76 default-invocation lint gates passed. Fresh CI remains under observation.

The current learning repair keeps delegate process completion distinct from correctness. Missing
mutation receipts remain unknown; opaque failures no longer become successful or turn-budget
classifications. Pending exit observations have automatic application disabled. The old role-only
prompt injection operation is a wire tombstone and cannot read or inject other owners' unreviewed
lessons. Canonical memory remains the path for scoped reviewed procedure context. The integration
fixture now requires real retained delegate observations while checking both subsequent prompts
exclude ungraded exit lessons. This is not independent task grading or qualified positive reuse.
Those remain required before completion.

## Active continuation: owned results, exact-request cache and workspace hooks

The current implementation retains full owned canonical responses in internal agent results.
HTTP and host-owned native producers transfer the admitted response; selection/repair preserve
model and source identity. Explicit text extraction is an output boundary. Lifecycle consumers
release typed storage together with its display projection, including error-only responses.
The response snapshot codec preserves opaque sidecars separately from durable session JSON.
Optional pure-text result caching binds authenticated owner, role, configured provider/model and
exact admitted request. Captured/user sessions, opaque credentials and fallback lineage execute
normally; legacy flat cache rows cannot hit. Reuse creates no fresh provider-health success or
correctness evidence. Tests cover ownership separation, source invalidation and typed round trips.

Native hook policy probes and memory interception now acquire the approved workspace resource,
then restore the enclosing scope. Unknown/malformed Git worktree state forces verification rather
than admitting old verification state. The Go wire explicitly emits `dirty:false` for a clean tree.
Windows/macOS thin-client jobs no longer skip executable testing-target PRs; fresh CI must confirm
those platforms. This does not qualify native server execution on those operating systems.

The old process-success-to-KB-`fixes` feedback writer is retired. The canonical local terminal
observer remains ungraded. Remaining compatibility native producers, independent local grading
and next-run learning reuse, live subscription/platform qualification, and independent production
promotion are still required. Do not stop continuation after publishing this checkpoint.

## Merged #3038 validation and replacement history: 18 October 2026

The user authorized replacement of #3036 to satisfy the permanent attribution gate, then
explicitly requested its retirement. Active work is now on
[#3038](https://github.com/RakuenSoftware/aimee/pull/3038), branch
`fix/browser-ir-runtime-personal`, targeting `testing`. It contains all of #3036's migration
work. Its published checkpoint `07548c7fe` passes all 60 checks, including strict attribution,
full integration, sanitizers and deployment tiers T1, T2, T2-luks and T3. Integration passes
156/156 in CI, including real Docker delegate completion/readback (25 other service-dependent
checks skipped). The earlier note calling all four deployment tiers green on `6b8d720c0`
was incorrect: the job tool returned only its first page. Full check enumeration confirms
all 60 pass on `07548c7fe`.

#3036 is closed, its body is empty, and `fix/browser-ir-runtime` was deleted locally and
remotely. GitHub retains the closed PR record. All older instructions below naming #3036
as the active PR/branch are historical and superseded. Do not reopen it, restore its branch,
or create another replacement for that closed PR. The historical #3038 continuation instruction
was superseded when the user confirmed its merge; automatic reviews and strict attribution
checks remain enabled.

- Canonical primary loading/model switching, durable-log history, standard browser controls, native hook admission, compaction and several credential/consumer cutovers are implemented. The whole migration remains unfinished, especially credential-free native execution, scoped native tools and account-bound replay/cache proof.
- The user explicitly confirmed that production .100 should NOT use a KB instance right now. Unset KB mode/endpoint are intentional. Do not configure/connect a KB or enable a dependency on one to fix learning. Repair and qualify server-local learning, failed approaches and authenticated task-outcome capture through their existing owners.
- The active-project CLI repair is installed and was exercised from wol-lore. Keep the completed WOL PR separate; no WOL content belongs in #3038.
- Strict opening-app attribution remains enforced; #3038 passes without an exemption. Production promotion still needs the recorded independent review required by OWNERS.md.

## Replacement CI repairs: 18 October 2026

- The delegate learning journey now owns a registered `shared` server checkout,
  separate from the earlier mirror journey's client-side path. The integration
  Make target builds the actual `aimee-delegate-egress` sandbox owner as a prerequisite.
  Real Docker completion/readback remains required where Docker is available.
- MR-18's frozen `main_test.go` input was reviewed: removing only event 6147
  from the expected Learning registry reproduces the previous frozen SHA-256 exactly
  (`09438b193d19bc2551759b538037f4da20604a707f04d2ff4f30e1f34a59a725`).
  The refreshed input hash admits the new canonical execution-outcome stage;
  no invariant cases, groups, assertions, thresholds or denominators changed.

## Historical 17 October startup instructions (superseded)


- Repository: https://github.com/RakuenSoftware/aimee
- **Keep all further work in the existing PR #3036**: https://github.com/RakuenSoftware/aimee/pull/3036
- Branch: `fix/browser-ir-runtime`; target: `testing`.
- **Historical 17 October pushed and verified commit: `3f44879630c02206bd908870e4b192b86ea54d2c`.** Its Git tree is `2f63314f5ad5d95da0a7d8cbb2adddffbe4672b2`, exactly equal to the tested staged tree.
- Commit parents: previous published head `9bc8d682cb626e8afefdb57df50319d4a1279d15`, plus `testing` at `096aaf6a5d7eeeb4632d5127d8df72078bab281a`. The latter contains merged #3033, #3034, #3035 and #3037.
- Raw Git author and committer were checked through GitHub: `JBailes <jbailes@gmail.com>`. No AI attribution trailer was added.
- **The full IR migration remains unfinished.** The pushed change cuts over the delegate execution loop and history; it does not finish every ingress, session owner, control channel, or internal consumer.
- Do not spend the overnight session merely watching CI. Fix actual failures, but continue the substantive migration described below. Do not create additional PRs or claim completion after another foundation slice.

Suggested local startup, using an isolated worktree if the existing checkout has work:

```bash
git fetch origin fix/browser-ir-runtime testing
git worktree add -b local/aimee-ir-overnight ../aimee-ir-overnight origin/fix/browser-ir-runtime
cd ../aimee-ir-overnight
git log -1 --oneline
git status --short
```

If that local branch or directory already exists, choose another local name; **do not make another remote PR branch**. Push completed commits to `origin fix/browser-ir-runtime`, checking for concurrent changes first. Read the checkout's current `OWNERS.md`, `CONTRIBUTING.md`, and applicable `AGENTS.md` before edits.

## What the user actually wants

The existing Aimee web GUI should let the user work from any machine in the house, using the server's same terminal/environment, tools, delegates and models as their desktop CLI. They authorize full environment/terminal access. They do not want a new IDE, another GUI, or a separate web proxy proposal. Refer to the Go service as the **web backend**.

Their architectural rule is explicit: **convert native model or harness output into typed IR at its entry into Aimee; downstream execution, history, persistence, replay and webchat should consume IR. Render native wire formats only at explicit output boundaries.** Scraped terminal screens are not a valid structured entry adapter.

Claude should use headless `claude -p --output-format stream-json --include-partial-messages` instead of tmux capture. The user accepted headless tradeoffs and asked about caching; preserve native session continuation and prompt-cache semantics. Keys/OAuth belong to Vault. Credentials must not be exposed as model/browser content; reuse Vault custody and existing typed credential/egress paths.

One PR only. Previous assistants repeatedly opened PRs and claimed partial work was complete; the user explicitly rejected that behavior.

## Existing published browser foundation in #3036

These pieces were already on the branch before the new delegate cutover:

- Server-owned canonical capture in `src/server/server_chat_ir.c`; persist before delivery. Browser disconnect loses a subscriber, not the admitted execution. Storage failure aborts capture/execution.
- Migration 44 introduces the ordered durable canonical session log and append/page operations. Migration 45 stores owner-checked per-harness native resume IDs.
- Claude stream-json, Codex app-server and ACP entry adapters live in `src/modules/translation/harness_ir.c`. Typed content/control requests are emitted at the reader boundary.
- Claude browser primaries use a real child headless runner. Vault-backed ephemeral credential homes retain native resume directories through symlinks to `.claude/projects` and `.codex/sessions`. Codex app-server has OAuth and Aimee MCP configuration.
- Browser requests use `ir_version:1`; replay uses `format=ir&cursor`. The web backend forwards canonical event bytes unchanged. The frontend uses one canonical reducer for live delivery and replay.
- Admitted host turns retain presence/arbitration/workspace leases after the browser disconnects.
- Permission requests are displayed as typed controls. **Interactive browser permission replies are not implemented.**
- Harness capture owns observation for those browser turns; ingress is not also captured for them, avoiding duplicate model turns.
- Legacy unversioned events, native-history storage and other runtime paths still exist. The foundation is not a global cutover.

## What the newly pushed commit changes

### Delegate execution actually retains IR

`src/posix/agent_runtime.c` now receives owned `aimee_response_t` from the native decoders and retains typed blocks through governance, respond normalization, dispatch, history and final-turn capture. There is no `parsed_response_t` projection in the active tool loop and no three-way provider-format history append branch there.

- Full opaque tool IDs are retained; no fixed 16-call execution cap. Admission caps responses at `AIMEE_IR_MAX_BLOCKS` (4096).
- Tool arguments are dynamically allocated views of each typed `tool_input`; original typed JSON remains the source.
- Governance batches the dynamic list through the existing 16-wide registered Go policy provider, applies deletions backwards, and preserves non-tool blocks and typed truncation.
- `delegate_respond` normalization operates on typed blocks. Mixed respond/real-call output drops only respond. Pure respond becomes final text while retaining thinking/media/context.
- History is canonical `{role, blocks}`. One assistant append path and one tool-result append path replace the native-format branches. Host/user text is canonical at construction.
- Tool attempt counts are recorded before dispatch/capture, so a result-publication failure does not falsely imply the tool never ran.
- Local capture/continuity failure is terminal and does not rotate credentials or replay the turn with another provider after a tool may have executed.
- `agent_result_t` still exposes a legacy flat text projection to external/internal callers; replacing all those consumers is unfinished.

### Entry adapters and history import

`src/posix/agent_ir_parse.c` provides owned IR decoding for buffered native responses. It rescues tool calls once at entry, validates admitted assistant content, and publishes buffered lifecycle events. API responses reject forged tool results, invalid roles, duplicate tool IDs, invalid function arguments and unsupported content. Legacy `agent_ir_parse_*` wrappers remain for callers not yet migrated.

**Legacy-history conversion was moved out of POSIX into** `src/modules/translation/history_ir.c`, exported as `aimee_frontend_history_import` in `aimee_frontend.h`:

- Canonical histories are codec-validated and copied.
- Native Anthropic, Chat and Responses histories enter through their frontend adapter and become canonical.
- Mixed canonical/native histories fail.
- Leading legacy system content is lifted out; current host system instructions are supplied separately.
- Chat assistant `reasoning_content` / string `reasoning` is retained as THINKING before visible content.
- Existing cross-protocol fixtures now test all three history imports, tool pairing, canonical reload, reasoning and mixed-shape rejection.

### Canonical IR codec/lifecycle

IR now preserves thinking ID, encrypted reasoning content, redacted state, cache metadata, context metadata, typed error text and full cache/read/write/reasoning usage. New helpers free messages, validate blocks, enumerate/drop tools, allocate response text and render media references.

`aimee_ir_message_parse` is the owned canonical decoder. Tool input is opaque JSON or absent globally, because ACP may provide arbitrary/partial input; the completed API function-call decoder separately requires object arguments.

`aimee_ir_history_repair` tracks ordered tool cycles, supports reused IDs in later cycles, inserts cancelled results for unanswered current calls and removes orphan results without dropping unrelated blocks. It builds a replacement before mutating history.

Buffered response publication is TURN_START -> MESSAGE -> TURN_STOP. Provider failures produce a typed ERROR lifecycle. A thread-local capture-failure latch distinguishes continuity failure from retryable provider errors.

### Translation/egress

- Anthropic request/response parsing shares a block adapter, including signed and redacted thinking.
- Responses SSE extraction belongs to `aimee_backend_responses.c`; the former several-hundred-line parser in `server/agent_bridge.c` is now a compatibility wrapper. It preserves output-index order, completed items omitted by the terminal snapshot, encrypted reasoning, full IDs, CRLF/multiline framing, refusal and incomplete/error outcomes. Missing terminal events and unknown content fail rather than flattening to empty text.
- Responses egress preserves ordered text/media/tool uses/results and structured tool results, requests encrypted reasoning, and retains native reasoning IDs. Foreign opaque thinking is retained internally without fabricating a native Responses reasoning ID.
- OpenAI Chat parses image/file parts and preserves media at egress. Chat egress does not yet replay THINKING as provider-specific `reasoning_content`; check compatible-provider needs rather than inventing universal OpenAI fields.
- Anthropic egress retains URL/media distinctions and opaque data-URI handling. **Its general cache-normalization policy still strips/replaces client breakpoints; the requested same-protocol raw fast path is not implemented yet.**
- All three response parsers/renderers handle typed errors; truncation/stop reason from merged #3033 remains intact.

### Canonical request/history and compaction

`agent_build_request_ir` in `server/agent_bridge.c` imports the native provider profile/catalog into IR, replaces its messages from canonical history and renders at egress. Existing profile-specific knobs remain in the output profile. This is an active delegate cutover, not deletion of every legacy builder elsewhere.

C compaction recognizes canonical blocks, protects complete user/system/developer messages, keeps signed reasoning/tool cycles in the retained tail, and folds evidence segments around protected messages. It builds the replacement before mutation and keeps mixed protected result messages intact. The merge preserves #3034's separate native-history cut alignment fix.

Go economizer folding/compression/repair recognizes canonical blocks and produces canonical summaries. **The Go record builder was also fixed** (`module_record.go`) to extract canonical text and tool-result evidence, avoid roleless-reasoning-as-result classification, and preserve paths found in results. Shared typed Go IR across all modules remains unfinished.

### Browser reducer correction

`frontend/src/ir/session.ts`: a full message snapshot received inside an already-started model turn stops its blocks but preserves the previous completion state. TURN_STOP owns completion/usage. Standalone messages complete immediately; a snapshot after TURN_STOP stays complete. This fixes the observed replay error, “Delta follows message completion”. Metadata and usage tests cover live/replay parity.

### Capture failures do not replay tools

`AGENT_RC_CAPTURE_FAILED` is a local refusal alongside context refusal. Dispatch, provider fallback, credential retry and browser execution stop on it and do not penalize provider health or cool down credentials. The simple completion parser in `server/agent_runtime.c` also uses the owned IR decoders instead of three handwritten native parsers.

## Validation actually completed

All **13 focused native binaries passed**. After incorporating testing's #3034/#3035, all were rebuilt/rerun successfully (the final nine were rerun immediately before handoff):

```
agent-context-refusal
delegate-credentials
agent-error-retryable
responses-parity
ir-session-event
ir-crossproto-egress
session-compact
harness-ir
server-chat-ir
response-governance-stage
agent-runtime-messages
agent-ir-parse
delegate-driver
```

Important coverage includes a real 17-tool turn with long opaque IDs, capture failure before/after dispatch, no credential/provider replay, ordered Responses SSE with incomplete/empty terminal snapshots, native history conversion and tool-cycle-preserving compaction.

- Frontend: **259 tests across 32 files**, plus `tsc --noEmit`, passed after the reducer fix.
- Go: `-race ./modules/aimee/families ./modules/economizer -count=1` passed; economizer race tests passed again after the canonical record-builder fix.
- Python attribution/identity: **11 tests passed**, including the literal single-asterisk footer reported by the user.
- `git diff --check` and clang-format 19 dry-run over changed C/header files passed.
- Ownership/module gates passed: source ownership, header layout, docs, inventory, test registration, descriptor sources, placement, emitter linkage, Go package ownership and module bus boundary. The new history codec include is explicitly recorded as a shared IR contract, not an undocumented peer service call.
- ASan/UBSan **four binaries passed**: IR session codec/events, cross-protocol egress, governance and Responses parity. These sanitizer runs preceded the final helper relocation/history-import fixture and latest testing merge; rerun locally with the final tree.
- Do **not** claim the larger end-to-end ASan targets passed: this environment's non-LTO sanitizer build exposed unresolved existing test linkage (DB1/tool effect-contract symbols) for agent-context/delegate-driver. Standard builds of those targets pass.
- Fresh CI for `3f448796...` has **not** been inspected. Prior published-head CI mostly passed, but that is not evidence for the newly pushed tree.
- No live Vault-backed deployment/subscription/native-resume acceptance test has been completed. PostgreSQL integration ran in CI for the earlier foundation; rerun current CI locally where possible.

## Highest-priority next work / concrete unresolved seams

1. **Primary history switching now needs canonical ownership.** Newly merged #3035's `primary_session_adapter.c` adopts another provider's history through `aimee_ir_convert_history` in `aimee_ir_serve.c`, which expects native arrays and renders them back to native wire. The new delegate loop now returns canonical histories. Inspect/fix this immediately: primary state, load, adoption and compaction should import once through `aimee_frontend_history_import` and keep canonical history regardless of current provider. Do not serialize canonical blocks as though they were native `content`/Responses items. Add a real primary-switch/reload fixture; there was no existing `test_primary_session_adapter.c` found.
2. **Durable session log must become the source of history**, not just a browser observation stream beside `primary_sessions`. All native/CLI primaries and delegates need the same server-owned ordered capture/replay path. Current thread-local sink does not automatically propagate into worker threads; do not borrow an HTTP socket context. Bind a log-only server owner to attested session/principal/execution scope using existing typed APIs.
3. Preserve scope for child delegates. Their private model conversation should not be dumped into the parent's prompt. Parent receives the delegate TOOL_RESULT; the UI may expand child execution scope. Session identity belongs in the transport envelope, not model deltas.
4. **Governance/log consistency remains unresolved:** entry capture sees the original model response, while governance/respond normalization mutates execution history. A denied tool use may appear in the session log without a paired denial result. The canonical log/projection design must represent the decision without losing the original event or silently creating a second history source.
5. **Interactive browser controls are unfinished.** Implement Claude/Codex permission/question replies through their documented native control channel, with owner-checked server pending IDs and correlated canonical CONTROL_RESPONSE events. Do not let a model's own ingress/MCP token approve its request. Claude currently writes a plain prompt and closes stdin; supported bidirectional stream-json input/control is needed. Verify exact schemas against official primary documentation before implementing.
6. **Audit CLI ingress/Vault integration.** Desktop `cli_proxy_launch` in `src/cli_proxy.c:528` sets Aimee base URLs and Codex's Responses provider. Browser Codex currently selects `model_provider="openai"`; headless CLI credential homes currently contain Vault-materialized subscription credentials and go directly upstream, with IR supplied by harness adapters. Determine whether the existing ingress can serve Vault-held subscription OAuth and reuse the desktop route/session binding. Do not invent a web proxy. Verify credential/tool custody rather than assuming an ephemeral HOME alone prevents a CLI tool reading credentials.
7. Remove remaining tmux execution paths globally; browser headless is done, but nonbrowser `AGENT_BACKEND_TMUX_CLI`, `agent_runtime_tmux.c` and related paths remain.
8. **C compaction fallback evidence reader** `msg_text` in `src/server/session_compact.c` currently reads first TEXT, not TOOL_RESULT.content. Go's record path was fixed, but inspect the fallback flashback/summary path for canonical tool-evidence loss and add a meaningful fixture. Avoid speculative changes; verify actual readers.

## Full remaining migration backlog from the user's audit

Do not narrow the task to just the above immediate bugs:

- Activate `ingress_preinject_anthropic_enabled` through IR **after** implementing a cache-preserving raw fast path; do not delete the setting or silently lose client cache breakpoints.
- Make IR response parse and stream relay the actual default, delete `anthropic_stream_feed_openai`, `driver->parse_response` and silent legacy response fallbacks once the IR path is complete.
- Remove `build_provider_body`, `driver->build_request` and direct `translate_request` paths; routing/policy must read IR rather than the translated raw request.
- Remove Responses -> Chat -> IR and Chat-format round trips that lose tool choice, reasoning and structured output.
- Implement real mutation tracking and same-protocol raw passthrough preserving exact bytes/cache metadata, then remove the separate Anthropic passthrough/native relay.
- Move tool policing/router/model pin and economizer/memory/governance onto typed IR; supply shared typed Go IR over the bus rather than extending per-format branches indefinitely.
- Add typed reasoning effort, structured output and hosted-tool choice fields. Opaque `thinking`, metadata and service-tier sidecars are still transitional.
- Remove residual `parsed_response_t`/legacy delegate registry/native tool-shape handling from unmigrated consumers. The delegate loop is cut over; the whole repository is not.
- Replace flat `agent_result_t` for its many internal callers; hand parser removal in simple completion is only part of this.
- Rebuild Responses `previous_response_id` history from canonical session history, replacing the flattened in-process text store.
- Trajectory export should preserve typed blocks and tool IDs. KB curator `provider_client.c` and Claude/Codex hook payloads must also enter IR.
- Migrate old native-array and tmux-text transcripts with explicit fidelity reporting; lost screen structure cannot be recreated honestly.
- Fix the 1 MiB history-load ceiling (`src/db1_client/sessions.c`, `db1_primary_session_load`) which returns empty on oversize. Check related list/load buffers and protocol limits rather than merely raising one malloc.
- Add the IR bypass checker requested in `ir-sole-path-residual.md`, with exact justified exceptions/retirement, covering the identified native-shape strings in C/Go. Do not rubber-stamp all current bypasses.
- Update README, STATUS, WHAT'S NEW and the pending canonical session proposal truthfully as cutovers complete.
- Preserve reload/live parity, tool ID pairing, controls, ownership, session switching and cache behavior across all models.

## Attribution CI policy: required and intentionally strict

The existing `no-coauthor-trailers` check was strengthened in #3036. It must reject all of:

- Raw Git author **or committer** identities such as `Claude <noreply@anthropic.com>` (the user's example was PR #3033 commit `2c18a5309bd1506622149658e9d3390d0c0fecc5`).
- Generated-by footers, including the exact single-asterisk Claude Code footer reported on #3034. A regression fixture was added this turn; it passes.
- The opening PR issue's structured `performed_via_github_app` attribution to Claude or ChatGPT Codex Connector, which produces GitHub's “with …” label. **Read the issues endpoint**, not just `/pulls`; missing/malformed metadata fails closed.

Files: `scripts/check-pr-attribution.py`, `scripts/check-commit-identities.py`, the existing workflow job, related Python tests, `CONTRIBUTING.md` guidance.

**#3036 itself has the disallowed Codex Connector opening attribution and is expected to fail this stronger gate.** Body edits do not remove the structured attribution. Do not exempt this PR, weaken the rule, falsify metadata or open another PR. This is a remaining human/policy-resolution blocker; surface it accurately after completing reviewable implementation.

Why #3034 previously passed despite its footer: the stronger gate was only in unmerged #3036; #3034 still ran the old generated-with-only check. #3034 is now merged into testing. Do not rewrite other PRs just to apply an unmerged policy.

## Cloud session details, only if needed

- Cloud checkout: `/workspace/scratch/7f86ecc3b843/aimee`.
- After publication, local HEAD was synchronized to `3f44879630c02206bd908870e4b192b86ea54d2c` by a soft reset after verifying exact tree equality. The tracked worktree/index is clean; the unrelated benchmark file remains untracked. The PR title/body was updated to distinguish completed work from the remaining migration.
- Before publication local HEAD was merge `f2b659909f722e7fa42abfc0a37569eaf913499a`, with **62 staged files, 3609 insertions / 1688 deletions**. The GitHub commit was recreated from the identical tree with the two published parents above. Local refs/index may need synchronization; verify tree equality before any soft reset. Do not hard-reset away edits.
- Unrelated untracked file must not be staged/deleted: `benchmarks/results/gemma4-unified/ab-v1/gemma4_e2b/.raw_gemma4_e2b.jsonl.ngLKJV`.
- Two retained stash backups: `ir-delegate-before-testing-3034-3035` and `IR delegate and canonical history migration in progress`. Do not pop them over the pushed code; they duplicate older WIP and can reintroduce conflicts.
- Do **not** regenerate `tests/baselines/refactor/index.json`. Module-test registration already passes with its current baseline.
- Initial 2.5 MB GitHub tree upload hung and was interrupted. Publication succeeded by sequential smaller tree batches; the final tree SHA exactly matched local Git. A single commit and lease-checked ref update then succeeded. No new PR was created.
- If local Git auth works, use ordinary commits/pushes under the verified human identity. Cloud shell Git had no write credential, so GitHub Git-data API was used. Never print credentials or add assistant author/trailer metadata.
- No subagents were used; the session's developer rules prohibit spawning them unless the user or an applicable repo instruction explicitly requests delegation.

### Cloud-only test tooling (local machine should use its own normal dependencies)

```
Go: /tmp/go/bin/go and gofmt (1.26.9)
clang-format 19: /tmp/aimee-clang19/clang_format/data/bin/clang-format
dependency headers: /tmp/aimee-test-deps/root/usr/include
dependency libs: /tmp/aimee-test-deps/root/usr/lib/x86_64-linux-gnu
standard object/test dirs: /tmp/aimee-ir-build, /tmp/aimee-ir-tests
sanitizer dirs: /tmp/aimee-delegate-ir-asan, /tmp/aimee-delegate-ir-asan-tests
```

Cloud native narrow build pattern:

```bash
make -s -j2 -C src \
  OBJDIR=/tmp/aimee-ir-build TESTPREFIX=/tmp/aimee-ir-tests \
  EXTRA_C_FLAGS='-I/tmp/aimee-test-deps/root/usr/include -DAIMEE_DISABLE_ZSTD' \
  TEST_L_FLAGS='-L/tmp/aimee-test-deps/root/usr/lib/x86_64-linux-gnu -lsqlite3 -lm -lpthread /tmp/aimee-ir-build/libaimee-core-connection.a -lssl -lcrypto -lz' \
  /tmp/aimee-ir-tests/unit-test-agent-context-refusal
```

`AIMEE_DISABLE_ZSTD` was only needed because this environment lacks zstd development headers. PostgreSQL could not start here due to chown/setgroups restrictions, and Unix-socket tests have environment limits; run them on the local system. Do not bypass those restrictions. Do not edit C sources while a Make using the same object directory is running.

Sanitizer experiments needed `--gc-sections`, `-fsanitize=address,undefined`, `-fno-omit-frame-pointer`, and nonfatal existing GCC `format-truncation`/`restrict` warnings at -O1. Runs used `ASAN_OPTIONS=detect_leaks=0` due to environment constraints. Fix actual new defects, not unrelated existing warnings simply to get this cloud build through.

## Overnight success criteria

Continue implementing the user's canonical-IR design in #3036. Start with canonical primary load/switch and the log/history source-of-truth seam, then native controls and ingress/cache-preserving cutover. Keep a precise checklist of what is removed versus still transitional. Run meaningful tests and the repository's required gates, fix CI defects, and push to the same branch. Do not report the entire task complete while interactive controls, ingress bypasses, native history ownership or other agreed consumers remain unfinished.

## Local continuation progress (18 October 2026)

Implementation continues in #3036 on `fix/browser-ir-runtime`. The migration remains unfinished.

- Primary model switching/reload now consumes canonical history. Completed primary histories are
  owner-checked atomic checkpoints in the ordered durable session log. Legacy provider rows are
  import-only. Related transcript reads use bounded dynamic transport frames instead of the old
  1 MiB empty-on-oversize behavior.
- Compaction fallback reads typed tool results as evidence. Same-protocol untouched Anthropic
  replay retains client cache markers and TTLs; unreported typed mutations prevent stale replay.
- Browser permission/question replies use a server pending registry bound to principal/session/turn.
  Only authenticated browser transport can enqueue decisions. Claude uses bidirectional stream-json
  stdin; Codex consumes correlated operator replies. Both persist control responses before delivery.
- Main-checkout CLI index discovery now bootstraps missing registry scope using the existing bounded
  ingest path and resolves the canonical registered project; linked-worktree scope is retained.
- Local frontend: 264 tests and TypeScript passed. Native server builds. Focused address/undefined
  sanitizer fixtures, including replay and the real Codex child control exchange, passed with
  leak detection enabled. Full native agent tests pass under `/var/tmp`; this host has `/tmp/.git`,
  so the isolation test correctly treats `/tmp` scratch directories as repository descendants.
- Local PostgreSQL live race fixtures cover checkpoint atomicity, ownership, replay and child-scope
  privacy. Web backend race tests cover authenticated reply forwarding and cross-origin refusal.
- CMake configuration still fails on existing deleted source references (starting with `_learn.c`).
  Make is the working native build. Do not describe CMake or deployment qualification as passed.

Still required: log-only capture owners for every nonbrowser primary/delegate worker, governance
and observation pairing, remaining ingress/cache and native replay cutovers, tmux retirement,
remaining typed IR consumers, bypass enforcement, and full integration/deployment qualification.
The opening attribution on #3036 remains rejected by the strengthened gate; no exemption was added.

### Additional local execution/history validation

- `agent_dispatch_one` now installs a log-only owner for attested native primaries without an
  existing capture and gives delegates private durable child sessions. Workers use copied typed
  attestation; no HTTP context is shared. Durable job IDs select stable child history across attempts.
- The tool loop loads/checkpoints that same durable canonical history. A prior unfinished host
  execution requires reconciliation; only the caller's server-owned active turn may be excluded
  from its own history load. This prevents restart from silently replaying an uncertain execution.
- Governance emits an explicit error TOOL_RESULT for each removed opaque call ID, including calls
  beyond the old mask width. Host-consumed synthetic final-answer calls also receive paired results.
- Configured integration passed 153/153 checks with 25 full-stack-service skips. Deployment compose
  environment tests passed, including the real emitter. HTTP reply tests reject model bearer/TCP
  spoofing; native pending ownership, consume-once and child subprocess fixtures cover correlation.
- New log owner and governance fixtures passed address/undefined sanitizers with leak detection.
  The all-family Go race suite passes against a fresh isolated PostgreSQL database; sharing it with
  the integration schema invalidates older tests' global table-count assumptions.
- Fresh CI on `a08f76473` passed lint and exposed link dependency omissions for native HTTP/control
  and a newly activated cold CLI fixture, plus a Windows unused-variable warning. Those are fixed
  locally and need fresh CI. Secret scanning reported the OpenAPI input SHA-256 as an API key;
  `.gitleaksignore` records that single commit/file/rule/line finding and its digest-only rationale.

Remaining scope includes child execution references/UI expansion, all ingress/cache and native
replay cutovers, tmux retirement, remaining typed consumers and bypass enforcement. Live Vault-backed
browser qualification and full-stack deployment checks remain required. The attribution gate is unchanged.

### Continuation: native replay and stream fidelity (18 October)

The next worktree phase preserves authored Responses message groups without inserting separator
blocks into model history. Native Anthropic edits preserve client cache breakpoints, TTLs and unchanged
native extensions. A strict wire codec retains the exact original frame when same-protocol semantic
comparison succeeds, including whitespace and numeric spellings; actual ingress attachment of this
byte sidecar remains unfinished.

Canonical stream deltas now carry thinking signatures, redacted state and initial tool inputs.
OpenAI final usage arriving after finish_reason is retained. The incremental Chat-to-Anthropic ingress
no longer executes its legacy stream translator, and premature EOF produces an error.

Fresh b6dec512c CI passed native/PG suites, Go, frontend, sanitizers and Windows. The complete script
suite found a missing ownership entry for session_ir_history_live_test.go (corrected). Three Docker
matrix jobs stopped in native asynchronous primary recovery. Investigation reproduced lost attested
identity: HTTP captured it only after early streaming routes returned. Capture now precedes those
handoffs. Generic OpenAI/Anthropic/OpenRouter and local compatible HTTP models also need native primary
adapter registration; they previously fell into the role-confined delegate route. A disposable local
server plus PostgreSQL and synthetic provider now completes PRIMARY_OK after these fixes. Fresh Docker
CI must still qualify the repaired path. The attribution gate remains intentionally strict and failing.

The full migration is still incomplete. Global headless execution/tmux retirement, credential custody,
remaining raw request/governance and response consumers, MCP elicitation forms and full Vault-backed
browser/resume acceptance remain open. Do not interpret the passing focused checks as completion.

### Continuation: non-browser Claude entry capture

The registered Claude provider adapter now runs the same incremental headless reader as browser
Claude, rather than collecting the entire subprocess output and decoding it afterward. Its timeout
honors the configured workflow cap. A real subprocess fixture verifies that rejected live capture
terminates the child before its later completion marker is written. Standalone native assistant
snapshots also supply the legacy text projection from the owned decoded response.

External harnesses do not produce a native-loop checkpoint array: their persisted canonical events
are the history source. The native wrapper no longer tries to checkpoint a NULL array after a
successful harness run. Conversely, a log owner cannot report success without a completed canonical
model response. Capture failures remain sticky when a nested owner restores a NULL parent sink.
Global tmux retirement, scoped runtime/resume unification and credential custody still remain.

The complete local script suite passed all 69 tests. All 75 lint checks passed for 6edd36846.
Fresh CI found two old fixtures whose contracts changed: default native cache preservation requires
explicit stable-prefix policy in the cross-protocol normalization test; a stream ending with [DONE]
requires a preceding native finish_reason. Both fixtures now test those explicit contracts.

### Continuation: native HTTP entry now uses canonical replay

The Anthropic HTTP provider builder now admits native requests through typed IR before egress.
The immutable original body is attached at the real ingress, rather than only in codec fixtures.
Independent typed/raw comparison selects exact replay: a handler-level fixture preserves whitespace,
an unknown large integer spelling, explicit stream:false and a one-hour cache TTL byte for byte.
Host stream-mode edits rebuild the native request while retaining untouched cache markers and forms.

The existing ingress_preinject_anthropic_enabled option now invokes the same canonical stage catalog
as translated requests. With the option off, canonical admission and exact replay remain active.
A native stage fixture verifies that opt-in assembly runs context/tools once, recalls the original
query and preserves its client cache TTL after persona insertion. Raw foreign-ingress policy stages,
legacy translated-request fallbacks and response projections still remain to be cut over.

### Cache guidance and worker reuse follow-up

Host tool/completion guidance is appended as a separate native system block before canonical admission; original client system blocks, cache breakpoints and TTLs survive. A new independent task clears a previous ownerless capture-failure latch, while nested work retains the active owner's failure. IR session, HTTP owner and native Anthropic handler fixtures passed with ASan/UBSan and leak detection. Global migration work remains open.

### Native harness retirement follow-up

The tmux model-execution engine (`agent_runtime_tmux.c`) and detached Claude delegate shortcut are removed. Historical backend spellings normalize to structured provider adapters, including old Claude aliases. Provider registration now creates headless records. Routing no longer gives the retired backend spelling affinity over canonical-history peers. Interactive PTY session support is separate terminal functionality; it does not produce model history.

Server-hosted native CLI executions prepare Vault-backed ephemeral homes using the same helper as browser execution. Conversation directories and owner-checked native handles use the current private canonical execution identity; sibling delegate sessions do not share native history. Claude binds its stream initialization handle; Codex resumes/starts and binds the native thread before sending a turn. Credential teardown preserves conversation files. Codex line storage now belongs to its process instance; the compaction reader no longer frees a borrowed line.

Admitted, attested legacy chat subscribers now capture canonical events too, independently of transport version. Their existing event projection remains available. Unattached/unattested ingress and remaining internal consumers still require the broader audit; this does not declare a global cutover complete.

Validation: full native agent suite (including a real child launched from a legacy tmux record), native CLI/session owner fixtures, provider Go race tests, all 75 lint gates, and disposable server/PostgreSQL primary execution passed. Focused ASan/UBSan checks cover native CLI adapters, private runtime homes and canonical owners. CI at published 34f5be6f3 passed Docker T1 and T3; T2 variants were still running at this checkpoint. The memory replay job failed a PageRank fixture deadline, while its canonical IR session replay step passed. The opening-app attribution failure remains unexempted.

### Foreign request admission and installed scope repair

Anthropic ingress now builds foreign provider bodies exclusively through canonical IR; the legacy driver body builder and shadow build are removed. Invalid typed admission fails before network send even with `AIMEE_IR_PATH=0`; the compatibility predicate is permanently enabled. Three native HTTP fixtures and ASan/UBSan pass against the real request seam. Raw routing/model-pin/tool-policing stages, response projections and other ingress paths still require cutover.

The fixed CLI from published 19d839003 is installed at the registered `/home/virant/.local/bin/aimee` via a separate `releases/ir3036-19d839003/aimee` binary. The original `releases/1.0.2/aimee` is preserved. The reported investigate query was rerun through that registered path after explicitly entering wol-lore: status ok, project wol-lore, four hits. Local install evidence/previous target is `/tmp/aimee-ir-cli-install.json`. The fresh CI native failure was an old environment-dependent tmux-affinity assertion in test_agent_caps; it now checks the canonical primary choice unconditionally.

### Typed buffered response and completion cutover

Anthropic ingress now admits completed native responses through the shared owned IR wire decoder, applies typed response governance/completion, and renders only at the client boundary. The legacy parsed-response bridge and response flag are removed. Completed-response admission rejects unfinished frames, trailing junk, forged results and duplicate calls; numeric usage is checked before conversion. Native signed/redacted thinking, media, cache usage and more than sixteen opaque tool calls survive buffered and synthesized streaming output. Unsigned foreign reasoning remains canonical and is explicitly reported when the Anthropic projection omits it.

Ten focused ASan/UBSan binaries passed with leak detection; the final stricter terminal-frame and dead-bridge changes passed five rebuilt standard binaries. All 75 lint gates ran, with only the expected generated-document comparison failing before committing the regenerated configuration table. Native Anthropic raw streaming, other ingress/policy consumers, native signature prefix binding after host compaction, MCP elicitation and credential custody remain unfinished. CI at 68ec8a34e passed both native shards, all PostgreSQL native shards, integration, frontend and Go tests; deployment/sanitizer jobs were still pending when inspected. The attribution gate remains strict and fails on the PR opening app.

### Native streaming relay cutover

The separate Anthropic raw-byte relay and observation-only reasoning parser are removed. Native Anthropic SSE now enters canonical deltas before persistence and client rendering. The same durable-before-delivery rule also applies to the OpenAI incremental relay. Native signatures, redacted blocks, initial and incremental tool input, message identity, cache usage and terminal spelling survive rendering. Admission checks block/message ordering, closed blocks, terminal reasons, safe counters and complete SSE framing; unsupported content variants and incomplete streams fail explicitly. A failed durable append stops downstream delivery.

Seven standard HTTP/stream/harness binaries passed; five ASan/UBSan binaries passed with leak detection, followed by four rebuilt sanitizer binaries after terminal-spelling preservation. All 75 lint gates passed before that final metadata-only extension. Native harness fixtures now include real final message_delta events before message_stop. Other ingress/policy consumers, thinking-prefix binding after host edits, MCP forms, credential custody and full browser/Vault acceptance remain open.

### MCP form replies and pending-control validation

Codex MCP form elicitations now publish a normalized form and accept typed browser submissions through the native correlated response. The browser renders flat strings, numbers/integers, booleans, single/multiple enums and their defaults/constraints. Unsupported nested forms retain decline/cancel controls. The shared IR contract validates required fields, primitive types, finite/safe numeric bounds, Unicode lengths, enums and array cardinality. The server checks the pending request before storing a reply; invalid answers remain correctable instead of consuming the request. Native rendering validates form data independently. Permission/question replies also reject unrelated payload fields before consumption.

All 270 frontend tests across 34 files and TypeScript checks passed. Five standard native control/harness binaries and three ASan/UBSan binaries passed; normalization was additionally exercised through a real Codex entry frame. All 75 lint gates passed. A freshly built disposable server/PostgreSQL primary turn returned PRIMARY_OK. The full integration suite is running. Thinking-prefix binding after host edits, remaining ingress/policy consumers, credential custody and live browser/Vault acceptance remain open.

### Mandatory OpenAI request admission and stream contract alignment

OpenAI ingress no longer invokes driver request builders when IR admission fails or preserve_messages is set. Preserving messages disables host context edits independently of admission. Both Responses handlers reject failed IR parsing instead of using the legacy direct translator; native framing rejects trailing junk. A new actual-ingress builder fixture asserts that legacy callbacks never run across all three provider wires and both host-edit modes, including malformed roles/content/tools. The remaining Responses-to-Chat projection and flat response consumers still require replacement.

The streaming cutover review caught missing canonical message identities and a Go validator missing the new raw_stop_reason field. Both are corrected: captured deltas now carry stable message identity, the test sink serializes every event through the actual codec, Go admits terminal native spelling, and the browser retains it separately from the canonical stop reason. These were defects in the newly added cutover, not evidence of successful live capture at the earlier checkpoint.

Validation: two rebuilt request fixtures and their ASan/UBSan runs passed; the stream-identity fixture passed with sanitizers. All 270 frontend tests, TypeScript, 75 lint gates and PostgreSQL-backed session IR race tests passed (7.061s). A real attached-session disposable server test switched OpenAI -> Anthropic -> OpenAI -> Anthropic across four calls, captured 48 ordered durable events, retained every prior prompt, and restored the native Anthropic signature on the final switch. This uses local provider fixtures, not live subscription credentials. The earlier full integration rerun passed 153/153 with 25 service-dependent skips after allocating a fresh schema; the first reuse had five admission-cap failures from existing workflow rows. CI at 68ec8a34e finished every technical check including all four Docker tiers; only the unchanged opening-app attribution gate failed.

### Durable browser fidelity reports and regular integration replay coverage

Projection loss now has a typed, durable session event. The browser reports reasoning/opaque-state omissions at the explicit request or response boundary while retaining canonical blocks; these reports never enter model messages. C producers, strict Go validation, the browser reducer and rendering consume the same contract. Request and response boundaries are distinguished so output conversion is not misreported as a model-input edit.

The new src/tests/test_primary_ir_live.py is called by the regular integration suite. It follows the GUI session-primary selection contract (without overriding the provider model ID), verifies A/B/A/B across native OpenAI/Anthropic HTTP fixtures, checks retained prompts/signatures and fidelity metadata, then compares every live envelope with authenticated durable replay and a resumed cursor. Standalone execution passed with four calls and 49 ordered events. Full integration passed 154/154 with 25 service-dependent skips before adding the projection assertion; a fresh full rerun is still needed for that final metadata extension. Three standard/native sanitizer targets, all 271 frontend tests, TypeScript, PostgreSQL session-IR race fixtures and all 75 lint gates passed. Live subscription credentials, thinking-prefix binding after host edits, remaining typed consumers/ingress/policy and child UI still remain open.


## Continuation: 18 October, 04:15 Europe/Amsterdam

The migration remains unfinished. Continue only on PR #3036 / fix/browser-ir-runtime.

- Durable primary replay now has a regular integration fixture, `test_primary_ir_live.py`: attached, authenticated GUI-style model selection A/B/A/B (agent names differ from provider model IDs), every prior prompt retained, native signed reasoning restored, exact live/durable replay parity and cursor replay. Projection decisions are durable typed events displayed in the browser; private delegate conversations stay separate.
- Curator requests now use canonical request fields and provider responses retain owned IR; text/JSON is an explicit application projection. Shared Chat response admission preserves ordered text/media, reasoning and full dynamic tool arrays/IDs. Structured output is typed, including schema/name/description and absent/null/false/true strict states, with explicit Chat, Responses and Anthropic boundaries. Unsupported Anthropic JSON-object and Bedrock structured-output modes refuse rather than silently drop the setting.
- Fresh d0527820f CI passed native/PG shards, Go, integration, replay, sanitizers and static checks, but the frontend production build failed (unknown JSX value and ES2022 Object.hasOwn). All four Docker tiers stopped on that same build failure. Both issues are corrected; local `npm run build` and 271 tests pass. `tsc --noEmit` on the solution root was insufficient evidence: run the actual production build.
- Current server AND KB production builds pass. Four focused ASan/UBSan binaries pass: cross-protocol egress, provider-client, kb-curator-llm and agent-ir-parse. The 75-check lint run is being completed. Rerun fresh full integration and CI after publication.
- Registered CLI active-project detection was rebuilt, installed and tested from wol-lore with a successful indexed investigation. The original release is preserved; installation/rollback metadata is in `/tmp/aimee-ir-cli-install.json`. The server has not been deployed.

Unfinished high-priority work: OpenAI HTTP response/Responses ingress still uses parsed_response_t and Responses-to-Chat-to-IR projections; durable owner-scoped previous_response_id replacement; typed Go bus consumers/economizer/memory, routing/tool policing/model pin; owned agent_result_t consumers; native hook payload/trajectory export; child references and expandable child UI; historical fidelity reporting; IR bypass gate; native signed-thinking prefix binding after compaction or other prefix edits; rebuilt opaque JSON numeric precision; credential custody via existing Vault/CLI ingress; remaining elicitation methods; final documentation, current-tree integration/sanitizers/Docker and live Vault/subscription/native-resume qualification. Do not count structural mock signatures as cryptographic native replay acceptance.

The attribution gate intentionally still rejects the immutable PR-opening app attribution. Do not exempt it or create another PR. An accidentally created unrelated remote docs/current-state-audit ref was reported to the user and exact-lease cleanup remains pending automatic approval; no PR was created for it and its local checkout is preserved.


### OpenAI HTTP response cutover, 18 October 04:30

The structured Chat/Responses HTTP response paths now hold owned `aimee_response_t` through mandatory wire admission, immutable capture, canonical governance/completion and explicit client rendering. `openai_chat.c` no longer invokes `driver->parse_response` or uses `parsed_response_t`. Dynamic Chat and Responses SSE rendering preserves every call rather than the old fixed call array/frame; the fixture checks 17 calls with 511-character IDs and 8 KiB arguments, reasoning/encrypted state, authored message grouping, cache/reasoning usage and incomplete termination. Chat text/media output uses the shared ordered content boundary. Unsupported Responses output media refuses explicitly. Curator and this cutover pass focused native and ASan/UBSan fixtures. Current server and KB builds pass; fresh PostgreSQL integration passes 154/154 (25 service-dependent checks skipped). Curator lint passed all 75 checks; the response-cutover lint rerun is in progress.

Remaining plain text completion/run callers still use `agent_result_t`. Full native request ownership and hosted tools, Responses continuation persistence, typed module consumers and the other unfinished items above remain open. The OpenAI HTTP response cutover does not complete them.


### Native request ownership and durable Responses continuation, 18 October 04:45

Structured Chat and Responses HTTP execution now consumes the complete native request as owned IR. The Responses-to-Chat-to-IR helper and the unused Chat-component provider builders are retired. Request-side OpenAI policing includes typed namespace children; router input is canonical text. Economizer gateway mutation receives canonical messages, and a rejected reduced request can rebuild/resend its restored canonical original once. Structured output and typed reasoning effort survive native request assembly. Tool definitions now have typed function/namespace/hosted kinds, nested function catalogs, strict/deferred-loading settings and explicitly provider-owned configuration. A backend which cannot represent the hosted catalog refuses rather than dropping it or forging an empty function. Forced hosted/allowed-tool choice and hosted execution output events are still unfinished.

The flattened in-process Responses store is deleted. previous_response_id now loads an authenticated, parent-session-scoped canonical checkpoint from the existing durable session log; response ids are random. Unknown, unauthorized or cross-scope ids refuse before provider dispatch. The real HTTP integration fixture exercises 17 native calls with >500-character IDs and 8 KiB arguments, streaming tool-result continuation, retained native signatures, schema/effort settings and cross-scope refusal. Structural signature retention is not cryptographic binding acceptance.

Validation: six focused fixtures pass standard and ASan/UBSan; the twelve other handoff fixtures pass after rebuilding. Current server and KB builds pass; lint passes all 75 checks. Fresh full integration passes 154/154, with 25 service-dependent skips. The standalone primary fixture must start the real memory module (the full integration suite does); absence correctly refuses context assembly. No production server deployment has occurred.

CI at 5dbebcf62: frontend build repaired; technical jobs mostly pass, but memory-owner PostgreSQL replay failed in TestMemoryRuntimeRoleReplay -> exercisePageRankRecallReplay: the full lexical pool/graph query hit the operation deadline. Investigate actual SQL/planning; do not raise a timeout or waive the test. T2-luks was still running at the last check. Attribution still fails on PR-opening app metadata as intended.

For local replay diagnosis pgvector 0.8.7 was checksum-verified from the official PGDG package metadata and unpacked ONLY under /tmp/aimee-ir-pg-deps. The existing PostgreSQL 18 runtime uses unversioned extension directories, so its vector extension files were copied within that temporary prefix. Metadata: /tmp/aimee-ir-pgvector-install.json. UTF8 disposable replay database: aimee_ir_memory_replay_utf8 (the initially created SQL_ASCII database cannot apply the Unicode schema). Schema provisioned from the actual roles/schema/grants files with embed dimension 384. A focused race-enabled runtime-role replay is running, with slow-query logging enabled for that disposable database only.


### Driver interface retirement and native alias routing, 18 October 05:00

The provider vtable no longer exposes build_request or parse_response; every registered callback and its unused legacy implementations/fixture hooks are deleted. Native Anthropic model selection now resolves registered agent aliases (including aimee) to the configured provider model ID while retaining concrete native-model passthrough and untouched client cache sections. Four affected native fixtures pass, three Anthropic fixtures pass ASan/UBSan, and the real HTTP primary/Responses fixture also verifies this Anthropic alias case.

Fresh a79d81986 CI exposed two concrete omissions: unit-test-agent-request-build lacked the newly shared OpenAI frontend render object, and test_economizer_live_surface.sh still searched for the old upstream_is_anthropic local variable. Linkage is repaired in every explicit backend-only target, and the gate checks the actual canonical selected wire. Affected request-build/HTTP/parity fixtures and the ASan request-build target pass locally. Lint rerun passes all 75 checks.

The focused race-enabled PostgreSQL memory runtime-role replay now passes locally in 280 seconds. Its first local failure was a fixture timezone bug: a date-only timestamptz boundary uses Europe/Amsterdam, while stored validity dates normalize to UTC. The boundary is now explicit UTC. An earlier attempted scope reset did NOT solve that failure and was removed; no scope relaxation is included. CI's separate PageRank full-lexical-pool deadline is still unresolved and must not be described as fixed by the timezone change. Investigate the recall query/candidate work rather than increasing the timeout.


### Native request policy cutover, 18 October 05:20

Anthropic Messages ingress now admits the complete native frame into owned IR before tool policing, routing, model pinning, host guidance and economizer mutation. Native wire is an immutable replay sidecar, not the policy/history source. Buffered and streaming execution render the admitted, transformed request once at the selected provider boundary; the request is no longer reparsed after editing native JSON. Count-token passthrough also requires canonical admission and resolves registered aliases to provider model IDs.

Buffered economizer rejection now restores the canonical original and resends once; the former log-only missing-resend path is deleted. Fixtures assert canonical reducer input, exact restored messages, one retry without rerunning policy, refusal of native-shaped reducer output before dispatch, admission before routing/policing, and real policy removal of a denied tool/choice while preserving client cache metadata. Four native fixtures and their ASan/UBSan runs pass; current server/KB build, real A/B/A/B primary plus durable Responses continuation/alias fixture and all 75 lint gates pass.

Fresh fc0164a59 CI exposed a stale frontend fixture that expected an invalid nameless custom tool to survive in a raw sidecar. It now asserts typed function/hosted/namespace ownership and separately asserts refusal of malformed/unknown tools. This fixes that fixture contract; custom free-form execution remains unimplemented and must not be claimed supported. Native thinking-prefix binding, credential custody, remaining owned consumers, child references/UI and the other outstanding migration work remain open.


### Private child references and browser expansion, 18 October 05:25

Delegate owners now durably append typed child_execution references (private session, execution turn, role and lifecycle status) to their attested parent log. Independent workers use copied attestation; nested workers reference their immediate private parent. No child prompts, reasoning or tool transcript enter the parent event or model history. Failure to publish either admission or terminal references propagates as capture failure, including after a child has executed.

The browser reducer retains references separately from messages. Expanding a delegate lazily opens its authenticated canonical session stream and uses the same transcript reducer/rendering; close/unmount cancels the subscriber, terminal completion closes it, and wrong-session delivery/cycles refuse. Parent and child cursors remain independent.

Validation: 274 frontend tests and the production build pass; native codec and concurrent child-log fixtures pass with ASan/UBSan and leak detection, including failed reference publication. PostgreSQL-backed session IR race tests pass (7.035s), covering parent prompt isolation, private child paging and cross-principal refusal. Current server/KB builds and all 75 lint gates pass. A fresh full integration run is in progress with schema ir_integration_child_3036. The other open migration items remain unfinished.


### All completion ingress uses owned IR, 18 October 05:45

Tool-free Chat no longer flattens role-separated messages through agent_dispatch_one. Buffered and streaming Chat share the owned request/response execution path regardless of tool catalog presence. Legacy /v1/completions has a direct typed entry adapter for its text prompt, sampling, stop settings and metadata; it does not construct a Chat-shaped intermediate. Unsupported batches, echo and tools refuse explicitly. Text extraction occurs only at its client output boundary. preserve_messages disables host context edits independently of mandatory admission, including streaming.

Short-window idempotent reuse is retained: its key includes the fully prepared provider payload after canonical economizer mutation, resolved identity/backend and behavior flags. Cache values contain canonical blocks and typed response/usage metadata, with stable client response identity; native sidecars and flattened output JSON are not stored. A cache hit publishes admitted canonical content before delivery; failed persistence refuses reuse. Tool-bearing responses are not cached, and restoration after a rejected reduction disables reuse for that changed request.

Validation: native entry/response/cache fixtures and the existing dedup suite pass, two rebuilt ASan/UBSan fixtures pass with leak detection, server/KB builds pass and all 75 lint gates pass. Real HTTP coverage now checks separate Chat system/user/assistant roles and media plus text-completion sampling/stop settings, both buffered and streaming, alongside A/B/A/B and durable Responses continuation. The full current-tree integration rerun passes 154/154 with 25 service-dependent skips (schema ir_integration_completion_3036). The child-reference tree's separate full integration rerun also passed 154/154.

CI at 523af6d0a passes PostgreSQL knowledge replay (7m30s), Docker T1 and T3; T2/T2-luks were pending at the last check. The unchanged attribution check still rejects the PR-opening app. This is not a waiver or completion of the overall migration: native thinking-prefix binding, Vault custody, typed module consumers, hook/trajectory ingress, remaining hosted/custom tool forms and final live browser/subscription/native-resume qualification remain open.


### Shared Go IR contract and namespace policy, 18 October 06:25

The public data-only server-go/ir package now owns canonical session/history validators and typed request/catalog codecs. Store ownership, authorization and dispatch remain in their registered module owners. The Go package ownership ledger explicitly records this contract; no peer implementation or bus exemption was added. Memory gateway planning now consumes a strict, bounded typed policy view with roles, callable identities and last-user text. Complete schemas, opaque provider options and prompt bodies stay in the owned host request. Legacy flat policy ingress is rejected.

Go policy returns descending namespace paths for shell removal. C validates the complete patch before mutation, prunes emptied namespaces, preserves remaining schemas and MCP tools, and relaxes removed forced choices. An empty catalog clears both choice and parallel-call settings. A real registered-Go fixture covers nested shell functions, hosted local shell and retained MCP/web-search tools. Its memory boundary fingerprint was refreshed with the precise boundary review; this does not assert a separate reviewer's production-promotion approval.

All backend constructors and native replay require canonical admission. Canonical request snapshots now provide the idempotency key source; request/body commitments are SHA-256 digests before bounded key-composition transport. Native frames and duplicate multi-megabyte prompts are no longer transported merely to derive a cache key.

Validation: shared-contract and PostgreSQL-backed session/gateway race tests pass; six focused native fixtures and five ASan/UBSan fixtures pass with leak detection. Mandatory egress admission additionally passes standard and sanitizer tests. All 75 lint checks pass. Current server and KB production builds pass. The integration rerun aborted at its thin-client rebuild because the compiler exhausted a /tmp disk quota; it has not passed for this tree yet. Compiler temporaries are being moved to /var/tmp for the rerun. Never run production builds alongside lint: the provider-registration detector test temporarily removes a real registration call while testing its diagnostic.

The overall migration remains unfinished. Native signed-thinking prefix binding, readable native credential homes, remaining typed consumers and flat results, native hooks/trajectory, opaque JSON numeric precision, remaining hosted/custom/elicitation forms and live browser/Vault/subscription/native-resume qualification remain open. CI at 1ae034d3f has passed the checked technical jobs, with integration and T2-luks still pending at the last inspection; attribution continues to reject the immutable opening-app metadata.


### Opaque JSON numeric fidelity, 18 October 06:30

The typed policy tree at fd48bd88b passes fresh full integration: 154/154, with 25 service-dependent skips. Task-owned compiler/test artifacts were relocated to /var/tmp after /tmp reached its user quota; unrelated files were preserved.

The shared C JSON codec now retains parsed decimal tokens through duplication, references and serialization. Explicit numeric setters invalidate the token, and direct numeric mutation is detected before serialization. Exact decimal comparison distinguishes adjacent large integers rather than merging their rounded double views; equivalent spellings still compare equal. Extreme exponents remain opaque JSON data. Completed tool-argument adapters require full JSON framing. Invalid numeric grammar and embedded NUL strings are rejected rather than re-emitted or truncated. This is a local vendor patch, with no dependency/version substitution. A dedicated number_lexeme field extends the vendored struct; every native consumer must rebuild against the matching header. Numeric nodes retain NULL valuestring, preserving legacy string checks.

The browser canonical entry parser, tool-fragment completion and transcript/control rendering preserve decimals outside JavaScript's roundtrip range. Known numeric contract fields continue to require ordinary valid numbers; opaque values use an immutable exact-decimal value and the canonical serializer, whose native JSON.stringify path refuses accidental conversion. Live and durable replay use the same parser and reducer.

Validation: all 19 rebuilt focused native fixtures pass, including the original handoff fixtures and current primary/log/ingress/policy/cache tests. Five rebuilt ASan/UBSan fixtures pass with leak detection. Tests cover long decimals and adjacent >2^53 values, clone/reference lifetime, mutation, semantic decimal equality, canonical tool/schema rebuild across all three native wires, streamed versus snapshot browser parity and exact visible tool arguments. All 279 frontend tests and the actual production build pass. Current server and KB builds pass. Lint and another full integration run for this numeric-fidelity tree are still required. The other outstanding migration/deployment items remain open.


Numeric-fidelity follow-up: 23 rebuilt native fixtures and five sanitizer fixtures now pass, including the standalone frontend/backend/tool-choice/stream suites. The completed-argument regression exposed a real admission defect: failed Chat/Responses history-argument parsing became an absent input and was accepted. Those API entry adapters now require a complete JSON object, with valid controls and trailing-junk refusal exercised for both requests and responses. The canonical codec still permits arbitrary harness input separately. The current server/KB builds pass; 279 frontend tests and the production build pass. Fresh integration of the dedicated decimal-token storage tree passed 154/154 (25 skips, ir_integration_decimal_3036); the final complete-argument admission extension requires its final integration rerun.

Fresh fd48bd88b CI found missing standalone export inclusion of server-go/ir and a stale bus-conformance caller sending flat gateway roles/tools. The export map now follows the two actual consumers, and aimee explicitly declares its IR contract dependency. The real interop fixture uses typed views and namespace paths. All 20 export regression tests pass (one service-dependent skip); the complete Go/module/C bus-conformance suite passes. The final 75-check lint and conformance rerun is in progress. These are real corrections, not CI waivers.


### URL elicitation and canonical operator replies, 18 October 07:00

63e707bf8 is published on PR #3036. Its final full integration passes 154/154 (25 service-dependent skips, ir_integration_numeric_final_3036), all 75 lint checks pass, and the complete bus-conformance rerun passes. No technical CI failure was reported at the last check of that commit; most jobs were still pending, and immutable opening-app attribution still fails.

Codex URL-mode MCP elicitation now normalizes at native entry into a canonical external-interaction request. The browser shows the requesting server and destination host and sends its owner-correlated consent when the user chooses to open the destination. It never sends secrets/form content through that URL consent response and does not mistake acceptance for completion. Explicit HTTP(S), authority, userinfo/control and byte limits apply; invalid targets retain decline/cancel. Already recorded consent permits reopening the external page. Device-verification/custom form extensions remain unsupported and must not be claimed implemented.

Browser-owned Claude and Codex control replies now publish the actual admitted canonical operator decision before native serialization/delivery, then resolve the adapter's pending identity. Native reply rendering emits no duplicate canonical event and cannot flatten cancellation into denial or replace a question's complete answer arrays with a joined native string in history. The native elicitation observer also normalizes accept/decline/cancel at entry. Nonbrowser native control projections still require the remaining consumer audit.

The real child-process fixtures exercise permission cancellation, questions and URL elicitation with exact native replies, and assert canonical decisions independently. A rejected Claude decision capture prevents native approval delivery. Owner/session/turn binding, invalid-content refusal and consume-once behavior are covered. Five focused standard fixtures pass; 281 frontend tests and the actual production build pass. Five corresponding sanitizer fixtures passed with leak detection before the final URI-case normalization/fixture extension; the final rebuild and rerun, current server/KB rebuild and lint are in progress. The migration and production qualification remain unfinished.

URL-control final local qualification: all five rebuilt native fixtures and all five ASan/UBSan fixtures pass with leak detection, including both actual child runners. The Codex fixture deliberately sends canonical cancel while asserting native decline, proving intent is retained independently. Current server and KB builds pass. The final 75-check lint run is in progress; full integration of this URL-control tree still needs its fresh schema/run.

### Signed native replay fences and durable stream snapshots, 18 October 07:45

The URL-control tree ffeb8da65 passes fresh full integration (154/154, 25 service-dependent skips). Its fresh CI technical jobs pass; the strict attribution gate continues to reject PR #3036's immutable opening-app attribution. No exemption or replacement PR was introduced.

Anthropic native entry now records a SHA-256 prefix and predecessor/current thinking-chain commitment on each canonical thinking block. The fence uses the actual sent system/tools/messages, excludes transport cache markers only at their defined locations, and canonicalizes object ordering and exact decimal spellings for comparison without changing opaque stored values. Ordinary schema/input members named cache_control remain significant. Model/sampling/output metadata remains outside the documented prefix. Vendor signature/account/model verification remains authoritative; this local fence is not a claim of cryptographic vendor acceptance.

At egress, incompatible or historically unbound thinking is omitted from the candidate native projection with a fidelity report. The immutable canonical checkpoint keeps its signatures and opaque state for later compatible replay. Oldest-block removal can retain an unbroken surviving chain; middle gaps cannot acquire new proof. Compatibility edits copy proof from the original admitted native frame rather than binding edited prefixes as original. Wire equality excludes context/fence metadata while retaining actual cache and native scalar-content form, so metadata-only changes can reuse exact original bytes.

Native HTTP execution prepares the response fence before dispatch from the actual rendered request, including restored/fallback attempts. Buffered responses bind before canonical capture. Native Anthropic streaming assembles owned canonical deltas, retains complete signatures/redacted blocks/tool inputs, and appends a bound full-message snapshot before the terminal event. A rejected snapshot prevents client completion. The bounded builder grows fragment buffers geometrically. Scalar native text and structured tool-result transport form survive durable history to avoid changing previously sent prefix values on append.

Canonical JSON ingress now rejects duplicate member names, including opaque nested values, instead of letting C/Go/browser consumers select different values. Shared Go decoding and browser lossless parsing enforce the same ambiguity refusal. The C comparison view never replaces original decimal tokens or source member ordering.

Validation so far: 282 frontend tests and production web build pass; PostgreSQL-backed shared-contract/session/gateway race tests pass. Five focused ASan/UBSan fixtures pass with leak detection, including real native-stream capture refusal and durable signed snapshot replay. Current server/KB build and full integration pass (154/154, 25 skips, ir_integration_binding_3036), including primary A/B/A reload, changed-header omission and subsequent unchanged-checkpoint replay. The broad native suite exposed a direct-parser fixture lacking the now-required actual sent prefix; its setup/capture assertions were corrected and its final rerun is pending. Final lint and sanitizer rebuilds for the small fixture/linkage/compatibility-cache adjustments are pending. Live Vault/subscription/native-resume qualification, readable native credential custody, remaining typed consumers/results/hooks/trajectory, custom/hosted capabilities and production promotion remain unfinished.

Signed-replay final checks: all 23 rebuilt native fixtures, all six rebuilt ASan/UBSan fixtures with leak detection, current server/KB production builds, 282 frontend tests, web production build, PostgreSQL-backed focused Go race tests and all 75 lint checks pass. The memory ownership ledger records the exact host-only replay change and retains its original memory symbols/owner. The integration result above precedes the final duplicate-schema/depth admission extension; a final published-tree integration rerun remains required. Current tests use real sent-request prefixes for native stream and buffered-replay fixtures, not fabricated empty requests that fail before exercising the parser.


### Economizer JSON fidelity follow-up, 18 October 07:50

Signed replay a330bcfc8 is published only on #3036. Its final fresh-schema integration now passes 154/154 (25 skips, ir_integration_binding_final_3036), including the final duplicate-schema/depth admission extension. Eight strict attribution regression tests pass. Fresh CI has started; its complete verdict is pending.

The Go economizer's order-preserving tree was still converting parsed opaque numbers to float64 before serialization/clone, despite native/browser exact-decimal preservation. It now retains original numeric tokens, detects direct numeric replacement, permits opaque extreme exponents, and applies complete framing/duplicate-member/depth/NUL checks. Structural summaries keep actual decimal values instead of rounding identifiers and constraints with historical %g formatting. The parsed-number differential fixture now follows the changed vendored C codec; the constructed-double formatting contract remains separate. Full economizer race tests pass, including clone isolation, adjacent large integer/long decimal/extreme exponent preservation, strict framing and explicit summary values. Legacy native-shape fold/repair branches and typed shared-IR admission remain to be cut over; this change does not claim that broader cutover complete.


### Typed economizer history and compaction records, 18 October 08:00

The economizer's reduce bus stage now requires strict shared canonical message admission before any state load or transform, and validates generated candidates before returning them. Complete envelopes reject unknown/duplicate fields. Provider/mixed arrays and wrong role/block combinations are refused. Original member order and decimal tokens remain in the cache-sensitive internal tree. The public shared contract's standalone export inclusion now follows the actual economizer importer as well as Aimee and Memory; its existing descriptor already declares IR.

Compaction record extraction now consumes typed ir.Message/ir.Block values instead of branching over native content/tool_calls/Responses items. Thinking is excluded. Structured tool evidence retains its actual opaque JSON. User text in a mixed tool-result message retains its quarantined lane instead of inheriting the tool result's lane. Original range, actor/storage and record policy ownership remain unchanged.

Full shared IR/economizer race tests, all 20 standalone-export regressions (one service-dependent skip), native economizer client, ownership checks and complete Go/module/C bus conformance pass. C interop now sends canonical record history. Structural-admission fixtures use canonical tool cycles and make their preconditions hard assertions instead of skips. Remaining native-format branches inside fold/repair algorithms are still present and are the next deletion; public ingress cannot select those branches. This checkpoint does not claim all remaining consumers or deployment qualification complete.

### Canonical economizer fold and repair cutover, 18 October 08:05

Native message-shape detection, Responses/Chat/Anthropic history branches and silent argument sanitization have been removed from the economizer's fold/compress/repair consumers. Core transforms explicitly refuse native or mixed history, emit canonical evidence messages, preserve unchanged opaque blocks and repair only current canonical tool cycles. Concurrent duplicate call IDs refuse without mutation; IDs reused in later cycles remain legal. Dynamic repair retains complete long IDs/names and groups larger than the retired 16-call API.

Recall injection was a remaining live native writer: its assistant notice used content instead of blocks and would fail the new typed candidate admission. It now uses the canonical evidence-message constructor. Tests mutate actual canonical text instead of adding ignored native content fields. Legacy wire-repair fixtures were replaced by canonical cycle, malformed/native refusal, grouped results, opaque/signature preservation, 40-call long-identity and idempotence cases. Provider-specific proof planners remain native egress evidence consumers, separate from history transforms.

Shared IR, economizer and Aimee-family race suites pass. Complete Go/module/C bus conformance and all 75 lint checks pass. The latest Go multicall artifact is rebuilt. Fresh integration of the complete economizer consumer tree remains required. Credential custody and live native-resume qualification, dynamic rescue/flat results, hooks/trajectory and the other remaining migration items are still open; no production promotion or separate reviewer approval is asserted.

### Canonical dynamic rescue and strict string fidelity, 18 October 08:30

The complete economizer tree aa075a66d is published only on #3036 and passes fresh integration (154/154, 25 service-dependent skips, ir_integration_economizer_3036). Its technical CI jobs were still finishing at the last inspection; attribution remains unchanged and failing on immutable opening-app metadata. Local Docker deployment tests cannot run because the Docker executable is absent; CI Docker qualification remains necessary.

Delegate rescue now returns an owned canonical assistant message over its existing stage. Request framing is explicitly v2 and v1 is refused; core and the delegates multicall artifact must be upgraded together. The fixed binary reply, parsed_response_t bridge, 16-call cap and short-name truncation are removed from this path. The Go dialect owner retains the inventory decision. Complete JSON object arguments are required; malformed/trailing/duplicate arguments refuse recovery rather than becoming {}. Missing arguments may still represent an explicitly empty argument object. XML/Qwen parameter-text conversion retains its existing dialect meaning without truncating a JSON-looking prefix away from trailing text. Dynamic overflow refuses the entire candidate. C prepares and validates every recovered block before mutating any source content, preserves source context/cache-breakpoint metadata, and leaves thinking/non-text blocks untouched.

The delegate entry JSON reader now retains exact decimal tokens and Unicode scalar values, instead of a float64/manual-surrogate roundtrip. Shared Go canonical decoding and the browser parser/serializer also refuse embedded NUL and unpaired surrogate spellings that native strings cannot represent. Valid astral Unicode and exact numeric arguments survive. The new public header imports only the shared owned IR data type; that exact crossing is recorded in the existing IR_SHARED_TYPE ledger, with no peer-service or attribution exemption. Memory transport symbols and policy ownership are unchanged; the source fingerprint records the rescue-only boundary review, not independent production approval.

Validation: complete delegates/economizer/shared-IR race suites pass; focused PostgreSQL-backed IR/session/gateway/rescue contracts pass across five packages. All 283 frontend tests and the production web build pass. Three native fixtures and all three corresponding ASan/UBSan fixtures pass with leak detection. Cases cover a 40-call group, long names/IDs, exact large/decimal/extreme values, atomic refusal and malformed argument framing. The real C-to-Go stage fixture retains a 200-byte name and >2^53 input and refuses retired framing. Complete Go/module/C bus conformance, all 20 standalone export regressions (one service-dependent skip), current server/KB builds, 25 boundary-regression tests and all 75 lint checks pass. Final paired-artifact integration and fresh CI for this rescue checkpoint are still required.

Remaining audit concern: server_session_log qualifies tool_id inside serialized MESSAGE/DELTA payloads for UI correlation. Message/session/turn identity belongs in the transport envelope; inspect durable native replay and move UI correlation out of canonical model IDs. Primary checkpoints may preserve original IDs, but observations must not silently rewrite native tool identity. Do not claim the full migration complete until that seam and the earlier remaining backlog are resolved.

### Native tool identity survives durable observation, 18 October 08:40

Rescue ea1bde20d is published only on #3036. Its paired-artifact full integration passes 154/154 (25 service-dependent skips, ir_integration_rescue_3036). Fresh CI is running; the attribution failure remains unchanged.

The log-only owner no longer qualifies tool_id inside canonical MESSAGE or DELTA payloads. Tool IDs, inputs, signatures and opaque values remain what the native adapter admitted. Event and message identities still belong to the transport envelope. Native IDs may recur in later model cycles, so UI correlation now derives a separate occurrence key from projected message identity/block position and execution scope. Repeated cycles and interleaved turns retain separate output cards. A unique outstanding call can receive an external result in a following request; unrelated/ambiguous results remain visible rather than being hidden or attached to an arbitrary call. These view keys never enter model history.

The C logger fixture exercises original tool IDs through buffered messages, streamed deltas, a full snapshot and repeated result cycles, while retaining signatures and exact input values. It passes normally and under ASan/UBSan with leak detection. All 287 frontend tests and the production web build pass, including live/replay equality, interleaved same-ID turns, cross-turn results, repeated cycle cards and visible ambiguous evidence. Current server/KB builds pass. Final lint/integration and publication of this identity checkpoint remain pending. Historical native fidelity still needs explicit migration/qualification; existing opaque log IDs must not be guessed or stripped heuristically.

Tool-identity final local checks: all 75 lint checks pass. The UI correlation result depends only on ordered canonical messages and occurrence/scope identities; later execution status cannot retrospectively move a recorded result. No schema or history rewrite is introduced. Local test builds use a fixed validation build-id override for object-cache reuse and are not production release artifacts; final deployment must use the exact reviewed CI/source identity.

### Canonical response stages, 18 October morning

Native tool identity checkpoint 99f9111f6 is published on #3036. Its final integration passes 154/154 (25 service-dependent skips, ir_integration_tool_identity_3036). At the latest CI inspection the native unit shards, web/backend checks and Windows builds pass; sanitizer, Docker and some PostgreSQL jobs remain pending. The immutable opening-app attribution failure remains; it is not exempted.

The response registry now carries the owned canonical response type. Governance uses that registry and its existing separately supervised policy provider, applying bounded masks across the complete dynamic IR response. The unused flat governance wrapper, its fixed-call cleanup and weak test stubs are removed. Completion policy/intervention now has only the typed request/response API; its alternate native-history readers and parsed_response_t writer are removed. Existing default/disabled-policy, owner failure, malformed-decision and completion-retry cases now exercise canonical values. Additional cases keep user/tool evidence from becoming edit instructions, preserve thinking and original text, and feed the actual inserted canonical marker into next-turn bounded-policy checks. This does not retire the remaining flat response parser/egress compatibility APIs elsewhere.

All three focused stage/registry fixtures pass; both changed response-stage sanitizer fixtures pass with leak detection. Current server/KB builds, 25 module-boundary regressions and all 75 lint checks pass. The new direct governance include is an exact shared IR data-type crossing; policy authority remains across the bus. No production promotion is asserted.

Credential research: installed codex-cli 0.160.1 can generate an experimental chatgptAuthTokens schema, but that schema explicitly labels this mode FOR OPENAI INTERNAL USE ONLY / DO NOT USE. It has not been adopted as Aimee's credential solution. Native readable credential homes, supported Vault custody and live subscription/native-resume qualification remain unresolved; ephemeral homes alone do not meet that custody requirement. The full migration remains unfinished.

### Canonical trajectory export and private result references, 18 October morning

Response-stage checkpoint d8c608cdf is published only on #3036. Its completed CI passes sanitizers, Docker T1/T2/T2-luks/T3, PostgreSQL knowledge replay and all three PostgreSQL unit shards. The only failing check is the unchanged strict opening-app attribution gate. No exemption, automatic-review disabling or replacement PR was introduced.

Trajectory export now loads the authenticated owner's primary history projection from the durable canonical session log and admits owned aimee_message_t values. It emits aimee.trajectory.v2 with canonical messages, the actual source session and explicit export fidelity. Native tool IDs, namespaces, signatures, replay fences, structured results and exact numeric arguments survive. Unknown/unmodeled input refuses instead of silently disappearing. Ordinary text/arguments/results may be screened by the existing Go memory owner; unsafe identities/signatures or contextual credential assignments refuse export. Field context is retained for scalar and nested structured values without implementing a second C screening policy. Compression records explicit per-message/block omissions and a digest of the screened export value. The immutable log is never rewritten. There is no fabricated success/reward or time-window overlay of other sessions' audit rows.

Agent results now carry an owned canonical_session_id copied from the actual private execution before log-owner teardown. This is a source reference, not a new transcript or borrowed connection. Session-bound work neither reads nor writes the legacy flat role-result cache; non-session internal compatibility remains. Batch export follows that real child reference instead of writing/reconstructing synthetic interaction events. The HTTP export receives its principal explicitly from the authenticated connection. Batch validates the owned parent, binds copied request attestation before loading agents/execution and restores it afterwards. The CLI supplies its active/explicit --session. Batch toolsets use the existing thread-local owner, preserving the calling thread's toolset and the process environment.

Validation: focused native export/batch tests pass against fresh PostgreSQL databases and the actual Go screening process; corresponding ASan/UBSan fixtures pass with leak detection. Cases include exact >2^53 input, Unicode, signed reasoning, structured results, explicit omissions, foreign/missing owners, unavailable/invalid screening, unsafe opaque keys, scalar credential values and nested credential containers. Full agent tests pass (three unchanged store-dependent cases skip without their store fixture), including attestation snapshot/getter/restore. Current server/KB builds pass. Full integration passes 154/154 with 25 service-dependent skips, including real HTTP trajectory export and two repeated cache-eligible summarize tasks with the actual delegates policy owner; each returns a distinct private canonical execution and retains no parent conversation in its export. Final nested-context strengthening is additionally exercised by the rebuilt native/Go-owner and sanitizer fixtures. All 75 lint checks ran; the sole generated OpenAPI ownership fingerprint failure was corrected with an explicit trajectory-only source explanation and its ownership target rerun. Memory API contracts/symbols and memory policy authority are unchanged. Eight attribution regressions pass.

This remains an unfinished migration. Other flat result consumers and legacy response entry/egress compatibility, native hook consumers, the broad bypass gate, remaining hosted/custom capabilities, supported native credential custody and live browser/Vault/subscription/native-resume qualification remain open. No production promotion or independent approving-reviewer identity is asserted.

### Owned native response entry only, 18 October morning

Trajectory checkpoint 042361d5d is published on #3036. Fresh CI caught the same unit-test-cli-argspec failure in native and PostgreSQL shards: the compiled batch marshaller carried the new session_id, while its served argument specification omitted it. The actual served spec now uses the existing limited session fact (including explicit-empty semantics), and differential cases cover overrides, environment/default resolution and empty values. No test expectation or gate is exempted. All 134 shipped specs / 1266 differential samples pass, also under ASan/UBSan; numeric argument parity reports zero mismatches.

The unused agent_ir_parse_json_response, agent_ir_parse_responses and assistant-message rebuild wrappers are removed from the production entry API. Buffered JSON and Responses/SSE entry now expose only the complete owned aimee_response_t decoder surface. There is no fixed-count/name/ID projection or native assistant-history reconstruction at this boundary. Compatibility parser/free stubs are removed from the ingress fixtures. Their golden cases now assert typed blocks, canonical checkpoint identities, complete usage and the actual decoder. Rescue tests explicitly retain prose beside a recovered call; reasoning counters observe the owned response without changing its blocks. The remaining flat parsed_response_t definition still serves legacy output-only helpers elsewhere and must not be mistaken for a completed consumer retirement.

All seven focused standard fixtures and all seven corresponding ASan/UBSan fixtures pass with leak detection: agent-ir-parse, responses-parity, agent-http, agent, anthropic-http, anthropic-http-p2c and anthropic-http-streaming-p2c. The full agent fixture retains its three explicit store-dependent skips. The additional argument-spec sanitizer fixture passes. Current server/KB builds and all 75 lint checks pass. This slice changes no Go wire framing, storage schema or frontend code. Fresh CI for the new published checkpoint still needs inspection. The credential/native-resume/replay and other remaining migration backlog above is unchanged; no production promotion or independent approval is claimed.

### Stop metadata and final-answer normalization, 18 October morning

Owned response-entry checkpoint 426c9da6e is published on #3036. Governance now sends the canonical stop vocabulary to its policy owner, while preserving the original native stop spelling when policy does not change the canonical reason. Final-answer normalization follows the same rule. An actual tool-use to end-turn transition clears the now-invalid native spelling. The obsolete parsed_response_t final-answer helper and its declaration are removed; existing normalization fixtures now exercise owned IR and preserve opaque exact arguments and native IDs.

Both focused native fixtures and both ASan/UBSan fixtures pass with leak detection. Production server/KB builds and memory/module boundary checks pass. This checkpoint does not change Go framing. The governance request still has a 31-byte name cap; replacing that remaining bounded identity projection is required next. All remaining migration, credential, live replay and deployment qualifications above remain open.

### Complete governance identities and single policy owner, 18 October morning

Stop-metadata checkpoint ce5af9b9a is published only on #3036. Fresh CI exposed test_agent_caps relying on the removed transitive agent_protocol include; it now explicitly includes its actual token-budget API. Secret scanning flagged the public OpenAPI input SHA-256 in historical commit 042361d5d (line 2741), verified against api/openapi-server-v1.yaml. Only that exact historical digest finding is recorded in the existing gitleaks fingerprint ledger. The strict opening-app attribution gate is untouched.

Governance v2 transports canonical identity views with complete names and namespaces through strict shared Go IR admission. The previous 31-byte tool-name projection and v1 request codec are removed. Decisions still use bounded masks across every chunk of the dynamic IR response. Native raw stop metadata stays owned by the host. The duplicate C policy handler/parity implementation is retired; the supervised Go module is the sole policy owner. Core and governance artifacts must be upgraded together. Standalone exports include the actual shared IR dependency; an independent exported governance build and policy tests pass.

Go governance/shared-IR race tests, three focused standard native fixtures (stage, production bridge, remaining process handlers), both stage/bridge sanitizer fixtures, and real C-to-Go governance interoperability normally and under ASan/UBSan with leak detection pass. Coverage includes 40 calls, long names/namespaces, malformed/duplicate/unknown/NUL/surrogate/retired framing, cancellation, owner failures and retired replies. All shipped Go/module/C bus conformance passes. All 68 descriptor/export regressions pass with one service-dependent skip. Current production server/KB builds and all 75 lint checks pass. The final explicit-include correction still requires full agent normal/sanitizer checks; paired-artifact integration and fresh CI remain pending. Earlier unresolved custody/native-resume and remaining consumer migration items are not declared complete.

Final correction checks: the complete agent fixture passes normally and under ASan/UBSan with leak detection (three explicit store-dependent skips). The CI-pinned gitleaks 8.30.1 binary was digest-verified and scans the complete current HEAD ancestry with no leaks found after the exact public-digest fingerprint classification. Attribution policy remains unchanged. Fresh paired-artifact integration is running in disposable schema ir_integration_governance_3036.

### Retired direct Anthropic translator, 18 October morning

Governance checkpoint 365387f38 is published only on #3036. Its paired-artifact integration passes 154/154 with 25 service-dependent skips in ir_integration_governance_3036. The first broader native rebuild found two remaining Anthropic test doubles using the retired string-only governance callback; both now consume canonical identity views.

The old Anthropic-to-Chat messages/tools/system translator, direct OpenAI-chunk-to-Anthropic streaming translator and parsed_response_t Anthropic response/replay renderers had no production callers. They and their declarations are removed rather than left as alternate APIs. anthropic_ingress now owns only existing per-request HTTP protocol metadata; content translation uses canonical entry and explicit native output adapters. The replacement ingress fixture tests actual canonical request/response admission and egress: exact native bytes with cache metadata, separate system blocks, image input, mixed user/tool evidence, exact extreme/large numeric arguments, signed thinking, truncation, full usage and 40 long-ID/name calls. Thread-local header isolation/reset is exercised with a second thread. Existing HTTP/streaming fixtures retain their native SSE and policy assertions.

All four focused native fixtures and all four corresponding ASan/UBSan fixtures pass with leak detection: anthropic-ingress, anthropic-http, anthropic-http-p2c and anthropic-http-streaming-p2c. Current production server/KB builds pass. This retirement changes no Go framing or durable schema; the full 75-check lint pass belongs to the preceding governance tree, with focused ownership/include/test-registration gates run for this removal. Remaining flat output compatibility APIs, C compaction native fallbacks, custody and live native-resume qualification are still open.

### Owned C compaction and mechanical bypass gate, 18 October morning

Direct-Anthropic retirement f9c6dab1e is published only on #3036. Its sanitizer/native shards and other technical CI pass at the latest inspection, except the script suite found the newly declared governance bridge missing from the test-registration baseline; that exact Make-registered, non-CTest row is now recorded. All 18 registration regressions pass. Two Docker variants were still pending, and immutable opening-app attribution remains failing. Read-only inspection found no recorded review submissions or inline review threads; independent production approval is not asserted.

C session compaction now admits complete canonical histories into owned messages once, and all text/evidence/signature/protection/pairing consumers read typed blocks. The native format readers, native summary writer, fixed 256-message tail splice and legacy repair/merge path are removed from this consumer. Focused summaries construct canonical values too. Literal structured tool results are evidence; their JSON is not interpreted as native message wire. Repairs and candidate summaries are staged before source mutation. All injected JSON allocation failures refuse without changing the original serialized history. Bounded text copies preserve complete UTF-8 code points. User/system/developer messages and aligned tool cycles survive unchanged. Same-count compaction is accepted only when it shrinks serialized context, fixing rejection of useful compression between protected user instructions. The summary counts every actual tool call, and read-only metrics preserve exact argument tokens.

The new lint-wired IR bypass checker scans production C/Go for retired entry/direct-translation APIs and driver build/parse calls. It also blocks provider-native history fields in the explicitly migrated execution/history owners and Go Memory/Economizer consumers. Native entry/egress adapters remain explicit boundaries. Scope grows with remaining consumer cutovers; this is not a claim that every repository consumer has migrated. Its exception ledger is empty. Any future exception needs an exact file/rule/line/content hash, owner, reason, retirement and expiration; expired, moved or unused entries fail. Six checker regressions cover C/Go mutations, adapter/test boundaries, comments and exception retirement.

Four focused standard and ASan/UBSan fixtures pass with leak detection: session-compact, session-compact-focused, context-engine and full agent (three explicit store-dependent skips). Tests retain the record/prose section contracts and signed-reasoning/tool-cycle cases, and add native/mixed/malformed atomic refusal, the allocation-failure sweep, Unicode boundary checks, a 300-message retained tail with cache metadata, same-count context reduction and 70-call metrics/cap reporting. Current production server/KB builds, 24 checker/registration regressions, eight attribution regressions and all 76 lint checks pass. Fresh paired-artifact integration is running in ir_integration_compaction_3036.

Next confirmed identity seam: the C delegate loop passes tool_name to validation/dispatch and does not consume tool_namespace. Resolve against its advertised catalog; never silently alias a namespaced call to a registered leaf. Native client-owned execution is a separate boundary. Credential homes, supported scoped Vault/native transport and native full-history signed replay qualification remain open, along with the other unfinished consumers above. No production promotion or full-migration completion is claimed.

Compaction final paired-artifact integration passes 154/154 with 25 service-dependent skips in ir_integration_compaction_3036. The native and Go artifacts use governance v2 together. No production release/deployment identity is implied by the local validation build-id override.

### Existing model listener accepts an explicit host transport, 18 October morning

Owned compaction/bypass checkpoint 88e7144d1 is published only on #3036. Its source tree passes integration 154/154 (25 skips), all 76 lint checks, the focused normal/sanitizer fixtures and complete-history secret scan. Fresh CI still has pending sanitizer/script/Docker jobs at the latest inspection. T3 failed the native asynchronous memory outage gate: evidence records only three provider requests before an unavailable refusal, rather than the planned injection after five. The downloaded artifact digest matches the published digest. This is not evidence of a sixth request, and it is not classified as a fixed or waived failure. The fixture now records the actual SIGSTOP ordinal and requires injection after five before crediting the induced-outage check. The existing requirement forbidding a sixth request remains unchanged. Local Docker is absent; fresh deployment CI is required.

The existing CLI model-only listener is separated from client-global authentication. Its bounded parser/workers/token/headers/stream lifecycle remain in cli_proxy.c; CLI configuration and launcher remain in client-only cli_proxy_cmd.c. cli_proxy_start_bound requires an explicit transport callback and immutable owner scope; it reads no global remote configuration or upstream credentials. Nonempty host session binding overrides client correlation headers. The existing CLI entry still requires its existing remote configuration and forwards with its existing authentication. Server can link the shared listener without the client-only wrapper. No new web backend, service or parallel native content serializer is introduced.

The new host-bound native fixture passes normally and under ASan/UBSan with leak detection. It checks independent owners/sessions, cross-token rejection, spoofed-header removal, exact numeric request bytes, an existing FD response handoff and cancellation. All 28 existing thin-client proxy regressions pass, including installed real Codex SDK/mTLS cases. Current server/KB builds and focused ownership/include/registration/bypass checks pass. The server scoped callback, native SDK child configuration, credential-free homes, selected Vault resolution and replay-proof reconciliation are not wired yet; readable runtime credential documents remain unresolved.

Additional confirmed C credential seam to fix with custody: agent_resolve_auth accepts any per-turn Codex token before selecting a registration, then can fall back to the server Codex account for a missing named connection. Extra account headers similarly accept an unbound per-turn account ID. Go providers already refuse named-to-server fallback. Bind token/account/registration snapshots together and preserve actual default-server success paths; do not borrow another connection. OAuth login currently stores the complete vendor document, so account ID must come from the same selected credential source. Research notes with concrete source locations are in /var/tmp/aimee-ir-native-custody-research.md (local, not a deployed artifact).

### Selected Codex connection credentials, 18 October late morning

Shared listener checkpoint 52fead66f is published only on #3036. Fresh CI passes integration, frontend, Go, lint, builds and most platform checks; sanitizers and deployment qualification were still running at inspection. The strict opening-app attribution gate remains enforced and failing. The strengthened T3 fixture records the actual outage injection ordinal; the previous early refusal is not classified as a successful outage test.

C per-turn Codex credentials now bind the selected registration, including across worker snapshots. A different named connection cannot borrow that token/account or the default server refresh credential. The selected registration, rather than a coincident model label, owns explicit Vault lookups. Delegate credential binding follows the same identity and cleans transient buffers on misses as well as success. A bound token without an account cannot borrow an unrelated Vault account header. The unused unscoped token-presence API is removed.

The full native agent fixture passes normally and under ASan/UBSan with leak detection, retaining its three explicit store-dependent skips. New cases cover default-server success, work/personal switching, missing named credentials, account-header isolation, worker snapshot restoration, selected Vault slots and misleading model labels. Current server/KB builds and IR/module/include boundary gates pass. This is a connection-isolation fix, not completion of credential-free native homes or the atomic refresh/document/account cutover. Those and the remaining migration work above remain open; no production promotion or independent reviewer approval is claimed.

### Canonical delegate request policy and fallback, 18 October late morning

Connection-isolation checkpoint 20ee67f47 is published only on #3036. Delegate request policing now consumes owned IR inside request construction, before native rendering. The provider-shape enum/selector and raw request-stage wrapper are retired. Both the initial request and first-turn fallback run the same canonical policy; fallback previously skipped request policing. Changed tools, tool choice and parallel-call settings render from the canonical result, while other established provider-profile knobs remain intact. Primary turns retain their delegation tools. The bypass checker rejects reintroducing the retired shape API, with a mutation regression.

The typed delegate fixture, real three-protocol HTTP/request-shape fixture and full agent fixture pass normally and under ASan/UBSan with leak detection. Actual request builders exercise primary/delegate behavior, selected/fallback models, surviving required choice and removal of the last tool/forced choice without mutating the shared catalog. Full agent retains three explicit store-dependent skips. Production server/KB and IR/module/memory/include gates pass; all six bypass-checker regressions pass. Fresh integration in ir_integration_delegate_policy_retry_3036 passes 154/154 with 25 service-dependent skips.

The first integration run in ir_integration_delegate_policy_3036 recorded 153/154: one MCP memory mutation timed out, and the memory module hung during its intended shutdown. SIGQUIT was used on that task-owned module to obtain a diagnostic; the visible stack was waiting in runModuleClient's admitted-job drain. Its transient directory was then cleaned by the harness. The successful fresh-schema rerun does not explain this intermittent service failure; retain /var/tmp/aimee-ir-delegate-canonical-policy-integration.log for follow-up rather than claiming it fixed.

52fead66f's T3 job was canceled when the next checkpoint was pushed. Its digest-verified deploy-T3-verdicts artifact contains four completed groups (605 checks, all passing), but no native-async verdict. It does not qualify the strengthened native outage test. Allow 20ee67f47's CI to finish before the next push. Native credential custody/replay, exact catalog-bound dispatch and remaining consumers above are still open. No production promotion or independent approval is claimed.

### Exact advertised tool identities and owner-classified outcomes, 18 October afternoon

All technical CI for published 20ee67f47 passes, including ASan/UBSan, full script regressions, integration, frontend/Go/platform checks and Docker T1/T2/T2-luks/T3. The strengthened native outage fixture is included in that passing T3 run. The sole failure is the unchanged strict opening-app attribution gate. Canonical delegate-policy checkpoint b47834846 is committed locally; publish it and this checkpoint together only to fix/browser-ir-runtime / #3036.

The runtime now retains the owned post-policy catalog from actual request construction, including replacement on model fallback. Exact function lookup treats names and namespaces as separate opaque identities, excludes hosted tools and refuses ambiguity. Argument coercion uses the advertised canonical schema. A call absent from that request cannot reach validators, policy or local dispatch by matching a leaf name. No synthetic namespace alias is introduced. The current local catalog is flat; namespaced external-client capabilities are not fabricated as local bindings. Namespaced respond calls also cannot become the host's final answer or receive a synthetic host-final result. Mixed local/foreign respond output preserves the foreign call.

Live qualification exposed another existing consumer bug: a structured governed-action refusal was marked successful because it did not start with error:. The dispatcher now carries its tool/receipt owner's semantic failure flag separately from text. Canonical tool-result is_error, history, evidence counting and liveness use that flag, including bootstrap evidence calls. Unconfirmed action receipts remain failures; confirmed prior receipts retain their owned state. Effect completion/audit follows the receipt outcome. Matching script-RPC and tool.execute consumers use the same classification; the explicit HTTP output includes is_error. Native exec-envelope classification remains at the tools owner and moved into its existing effect-contract source to satisfy the file-length gate. The memory host-context API and authority are unchanged; only the exact changed dispatcher fingerprint and source rationale are updated in the ownership ledger.

The integration harness now runs the real tools resource owner alongside the real Go execution-policy owner. Its new browser-primary fixture proves a real advertised read succeeds, a foreign namespace and unknown function produce durable errors without reading the fixture, live scoped call/result IDs pair, and native replay retains the original call ID. The explicit tools HTTP bridge exposes both successful and failed outcomes. The fixture failure log is retained in bounded output before temporary-directory cleanup. An initial fixture assertion incorrectly expected the raw native ID in live events; inspection of the real durable log confirmed the existing host-scoped ID contract, which the corrected assertions now check alongside native replay.

Seven focused fixtures pass normally and under ASan/UBSan with leak detection: IR, agent HTTP, delegate driver, agent context/refusal, full agent, governed-action owner and script runner. The affected four fixtures were rebuilt/rerun after outcome-helper relocation and the additional matching RPC fixes. Full agent retains three explicit store-dependent skips. Current server/KB builds pass. Fresh integration ir_integration_catalog_final_3036 passes 154/154 with 25 service-dependent skips; earlier successful owner-backed integration ir_integration_catalog_owner_3036 also passes. All 76 lint checks ran; the sole file-limit failure was fixed through owner-local code relocation, and affected line/include/module/memory/API gates pass. Seven mutation regressions pass for the IR bypass checker, which now rejects unclassified dispatch at the three migrated consumers. No broad exception or attribution exemption is added.

This is still unfinished. Supported native credential-free homes/transport, paired selected-connection OAuth account/refresh lineage, account-bound native resume/cache proof, native MCP actor/workspace qualification and the remaining consumers/capabilities listed above remain required. /var/tmp/aimee-ir-native-custody-research.md records the next seams and explicitly distinguishes design candidates from implementation. The earlier intermittent memory MCP timeout/shutdown-drain hang remains unexplained despite subsequent passing runs. No production promotion or independent approving-reviewer identity is claimed.

### Paired Codex credentials and login lineage, 18 October afternoon

- Serving now obtains bearer and extra headers through one `agent_resolve_auth_headers` call. The separate public resolvers are retired and covered by the bypass gate. Buffered/streaming HTTP, completion and delegate callers use the combined boundary; delegate credential setup no longer pins separately read Vault fields on reusable workers.
- The Vault file owner reads token/account/native-document/source marker from one immutable encrypted-file snapshot. All output buffers clear on any read/decrypt failure. Instance credentials remain on their existing local owner even when tenant credentials use another backend; unsupported snapshot backends fail explicitly rather than pretending separate reads are atomic.
- Under the existing cross-process refresh lock (separate named-connection locks), a changed native login resets normalized credentials and commits its semantic source digest before refreshing. An unchanged source preserves rotated refresh/access credentials, including metadata/formatting-only changes. Removed native sources cannot resurrect a normalized cache. Named connections cannot borrow the server account. Malformed, duplicate, mixed-shape, oversized and header-injection credential fields fail closed.
- The existing attested private Vault helper returns the selected token/account/source lineage together; Go provider diagnostics use that single refresh-aware response and never fall back to loose slots or stale native documents. Credentials stay off the module bus/model/browser content. Generic OAuth storage now records an unknown expiry as zero, preventing a new login from inheriting the previous token's expiry. Refresh already uses stdin; investigation disproved the suspected argv transport leak.
- Validation: nine focused native binaries passed normally (agent, three Anthropic HTTP fixtures, delegate credentials, Vault store/service, OAuth PKCE/reauth); six passed with ASAN/UBSAN and leak detection (agent, delegate credentials, Vault store/service, OAuth PKCE/reauth). The agent fixture reports three explicit store-dependent skips. Real encrypted-file concurrent replacement and service audit/clearing cases pass; selected-account, re-authentication, refresh rotation, source removal, cross-process locking and configured-header regressions pass. Providers Go race tests and server/KB/module builds pass. Fresh-schema integration `ir_integration_codex_pair_3036` passed 154/154, with 25 explicit service skips. All 76 lint checks ran; the only failure was stale exact memory-boundary hashes, subsequently reviewed and corrected with unchanged memory authority and a passing ownership check. Final originator-header correction passed both agent variants.
- The prior published `d88016646` technical CI passed, including T1/T2/T2-luks/T3 deployment regressions. The strict opening-app attribution gate still fails and is not exempted. No GitHub reviews or review threads were recorded at the latest check; production promotion is still unqualified. Native credential-free custody, account-scoped resume/cache proof, remaining IR consumers and live qualification remain unfinished; this checkpoint does not complete the migration.

### Flat response compatibility retired, 18 October afternoon

- Removed the unused `parsed_response_t` / fixed-array tool-call type, flat response policy helper, response shadow comparator, flat Chat tool emitters, and flat Responses SSE tail. No serving caller remained. The mechanical bypass gate now rejects their reintroduction. The live Go-backed canonical governance owner and its typed refusal/result publication remain authoritative.
- Moved the existing canonical buffered Chat/Responses and SSE output implementation from `openai_chat.c` into the translation owner. Serving and direct fixtures now call those same typed output boundaries. Orchestration snapshots carry borrowed `aimee_request_t` / `aimee_response_t` views rather than native JSON/flat response types; existing spawn/dispatch wrappers still have no model snapshot to supply.
- Replaced obsolete flat/shadow fixtures with real native-codec and production-output fixtures: all three wires retain 20 KB of exact text, 400-byte tool IDs, 40 ordered tools, exact numeric/string arguments, usage and truncation. Chat/Responses output tests cover namespaced identities, buffered/stream terminal parity, one envelope owner, cached usage and explicit capacity/argument failures. Existing live IR governance, buffered/streaming HTTP and ingress regressions remain passing. Request-shadow mismatch logs now report lengths without prompt prefixes; the shared diagnostic counter is atomic.
- Validation: 14 focused native fixtures passed normally; seven passed under ASAN/UBSAN with leak detection (canonical client output, all-wire response codecs, OpenAI shapes, gateway request policy, orchestration, canonical governance, and OpenAI ingress). The full agent fixture retains its three explicit store-dependent skips. Server and KB builds passed. All 76 lint checks passed, including unchanged 188 module crossings and memory authority. Seven bypass mutation tests passed with new retired-symbol cases. Fresh schema `ir_integration_flat_retire_3036` passed integration 154/154, with 25 explicit service skips.
- This removes dead compatibility consumers; it does not establish native credential-free execution, native account-scoped resume/cache proof, complete remaining result/hook consumers, or live/production qualification. The generic simple request builder already constructs IR internally; remaining review there concerns post-render profile edits and fallback prompt continuity, not a native handwritten request builder.

### Prepared prompt survives simple fallback, 18 October afternoon

- The non-tool `agent_execute` path applied pre-LLM hooks once for its primary request, freed that prepared prompt, then built fallback from the original user text. It now retains the same prepared user content through fallback and uses it for Anthropic payload accounting too; all early exits release the owned buffer.
- A transport-level regression with a real registered pre-LLM hook reproduced the missing context before the fix (assertion on the fallback body). It now passes normally and under ASAN/UBSAN with leak detection, verifies the hook runs exactly once, and retains both primary/fallback local-refusal terminal assertions. Server/KB rebuilds and affected IR, memory ownership and line checks pass. The exact runtime boundary hash/rationale records the change without altering memory authority.

### Canonical native hook admission, 18 October afternoon

- Native pre/post tool hooks now stop at `translation/hook_ir.c`. It owns complete tool identities, typed arguments and structured results, including the native semantic error flag. The thin CLI forwards a versioned canonical hook intent; the server and legacy direct command consume the same owned representation. Installed old clients retain one explicit unmarked native entry, while marked/mixed or unsupported versions cannot fall back. Scope and authenticated harness metadata remain transport-owned. Missing legacy tool IDs are never invented or published as model history.
- The old 64 KiB/truncated stdin behavior is replaced with a complete bounded reader (4 MiB for hooks). Exact original hook HTTP frames and frontend request frames reject trailing bytes, embedded/escaped NUL, invalid UTF-8, unescaped controls and duplicate keys before a native JSON parser can erase identity. Serialized native argument objects decode once; canonical string arguments remain strings. Opaque result numbers and full call IDs survive. Conflicting identity/scope aliases refuse rather than silently selecting a different owner.
- Worktree routing and discovery read canonical arguments directly. Host memory interception borrows the canonical call and retains its existing Go classification/storage owner. The policy call receives the actual full call ID; authentication, session policy, registered workspace and detached runner binding remain in place. Foreign namespaces have no binding in the current flat native-hook policy catalog and explicitly refuse before routing/state changes rather than borrowing a leaf's privileges. General advertised namespaced-native hook support remains open.
- Structured legacy hook results are retained as context evidence instead of being lost when content is an array/object. Existing native output/rewrite boundaries remain explicit. Hook intents are control/policy inputs, not a second model transcript. The bypass gate now rejects native hook envelope readers in the migrated direct-hook and memory-interception consumers; its exception ledger remains empty.
- Validation: six focused binaries passed normally and under ASAN/UBSAN with leak detection (hook scope/admission, util, frontend, cross-protocol replay, server dispatch and server HTTP). The dispatch fixture checks trusted canonical policy admission with a 400-byte ID, exact numeric arguments and namespace refusal before state mutation. Real CLI/server/KB builds pass. Fresh-schema integration `ir_integration_hook_final_3036` passes 155/155, with 25 explicit service-dependent skips; its real HTTP/CLI checks cover legacy/canonical admission, mixed versions, mismatched call/result IDs, namespaces, malformed bytes and oversized stdin. An earlier fresh-schema run also passed 155/155. The first attempt aborted on PostgreSQL disk quota before hook checks; the disposable test cluster was stopped cleanly, moved from `/tmp` to `/var/tmp/aimee-ir-pg-data-3036`, and restarted on its same 55463 port before fresh-schema reruns.
- All 76 lint checks ran; only the new source/IR-type registrations initially failed. Those registrations were corrected and affected ownership, module, line, memory, bypass and test-registration gates pass. Eighteen ownership/bypass mutation regressions and eight attribution regressions pass. Exact changed memory adapter/fixture fingerprints include source-boundary rationale; no memory policy authority or independent production approval is inferred. CMake and Make share the new pure IR/hook source declarations rather than duplicating canonical source ownership.
- Published head `a63c95c9b` now passes all technical CI, including complete sanitizer/scripts and T1/T2/T2-luks/T3 deployment jobs. The strict opening-app attribution gate is still failing and is not exempted. This hook slice does not finish the migration: credential-free native execution, actor-bound MCP, account-scoped resume/cache proof, remaining consumers/capabilities and live/production qualification above remain open. All further publication stays on #3036.

### 2026-10-18: verified caller ownership across async execution

- Compute contexts now own the originating request context, including verified caller proof, restrictive header budgets and the private attested Vault actor. Chat, steer continuations, tool execution, delegate execution, generic async operation runs, SSE offload and both parallel fan-out paths restore the captured context and clear it after execution. Copies containing caller proof are wiped when released. Conn-free continuations inherit the originating capability ceiling; a different authenticated connection discards the former caller proof and limits.
- Private connection attestation stays distinct from display/proxy principal strings. An inherited browser actor can execute its authorized task but cannot submit interactive permission/question approvals: those require the actual attested browser request. Async operation ownership remains installed through durable terminal capture. The existing operation lifecycle was extracted to server_http_op_runs.c to preserve the HTTP route owner's line bound.
- The first threaded regression failed before the fan-out fix. Six focused native fixtures now pass (agent-parallel, request-context, server-dispatch, server-http, full agent and server-chat-ir), including all six under ASan/UBSan with leak detection. The full agent retains three explicit store-dependent skips. Current server/KB builds pass. All 76 lint checks pass again with the final HTTP capture-order correction.
- Fresh integration initially exposed an actual regression: HTTP populated request context before capturing the authenticated connection, so the compute handoff discarded its incomplete owner and a real authorized file read failed. Capture now precedes context population after certificate/capability resolution; every connection-worker exit still clears identity/context. A fresh PostgreSQL schema, ir_integration_owner_capture_3036, passes 155/155 integration checks with 25 explicit service-dependent skips, including real canonical primary switching, replay and tool execution.
- Published a6f9acdc7 CI finished: sanitizer, integration, frontend, Go and T1/T2/T2-luks/T3 deployment tests pass. Two technical failures are fixed in this tree: the thin-client canonical usage bound explicitly converts LONG_MAX for Clang, and the exact Make-only translation registration for test_cmd_hooks_scope.c is recorded. Strict Clang validation and all 18 registration regressions pass. The immutable opening-app attribution gate remains failing, with no exemption or replacement PR.
- The user's additional production KB/learning requirement is open. The authenticated registered CLI reaches .100 and its background-process capability gives a verified command path into the aimee container. Production reports v1.0.2-history-index-repair-20261017; KB status is unavailable, kb_mode/kb_client_url and synthesis endpoint/model are unset, and dashboard vector status is unavailable. The learning module process is present; the durable delegate jobs listing is empty. DB1 delegate learning, no-progress approach failures and KB agent outcomes have distinct writers; zero rows must not be replaced by manufactured evidence or inferred to have one cause. Shared Vault-name inspection is explicitly refused without vault:write:server for webuser:admin; no self-grant or alternative credential extraction was attempted. Direct SSH has a changed host key and was not used. Production configuration has not been mutated, a real completed-task learning receipt has not yet been demonstrated, and production promotion remains unqualified.

Native credential-free transport and full authorized MCP tools, account-bound native replay/cache proof, remaining IR consumers/capabilities and the additional production learning repair remain required. This checkpoint does not complete #3036.

### 2026-10-18: delegate learning capture before terminal publication

- Confirmed a separate learning lifecycle defect: delegate_worker published its terminal response/job state before invoking delegate_record_exit_learning, and both DB1 writes ignored their results. Capture now runs before terminal publication. Its typed API returns incomplete capture on a failed learning row, exit event, applicable no-progress approach row or event serialization. Warnings identify the failed capture separately from the task's execution result; the durable exit payload records whether its learning row was accepted. A positive interaction-event ID is success, matching the existing DB1 client contract.
- The extended delegate-exit-classify fixture passes normally and under ASan/UBSan with leak detection. It exercises successful capture, each failed store write, positive event IDs, provider metadata and successful/failed no-progress approach writes. The server/KB build passes. Fresh Go race tests pass for modules/aimee/families and modules/learning. All 76 lint checks pass.
- Added a real async delegate completion/readback journey to the primary integration fixture: after a completed sandboxed format task, the next task must receive the first task's persisted lesson through the existing DB1 prompt reader. The request includes the required engineer persona. This requires the actual Docker sandbox, with no fake or unsandboxed substitute. The integration runner counts and prints its explicit skip when that service is unavailable. Local final integration passes 155/155 with 26 service-dependent skips in ir_integration_learning_final_3036. The new sandboxed readback journey is NOT locally qualified and requires fresh full-stack CI.
- b8354e0d6 is published on the same #3036. Its macOS build and complete script suite pass, confirming both prior technical CI fixes. A later handoff heading was appended after the local lint run and contained an em dash forbidden by the documentation gate; that exact heading is corrected here and docs-check passes. At the latest check its sanitizer and T2/T2-luks/T3 jobs were still running. The strict opening-app attribution failure is unchanged and remains enforced.
- Production KB destination is not configured in the connected instance or its managed Compose services. The user subsequently confirmed that this is intentional: no KB instance should be used. Configuration and grants remain untouched. These code changes do not establish production synthesis, approach accumulation or independently graded task outcomes, and do not complete the migration work above.

The production destination question is now answered: the user requires .100 to run without a KB instance. KB activation is not part of this repair. Confirmed remaining server-local dependency: agent_runtime.c record_outcome calls kb_client_agent_outcome_record, which sends agent.outcome_record to the optional KB; the server learning.application route likewise forwards to KB. DB1 delegate learning and approach failures are separate local writers. These remaining KB dependencies must be removed through the existing runtime owners, with actual verification provenance preserved; process completion alone must never become an independently graded outcome.

Escaping follow-up: a new regression reproduced malformed failure evidence when errors contain quotes, backslashes, newlines or Unicode. DB1 stores evidence_json as TEXT, so this corrupts provenance rather than proving the cause of zero rows. Classification now serializes error data with cJSON, and its evidence buffer holds the worst-case JSON escaping of the production 512-byte error buffer. Metadata/exit serialization failures refuse incomplete capture instead of writing partial provenance. Normal and ASan/UBSan fixtures pass, including a 511-byte control-character error. Final server/KB builds and all 76 lint checks pass. Fresh integration passes 155/155 with 26 explicit service-dependent skips in ir_integration_learning_escape_3036. The new sandboxed delegate readback remains a local skip, not a qualified live-learning result.

### 2026-10-18: standalone canonical execution observations (in progress)

Production .100 must remain disconnected from KB, as explicitly confirmed by the user. The five legacy agent_runtime outcome calls sent heuristic observations to optional KB. Those calls are removed. Browser and private execution capture owners now invoke the registered Go Learning stage only after a successful canonical terminal append. Learning requires a host-owned authenticated context, validates the typed terminal event and exact positive source sequence, and returns an explicitly ungraded observation. The host persists that projection through the existing local DB1 interaction_events ledger as task_outcome, with the immutable canonical session/turn/event/sequence and actual actor in the payload. No new transcript or database is introduced. A capture failure leaves the source event intact and warns; execution completion is never promoted into an independently verified grade.

Go race tests pass, including authority refusals and exact sequences above the JavaScript integer limit. Browser/private native fixtures check that the source append precedes observation; rejected appends do not emit observations. Browser capture, private capture and full-agent fixtures pass under ASan/UBSan with leak detection. A new read-only live assertion verifies four actual browser terminal observations against their canonical PostgreSQL source records. Fresh integration ir_integration_local_learning_grant_3036 passes 155/155 with 26 explicit service-dependent skips. Its live Go assertion verifies all four local observations against the exact canonical source rows. The harness starts the real Learning owner with its generated grant installed before the bus starts. Earlier red runs caught absent owner/grant installation; neither was accepted as a skip. All 76 lint checks pass. Eight attribution checker regressions pass without changing the gate. Automatic reconciliation of a failed observation write is not implemented.

Fresh CI on 615261841 passes lint and Go/frontend/native unit jobs. Its integration failure identifies a new fixture setup error: the delegate learning journey used the test configuration directory, which is outside the registered workspace. The fixture now supplies the actual registered mirror checkout as its cwd; sandbox checks remain unchanged. Local Docker qualification remains unavailable. The opening-app attribution gate continues to fail without exemptions.

The server learning.application route still forwards receipt-bound feedback to KB and requires a standalone owner path. Independently graded outcomes, synthesis, remaining learning intake and production qualification remain open alongside the native credential custody, replay and remaining IR consumer work. This checkpoint does not complete #3036.

CMake follow-up: mirrored the new helper, scoped Learning stub, log stub and interaction-event names into the two affected fixture declarations. A focused CMake project extracted those declarations from src/tests/CMakeLists.txt, linked the actual required IR/core sources and ran both fixtures successfully. The repository-wide native CMake configure still fails on existing retired source/test references (including DB1); this is not a full native CMake qualification. Make's current server/KB and affected fixtures remain qualified as described above. All four deployment tiers finished successfully on 615261841; that head's only failed checks are integration (the fixture cwd corrected here) and immutable opening-app attribution.


### 2026-10-18: reconcile native CMake capture qualification (local)

The native CMake configure failure recorded above is now resolved locally. Removed one malformed orphan source token and seven registrations for test sources already retired from this checkout; no runtime fallback or active fixture was removed. Existing Go store/private-memory suites and canonical native fixtures retain the migrated behavior. The private PostgreSQL bootstrap client now guards its GNU feature macro so CMake's command-line definition does not cause a redefinition error. Actual repository builds then exposed the private-session fixture's missing canonical event codec and SHA-256 dependencies, which now match Make.

Repository-wide native CMake configuration succeeds, and both canonical browser/private capture targets build and pass through that graph, including RelWithDebInfo with assertions retained. The existing build-integrity CI job now installs CMake explicitly and configures/builds/runs those same targets so stale removed-source references and missing link dependencies are exercised automatically. Full native CMake all-target compilation is not claimed. Make server/KB parity and all 76 lint checks pass. These changes are local and do not complete the remaining migration or standalone learning work.


Fresh CI on a3d470b98 passes the standalone Learning live assertion against four canonical source rows, plus lint, Go/frontend and ordinary native unit jobs. Full integration still fails: after the corrected registered cwd, its repository-backed format delegate requires a dedicated worktree, and the disposable bare remote cannot satisfy the production remote-default fetch policy without the Git owner. The fixture server now explicitly selects the supported current worktree base for this deliberately offline disposable checkout; production policy is unchanged. Workspace/owner/worktree setup now runs even without Docker and must reach the mandatory sandbox refusal before the completion/readback journey may be reported skipped. Fresh local integration ir_integration_learning_worktree_3036 passes 155/155 with 26 explicit service-dependent skips. Its actual delegate job creates a dedicated worktree and reaches the mandatory Docker refusal without a provider call. This qualifies local setup, not real Docker completion/readback, which is still not established. All 76 lint checks passed before the final fixture update; Python compilation, shell syntax, docs checks and the complete integration run pass for that update.


### 2026-10-18: CI workflow identity and supervisor fixture correction

The user requested a meaningful name for c-repository-pins. Its new displayed workflow name is
Module exports and runtime validation, with the job Export builds and runtime checks. Its main-only
application snapshot/pin gate remains main-only; testing still exercises the exported builds and
module runtime. The failing pins step in 78222d62e was test_module_supervisor.sh after the memory
module restarted, not the published-pin gate.

The fixture's visible count file could be truncated while its set-e reader polled, causing an immediate
EOF exit with no diagnostic. The helper now atomically publishes complete PID/count files, preserving
PID-before-count ordering. Polling treats an incomplete read as not ready within the same bounded
restart timeout. Cleanup waits for the supervisor and socket process before removing their files.
A forced short-read experiment reproduces exit 1 with the previous polling expression and passes
with the corrected expression; five actual restart/shutdown runs and Go supervisor race tests pass.
The seven application-lock regressions, export lock-preservation regression and workflow-pin check
also pass. This does not weaken restart or child-shutdown assertions.

An Aimee-authenticated edit of #3036 succeeded, but its newly triggered attribution job still failed on
the recorded connector app. The user's authorized replacement procedure above is being prepared.
The overall migration and production learning repair remain unfinished.


### 2026-10-18: recover local execution observations from canonical replay

DB1 now binds task_outcome projections to the exact immutable source session, sequence,
event ID, turn and actor, with status/error checked against that event. The session row lock
serializes concurrent retries; identical projections are idempotent and inconsistent replacements
refuse. Unknown sources, another actor and a fabricated grade cannot enter this path. The
existing observation wire, Learning validation owner, local interaction ledger and canonical source
remain in place; no transcript or schema is added.

Owner-checked browser event replay retries terminal observations before delivery. A lost or pruned
derived row can therefore be recovered from the retained canonical source without changing the
execution result or duplicating another subscriber's observation. This is replay-driven recovery,
not a claim of a background sweep over every unvisited private session.

Real PostgreSQL race tests pass with 16 concurrent retries producing exactly one row, source and
grade forgery refusals, loss/reconstruction and deleted-source refusal. The real HTTP integration
journey verifies four captured observations, removes one disposable projection as a fault injection,
then recovers it through actual authenticated replay and verifies exactly four after repeated/resumed
replay. Fresh schema ir_integration_recovery_3038 passes 156/156 with 26 explicit service skips
(including local Docker completion/readback). Native HTTP/dispatch and Go owner/Learning race tests
pass. Server/KB build and affected ownership, bus, bypass, docs and test-registration gates pass.
Both affected native fixtures also pass with ASan/UBSan and leak detection. All 76 lint
checks pass.

Procedure feedback still needs a standalone path: learning.application continues forwarding
receipt-bound feedback to KB. Independently graded learning, synthesis, credential-free native
execution, actor-bound native tools, cache-preserving account-bound replay and remaining IR
consumers remain unfinished. Production .100 remains intentionally disconnected from KB and
has not been promoted to this migration head.

### 2026-10-18: canonical asynchronous inference runs

`/v1/runs` now admits complete canonical Responses input and pins the selected registration.
Its governed server tool loop retains ordered roles, media, instructions, sampling and explicit
output limits, full tool identities/results and exact aggregate typed usage. There is no
role-transcript flattening or fallback to another model. Unsupported execution controls, client
catalogs and explicit limit/temperature controls on the Responses/Codex backend refuse before
execution rather than silently disappearing. The existing bounded live projection exposes the
private canonical session reference; durable history remains with the session log. Token-limited
output stays incomplete, and cancellation interrupts an actual in-flight provider request.

Inference and operation run records require their authenticated owner. Read, event and stop routes
hide foreign records. Generic queued operations use the same captured owner for the record and
worker; pipeline callers pass their real connection through the existing compute context capture
owner. A different connection discards inherited caller proof and budget. Pooled worker tests
verify no previous checkout, credentials, caller proof or budget leaks. CLI polling recognizes
incomplete as terminal. Five unused input flattening/conversion APIs are removed and guarded
against reintroduction. The API documentation and its exact memory-boundary disposition are
reviewed; memory admission remains Go-owned.

Eight affected native fixtures pass (HTTP, dispatch, full agent, session log, OpenAI shape,
canonical ingress, live run store and CLI polling), including ASan/UBSan with leak detection.
Full-agent fixtures retain three explicit PostgreSQL-dependent skips. Eight bypass mutation tests
pass. Server/KB builds pass. Fresh schema ir_integration_runs_final_3038 passes integration
156/156 with 26 explicit service-dependent skips, including unavailable local Docker. The actual
run journey exercises full roles/media and a 20KB message, selected-model routing, a governed
read_file call with a 409-byte identity, complete durable history, exact two-turn usage, native
SSE output, truncation, malformed-control refusal and cancellation before provider release.
All 76 lint checks pass. This does not qualify production promotion or finish credential-free
native execution, actor-scoped native tools, account-bound native replay/cache proof, remaining
IR capabilities/consumers or the broader standalone learning repair.

### 2026-10-18: retain exact browser tool identities

Browser canonical capture no longer qualifies native tool IDs with host turn/message IDs.
Event/message identities remain host-qualified; model tool IDs, including streamed block starts,
remain unchanged. The existing frontend already correlates calls by projection message/block
identity and execution scope, including repeated native IDs. Removed the obsolete per-compute
mapping and cleanup. This aligns the durable browser event stream with provider replay rather
than storing a different model identity for presentation.

The native capture regression exercises 1024-byte identities, repeated IDs in separate model
responses, exact result pairing and streamed tool starts without mutating producer blocks.
Normal and ASan/UBSan/leak runs pass. Both focused frontend suites pass (26 tests). The actual
HTTP journey now requires the exact original call ID on live calls/results and provider replay.
Fresh schema ir_integration_browser_ids_retry_3038 passes integration 156/156 with 26 explicit
service skips. The first attempt hit the local /tmp user quota before the new assertions;
the integration runner now honors TMPDIR for its isolated home, and the rerun passes. All
76 lint checks passed; the final temp-home adjustment also passes shell syntax and tempdir gates.
Server/KB builds pass. All 680 declared fixture/helper targets compile locally; local GCC's
existing unterminated-string-initialization and unused-but-set-variable warnings required
nonfatal treatment for that broad compile. No CI compiler flags were weakened, and this is not
a claim that all 680 fixtures were executed.

Fresh CI on the canonical-run publication caught the old minimal-ingress weak free stub's
void-pointer declaration after agent_exec.h began including the canonical types. Published
c60e7f20b corrects it to aimee_response_t*. All three consumers compile and pass locally;
the replacement CI's ordinary unit shards and two PostgreSQL shards have passed so far.
Continue checking every named job through Aimee git pr checks, not only the first job-list page.
The overall migration and standalone learning/production work remain unfinished as listed above.

### 2026-10-18: preserve complete attested session owners

The C server-session DTO allowed only 64 bytes, while private capture/native bindings and the
control broker allowed 128. The Vault contract permits a 128-byte name plus its namespace.
Legal authenticated identities could therefore fail history-owner checks, be truncated in
capture, or fail to open a permission/question scope. These boundaries now use the existing
VAULT_PRINCIPAL_MAX contract. The C store client already sizes decoded cells from the DTO;
the SQL schema, wire, authenticated identity factory and Go admission authority are unchanged.
The external memory boundary disposition records the narrow identity-buffer change.

Native regressions exercise a full-length legal webuser identity through an existing parent,
private execution, source capture and identity copy. Another identity with the same long prefix
and a different final byte cannot adopt that parent. Control replies similarly accept only the
complete original owner and remain consume-once. Five affected fixtures (private log, controls,
browser capture, HTTP and full agent) pass normally and under ASan/UBSan with leak detection.
The full agent retains three explicit store-dependent skips. Server/KB builds and all 76 lint
checks pass. Fresh schema ir_integration_owner_width_3038 passes integration 156/156 with 26
explicit service-dependent skips.

The earlier c60e7f20b CI reached 56 passing checks but T3 failed at the native asynchronous
memory gate. Its artifact proves memory arrival/receipts, recovery, refresh and protected folding
passed; the first failed assertion was the tmux accounting refusal diagnostic. The new canonical
history-adoption refusal had preceded the existing external-backend accounting fence. Published
482ce4580 restores that fence's priority without weakening either refusal. Its new full-agent
regression fails before the repair and passes afterward for both tmux/Claude and provider/ACP;
the final full-agent sanitizer run also passes. Fresh deployment CI is required to qualify the
complete T3 journey and all later assertions. Keep the attribution gate and automatic reviews.


### After merged #3038: scoped native runtime prerequisites (local)

Continuation branch `fix/browser-ir-continuation` starts from merged #3038 (`344139139`).
This slice prepares the native child boundary; it does **not** activate credential-free SDK
execution or claim the migration complete.

- The existing local listener now accepts a host-owned route/forward callback. The CLI's
  ordinary proxy remains model-only. A new server runtime captures an attested session owner,
  selected registration and restrictive capability set. It admits only model/MCP entry routes,
  strips operator authority, ignores child-supplied identity/credentials, pins model resolution
  and cancels an in-flight handler on disconnect. The original browser dedup key cannot collapse
  separate native requests. Codex Vault reads use the captured actor, retain the paired source
  lineage and refuse a changed login before forwarding. Server credentials never enter a child
  response. This is a same-host scoped capability, not a process sandbox.
- Existing HTTP handlers accept the captured owner through an explicit internal entry. Native
  children cannot acquire interactive browser-operator authority. The selected-model binding is
  thread-local, nested and restored; identical vendor model names cannot change the connection.
- Credential-free homes are available with durable native state partitioned by session and a
  scope digest. They read no Vault credentials and link no shared native configuration. The
  production SDK spawn sites still use the old credential-bearing homes until replay and
  supported model/tool transport are wired and qualified together.
- The canonical event contract reserves strict `replay_proof` metadata, consistently validated
  by C, Go and the browser. It is excluded from transcript history. Actual source-proof capture,
  restored signed-thinking egress, ambiguity handling and account-bound resume adoption are
  still outstanding; the new event is not yet produced by a live SDK runtime.

Local validation so far: the full agent fixture passes with its three explicit store-dependent
skips; native listener/runtime, HTTP, native homes and canonical event fixtures pass. The five
boundary fixtures also pass ASan/UBSan with leak detection. All 288 frontend tests, TypeScript
checking and Go IR/session-owner race suites pass. All 76 lint checks and server/KB builds pass.
The new runtime fixture passes through the actual repository CMake graph with assertions enabled.
Session-erasure, malformed-lineage, full 127-byte session identity and protocol-header cases also
pass normally and under ASan/UBSan. CI now builds/runs the runtime fixture through CMake. The
first CI head passed strict attribution but its secret scan flagged the hexadecimal validation
alphabet. The predicate now uses character comparisons; the scanner and its full-history scope
remain unchanged. The branch commit is rewritten to remove the flagged line from reachable
history. The configured local test PostgreSQL endpoint is unavailable, so integration and
deployment qualification remain with fresh CI; no production qualification is claimed.

The next required work remains actual credential-free native SDK wiring, actor-bound full tools,
account-bound cache/history adoption, upstream-response replay evidence, Claude refresh custody,
remaining typed consumers/capabilities and standalone procedure feedback. Production `.100`
remains intentionally KB-free. Independently graded learning, synthesis and live acceptance
cannot be inferred from process-success observations. Production promotion still requires the
independent reviewer specified in `OWNERS.md`.


### Scoped native replay provenance (local, after the transport checkpoint)

The scoped runtime now records proof-only metadata from actual canonical upstream responses in
its existing principal-owned session log. This is a derived, mutex-protected cache rebuilt from
that log, not another transcript store. Block commitments exclude host-only cache/context markers.
Anthropic signatures recover their actual observed `ap1` binding; echoed prefixes cannot mint one.
Conflicting bindings never become last-write-wins. Unknown/account-model-mismatched opaque state
is omitted from the egress request with a durable fidelity report, preserving the source history.
The report uses the captured selected model and is emitted once across entry/egress rechecks.
Source read/write failures refuse, and paging checks actor, session and exact sequence continuity.
Cancellation interrupts proof paging. The listener also gives each admitted native HTTP request
its own request identity while preserving the host session and restrictive limits.

The translation owner exposes a borrowed thread-scoped guard. Both HTTP owners recheck actual
provider egress after history restoration and policy edits. Raw-sidecar comparisons remain pure:
applying the guard to both sides would let removed opaque bytes look unchanged and reappear.
The guard/sink apply only to model requests; MCP execution retains its own capture owner. Model
traffic currently requires the stable Codex Vault lineage; Claude custody/refresh support remains
open. Production SDK spawn sites are still unchanged and must not be switched before history,
actor-scoped tools and actual streaming are qualified together.

Local normal fixtures pass for the durable proof owner, scoped runtime, cross-protocol egress,
OpenAI shapes/ingress and all three Anthropic HTTP variants. They cover eight concurrent proof
writers, multi-page replay, source corruption/unavailability, failed writes, cancellation,
connection/model scope, ambiguous proof, explicit omission, late restored-history admission and
request cleanup on guard failure. The final five focused fixtures pass ASan/UBSan with leak
detection; the proof/runtime fixtures also pass ThreadSanitizer. The full agent suite passes
with its three explicit store-dependent skips. Both actual CMake fixtures, server/KB builds and all 76 lint checks pass for this source tree. Fresh CI is still required
for this replay-provenance slice. Transport checkpoint `8565183b0` finished with 55 passing
checks and the two existing platform skips, including integration, sanitizers/scripts and
deployment tiers T1, T2, T2-luks and T3 (CI run `39157560588`).

A real installed Codex `0.160.1` app-server qualification found and exercised the stable
`thread/inject_items` method, without `experimentalApi`. Using a temporary credential-free home
and a loopback provider fixture, the actual provider request retained adopted user/assistant
roles, full call/result identity, prior content and the current user. After process restart,
`thread/resume` retained the adopted history and completed native turn. This is a supported
adoption candidate, not yet wired into Aimee or evidence of real upstream cache billing.
Artifacts: `/var/tmp/aimee-native-inject-http-probe.py` and its captured request JSON. The stable
and experimental schemas were generated from the installed binary. The separate experimental
`thread/resume.history` field explicitly says `FOR CODEX CLOUD - DO NOT USE`; it stays unused.
The official source also declares stable `thread/inject_items` in
https://github.com/openai/codex/blob/main/codex-rs/app-server-protocol/src/protocol/common.rs.

Next: implement and qualify canonical history adoption/cache continuation at the actual SDK
spawn sites, including changed-model/account scopes and complete tools/workspace/control owner
handoffs. Preserve same-scope native caches where their canonical prefix is still valid; do not
silently start blank or flatten roles. SDK internal compaction and true Responses streaming need
qualification before activation. Claude OAuth custody/refresh, remaining IR consumers/capabilities,
standalone procedure feedback, independently graded learning and production acceptance remain
required. `.100` remains intentionally KB-free; no production promotion has occurred.

## Model execution attribution in the continuation

The host request context now carries model-execution intent independently of the
authenticated principal. Native listener workers always set this intent, and the
common in-process agent tool loop sets/restores it across all returns. Async
context snapshots preserve it. A verified person still owns access to their data;
model-generated memory writes under that identity are classified as model authority
rather than human feedback. Restoring nested intent preserves receipts accumulated
inside the execution. No client header can set this host-only field.

Request-context, MCP memory-authority, native-runtime and full agent fixtures pass.
The first three also pass ASan/UBSan with leak checking. This closes an attribution
prerequisite; standalone feedback admission and native SDK activation remain open.

## Stable native history boundary and real SDK qualification

Both Codex adapter entry points now accept canonical history and use stable
`thread/inject_items` before starting a model turn. This is an output-boundary
implementation; browser/session callers still need account-scoped prefix proof
and the complete native tool/workspace/control handoff before activation. A typed
request carries preceding history (only unseen tail on a proven resume), never a
flattened transcript. Rejection or invalid IR refuses before model execution.

The new opt-in `src/tests/test_codex_native_adoption.py` drives the compiled Aimee
adapter against the actual installed Codex 0.160.1 and a credential-free loopback
provider. It passes initial canonical adoption, actual provider-input assertions,
process restart and native resume retaining prior roles/tool pairs/completed turn.
This caught and fixed real SDK startup rejection of the obsolete `untrusted`
approval policy; startup now uses `on-request` and retains host control handling.
Synthetic provider usage does not establish production cache savings.

## Standalone feedback persistence in the continuation

The KB-free `learning.application` path now uses the existing Learning admission
and projection owner and DB1 telemetry's local immutable event ledger. The host
constructs exposure/scope from its principal-owned WORM receipt and replaces JSON
actor claims with the attested local principal. Host model intent refuses human
feedback, including on the configured-KB route. Daemon bearer identity alone does
not establish a user. Existing configured KB behavior is preserved; no KB service
or endpoint is enabled. Explicitly misconfigured remote mode does not fall back.

Migration 46 adds local observation rows and payload-free erasure intent. Typed
DB1 snapshot/append operations preserve full JSON, use optimistic cohort digests
and prevent mutable event-ID replacement. C projects before append and retries
projection on a concurrent update. No additional active session/project is needed
for feedback on an existing receipt. Subject erasure and startup replay cover the
new rows; triggers and the existing epoch fence erased IDs and old RR snapshots.

Local qualification uses a newly built isolated PostgreSQL 17.6 under
`/var/tmp/aimee-ir-postgres`, loopback port 55449, UTF-8 database `aimee_ir_test`.
The source archive's published SHA-256 was checked. No production DB is used.
The complete Go DB1-family and Learning race suites pass with both PostgreSQL
fixture variables configured. Local feedback ownership/CAS fixtures pass normally
and under ASan/UBSan. A real Codex adoption/restart regression also passes against
CI's pinned 0.153.4, and is now included in the build workflow.

This closes local explicit-feedback persistence, not autonomous learning. Native
SDK activation, account-bound canonical-prefix/cache admission, complete native
tool/workspace/control snapshots, Claude custody, remaining consumers and actual
independent learning/grading/synthesis acceptance remain open. Production `.100`
remains KB-free and is not promoted without the required independent review.

CI follow-up: the native SDK test's registration inventory entry was missing and
is now recorded without changing the checker. The parallel script run also
reproduced a shared-checkout fault-injection race: principal-ref and provider
registration tests wrote their negative cases into actual production files. They
now use a private fixture/virtual read, with 12 concurrent gate runs passing and
production inventory/contracts unchanged. These are separate pushed CI repairs;
the standalone learning slice's fresh CI still follows its own publication.

The standalone feedback slice also passes the repository CMake fixture with
assertions enabled, the server production build, all 76 lint checks and full
DB1-family/Learning race suites against a fresh UTF-8 fixture database. Focused
PostgreSQL retries cover maximum admitted action/gap arrays exceeding 64 KiB,
without the old 4096-byte DTO. The script suite's observed shared-checkout race
was repaired and independently stress-run; its complete rerun/fresh CI remain
the final validation for this checkpoint.


## Native execution context handoff

The scoped listener now owns captured tool permissions and source-authority paths,
including role, toolset, deny list, write/shell/knowledge limits and turn snapshot.
Callbacks bind the captured workspace, cwd, job, session and delegate context and
restore the worker context on success or refusal. Empty native scopes cannot fall
back to another delegate's process environment. Model callbacks clear effect grants
and operator controls; the SDK reader retains human control ownership. The caller
must retain workspace/delegate leases until the child exits and listener joins.

MCP bodies receive the host session identity, preserving tool argument numeric
spelling. Session overrides retain the full 127-character canonical identifier.
Scoped HTTP workers use the server's 32 MiB stack size for deep model/tool handlers.
The production server, full agent fixture, native runtime fixture, ASan/UBSan,
ThreadSanitizer and CMake assertions pass. All 76 lint checks pass; exact CI
Cppcheck 2.13.0 reports no diagnostics for the six affected production files.

This supplies context handoff, not complete SDK activation or MCP admission.
The served MCP boundary still needs shared role/permission/effect enforcement;
carrying a permission snapshot alone does not establish that enforcement. Native
history/cache prefix proof, streaming and credential custody remain prerequisites.

The full local script rerun passes all 69 tests (one proxy fixture runs separately).
The feedback route fixture now links the real request-context owner and covers
local/remote selection plus model-feedback refusal. Its DB1 reply is initialized;
the exact CI Cppcheck ratchet returns to 46 pre-existing telemetry diagnostics.
Published checkpoint `4be93529f` has 54 passing checks, two platform skips and
one Docker tier pending at this observation; no failures. Later commits require
their own fresh qualification. No production promotion has occurred.


## Real SDK tool history qualification

The actual SDK fixture now runs a local stdio MCP server, executes a failed
read-only tool, resumes after process restart, then adopts the captured canonical
messages into a separate native thread. Both Codex 0.153.4 (CI pin) and 0.160.1
pass. The provider receives the same full call/result ID, actual arguments and
callable namespace on fresh canonical adoption. Structured failure content survives.
No vendor credential, external model or production service participates.

This found and repaired SDK tool ingress semantics: MCP/dynamic arguments are no
longer wrapped in an extra host `arguments` object, MCP callable namespaces retain
the SDK's namespace identity, and dynamic results retain `contentItems`. Failed
commands with completed status and nonzero exit code, dynamic `success=false`,
MCP error payloads and SDK failed/declined status all set canonical error intent.
The narrow C fixtures and ASan/UBSan pass, as do both real SDK qualifications.
The CMake harness/headless targets now link their required SHA-256 owner and
OpenSSL dependency; both pass with assertions enabled. All 76 lint gates and
exact CI Cppcheck pass for this change.
Old durable records are not heuristically rewritten; earlier wrapped records do
not establish byte-equivalent native replay. Full account-bound prefix admission,
compaction qualification and command-item adoption still need completion.


## Continuation integration qualification

Checkpoint `16c218fa9` passes the full HTTP integration run: 156/156, with
26 explicit unavailable-service skips. This includes A/B/A/B primary switching,
canonical history, ordered durable replay/resumed cursors, source-bound execution
observations and lost-projection recovery. The local fixture uses PostgreSQL 17.6
on 127.0.0.1:55449, schema `ir_continuation_16c218fa9`, with a separate
`aimee_store_runtime` login and `virant` migration role. The first attempt correctly
refused identical runtime/migration roles; qualification uses separated roles.
The run does not establish independent grades, Docker completion/readback or
production acceptance. Log: `/var/tmp/aimee-ir-continuation-final-integration.log`.

The installed registered CLI still points at the older `ir3036-19d839003` release.
During PR metadata updates its implicit checkout resolution failed; explicit
`path=<continuation worktree>` succeeded. Current `marshal_git_cli` already stamps
the invoking directory as `path`, so do not reintroduce a duplicate source fix or
weaken checkout confinement. Use explicit paths with that installed CLI until an
independently reviewed production promotion updates it. Production stays KB-free.


## 19 October CI and testing merge repair

Merged `origin/testing` at `123ecafdc` into the continuation. The Makefile keeps
both testing's native cleanup and the local feedback source; the Codex fixture
keeps missing-input checks and canonical history adoption/real SDK probe modes.
The DB1 transport extraction also requires the two new local procedure calls to
use `db1_call(&family, ...)`, matching the shared transport owner.

The failed Go CI job was a routing process exit before bus admission:
`install synchronized socket filter: permission denied`. Linux no_new_privs is
thread-local, so its prerequisite and the following seccomp TSYNC syscall now run
on a locked OS thread. A positive TSYNC failure-thread result also refuses startup.
The network restriction is not disabled or narrowed. Twelve fresh-process checks
cover existing threads refusing both IPv4 and IPv6 while retaining Unix sockets;
they pass under the race detector. The complete `make go-unit-tests` job, including
all shipped Go module interop and routing startup, passes locally. The merged
Codex unit/real pinned SDK adoption fixture, native runtime/local learning fixtures,
production server build and all 76 lint gates pass. Fresh CI must qualify the
published merge; the previous aggregate unit failure was the Go job's consequence.


## Native MCP permission admission continuation

PR #3047 merged at `fedfa6728af96c14676a68ece6e4f84bfe8310eb` after all
58 applicable checks passed; the two PR platform skips are still skips, not
Windows/macOS qualification. Continuation branch: `fix/browser-ir-native-execution`.

The tools owner now exposes the existing role/toolset/deny-list, shell and
knowledge-command checks through one typed permission API. In-process dispatch
retains its refusal classifications and messages. Served isolated native callbacks
apply that same API after bridge/family resolution and before any handler or gateway
branch. Ordinary authenticated MCP clients retain their existing capability checks.
Git multiplex commands now expand before permission and workflow externalization
gates, so direct `git_push`, `git` command=push and `call_tool` targeting that family
share an identity. Insufficient family output capacity refuses instead of truncating.

The agent and MCP registry suites pass, including ASan/UBSan. New checks cover
captured permissions, denied flat/family/bridged Git calls, notes, remote names,
shell and knowledge-command limits, toolset limits, read access and truncated
identities. Clean builds also exposed an unused mock counter and Go fixture outputs
that mishandled absolute OBJDIR; those are corrected without suppressing warnings.
The memory ownership ledger records unchanged memory symbols and the actual host
admission boundary change; it does not claim independent reviewer approval.

This is permission admission only. Full served effects, write/path confinement,
argument-bound human approvals and question round trips still need completion
before SDK activation. Production callers remain on the old native execution
path; account-bound prefix/cache admission, real incremental Responses streaming,
remaining credential custody and IR consumers, independently graded KB-free
learning and production acceptance remain open. Production must remain KB-free.


## Native MCP directive/policy admission and fresh CI repair

The isolated served boundary now also invokes the existing hard-directive and
execution-policy owners before any handler or gateway branch. Its attempt is
bound to the callback's host request ID; tool-supplied attempt/side-effect/
authorization fields do not supply that binding. Missing/mismatched host session
identity, non-object arguments, cancellation and refused assembly context reject
execution. Context and cancellation are rechecked after policy admission. This
preflight grants neither effect authorization nor a human decision.

The admission, real execution-policy transport and native-runtime fixtures pass;
admission and policy transport also pass ASan/UBSan. The new admission fixture
passes CMake/CTest with assertions enabled and belongs to the ordinary Make unit
suite. The production server builds, all 76 lint gates pass and pinned Cppcheck
2.13 reports zero diagnostics for the new admission source, fixture and served
route. Full effects/receipts and argument-bound human approval remain open.

Fresh CI for `b833a37b8` found an LSP release-lineage failure and its dependent
unit aggregate. The retained report has 20/20 successful cold starts for both
gopls and pyright, complete reference recall and no false positives. The rejected
source paths are `mcp_tools_extended.c` and `test_mcp_client_registry.c`: this PR's
Git dispatch changes and tests. `s1-host-integration.json` records those exact
blocks independently from memory integrations, reverses them for frozen source
comparison, and retains the full remaining file/build-plan checks. Mutation tests
reject LSP drift, altered/removed/duplicated host blocks and changed assertions.
All 20 S1 gate/evidence tests pass. Checkpoint `a6834ef00` publishes that repair;
its fresh CI, and any later checkpoint's CI, still require their own qualification.


## Opaque reasoning delta parity for incremental Responses

Canonical deltas now retain `thinking_id` and `encrypted_content`, matching full
thinking blocks across C serialization/owned assembly, Go session validation and
browser live/durable projection. They are complete values at block start or a
later block delta, not text fragments. Identical repeats retain one value and
consume no additional C byte budget; conflicting identities/ciphertext, wrong
block kinds and lifecycle-event placement refuse. Owned assembly does not retain
borrowed provider buffers. The Anthropic stream renderer refuses opaque Responses
state it cannot represent rather than dropping it.

C session/stream fixtures and ASan/UBSan pass, Go IR/session-family race tests pass,
and all 289 frontend tests plus TypeScript checking pass. This closes a streaming
contract gap; it does not implement the incremental Responses backend or activate
the production SDK path. Remaining effects, human controls, canonical prefix/cache
admission, streaming transport, credential custody, consumers and KB-free learning
acceptance remain open.


## CI cancellation reporting during continuation

Editing live PR metadata deliberately reruns attribution and cancels the older
same-head CI workflow. At `e7c693d3b`, the registered older CLI displayed those
cancelled rows as `fail`; the independent GitHub job metadata confirms cancellation,
and `git pr failures` reports no failing checks. Do not label that as an assertion
or compiler failure. The current check-row formatter now displays `cancelled`
explicitly. Go merge grading remains unchanged and refuses cancellation as green;
C presentation regressions and the Git module's race suite pass.

Keep automatic reviews and attribution checks enabled. Prepare metadata before the
final push and monitor the replacement run, avoiding unnecessary same-head edits
while CI is running. Current installed CLI behavior will remain until the reviewed
runtime promotion; source fixes alone do not update production or establish a
complete migration. Production remains KB-free.


## Incremental Responses provider transport

`/v1/responses` now has a true incremental path for Responses-speaking providers.
The native entry adapter emits owned-validated canonical text/refusal, reasoning
summary and namespaced function-argument deltas before receiving a terminal frame.
Its bounded native frame history stays inside translation. At completion, the
existing full Responses decoder supplies the authoritative full snapshot; emitted
prefixes, identities, completed-item snapshots, opaque reasoning and structured
arguments must agree. EOF/truncated framing, conflicting snapshots, unknown semantic
events, malformed arguments and failed sinks refuse completion. There is no retry
or economizer resend after the incremental attempt begins.

The frontend consumes canonical deltas and preserves authored text grouping through
`message_group` on text block starts in C serialization/assembly, Go session
admission and browser live/durable reduction. It emits item-added before deltas,
retains opaque reasoning in final items, and verifies the whole projected output
against the admitted full snapshot before any item-done/client-completion event.
The host commits the canonical snapshot and durable Responses history before
publishing completion. Existing routing, request policy/stages, wire fences,
provider custody, response stages and accounting remain in the prepared executor.
Response-governance/subagent-prevention modes retain buffered admission; other
upstream wires and Responses-to-Anthropic ingress still need incremental cutover.
This is not SDK activation or full migration completion.

Focused parser/frontend and byte-fragmented transport fixtures cover early delivery,
message parts/groups, namespaced partial arguments, late encrypted reasoning,
incomplete responses, rewrite/identity refusal, truncated/malformed framing,
single-use snapshot transfer and persistence failure. Sanitizer, CMake/CTest,
production build, Go race, frontend and static/lint evidence is recorded with the
checkpoint. The immediately preceding pushed head `c20f87ac6` passed all 56 applicable
checks, including every Docker tier, with zero failures and the two intentional
platform skips. Its CI does not qualify this new increment. Production remains KB-free;
SDK effects/human controls/cache/custody/consumer and independent learning/deployment
acceptance remain open.

## Real Codex resume instruction qualification (continuation)

A real credential-free Codex app-server probe against a loopback Responses peer
found that accepting `developerInstructions` on `thread/resume` does not establish
that the next provider request uses it. The installed stable schema accepts the
field, but the captured next request retained `OLD_NATIVE_POLICY` and contained
no `CURRENT_NATIVE_POLICY`. The proposed adapter-only field change passed mocks
and ASan/UBSan, failed this real integration assertion, and was discarded in full.
Do not reintroduce it as a completed cache/context repair.

Artifacts: `/var/tmp/aimee-native-resume-instructions-probe.py` and
`/var/tmp/aimee-3050-resume-native-integration.log`. The probe keeps native thread
identity/history across process restart, uses a temporary credential-free home,
read-only sandbox and local fixture provider, and never dispatches real upstream
credentials. Changed or cleared authoritative system context therefore needs a
verified cache-context mismatch and fresh canonical adoption unless a separately
qualified native update actually changes model-visible context.

A native resume checkpoint also must be consumed durably before dispatch. Otherwise
a failed/cancelled/compacted native turn could leave uncheckpointed native content
behind a previously successful prefix proof. The continuation adds an
owner-checked atomic compare-and-swap to the existing harness metadata contract;
real PostgreSQL concurrency tests require one winner and refuse foreign/stale
completion. The cache/history controller and production SDK activation remain
unfinished; this storage prerequisite alone does not make any native handle safe
to resume.


## Native cache checkpoint controller (not activated at SDK spawn sites)

The host controller now compares current canonical context/catalog and source-log
history before claiming a native cache. Same attested scope/context with a valid
canonical prefix returns the native handle and the unseen history offset. Scope,
context, model or canonical-prefix changes select fresh full adoption. It never
loads a vendor transcript as the source. Missing required/unavailable/corrupted source,
foreign actor/session, sequence gaps, contradictory checkpoint metadata and an
unresolved existing claim refuse rather than becoming empty history.

A unique pending claim, linked to its canonical source turn, is committed through
new session operation 34 before any
resume handle is returned. Completion requires the source turn's durable
`completed` lifecycle and its canonical history, not process exit success. It
advances to finishing, appends a typed `native_checkpoint`, binds the native
handle, then publishes the completed claim revision. Partial failures leave an
unusable pending/finishing state. After the child is known quiescent, explicit
abort invalidates only derived cache metadata; close alone does not claim that a
child stopped. Original native-handle ownership/uniqueness remains in its existing
owner; checkpoint claims use a separate metadata key. No schema or transcript
store is added.

The shared C/Go/browser checkpoint contract contains scope/context/prefix digests,
claim, harness, opaque native ID and message count; consumers do not turn it into
model messages. The PostgreSQL race fixture has 24 competing claims and requires
one durable winner, rejects foreign/stale/late completion, and checks the final
stored value. Native-controller fixtures cover unchanged-prefix resume, context
and account changes, compaction/prefix mismatch, source failure/corruption,
failed source turns, lost claim replies, incomplete finalization and unresolved
claims. A metadata wrapper formatting failure was repaired and its focused gate
passes; complete final-head qualification is recorded with the checkpoint.

The controller is not yet called by the production SDK spawn sites. Before those
sites can switch, qualify full captured tool/workspace/sandbox/profile binding,
native compaction and adopted command-item fidelity, effects/receipts, human
controls, complete credential custody and the actual browser/delegate journeys.
The real Codex negative resume-instruction probe above still applies. This slice
must not be described as SDK activation or overall migration completion, and it
does not promote production or attach a KB instance.


## Checkpoint qualification and host project scope

Published head `c09ecdf2d` passed all 56 applicable CI checks, including the full
script regression suite and every Docker tier; the existing workflow deliberately
skipped Windows/macOS. These results precede the cache-controller increment.

The cache increment passed the real PostgreSQL 24-claim race, Go IR/family race
tests, 291 frontend tests and TypeScript, 76 lint checks, 47 module descriptor
regressions and 18 registration regressions. The final turn-linked token version
also passed native cache/IR-event ASan/UBSan fixtures and the registered CTest.
Cppcheck reports ten existing DB1 wrapper diagnostics, with no increase.

Native MCP forwarding now replaces child-supplied top-level `cwd` with captured
host project scope, alongside its existing session identity replacement. Nested
tool arguments retain their own paths and exact numeric values. The native
runtime fixture exercises a forged project scope and passes normally and under
ASan/UBSan; this is not a replacement for exact resource/path authorization.
The production server build and completion-policy/Responses transport fixtures
also pass. Responses completion-repair mode retains buffered admission because
its response owner can append a governed tool call after the answer.


## Tool classification prerequisites and canonical cache bypass guard

The supervised Go tools provider now agrees with the C module: Git publication
and web access are remote; `context_contract_expand` is control. Previously Go
reported Git publication as write and web access as read. The shared effect
consumer also discards partially written classifications when the provider
fails, and rejects invalid class values. These changes do not grant effects or
complete the native MCP KB-registry cutover: the served catalog still needs
complete owner classifications before that legacy label lookup can be removed.

The canonical history bypass gate now includes `server_native_cache.c`.
Its planted regression iterates every declared canonical C consumer as well as
Go consumer trees; native history field readers cannot enter the new cache
controller through an unguarded allowlist gap. No bypass exception was added.

A credential-free real Codex loopback probe additionally verifies namespaced
adopted tool calls and their results in the actual provider request. After process
restart it resumes the same native thread, injects only the unseen canonical tail,
and verifies exactly one copy of both the prior prefix and new tail. Artifacts:
`/var/tmp/aimee-native-cache-tail-probe.py` and
`/var/tmp/aimee-3050-native-cache-tail-integration.log`. This qualifies that subset
of SDK adoption; it does not qualify native compaction, full execution profiles,
SDK activation or live provider prompt-cache accounting.


## Native cache recovery and execution-profile construction (not activated)

Host-only recovery can invalidate a lost pending/finishing claim after the caller
independently proves the exact native child quiescent and the durable source turn
is terminal. It compares the stored source-turn identity and CAS revision, never
publishes a native handle, and leaves canonical history intact. Running turns,
source failure, stale recovery and late finalization refuse. Source scanning now
rejects unknown lifecycle statuses. Finalization also refuses another turn's
canonical source activity after this turn began: a late child cannot attest a
prefix containing another execution's messages.

The runtime can construct a separate cache execution scope from its captured
account/model lineage, canonical session, capabilities, role/toolset/denials,
write/shell/knowledge ceilings, path guards, source authority and delegate context.
The host must additionally supply attested digests for the actual SDK build,
effective launch configuration (sandbox, built-ins, hooks included) and stable
workspace lease. Missing attestations refuse; process pointers and credentials
are not profile identity. Denials are treated as a set. Per-turn counters,
snapshot IDs and local transport nonces do not invalidate a stable profile.
Account replay provenance remains a distinct binding.

These APIs still are not wired into production SDK spawning. The actual caller
must qualify and supply those SDK/resource attestations, full native adoption and
compaction fidelity, effects/human controls and credential custody before that
cutover. The production KB-free and independent promotion-review requirements
remain in force.


## Native compaction observation and cache refusal

Native adapters no longer ignore Codex `thread/compacted` / `contextCompaction`
and Claude `system.compact_boundary`. They emit the shared typed host-only
`native_compaction` observation (harness only, no native summary). C/Go/browser
admit that closed contract; durable history reconstruction excludes it from model
messages. The cache controller refuses to checkpoint the rewritten native prefix.
After known-quiescent abort/recovery, subsequent execution uses fresh canonical
adoption. This does not certify compaction fidelity or activate production SDKs.

Native parser and controller fixtures cover both SDK boundaries, capture refusal,
compaction followed by successful process/turn completion, and fresh adoption
instead of reviving a rewritten handle. Shared Go IR/family race tests and all
292 frontend tests plus TypeScript pass. Final sanitizer/static/registration and
production build qualification is recorded with this checkpoint.


The checked-in real SDK qualification `src/tests/test_codex_native_adoption.py`
now also resumes an existing thread AND imports only an unseen canonical tail
through the compiled C adapter. It checks unchanged native thread identity,
exactly one copy of each prior/new call-result pair, tool namespace and argument
fidelity, roles, and exactly one original/new user prefix. The existing CI build
job runs this fixture. It passes locally against installed Codex 0.160.1 and the
CI-pinned 0.153.4 native executable, in addition to the adapter ASan/UBSan fixture.
The pinned npm JavaScript wrapper initially lacked Node in the deliberately
restricted local child PATH; invoking its packaged native executable qualified
the pinned SDK without widening the child environment.


## Real native command-item fidelity finding

A credential-free loopback probe through the compiled adapter dispatched only
`printf command-fixture` in a fresh temporary HOME. The actual provider request
contains `exec_command` with `{"cmd":"printf command-fixture"}`. SDK observations
currently reconstruct a synthetic `commandExecution` tool with wrapped shell
arguments/cwd; their result also differs from the exact provider-visible output
wrapper. Call/result IDs match, but that is insufficient to certify complete
native history adoption. Do not relabel this as a qualified native prefix or
activate cache checkpoints for those records merely because the process passed.
Artifacts: `/var/tmp/aimee-native-command-probe.py`,
`/var/tmp/aimee-native-command-probe-report.json`, and
`/var/tmp/aimee-3050-native-command-qualification.log`.

Published head `1c8eb94ed` passed all 56 applicable CI checks, including Docker T2
and T3; the two existing platform skips remain. This precedes the recovery,
execution-profile and compaction increments, which require fresh CI.


## Built-in catalog canonical construction

The server's built-in catalog now constructs validated owned IR tool definitions
before native rendering. Chat/Responses compatibility builders use the translation
owner's existing renderer, retaining their declared surface membership and live
Git-dispatcher availability. A fresh owned canonical built-in catalog is available
for the SDK host-tool bridge without recursively building MCP-derived adverts or
sharing mutable schema cache entries. It is not itself role/policy authorization.
The existing full agent and MCP surface fixtures retain their success paths and
now check canonical/schema fidelity and independent ownership. Dynamic MCP/remote
catalog conversion and the native command-item identity gap remain open.

Legacy role-filter compatibility ingress now uses the translation parser and
makes its decisions from admitted IR tool identities. It no longer reads native
`function.name` / flat `name` itself. Malformed schemas are excluded before role
eligibility; Chat, Responses and Anthropic catalogs have planted malformed-entry checks.
Valid catalog entries and captured role/toolset semantics are preserved.

The actual agent runtime now supplies its catalog wire explicitly for role
filtering. Anthropic request admission and compatibility role filtering share the
same owned tool parser, so Anthropic `input_schema` is never mistaken for an
unmodeled OpenAI option. This retains hosted-tool, strict/deferred and cache
metadata in their declared translation owner.

Catalog-construction failure now stops the tool-enabled runtime before dispatch
and propagates through the Anthropic compatibility builder, instead of silently
becoming an empty tool catalog. The module-header dependency caught by lint was
removed using format-specific compatibility entry points; the bus boundary gate
remains unchanged at 188 declared crossings. Published head `db176e1e3` passed all
56 applicable CI checks (plus the two existing platform skips), including the
checked-in pinned real SDK replay journey. This catalog increment needs fresh CI.


## Authoritative post-policy request egress

`agent_build_request_ir` now returns the complete translation backend result after
canonical admission and policy. It no longer merges only conversation/tool fields
into an earlier native profile. Qwen no-think shaping operates on the owned IR
request before policy, retaining string/media/tool-result behavior and leaving
durable source history untouched. Split-text existing prefixes remain idempotent.

The OpenAI translation owner retains a closed set of same-wire provider profile
extensions (min_p, repeat_penalty, chat_template_kwargs, route, models and
reasoning_split). Typed fields always win; unrelated/private root keys are not
replayed. Those model-specific extensions are discarded if the typed model differs
from the entry snapshot, preventing stale routing or MiniMax options on a switch.
Anthropic retains its existing translation-owned cache-overlay behavior.

Focused actual request-path fixtures compare existing Ollama/Qwen, OpenRouter,
MiniMax, Mistral and OpenAI profiles, including canonical-source isolation and
empty/split user text. All three protocol/delegate-policy fixtures and backend
projection tests pass, alongside ASan/UBSan and the production build. Initial
profile preparation still uses compatibility builders before IR admission; moving
that remaining ingress to a typed host profile is unfinished. Native SDK activation
and the other open qualification/promotion requirements above remain open.


## Typed host Responses profile and sampling resolution

Responses execution now constructs its host model, instructions, streaming and
forced evidence-tool choice directly as owned IR. Its compatibility tool catalog
uses the shared typed tool parser, but no empty Responses request is built and
reparsed. Default instructions, preferred code_search/read_file selection, first
available-tool fallback, delegate filtering and malformed-schema refusal retain
their existing contracts. Codex-unsupported output-token/temperature knobs remain
absent from Responses egress.

Sampling resolution is available as a typed host value: required model temperature
precedes caller preference, opt-in recommendations and provider fallback. Existing
Chat/Anthropic compatibility builders share that resolver; curated recommendations
are unchanged. This prepares their remaining native profile ingress cutover.
The focused request fixture now also exercises opt-in Qwen sampling through actual
canonical egress. Source/ASan fixtures and production build pass; module/lint/static
checks are recorded with the slice.


## Typed host Chat profile

The tool-enabled Chat execution profile now starts directly as canonical IR too:
model, output cap, sampling, system blocks, admitted tool catalog, forced/automatic
tool choice, serial-call restrictions and reasoning effort. Provider-specific
min-p/repetition settings, local thinking switches, MiniMax split reasoning and
OpenRouter fallback routing enter a closed typed translation API; their native
representation is built only by the translation owner. Existing selected-first
fallback ordering/deduplication and vendor-specific settings are retained. The
helper refuses to overwrite an inbound native sidecar or operate across wires.

Chat and Responses no longer build empty native requests to rediscover their host
profiles. Anthropic still uses the compatibility profile pending preservation of
its exact scalar/block system-cache form; do not erase its native cache overlay
merely to remove the remaining call. Existing request/profile, delegate-policy and
translation fixtures pass through the new Chat path.


## Typed Anthropic host profile with cache-form preservation

All three tool-enabled host request profiles now start directly in IR; the
execution request path no longer constructs/parses an empty native request. The
Anthropic translation owner prepares canonical system blocks and retains only
the scalar/block system-cache representation in a translation-owned sidecar. That
sidecar contains no alternate model, tools or conversation. Typed sampling, output
cap and tool choice are built by the host, and compatibility catalog ingress uses
the shared Anthropic tool parser.

The actual request fixture compares exact system wire bytes against the existing
cache helper across 24 combinations of missing/empty/stable/volatile/whitespace
system text, cache enablement and size floors. Scalar form and stable-prefix-only
markers are preserved. The IR bypass gate now rejects native profile rebuild calls
inside all four migrated request/profile functions, with planted regressions that
include nested/literal/comment braces; compatibility output boundaries remain
explicitly outside that migrated function scope. No exception ledger was changed.

Published `1a8469ebf` passed all 56 applicable checks, including all Docker tiers,
with two existing platform skips. Subsequent Chat/Responses/request-egress slices
were pushed together as `61bfa8290` and need their own fresh CI. This Anthropic
increment is newer still; SDK activation, complete command fidelity, resource and
credential ownership, remaining consumers, independent learning and production
promotion are still unfinished.


## Plain agent request consumer cutover

The plain (system/user) request builder now resolves sampling and cache structure
in IR before egress. It no longer patches native sampling/system fields after the
backend, nor removes native sampling fields after adaptive-thinking admission.
The translation owner supplies plain-turn layouts that preserve the existing
request bytes, including sampling-at-tail and Anthropic system placement. Signed
Anthropic binding runs on the final rendered request. Existing adaptive capability
gates and compatibility stable-prefix behavior are unchanged. Allocation failures
refuse cleanly instead of falling back to an unshaped prompt or dereferencing a
missing block allocation.

The unchanged OpenAI/Anthropic/Responses byte goldens and adaptive-thinking tests
pass. Added actual plain-request checks retain opt-in Qwen sampling, Anthropic's
exclusion of min-p and Moonshot's hard temperature constraint. The consumer joins
the canonical-files bypass gate; a planted rule rejects reintroduction of native
sampling/cache/message shaping there. This raises that focused guard suite to ten
passing tests without editing the exception ledger. Fresh sanitizer/build/lint
results are recorded with the slice.


## Retire unused flattening/terminal shell drivers

Source call-site inspection found no production caller of
`agent_execute_cli_shell_driver`: only the old full-agent fixture exercised it.
Removed that prompt-flattening text collector, its unused registry/header, the
Claude pipe/PTY terminal collectors and the output-discarding Codex stub from both
Make and CMake. The structured Claude headless and Codex app-server adapters are
retained. Two obsolete fixture functions (three cases) solely exercising the
removed generic adapter were retired; active native adapter, cancellation, tool,
permission and history fixtures remain. The IR gate now rejects the retired shell
APIs/event types and has planted regressions; no exception was added.

Read-only installed-CLI regression checks now succeed without explicit project
overrides from both the Aimee continuation worktree and `/home/virant/dev/wol-lore`,
returning only `aimee` and `wol-lore` hits respectively. This directly rechecks the
original scope-required failure. It does not claim production SDK activation or
promotion of the newer native callback cwd binding.


## Selected Vault API-key lineage for scoped native transport

The Vault owner resolves selected API-key connections under the captured principal
without config/environment/default-account/name-alias fallback. It commits the
actual key and exact registration/provider/endpoint/auth/header target to opaque
lineage, rejects competing credential headers, and supplies a per-request host
binding. Serving uses the exact checked credential, refuses target edits within
that registration and truncated headers, and never swaps its source after admission.
Other registrations resolve their own credentials, without borrowing this key.
Snapshot/restore carries and cleans the API-key binding alongside Codex credentials.
Ordinary unscoped credential compatibility is unchanged.

The scoped native transport can now forward using that API-key lineage as well
as Codex OAuth. It rechecks the selected source for each callback, refuses a
rotated/revoked source in the old runtime, and permits a new runtime with a new
binding. API-key bindings are installed only for model forwarding, so MCP tools
and their delegates resolve their own credentials. Unsupported/missing native
credential sources retain MCP-only operation,
without model replay/cache authority. No key reaches the child or canonical log.

Actual Vault-owner and runtime fixtures exercise selected-key custody, stable and
rotated lineage, exact headers, no alias fallback, credential snapshots, source
changes and host-only forwarding. Two concurrent owner threads retain distinct
checked keys. Production build, ASan/UBSan owner/runtime/fan-out fixtures, runtime
ThreadSanitizer, CMake/CTest, all 76 lint checks and zero-diagnostic changed-source
static analysis pass. Claude OAuth refresh/custody, actual attestation
factories, native command fidelity, complete effects/controls and SDK activation
are still open; this API-key capability is not a production promotion.


## CI repair: unchecked POSIX capture output receiver

CI for `46f6e1085` failed Cppcheck shard 0 on `ctunullpointer` in `posix/util.c`:
the capture helper dereferenced a missing output receiver before validating it.
It now refuses that call before spawning a child. A focused regression covers
null receivers, invalid argv clearing stale output, and unchanged valid capture;
normal and ASan/UBSan fixtures pass. The exact production Make Cppcheck shard
rerun checks all 124 sources and passes with 99 known diagnostics and no increase.
Neither the analyzer debt baseline nor attribution policy was changed. The
aggregate static-analysis check failed from its Cppcheck dependency.

The selected API-key commit was caught locally with a named-file staging omission
of tracked `agent_credentials.c`; the incomplete unpublished commit was replaced
using the registered tracked-files commit mode after checking the exact eight-file
diff. `b2ebad48c` contains the full tested implementation. The first registered
commit request also returned a transient explicit-workspace availability refusal;
a retry against the same explicit checkout succeeded. No other checkout was used.

## OAuth accessor validity repair: 19 October continuation

The high-level Vault OAuth accessor now applies the same expiry check as the
lower-level loader and refuses `REAUTH_REQUIRED` before and after refresh. A
transient refresh failure can still use an unexpired access token; it cannot
return an expired token. Failure clears the caller's output. The previous
implementation reproduced the expired-token regression before this repair.

Real Vault fixtures cover fresh/expired/unknown-expiry access, marked rejection,
transient refresh before and after expiry, `invalid_grant`, and successful refresh
through a private fake curl. OAuth/PKCE and reauthentication fixtures pass normally
and under ASan/UBSan; changed-source Cppcheck has no diagnostics. This repairs the
shared accessor already used by selected Codex credentials. Claude custody and
production SDK activation remain unfinished.

## Selected Claude Vault custody and native broker: 19 October continuation

The selected native broker now supports the sealed Claude login document as well
as Codex and API keys. The Vault owner validates native `claudeAiOauth` fields,
converts millisecond expiry, preserves persisted client ID and exact granted
scopes, and uses the fixed subscription refresh endpoint. The legacy public
client fallback and document/profile fields were checked against the installed
Claude 2.1.227 SDK's public source, without reading operator credential files.
Codex and Claude share the same registration refresh lock/source fence. An
unchanged imported document cannot overwrite a refreshed token; removal revokes
it, and switching vendor families cannot reuse the other family's normalized
access token. Malformed/duplicate source fields and credential headers refuse.

The shared OAuth owner now supports typed JSON/form refresh profiles. Requests
have bounded curl deadlines and correctly quote the endpoint; refresh bodies stay
on stdin, full-length form tokens are encoded without truncation, and temporary
refresh material is cleansed. Only a successful HTTP status can store a token;
only an actual `error: invalid_grant` marks rejection. The existing form API is
retained. Model callbacks bind the exact selected Claude bearer and target in the
credential snapshot. Rotation/revocation refuses an old runtime. Tool/delegate
callbacks do not inherit that model bearer. Anthropic parity header rebuilding
preserves the subscription OAuth beta capability alongside caller beta flags.

Seven focused suites (agent, native runtime, three Anthropic ingress/policy suites,
OAuth reauthentication and PKCE) pass normally and under ASan/UBSan. Native runtime
also passes TSan, and actual CMake/CTest native runtime/cache targets pass 2/2.
The full agent fixture retains its three explicit PostgreSQL-dependent skips
when `AIMEE_STORE_URL` is unset. Changed-source Cppcheck has no diagnostics.

These are local real-Vault refresh/owner and loopback broker qualifications, not
live Claude subscription acceptance. Production SDK spawn sites remain inactive.
The normalized OAuth store still writes access/refresh/expiry as separate Vault
fields; atomic rotating-token persistence and failure injection remain required
before claiming custody complete. Attested launch/workspace construction, exact
native command replay, full effect/human tool admission and independently graded
KB-free learning also remain unfinished. Production `.100` remains KB-free.

## Fresh Claude broker CI scanner finding

CI on `b70c51d8f` flagged the publicly distributed Claude subscription OAuth
client ID in the legacy-client fixture (`generic-api-key`, test_agent.c:1916).
The exact commit/file/rule/line finding is recorded in the existing reviewed
non-secret fingerprint ledger. No rule/path exclusion, history-scan reduction,
attribution exemption or gate change is introduced. Atomic-token work remains
local while this fresh CI failure is repaired.

## Atomic OAuth rotation/source seeding: 19 October continuation

The existing Vault file owner now stages related credential fields under its
write lock, encrypts all of them, and commits one atomic file replacement.
`vault_store_write_batch` / `vault_service_write_server_batch` reject duplicate
fields and unsupported storage without sequential emulation. The existing single
field setters use the same checked envelope staging. Allocation failures in
metadata/base64 construction can no longer persist an incomplete envelope.

OAuth refresh atomically updates access/refresh/expiry and rejection state. Native
Codex/Claude source seeding includes the source fence in that same commit and no
longer removes the previous credential first. No new token layout/store is
introduced. A refresh response without a refresh token preserves the existing
one; a new native source must supply its own. Explicit rejection reset prevents
legacy rejection metadata from resurfacing. Token validity comes from one
immutable access/expiry/rejection snapshot; missing legacy fields still migrate
through sealed Vault and custody failures cannot become missing optional fields.

Validation: normal and ASan/UBSan Vault store/service, OAuth reauthentication/PKCE
and agent fixtures pass. The store fixture injects 94 allocation failures, checks
second-field failure after staging the first, rejects unsupported/duplicate
updates, and exercises concurrent snapshots. The real OAuth fixture checks all
five source/rotation fields under five injected failures and concurrently
alternates valid/expired generations without exposing the expired token. Bootstrap
and privilege compatibility fixtures pass. Changed-source Cppcheck and all 76
lint checks pass; actual CMake/CTest native runtime/cache targets pass 2/2.

The real Vault/OAuth concurrency fixture passes TSan with `WITH_LIBSECRET=0` in a
separate build and private HOME, qualifying the container file-backend path. The
default libsecret-linked run reported a GLib `g_queue_pop_tail` race during legacy
backend discovery before the rotation worker started; that report is retained,
not suppressed or reported as passing. The post-fork credential-custody refusal
also remains enforced: request concurrency uses threads, while the store fixture
uses independently supplied test keys for its process concurrency.

This closes atomic rotating-token persistence, not overall native custody/SDK
qualification. Actual credential-free SDK spawn activation, launch/workspace
attestation, native command fidelity, full tool effects/human controls, remaining
IR consumers and independently graded KB-free learning remain unfinished.

## Canonical tool-schema validation: 19 October continuation

The delegate tool loop already found each callable in the post-policy IR
`dispatch_catalog`, but then `tool_validate` loaded an independent KB schema.
It now calls `tool_validate_ir` with the exact admitted `aimee_tool_t`. Registry
metadata can still disable a tool; it cannot replace the advertised schema after
the model's response. A newly admitted host function does not need a KB schema.
This does not change optional tool enablement or policy classification, and the
same canonical argument bytes still reach validation and the tool-service owner.

The regression supplies an admitted schema that intentionally disagrees with the
KB schema, verifies the admitted contract wins, checks missing/type/extra/malformed
arguments, preserves an operator disable, and refuses hosted/missing-schema input.
Tool validation and full agent suites pass normally and under ASan/UBSan; changed
C sources have no Cppcheck diagnostics. The bypass gate's eleven fixtures include
a negative plant refusing legacy validation/schema lookup in migrated consumers.
The separate native MCP side-effect lookup still requires full catalog-owned
classification; it has not been replaced or relabeled as complete by this change.

## Codex model-only request binding

The native broker now also restricts the per-request Codex token/account override
to model requests, matching the Claude/API-key callback behavior. MCP callbacks
still recheck the selected source lineage under the captured principal, but tools
and delegates resolve their own connection credentials. The loopback regression
checks that both the Codex token and account override are absent on MCP calls,
while model requests retain the selected credentials. Production build and the
native runtime fixture pass normally, under ASan/UBSan and under TSan.

## Attribution checks without discarding source CI

PR title/body edits previously cancelled long-running source CI because the strict
attribution job shared its workflow and concurrency group. The unchanged
`no-coauthor-trailers` job now belongs to `PR attribution`, with independent
concurrency and the same read-only permissions, commit identity/trailer checks,
and live opening-app/body checks. There is exactly one owner of that check name.
No new exemption or rejection-rule change is introduced.

Source CI still handles edited events and runs its complete suite. Known title/body
edits queue without cancelling an active source run; base changes, code updates
and unrecognized edits retain cancellation/restart behavior. Source jobs are not
skipped on metadata edits. Workflow regression tests exercise the actual
cancellation expression and preserve both event coverage and attribution checks.
The installed server's cancelled-check display remains unpromoted; source already
reports cancellation separately, while a current cancelled run still blocks merge.

## Native tool-owned policy metadata

The supervised tools owner now reads one explicit built-in/MCP metadata catalog;
the C fixture table is generated from those same bytes. The existing version-1
classification wire is retained. Version-2 requests on the same event/stage return
classification, primary policy category and a bounded category set. Complete
external names up to 4,095 bytes are length-framed; over-limit, embedded-NUL and
trailing input refuse without prefix truncation. Unknown names
remain unclassified, qualified remote capabilities remain external, and malformed
metadata/owner failure refuses instead of becoming an implicit `read`.

Isolated native MCP admission now uses the typed owner. Optional KB metadata may
still disable a tool or supply a custom operator policy label; it cannot replace
classification or remove owned categories. The required execution-policy owner
evaluates the complete set against one loaded policy. Adaptive session checks
retain those categories and bind them into attempt identity. Metadata is neither
an effect authorization nor a human approval. Broad multiplexers retain
conservative capability metadata; operation-specific refinement and full effects
remain required. The other legacy `tool_side_effect` consumers are not cut over
by this increment.

Coverage checks use actual built-in IR and flat MCP catalogs. C fixture and real
C-to-Go module-bus tests exercise every catalog entry, including version-1
compatibility and unknown/malformed refusal. Policy fixtures cover custom labels,
operator disables, a network block behind a read primary label, and category
forwarding to adaptive accounting. The bypass gate prevents the native MCP path
from restoring `tool_side_effect`.

The attribution workflow also passed live qualification at `66e8a1307`: a PR
body edit completed a new strict attribution run while source run 39255933705
stayed in progress and metadata source run 39256255876 queued. The preceding
`70cfaf072` source run and associated workflows completed successfully. The full
local script suite's only initial failure was a missing Go executable on PATH;
that three-case generator fixture passed with the installed Go toolchain.

Metadata qualification: production Make, six focused native fixtures and their
ASan/UBSan builds pass; real C-to-Go bus parity covers the complete catalog and a
307-byte qualified name. The bus sanitizer rerun uses the rebuilt Go executable;
the earlier stale-binary attempt is retained separately. Go tools/execution-policy
race suites and multicall tests pass. Actual CMake/CTest runtime/cache tests pass
2/2, all 76 lint gates pass, and changed-source Cppcheck has no diagnostic
increase. Four process-start calls in the touched bus fixture were moved outside
assertions. The generated catalog's memory-prefixed capability names are reviewed
as tools metadata in the existing ownership ledger; memory execution remains Go-owned.

## Fresh metadata CI benchmark fixture qualification

`79acd6c01` passed attribution but Benchmark Smoke rejected the added tools-owner
coverage assertions in the shared MCP fixture under its frozen S1 source
comparison. The existing reviewed host-integration ledger now reverses exactly
the new include and assertion block before applying the complete frozen
comparison. No protected path, LSP implementation, schema, golden, probe input,
threshold or attribution rule is excluded. A new negative-plant regression rejects
removed/weakened/duplicated coverage or replacement by an empty catalog.

The repaired frozen comparison passes all 18 focused S1 cases and the complete
786-case benchmark unit suite. Fresh CI is required for
this follow-up; a cancelled or superseded workflow is not a passing result.

## Remaining live tool-policy read fallbacks retired

The delegate loop and async tool execution API now use the same supervised
metadata/category authorization as native MCP. All live `tool_side_effect`
callers, its implementation and public declaration are removed. Its name is now
retired globally by the bypass gate. A missing/unknown tools metadata owner cannot
become a read. Optional registry disables and custom operator labels retain their
existing restrictions alongside the owned category set.

The real delegate context-refusal fixture explicitly supplies the tools metadata
fixture while leaving execution-policy absent, preserving its original gate
ordering assertions. It additionally refuses missing metadata before execution.
The full integration harness now checks a network policy block behind web_read,
ordinary file reads under that same restriction, and unknown-tool refusal.

An independent loopback SDK probe found original command call/argument/result
notifications on fresh Codex threads after experimental opt-in, but not after a
new process resumes the thread. The pinned upstream implementation explicitly
attaches resumed listeners without raw events. That experimental route alone is
therefore insufficient for durable native command fidelity. No SDK activation or
faithful resumed command capture is claimed; the gateway's canonical wire boundary
remains the stable owner to connect for that work.

The complete local integration run passes 159/159 with a fresh PostgreSQL database,
separate non-superuser migrator/runtime roles and no KB instance. Its 26 existing
service-dependent skips remain explicit, including real Docker delegation. The
three new API assertions verify a network-category block, a permitted file read
under that same policy, and unknown metadata refusal. Earlier missing-module and
missing-migration-credential fixture attempts are retained as setup failures, not
passing runs. Four focused native suites pass normally; context, agent and tool
validation pass ASan/UBSan. The context fixture's hook registration was moved
outside an assertion after Cppcheck reported its side effect. Changed-source
Cppcheck has no diagnostics, all 76 lint checks pass, and actual CMake/CTest
runtime/cache targets pass 2/2. Fresh `6a0b24e2f` benchmark CI is green; its main CI
snapshot had 52 passing checks, four pending and the two existing platform skips.


## Admitted arguments survive dispatch; repository bootstrap uses admission

The delegate executor now passes its exact admitted IR function and canonical
arguments to typed dispatch. That boundary rejects malformed/noncanonical input
and transfers the parsed object into the existing dispatcher without consulting
its legacy process-global schema cache. Permission checks, cancellation, effect
contracts, receipts and completion auditing still run. Legacy script/API ingress
retains its separately tracked normalization path. The bypass guard now prevents
the canonical delegate from reintroducing that cached-schema entry point.

Automatic repository evidence must find `list_files` in the actual post-policy
catalog, canonicalize and validate against that definition, and pass directives
and supervised metadata/category execution policy before dispatch. An absent
policy owner cannot produce successful evidence. Both defects were reproduced
with failing native fixtures before repair. The stale-cache control changes an
advertised integer field to a string and verifies that the executed value stays
a string; refusals cannot increment the provider's call count.

The new tools-to-IR header declaration is a shared typed data-contract use in
`IR_SHARED_TYPE`, not a peer service call or a policy bypass. No new peer service
coupling or attribution exception is introduced. Exact admitted definitions and
strict JSON parsing stay at that boundary; policy metadata remains supervised.


## Unused name-only validation retired

`tool_validate` had no production callers after the canonical delegate cutover.
Its registry-schema fallback and permissive unknown qualified-tool path are now
removed, along with the declaration. The bypass gate retires the symbol globally.
Validation fixtures now take exact definitions from the tools owner's canonical
catalog, including canonicalization before validation and explicit refusal when
a local or qualified identity was never admitted. Nearest-name suggestion tests
remain separate; optional operator disables still have their original coverage.

Qualification of admitted dispatch/bootstrap passes five focused native suites,
three ASan/UBSan suites, CMake runtime/cache 2/2 and all 76 lint gates. The real
PostgreSQL integration rerun passes 159/159 with the same 26 explicit unavailable
service checks; no KB is configured. The validator retirement additionally passes
normal and sanitizer validation/context fixtures and changed-source Cppcheck with
no diagnostics. Fresh CI is still required after publication.

The full local native suite exposed two compiler portability issues in existing
Vault fixtures: message/junk arrays lacked space for their string terminator.
They now infer array size and explicitly exclude the terminator, preserving the
original signed bytes and wrong-size key file. A credential-isolation fixture
also now reads `/proc/self/environ` through `cat` before `tr`; direct redirected
`tr` locally returned no bytes. All original positive FD and negative secret
assertions remain. The first full-run invocation used unsupported absolute build
paths and did not launch binaries; the subsequent real run isolated this one
credential fixture failure, which passes independently after repair. The corrected full-suite rerun passes all 653 native test binaries and the
30-case proxy suite. The P1 PostgreSQL preflight
and store-dependent fixtures explicitly skip without their configured local
service; the separate restricted-role integration run remains 159/159. Final
retirement lint passes all 76 gates. Published `2b5aaa6d5` is fully green in fresh
CI, including all Docker tiers and attribution, with its two existing platform
skips. That CI result does not qualify the subsequent local commits.


## First-class tool API uses admitted canonical arguments

`tool.execute` now resolves an owned definition from the current execution
catalog, admits strict object JSON, canonicalizes once, and validates that exact
schema before directives, workspace/guardrail and supervised metadata/category
policy checks. Typed dispatch receives those same arguments. Each request gets a
host-random attempt identity rather than sharing `external-tool` across unrelated
requests. Missing capabilities, duplicate JSON fields and trailing bytes refuse.

The tools context owner resolves built-ins directly and MCP-derived definitions
from their live provider. Qualified plugin definitions cross the existing explicit
compatibility catalog boundary; unrelated entries do not become preconditions
for a valid requested capability. No process-global cached schema authorizes
this API. The bypass gate now also protects this consumer from legacy dispatch.

Real restricted-role PostgreSQL integration passes 163/163, with 26 existing
service-dependent skips and no KB. The four additional assertions cover canonical
and aliased path-policy parity, trailing argument bytes and duplicate argument
fields. Fresh-definition dispatch passes normally and under ASan/UBSan;
changed-source Cppcheck has no diagnostics. The full native run passes all 653
binaries and the 30-case proxy suite, and all 76 lint gates pass.

Script RPC still needs its caller's execution scope carried into its worker and
its own admitted-definition cutover. This API increment does not claim those,
production native SDK activation, resumed native command fidelity or independently
graded learning complete. Fresh published `591d791e2` benchmark, attribution and
non-main workflows pass; its source CI remains in progress at this checkpoint.


## Script RPC retains caller tool permissions

A real-dispatch regression reproduced a denied `read_file` succeeding through
script RPC and returning a private fixture file. The RPC worker now restores an
owned immutable copy of the caller's role, toolset, deny list, shell/knowledge
permissions and parent-write guard. Cleanup joins the worker before freeing that
copy, including failed startup paths. The stop flag is atomic rather than volatile.
No per-call effect authorization or model credential is copied.

The regression now refuses the denied read, proves the parent scope is unchanged,
and permits a real read after the caller clears its restriction. Framing tests
also refuse startup when permission capture fails. Agent and script-runner suites
pass normally and with ASan/UBSan; changed-source Cppcheck has no diagnostics.
All 653 native binaries and all 76 lint gates pass; serial CMake runtime/cache
checks pass 2/2. An earlier CMake attempt overlapped lint's deployment-env negative
plant, which temporarily edits a source file; that failed attempt is retained and
is not counted as a pass. A local mixed absolute/relative object dependency cache
was normalized before the actual permission regression qualification.

This is the tool-permission handoff. Script RPC's caller identity/cwd/source and
workspace handoff, canonical definition/policy admission and cancellation remain
separate outstanding work; production native activation and independent learning
qualification are still unfinished. The previous tool API integration remains
163/163 with its 26 explicit service skips, not a new integration claim for this
script increment.


## Script RPC owns cwd, identity, source and workspace context

A new real-dispatch regression reproduced an absolute read succeeding while the
same script's relative `evidence.txt` read used the server process directory and
failed. The RPC owner now captures the actual script cwd, immutable caller tool
scope, checked source-authority copy, attested request snapshot, session and Vault
principal identity, active workspace provider, job identity and cancellation flag.
The private execution context outlives its joined worker. Source allocations and
verified request-token snapshots are released/cleared after join; caller resource
leases enclose the synchronous script execution.

Workers retain model execution limits and remove operator-only capability bits.
Only session/principal identity is bound into the credential context; parent model
credentials are not copied. An absent source overlay is explicit, preventing a
concurrent delegate's process environment from becoming authority. Human control
scopes/mailboxes are not copied. Their retained owner integration remains separate.

Real fixtures prove relative reads, the caller's actual provider, request/principal,
source restrictions and shared cancellation flag, empty Codex/Claude/API-key fields,
and unchanged parent authority. The provider cancels the host flag after one read;
the next RPC request refuses without another file read. This qualifies RPC
cancellation admission, not termination of an arbitrary running Python/shell process.
Permission or source capture failure prevents script startup.

Agent/script framing fixtures pass normally and under ASan/UBSan; changed-source
Cppcheck has no diagnostics. All 653 native binaries, the proxy regressions and
all 76 lint gates pass. Serial actual CMake runtime/cache checks pass 2/2. Script
RPC canonical definition/policy admission and running-process cancellation remain
the next work; production SDK activation and independent learning remain unfinished.


## Script RPC admits canonical definitions through supervised policy

Script callbacks now resolve a fresh owned execution definition, admit strict
unique-field JSON, canonicalize and validate against that definition, then apply
caller permissions, directives and supervised tools/policy metadata before typed
IR dispatch. Host-generated attempt identities bind policy receipts to each call.
Missing tools or execution-policy owners refuse; legacy schema caches and
name-only dispatch no longer authorize this ingress.

A real Go-owner fixture proves allowed aliased reads, path-policy refusal,
permission/source/cwd propagation and refusal after either owner disappears. The
production classifier uses the full-name metadata contract, including a 307-byte
qualified tool name; the legacy wire ABI remains unchanged. Framing fixtures
reject duplicate/trailing/NUL-bearing requests. Socket shutdown interrupts partial
frames, and closed peers cannot terminate the host with SIGPIPE.

Restricted-role PostgreSQL integration passes 167/167 with the 26 existing
service-dependent skips and no KB. The fixture now creates an owned session
through HTTP; reads must return file content rather than merely an HTTP success
wrapper. Normal and ASan/UBSan agent/script fixtures pass, all 653 native binaries
and proxy regressions pass, changed-source Cppcheck has no diagnostics, focused
benchmark checks pass 21/21, and actual CMake runtime/cache checks pass 2/2.
All 76 lint gates ran: 75 passed initially; the memory boundary review ledger
then identified the added tools-only header declaration. Its unchanged memory
contract is recorded with the exact source hash, and that gate passes on rerun.
Published predecessor `4e0ca19c2` has all fresh workflows green. New-head CI must
be checked separately after publication.

Running-process cancellation, bounded RPC output fidelity, retained human
controls, production native SDK activation, exact resumed native history and
independently graded KB-free learning remain unfinished. This increment does not
qualify production deployment or the two skipped platforms.


## Running script cancellation and bounded result transport

Host cancellation is now checked before launch and throughout script execution.
The owner polls at most every 50 ms while waiting for output or child exit, so
closing stdout/stderr no longer escapes the timeout. Cancellation/timeout kills
the owned process group, including TERM-resistant processes, and the parent reaps
the child. Results retain separate `cancelled` and `timed_out` fields. The tools
owner maps cancellation to the existing error/cancelled outcome and timeout to
its timeout outcome; neither becomes ordinary success from output text.

The running-script regression first failed against the old loop. The fixed real
host-flag fixture cancels after the child signals readiness, including a child
that ignores SIGTERM. A separate fixture closes both output descriptors and
still times out. The real provider-scope fixture now expects cancellation to
terminate the waiting script, preserving its original actor/source/credential
and single-read checks.

A second reproduced defect silently cut results at 64 KiB and then exceeded the
64 KiB encoded frame limit, sending no reply. RPC now measures the complete JSON
frame. Complete fitting results remain intact; oversized results return a bounded
UTF-8-safe text preview with `result_truncated:true` and the original `result_bytes`.
The actual tool success/error status remains unchanged. JSON-looking results use
strict unique-field full-frame parsing; invalid/trailing/duplicate-bearing text
remains verbatim text. Tests cover escaped text, error duplication, multibyte
boundaries, a complete 64,000-byte result and repeated callbacks after a projection.

Cancellation qualification passes all 653 native binaries and proxy regressions.
Final combined agent/script fixtures pass normally and under ASan/UBSan, and five
changed-source Cppcheck inputs have no diagnostics. The actual CMake application
and runtime/cache targets build; CTest passes 2/2. The restricted-role PostgreSQL
integration run passes 167/167 with the 26 existing explicit service skips and no
KB. Focused benchmark qualification passes 21/21. All 76 lint gates pass on the
serial run, avoiding the negative plant racing a build.

GitHub reports #3050 merged at `4e0ca19c2`, merge commit `7e17d58be`.
The subsequent canonical script-admission commit `5cf885b4c` is pushed on the same
branch but has no PR workflow run because that PR is closed. The handoff's
existing-PR-only instruction is retained pending the user's follow-up PR decision;
no replacement PR has been created. New commits are not claimed CI-qualified.
Native activation, exact resumed command history, retained human controls and
independently graded KB-free learning remain unfinished; no production promotion
has occurred.


## Matching host bash cancellation/deadline repair

The production `bash` tool had the same EOF-then-blocking-wait defect as the
script runner. Its cancellation predicate also consulted durable delegate/job
state but omitted the host turn's atomic cancellation flag. A new real tool
fixture reproduced a TERM-resistant command closing both output streams and
returning successful exit after cancellation. The owning tool now checks that
flag, refuses pre-cancelled execution, polls child exit without losing its
deadline after EOF, and preserves separate cancellation/timeout fields for the
tools owner's semantic outcome classification. Existing sandbox/provider routing
and ordinary command/output behavior remain intact. Active remote/container
provider cancellation is not newly qualified by this local-process repair.

The fixture signals readiness before the host cancels, verifies cancellation
through the real tool, proves a subsequent pre-cancelled command creates no file,
and separately verifies timeout with both output descriptors closed. It passes
normally and under ASan/UBSan. Changed-source Cppcheck has no diagnostics. All
653 native binaries and proxy regressions pass; the actual CMake application
build and runtime/cache checks pass 2/2. The rebuilt server passes restricted-role
PostgreSQL integration 167/167, with the same 26 explicit service skips and no KB.
All 76 serial lint gates pass.


Historical script ingress/lifetime findings (fixed by the next increment): the generated Bash shim uses Python
`raw_decode`, which accepts a JSON prefix and collapses duplicate fields before
host admission. It must forward exact argument JSON to the strict host boundary.
Also, RPC stop must propagate a local cancellation scope into an executing tool;
a script-only timeout currently closes its peer but can still wait for a blocking
nested call's original deadline. Keep the borrowed parent flag and host-owned
local-stop scope distinct so a tool timeout never changes its parent turn flag.
Neither finding is fixed by the already qualified host-flag repair.

No registered Aimee command for manual workflow dispatch is available. The local
personal `gh` has no authenticated host, and no browser is available. No new login
or connector write was attempted. A follow-up PR decision remains pending; the
new branch commits have no fresh PR CI qualification after #3050's merge.


## Merged testing CI: Windows thin-client codec linkage

The merged testing commit `7e17d58be` has a real Windows build failure in source
CI run `39278223901`, job `118147618843`. The final MinGW link cannot resolve
`aimee_codec_base64_capacity`, `aimee_codec_base64_encode` and
`aimee_codec_base64_decode` from the thin client's shared `util.c`. The independent
CLI target omitted the codec library even though the server/core target included
it. Its CMake link now explicitly names the existing `aimee-core-codec` owner.
No duplicate codec implementation or platform exemption is introduced.

The actual local CMake CLI build succeeds, its generated link command contains
the codec library, and the real core base64 vector fixture passes 1/1. This is
local verification of the repair; a fresh Windows CI run is still required.
The same merged-run macOS build and macOS real-LSP qualification passed. Linux
integration and T2/T2-luks were still pending when the Windows failure was found;
T1/T3 had passed. The merged PR's two platform skips never qualified Windows.


## Exact script shim ingress and local cancellation leases

New regressions reproduced the generated Bash shim accepting trailing JSON and
collapsing duplicate fields before the host saw them. It now forwards exact
argument JSON as encoded text and exact response bytes through the bounded
transport. The host decodes that text independently, preventing trailing argument
bytes from becoming envelope metadata. Legacy object frames and descriptor-only
authentication remain supported; ambiguous dual argument forms refuse. The strict
host codec remains the admission owner, including decimal-token fidelity. The
shim's old default-argument expansion also appended a spare closing brace; that
is corrected rather than relying on prefix decoding. Python calls preserve the
explicit argument type: an empty array no longer silently becomes an object.
Header/body reads and packet writes now handle partial socket I/O and always
close the duplicated client descriptor.

A real tools/policy owner fixture also reproduced a script-only timeout waiting
for its nested `bash` tool. The host now has a typed chain of immutable, leased
cancellation nodes. The RPC worker retains its parent's flag/scope and adds its
own atomic stop flag. Stop reaches running cancellable tools without changing
any parent flag. Every enclosing scope outlives its joined children. The real
context fixture separately qualifies ordinary parent cancellation and inherited
scope cancellation with the original identity/source/credential checks intact.

Agent/script fixtures pass normally and under ASan/UBSan; all 653 native binaries
and proxy regressions pass; five changed-source Cppcheck inputs have no diagnostics.
The actual server builds and passes the 167-check restricted-role integration
journey against PostgreSQL 18.6 extracted from the exact CI OCI manifest, with
26 existing service-dependent skips and no KB. That diagnostic harness bounds
memory teardown and preserves its owner log; no stop failure occurred. All 76 serial
lint gates pass for the final encoded-argument and local-scope implementation.

Merged CI integration run `39278223901` did time out after a private model-memory
store failed and memory-owner TERM teardown hung. This remains an open CI defect,
not a passing or superseded run. Clean local PostgreSQL 17 integration passed
167/167; a two-thread Go scheduler with 25 extra model writes passed 192/192;
the exact CI PostgreSQL 18.6 image also passed 167/167. The four merged Docker
tiers and macOS passed; the separately pushed Windows codec repair still needs
fresh Windows CI. Investigation is comparing the exact merged C source next.


## CI memory teardown retains a failing verdict and owner evidence

The integration harness now bounds its disposable memory child’s TERM shutdown
at five seconds. A stuck owner records a real test failure, emits Go stacks with
QUIT and is killed/reaped before the outage/restart checks continue. Assertions
are retained. Cleanup captures the entry status and forces a nonzero exit for a
shutdown failure, including a failure discovered after the printed summary.

Three real-process fixtures pass: graceful reaping without a failure; a child
ignoring TERM/QUIT that is killed and makes final cleanup exit 1; and preservation
of an existing exit 7. The frozen benchmark checks pass 21/21 without changing any
baseline or validator. The actual updated integration harness passes 167/167 on
the exact CI PostgreSQL 18.6 image, with the existing 26 service skips and no KB.
This fixes the harness’s unbounded wait and adds evidence for the unresolved
model-memory write timeout; it does not claim that production owner defect fixed.

A separate archive of the exact merged source, built with isolated objects,
passed the private-memory assertions against that same PostgreSQL 18.6 image.
That shortened diagnostic run is not full integration qualification: its archive
had no Git metadata, so an earlier git_status check failed, and it intentionally
stopped after private memory. The original CI model-write failure remains open.


Final script-scope qualification also passes the rebuilt production server,
actual CMake application/runtime/cache targets (CTest 2/2), and the updated original
integration harness against the exact CI PostgreSQL 18.6 image: 167/167, with the
26 existing explicit skips and no KB. The final raw-argument form cannot append
extra envelope metadata; both argument forms together refuse. The descriptor-only
legacy/HMAC-ignored control remains a positive test. All 653 native binaries,
ASan/UBSan agent/script fixtures, five-source Cppcheck and all 76 lint gates pass.
Fresh branch CI and the unresolved merged model-memory timeout remain separate.


## Retained canonical control scopes for joined execution callbacks

Script RPC workers and native MCP callbacks now capture an owned control-scope
lease bound to the attested session and principal. The creator closes the scope
immediately for new replies and requests; existing leases retain allocation
until callback joins and release. A delayed worker cannot register against a
replacement execution even when its session/turn identity is reused. Missing
control scope stays a supported case; cross-session/principal capture refuses.
Native model requests continue without a control binding. No HTTP compute
context, model credentials or session-event sink crosses the script boundary.

Browser replies reject duplicate JSON fields recursively before consuming a
pending control, including ambiguous decisions, answers and form content. The
existing positive permission, question/form validation and exactly-once reply
contract are retained. Focused fixtures cover creator-before-worker teardown,
owner/session capture, concurrent replies, real script context propagation and
control confinement in the actual native listener. This establishes safe control
leases; the host request_input durable publication bridge and native activation
remain unfinished.

Retained-control qualification passes all 653 native binaries and proxy
regressions; four focused ASan/UBSan fixtures (controls, actual native listener,
script runner and agent); six-source Cppcheck with zero diagnostics; the rebuilt
production server; actual CMake application plus controls/runtime/cache CTest
3/3; frozen benchmark assertions 21/21; and the original PostgreSQL 18.6
integration harness 167/167 with 26 explicit service skips and no KB. The CMake
control fixture now names its missing canonical IR source dependencies. Fresh
branch CI and the original merged memory-owner timeout remain unqualified.
All 76 repository lint gates also pass for this retained-control increment.

## Canonical host questions and strict browser reply frames

The server's existing request_input hook now publishes primary/browser and native
MCP host questions as canonical durable control requests. Its worker retains the
session/principal-bound control owner, carries no HTTP compute context or model
sink, and publishes an empty-option free-text question through the existing UI.
The existing always-on durable EventSource carries independently appended events.
Request IDs are host-random and turn-qualified; independent control event IDs do
not share a borrowed primary counter. DB1 remains the only history writer.

Durable publication and consumption run under a per-scope mutex. The registry
lock covers only membership and reference leases, so a blocked store for one turn
does not stall another turn's controls. A request is recorded before becoming
replyable, and a response is recorded before an answer reaches its waiting tool.
The native harness wildcard consumer cannot take recorded host-question replies.
Only their specific host ID selects them. A recording failure is shared with the
parent primary owner and refuses further model capture or successful completion.

Host cancellation and deadlines seal a pending question with a durable cancel
response; late replies refuse. Declines/cancels return error text, never an effect
grant. Invalid answers do not consume the request. The real server hook retains
its old delegate-mailbox fallback when there is no browser control scope. Private
execution logs still clear the parent's scope; their mailbox and question-routing
cutover remains unfinished and is not claimed by this increment.

The browser control route now parses the exact frame with the canonical JSON
codec before selecting session/request/reply fields. Duplicate outer fields,
trailing documents and embedded NULs refuse without consuming the pending
request. Authenticated positive routing and these negative controls were run in
an isolated user namespace, also under ASan/UBSan; untrusted transports retain
their existing authorization rejection.

Host-question qualification passes all 654 native binaries and proxy regressions;
seven focused ASan/UBSan fixtures plus authenticated HTTP reply routing in an
isolated user namespace; nine-source Cppcheck with zero diagnostics; actual
CMake application and controls/question/runtime/cache CTest 4/4; rebuilt
production server; and the original PostgreSQL 18.6 integration journey 167/167
with 26 explicit service skips and no KB. The two fixture setup/assert cleanups
were rebuilt and rerun normally and under ASan/UBSan. Frozen benchmark checks
pass 21/21 with no baseline or validator changes. These local results do not
replace fresh branch CI or explain the merged CI model-memory timeout.
The exact HTTP-route diff was reviewed for its memory-ownership ledger pin:
only control-frame decoding changed, with memory_stats and Go authority intact.
The recorded hash/review was refreshed; no memory gate or inventory was exempted.
All 76 repository lint checks pass for the final host-question increment.

## Delegate mailbox wait preserves reply delivery and observes cancellation

The remaining legacy parent-input wait now checks the active host cancellation
scope at bounded intervals rather than waiting the entire configured input window (currently 60
seconds). It retains parent reply delivery and consume-once behavior. The deadline
uses monotonic time; Linux initializes the condition with that clock and Darwin
uses its supported relative condition wait (see the [Apple pthread header](https://github.com/apple-oss-distributions/libpthread/blob/main/include/pthread/pthread.h)).
Condition initialization failure unwinds the newly initialized mutex and leaves
the slot inactive. This is a waiting/lifecycle repair, not the private question
or delegate message-history cutover.

The actual prior source was rebuilt against the new fixture: it fails the
three-second cancellation handshake. Restoring the repair passes. The fixture
uses the production mailbox allocator, wait, reply, release and real server
request_input hook: normal parent delivery is retained, cancellation returns
without a fake parent reply, pre-cancelled consumption refuses, and a real empty
mailbox waits for its monotonic deadline. A queued reply remains available to
its owning live wait and is consumed once. The rebuilt Linux production server,
actual CMake application and focused CTest pass. Darwin is not newly qualified.

The final mailbox-wait fixture also passes ASan/UBSan, two-source Cppcheck with
zero diagnostics, normal native execution, CMake/CTest and all 76 lint gates.
The preceding host-question commit `7230d5805` was pushed after one transient
GitHub internal-server rejection; full HEAD secret scanning found no leaks
across 6,198 commits. Fresh branch CI remains pending.

## Delegate mailbox reply leases survive creator teardown

The reply handler previously received a borrowed mailbox pointer from lookup;
creator cleanup could destroy its mutex or reuse its slot before the reply was
queued. Lookup now returns a retained lease. Creator release retires the mailbox,
wakes waiters and drops only its reference. Waiters and the legacy input hook
retain the mailbox while operating; synchronization objects and the slot are
reclaimed only after the last lease ends. Closed mailboxes reject late replies.
The sole production reply handler puts its lookup lease on both outcomes and
reports a closure race as an error instead of success.

Focused fixtures preserve normal parent delivery and cancellation, close the
creator while the real input hook is active, and hold a reply lease past closure.
A retired slot cannot be allocated to another delegate while that lease exists;
a late reply refuses without reaching the new delegate. The slot is reusable
after the lease is released, and a valid replacement reply is delivered once.
This retains legacy delegate conversation semantics; private canonical question
routing/history cutover remains unfinished. All 654 native binaries and proxy
regressions, the focused ASan/UBSan fixture and three-source Cppcheck pass.
The mailbox-lease increment additionally passes the actual CMake application
and focused CTest, rebuilt Linux server and original PostgreSQL 18.6 integration
harness (167/167, 26 existing service skips, no KB). No fresh Windows or Darwin
qualification or production promotion is inferred from these Linux results.
All 76 repository lint checks pass for the mailbox-lease increment.

## MCP delegation preserves model routing and the caller's workspace scope

The served delegate schema already advertises via, scope and handoff_json, and
the server delegate handler reads them. The MCP wrapper dropped all three.
It now forwards those fields with their declared types, including explicit
false; absent options remain absent so server-owned defaults still decide.
The transport-attested parent session remains authoritative.

Default cwd resolution previously cleared the caller's cwd/session override,
leaked resolver ownership state, and ignored inaccessible-worktree errors.
The lookup now restores the caller's cwd, explicit session override (or its
absence) and Git worktree flag on every outcome. It copies the resolved cwd
into the new request and refuses negative results or a context mismatch before
launching a delegate. A genuine unresolved optional cwd still reaches the server
without a cwd field, with the caller's original context intact. No persona,
capability or permission ceiling is widened.

Three independent new fixture modes fail against the prior wrapper: dropped
routing fields, leaked caller context and launching after failed worktree
resolution. The repair passes, including true/false options, omitted defaults,
transport-session binding, 127-byte caller sessions, explicit cwd, no override,
resolver errors and the existing persona/capability refusals. The focused
ASan/UBSan fixture and two-source Cppcheck pass. This also addresses a remaining
source of default-project confusion; no production installation is asserted.

The final MCP delegate increment passes all 655 native binaries and proxy
regressions, the focused ASan/UBSan fixture, two-source Cppcheck, actual CMake
application/CTest, rebuilt Linux server, original PostgreSQL 18.6 integration
167/167 (26 existing service skips, no KB), and all 76 lint gates. The unchanged
frontend suite also passes all 292 tests across 35 files. Production promotion,
fresh branch CI, native SDK activation/cache fidelity and independent learning
qualification are still outstanding.

## Browser host-question round trip

A frontend regression projects the durable empty-options question emitted by the
host bridge, enters free text, verifies the qualified reply, then applies the
canonical response and observes a resolved control. Control exchange creates no
model messages. The full frontend suite now passes 293 tests across 35 files,
and TypeScript project checking passes. Native/source qualification above is
unchanged; this test does not constitute production browser deployment approval.

## Retained canonical capture owner across joined callbacks

Primary and private producers now share a retained durable writer containing
copied session/turn/principal/source identities, append counters, a per-owner
mutex and a shared failure flag. It holds no connection, subscriber, compute
context or credential state. Delivery remains with the primary producer. Native
callback workers retain the writer until listener shutdown and restore their
previous binding on return; creator closure seals writes without freeing an
outstanding callback lease. A committed terminal seals later event writes.

Source identity and destination history remain distinct: a private child can
retain its own writer using its verified parent execution session and actor.
Message/control IDs are qualified once; original tool IDs, names, exact numeric
arguments and results are preserved. Append failures and callback-only capture
failures reach the producing execution even when the producer's TLS error flag
is healthy. Successful completion then refuses; private failed-terminal
recording remains available. Native replay still captures proofs only; this
refactor does not activate SDK spawn sites or establish complete native history.

Qualification: all 656 native binaries and proxy regressions, four focused
ASan/UBSan fixtures, nine-source Cppcheck, actual CMake application plus five
CTest fixtures, rebuilt Linux server, original PostgreSQL 18.6 integration
167/167 (26 existing service skips, no KB), and 21 semantic-context benchmark
checks pass. Fresh platform CI, full native wire capture/reconciliation, actual
SDK activation/cache qualification and independent learning qualification remain
outstanding. The frontend remains qualified at 293 tests plus TypeScript checks.

All 76 repository lint gates pass for the retained capture-owner increment.

## Private native replay metadata follows the retained destination

The native runtime verified transport ownership against the parent execution
session, then also used that session as the replay log destination. For a private
execution this put replay proofs and projection reports in the parent history.
The retained writer now exposes its immutable destination for the duration of
an owned lease. Replay storage uses that destination; transport/session admission
and callback context still use the verified source session. Runtime operation
without a capture owner keeps its existing source-session destination.

A listener regression fails on the previous source when a private child has a
parent transport. It now checks both primary/private destinations, callback
binding and failure/closure handling, while preserving the no-owner fallback.
Copied destination identity is also verified after mutation of constructor input.
Three native fixtures, their ASan/UBSan runs, four-source Cppcheck, actual CMake
application and CTest 3/3 pass. This repairs proof/projection placement; it does
not yet capture complete native model history or activate the SDK runtime.
The preceding capture-owner commit also passed a full-HEAD Gitleaks scan
(6,203 commits, no leaks). No production deployment or fresh platform CI claimed.

All 76 repository lint gates pass for the private replay-destination repair.

## Original gateway model responses reach the retained canonical writer

Native model callbacks now send their admitted canonical response events through
the retained writer before derived replay-proof capture. Tool IDs, names and
exact JSON arguments are observed before SDK command normalization. Projection
reports commit once through the writer rather than also through the proof log.
No-owner runtime operation preserves the existing proof-only behavior.

Each callback uses its host-generated request nonce to qualify message IDs.
Gateway response helpers can generate the same thread-local ID on fresh callback
threads; using only the host turn would let a later response replace an earlier
one in canonical history. Only message identity is qualified; original tool IDs,
arguments and content are unchanged. The callback-local sink context is restored
before the forwarding stack returns; the runtime retains the durable owner.

The previous source fails the new listener regression because it commits no
model responses. The repaired fixture verifies original command name/ID and a
30-digit fractional argument, distinct messages across two callbacks in one
turn, single projection publication, primary/private destinations, durable append
failure and proof failure. Either failure reaches the producer's shared flag
while its separate TLS flag remains healthy. No host execution terminal or
completed learning outcome is manufactured by the model callback.

Five native fixtures and their ASan/UBSan runs, two-source Cppcheck, and the actual
CMake application/listener CTest pass. This is gateway **response** capture, not
complete native history: request-prefix/current-context reconciliation, actual
tool-result/effect capture and SDK-observation duplicate suppression must land
before activating SDK spawn sites or certifying native cache completion. Live
subscription, independent learning, fresh platform CI and production promotion
remain outstanding.

All 76 repository lint gates pass for gateway response capture.

## Shared Git dispatch retains the caller's project context

The same lost-context pattern repaired in the delegate cwd resolver also existed
in the shared Git dispatcher used by external MCP and native agent Git tools.
Its inner cleanup cleared the caller's cwd/session override and leaked the Git
worktree flag. An outer scope now bookmarks and restores those values across
all existing success/refusal returns. It also owns an exact operation-session
copy because the supplied session can borrow the TLS override changed by worktree
resolution. Existing workspace, mismatch, mutation and Git verification decisions
remain in the inner dispatcher.

A new fixture links the actual production dispatcher with unrelated sections
removed by the linker. The previous source fails its scope assertion. Forty-eight
combinations cover success, both resolver errors, mutating mismatch refusal,
permitted read-only mismatch, unresolved cwd, present/absent cwd and override,
a 127-byte caller session and operation identity borrowed from TLS. The fixture,
ASan/UBSan, two-source Cppcheck and actual CMake application/CTest pass. The memory
ownership ledger records review of the exact Git-only source diff, with its
existing memory symbols and owners retained; no exemption or approval is added.

Nested workspace-provider ownership is a separate outstanding issue in this
same dispatch path: its legacy bind/unbind helper can disturb an enclosing turn.
The cwd/session/worktree repair does not claim to fix that resource lifetime.

The shared Git context increment additionally passes all 656 selected native
binaries, the separately run new Git scope fixture and proxy regressions, the rebuilt server, original PostgreSQL 18.6 integration
167/167 (26 service-dependent skips, no KB), and 21 semantic-context benchmark
checks. These Linux checks do not establish fresh platform CI or deployment.

All 76 repository lint gates pass for shared Git caller-context restoration.

## Nested Git workspace scopes retain the enclosing resource owner

Shared Git dispatch now uses a scoped binding with its own detached-provider
storage and mirror cwd/drift metadata. The scoped and ordinary turn binders use
the same resolver. Scope close restores the previous provider and metadata in
LIFO order; it never calls the enclosing turn's unbind or releases its container.
Even a shared/unresolved nonempty cwd has a scope that must be closed. Allocation
failure refuses before Git dispatch. Container-bound admission now also checks
that the active provider is that owned container, so a temporary detached binding
cannot inherit its shell exemption.

Real workspace-owner fixtures retain an outer detached provider across nesting,
and retain an outer container across shared and detached nested scopes. The
container's storage stays valid, no release occurs until its original turn ends,
and an out-of-order close refuses without changing the active provider. The real
Git dispatcher fixture checks paired cleanup across all returns, including the
zero/shared result. Its old unscoped path fails the new regression.

The production resolver also allocates a diagnostic before its unauthorized-path
-2 return. The dispatcher leaked that string. The expanded fixture reproduces
eight leaks under ASan; freeing the diagnostic preserves the existing refusal.
The fixture now covers 56 caller/outcome combinations. Both focused native
fixtures pass ASan/UBSan and four-source Cppcheck reports no diagnostics. CMake's
actual application and Git dispatcher fixture build; the workspace fixture is
Make-owned and has no pre-existing CMake target. All 656 then-selected native
binaries, the separately run Git fixture and proxy regressions, the rebuilt Linux
server and original PostgreSQL 18.6 integration 167/167 pass (26 service-dependent skips, no KB). Lint qualification follows
below; no deployment or fresh CI is claimed.

## Unit-suite registration follows Make's actual selection point

The new Git fixture's TEST_TARGETS addition initially appeared after Make's
immediate UNIT_TEST_TARGETS snapshot. Focused normal/sanitizer/CMake runs included
it, but the full runner still selected 656 binaries. The addition now sits before
suite/shard selection. The reachability gate rejects later TEST_TARGETS changes
instead of counting their names as coverage. Six regression cases, including
actual GNU Make execution of the silent-omission case, run from the existing
lint target. No test exemption is added. Final complete 657-binary qualification
passes, with the new fixture present in the actual runner log. The earlier 657 full-run description is corrected above.

The first nested-scope lint run also caught three added hardcoded fixture paths.
The new cases now reuse the fixture's named registered roots; duplicate existing
root literals share that same constant. Temporary-path debt falls without a
baseline change. No production behavior changed after the successful integration
run; final native/sanitizer/lint checks include the fixture and gate corrections.

The final registered 657-binary suite and proxy regressions pass, as do both
rebuilt focused ASan/UBSan fixtures and all 76 lint gates (including six runner
selection regressions). No temporary-path or coverage baseline exemption added.

## Bounded SDK catalog qualification with the actual MCP transport

The existing real-SDK adoption fixture now runs its complete ordinary scenario
and a bounded SDK configuration scenario. It preserves all prior namespaced
call/result, error-result, process-restart, fresh-adoption and unseen-tail
assertions, and checks every actual provider catalog in the additional scenario.
Both installed Codex 0.160.1 and CI-pinned 0.153.4 pass both scenarios through the
compiled Aimee adapter. This is a fixture configuration, not an activated server
launch profile or evidence of live upstream cache billing.

Independent loopback probes first checked the SDK without an MCP server, then
the expanded fixture caught additional SDK resource helpers when MCP was enabled.
The observed bounded catalog permits only the fixture's MCP tool, user input,
and the three MCP resource helpers. It rejects local command/edit/image, agent,
goal, web and other unexpected tools across fresh and resumed requests. SDK
feature listings identify features.view_image as effective for these versions;
tools.view_image=false alone still advertised view_image in the initial probes.
The fixture keeps hooks, snapshots, memories, apps and optional SDK execution
features disabled, without changing the ordinary scenario.

MCP resource helpers are NOT qualified as admitted Aimee tool execution here.
The existing CLI maps resource reads to direct read APIs, while the scoped native
proxy admits model and MCP tool routes. That resource ingress still needs an
owner-preserving cutover before native activation. Full native request-prefix /
current-context reconciliation and SDK-observation duplicate suppression remain
open, together with actual launch/build/workspace attestation, live subscription,
independent learning and fresh CI/deployment qualification.

Current official references: [configuration](https://learn.chatgpt.com/docs/config-file/config-reference)
and [app-server](https://learn.chatgpt.com/docs/app-server). The feature/call/catalog
observations above come from the two actual binaries, not from assuming that
current documentation proves the behavior of a pinned older SDK. The previous
workspace-source HEAD also passed Gitleaks (6,207 commits, no leaks).

All 76 lint gates also pass for the expanded real-SDK catalog fixture. The
unchanged native production code retains the preceding 657-test, sanitizer and
167-check integration qualification. Fresh PR CI remains pending the follow-up
PR decision; no new PR, attribution exemption or production deployment made.


## Owner-checked snapshots of an active canonical turn

The new sessions operation 35 loads primary history for an exact session, actor
and running turn. It reads the durable log in one statement, validates contiguous
sequence, event ownership and model-message admission, and includes the current
turn's committed observations. The completed-history operation retains its old
contract. Missing/closed turns, malformed history, unfinished checkpoints and
unrelated open turns do not produce a partial snapshot. Exact JSON numeric
lexemes and original tool identities survive projection.

The retained capture owner reads this snapshot using its copied destination,
principal and turn. Native runtime construction retains an immutable initial
snapshot and refuses absent/corrupt history. Private callbacks also recheck their
destination's ownership, in addition to the transport parent's; revocation fails
the shared capture permanently. This does not implement native request-prefix
reconciliation or activate SDK execution.

Qualification: the registered 657-binary suite and proxy regressions pass; five
focused ASan/UBSan fixtures, five CMake/CTest fixtures and the actual application
build pass. Three Go packages pass race tests, and the new live PostgreSQL 18
snapshot/ownership/checkpoint regression passes with race detection. Cppcheck on
eight affected sources reports ten known diagnostics and no increase. The DB1
client contract check passes. CMake's history-client fixture now links its
existing transport dependency, as its Make target already did.

The broader PostgreSQL integration run FAILED: 164/168 checks, with 26 existing
service skips. It reproduced the outstanding CI private model-store timeout and
blocked memory-owner shutdown; dependent model read/correction checks also fail.
Its captured stack includes a code-index initialization mutex wait. This remains
an active defect, not a flaky-test exemption. Full diagnostics are in
/var/tmp/aimee-ir-script-scope-pg18-integration.log.

The user authorized one follow-up PR after #3050 merged. PR #3052 targets testing
and has started fresh CI; all further migration work stays in that PR. Automatic
reviews and the attribution gate remain enabled. Production remains KB-free and
has not been promoted.

All 76 local lint gates and 21 semantic-context benchmark regressions pass for
the running-history increment. Fresh #3052 CI found a formatter difference in
the Git fixture: Ubuntu's clang-format 19.1.1 treats its generated stub group as
an unfinished declaration before subsequent functions, while local 19.1.7 did
not. Moving those unchanged generated stubs after the ordinary functions passes
both exact formatter versions, without a format exemption or toolchain change.
The attribution/no-coauthor gate has passed on #3052.


## Reproduced private-memory initialization deadlock repaired

The PostgreSQL integration failure above exposed a real datastore ownership bug.
The personal code-index worker held codeIndexState.mu while request dispatch
copied postgresDataStore by value. A request could inherit a locked independent
mutex with no owner capable of unlocking it, blocking the module's handler and
shutdown. Code initialization state is now one constructor-owned pointer shared
by request-bound copies and the background worker. Schema validation, readiness
publication and failed-migration retry keep the same initialization path. The
existing restart fixture resets its initializer with a fresh owned state.

A deterministic regression holds initialization while making the request copy.
It fails on the old source (the copied mutex never unlocks) and passes 20 runs
under the race detector after the fix. It also exercises the actual dispatcher
and verifies completed initialization is not repeated by later request copies.
The full memory and module-host packages pass race tests. PostgreSQL 18 full
integration now passes 167/167, with the same 26 unavailable-service skips; the
private model write, correction/readback and bounded owner shutdown all pass.
The original failing verdict is retained above as evidence, not reclassified.

The exact CI formatter version 19.1.1 now passes the full source-format target.
The previous snapshot HEAD also passes Gitleaks (6,209 commits, no leaks). An
additional exploratory whole-memory go vet still reports existing unkeyed
fixture literals and a rankingCapture fixture's JSON marshal of a mutex value;
this is not claimed as a clean whole-package vet result.

Final qualification for the shared-initializer fix: all 76 lint gates pass with
CI's exact clang-format 19.1.1; no baseline or gate exemptions were added.


## Fresh CI export coverage repair

Fresh #3052 export CI caught the new memory regression missing from the explicit
module bundle source list. The descriptor now includes it, and the exported
module's existing selected-test command runs that regression too. The running
canonical-history regression is also included in the Aimee module descriptor.
No source-parity assertions or existing test selections were removed. The full
21-case export build/runtime suite passes (one existing unavailable compiler
case skipped). All 76 lint gates pass for the memory export repair; focused
module descriptor/inventory checks additionally cover the history fixture entry.


## Typed Codex host-tool callback boundary (not native activation)

Both Codex adapter entry points accept an explicit typed tool catalog and host
callback together. Tool definitions render from canonical fields into the SDK's
experimental dynamicTools protocol; unsupported hosted tools, strict true/null
and cache metadata refuse rather than silently disappear. SDK calls enter as
owned canonical TOOL_USE blocks with their original ID, namespace, name and
opaque JSON arguments. The bridge checks current SDK thread/turn acknowledgments,
exact catalog identity, unique JSON fields and duplicate call IDs before invoking
the host callback. Host results must be canonical string TOOL_RESULT blocks with
the same call ID; success/error uses the authoritative flag, not result text.
Invalid source/results fail capture and stop further SDK work.

The callback grants no effect permission itself: its host owner must still match
the durable original call and apply fresh execution policy and receipt handling.
Current production callers do not enable this new option. It does not establish
an isolated launch profile, reconcile the model request prefix/current catalog,
suppress SDK model observations, or activate server native execution/cache.
Those integrations remain required before activation. On resume, the host must
prove the persisted catalog's scope; the SDK resumes stored definitions and does
not offer dynamicTools replacement on thread/resume in the tested protocol.

Eighteen process fixtures exercise both adapter entry points: nominal host error
results, unknown namespace/name, wrong thread/turn, repeated call, host failure,
wrong result ID and duplicate fields. Pure boundary checks also cover unsupported
definitions and raw-sidecar substitution. Original 30-digit argument fractions
survive the callback codec. The existing real-SDK fixture retains its ordinary
and bounded MCP scenarios and adds a bounded typed-host scenario with no MCP
server. All three pass through the compiled C adapter on installed Codex 0.160.1
and CI-pinned 0.153.4, including process restart, fresh canonical adoption and
unseen-tail resume. The new scenario verifies original callback identity, host
error results, the exact decimal token and a catalog limited to the host tool and
SDK user-input helper.

Canonical fixture persistence now uses the C codec's exact message JSON string;
Python's binary float decoder must not rewrite admitted numeric arguments when
constructing the re-adoption input. Initial exploratory decoding only appeared
to show SDK rounding: raw callback frames from both actual binaries preserve all
30 digits. Separate actual-SDK probes found a reserved flat exec_command name
collision, while explicit aimee.exec_command namespace calls work. No source
history or existing tool is silently renamed to work around that collision.

Qualification so far: registered 657-binary native suite and proxy regressions
pass; two focused ASan/UBSan fixtures pass, with the final strengthened codec
assertion rebuilt separately. Cppcheck on both changed C files reports zero
diagnostics; memory-ownership preflight passes. The normal server build and
PostgreSQL 18 integration pass 167/167 with 26 existing service skips. CMake's
thin-client application target also builds; that is not a server CMake claim.
Fresh CI on published 945aceaff has passed lint and integration; remaining Docker
tiers were still running at the last check. No production promotion performed.

All 76 lint gates pass for the typed callback increment with CI clang-format
19.1.1. Published 945aceaff CI subsequently reached 48 passing jobs, including
integration and three Docker tiers; the complete script-regression job failed
and T2 was still running. That script failure is being investigated next.


## Recursive Make script-fixture repair from fresh CI

The complete script-regression job on 945aceaff failed two suite-selection
assertions. Its nested make -C invocation exports directory-printing state, so
the miniature GNU Make fixture's stdout included Entering/Leaving banners as
well as its actual test names. The failure reproduces locally with MAKELEVEL=1
and MAKEFLAGS=w. The fixture now requests --no-print-directory explicitly and
retains exact stdout/set comparisons. A seventh regression invokes actual Make
with that recursive environment. All seven pass directly, under the reproduced
environment, and through the actual tests-are-run-check Make gate. No coverage
selection, assertion, baseline or CI job was removed or exempted. The full script
suite and fresh CI rerun remain to qualify this repair.

The complete local script suite now passes on 21dec06c0: 73 discovered test files;
the thin-client proxy suite is run separately with the native suite and real SDK
qualification. Full-HEAD Gitleaks passes (6,213 commits, no leaks). The native
runner executes 657 target entries; the reachability gate counts 655 distinct
names. Those counts describe different denominators, not 657 distinct binaries.
No test registrations or exemptions changed in the recursive-Make repair.


## Canonical OpenAI-family cache configuration and honest projection

The IR now owns prompt_cache_key, prompt_cache_retention and the opaque
prompt_cache_options object. Chat and Responses entry adapters admit them once;
both output boundaries rebuild them from typed fields. Native cache-only edits
invalidate original raw replay even without a mutation flag, including exact
numeric changes beyond binary-float precision inside cache options. Canonical
request JSON and Go request consumers retain those values; empty cache keys
remain distinct from absent keys. No retention/TTL conversion or default policy
change is introduced. Malformed types and duplicate option keys refuse.

OpenAI-family cache controls cannot be applied by the Anthropic renderer. Their
omission now has a canonical projection count, including a cache-only report
with no reasoning loss. Go and frontend admission preserve the old no-op/negative
report refusals, and the GUI displays cache loss independently of reasoning.
Actual provider request builders commit fidelity before returning a sendable
body; failed projection capture refuses the body. Cross-provider rendering keeps
the canonical configuration rather than deleting it from history/context.

Run-entry capability checks now live beside canonical request-profile ownership
in agent_bridge.c. The same predicate and refusals are retained; the economizer
loop does not inspect or mutate provider cache settings. Cache controls not yet
owned by the external run entry refuse explicitly instead of silently disappearing.
Six public-entry checks cover both existing external backends without launching
them, while preserving the pre-existing accounting refusal without cache controls.
No structural-gate exemption was added.

CMake's frontend/backend/IR-serving fixtures were missing existing renderer,
SHA-256, turn-integrity and disabled-memory seam dependencies present in Make.
Their targets now link the actual sources, public core header path, Threads and
OpenSSL Crypto; all four selected CMake/CTest IR fixtures pass. No crypto stub or
platform skip was added. Two decoder calls in an older ingress fixture were moved
out of assert expressions, retaining the assertions and ensuring the calls are
not conditional on NDEBUG. Cppcheck uses CI's src working directory/include paths:
13 affected production/bridge sources and six final owner/fixture sources pass
the ratchet with three existing diagnostics and no increase. Ownership preflight
also passes, without changing any baseline.

Qualification so far: the complete 657-entry native run (655 distinct target
names) and proxy regressions pass after the ownership repair; four focused
ASan/UBSan fixtures pass; three Go packages pass race tests; all 294 frontend
tests across 35 files and TypeScript checks pass. The normal server build and
PostgreSQL 18 integration pass 167/167 with 26 existing unavailable-service skips.
The final ingress assertion cleanup passes its normal and ASan/UBSan rebuilds;
all 76 lint gates pass. No analyzer baseline or gate exemption changed.

This preserves cache configuration, not evidence of live provider cache billing.
Per-content prompt_cache_breakpoint ownership remains a separate unimplemented
consumer; do not claim full explicit-breakpoint replay. Native activation still
requires owned current request context, original durable-call claims/effect
receipts, SDK-observation suppression, actual launch/build/workspace attestation
and live subscription qualification. Production remains KB-free and unpromoted.

Official schema/context: [Chat request cache controls](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)
and [prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching).
The controls have independent semantics; this change preserves supplied values
rather than selecting a model-specific caching or retention policy.


## Owned host request preparation before native activation

`agent_prepare_request_ir` now returns the complete owned canonical host request
before provider rendering: model/profile, ordered system blocks, canonical
history, sampling/output controls and the post-policy tool catalog/choice. The
existing egress builder uses this same preparation owner, retaining projection
capture and provider rendering at egress. Preparation emits no projection and
never imports an SDK request to recover host authority. Success replaces the
previous owned output; failure leaves that output unchanged. Inputs are borrowed
and can be freed immediately after success. Chat retains only its existing
host-created extension sidecar, without whole native prompt bytes.

The focused HTTP fixture covers all three profiles, released source inputs,
replacement/refusal ownership and exact 30-digit tool input preservation; existing
provider-profile parity and delegate/fallback catalog regressions also pass.
ASan/UBSan passes; correctly configured Cppcheck reports three existing production
diagnostics with no increase. The complete 657-entry native rerun (655 distinct
target names) and all 76 lint gates pass after the transport-fixture repair.

The broad native run exposed an independent transport-fixture defect: its mock
captured at most 4095 request bytes, then closed an 8192-byte request with unread
payload, allowing a TCP reset to invalidate the gzip response. It now drains the
advertised body while retaining bounded header capture, checks header/body bounds
and handles interrupted reads. The original gzip round-trip assertions remain;
30 independent full thin-client fixture processes pass, and Cppcheck reports zero
diagnostics for the fixture. Production transport behavior is unchanged.

Additional CMake qualification of the existing `test_agent_http` target failed:
its agent dependency lacks the providers public include path, and its target
omits existing translation/profile/configuration owners and the Go provider
fixture build/environment. An exploratory partial wiring repair was removed;
this target is not qualified. This does not invalidate the four narrower CMake
IR fixtures qualified in the preceding cache increment. Finish its complete
owner/fixture wiring as a remaining build consumer; do not skip the target or
replace the real provider fixture with fabricated successful responses.

Native activation is still pending: this preparation API is a prerequisite,
not an activated SDK execution path. Original durable-call claims and supervised
effect receipts, current-context/history binding, observation suppression,
launch/profile attestation and live Claude qualification remain required.


## Live subscriber progress from retained canonical capture

The retained capture owner exposes its last committed global sequence under the
writer mutex, including after a terminal event. The primary producer can drain
through that cursor from the existing owner-checked DB1 session pages. Callback
workers retain only the durable writer and never a browser connection. The
Codex primary's existing progress callback now performs that drain; ordinary
same-thread capture remains an immediate no-op when already delivered.

Delivery refuses page gaps and never rewinds or repeats an envelope already
included by a page read beyond the sampled target. The fixture now honors the
128-event page bound and covers 131 retained-worker events, a further commit
during page retrieval, repeated progress and sealed-writer tail delivery. The
original event payloads and order match persistence; no SDK observation is used
to synthesize model content. Disconnect behavior remains subscriber-local.

The two native fixtures and ASan/UBSan pass; the normal production server builds;
both CMake/CTest owner fixtures pass. Correctly configured Cppcheck covers five
changed production/fixture sources with one existing diagnostic and no increase.
The complete 657-entry native suite (655 distinct target names), PostgreSQL 18
integration (167/167, 26 existing unavailable-service skips), both ThreadSanitizer
fixtures and all 76 lint gates pass.
This does not activate the native broker or suppress SDK-normalized observations;
those still require current-request ownership and durable tool/effect admission.


## Owned current native request context (constructor, not SDK activation)

`aimee_ir_request_clone` provides a deep typed snapshot without a native-wire
round trip. It retains context provenance, ordered blocks, signatures/opaque
state, media, nested/hosted tool owners, output/sampling/cache configuration and
exact sidecars. In-place replacement is supported; failed admission/allocation
leaves the previous owned output intact. The pure fixture releases the source,
checks exact long decimal input and injects 64 successive JSON allocation failures.

The translation entry boundary now has a separate scoped host-request owner.
It runs before replay-proof restoration at native admission, and never runs at
egress or inside pure renderer comparisons. The three-wire/six-entry fixture
checks replacement, refusal/cleanup, scope restoration and retention of later
approved transforms. Native SDK context never becomes authoritative by echo.

`server_native_runtime_open_request` copies a complete host-prepared post-policy
profile for the selected model. Original native request bytes and a missing
retained capture refuse. Its conversation content comes only from a fresh
running-log snapshot for each callback, held consistently across any native
reparsing within that callback. The owned system/catalog/model/controls replace
SDK context and side-channel selectors at entry. Original observed model output
still commits through the existing writer before replay-proof capture. Profile
identity participates in native cache scope, including the bounded host extension
sidecar. Parallel model callbacks on one runtime refuse rather than fork its
running snapshot. Existing constructor success paths remain unchanged.

The scoped-listener fixture releases the input profile, updates durable source
between callbacks, checks the original selected model/system/namespace/cache key,
verifies post-entry changes survive proof checks, tests profile-dependent cache
identity, and covers parallel refusal plus cancellation/join. It also refuses
unselected models and native-byte profiles without poisoning a valid capture.
The listener fixture stubs its HTTP/proof boundary but now links the real
translation scope/codec owners in both Make and CMake; actual entry and consumer
behavior are also tested separately. This is not claimed as a live SDK launch.

Two live consumers depended on pre-ownership native fields: Chat admission now
requires canonical messages rather than a raw-sidecar array, and Anthropic token
counting resolves the admitted canonical model rather than the SDK selector.
Focused regressions check raw-free host history and count forwarding from an
owned model/history while refusing normal malformed input as before. Count bodies
already rendered canonically; no raw-body proxy regression was introduced.

Focused pure IR, cross-protocol, listener, Chat ingress and Anthropic HTTP tests
pass. Initial clone ASan/UBSan and Cppcheck pass; final broad qualification is
running. No production SDK spawn caller uses the new constructor yet. Actual
credential-free launch/build/workspace attestation, original durable-call
claims/effect receipts, SDK-observation suppression, live subscription and
independently graded local learning remain required. Production remains KB-free.

The prepared-profile sidecar validator belongs to translation. It allows only
Chat's existing host routing/template extension keys and Anthropic's host system
layout; Responses profiles carry no native selector fields. Whole SDK request
sidecars, unrelated models and conversation selectors refuse before a listener
can be handed to a child. Golden layout checks retain valid Chat and Anthropic
cache forms. The listener fixture no longer reimplements translation TLS scopes;
its real bindings are restored and checked after each joined callback.

The final normal server build and 657-entry native suite pass, as do five
ASan/UBSan fixtures, four CMake/CTest IR/listener fixtures plus the thin client,
and correctly configured Cppcheck on ten source/fixture files (zero diagnostics).
Two older Anthropic fixture calls were moved outside asserts without removing
assertions. Final PostgreSQL 18 integration passes 167/167 with the same 26
unavailable-service skips; the real-scope listener passes ThreadSanitizer and
all 76 lint gates pass. No baseline or test exemption changed.

Published checkpoint `9425078eb` is fully green in fresh main CI run
`39328706353`: 50 main jobs passed, including T1, T2, T2-luks and T3. Its export,
audit, fuzz, release and attribution workflows also pass. The two testing-target
platform skips remain unchanged; Windows/macOS were not run on that head.
This is a qualified checkpoint, not completion of the whole migration. The
owned-context increment still needs its own published-head CI qualification.


## Running history numeric ordering and early streaming tool outcomes

The live streaming-history fixture reproduced a previously hidden running-query
bug after sequence 9: selecting `sequence::text` and ordering by the unqualified
output name sorted PostgreSQL rows as 1, 10, 11, ... 2. The reader correctly refused
that apparent gap. The running loader now orders by the qualified underlying
BIGINT column; sequence output remains a string for JavaScript precision. The
completed-history query and paged replay select numeric sequences already and
were independently checked for the same defect. They do not need a cast fix.
No stored event is rewritten. Source diagnostics now distinguish sequence,
actor and event failures without including payload content.

A native SDK can finish an original tool call while its model stream remains
open, before the final assistant message snapshot commits. The history projector
now remembers valid assistant TURN_START positions and orders later snapshots by
that first durable observation, retaining tool outcomes after their original
calls. Duplicate snapshots replace the same slot; checkpoints and terminal turns
reset observation indexes. Unmatched starts do not create messages, and their
tracking is bounded by the canonical message limit. Non-assistant messages keep
their observed position. This derives history only from the existing log.

Focused running/completed tests cover early outcomes, repeated snapshots,
unmatched starts, checkpoint reset and tracking bounds. The PostgreSQL 18 fixture
commits real start/block/argument/stop events plus an outcome before the final
snapshot, crosses sequence 10, and preserves exact 30-digit arguments. It also
checks that each stored event remains canonically admitted. The three Go race
packages pass, as does the normal server/module PostgreSQL integration run:
167/167 with the same 26 unavailable-service skips. All 76 lint gates pass.

Original tool-source verification still needs to use actual writer-committed IR
when callbacks precede the final snapshot; an SDK-normalized command is not a
source receipt. Effects remain with the existing governed-action owner, not a
new receipt store. SDK launch sites are still unactivated.


## Shared host-turn generation leases and original-source claims

A retained capture is now the unique live writer for its (session, turn).
Duplicate opens, including a different actor/source spelling, refuse until every
retained lease joins/releases. Both production creator sites use fresh random
turns; consumers already retain the owner. The live directory carries no
connection or credential state and removes entries at final release.

Model generation admission belongs to this host-turn owner rather than one SDK
runtime's mutex. A returned generation token retains the writer; a stale/double
leave cannot release a newer generation. Both runtimes sharing a capture refuse
parallel model generations; read-only token counting need not take the generation
lease. Closing a producer seals new admissions while an existing generation can
still release its final retained reference safely.

The same owner provides bounded once-only claims for independently verified
committed source-sequence/block pairs. Noncanonical/future sequence values,
invalid block indexes, sealed/failed owners and excess capacity refuse. A repeat
reports already claimed. These are transient execution admissions, not source
verification or a parallel durable receipt journal. Existing governed-action
permits/outcomes remain authoritative. No production tool dispatch calls the
claim API yet; original typed source matching and effect wiring remain pending.

Focused tests cover eight racing writer opens, eight racing claims with exactly
one winner, all 4096 legal source slots, refusal beyond the bound, stale generation
leaves, closure with the generation holding the last reference, and competing
native runtimes on one capture. The full 657-entry native suite and normal server
build pass. Three ASan/UBSan fixtures, two ThreadSanitizer fixtures, three CMake
owner fixtures and Cppcheck on four source/fixture files (zero diagnostics) pass.
PostgreSQL 18 integration also passes 167/167 with the same 26
unavailable-service skips. All 76 final lint gates pass.

Published `115677b34` completed all 50 main CI jobs, including every Docker tier,
with only the two unchanged testing-target platform skips; export, audit, fuzz,
release and attribution workflows also passed. The later numeric/causal history
repair `c77cc0492` also completed all 50 main CI jobs, including all Docker tiers
and identity mint, with the same two platform skips. Its HEAD scan passes
Gitleaks (6,218 commits, no leaks). The shared source-claim change is committed
as `43b960287`; its HEAD scan found no leaks across 6,219 commits. No production
promotion occurred.


## Original model calls use the actual egress catalog

Owned-profile native forwarding now retains an owned typed catalog from the
translation replay guard, including the final post-policy pass. It does not
retain another conversation snapshot. Original buffered tool calls must match
that catalog's separate namespace and function identity. Original streamed
calls are assembled with the existing bounded canonical response builder,
including exact argument fragments, and checked before their BLOCK_STOP can
commit or be released to the SDK. Changed message identities, overlapping
response starts, malformed arguments and unadvertised tools fail the capture.
Earlier committed fragments remain observations; no completed call is invented.

Eight focused cases cover valid buffered and streamed calls, an unadvertised
SDK tool, wrong namespace, malformed streamed arguments, changed stream identity,
a removed post-policy tool, and its valid post-policy replacement. Exact
30-digit arguments remain intact. The existing legacy forwarding success path
is unchanged. This is original-source catalog admission, not callback source
verification or effect authorization. Durable source lookup, once-only claims,
governed dispatch and launch activation still need wiring. The normal server
build and all 657 native suite entries pass. The focused runtime fixture also
passes ASan/UBSan, ThreadSanitizer and CMake/CTest. Cppcheck reports zero
diagnostics for the two changed C sources; all 76 lint gates pass.


## Original-call readback and shared once-only admission

Owned native model callbacks now retain only bounded derived call admissions:
the original typed call, its actual advertised function, and an immutable hash
commitment to the writer-committed response prefix through call completion. The
comparison hash uses canonical JSON; original opaque arguments stay untouched.
Full message snapshots repeat existing streamed call slots without resetting
their source or claim. Message identities remain host-qualified.

A host callback can resolve exactly one matching unclaimed original ID, namespace,
name and argument object, read back the entire committed response prefix through
its source sequence, recheck both session incarnation identities, and claim once
through the shared retained capture. A missing/changed prefix, deleted/recreated
session, ambiguous provider ID or duplicate claim refuses. Claims return owned
original arguments and the exact advertised function. No SDK-proposed arguments
become execution authority. The cache is bounded to 4096 calls and 16 MiB of
canonical call/catalog content, with no alternate durable history or receipt store.

Focused tests cover buffered and streamed sources, exact 30-digit arguments,
earlier argument-fragment replacement while preserving the later BLOCK_STOP,
source erasure, session recreation, wrong arguments/namespace, two ambiguous
original responses, output preservation on refusal and eight racing claimants
with exactly one winner. Owned-profile transports also refuse legacy MCP
routes, preventing an alternate tool ingress around original-source claims;
legacy authenticated forwarding retains its existing successful behavior.
Source readback requests only through the committed completion sequence, avoiding
unrelated later content. These tests execute no effects. The normal server build and all 657 native
suite entries pass. The final runtime fixture passes ASan/UBSan, ThreadSanitizer
and CMake/CTest; Cppcheck reports zero diagnostics across all three changed C
sources. PostgreSQL 18 integration passes 167/167 with the same 26 service skips.
All 76 final lint gates pass. Supervised dispatch, source/effect admission synchronization, credential-free
attested launch and SDK observation suppression remain unfinished; a source
readback/claim alone does not authorize an effect or attest a launch.


## Verified native tool execution through existing supervised owners

The synchronous host SDK callback now consumes a verified once-only original call,
uses its exact advertised function/schema and original arguments, and invokes the
existing permission, directive, metadata and execution-policy owners before the
canonical dispatcher. Its logical attempt binds the committed source prefix,
sequence/block, session and account/model binding; SDK idempotency fields confer
no authority. Only the explicit `aimee` namespace binds to the host registry.
The callback also requires the captured tool-execution capability.

It reuses the runtime's authority setup/restore path, including fresh Vault lineage,
actor/model/tools, source overlay, delegate scope, workspace, cwd and durable job.
The producer's existing cancellation flag and immutable cancellation scopes are
restored after the nested call. Protocol capture state stays untouched. The callback
runs synchronously on its creator, with its held capture, sink and workspace still
bound; workers cannot borrow the browser sink. Standard durable host questions
use the retained control owner. The GUI's independent canonical EventSource keeps
reading those requests while execution waits for a reply.

Effect authorization is set only around the existing admitted dispatcher. Actual
results retain the tool owner's status and are durably emitted with a source-bound
message ID before return to the SDK. Ordinary permission/policy refusals become
error results without dispatch. Cancellation or context revocation fails the
capture. If a result cannot be retained after dispatch, execution fails without
inventing a cancellation or retrying the effect; the governed-action owner remains
the only durable permit/outcome store. The legacy MCP admission entry still uses
its existing host-request nonce; its successful behavior is preserved.

Focused cases cover exact 30-digit arguments, permission/policy denial, a real
recorded host question/reply before dispatch, actual tool-error status, absent
execution capability, cancellation and context revocation during policy, wrong
producer thread, restored parent state, duplicate calls and capture/annotation
failure after a dispatch. Three ASan/UBSan fixtures, the runtime ThreadSanitizer
fixture, three CMake/CTest owners and Cppcheck on five C sources (zero diagnostics)
pass. The normal server build and all 657 native suite entries pass.
PostgreSQL 18 integration passes 167/167 with the same 26 service skips.
All 76 final lint gates pass.

This callback is not yet wired at SDK launch sites. Credential-free attested launch,
SDK observation suppression, live Claude custody/source binding, unresolved-effect
history reconciliation and remaining IR/learning consumers remain unfinished.
The generic legacy history repair's cancelled/timed-out placeholder is not a
source receipt for an effect whose result could not be recorded; resolve that
consumer before native activation. No production promotion occurred.


## Missing tool outcomes remain unknown; authoritative history refuses repair

Canonical compatibility repair in C and Go now states that no result is available
in this history and the execution outcome is unknown. Its synthetic block carries
explicit platform diagnostic provenance, not a tool owner's outcome or a claim
that cancellation prevented execution. Original calls/arguments and actual results
stay unchanged. The remaining legacy native repair views use the same truthful
warning while those consumers await retirement.

The pure read-only canonical history admission helper rejects incomplete/orphan
cycles and unresolved-result diagnostics, including a diagnostic that superficially
pairs with a call. Primary and delegate authoritative loads use that admission;
they do not checkpoint a repaired view as executed history. Native model generation
checks the fresh source history at entry and the actual request again before
egress, preventing a downstream transform from introducing a synthetic resolution.
Read-only token counting does not need to authorize another model generation.

Focused cases cover unchanged inputs, reused IDs, preserved exact arguments and
actual results, diagnostic provenance, strict primary/delegate load refusal, and
pending or newly synthesized post-policy native histories. The normal server build
and all 657 native entries pass. Five ASan/UBSan fixtures, two ThreadSanitizer
fixtures, two CMake/CTest owners and the economizer/IR Go race suites pass.
Cppcheck reports the same three existing agent_bridge diagnostics across ten
changed C source/fixture files, with no new diagnostics or baseline change.
PostgreSQL 18 integration passes 167/167 with the same 26 service skips, and
all 76 final lint gates pass.

Reconciliation still belongs to actual governed-action receipts or an explicit
host resolution. This change does not infer an effect result from missing data
or invent a parallel receipt journal. SDK launch/cache activation, source-bound
live Claude execution, remaining ingress/IR consumers and independent local
learning qualification remain unfinished. Production remains KB-free and unchanged.


## Strict SDK/provider frame admission before C-string conversion

The Codex JSON-RPC reader admits the exact raw line before copying it into a
C string. Embedded/escaped NUL, invalid UTF-8, duplicate fields and trailing JSON
refuse capture with `invalid-json-rpc-frame`. Dynamic callback replay is keyed by
canonical, type-sensitive RPC identity: repeated numeric IDs refuse, while a
provider call ID reused by a later independently admitted original call can
proceed. The durable source owner still claims each original call once.

Nested completed tool arguments and buffered/streamed provider frames use the
shared strict parser at translation boundaries. The buffered Responses SSE
fallback cannot hide bytes behind a NUL. Buffered JSON HTTP POST now rejects
embedded NUL using the actual received length before returning a C string;
fixed-length and chunked socket regressions also verify that exact-length binary
streaming remains intact and the send guard is released before response reads.
Outbound immutable request bytes retain their existing contract.

The focused SDK fixture covers 18 cases for each primary/delegate entry, including
RPC numeric/string identity, distinct original calls with reused provider IDs,
and malformed frames rejected before callback dispatch. Both real Codex pins
(0.160.1 and 0.153.4) pass ordinary, bounded-catalog and typed-host-tool replay
qualification. These are local protocol qualifications, not production factory or
credential-custody evidence. Six ASan/UBSan fixtures and five CMake fixtures pass;
Cppcheck reports zero diagnostics across the eleven changed production sources.
The normal server build and all 657 native entries pass. PostgreSQL integration
passes 167/167 (26 service-dependent checks unavailable locally); all 76 lint
gates pass. The previously published `2d49add9b` also completed every CI job.
Native SDK activation, cache lifecycle ownership and live Claude custody remain
unfinished; do not substitute these protocol results for that work.


## Canonical core repair and unresolved native-cache refusal

The final provider-format branches of `message_history_repair` are retired. All
confirmed production callers in the delegate loop and both gateway retry seams
already supply canonical arrays. The core validates every canonical message
before applying the ordered IR repair; native, mixed and malformed histories
refuse without mutation. Explicit historical import stays in translation. The
unused native argument-normalization/sanitization module and its Make/CMake
linkages are removed. Invalid native arguments can no longer be silently changed
to `{}`; the MiniMax regression verifies strict entry refusal and exact valid
30-digit argument emission through typed egress.

Native-cache source loading now also applies authoritative history admission:
missing results and platform unknown-outcome diagnostics refuse before any cache
claim. Actual known tool errors remain replayable. Reused call IDs are checked
causally, preserving earlier receipts and signed reasoning. The bypass gate now
checks the migrated core repair function while retaining explicit compatibility
output boundaries; plants cover native readers and both retired sanitizer APIs.

The focused core/cache/strict-provider fixtures pass, as do four ASan/UBSan
fixtures, both native cache/runtime CMake fixtures, and all fourteen bypass-gate
tests. Cppcheck retains the three existing agent-bridge diagnostics with no new
ones or baseline changes. The normal server build, all 657 native suite entries
and all 76 lint gates pass.
Native SDK launch activation and the other unfinished work above remain required.


## Claude exact byte frames and actual MCP identity qualification

Claude's headless reader now finds frame boundaries with `memchr` over the
actual received length and admits object frames with the shared strict parser.
It cannot hide a suffix behind NUL or accept a duplicate field/partial JSON
parse. Existing non-JSON diagnostic stdout lines remain outside conversation
capture. EOF without a final newline and fragmented valid frames still work.
Six malformed initial frames and a malformed unterminated tail after a successful
result refuse capture, retain earlier real observations and never report a
successful turn. The focused lifecycle, ASan/UBSan and CMake fixtures pass;
Cppcheck reports zero diagnostics. The normal server build, all 657 native
suite entries and all 76 lint gates pass.

An offline run of the actual installed Claude Code 2.1.227 binary completed two
local Anthropic requests and one local MCP tool call using a fresh credential-free
HOME, `--bare`, empty setting sources, strict explicit MCP config and no built-in
tools. The MCP call includes the original provider ID in
`params._meta["claudecode/toolUseId"]` (observed `toolu_original_fixture`). This is
a verified SDK boundary field, not a guessed metadata name. The probe script and
small local outputs are under `/var/tmp/aimee-ir-claude-mcp-probe.py` and
`/var/tmp/aimee-ir-claude-probe-*`; they contain only synthetic local data.

The same actual SDK probe confirms an argument-fidelity gap: original numeric
`0.123456789012345678901234567890` reaches MCP as `0.12345678901234568`. Original
host-captured arguments must remain the execution source. Do not weaken the
existing exact proposed-call claim API or call normalized MCP JSON canonical.
A Claude bridge still needs an explicit host-owned tool-identity binding and
original-ID lookup/proof before dispatching original arguments; metadata alone
grants no tool/effect authority. This run qualifies SDK protocol/correlation only,
not live subscription custody, production activation or native history adoption.


## Codex host-owned launch and durable-terminal cache cutover

The browser Codex worker and canonical delegate path now call the server native
execution owner before any legacy credential HOME is materialized. Refusal cannot
fall back to the old credentialed scheduler. The server retains the selected Vault
lineage and post-policy request. The scheduler receives a private HOME/cwd, closed
environment, local model capability and explicit namespaced host catalog. SDK
model/tool/message observations are suppressed; original provider capture owns
conversation and exact arguments. Final response and usage are cloned from the
original completed canonical response after transport callbacks join. Owned
runtimes also refuse private `/responses/compact`, which cannot replace canonical
history.

On Linux, launch holds and hashes the actual executable descriptor and executes
that descriptor with the owned cwd descriptor. Effective SDK settings and their
contributing layers are read back before cache acquisition or thread start/resume.
Built-ins, plugins, hooks, project MCP/config, keychain auth stores and ambient
credentials are disabled/refused. This launcher currently supports an explicitly
registered native ELF executable; arbitrary scripts/npm dispatchers and other
platform descriptor launchers remain unsupported rather than silently trusted.
CI resolves the actual executable from its pinned npm platform package. The
workspace owner supplies held inode identity for shared/mirror leases and fresh
resource acquisition epochs for detached/container leases; provider pointers do
not establish cache identity.

Completed canonical history supplies adoption. A cache claim supplies only its
verified unseen tail and scoped resume handle. After child exit and callback join,
only derived cache metadata transfers to the retained capture owner. The capture
seals on the durable terminal write; its action runs outside the writer lock,
exactly once. Only a healthy completed terminal publishes a native checkpoint.
Cancellation, failure, lost completion and close invalidate the quiescent claim.
Private session IDs no longer write the legacy harness key from this launch path.

Older browser delivery projects the same committed canonical tail, including
retained worker events and disconnected presence-ring delivery. Exact per-block
rendered prefixes prevent final snapshots from duplicating streamed text; a
conflicting snapshot refuses capture. SDK-normalized text and private IDs do not
become this view's source. IR-v1 delivery retains its canonical envelopes.

Validation covers a factory lifecycle fixture with actual HOME/config/descriptor
resources and explicit peer mocks, including original-result projection, canonical
unseen-tail adoption, identity refusal, pending-history refusal, SDK failure,
terminal failure/cancellation and cleanup. This fixture is not live subscription
or whole-server native execution evidence. Actual Codex 0.160.1 and pinned
0.153.4 pass all four offline protocol modes, including descriptor launch,
effective-config proof, disabled project MCP, exact decimal host arguments,
fresh-HOME resume and unseen-tail adoption, and suppression of SDK observations.
Both SDK versions also refuse mismatched model/config and notification settings
before any thread, model traffic or host-ready/cache callback. Typed catalog and
launch-profile proof helpers now live in `cli_codex_profile.c`, keeping the
process adapter within the unchanged source-size limit.

The server build and all 658 native suite entries pass, including a full rerun
after the helper extraction. Seven focused ASan/UBSan fixtures and four TSan
lifecycle/resource fixtures pass; CMake runtime/factory fixtures and Cppcheck
with zero new diagnostics pass. PostgreSQL integration passes 167/167, with the
same 26 service-dependent local checks unavailable. All 76 lint gates pass.
Qualification logs use `/var/tmp/aimee-ir-native-factory-*`. This qualification
is not live subscription or production deployment evidence. Fresh CI remains
required for the published commit; production promotion and the overall
migration remain unfinished.


## Claude original-call execution continuation: 20 October 2026

The local continuation adds explicit logical-tool/registry bindings and a private
Streamable HTTP MCP owner. Claude's `claudecode/toolUseId` selects a unique committed
original call; SDK-rounded arguments never become effect input. The same source
readback, once-only claim, current registry definition, supervised policy/effect
and durable receipt owners apply to Codex and Claude. Missing or reused source IDs,
unknown aliases and failed persistence refuse execution. Model forwarding waits
for creator-side launch attestation. Actual Claude initialization must report the
owned cwd, credential source, permission mode, exact tool catalog, connected MCP
server and empty plugin set before that release.

Both browser native SDKs now use the general agent worker's registered workspace
and isolation owner. The SDK sees an opaque scheduler trigger; complete task,
context, selected Vault authority and post-policy tools come from canonical host
IR. Claude runs with held executable/cwd descriptors, a private credential-free
HOME, closed environment, bare mode and strict MCP configuration. Configured Linux
managed settings/MCP files refuse the supported launch profile. A real SDK probe
confirmed `/v1/messages?beta=true`; only that exact query and its count-token
counterpart normalize to existing native model routes. Other queries remain refused.

Actual-server qualification exposed and repaired two Responses defects: admission
looked for SDK raw input after canonical context replacement, and canonical output
omitted the SDK-required `usage.total_tokens`. Focused persistent regressions cover
both. Actual local Codex 0.160.1 and Claude 2.1.227 each pass three provider wires,
real README tool effects/receipts, durable browser replay and a second resumed turn.
CI now installs pinned Codex 0.153.4 and Claude 2.1.227 native platform packages and
requires the same six cases in its PostgreSQL integration job; no credentials or
production KB are needed. The separate Codex adoption probe now includes a fifth,
flat host-tool catalog mode.

Qualification completed locally: all 658 native binaries, seven focused
ASan/UBSan fixtures, two TSan fixtures, four CMake fixtures, five real Codex
adoption modes on both 0.153.4 and 0.160.1, Cppcheck without increased diagnostics,
and full pinned-SDK PostgreSQL integration (168/168; 26 other checks require
unavailable local services), plus all 76 lint gates. Fresh CI follows publication. Production has not been promoted; `.100`
remains KB-free. This is a continuation checkpoint, not completion of the overall
migration. Retire the remaining private SDK compaction/ingress paths and finish
independently graded local learning/reuse after publishing this qualified slice
in the existing PR #3052.


## Canonical browser compaction continuation: 20 October 2026

The next local slice removes the browser's private Codex compaction launcher and
its unused public C helper. Native browser turns now use the same agent dispatcher.
Canonical compaction accepts any configured model because it performs no model or
SDK execution; the authenticated canonical session takes priority over an SDK
handle. Existing checkpoint ownership, history admission and failed-write refusal
remain in force. Successful host checkpoint operations have a separate completion
condition from model turns. They emit neither an empty user message nor invented
model output. A missing checkpoint still fails completion.

The actual-SDK journey now compacts between its tool-bearing turn and resumed
turn, supplies a foreign provider handle, verifies a canonical checkpoint, and
requires zero extra model calls. It passes for both SDKs on all three model wires.
The native adapter keeps all three existing reasoning-effort request spellings.
CMake qualification also repaired the history fixtures' missing private legacy
include path and Anthropic replay/sha dependencies to match their Make targets.

Local qualification: all 658 native binaries, two ASan/UBSan fixtures, one
TSan fixture, two CMake fixtures, Cppcheck without increased diagnostics and full
pinned-SDK PostgreSQL integration (168/168; 26 local service-dependent checks
unavailable). All six actual SDK/model cases include canonical compaction. All
76 lint gates passed; publication checks follow.
The overall migration remains unfinished: canonical seeded-run native execution,
remaining ingress/consumers, independently graded local learning/reuse, broader
native platform/launcher and live subscription coverage still need completion.


## Canonical seeded native runs continuation: 20 October 2026

The local slice now admits complete canonical seeded run history for Codex and
Claude. The admitted request is committed through the active session-log owner
before launch; model context comes only from that durable capture. Admitted output
limits/temperature reach the selected model profile. Native execution returns owned
original typed content, retaining opaque reasoning fields, while compatibility
text remains a separate projection. The old attested-session credential-HOME and
private-handle fallback in the generic execution path is removed. Standalone
adapter fixtures remain explicitly unowned and cannot borrow a server session.

Asynchronous `/v1/runs` now accepts an absolute `cwd` for native SDK execution and
holds the same nested workspace scope, foreign-path check and session isolation
used by browser tools until native callbacks join. The API contract documents
this native requirement. Direct HTTP model runs retain their existing behavior.
Actual SDK runs use a full 20,000-character message, preceding conversation,
inline PNG, explicit instructions, real read-file effect/result and durable
trajectory export. All six SDK/provider combinations pass. Host task usage now
sums committed model completions instead of reporting only the final model call;
original per-response observations remain intact in canonical history. Overflow
refuses rather than wrapping a counter.

Local qualification completed: all 658 native binaries, three ASan/UBSan fixtures,
two TSan fixtures, two CMake fixtures, Cppcheck without increased diagnostics and
full pinned-SDK PostgreSQL integration (168/168; 26 local service checks require
unavailable services). The extended native journey covers twelve browser/seeded
cases. All 76 lint gates passed, including reviewed source/generated OpenAPI
ownership pins. Publication checks follow. Continue in PR #3052 with the
remaining launcher/platform qualification, IR-consumer audit and independently
graded server-local learning/reuse. Production is still KB-free and unpromoted.


## Registered Codex npm launcher continuation: 20 October 2026

The native execution owner now resolves the registered Codex npm command to its
matching platform-package executable without running JavaScript. Both nested and
hoisted npm layouts are supported on Linux x64/arm64; direct native ELF commands
remain supported. Resolution validates the root package, declared optional alias,
platform package name/version, trusted file permissions and actual held executable
digest. An invalid nested package refuses instead of silently selecting a hoisted
SDK. Arbitrary scripts remain unsupported. CI now exercises the registered npm
command in the twelve actual-server browser/seeded native journeys.

The focused filesystem fixture covers held executable identity, PATH and symlink
registrations, both npm layouts, unsafe permissions, mismatched metadata and
missing binaries. A planted wrapper effect proves the script is never executed.
The preceding published head 8740d9d31 finished with 55 passing CI checks and two
existing platform skips. The seeded-run commit is ca19c574d; its only publication
scan match is the reviewed public OpenAPI SHA-256 input pin, recorded by exact
historical fingerprint. Launcher qualification passed: the full native suite and
new registration fixture, two ASan/UBSan fixtures, two CMake fixtures, Cppcheck
without increased diagnostics, all 76 lint gates and full pinned-SDK PostgreSQL
integration (168/168; 26 unavailable local services). All twelve native journeys
passed using the registered npm command. Fresh publication/CI follow.
This does not qualify macOS/Windows native descriptor execution, independent
learning grades/reuse or production promotion.

## Execution history repair retirement: 20 October 2026

The direct tool loop now uses strict canonical causal admission before another
model request. Its pressure/middleware fallbacks no longer create missing-result
diagnostics. The compactor admits both the original and staged replacement;
incomplete/orphaned cycles refuse even below the compaction threshold. Failure
leaves source bytes untouched and clears any staged success report. Ordered
call/result receipts and opaque retained content remain authoritative.

The economizer restores its exact original provider payload on retry; it no
longer sends that wire array through the canonical-history repair helper. No
production caller of message_history_repair remains. Its compatibility diagnostic
view and explicit IR repair tests are not execution admission paths.

Qualification passed the full native suite, two ASan/UBSan fixtures, two CMake
fixtures, Cppcheck without increased diagnostics, pinned-SDK PostgreSQL integration
168/168 (26 unavailable local services), all twelve native browser/seeded journeys,
and all 76 lint gates. Allocation-failure coverage now measures the successful
path and injects every JSON allocation failure rather than stopping at a fixed
ceiling. CMake aggregate libraries now expose the missing Make-equivalent public
include paths and build shared session-event code in the IR base, fixing the
actual focused compaction link. Fresh CI on the preceding published 72599fe5f
has 45 passes, two existing platform skips and nine pending checks, with no failures
at the last completed enumeration. Overall migration/learning qualification remains
unfinished; production remains KB-free and unpromoted.


## Native Vault OAuth integration qualification: 20 October 2026

Actual pinned native Codex/Claude now also exercise the selected host's Vault-held
OAuth credential path against synthetic local upstreams. The fixture checks the
Codex account header and Claude OAuth/beta headers, rotates each OAuth source
between browser turns, and requires the fresh token on subsequent model traffic.
Neither old/new access tokens nor refresh material may appear in model content.
The schedulers retain their closed credential-free launch environment.

All sixteen browser/compaction/resume/seeded journeys pass, including both OAuth
cases. Full PostgreSQL integration passes 168/168 with 26 unavailable local service
checks; all 76 lint gates pass. These are real SDK executions with synthetic Vault
credentials and upstreams. They do not claim live subscription acceptance or
production promotion. The latter remain separate qualification requirements.


## Required native browser operator integration: 20 October 2026

The native CI job now requires two further actual-SDK journeys, one per SDK: a
request_input question must appear on the live browser IR stream while execution
is blocked. An ordinary authenticated UID reply must be refused. A fresh matching
root-UDS browser operator then replies through /v1/chat/control-reply; the original
model receives its answer and the stream must include the matching control
response and successful durable terminal. Server authorization is unchanged.

Python compilation and all 76 lint gates pass. The preceding sixteen API-key/OAuth
journeys are locally qualified. This workstation cannot run the additional root
operator journey because passwordless sudo is unavailable; it is mandatory in
CI, with no new CI skip or server authorization exception. Its actual CI execution
remains pending and must be checked before this journey is reported qualified.


## Current documentation and CI queue continuation: 20 October 2026

README, STATUS, WHATS_NEW and the pending canonical session proposal now distinguish
qualified native original-source execution and closed homes from the earlier
credential-bearing scheduler implementation. API/OAuth qualification is recorded
as sixteen local journeys; actual root-UDS browser journeys still await CI.
Independent learning/reuse, remaining consumers/platforms, live subscription and
reviewed production qualification remain unfinished.

The latest source run 39368457405 for 71b361ccf is pending behind the still-running
OAuth run 39368084307. The following documentation push has no PR metadata edit,
so its source synchronize event can supersede the stale run under the existing
concurrency policy. No workflow gate, automatic review, attribution rule or
production configuration is changed. Registered PR commands expose check/failure
reads but no workflow cancellation. Personal gh is not authenticated and no
browser is available; no connector mutation or new login was attempted.


## Native browser mirror qualification repair: 20 October 2026

CI run 39368971570 passed all sixteen API/OAuth native journeys and real Docker
delegate learning/readback, then failed the new browser fixture on a noncanonical
early frame. The fixture had supplied an unregistered raw browser workspace. Its
stream diagnostics now retain the full rejected frame and it registers a mirror
through the ordinary workspace owner. The client clone is deliberately dirtied;
all model read effects must still see the approved committed mirror content.

This exposed two production defects. Native workspace identity rejected the
owner's reconstructed directory because its path differs from the client root.
The owner now holds the exact directory inode through the turn, authorizes only
that active reconstruction, rejects replaced paths and escaping repository
symlinks, and duplicates that descriptor for native identity. Failed acquisitions
never count as a truthy grant. Mirror binding also selects the shared filesystem
provider explicitly, preserving enclosing scope restoration. No general allowance
for paths under the mirror directory is added.

The file guard had passed a file path to git's directory worktree probe, then
used a different bare-repository prefix length to rewrite the path. It now probes
the target directory and verifies the repository prefix before rewriting. The
ordinary-checkout redirect remains covered, alongside the new arbitrary linked
worktree read regression.

Workspace/guardrail suites and both ASan/UBSan fixtures pass; Cppcheck has no new
diagnostics. Sixteen actual native API/OAuth browser/seeded journeys pass through
the explicit mirror with the dirty-client fallback detector. All 76 lint checks
pass across the complete run and the final ownership-pin review rerun. Reversing
just the guardrail path edits reproduces its previous pinned source exactly; the
memory adapter is unchanged. Fresh CI must still qualify the root operator
question journeys. Continue with owned mirror-lease capture/restore on retained
native callback threads and native mirror delegation; the read/question fixtures
do not establish that broader callback/delegate qualification. Independent local
grading/reuse and production review/promotion remain unfinished and KB-free.


## Attested session ownership repair: 20 October 2026

CI run 39371398721 passed the sixteen real API/OAuth native journeys through
registered mirrors and the Docker delegate learning/readback journey. The new
root browser case still failed before execution with `cannot initialize canonical
session`. The now-visible error led to session.create storing the socket peer UID
as owner: root browser backends became uid:0 even though canonical execution
correctly required their attested webuser principal.

Session creation and the matching legacy import ownership check now use the
ingress-attested principal. They preserve the existing UID fallback for legacy
unattested local callers and ignore user-supplied principal fields. No ownership
row is silently reassigned. A focused production-handler fixture covers two
browser accounts sharing UID 0, long browser names, distinct certificate identities,
ordinary UID callers and forged unattested/body principal fields. It passes Make,
ASan/UBSan, CMake and Cppcheck with no increased diagnostics. All 76 lint gates pass.
Fresh CI must qualify the root question journey beyond canonical initialization;
stream termination diagnostics now retain final records when no question appears.
The separate retained mirror callback/delegate lease continuation is prepared
under /var/tmp, not applied to this qualified source snapshot.


## Live browser question catalog and capture repair: 20 October 2026

CI run 39372736936 passed the sixteen API/OAuth native journeys and real Docker
learning/readback, then exposed the missing request_input model catalog entry.
Its dispatcher and session question owner already existed, but the canonical
built-in catalog did not advertise it. The catalog now supplies one schema to
all three provider profiles, declares its internal control egress and includes
it in the normal role toolsets.

Actual local streaming then exposed direct DB1 question writes bypassing the
canonical capture cursor and live delivery. Questions now reserve a host-only
reply consumer, publish through the canonical sink and commit the response
through that same sink before releasing an answer. Reservations admit exactly
one matching publication; stale, changed and abandoned reservations refuse.
Native harness drainers cannot steal the host reply. Cancellation and storage
failure remain terminal, without borrowing a browser sink on callback workers.
The Anthropic fixture's completed original input now matches its question delta;
it had incorrectly retained the read-file arguments in the completed snapshot.

An unprivileged user namespace provides a disposable server with a real root-UDS
peer boundary, overcoming the unavailable passwordless sudo for local testing.
All eighteen actual pinned-SDK journeys pass there: sixteen API/OAuth/history/run
cases and both live browser questions, rejected UID impersonation, matching
operator replies, canonical response and durable successful terminal. The full
660-binary native suite, focused ASan/UBSan and TSan, two CMake fixtures and
Cppcheck with no increased diagnostics pass. Fresh hosted CI remains required.
The retained mirror callback lease work is stashed separately until this repair
is published. The overall migration, independent local learning/reuse, broader
native platforms and reviewed production promotion remain unfinished; production
must remain KB-free.

All 76 lint gates also pass on the final question repair.


## Retained native mirror callback capability: 20 October 2026

Native runtime owners now retain a duplicate of the exact approved mirror
checkout descriptor. Every callback revalidates its path/inode, binds the held
capability for its own thread and restores the previous binding; close releases
it only after callback join. Nested scopes preserve the enclosing capability.
No mirror-base prefix or request-body path claim becomes filesystem authority.
The workspace fixture unbinds the original producer before a real callback thread
uses its retained capability, tests nested restoration and refuses directory
replacement with an escaping symlink. Native callback probes separately require
the imported capability and prove it is cleared on teardown.

The full 660-binary native suite, both ASan/UBSan and TSan fixtures, native runtime
CMake fixture and Cppcheck without increased diagnostics pass. All eighteen actual
pinned-SDK journeys pass again through the explicit mirror, including both live
root-UDS browser questions and operator replies. Broader queued native delegation
and durable workspace origin propagation still require their own producer/worker
qualification. This is not completion of the migration or independent learning.

All 76 lint gates also pass for the retained callback capability.


## Deployment T3 native refusal fixture repair: 20 October 2026

Published b55b174e0 passes real PostgreSQL integration 168/168, all eighteen actual
native SDK journeys including both browser operator replies, and Docker delegate
learning/readback. The T3 deployment job 118471635875 then fails in the separate
memory-native-async fixture. Its retained native-async.json identifies the exact
failed check: unobservable tmux-cli never queued a worker. The fixture had omitted
the absolute cwd now required by canonical native /v1/runs admission; it therefore
failed before reaching the accounting fence it intended to test.

That fixture now supplies its already-created isolated checkout only for those
external-backend cases. It still requires a queued asynchronous worker, the same
request_budget_unavailable refusal and zero provider sends. The actual-server
native fixture independently exercises both legacy tmux/Claude and unsupported
provider/ACP negative cases with an approved mirror cwd and inherited zero byte
cap. Both reach the intended worker refusal without a provider send, alongside
all eighteen successful SDK journeys. No production admission, budget guard,
assertion, deployment tier or platform gate is exempted. Fresh deployment CI
must qualify the complete T3/T2 sequences after publication.

All 76 lint gates also pass for the deployment fixture repair.


## Governed native seeded runs under inherited byte limits: 20 October 2026

The actual accounting-owner probe first confirmed that missing Go accounting
correctly refuses bounded traffic. With the real economizer process attached,
browser SDK execution succeeds, but a seeded /v1/runs was still rejected as
request_budget_unavailable before launch. The agent loop had classified every
provider CLI as an unobservable external transport, including the qualified
host-owned Codex/Claude pipeline.

Only Codex/Claude with the current durable log and retained canonical capture
now proceed to their existing host executor under a byte limit. That executor
still owns launch/source/Vault qualification and has no attested-session fallback.
The final canonical model gateway uses Go to admit the actual rendered bytes.
Standalone and unsupported adapters retain the external transport fence. The
unsupported deployment probes now use ACP under both legacy/current backend
spellings; qualified Codex/Claude have their own real zero-byte refusal cases.

The integration harness installs, grants, starts and stops the actual Go
accounting owner. All successful native API/OAuth/browser/seeded requests carry
a 262144-byte caller limit. Eighteen actual SDK journeys pass, including both
live root-UDS questions, and four negative cases prove zero provider sends for
qualified SDK zero-byte caps and unsupported external adapters. Full PostgreSQL
integration passes 168/168 with 26 unavailable local-service checks. The full
660-binary native suite, CMake agent library build and Cppcheck without increased
diagnostics pass. This is source qualification, not production promotion or
independent learning completion. Queued mirror-source work remains stashed.

All 76 lint gates also pass for governed native seeded runs.


## Queued registered mirror delegation: 20 October 2026

Background delegates now acquire the registered source before file references,
Git isolation or prompts can open it. Mirror and detached registrations reconstruct
their recorded source/head locally; failed reconstruction refuses. Raw client
roots no longer authorize server sandbox mounts, even when that path happens to
exist on the server. Foreground mirror acquisition also reports failure explicitly.

The trusted worktree creation result retains its own directory descriptor. Its
Git common directory must match the held source under a sanitized Git environment;
ambient GIT_DIR cannot forge that relationship. Exact child authority survives a
producer ending through a retained callback lease, and rejects replacement paths.
There is no blanket mirror-directory grant.

The real queued integration journey deliberately dirties the client checkout and
requires the committed reconstruction. Locally it reaches the mandatory sandbox
refusal without model traffic; hosted Docker must complete the actual read. The
660 native binaries, PostgreSQL integration 168/168 (26 unavailable local-service
checks), 30 thin-client proxy tests, workspace ASan/UBSan and TSan, CMake agent
build and Cppcheck without increased diagnostics pass. Fresh CI must qualify the
Docker completion. Native-primary orchestration exposure and queued transfer of
an already-held callback capability remain unfinished, alongside independent
local grading/reuse, remaining consumers/platforms and reviewed production
promotion. Production remains KB-free.


## Docker bare-mirror discovery CI repair: 20 October 2026

Published 70a042fcd reaches the queued mirror source and creates its dedicated
checkout, but CI integration job 118492993945 fails before the first delegate
model request: Docker discovery assumes every linked admin directory contains
<repo>/.git/. The actual registered mirror is bare and has <mirror>/worktrees.
The eighteen native SDK journeys and both real browser controls still pass in
that job; the new Docker mirror completion correctly remains a required failure.

Discovery now accepts the bare repository's standard commondir relationship,
requires its Git metadata and verifies the worktree backlink even when nested.
Existing workspace authorization, isolated networking and mount ownership are
unchanged. A real Git bare-mirror regression covers successful discovery, broken
backlinks and unrelated common directories. Hosted Docker must qualify the actual
execution after publication. Native-primary orchestration and queued callback
capability transfer remain stashed separately; they are not part of this CI fix.


## Native primary delegation and queued capabilities: 20 October 2026

Primaries now advertise delegate and delegate_status from the existing MCP
catalog. Delegate role toolsets do not inherit orchestration. Calls re-enter the
normal authenticated dispatch with the current model owner's capability ceiling;
body session claims cannot replace the host session. Default delegation preserves
an already-approved checkout instead of resolving a linked mirror to its bare
common directory. Compute contexts duplicate the producer's held directory lease,
bind it on each worker and restore the prior thread binding after cleanup.

The live journey exposed the generic provider-spawn heuristic also banning the
registered Aimee delegate tool. The host registry dispatcher now supplies a distinct
entry classification for that exact tool; other guardrail and audit checks still
see the original arguments. External hooks cannot request this classification.
Raw external delegate, Task, Agent, spawn_agent and unknown spawn-like tools remain
blocked. A failed mirror acquisition also stops lifecycle hooks before filesystem
probes; successful mirror-hook resource semantics and other ingress paths still
require their remaining audit.

The actual Go routing owner is installed and attached in integration, alongside
Go accounting. Both real SDK primaries call delegate and delegate_status, preserve
both original call IDs in durable history and finish before the fixture releases
the child's first provider response. The child must read committed mirror content in Docker and cannot
inherit orchestration tools. Locally it reaches the mandatory sandbox refusal;
hosted Docker completion remains required. There is no new CI skip.

The 660 native binaries, PostgreSQL integration 168/168 (26 unavailable local-service
checks), all twenty root-namespace actual-SDK journeys and four budget refusals pass.
Four focused ASan/UBSan fixtures, two TSan fixtures, CMake agent/two fixtures and
Cppcheck without increased diagnostics also pass. The guardrail ownership pin was
reviewed against an exact reverse-to-previous-source proof; its memory adapter and
Go policy inputs are unchanged. Fresh CI is required for this source snapshot.

The earlier Docker bare-mirror repair b33f4d3fc passes hosted integration 168/168,
including the real sandboxed mirror read and delegate learning/readback. At the
last complete enumeration it has 55 passing checks and two existing platform
skips, including all deployment tiers. The migration, independently graded local learning/reuse, remaining IR
consumers, native platforms/live subscription and reviewed production promotion
remain unfinished. Production remains KB-free.


## Native terminal and frozen LSP gate CI repairs: 20 October 2026

Published d036a2326 completes its ordinary native unit shards and all deployment
tiers, but hosted integration fails on the Claude delegation stream: its parent
execution completes and then emits a native cache checkpoint. The fixture had
incorrectly required the final frame itself to be the completion. Fixtures now
require the exact parent execution outcomes running/completed, independently of
cache metadata and child notices. Failed, cancelled, missing and duplicate parent
terminals still refuse. Native cache persistence is unchanged.

The real-provider LSP job separately rejects the new orchestration wrapper in
the shared MCP table against its frozen source pin; the unit-tests aggregate
therefore fails too. Both actual providers pass 20/20 cold starts, full reference
recall and zero false positives. Three exact host-only deltas now reverse to the
previous MCP table before the full frozen comparison. No LSP implementation,
protected path, threshold or denominator is exempted. The original unmodified CI
artifact passes the repaired release validator; deliberate containment-policy and
catalog mutations still fail. All eighteen S1 contract tests and three terminal
regressions pass.

The rebuilt published runtime with the corrected fixtures passes PostgreSQL
integration 168/168 (26 unavailable local-service checks), all twenty actual-SDK
journeys including both browser operator replies, and four budget refusals in a
root user namespace. Fresh CI must qualify the complete hosted execution again.
The early queued-source acquisition and role/persona reader repairs continue
below; the overall migration and independent learning are unfinished.
Production remains KB-free and unpromoted.

## Queued source admission and project templates: 20 October 2026

Published 02dfe4eba now passes all 56 checks, with the two existing platform
skips, including every deployment tier. PR #3052 remains open and mergeable.

Queued delegates acquire their source before role/persona discovery, permission
frontmatter, file references or provider selection. Shared sources require their
registered authority; a raw mirror path cannot recreate a native callback's held
capability. Early failures release the acquired scope through the common cleanup.
The actual server fixture proves that dirty client role templates are ignored,
a client-only persona is unavailable in the approved mirror, and unregistered or
unleased sources refuse before provider traffic.

Persona and role readers validate names and complete path lengths before opening
files. Project files must resolve inside the project, including through symlinks;
links within it and trusted user/builtin fallbacks retain their existing behavior.
The platform containment query resolves actual POSIX paths or Windows handles;
it reports a filesystem fact and does not replace the higher-level retained
workspace capability or eliminate filesystem rename races.

All 660 native binaries, 30 proxy tests, PostgreSQL integration 168/168 (26 locally
unavailable service checks), twenty actual-SDK/browser journeys, four budget
refusals, two focused ASan/UBSan fixtures, the CMake agent library, Cppcheck without
increased diagnostics and all 76 lint gates pass. Full-history secret scanning
finds no leaks. Fresh CI is required for this new source snapshot. Windows native
execution is not qualified by these Linux checks.

Successful mirror-hook resource handling, other ingress/IR consumers, independent
local grading/reuse, broader platforms/live subscription and reviewed production
promotion remain unfinished. Recovered queued work without its original held
capability fails closed; this is not qualification of restart/resume. Production
remains KB-free and unpromoted.

## Minimal template fixture sanitizer linkage: 20 October 2026

Published d6f350086 fails native unit shard 1/4 during the sanitizer link of
delegate-role: role_templates now requires platform_path_is_beneath, but this
minimal fixture did not link the platform path object. Both minimal consumers,
delegate-role and cmd-delegate, now declare that dependency using the existing
platform-selected object list. Both rebuilt fixtures pass ASan/UBSan. The
containment check and sanitizer gate remain intact. Pending Git MCP source work
is stashed separately while this CI repair is published.

## Git mirror source retention: 20 October 2026

The pending Git work was restored after publishing sanitizer-link repair
38659f6a9. Git MCP now acquires its nested workspace scope using the original
registered client identity before remapping the request. Failed acquisition
cannot fall through to a server-local probe. Reconstructed cwd requests disable
stale session redirection; explicit path requests retain their existing
authoritative routing. Detached requests keep their runner and client path.
Caller cwd/session/worktree state and the enclosing binding survive every exit.

The real PostgreSQL/server journey deliberately points the session tracking file
at another dirty checkout. Both path and cwd requests still observe the clean
approved mirror rather than either dirty source. Full integration passes 168/168
(26 locally unavailable service checks), including twenty actual-SDK/browser
journeys and four budget refusals. The focused Git scope fixture passes normally
and under ASan/UBSan; Cppcheck has no increased diagnostics and all 76 lint gates
pass. Replacing only the changed Git-dispatch function with its previous body
reproduces the pinned source exactly; the memory adapter/policy bytes are
unchanged. This records boundary analysis, not independent release approval.

Fresh CI remains required for this snapshot. The earlier pending migration,
independent learning, platform/live and production qualifications remain open;
production stays KB-free and unpromoted.

## Browser usability and replay controls: 20 October 2026

The published Git-source snapshot 5b62ff5f is green in fresh CI: 56 checks pass,
with the two existing thin-client platform skips. The user's next manual browser
pass takes priority over the remaining migration work.

The browser now uses a horizontal tool navigation on narrow screens and hides the
empty channel rail until requested. Workflow options start collapsed when no
workflow is bound. Conversation text uses readable foreground colors; permission
and question forms have consistent spacing, focus states and buttons. Question
submission remains visible but disabled until every answer is valid. The canonical
transcript obeys the existing history window and no longer adds a duplicate
text-only copy. Resolved requests are collapsed; control-only turns retain their
acknowledgments.

The owner-checked canonical SSE subscriber sends a transport-only
`replay_complete` frame containing `session_id` and the decimal `cursor` after
initial durable catch-up. It does not allocate an IR sequence or write another
history event. The browser validates that boundary against ingested history and
withholds controls until it arrives, including after reconnect or returning to a
tab. Disposed subscriptions cannot update the UI. Reply guards prevent duplicate
submission during overlapping delivery, rapid clicks and tab changes; an HTTP
failure releases the guard for retry. This prevents historical unanswered-looking
requests from briefly becoming actionable before their saved replies replay.

Channel history and live frames now include the SQLite message ID as a decimal
string. SSE resumes with Last-Event-ID and reads ordered rows from the durable
store; the broker only wakes it, with periodic catch-up for missed notifications.
The initial history query returns the latest 200 rows in chronological order.
The browser merges snapshots and live arrivals by ID, preserving distinct
identical-text messages and ignoring late responses from disposed subscriptions.
A successful reconnect refreshes the snapshot as well.

Validation: 298 frontend tests, production web and console builds, Chromium setup
recovery and canonical webchat regressions, runtime-web Go race tests, PostgreSQL
integration 168/168 (26 unavailable local-service checks), twenty actual native
SDK/browser journeys, four budget refusals, focused server HTTP/dispatch
ASan/UBSan fixtures, and Cppcheck without increased diagnostics pass. The actual
server replay fixture verifies readiness follows the last committed envelope for
both full and resumed replay. Chromium checks POST/SSE overlap, reconnect,
navigation/reload, a duplicate question and its reply, and the 390px layout.
This browser fixture uses a deterministic backend; it is separate from the actual
SDK integration qualification.

All 76 repository lint gates also pass. Fresh CI is required for the browser-fix snapshot. The migration, independent
learning qualification, broader platform/live checks and reviewed production
promotion remain unfinished. Production stays KB-free and unpromoted.

Final visual inspection also caught the generic Spinner's 24px padding inside
Steer: it expanded the busy button and squeezed the mobile input. The composer
now uses a compact decorative spinner with reduced-motion support; the browser
regression requires a busy button at most 48px high and a mobile input at least
250px wide. Selecting a channel closes the rail, and only affirmative control
actions receive primary styling.

### Retained large legacy sources

Legacy load/latest and host50 source verification read the exact retained
`primary_sessions` tuple through bounded UTF-8 chunks under its row lock.
Individual PostgreSQL cells remain capped at 1MiB. Complete source bytes and
SHA256 are preserved; tmux archives remain outside model history.

Schema migration50 validates a 16MiB bound only for that retained source column.
The original migration20 SQL/checksum stays immutable. Pending upgrades use one
explicit alternate20 plan differing only by deferred validation of this source
check; its actual SQL has its own recorded checksum. Replay accepts only the
original or this exact alternate plan. Unknown plans still refuse before later
DDL. Ordinary legacy writes retain their existing wire limit.

The retained-source storage ceiling is inclusive 16MiB; a complete legacy getter
reply must also fit its existing 16MiB module frame, including its header, field
lengths and metadata. Load/latest return typed `too_long` without source bytes
when that complete envelope would exceed the frame. The retained row is unchanged.

## Live qualification and account catalog repair: 22 October 2026

PR #3061 is merged; its post-merge CI run 39807229032 succeeded. The
subsequent a12100390 candidate is pushed on fix/native-mcp-startup-and-editor;
it still requires a pull request and fresh CI. Its full source-built Docker
installation on the owned .252 guest passed the first native Claude browser
turn, including a real question reply, governed tools, canonical terminal and
checkpoint, and indexed lookup against the selected octocat/Hello-World project.
The SDK discovery barrier also waited through real delayed MCP discovery.

The HTTP Codex switch then failed because its configured gpt-5.5 model was no
longer supported by the selected ChatGPT account. Public account discovery
returned HTTP 400: the legacy adapter omitted its compatibility-version query
and parsed the wrong model-ID field. Discovery now uses the selected sealed
OAuth account, the adapter's existing pinned compatibility version, JSON GET,
and visible model slugs in server order. Initial subscription registration uses
the discovered account catalog instead of guessing a static model; existing
operator model selections remain intact. Read-only probes issue no inference.
Roster revision and credential-source revalidation reject observed changes
during discovery. Providers race tests and three normal plus three sanitizer
C fixtures passed; live catalog and resumed model switching remain required.

The first HOST permission attempt was a harness failure after one question
Send: an assertion used session_id instead of aimee_session_id. Closing that
driver was followed by a canonical cancellation and normal completion. No
restart fault or human permission reply occurred; this is not an Allow pass.
Its original evidence and canonical history are retained separately. A corrected
fresh attempt, independent kernel retirement checks, strict idle UI checks,
and final fresh published-testing installation remain open.

Production .100 remains untouched and KB-free. Do not claim complete migration
or uniform three-turn success from these partial qualification results.

Combined catalog snapshot 79d348da2 passed 75 of 76 lint gates. The remaining
ownership-ledger gate correctly required review of the changed private Vault
adapter. Review confirms the only change is a nonsecret compatibility-version
field; memory transport, custody and Go policy ownership are unchanged. Its
source fingerprint and review evidence are updated without changing the gate.

### Stream refusal UX and final candidate checks

The exact 504c9b85f source passes the full local integration target: 169/169
checks pass, 27 explicitly unconfigured service checks skip, and none fail.
All eight actual SDK wire/auth profiles pass tools/results, compaction, durable
replay/resume and seeded execution. Docker-dependent grading is unavailable
on this workstation and remains a separate live qualification.

An actual refused history stream exposed an indefinite Synchronizing state.
A failed canonical EventSource now closes its generation, displays a fixed
reconnect message, preserves admitted history and drafts, and withholds controls
until explicit reconnect finishes replay. Late frames/errors from the closed
generation cannot affect its replacement. The two refusal/disconnect regressions
fail on the old source and pass with the fix; all 467 frontend tests and both
production builds pass. No message or permission reply is automatically retried.

The second HOST permission attempt reached a genuine question and qualified
native leaf, then its custom restart controller timed out before any Allow.
It incorrectly waited for configuration readiness before restarting stale module
peers. Its safety stop retired all owned processes and removed the old cgroup
root. Subsequent recovery correctly refuses the changed root inode; the old
claim remains a retained negative fixture, with no SQL repair or weakened
identity check. The shipping container restarts its planes together. A corrected
HOST controller starts fresh module peers before requiring readiness and proves
the replacement listener belongs to the actual new core. Fresh genuine Allow
qualification remains open; neither failed harness attempt is an Allow pass.

Keep the remaining work together on fix/native-mcp-startup-and-editor. Finish
implementation and validation before PR handling; do not split repairs into
intermediate PRs or merges. Production .100 is excluded from this work, including
read-only investigation. Final CI, publication, released-image acceptance and
owned-resource cleanup remain required.

### Genuine Allow exposed duplicate authorization

The corrected HOST restart controller completed actual kernel custody recovery:
the original qualified leaf became empty before retirement, and normal browser
recovery reconciled the canonical history. The following write nevertheless
failed: one SDK invocation produced permission 61 for its relative path, an
actual browser Allow at 62, and another permission at 63 after the tools owner
resolved the absolute path. The latter was cancelled; result 65 reports refused
dispatch. The file is absent and no governed dispatch exists. Completion 194 and
checkpoint 195 describe the model turn, not a successful write. This is a product
regression, separate from the preceding harness failures; no second Allow was sent.

The tools owner now exposes the same registered-action predicate it uses during
dispatch. For those actions only, the extra unknown-effect permission waits until
the owner has resolved the exact final resource and arguments. Baseline policy,
role/directive checks, source/liveness validation and sticky denial remain in
place. The final gate still hashes the complete action and uncertain receipts;
it does not reuse approval across changed arguments. Unmanaged tools and a
missing final owner retain their initial gate.

The composed fixture now links the actual tools-owner predicate. It fails on the
old duplicate-prompt implementation and passes normally and under sanitizers,
covering relative/absolute input, one permission and durable dispatch, changed
payload/receipt, denial, replay, publication and liveness fences. This fixture
is not an independent filesystem-effect proof; fresh genuine Allow remains required.

The 504 candidate's actual account catalog returned HTTP 200 with no offered
models. No model was guessed, saved or executed. The projection alone cannot
distinguish an empty upstream list from filtered entries. Empty Codex catalogs
now return a typed error with bounded counts of raw rows and visibility classes;
provider bodies, model instructions, arbitrary visibility text and credentials
are excluded. The declared adapter/installer and CI SDK pins now agree on
[the published Codex 0.163.0 release](https://github.com/openai/codex/releases/tag/rust-v0.163.0).
The next live call must establish account availability; an old-version filtering
cause is still a hypothesis. The selected legacy credential origin is unchanged.

The browser failure-state fix remains included. The next complete source build
will contain it together with these permission and catalog repairs. Preserve
all failed turns, previous native cache evidence and negative custody fixtures.

# Aimee overnight continuation handoff

Prepared 17 October 2026, approximately 23:30 Europe/Amsterdam. The user requested implementation to stop here, the work to be pushed, and a complete handoff for a local session. Do not infer that the overall migration is complete.

## Start here

- Repository: https://github.com/RakuenSoftware/aimee
- **Keep all further work in the existing PR #3036**: https://github.com/RakuenSoftware/aimee/pull/3036
- Branch: `fix/browser-ir-runtime`; target: `testing`.
- **Pushed and verified commit: `3f44879630c02206bd908870e4b192b86ea54d2c`.** Its Git tree is `2f63314f5ad5d95da0a7d8cbb2adddffbe4672b2`, exactly equal to the tested staged tree.
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

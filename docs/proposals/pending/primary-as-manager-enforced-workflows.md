# Proposal: Primary-as-manager — request→workflow routing with aimee-enforced review + roundtable

- **State:** IN PROGRESS (partial) — reviewed (4 design rounds, §9); S0 + S1-advisory + the S2 cores
  are in testing (#936); S2 integration + S3 + S4 remain.
- **Progress:** **S0** (interactive workflow catalog + engine invariants I1/I2/I3) and **S1** (the
  request→workflow router at the gateway seam — live but strictly *advisory*: logs a decision, binds
  and mutates nothing) both landed via #936, as did the **S2 cores** (`wfe_enforce`/`wfe_binding`/
  `wfe_router` decision logic + the `workflow_binding` table + an env-gated advisory log). **Remaining:**
  the S2 integration layers (live binding + the `advance_request` interactive driver + per-block
  tool-strip + externalization guard are not yet wired to live ingress), S3 staged enforcement (flip
  `gate.deliver` to blocking, dialed via `AIMEE_WORKFLOW_ENFORCE_STAGE`, default OFF), and S4 autonomous
  parity (`/v1/dev/submit` + sweep through the same router). The S3 hard-flip is an operator decision
  gated on live `.254` router-cost numbers + S4 parity. Ships default-OFF; default-ON is a later,
  separate proposal.
- **Author:** JBailes
- **Date:** 2026-07-01
- **Charter roles:** Orchestrate / Delegate-Manage / Review-Roundtable / Evaluate-Optimize
- **Thesis:** The primary agent should not *do the work* — it should **manage** it.
  Its job is to talk to the user, split the request into bounded packets, hand
  those to delegates, then **review** what comes back and **lead a roundtable
  review** before anything is delivered. Today aimee has all the review
  machinery (delegates, roundtable panel, fail-closed verdicts, audit trail) but
  the primary chat path runs *beside* it, not *through* it — so the manage→
  review→roundtable loop is advisory, not enforced. This proposal routes every
  substantive request to a **workflow**, ships a **default** workflow, lets aimee
  **match** a request to the right workflow, and makes that binding
  **aimee-enforced** on both the interactive and autonomous surfaces.

## Goal

Make the primary agent's operating role — **communicate, split, delegate,
review, roundtable** — the *only* way substantive work reaches "done," with
three guarantees the current split between chat and the workflow engine does not
give us:

1. **Every substantive request runs inside a workflow.** Not a convention the
   prompt asks for and the model may skip — a work-item the primary session is
   bound to, whose gates the engine owns.
2. **Aimee picks the workflow.** The user states intent in natural language;
   aimee matches it to a workflow (with a mandatory **default** fallback), tells
   the user which one it chose, and lets them override by name.
3. **Review + roundtable are non-bypassable.** The primary agent cannot mark a
   substantive deliverable accepted until it has reviewed the delegate output
   *and* the roundtable gate has returned a fail-closed pass. The primary
   *leads* the roundtable (triggers it, adjudicates loop-backs); it does not get
   to rubber-stamp it.

Non-goal: turning trivial conversation ("what does this function do?", "hi")
into a heavyweight pipeline. The router must have a cheap conversational lane, or
the feature will be hated. See §3.

**Author ruling (2026-07-01) — the enforcement floor.** Enforcement gates *all
substantive change*, but with a graded floor, not one heavy panel for everything:
read-only/conversational turns **bypass entirely** (`converse` lane, §3); genuine
one-liners/typos take an **expedited `hotfix` workflow** (§2, §4); full multi-file
change takes the full `managed-change` panel. **"Expedited" means faster/cheaper
models + a tighter round/deadline budget — NOT fewer required personas** *(per
roundtable RT-Rev2 #1).* The fail-closed required-persona quorum is identical
across `hotfix` and `managed-change`; the §8 guarantee ("no substantive change
delivered without a fail-closed roundtable pass") holds on both. Expediting is a
*cost* optimization, never a *security* weakening. This resolves forks R5 and the
*policy* half of R2 (the precise "substantive change" predicate is still tuned
observe-only in S1 — see §7).

## §0 What already exists (so we don't rebuild it)

The workflow engine is battle-tested and the delta here sits **above** it, not
below. Confirmed by code walk:

- **Workflow engine + DAG state machine — LIVE.** `src/workflow/wfe_engine.c`
  (`wfe_engine_advance` :192, `wfe_engine_run` :388) drives a typed DAG of blocks
  with atomic per-transition DB txns and an immutable `lifecycle_event` audit
  row. Block catalog in `wfe_def.c:18-47`
  (`author.proposal`, `author.plan`, `implement`, `document`, `freeze`,
  `gate.roundtable`, `gate.human`, `pr.open`, `merge`, `gate.ci`,
  `check.mergeable`, `custom`).
- **Roundtable gate + fail-closed verdict — LIVE.** `wfe_roundtable.c`
  (`exec_roundtable` :60) fans a proposal/plan/frozen-diff to a persona panel via
  a pluggable provider; `wfe_verdict.c` (`wfe_gate_decide` :37) requires
  structurally-valid verdicts from required personas, counts high-severity
  blockers, and coerces untrustworthy verdicts to REQUEST_CHANGES. Verdict
  evidence is replayed/graded against indexed code in
  `server/roundtable_verify.c` (`roundtable_grade_item` :19).
- **Delegate providers + management channel — LIVE.**
  `server/server_compute.c` `handle_delegate` :1763 (sync),
  `handle_delegate_launch` :1566 (parallel packets), `handle_delegate_reply`
  :1962 + `delegation_mailbox_t` :69 (two-way). `wfe_live_delegate.c`
  (`wfe_live_delegate_run` :67) is the registered delegate provider the engine
  calls from `implement`/`document` blocks.
- **Delegate-only enforcement (PR #719) — LIVE.** The primary already cannot
  spawn its own sub-agents: `gateway_policy.c` strips Task/Agent/spawn_agent
  (`gateway_policy_strip_tools` :71, `..._police_parsed_response` :160) and
  `server.c` intercepts the hook to auto-launch a delegate. This is the
  precedent for *tool-level* enforcement; this proposal adds *lifecycle-level*
  enforcement.
- **Autonomous driver + scheduler — LIVE.** `wfe_scheduler.c` background thread
  drives `mode=autonomous` items via `wfe_autonomy_run` (`wfe_autonomy.c:34`);
  interactive items are human-driven and skipped by the scheduler
  (`wfe_scheduler.c:42`). Human gates are HMAC-non-forgeable
  (`wfe_approval.h`), gate application is atomic/TOCTOU-safe
  (`wfe_store.h:73`).
- **A default already exists (weakly).** `/v1/dev/submit`
  (`server_http_routes.inc:2980`) defaults `workflow` to `"build"` when omitted;
  sweep filing hardcodes `manual-review` (`server_sweep.c:260`). Two shipped
  workflows: `config/workflows/build.yaml` (13 nodes, full lifecycle) and
  `config/workflows/manual-review.yaml` (2 nodes).

**What is missing** — the entirety of this proposal:
1. No request→workflow **matching**. The word doesn't appear in workflow code;
   workflows are opt-in by explicit name.
2. No **binding** of an interactive primary session to a workflow work-item.
   `/v1/sessions` (chat) and `/v1/workflow/items` (wfe) are independent paths.
3. No **enforcement** that substantive work goes through a workflow. The primary
   can do multi-file work in a plain chat turn and never touch the engine.
4. No **interactive-grade default workflow** — `build` is the full autonomous
   proposal→PR→merge pipeline, too heavy for a live "fix this bug with me" turn;
   `manual-review` is filing-only.

## §1 The router (request → workflow, with a mandatory default)

A new **workflow router** runs at session ingress, before the primary agent
takes the turn. It answers one question: *which workflow (if any) governs this
turn?*

- **Two-tier classification, cheap-first.**
  1. **Deterministic prefilter** (no LLM): a *thin fast-path for obvious cases
     only* — greeting/acknowledgement shapes, turns with no imperative verb, and
     turns the user explicitly tagged (`just chat`, `use <workflow>`). It routes
     those straight to the `converse` lane or the named workflow. *(Per
     roundtable RT9: it does NOT attempt to deterministically decide "touches a
     repo" from free text — that needs parsing, which is the classifier's job.
     The prefilter is a cheap skip for the unambiguous, not a full router.)* Its
     precision/recall on a labeled set is measured before S2; it is never allowed
     to route *toward* enforcement bypass on uncertainty (uncertain ⇒ classifier).
  2. **LLM intent classifier** for everything the prefilter didn't resolve: it
     maps the request to a workflow *id* from the installed catalog, or returns
     `default`. It classifies **intent and scope** (hotfix vs feature vs research
     vs review-only). *(Per roundtable RT8: no request-hash cache — NL rephrases
     hash differently, so it would be a near-no-op. Treat the classifier as a
     fixed, measured per-turn tax (§8); revisit embedding-based semantic caching
     only if the measured tax justifies its embedding cost.)*
- **Mandatory default, graded by confidence** *(per roundtable Rev 3 #4/#11).*
  "Unmatched" never means "unmanaged" — but the fallback is graded, not always
  the heavy path: **clear multi-file change intent → `managed-change`**;
  **genuinely ambiguous intent → `research`** (read-only, no panel — cheap and
  safe, since a read-only misroute cannot ship anything); **classifier error
  (timeout / rate-limit / 5xx) → `research` as the conservative floor**, never a
  silent fall-through and never `converse` (which a change imperative must never
  reach — §4.5). Every fallback + classifier error is logged to `lifecycle_event`.
  (Contrast today's silent fall-through to `build`.)
- **User transparency + override.** The chosen workflow is surfaced to the user
  ("Running this as *managed-change*; say `use hotfix` to switch, or `just chat`
  to drop the workflow"). Explicit user naming always wins over the classifier.
  An enforced system the user can't see or steer will be worked around.
- **Catalog-driven, not hardcoded.** Router selects from
  `$AIMEE_HOME/workflows/*.yaml`. Each workflow declares router metadata
  (intent tags, scope, `default: true` marker, `enforced: true|false`). Adding a
  workflow = dropping a YAML; no code change. Exactly one workflow may carry
  `default: true` (validated at load).

Open fork **R1 (matcher engine):** LLM classifier vs pure heuristics vs hybrid.
Recommendation: hybrid (deterministic prefilter + small LLM for work-shaped
turns). Pure heuristics can't tell "fix the flaky test" from "explain the flaky
test"; pure-LLM taxes every "hi." See §7.

## §2 The default managed workflow (primary-as-manager, interactive-grade)

A new shipped workflow — `managed-change` — encodes the role directly and is the
router's default for substantive interactive work. It is **lighter than
`build`** (no proposal-doc / PR-open / merge ceremony) but keeps the
non-negotiable review→roundtable spine:

```
understand(with_user)              # primary clarifies scope WITH the user
  -> split                         # primary decomposes into delegate packets
  -> implement(fanout)             # delegates do the work (existing block)
  -> review(primary)               # NEW block: primary reviews delegate output
  -> gate.roundtable               # existing gate, fail-closed verdict
        on_pass -> deliver         # summarize + hand back to user
        on_fail -> split           # loop: re-scope / re-delegate
```

- **New `review` block — read-only** *(per roundtable RT6).* The primary
  inspects the delegate diff/result against the packet's acceptance criteria and
  either forwards a clean artifact to the roundtable or loops back with concrete
  change requests. Its tool surface is **read-only** (write/edit/patch stripped
  at the gateway, §4 seam 2): the primary **cannot edit the diff itself** — any
  required change is re-delegated as a new packet. If the primary could patch
  here, it would be *doing the work* and the manager thesis would collapse; the
  read-only constraint is an enforced YAML property, not a prompt instruction.
- **New `understand`/`split` blocks — single-shot structured artifacts** *(per
  roundtable RT10; resolves the interactive-vs-node ambiguity).* Each is a
  single-turn block that must emit one structured artifact: `understand` → a
  scoped-intent record; `split` → a packet plan. The *conversation* that informs
  them happens in the preceding `converse`/turn flow; the block itself is the
  point where that intent is **sealed** into an auditable node output. They are
  not multi-turn sub-DAGs (that ambiguity would force a redesign in S0). If the
  artifact is unsatisfactory, the user loops the node, exactly like any gate.
- **The primary leads the roundtable.** It triggers `gate.roundtable`, receives
  the panel verdict, and adjudicates the loop-back — but `wfe_gate_decide` owns
  the pass/fail, so "lead" ≠ "override." Rubber-stamping is structurally
  impossible (required-persona quorum, fail-closed).
- **Companion workflows** the router can also select (all catalog YAML):
  `converse` (passthrough, §3), `research` (read-only, no implement/roundtable),
  `hotfix` (single-packet, expedited but still roundtabled), and the existing
  `build` for full proposal→PR→merge asks.

## §3 The `converse` lane (why this won't be hated)

Not every turn is work. The `converse` lane is a degenerate "workflow" that is
pure passthrough — the primary answers directly, no delegates, no gate, no
work-item, no added latency. The prefilter (§1) routes here by default for
conversational/read-only turns, and the user can force it (`just chat`).

**`converse` is read-only, enforced at the gateway** *(per roundtable RT3).* The
lane is not "trusted to behave" — its tool surface has write/edit/patch/
run-mutation tools **stripped** by the per-block gateway policy (§4 seam 2), the
same mechanism PR #719 uses for Task/Agent. So "just chat" followed by a direct
file edit is not a bypass: the edit tool isn't in the request. Every `just chat`
opt-out is audit-logged as a routing decision (§4). An automated test asserts
write tools are absent in `converse` mode.

This is the pressure valve that makes enforcement acceptable: enforcement bites
**only** on substantive change work, exactly where an unreviewed primary is a
liability. Getting the *predicate* right is the single biggest UX risk (§7, fork
R2) — but a mis-route to `converse` can now only *under-serve* (answer instead of
building), never *silently mutate the repo unreviewed*.

**Known ceiling — enforcement stops the *primary from shipping*, not the *user***
*(per roundtable Rev 3 #6).* In `converse` a primary can still emit a code block
the user copies in by hand. This is an accepted, named limit: the guarantee is
"the primary agent cannot deliver unreviewed change through aimee," not "no code
can reach the repo by any path." Mitigations (bounded code-block length in
`converse`, an advisory nudge to switch to `managed-change` for real edits) are
S1 tuning, not load-bearing — the user pasting their own code is out of scope by
design.

## §4 Binding + enforcement (aimee-enforced, both surfaces)

**Binding.** When the router selects an enforced workflow, session ingress mints
(or resumes) a `mode=interactive` work-item and **binds the session to it**. The
primary agent's turns become the interactive driver of that work-item: its
delegate launches, reviews, and gate triggers advance the DAG. The user still
converses normally; the engine is the substrate underneath, surfaced as
lightweight status.

**Enforcement — two concrete seams, no text-claim interception** *(revised per
roundtable RT1/RT3)*. A free-text "I've completed X" **cannot** be intercepted by
the engine, and refusing on a fuzzy done-predicate is unsound. Enforcement is
instead the composition of two mechanical seams, both reusing PR #719:

1. **New terminal `gate.deliver` block primitive** (added in S0). Every
   substantive workflow ends with `gate.deliver`, which the engine will only
   let a work-item cross once its upstream `review` + `gate.roundtable` nodes
   have passed (fail-closed, same DAG-control semantics as the existing gates).
   "Done" for a bound session *is* crossing `gate.deliver` — a state, not a
   sentence. Actions that externalize a deliverable (`pr.open`, `merge`,
   marking the work-item accepted) are engine primitives already and are the
   only ways to "ship"; they sit behind `gate.deliver`.
2. **Per-block gateway tool-policy** (the #719 pattern, generalized). Each
   workflow block declares an allowed tool surface; the gateway strips
   everything else from the request for a bound session, exactly as
   `gateway_policy_strip_tools` strips Task/Agent today. This makes the
   "primary is a manager, not a doer" property *mechanical*, not prompted:
   - `converse` block → **read-only**: write/edit/patch/run-mutation tools
     stripped. So "just chat" cannot itself mutate the repo (closes RT3).
   - `review` block → **read-only**: the primary inspects the delegate diff but
     cannot edit it; any change is re-delegated as a new packet (closes RT6 —
     otherwise the primary is doing the work and the thesis collapses).
   - `implement` block → delegate-launch tools only (primary still cannot write
     directly; delegates do).
   Every routing decision, including each `just chat` opt-out, is written to the
   `lifecycle_event` audit row so bypass attempts are visible post-hoc.

The engine already refuses to advance a `gate.roundtable` without a fail-closed
pass; `gate.deliver` + per-block tool-stripping make that refusal *bite* on the
interactive path without ever having to guess whether a sentence means "done."

**Interactive driver loop** *(new, per roundtable RT2)*. The scheduler skips
`mode=interactive` items (`wfe_scheduler.c:42`) and there is no interactive
equivalent of `wfe_autonomy_run` today — so binding needs an explicit driver,
defined as the core of S2:

```
user turn → primary agent takes turn (tools scoped to current block, per above)
         → on a block-completing action, call wfe_engine_advance(work_item)
         → engine moves to next node (may pause at a gate)
         → primary surfaces the new state to the user, awaits next turn
```

Unlike `wfe_autonomy_run` (a background thread that self-advances through
preauthorized gates), the interactive driver advances **only** on a user/agent
turn and **never** auto-crosses a gate — the human is in the loop by
construction. It reuses `wfe_engine_advance` unchanged; only the *caller* is new.

**Interactive lifecycle** *(new, per roundtable RT7)* — the multi-turn edge cases
must be defined before S2 routes real traffic:
- **Single-writer binding.** A work-item is bound to exactly one session; a
  second session (e.g. a second tab) on the same work-item is rejected, not
  silently forked. Prevents double-execution / divergence.
- **Scope added mid-workflow** ("also fix X" after split): opens a **child
  packet** on the current work-item, not a silent re-scope of a passed node.
- **User overrides the split:** re-enters the `split` node (loop-back), not an
  ad-hoc edit of a downstream artifact.
- **Primary disconnect / crash mid-workflow:** the work-item is orphaned, not
  lost — resumable by rebinding (state is already durable in DB1).
- **Gate never invoked:** an engine-side watchdog audits "bound work-item with no
  gate advance after N turns" and surfaces it, rather than stalling forever.

**Both surfaces, one router + one default:**
- **Interactive** (`/v1/sessions`): as above — session bound to work-item,
  primary as live driver.
- **Autonomous** (`/v1/dev/submit`, sweep): route through the *same* router and
  the *same* default so `workflow` is never silently `build`; the coordinator
  fills the primary-as-manager role and the scheduler drives it. This tightens
  today's hardcoded defaults into router decisions.

**Escape hatches (enforced ≠ obstructive):**
- Explicit user opt-out per turn (`just chat`) drops to `converse` — but this is
  logged, and cannot itself deliver a substantive change (it has no deliver
  node).
- **Degraded panel** reuses existing pause reasons (`panel_degraded`,
  `panel_unreachable`): the work-item parks rather than silently passing —
  fail-closed, consistent with the engine today.
- Operator flag governs enforcement strictness (advisory-log → soft-block →
  hard-block), so rollout can start observe-only. See §6.

## §4.5 Threat model + trust boundaries (per roundtable Rev 2)

Enforcement is a *security regime*, not just a UX flow — so the trust boundaries
and bypass vectors are first-class, not implementation notes. Rev 2 of the
roundtable was entirely security-focused; these are its findings and rulings.

- **"Read-only" is a tool-surface fact, not a confidentiality claim** (RT-Rev2
  #2). Stripping write/edit/patch/run-mutation from `converse`/`review` still
  leaves read/search/shell-read — so a mis-routed turn cannot *mutate* the repo
  but could still *read* it. In-scope position: the primary's read surface is
  already the ambient session surface (unchanged by this proposal); this proposal
  does not *widen* it. Confidentiality of the read surface is a pre-existing
  concern tracked elsewhere, explicitly **out of scope** here — stated so the
  "closes RT3" claim is scoped to *mutation*, not *exfiltration*.
- **Workflow YAMLs are operator-write-only** (RT-Rev2 #3). The catalog files
  (`$AIMEE_HOME/workflows/*.yaml`) carry the enforcement knobs
  (`allowed_tools`, `enforced`, `default: true`), so if any primary-influenced
  path could write them, a permissive/gate-less workflow could be injected. Trust
  tier: **operator-write-only, delegate-read-only**; the loader rejects YAMLs not
  owned by the operator; a test asserts no delegate-launch tool can reach the
  workflows directory. Same posture as the HMAC approval key.
- **`enforced: false` is never classifier-reachable for change** (RT-Rev2 #4).
  The router selects only `enforced: true` workflows for substantive intent; an
  `enforced: false` workflow is reachable **only by explicit user name**, never
  as a classifier default. Prevents a routing-layer bypass. (Test-locked.)
- **The classifier is untrusted input** (RT-Rev2 #8). The per-turn LLM router is
  a prompt-injection surface: a crafted turn could try to route a real change to
  `converse` (unreviewed) or a permissive workflow. Controls: (a) classifier
  output is **constrained to the catalog id set** (no free-form); (b) the
  deterministic prefilter **vetoes a `converse` verdict when the turn contains a
  change imperative** (fail toward *more* enforcement, never less); (c)
  over-routing-to-converse has a measured target rate (§8), distinct from the
  under-routing the mandatory default already covers.
- **Interactive advance needs an integrity control** (RT-Rev2 #5). Autonomous
  human gates are HMAC-non-forgeable; the interactive driver calls
  `wfe_engine_advance` on a "block-completing action," and a prompt-injected
  primary must not be able to cross `gate.deliver` by emitting the right-shaped
  artifact. Control: crossing `gate.deliver` (and any `gate.*`) still requires the
  **same gate satisfaction the engine enforces today** (roundtable fail-closed
  pass / HMAC human approval) — the interactive driver only *advances between*
  non-gate nodes; it has **no authority to satisfy a gate**. The primary emitting
  an artifact is *input to* a gate, never *approval of* it.
- **Delegate→review is a trust boundary** (RT-Rev2 #6). The `review` block's job
  is to read delegate output, so tool-stripping can't help — injected delegate
  content could poison the primary's verdict at the choke point. Controls:
  schema-enforced structured artifacts between `implement` and `review`; delegate
  diffs rendered to the primary as **quoted untrusted content** (consistent with
  the existing "treat external content as untrusted" rule in the prompt); and the
  roundtable panel independently replays evidence against indexed code
  (`roundtable_verify.c`), so a tainted forward is caught downstream, not trusted
  on the primary's say-so.
- **Tool-policy is snapshotted per turn** (RT-Rev2 #7). To avoid a TOCTOU where a
  mid-turn DAG advance (async delegate completion, child packet) swaps the current
  block and lets a newly-permitted tool run against context assembled under the
  old policy: the **tool surface is snapshotted once at request ingress and held
  for the turn**; mid-turn advances apply to the *next* turn. This is an
  enforcement-semantics rule, not an implementation detail.
- **Child-packet scope-add must re-gate intersecting nodes** (RT-Rev2 #9). A child
  packet that touches files already covered by a *passed* `review`/`gate.roundtable`
  node invalidates that pass. Rule: if a child packet's file set intersects any
  passed node's file set, the affected upstream nodes **MUST re-run** (or the
  child is rejected). Append/rebase-without-re-gating is a bypass and is ruled
  out — not left as a tuning detail.

## §5 Prompt + role changes

`src/prompts.c` STANDARD/EXTENDED tiers currently say only "delegate multi-file
work." Rewrite the primary role to the manager charter: *communicate with the
user, split into packets, delegate, **review each delegate's output**, and lead
a roundtable review before delivering.* Add a short "you are operating inside
workflow `<name>`; the review and roundtable gates are enforced — do not claim
done before they pass" preamble injected when a session is workflow-bound. The
novel-mode personas keep their read-only delegate posture (no change to their
enforcement).

## §6 Slicing (ship default-off, flip per operator decision — house pattern)

All slices land **default-off** behind a config flag; enforcement strictness is a
staged dial. This mirrors every recent aimee rollout (fold, ingress-compress,
autonomous-dev) where code merges dark and the operator flips.

- **S0 — Interactive workflow catalog + new primitives, as engine invariants.**
  Author `managed-change`, `converse`, `research`, `hotfix` YAMLs + the new
  `understand`/`split`/`review` block executors **and the new terminal
  `gate.deliver` block** (§4 seam 1) + the **per-block allowed-tool-surface**
  field on the block schema (§4 seam 2; wired but not yet enforcing). Selectable
  by explicit name only. No routing, no enforcement. S0 lands these three
  **engine invariants** (per roundtable Rev 3 — enforcement must be structural,
  not conventional):
  - **I1 — structured block artifacts.** `understand`/`split`/`review` emit
    schema-validated artifacts (scoped-intent record / packet-plan / verdict-
    referencing-acceptance-criteria); the engine advances on **structural
    validity of the artifact**, never on a free-text claim. This is what stops a
    regression back to RT1's unsound "detect *I've completed X*" interception.
  - **I2 — `gate.deliver` is a load-time + run-time invariant, not YAML order.**
    The loader **refuses any `enforced: true` workflow that lacks a terminal
    `gate.deliver`**, and the engine **refuses to execute externalization
    primitives** (`pr.open`, `merge`, accept) for a work-item whose
    `gate.deliver` has not passed — regardless of how the YAML is wired.
  - **I3 — single tool-invocation funnel + audited strips.** All primary tool
    calls pass through one funnel (`gateway_policy_strip_tools` or equivalent);
    S0 proves write/edit/patch are covered and logs every stripped tool to
    `lifecycle_event`. (Proves the workflows run end-to-end via the existing
    engine, `gate.deliver` included.)
- **S1 — Router (advisory) + instrumentation.** Deterministic prefilter (thin
  fast-path) + LLM intent classifier + mandatory default + catalog
  router-metadata. Surfaces the chosen workflow to the user and logs it; primary
  still runs free. **Defines and emits the metrics + acceptance thresholds up
  front** (§8: P50/P95 per-turn latency, classifier cents/turn, panel turnaround,
  expedite ratio, router precision/recall on a labeled set) — observe-only is the
  place to instrument, but the thresholds are set *before* this slice ships, not
  after. Prompt/role rewrite (§5).
- **S2 — Binding + interactive driver + lifecycle (soft).** Bind interactive
  sessions to the matched work-item; implement the **interactive driver loop**
  and the **interactive lifecycle** rules (§4: single-writer binding, child-packet
  scope-add, orphan/resume, gate watchdog); turn on **per-block tool-stripping**
  so `converse`/`review` are genuinely read-only. Also lands the concrete
  **hotfix-vs-managed routing heuristic** (§7 R5) before real traffic is routed.
  Workflow state surfaced in chat. `gate.deliver` present but not yet blocking.
- **S3 — Enforcement (staged).** Turn `gate.deliver` from pass-through into a
  blocking terminal, dialed advisory-log → soft → hard. Escape hatches +
  degraded-panel parking (reuses existing pause reasons). This is the slice that
  makes review+roundtable non-bypassable. Flip to `hard` is a separate operator
  call, gated on the S1 cost numbers being acceptable **and on S4 autonomous
  parity having shipped** — otherwise a knowledgeable caller could pick the still-
  weak autonomous surface for the same request (RT-Rev2 #10: the S1→S4 window is a
  real enforcement *asymmetry*, not mere drift). If the operator wants interactive
  `hard` before S4, it requires an **explicit signoff** accepting the asymmetry as
  residual risk, recorded like any gate approval.
- **S4 — Autonomous parity.** Route `/v1/dev/submit` + sweep through the same
  router/default; coordinator-as-manager; retire the hardcoded `build`/
  `manual-review` defaults in favor of router decisions.

Each slice roundtable-reviewed (code + design) before its PR, per house rule.

## §7 Risks + open forks (for the roundtable and the user to rule)

- **Fork R1 — matcher engine.** Hybrid (rec.) vs pure-heuristic vs pure-LLM. Cost
  and mis-route risk vs simplicity. §1.
- **Fork R2 — converse/work boundary.** *Policy ruled* (graded floor: bypass /
  hotfix / full — see §Goal ruling). *Still open:* the crisp, testable predicate
  for "substantive change" vs "trivial" vs "read-only." The single biggest UX
  risk — too eager taxes questions, too lax lets unreviewed change slip. Tune
  observe-only in S1 before S3 bites. §3.
- **Fork R3 — enforcement altitude.** Lifecycle deliver-gate (rec., reuses #719
  precedent) vs deeper gateway interception vs pure prompt-convention (weakest).
  §4.
- **Fork R4 — latency/cost tax.** Router LLM + per-change roundtable adds a real
  per-turn cost. Must publish honest numbers (house rule: no cherry-picked
  headline). *(Per roundtable RT4/RT8: no request-hash cache — it would be a
  near-no-op on NL rephrases. The metrics + acceptance thresholds are defined and
  emitted in S1, and the S3 `hard` flip is gated on them.)* Mitigations: prefilter
  fast-path, expedited `hotfix` panel, measure on a live testbed before
  default-on.
- **Fork R5 — trivial-change friction.** *Ruled:* genuine one-liners take the
  expedited `hotfix` workflow — **same required-persona quorum**, faster/cheaper
  models + tighter round/deadline budget (RT-Rev2 #1: no persona reduction, so no
  security regression); read-only bypasses. Remaining detail: the model/budget
  profile for the expedited panel + the size/scope threshold that routes to
  `hotfix` (S2, see below).
- **Interactive gate UX.** Roundtable loop-backs mid-conversation must read as
  helpful ("the panel flagged X, fixing") not as the agent stalling. Surface
  panel verdicts as first-class chat, not silent parks.
- **Backwards-compat.** Existing `/v1/dev/submit` callers passing explicit
  `workflow` must keep working unchanged; router only fills the *unspecified*
  case.

## §8 Success criteria

- A substantive interactive change **cannot** be delivered without a recorded
  primary review + a fail-closed roundtable pass (audited in `lifecycle_event`).
- Conversational (`converse`-lane) turns add **no** delegate/panel cost and only
  the prefilter's fixed overhead; the classifier tax is measured and published
  (P50/P95 latency, cents/turn) rather than asserted as "none."
- The router picks the right workflow on a labeled request set at an agreed
  precision, and *always* falls back to the default (never "unmanaged").
- Honest published cost delta for the managed path; operator has a clean
  observe-only → soft → hard rollout dial.
- **Initial acceptance targets** *(per roundtable Rev 3 #5; confirmed/tuned in
  S1, gate the S3 hard-flip):* router P95 ≤ 500 ms on `converse`-shaped turns;
  managed-path added cost ≤ ~$0.01/turn; expedite (`hotfix`) ratio ≥ 40% of
  substantive changes. Numbers are provisional and re-baselined against the live
  `.254` testbed in S1 — not asserted as final here.

## §9 Revision log

- **Rev 1 (2026-07-01) — roundtable design review.** Panel degraded (1/6
  panelists survived; glm/minimax upstream stalls, see roundtable-invocation
  memory) but the survivor returned 10 substantive items. Folded in:
  - **RT1 (blocker):** no `deliver` engine primitive → added terminal
    `gate.deliver` block (S0); dropped the unsound "intercept the *I've completed
    X* text claim" framing. §4.
  - **RT2 (blocker):** no interactive driver (scheduler skips interactive items)
    → defined the explicit interactive driver loop as the core of S2. §4.
  - **RT3 (blocker):** `just chat` → direct edit was a full bypass → `converse`
    lane is now gateway-read-only (write tools stripped, #719 pattern) + audit
    log + test. §3/§4.
  - **RT6:** `review` block constrained read-only (re-delegate changes) so the
    manager thesis holds. §2/§4.
  - **RT7:** added the interactive lifecycle rules (single-writer, child-packet
    scope-add, orphan/resume, gate watchdog). §4.
  - **RT10:** resolved `understand`/`split` as single-shot structured-artifact
    blocks, not multi-turn sub-DAGs. §2.
  - **RT4/RT8/RT9:** router honesty — dropped the request-hash cache (no-op on NL
    rephrases), scoped the prefilter to a thin fast-path, committed metrics +
    thresholds to S1 before it ships. §1/§7/§8.
  - **RT5:** hotfix-vs-managed routing heuristic to be concrete in S2. §7.
  - *Re-roundtable recommended once a panel is non-degraded, to catch what the 5
    stalled panelists would have added.*
- **Rev 2 (2026-07-01) — second roundtable (single-round, security-focused
  panelist survived).** New axis: the *threat model*. Added §4.5 (trust
  boundaries + bypass vectors) and resolved:
  - **#1 (blocker):** `hotfix` "reduced panel" contradicted §8's fail-closed
    guarantee → redefined "expedited" as faster models + tighter budget, **same
    required-persona quorum** (no security regression). §Goal, §7 R5.
  - **#2:** read-only ≠ confidential — scoped the "closes RT3" claim to mutation;
    read-surface exfiltration explicitly out of scope. §4.5.
  - **#3:** workflow YAMLs = operator-write-only / delegate-read-only, loader
    rejects non-operator files. §4.5.
  - **#4:** `enforced:false` never classifier-reachable for change (explicit-name
    only). §4.5.
  - **#5:** interactive `wfe_engine_advance` has no gate-satisfaction authority —
    gates still require the engine's existing fail-closed/HMAC satisfaction. §4.5.
  - **#6:** delegate→review trust boundary — schema-enforced artifacts + untrusted
    rendering + independent panel replay. §4.5.
  - **#7:** per-turn tool-surface snapshot at ingress (closes the mid-turn
    advance TOCTOU). §4.5.
  - **#8:** classifier is untrusted — catalog-id-constrained output + prefilter
    veto of `converse` on change imperatives. §4.5.
  - **#9:** child-packet scope-add must re-gate intersecting passed nodes (no
    append-without-re-gate). §4.5.
  - **#10:** S1→S4 enforcement asymmetry — S3 `hard` flip gated on S4 parity or
    explicit operator residual-risk signoff. §6.
  - *Panel still degraded (1/6, non-security personas stalled). A clean full-panel
    pass would add correctness/architecture coverage on the Rev-1/Rev-2 changes;
    worth one more attempt before S0, but the security surface is now well-mapped.*
- **Rev 3 (2026-07-01) — third roundtable, healthier panel (2/6 survived after
  the mistral vault key was rotated back to healthy; correctness-focused).** 13
  items, 3 blockers — all converging on "enforcement must be an *engine
  invariant*, not a convention." Folded in:
  - **#1 (blocker):** interactive block-completion signal undefined → **S0
    invariant I1** (schema-validated `understand`/`split`/`review` artifacts;
    engine advances on structural validity, not free text). §6.
  - **#2 (blocker):** `gate.deliver` relied on YAML ordering → **S0 invariant I2**
    (loader refuses `enforced:true` without terminal `gate.deliver`; engine
    refuses externalization primitives pre-pass). §6.
  - **#3 (blocker):** per-block tool-strip funnel underspecified → **S0 invariant
    I3** (single funnel, write/edit/patch covered, strips logged to
    `lifecycle_event`). §6.
  - **#4/#11:** graded default-fallback — ambiguous/errored classification →
    `research` (read-only), not heavy `managed-change`; never `converse` for
    imperatives. §1.
  - **#5:** concrete initial acceptance targets (router P95 ≤500ms, ≤~$0.01/turn
    managed, ≥40% expedite). §8.
  - **#6:** named the enforcement ceiling — "primary can't ship," not "user can't
    ship" (converse copy-paste is out of scope by design). §3.
  - **#7/#9/#13 (S2 UX):** resume-recap on rebind, panel-degradation user-facing
    recovery, and user-visible lifecycle events (rejected 2nd-tab, parked panel)
    — assigned to S2's interactive-lifecycle work.
  - **#8 (S4):** interactive↔autonomous concurrency on one repo needs a
    work-item mutex / conflict surfacing — assigned to S4 (or earlier if it bites).
  - **#10:** workflow version is already pinned at work-item creation (§0); in-
    flight items use the start-time version — stated explicitly.
  - **#12 (S2):** audit-log volume from per-turn strip/routing rows — add a
    back-of-envelope row-rate estimate + retention note in S2.
- **Rev 4 (2026-07-01) — I1/I3 implementation design consult (non-degraded, 32
  items).** Panel ruled proceed A/A/A/B on the four forks with hardening that
  expands the I1/I3 spec — folded here as the build contract:
  - **Q1 `review` block = read-only reviewer-persona delegate now (primary at
    S2), WITH:** read-only enforced at the *tool/dispatch layer* (not persona
    prompt); the reviewer persona pool an **engine-enforced disjoint set** from
    the roundtable pool AND the producing delegate (anti-rubber-stamp); a
    **bounded** review→re-delegate loop that terminates in **ERROR** (not silent
    LOOP) on exhaustion with a lifecycle_event; and a **structured versioned
    verdict schema** (`{verdict, blocking_findings:[{block_id,rule_id,expected,
    observed,suggested_fix}], non_blocking}`) — schema-invalid = non-advancing
    fail. Division of concern: review = intent-alignment, roundtable = quality/
    security.
  - **Q2 understand/split = delegate artifacts now, WITH typed versioned
    schemas** (intent record + packet plan), reserved S2 extension slots
    (`user_clarifications`, `replay_anchor`, `with_user_session_ref`), S1 intent
    stamped `unconfirmed`, and a **contract test both the S1-delegate and
    S2-primary executors satisfy**.
  - **Q3 I3 = defer request-side per-block tool-strip to S2, BUT the
    externalization refusal is a *dispatch-time invariant keyed on run-state***:
    a delegate in a run whose `gate.deliver` has not ADVANCED gets **zero**
    externalization primitives — a **denylist far broader than pr.open/merge**
    (git push, writes to remote/protected refs, shell-exec, raw network egress,
    MCP side-effect tools, CI edits, posting comments). Per-block `allowed_tools`
    (S2) layers on top, never replaces this baseline. `allowed_tools` carries a
    `status: {audit_only|enforced}` marker + emits a lifecycle_event whenever a
    block runs while not `enforced` (so "looks-enforced-isn't" is observable).
    **Sequencing (hard):** no executor that could externalize ships without this
    dispatch-time guard in the SAME slice.
  - **Q4 gate.deliver = re-verify (B), done as a structural lookup over
    engine-owned APPEND-ONLY verdict records** (not mutable/delegate-writable
    state — TOCTOU/forgery), as a policy over the run's **verdict graph** ("all
    gates whose on_pass feeds deliver have an approving record"), not a hard-coded
    review+roundtable allowlist; **no LLM re-judgement**. On failure →
    **ERROR/halt** (dedicated terminal code + worktree quarantine + verdict-chain
    snapshot event), never LOOP.
  - **First increment landed (this slice):** `wfe_manager_artifacts.{h,c}` — the
    typed versioned intent/packet/verdict schemas + validators + the dual-path
    contract test (`test_wfe_manager_artifacts.c`), including the S2
    forward-compat round-trip. Executors + dispatch-time externalization guard +
    verdict-record store follow in dependency order behind this contract.
  - *Panel is now non-degraded-enough for real coverage (2 reviewers). The
    diagnosis+fix that got us here: the `.254` roundtable panel was starved to 1
    healthy provider (mistral vault key was 401 / codex quota-exhausted / mimo
    endpoint-400); rotating a valid mistral key restored a 3-provider pool. See
    the roundtable-invocation memory for the full panel-health diagnosis.*

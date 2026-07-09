# Proposal: Local-first memory & trust patterns — concepts to adopt

- **State:** PENDING — concepts/design only, no code in this PR. A survey of
  seven transferable patterns drawn from a from-scratch, consumer-hardware
  agentic harness, re-expressed as ideas and mapped onto aimee's existing
  substrate. Every item is scoped as **reuse/extend what exists**, not build anew.
- **Author:** JBailes
- **Date:** 2026-07-09
- **Charter roles:** Persist (memory synthesis, quarantine, compaction),
  Review (out-of-band approval gates, provenance, undo), Execute (idempotent
  side effects), Reason (trust-tier gating).
- **Reviewed by:** multi-persona roundtable panel (security, architect, QA,
  contrarian, constructive), two passes. Round 1 raised 8 blocking findings;
  round 2 (confirmation) returned **zero blocking findings** — only refinement
  suggestions, now folded in. Converged. See "Roundtable resolutions" at the end.

---

## Verification status (read first)

Claims below of the form *"aimee already has X"* are **inferred from source-file
names, `docs/`, and public symbols — not yet confirmed against implementation.**
They are marked **[verify]** in the traceability table and MUST be validated before
any item is accepted. In particular, `test_vault_capability` is a **test artifact**,
not evidence of a production capability model, and is treated as such here. This
proposal deliberately does not assert internal behavior it has not read; where a
claim drives a design decision, the decision is written to be correct whether or not
the claim holds.

### Traceability of "already has" claims

| Item | Claim about aimee | Evidence pointer | Status |
|------|-------------------|------------------|--------|
| 1 | pending/TTL lifecycle + curator drain + context compaction | `memory_lifecycle.c`, curator drain, `context_reduce.c` | [verify] — confirm PENDING semantics + promotion paths |
| 2 | memory packing/assembly is budget-aware | `memory_pack`, `memory_assemble`, `context_reduce` | [verify] — confirm current compaction is LLM-summary vs deterministic |
| 3 | PII/fact gates exist; no assembly-time provenance labelling | `memory_pii_gate.c`, `memory_fact_gate.c` | [verify] — confirm no owner-gate on passive injection today |
| 4 | per-delegate tool scoping + personas | `cmd_agent_delegate.c`, persona configs | [verify] — confirm default (allow vs deny) for an unscoped non-owner session |
| 5 | autonomous-dev mechanical verify + worktree isolation | `git_verify`, autonomous-dev substrate docs | [verify] — confirm approval reachability from agent context |
| 6 | WFE steps + delegate job leases (concurrency guard) | `db1_agent_job_take_lease` | [verify] — confirm no arg-derived sequential-retry dedupe exists |
| 7 | full skills subsystem | `skill_curator`, `skill_review`, `cmd_skill` | [verify] — confirm no tool-count capture trigger today |

**Acceptance of any item is gated on turning its row green** (a real symbol/behavior
match), not on the prose below. The "Have" designations elsewhere in this document
are therefore **provisional** — read each as "assumed-present, pending verification" —
and every item's design is written to **degrade gracefully if the assumption fails**
(the missing capability is simply built rather than extended).

---

## Threat model & scope

The patterns target three failure modes that appear the moment an agent acts
autonomously and consumes content it did not author:

1. **Indirect prompt injection (IPI)** — instructions smuggled through tool output,
   retrieved memory, or an inbound channel message.
2. **Unattended memory drift** — the agent's own self-authored notes silently
   becoming "facts" it acts on.
3. **Duplicated / unauthorized side effects** — a retried or mis-scoped call sending
   a message twice, opening two PRs, or granting the agent a capability.

**Trust principals.** "Owner" here is shorthand for *an authenticated principal on a
trusted channel*, not a bare boolean an agent can flip. Any control below that keys
off "owner" is only as strong as the channel authentication behind it; where the
source used an in-process boolean, this proposal calls for deriving it from the
transport's authenticated identity (see #4). **Prompt-level labelling (#3) is
defense-in-depth, not an enforcement boundary** — the enforcement boundaries are the
structural ones (owner-gated injection, default-deny tool resolution, out-of-band
approval).

None of this replaces aimee's memory/curator internals; it adds a small set of
patterns at the edges where aimee crosses trust boundaries.

---

## 1. Event-triggered synthesis into a quarantine lane

**Idea.** Fire the same summarizer the context-compactor already uses under memory
pressure — but proactively, on an **explicit lifecycle event**, not on wall-clock
"idle" (autonomous-dev and headless runs may never idle). Triggers: session/turn
close, a turn-count threshold, or delegate-job completion. Distil what was
said/decided/discovered into a compact note that lands in a **structurally isolated
quarantine tier**, provenance-tagged (`self-synthesized`, session, timestamp),
**never auto-promoted** to identity/critical tiers by any automated stage, reviewable
as a list, and **individually undoable**.

**Undo rebuilds from the verbatim surviving notes** (the authoritative copy, per #2 —
never from the compressed/summarized form), so what undo consumes is unambiguous and
consistent with the rest of the design. Two rollback strategies, chosen by whether the
summarizer is deterministic — an explicit **[verify] precondition**, since an LLM
summarizer generally is *not*:

- **If the summarizer is deterministic** (or a deterministic compaction is used for
  this tier): re-run it over the ordered surviving verbatim notes — a pure function of
  its inputs.
- **Fallback (assume non-deterministic):** **snapshot the prior rolling-summary at
  each write.** Undo restores the immediately-preceding snapshot rather than
  regenerating, giving true rollback without depending on summarizer stability.

Derived artifacts (a promoted claim, a KB entry) are linked to their quarantine source
so undo can find and flag them rather than orphaning them.

- **aimee already has [verify]:** `memory_lifecycle` PENDING+TTL, curator drain,
  `context_reduce`.
- **Concrete have/missing split:** *Have* — a state machine with a PENDING state and
  a summarizer. *Missing* — (a) an event trigger for proactive synthesis distinct
  from pressure-compaction; (b) a provenance class for *self-generated* writes that
  **no curator stage may promote** (PENDING today gates *unconfirmed third-party
  claims*, a different thing); (c) a review + single-item-undo surface with derived-
  artifact linkage.

## 2. Compressed-in-context / verbatim-on-demand split, with a shorthand codec

**Idea.** Store each memory as a **compressed summary** (injected) linked to the
**verbatim original** (stored, never injected, recalled only by an explicit tool).
Injection is **budget-aware**: admit up to `ctx − base − tool_schemas − reserve`,
most-relevant-first. The compressed form may use a **deterministic shorthand codec**
(abbreviation map + filler stripping).

**Bounded to avoid the panel's semantic-loss/collision risk:** the codec is
**lossy-summary-only and never the sole copy** — the verbatim original is always
retained and authoritative, so a bad abbreviation degrades a hint, never a fact. The
abbreviation map is **versioned and persisted** (stored with each compressed row's
codec version) so a map change never silently re-interprets old rows; collisions are
prevented by requiring whole-token, longest-match replacement against a curated map,
not substring rewriting.

- **aimee already has [verify]:** `memory_pack`, `memory_assemble`, `context_reduce`.
- **Have/missing split:** *Have* — packing/assembly and a token budget. *Missing* —
  a materialized compressed/verbatim *pair as linked rows* (stable, auditable
  injected footprint) and an optional deterministic codec stage on top of existing
  summarization.
- **Scope caveat:** low payoff on large cloud contexts — **constrained/small-model
  tiers only.** Ranked last accordingly.

## 3. Owner-gated injection (the boundary) + soft provenance labelling (a mitigation)  *(prerequisite for #4)*

**Idea.** Two distinct controls. Only the first is a trust boundary; the naming here
keeps them separate on purpose.

**3a — The boundary (structural enforcement).** Passive injections (memory, project
lists, bio) are **owner-gated** — a non-owner session never receives them, so there is
nothing to leak regardless of what the model does. This is the real fix and the only
part that is an enforcement boundary.

**3b — Soft provenance labelling (mitigation, not a boundary).** Non-owner input (tool
output, inbound messages) is **labelled inline** as data-not-instructions as it enters
history, and a session-sticky flag injects a provenance reminder. This *reduces* IPI
susceptibility but is **not** a guarantee — a capable injection can still talk past a
label. It is defense-in-depth layered under 3a, never a substitute for it, and is not
described anywhere as a "boundary."

> Illustrative (external source, not an aimee observation): a comparable system had a
> public channel recite the owner's hostname from *passive memory injection* with
> zero tool calls — the leak was on the injection path, not the tool call. The
> transferable lesson is where to look, not a claim about aimee's current behavior.

- **aimee already has [verify]:** `memory_pii_gate`, `memory_fact_gate`, delegate
  scoping — gating *storage* and *tool actions*, not *how content is labelled in the
  assembled prompt*.
- **Have/missing split:** *Have* — gates on write and on tool dispatch. *Missing* — a
  provenance-labelling convention at assembly time (soft) **and** an explicit owner
  check on every passive-injection site (hard). The action item is a concrete audit:
  enumerate every place aimee appends context to a prompt and confirm the owner gate,
  separate from the tool-dispatch gate. **Effort re-estimated: Medium** (a
  cross-cutting audit of all injection sites, not a one-file change).

## 4. Unified trust-tier tool policy for non-owner sessions

**Idea.** One policy resolver — not a second parallel mechanism — that every session
construction site calls. It composes with aimee's existing per-delegate scoping
rather than contradicting it: today's scoping decides *which* tools a delegate gets;
this adds the **default and the failure mode** around that decision.

- **Owner = authenticated principal**, derived from the transport's verified identity,
  **not a bare in-process boolean** (the source's weakness). An unauthenticated or
  unclassified session is non-owner by construction.
- **Default-deny + fail-closed:** a non-owner session with no resolvable profile gets
  *nothing*; a missing/typo'd/broken profile denies rather than inherits.
- **Sensitivity tiers** (`read_only` / `write_local` / `execute` / `self_modifying` /
  `outbound_action`); **unclassified ⇒ most-restrictive.**
- **Structurally absent, not disabled:** the highest-risk capabilities
  (self-modification, tool creation, autonomous-dev approval) are **never registered**
  into a non-owner session's tool universe.
- **On step-up auth:** the source's PIN-entered-in-chat is **explicitly not
  recommended** — sending a secret through the model's channel is a leak and is
  phishable/replayable. For aimee, step-up should be an **out-of-band or
  channel-authenticated** action (the verification *state* is consulted at dispatch;
  the secret never transits the chat transport).

- **aimee already has [verify]:** per-delegate tool scoping, personas.
- **Have/missing split:** *Have* — per-delegate scoping. *Missing* — a single
  resolver enforcing default-deny/fail-closed, unclassified⇒restricted,
  structural-absence for the top tier, and owner-derived-from-auth. Depends on #3
  (a non-owner session must also be denied passive injection), so **#3 lands first.**

## 5. Self-expansion behind an out-of-band human gate + append-only audit

**Idea.** When an agent authors a capability for itself (a tool, and by extension a
skill/template/persona that can steer future behavior), **two separate gates apply,
and only the second is a safety gate:**

1. **Syntax check (`compile()`, never `exec()`)** — a *correctness* filter only. It
   proves the artifact parses; it proves **nothing** about safety. Stated explicitly
   to avoid the conflation the panel flagged.
2. **Out-of-band human approval** — the actual gate. The approval action is
   **deliberately not an agent-callable tool**; a human runs it from a trusted
   surface, unreachable from any chat turn, injected or not.

**Audit log:** every lifecycle event (staged/approved/rejected/deleted) is appended.
Redaction and retention are specified, not hand-waved:

- **Who/when:** redaction runs **at append time** (write-path), before the entry is
  persisted — never deferred to read time, so a secret is never at rest in the log.
- **What:** a concrete detector over the captured source — API keys/tokens (high-
  entropy strings + known key prefixes), passwords/connection strings, and PII —
  replaced with typed placeholders (`<REDACTED:token>`).
- **Failure mode:** **redact-and-flag**, and if the detector itself errors, **fail
  closed** — the entry is stored with the raw span withheld and marked
  `redaction_uncertain` for review, rather than written in the clear.
- **Retention:** size-capped and time-capped with rotation (a DoS bound), so an agent
  that stages many artifacts cannot grow the log without limit.

**This gate applies to non-code self-authored artifacts too** — skills, prompt
templates, personas — not only executable tools, since those also alter future
behavior.

- **aimee already has [verify]:** autonomous-dev mechanical `git_verify` gate,
  bounded implement→verify, worktree isolation — **ahead here for code changes; do
  not rebuild.**
- **Have/missing split:** *Have* — mechanical verify + isolation for code. *Missing* —
  (a) the principle that approval is unreachable from the producing context, applied
  to *all* self-authored artifacts including non-code; (b) a redacted, retention-
  bounded, full-source audit trail.

## 6. Idempotency ledger for side-effecting calls

**Idea.** A small ledger that dedupes retried side effects, keyed on a
**canonicalized** hash of the call's semantic arguments.

**Addressing the panel's determinism/partial-failure findings:**
- **Canonicalize before hashing:** drop non-semantic fields (timestamps, nonces,
  trace ids), normalize ordering/whitespace, so a genuine retry hashes identically and
  a distinct call does not. The key derives from *intent*, not wire bytes.
- **Two-phase, not cache-on-success-only:** record `in_flight(request_id)` **before**
  the effect, transition to `done(result)` after. A retry that finds `in_flight`
  blocks/queries rather than re-firing; a crash mid-effect leaves a resolvable record
  instead of a silent double-send. **Failures are not cached as success.**
- Scope to genuinely idempotency-sensitive effects (the **outbound-action tier**);
  read-only calls skip the ledger.

- **aimee already has [verify]:** WFE steps, delegate job **leases** — guarding
  *concurrent* double-execution.
- **Have/missing split:** *Have* — concurrency guard via leases. *Missing* — a guard
  for *sequential* retries across process restarts / re-dispatched jobs / re-run
  passes. **Composes with leasing.** **Effort re-estimated: Low–Medium** (the ledger
  is small, but per-tool canonicalization + two-phase wiring is per-call-site work).

## 7. A skill-capture nudge after multi-step workflows

**Idea.** After a session crosses a tool-call threshold, emit a **one-time
capture signal** suggesting the procedure be saved as a skill.

**Not injected into an untrusted session's model context** (the panel's trust-boundary
concern): prefer a **curator-side signal** keyed on the delegate run's tool-call count
that queues a skill-capture candidate for owner review, rather than a prompt string
injected mid-conversation — especially never in a non-owner session. If implemented as
an in-context nudge at all, owner sessions only.

- **aimee already has [verify]:** `skill_curator`, `skill_review`, `config_skills`,
  `cmd_skill`.
- **Have/missing split:** *Have* — the whole skills subsystem. *Missing* — just the
  trigger heuristic (a curator signal on tool-call count), wired to the existing
  capture/review path.

---

## Prioritization

Ranked by value-to-effort, **with dependencies explicit**. #3 and #4 form one
trust-policy workstream and are ordered by dependency (#3 → #4), not lumped.

| Rank | Item | Value | Effort | Depends on | Notes |
|------|------|-------|--------|-----------|-------|
| 1 | **#3** Owner-gated injection + provenance labelling | **High** | **Med** | — | Structural fix + cross-cutting injection-site audit. Prerequisite for #4. |
| 2 | **#6** Idempotency ledger (canonicalized, two-phase) | **High** | Low–Med | — | Self-contained; protects outbound actions; composes with leases. |
| 3 | **#4** Unified trust-tier policy (default-deny, auth-derived owner) | **High** | Med | #3 | Hardens non-owner sessions; one resolver, not a parallel path. |
| 4 | **#1** Event-triggered synthesis → quarantine + undo | **High** | Med | — | Governs self-authored memory; extends `memory_lifecycle`. |
| 5 | **#5** Out-of-band approval + redacted audit (all artifacts) | Med–High | Low–Med | — | aimee ahead on code; lift the two principles + non-code coverage. |
| 6 | **#7** Skill-capture signal (curator-side) | Med | Low | — | Trigger heuristic on the existing subsystem. |
| 7 | **#2** Compressed/verbatim split + bounded codec | Med | Med | — | Constrained/small-model tiers only. |

**Recommended first cut:** **#3 then #6.** #3 is the highest-value structural fix and
unblocks #4; #6 is self-contained and independently valuable. #4 follows #3; #1 is the
strongest additive memory concept and can proceed in parallel.

**Explicitly not proposed:** adopting the source's memory architecture wholesale, its
persona/voice/TTS layer, desktop UI, backend abstraction, or its **PIN-in-chat**
step-up (replaced by out-of-band/channel-auth here). The value is a small set of sharp
patterns, not a system.

---

## Roundtable resolutions

Blocking findings from the review panel and how this revision resolves them:

1. *Unverifiable "already has" claims / `test_vault_capability` miscitation* →
   Verification-status section + `[verify]` traceability table; acceptance gated on
   green rows; the test artifact is no longer cited as production evidence.
2. *PIN gate leaks secrets in chat* → PIN-in-chat explicitly rejected (#4); step-up is
   out-of-band / channel-authenticated, secret never transits the transport.
3. *Idle trigger vs autonomous-dev (never idles)* → trigger is event-based
   (session/turn close, count threshold, job completion), not wall-clock idle (#1).
4. *Prompt-level labelling ≠ enforcement* → #3 split into a structural enforcement
   control (owner-gated injection) and an explicit defense-in-depth label; threat
   model states labelling is not a boundary.
5. *Compile-check vs human-approval conflation* → #5 states the syntax check is a
   correctness filter only; the sole safety gate is out-of-band human approval.
6. *Ambiguous 80/20 claim* → each item now carries a concrete Have/missing split; the
   quantified framing is dropped.
7. *Passive-injection anecdote unanchored* → labelled illustrative/external, not an
   aimee observation.
8. *Prioritization lumps items / misses deps* → ranking now lists explicit
   dependencies; #3 precedes #4; #2 ranked last with its scope caveat.

Strong non-blocking suggestions also folded in: idempotency canonicalization +
two-phase partial-failure handling (#6); owner-as-authenticated-principal (#4); audit
log redaction/retention + non-code artifact coverage (#5); codec versioning/collision
bounds (#2); deterministic undo + derived-artifact linkage (#1); effort re-estimates
for #3/#6; explicit threat model.

**Round 2 (confirmation) — zero blocking, converged.** The re-review returned only
refinement suggestions, all folded into this revision:

- *#1 undo assumed a deterministic LLM summarizer* → undo now rebuilds from the
  **verbatim** surviving notes (reconciled with #2's authoritative-verbatim guarantee),
  with a **snapshot-at-write rollback fallback** and summarizer determinism marked
  **[verify]**.
- *Traceability "Have" column vs `[verify]`* → "Have" designations declared
  **provisional**; every design degrades gracefully if the symbol proves absent.
- *#3 title called labelling a "boundary"* → section split into **3a (boundary)** and
  **3b (mitigation, not a boundary)**; labelling is nowhere called a boundary.
- *#5 redaction under-specified* → redaction timing (append-time), scope (tokens/keys/
  passwords/PII with a typed detector), failure mode (redact-and-flag; fail-closed on
  detector error), and retention bounds (size/time cap + rotation) now specified.

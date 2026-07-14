# Proposal: One governance policy surface — posture profiles, gate completion, and oversight defaults

- **State:** PENDING — Part 2 of the three-part governance arc
  ([attestable enforcement](governance-attestable-enforcement.md) + this +
  [agent identity & artifact trust](governance-agent-identity-and-artifact-trust.md)).
- **Origin:** autonomous overnight governance deep-dive commissioned by JBailes,
  2026-07-13.
- **Charter roles:** Enforce / Gate-Promote / Classify-Score.
- **Relation to prior work:** the done
  [governance-decision-records-and-action-audit](../done/governance-decision-records-and-action-audit.md)
  proposal declared policy consolidation a non-goal *"until a concrete need
  appears."* The need has appeared: autonomous execution is now default-on
  (`wfe_live_forge_enabled = 1`, operator ruling 2026-07-13, `src/config.c:811`;
  triggers default `mode: "autonomous"`, `src/config_trigger.c:13`) while the
  defensive gates that should envelop it are default-off, shadow-mode, or wired
  into a single call site.

## Thesis

aimee's individual controls are strong; its *posture* is incoherent. Verified
defaults as of today (`src/config.c` initializer unless noted):

| Control | Default | The problem |
| --- | --- | --- |
| `wfe_live_forge_enabled` | **ON** | autonomous PR/merge ops on by default (bounded by the merge-target rail) |
| trigger `mode` | **`autonomous`** | watch-dir content starts hands-off runs with no default human gate |
| `integrity_enabled` (injection/poison gate) | **OFF** (`:698`) | the one deterministic anti-injection control is inert |
| `integrity_dry_run` | **ON** (`:699`) | even when enabled it only shadows |
| integrity gate wiring | **one call site** (`src/learning_router.c:419`) | KB ingest, memory writes, webchat, and — critically — watch-dir trigger content are unscreened |
| `guardrails_blast_radius_advisory_enabled` | **OFF** (`:756`) | blast-radius advisory never fires |
| `audit_worm_enabled` | **OFF** | tamper-evident audit idles (Part 1, A1) |
| TCP rate limit | 0 = **unlimited** (`src/server/server_http.c:1000`) | no default backpressure on a bearer |

Each flag is individually defensible; the combination — *autonomy on, defenses
off* — is not a posture anyone would choose on purpose. An operator cannot
reasonably be asked to know twelve flags and their interactions. The unit an
operator reasons about is a **profile**; the unit a reviewer audits is the
**mapping table from profile to flags**; the unit the chain records (Part 1, A3)
is the **policy revision** that mapping produces.

This is also the industry's converged shape: declarative policy with
`allow / deny / require_approval` verdicts, fail-closed evaluation, a default
action, and staged rollout — evaluated *outside the agent's reasoning loop*.
aimee has every mechanism (verdict choke points, HMAC human gates, staged
enforcement dials, park/resume); it lacks the single surface that composes them.

## Deltas

### B1 — Governance profiles: `observe | standard | hardened`

One config key, `governance.profile`, expanding to a **pinned, published table**
of the flags above (plus Part 1's audit flags). Explicit per-flag settings always
win (profile = defaults layer, not a lock). Sketch:

| Flag | observe | standard | hardened |
| --- | --- | --- | --- |
| `audit_worm_enabled` | on (dual-write) | on (authoritative) | on (authoritative, fail-closed) |
| `integrity_enabled` / `dry_run` | on / shadow | on / enforce for block-severity non-user content | on / enforce |
| trigger `mode` default | autonomous | interactive unless the workflow declares a human/roundtable gate before externalization | interactive |
| `wfe_live_forge_enabled` | on | on (rail-bounded) | on, but every forge op behind `gate.human` |
| blast radius | advisory | advisory | enforce (block over threshold, operator override) |
| native-gate stage (`AIMEE_WORKFLOW_ENFORCE_STAGE`) | advisory | soft | hard |
| TCP rate limit | unlimited | sane non-zero default | sane non-zero default |

The active profile + resolved flag set becomes Part 1's `policy_rev` /
`policy.snapshot`, so every chained verdict names the posture that produced it.
Default ships `observe` (today's behavior, made honest), with `standard` the
documented recommendation; flipping the *default* to `standard` is a later,
separately-decided operator ruling recorded as a `decision_log` entry.

### B2 — Integrity-gate completion (wire the gate where content actually enters)

`integrity_gate_check()` already classifies MEMORY_RESET / IDENTITY_OVERRIDE /
AUTHORITY_CLAIM / INSTRUCTION_INJECTION / ENCODED_PAYLOAD with source-sensitive
ACCEPT/QUARANTINE/REJECT (`src/integrity_gate.c:247-315`) and is obfuscation-
normalizing. It guards one path. Wire it, staged shadow→enforce per source, into:

1. **Trigger content materialization** (`trigger_scheduler` watch-dir/proposals
   sources) — the highest-value site: a `.md` dropped in a watched dir is today
   an unscreened instruction channel into a default-on autonomous forge pipeline.
   Source class `document`; REJECT parks the work item with the verdict attached
   (human sees *why* in webchat), QUARANTINE forces `mode: interactive` for that
   item regardless of rule mode.
2. **Memory/KB write path** — at the `memory_fact_gate` seam so agent- and
   delegate-sourced memory is screened before persistence (complements, does not
   replace, the pending
   [evidence provenance tiers](proposal-evidence-provenance-tiers.md) — that
   proposal decides *how much a stored memory is trusted*; this one decides
   *whether hostile content is stored at all*).
3. **KB document ingest** ahead of the Normalize stage (the pending
   [org-data connectors](org-data-connectors-and-source-ingestion.md) intake
   inherits the same seam when it lands).
4. **Webchat inbound attachments/pastes** as source class `web` (user-typed chat
   stays `user_stated` — the gate's source model already distinguishes this).
5. **Retrieved-content assembly (recall injection)** — shadow-only in v1:
   annotate, never block, so a poisoned stored memory is at least *flagged* at
   the point it re-enters a context window.

Every verdict emits an `ingest.integrity` chain row (Part 1, A2). The gate stays
deterministic and fail-open in `observe`, fail-closed for REJECT in
`standard`/`hardened`.

### B3 — `require_approval` as a first-class verdict

Today's `pre_tool_check` contract is `0=allow / 1=rewrite / 2=block`
(`src/headers/guardrails.h:134`); human approval exists only as a workflow-graph
node (`gate.human`, HMAC-signed, `src/workflow/wfe_approval.c:217-237`). The
missing verdict is **park-for-approval on a rule match** — the triad every
surveyed policy layer converged on (`allow / deny / require_approval`), and
OWASP ASI09's control shape.

- New outcome `3=approval_required` from `pre_tool_check_inner`, produced by
  rules (initially: the native-gate externalization classes under `standard`
  where `hardened` would hard-deny; blast-radius over-threshold; forge ops under
  `hardened` per B1).
- **Workflow/delegate paths get true parking**: the engine already parks
  `pending_human` — route the verdict there; resume re-runs the check with the
  approval token attached, satisfied only by the existing HMAC gate. Approvals
  land in the chain (`gate.approve` rows exist in the WORM action registry).
- **Interactive primary paths get deny-with-instruction**: hooks are synchronous
  and cannot park a foreign harness, so the PreToolUse deny JSON carries "this
  action needs operator approval — approve in webchat (item N), then retry" —
  the retry finds the approval recorded and allows. No new UI: the webchat gate
  endpoint (`webchat/workflow.go:132`) is the approval surface.

### B4 — Autonomy oversight: containment, not just configuration

- **Kill switch.** `aimee autonomy stop [--scope trigger|run|all]`: parks every
  running autonomous work item at its next step boundary, disarms trigger rules,
  and revokes outstanding delegate job leases (`db1_agent_job_take_lease` seam)
  so parked work cannot be re-taken. Today's opt-outs are config flags and
  per-op re-checks (`forge_allowed()`, `src/server/wfe_live_forge.c:35-47`) —
  good bounding rails, but there is no single operator action that *stops the
  fleet now* and leaves a chain row (`autonomy.stop`) saying who stopped it and
  what was parked. Restart is deliberate (`aimee autonomy resume`, distinct row).
- **Cumulative spend ledger.** Caps exist per trigger (`max_spend_usd`), per
  ensemble, per autonomy intake — but nothing accumulates spend per agent/scope
  across runs. Add a rolling ledger (existing cost-accounting rows, summed per
  principal/scope/day) with a soft ceiling → `require_approval` (B3) and a hard
  ceiling → park. This converts cost from a per-run parameter into governance.
- **Externalization always gated in `hardened`.** The union of the native gate's
  externalization classes (`src/workflow/wfe_native_gate.c:158-208`) and forge
  ops is the "leaves the machine" set; `hardened` routes all of it through B3.

## Non-goals

- **Policing un-mediated actors.** Deployment precondition (operator ruling
  2026-07-13): properly configured clients route LLM traffic through aimee's
  gateway and install the hooks. This proposal governs the mediated population;
  discovering the unmediated one is Part 3's registry.
- **A policy DSL / external policy engine (OPA/Cedar).** The decision cores stay
  the existing C code; the profile is a defaults layer over flags that already
  exist. A declarative rule language is a possible later surface, not v1.
- **Replacing the merge-target rail or HMAC gates** — B1/B3/B4 compose them.
- **LLM-based injection scoring.** The integrity gate stays deterministic;
  semantic guardrails remain their own (existing, default-off) surface.

## Risks / honest limits

- **Profile ≠ security boundary.** A profile is configuration; an operator (or
  anyone with config write access) can lower it. Part 1's `policy_rev` chain rows
  make that *visible and provable*, which is the design: posture changes are
  governed events, not silent drift.
- **B2-1 false positives** on watch-dir proposals (a proposal *about* injection
  patterns will trip the gate — this very document contains the category names).
  QUARANTINE→interactive (not REJECT) is the default verdict for block-severity
  `document` content precisely so a human adjudicates; patterns get an
  allowlisted fenced-code/quotation carve-out before enforce mode.
- **Approval fatigue.** `standard` gates only externalization + over-threshold
  blast radius; if operators rubber-stamp, the answer is narrowing rules, not
  removing the verdict. Approval latency parks work items — bounded by the
  existing park/resume machinery, and `observe` keeps today's flow.
- **Kill-switch scope.** `autonomy stop` stops aimee-managed work; it cannot
  stop an external harness mid-Bash. That tier is OS isolation (Part 3 non-goal
  discussion) — stated, not implied away.

## Tests

- B1: profile expansion table golden-tested; explicit flag overrides win;
  profile change → new `policy_rev` + `policy.snapshot` row.
- B2: seeded hostile `.md` in a watch-dir → QUARANTINE → item parked interactive
  with verdict attached + `ingest.integrity` row; same content as `user_stated`
  chat → ACCEPT; fenced-code carve-out passes a proposal that *quotes* patterns.
- B3: matching rule under `standard` → `approval_required` → parked item →
  HMAC-approved resume → allow row referencing the approval; forged approval
  fails; interactive primary gets the deny-with-instruction JSON and succeeds
  after webchat approval.
- B4: `autonomy stop` parks running items, disarms rules, revokes leases, writes
  `autonomy.stop`; spend ledger crossing soft ceiling flips to
  `approval_required`; hard ceiling parks.

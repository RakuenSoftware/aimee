# Proposal: Attestable enforcement — complete the WORM trust anchor and make every verdict provable

- **State:** PENDING — Part 1 of the three-part governance arc
  (this + [governance posture & policy surface](governance-policy-surface-and-posture.md)
  + [agent identity & artifact trust](governance-agent-identity-and-artifact-trust.md)).
- **Origin:** autonomous overnight governance deep-dive commissioned by JBailes,
  2026-07-13 (codebase enforcement-map + threat-coverage sweep + verified external
  landscape research).
- **Charter roles:** Enforce / Constrain-Verify / Gate-Promote.
- **Depends on:** shipped WORM store
  ([auditable-worm-audit-store](../done/auditable-worm-audit-store.md)) and shipped
  per-action audit
  ([governance-decision-records-and-action-audit](../done/governance-decision-records-and-action-audit.md)).
  Renders through the pending
  [operator-audit-activity-surface](operator-audit-activity-surface.md).

## Thesis

The governance question the industry cannot yet answer is not *"do you enforce
policy?"* — it is *"can you prove, without trusting the governed agent, that
enforcement actually held?"* An enforcer that shares a trust boundary with the
workload it constrains can be subverted, not merely ignored; the trust anchor has
to live outside the governed process or it doesn't exist.

aimee is structurally ahead here. Its authoritative enforcers already sit outside
the governed agent's process — `pre_tool_check` runs server-side for delegates and
MCP dispatch (`src/server/server.c:924`, `src/server/server_compute_async.c:206`),
the gateway rewrites the primary's LLM traffic in aimee's own process
(`src/gateway_policy.c`), workflow gates are engine-owned and HMAC-non-forgeable
(`src/workflow/wfe_approval.c`), and a hash-chained, MAC-checkpointed WORM store
exists and verifies (`src/modules/audit/audit_worm.c`, `src/modules/audit/audit_worm_chain.c`). What is missing
is the last mile that turns "we log verdicts" into "we can attest verdicts":

1. the tamper-evident chain is **default-off** (`audit_worm_enabled` has no
   initializer assignment → 0) while the default-on audit path (`audit.log`,
   `audit_action_enabled = 1`, `src/config.c:818`) is **not chained**;
2. several live enforcers **never reach the chain at all** (attention guard,
   gateway policy, memory interception, integrity gate, native gate, vault log,
   trigger/forge ops each write their own side log or nothing);
3. a verdict row does not record **which policy was in force**, so a row cannot
   prove what was enforced, only that something was;
4. the two components that keep the chain trustworthy against a compromised host —
   the **privileged sealer sidecar** (R2-5) and the **signed off-host anchor**
   (R2-8) — shipped as carried follow-ups, not code.

Close those four and aimee ships something none of the surveyed platforms ship:
per-action, policy-versioned, hash-chained, externally-anchored enforcement
evidence, produced by enforcers that do not live in the governed agent's process.

## External context (verified against primary sources, 2026-07-13)

- The major agent platforms converged in 2025–26 on **gateway-enforced,
  out-of-loop policy** (Cedar-evaluated tool-call policy at a managed gateway,
  default-deny; control-plane guardrails per tool call; org-level model/tool-call
  screening with per-agent SPIFFE-based identity). "Don't let the agent police
  itself" is now the shipped industry default — the architecture aimee already has.
- **None of them attest enforcement.** The audit ceiling in shipping systems is
  integrity-validatable log digests; per-action hash-chained agent audit exists
  only as an unadopted IETF individual draft
  (`draft-sharif-agent-audit-trail-00`, SHA-256 `prev_hash` chaining, explicitly
  targeting EU AI Act **Article 12** logging, effective 2026-08). TEE products
  attest *code identity*, never per-action verdicts.
- The trust-boundary framing (ARMO, 2026-05: "the trust anchor lives outside the
  agent's process, or it doesn't exist"; kernel-side evidence over workload
  self-reports) matches the WORM proposal's own §7 out-of-band anchor design —
  which is exactly the deferred piece.
- OWASP Top 10 for Agentic Applications (2025-12) makes this ASI-legible:
  this proposal is the ASI10 (rogue agents) / ASI03 (identity & privilege abuse)
  *evidence* layer — the ability to detect and prove, not just configure.

Sources: armosec.io/blog/ai-agent-governance/ · federalregister.gov 2026-00206
(NIST-2025-0035) · genai.owasp.org Top 10 for Agentic Applications (2026) ·
datatracker.ietf.org draft-sharif-agent-audit-trail.

## What already exists (do NOT rebuild)

| Piece | Where | Status |
| --- | --- | --- |
| Per-tool-call verdict + exactly-once audit emit | `pre_tool_check` wrapper, `src/guardrails_action_audit.c:90-146` (fail-open post-verdict; actor primary/delegate; keyed-HMAC `args_hash`) | shipped, default-on to `audit.log` |
| Hash-chained WORM store, MAC checkpoints, green/amber/red verify | `src/modules/audit/audit_worm.c`, `src/modules/audit/audit_worm_chain.c`; dedicated chain key `.audit-chain-key` | shipped, **default-off dual-write** |
| Sealing to kernel-immutable segments (crypto-only degrade) | `src/modules/audit/audit_worm.c` (`VACUUM INTO` + `FS_IMMUTABLE_FL`) | shipped; sealer runs in-process, sidecar deferred |
| Decision records (status / supersedes / revisit / linked policy, one-active-per-scope) | `decision_log`, `db2_decision_log_record()` | shipped |
| Vault write audit (append-only, key fingerprints) | `src/server/server_vault.c:185-214` | shipped, separate unchained log |
| kb-side WORM twin (`kb_audit_event`, byte-identical chain code) | db2, plpgsql WORM triggers | shipped, default-off |
| Structured WORM actor `{role, principal_id}` | WORM record model | shipped |

## Deltas

### A1 — One chain, default-on

Flip `audit_worm_enabled` default **on** (dual-write) after the deploy-tier live
verify that the done audit proposal carried; then execute that proposal's own
migration path (§7): parity check → WORM authoritative, `audit.log` derived. The
unchained `audit.log` being the authoritative record while a shipped
tamper-evident store idles default-off is the single cheapest posture fix in the
tree. Rollout: `observe` (dual-write, file authoritative) → `standard` (WORM
authoritative, fail-closed writer per the done proposal's §Durability). Profile
naming is Part 2's; the flag flip is this proposal's.

### A2 — Capture completeness: every enforcer reaches the chain

The done WORM proposal's §4 inventory + CI lint guard, actually executed. Today's
enforcement points that decide allow/block but never write a chained row:

| Enforcer | Today's sink | Delta |
| --- | --- | --- |
| Attention guard / session-worktree isolation (`src/cli_attention_guard.c:617-666`) | per-session JSON under `.cache/attention/` + stderr | emit `tool.guard` verdict via the existing hook→server report path; client-side blocks buffered and flushed on next server contact (hook subprocesses must not need DB access) |
| Gateway policy (`src/gateway_policy.c:94-211`: subagent strip, response police, model pin) | none | `gateway.policy` rows: what was stripped/pinned, request id — the gateway is aimee's own process; direct `audit_event()` |
| Memory interception (`server_memory_intercept`, `src/server/server.c:729,907`) | `interception.jsonl` | `memory.intercept` rows (deny+redirect is a governed verdict); keep the jsonl as operational detail |
| Integrity gate verdicts (`src/integrity_gate.c:247-315`) | returned struct only; caller decides | `ingest.integrity` rows at each wired call site (Part 2 wires the sites) with category + verdict + source class |
| S2 native gate DENY / WOULD-DENY (`src/cmd_hooks.c:180-212`, `src/server/s2_native_gate_hook.c`) | `audit_log("s2-native-gate", …)` | route through `audit_event` like the other block sites |
| Vault writes / capability grants (`src/server/server_vault.c`) | dedicated append-only file | dual-write `vault.write` / `vault.capability` rows (fingerprints only, per its existing redaction) |
| Trigger fires + forge ops (`src/server/trigger_scheduler.c`, `src/server/wfe_live_forge.c:35-47`) | lifecycle events | `trigger.fire` / `forge.op` rows incl. the `forge_allowed()` rail decision — autonomous runs are default-on (`wfe_live_forge_enabled = 1`, `src/config.c:811`); their governed ops belong in the chain first |

Plus the lint guard: mutation entry points in the audited domains must route
through `audit_event`; a bypassing call site fails CI (per done proposal §4).

### A3 — Policy-versioned verdicts ("what was in force")

A row today proves *a* decision happened; it cannot prove *under which policy*.
Add to the verdict row's allowlisted `detail`:

- `policy_rev` — a short hash over the governance-relevant effective config
  (guardrail mode, enforcement-stage dial, integrity gate mode, toolset id, active
  profile), computed once per config load/reload and cached; live-reload bumps it.
- `rule_id` — already exists as the stable `reason_code`; keep.
- `stage` — the `AIMEE_WORKFLOW_ENFORCE_STAGE` dial value where relevant.

With A3, "show me every action allowed while the integrity gate was in shadow" is
a chain query, and a compliance evidence export (time-window → policy revisions →
verdict counts → chain-verify status) is a report, not an investigation. Record
each `policy_rev`'s full flag set once as a `policy.snapshot` row so revisions are
self-describing.

### A4 — The out-of-process anchor pair (the actual trust anchor)

Execute the two carried follow-ups that make the chain trustworthy against a
compromised aimee host, i.e. the literal "enforcer evidence outside the governed
trust domain":

- **Sealer sidecar (R2-5).** A separate minimal process (host cron / systemd unit
  on the CT host) holding `CAP_LINUX_IMMUTABLE`, no write access to the live tail,
  narrow "seal `<path>`" API. The audited service never holds the capability.
- **Signed off-host anchor (R2-8).** ed25519-signed
  `{seq, head_hash, key_id, mac}` pushed per checkpoint to a store the audited
  host cannot silently rewrite. Smallest deployable shape on today's topology:
  **cross-service anchoring** — aimee-server anchors to a write-only append
  endpoint on aimee-kb and vice versa (they are already separate trust domains
  with separate WORM stores); on the combined CT, additionally to the Proxmox
  host (`pvesh`/ssh append-only drop) or the second CT. The anchor client is dumb
  (POST a signed tag); custody protocol per R2-8.

### A5 — `aimee audit attest`

One operator/auditor command that answers the vendor-questionnaire question. Over
a time window it produces a signed attestation bundle:

```
{ window, verify: green|amber|red (+first bad seq),
  newest_checkpoint, anchor: {present, fresh, signature_ok},
  policy_revs: [...], verdict_counts_by_actor_role,
  uncovered_enforcers: [] }   ← from the A2 inventory, so silence is visible
```

signed with the anchor key's public-verify counterpart. Green means: every
governed action in the window is in an intact chain, anchored off-host, with the
policy that judged it identified. That is "attest that enforcement held" in the
strongest form available without TEEs — and stronger than anything surveyed
shipping today.

## Non-goals

- **TEE / code-identity attestation.** A remote-attestation quote proving the
  aimee-server binary itself is a documented future upgrade, not v1.
- **Prevention on a fully compromised host.** The guarantee remains *detection*
  (done proposal §0); A4 shrinks the undetectable window, it does not zero it.
- **Attesting agent *content*** (prompts/outputs). This proposal attests
  enforcement decisions, not model behavior.
- **Un-mediated actors.** Deployment precondition (operator ruling 2026-07-13):
  properly configured clients route LLM traffic through the gateway and install
  the hooks. Direct-LLM-egress agents are out of scope for runtime enforcement;
  making them *visible* is Part 3's fleet registry.

## Risks / honest limits

- **Hot-path cost of default-on WORM**: synchronous fsync per governed action —
  the done proposal's benchmark gate applies before the authoritative flip; A2's
  new sources (gateway, triggers) are lower-frequency than tool calls.
- **Client-side buffering (A2 attention guard)**: a hook subprocess that blocks
  and dies before flushing loses that row — fail-open audit, bounded and
  documented; the server-side enforcers (the authoritative tier) have no such
  window.
- **Anchor availability**: cross-service anchoring fails amber (not closed) when
  the peer is down; `verify` already reports anchor staleness distinctly.
- **`policy_rev` churn**: live config reload bumps revisions; the
  `policy.snapshot` row keeps them cheap (one row per distinct rev, not per
  action).

## Tests

- Each A2 enforcer: trigger its block/allow path → exactly one chained row with
  the right `action`/`verdict`/`actor`; lint guard fails a synthetic bypass.
- A3: config reload changes `policy_rev`; rows before/after carry the right rev;
  `policy.snapshot` round-trips the flag set.
- A4: tamper each class (edit/delete/reorder/truncate/rollback-vs-anchor) →
  `verify` red with the offending seq; sealer sidecar seals without the service
  holding the capability; anchor mismatch after a restore-from-backup is caught.
- A5: attest bundle over a seeded window verifies offline with the public key;
  an uncovered enforcer (inventory entry without rows) is reported, not silent.

# Proposal: Roundtable reliability — get the review gate to 100%

- **State:** diagnosis + first fix (claude-CLI panel exclusion) shipping; follow-ups listed
- **Author:** JBailes (investigation 2026-06-15)
- **Motivation:** the roundtable is the project's quality gate, but in practice it
  is unreliable/slow. This is a root-cause diagnosis (empirically validated
  against the live .254 server) plus the fixes to reach 100%.

## How the roundtable is wired (so the failure modes make sense)

- **Engine roundtable** — `aimee delegate roundtable` / MCP `delegate.roundtable`
  / the authoring pipeline → `handle_delegate_roundtable` → `delegate_roundtable_run`
  (`delegate_ensemble.c`). Fans out over `ensemble.reference_models`; when unset,
  `ensemble_default_panel_from_agents` seats **all enabled agents**. Each panelist
  runs via `agent_run_named` → `agent_execute` — **tools OFF**. (As of #317 each
  review panelist also gets a distinct persona.)
- **Manual per-model review** — `aimee delegate review --persona X --via M`.
  Routes by role; `review` is in `delegate_role_enable_tools_by_default`, so it
  runs **tools ON** unless `--max-turns 1` or the model lacks a tools capability.

## Root causes (empirically validated, 2026-06-15)

Probed all six configured models on a tiny self-contained review:

| model | tools-OFF review | tools-ON review |
|---|---|---|
| mistral | ✅ correct | wanders / 429s |
| minimax | ✅ correct | wanders (read_file/bash), exhausts turns → no output |
| mimo-2.5 | ✅ correct | wanders / context-limited on big prompts |
| glm | ✅ (has `review` role) | tool-capable |
| codex (gpt-5.5) | ✅ correct | no tools cap → excluded by the `caps=tools` gate |
| claude (sonnet) | ❌ **"failed to build request URL"** | n/a |

1. **The manual review path runs tools-ON, and that is the main failure.**
   Tool-capable but weaker models (minimax, mimo) burn all their turns calling
   `read_file`/`bash` instead of reviewing the artifact in front of them, and
   return nothing. **Tools-off, every HTTP model returns a correct review.** A
   *standalone* review (`review the auth module`) legitimately needs tools to go
   read the code, so `review` defaulting to tools-on is not wrong in general — it
   is wrong for an **artifact-provided panel review**. The engine roundtable
   already does the right thing (tools-off); the manual per-model path used *as* a
   roundtable does not.
2. **`caps=tools` excludes tool-less reviewers.** codex/claude advertise no
   `tools` capability, so the role-routed (manual) path with tools required drops
   them — yet **codex reviews perfectly tools-off**. The engine fan-out
   (`agent_run_named`, by name) bypasses this gate, so it is a manual-path issue.
3. **claude-CLI poisons the auto-panel.** `ensemble_default_panel_from_agents`
   seats claude even though it has no HTTP endpoint (it runs on the thin client
   over the reverse channel, primary-only by default). Server-side it fails fast
   with "failed to build request URL", wasting a panel slot. **← fixed here.**
4. **The live .254 server runs an OLD build.** It predates #233 (model-derived
   output caps — the prior 4096 cap starved reasoning models into empty
   artifacts), #314 (detached-workspace bind so delegates can read client files),
   and #317 (per-participant personas). Much of the *observed* degradation is
   deploy-lag, not current-code behavior.
5. **The engine roundtable is slow + opaque.** Multi-round, synchronous, up to a
   10-minute deadline, no streamed progress — a 4-minute probe returned nothing
   and looked hung. A correctness-neutral UX problem, but it is why it "feels
   broken".

## Fixes

### Shipped in this change
- **Exclude claude-CLI from the auto-panel** (`ensemble_default_panel_from_agents`)
  unless `claude_cli_delegate_enabled`, mirroring the manual route's existing
  gate. The engine roundtable's default panel becomes the five HTTP models
  (minimax, mistral, mimo-2.5, codex, glm), all of which return reviews tools-off.

### Required to actually see the fix live
- **Deploy current `testing` to .254.** This is the single biggest lever — it
  brings #233 + #314 + #317 + this fix. (Operator-gated.)

### Follow-ups (separate changes)
- **Canonicalise the engine roundtable as THE multi-model gate** in docs/quickstart
  so users stop hand-running tools-on per-model `aimee delegate review` jobs as a
  roundtable substitute.
- **Streamed/async progress + a saner default deadline** for the engine roundtable
  so it is not a multi-minute silent block.
- **Aggregator robustness:** the synthesis pass defaults to `reference_models[0]`;
  fall back to the next healthy panelist if it fails, rather than degrading to a
  single best candidate.
- **Optional no-tools panel review on the manual path** (e.g. honour a
  `--no-tools` / artifact-provided hint) so a hand-run per-model review of an
  inline diff does not wander.

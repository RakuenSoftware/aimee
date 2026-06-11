# Proposal: Envelope compression, cache-prefix alignment, reversible rehydration, and failure-mined corrections

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-11
- **Charter roles:** Rewrite (envelope compression / cache alignment),
  Recall (rehydration handle), Extract / Gate-Promote (failure-mined
  corrections), Calibrate / Evaluate-Optimize (token + accuracy A/B).
- **Scope:** `src/server/ingress_preinject.c` (envelope assembly + compression
  hook), `src/server/anthropic_http.c` + `src/server/openai_chat.c` (cache-prefix
  placement at the ingress seams), `src/server/server_mcp.c` (rehydration tool),
  config plumbing (`src/headers/config.h`, `src/config.c`, `src/config_fields.c`,
  `src/config_sections.c`, `src/config_save.c`), the curator pass family
  (`src/kb/kb_curator_extract.c`, `src/memory_maintenance.c`) for failure
  mining, `bench/ingress_token_bench.py` + `benchmarks/learning/learning_replay.py`
  for the accuracy/token A/B, unit + integration tests, docs. No new long-lived
  service; the ML prose compressor is explicitly out of scope (see §5).

## Provenance

The four ideas below are lifted — by design, not code — from
[`chopratejas/headroom`](https://github.com/chopratejas/headroom), an
Apache-2.0 context-compression layer for agents (ContentRouter → specialized
compressors → CacheAligner → reversible-compression/CCR, plus a `headroom learn`
failure miner). Headroom *as a product* overlaps Aimee's identity (it sells
cross-agent memory and an ingress proxy, both of which Aimee already is), so this
proposal does **not** adopt headroom. It mines four mechanisms headroom has that
Aimee's injection path does **not**, and reimplements them on Aimee's existing C
seams. None require headroom's Python/Rust runtime or its `Kompress-base` model.

## Relationship to existing proposals

This is a sibling of two pending proposals and must not duplicate them:

- `docs/proposals/pending/recall-economy-progressive-disclosure.md` already owns
  **bounded envelope assembly**, **progressive disclosure (preview + pull-handle)**,
  **learned shortcuts**, and **intent annotation**. That work decides *which
  records, at what size, in what shape* enter the envelope.
- `docs/proposals/done/context-preinjection-ingress.md` shipped the envelope
  itself (`ingress_preinject_build()`, confidence steering, the attention guard).

This proposal is orthogonal: it operates on the *bytes that survive those
decisions*. Recall-economy decides **what** to inject and **how much**; this
proposal makes each surviving record **denser per token**, makes the injection
**cheaper at the provider's KV cache**, makes a compressed record **losslessly
recoverable on demand**, and feeds **session failures back into the records**.
Where the two touch (§3 rehydration vs. recall-economy's pull-handles) is called
out explicitly and unified, not forked.

## Goal

Cut the per-turn token cost *and* the per-turn dollar cost of pre-injection
without losing fidelity, and close the loop so the records we inject get better
when a session goes wrong. Concretely, four levers:

1. **Structural compression of envelope content** — fold JSON tool-output and
   code spans before they enter `<aimee-context>`, so more signal fits under the
   same budget and the attention guard fights less filler.
2. **Cache-prefix alignment at the ingress seams** — place injected context so it
   does not invalidate the provider's KV-cache prefix, recovering cached-input
   pricing on multi-turn sessions.
3. **Reversible compression + a rehydration tool** — keep the uncompressed
   original locally and expose an MCP tool so the model recovers full fidelity
   only when it actually needs it.
4. **Failure-mined corrections** — a curator pass that diffs what an agent did
   against how the session ended and writes a durable correction back to memory.

---

## §1 Structural compression of envelope content

### Problem

`ingress_preinject_build()` packs code snippets and a rendered
`memory.context_block` into the envelope. Recall-economy caps and ranks what
goes in, but the *content of each entry is still verbatim*: a JSON tool result
keeps every key, brace, and whitespace run; a code span keeps every blank line
and comment. Verbatim bytes are exactly what `cli_attention_guard.c` exists to
ration — they consume budget and dilute attention without adding signal.

### Approach

Add a pre-injection **compression hook** in `ingress_preinject.c`, modeled on
headroom's ContentRouter → specialized-compressor split, but limited to the two
**structural** (non-ML) compressors that carry most of headroom's measured win:

- **JSON folder** (headroom's `SmartCrusher`): for tool-output entries that parse
  as JSON, collapse whitespace, elide repeated array shapes to a head sample +
  count, and drop null/empty fields. Reversible (§3 keeps the original).
- **Code folder** (headroom's `CodeCompressor`, AST-aware): for code-span
  entries, strip blank-line runs and optionally comment bodies, and — where the
  symbol-span machinery from `context-preinjection-ingress.md` P2 already gives
  us `line_start`/`line_end` — prefer signatures + the relevant span over whole
  blocks. C-side this can start as a conservative line/brace folder and grow.

The router picks a compressor by the entry's existing content-type tag; prose
falls through uncompressed (the ML prose model is out of scope, §5). Gated by a
new config bool `ingress_compress_enabled` (default **off**), with a per-call
`x-aimee-compress: 0` header escape, mirroring the `ingress_preinject_enabled`
pattern.

### Why this is safe

Structural folding is information-preserving for the consumer (an LLM reading
folded JSON loses nothing it would have used), and §3 makes it *byte-exactly*
reversible. The A/B harness (§5) is the gate: ship the default flip only if
token reduction is real **and** task accuracy holds.

---

## §2 Cache-prefix alignment at the ingress seams

### Problem — the expensive, subtle one

Aimee's ingresses inject context by **mutating the prompt**. The Codex/OpenAI
handlers splice the envelope at the system-prompt seam; the Anthropic
`/v1/messages` path is a deliberate pure passthrough today. Any mutation near the
**front** of the request changes the prompt prefix — and providers key their
**KV cache on a stable prefix**. A per-turn-varying envelope inserted early means
every turn re-pays *full* (uncached) input price instead of the ~10× cheaper
cached-input rate. As a session grows, this is the dominant cost, and it is
invisible in a single-turn token count.

> Before relying on the exact cache-key rules and cached-input pricing for the
> Anthropic ingress, confirm them against the current Claude API reference
> (prompt-caching cache-key semantics, `cache_control` breakpoints, 5-minute /
> 1-hour TTLs, and cached-vs-uncached input rates) — do not hardcode from
> memory.

### Approach

Borrow headroom's **CacheAligner** principle: *stabilize the prefix; put the
volatile, per-turn content where it does not move the cached boundary.*

- **Placement audit.** Determine, per ingress, the latest point in the request at
  which injected context can live while keeping the cacheable prefix byte-stable
  turn-to-turn. For Anthropic, that means using explicit `cache_control`
  breakpoints so the system blocks + stable history cache, and the volatile
  `<aimee-context>` sits *after* the last breakpoint — which also lets the
  Anthropic path inject **without** abandoning its stateless-proxy contract,
  since the proxy still forwards rather than reconstructs.
- **Prefix-stability invariant.** Add a test that asserts the bytes before the
  injection point are identical across two turns of the same session given the
  same upstream history, so a future change can't silently re-break caching.
- **Telemetry.** Surface `cache_read_input_tokens` / `cache_creation_input_tokens`
  (or the provider's equivalent) from upstream responses into the existing A/B
  harness, so the saving is *measured*, not assumed.

### Why it's worth it

This is the only one of the four levers that reduces **dollars without changing a
single injected byte** — it's pure placement. It also de-risks injecting more
aggressively in §1/§3, because the marginal injected token can be a *cached*
token.

---

## §3 Reversible compression + a rehydration tool

### Problem

§1 only pays off if folding is safe, and "safe" means the model can always get
the original back. Today there is no recover-the-original path for injected
content.

### Approach — headroom's CCR, unified with recall-economy's pull-handles

Store the uncompressed original of every compressed entry in a local,
TTL-bounded store keyed by a short handle, and expose recovery as an MCP tool
(headroom's `headroom_retrieve`). **Do not** invent a second handle scheme:
recall-economy already introduces id-addressable pull-handles at the MCP tool
layer for progressive disclosure. This proposal **extends that same handle** with
a `rehydrate` capability (return the byte-exact pre-compression original) rather
than only `fetch full record`. One handle namespace, two verbs:

- `fetch` — recall-economy: preview → full ranked record.
- `rehydrate` — this proposal: compressed/folded form → original bytes.

The store is a bounded in-process LRU (no new service), TTL from a config field
`ingress_rehydrate_ttl_s`. If recall-economy lands first, this is purely
additive to its tool; if this lands first, the handle is designed to accept
recall-economy's `fetch` verb later.

### Payoff

Lets §1 fold **aggressively** (optimize for density, not for "safe to lose"),
because nothing is actually lost — only deferred behind a tool call the model
makes iff it needs the detail. High recall, low resident tokens.

---

## §4 Failure-mined corrections

### Problem

Aimee's curator and `memory_maintenance.c` enrich and maintain memory, and
`benchmarks/learning/learning_replay.py` replays sessions — but nothing
specifically targets **sessions that ended badly** to write a *correction*.
Headroom's `headroom learn` mines failed sessions and writes corrections to
`CLAUDE.md`/`AGENTS.md`. Aimee's equivalent should write to **memory** (its
durable store), not a flat markdown file.

### Approach

A new curator pass (sibling to the existing extract/contradiction passes under
`src/kb/`), driven from the session/attention log the attention guard already
keeps:

1. **Detect** failure signals already observable in-session — destructive op the
   guard blocked, a raw-scan redirect that fired repeatedly, an abandoned/retried
   turn, an explicit user correction.
2. **Diff** the agent's action against the session outcome to phrase a correction
   ("when X, prefer Y, because Z").
3. **Write** it as a durable memory record with an *intent* phrasing (dovetailing
   with recall-economy §Phase 4 intent annotation) so it surfaces on the next
   matching turn via the normal recall blend — closing the loop into §1's
   envelope.

Gated default-off behind `curator_failure_mining_enabled`; promotion of a mined
correction into recall reuses the existing Gate-Promote path, not a new one.

### Why memory, not a file

A flat `CLAUDE.md` correction is per-clone and unranked. A memory record is
cross-agent (every ingress sees it), ranked, deduped, and contradiction-checked
by machinery Aimee already has. This is the one place Aimee's design is strictly
better than headroom's, and the proposal should lean into it.

---

## §5 What is explicitly out of scope

- **The ML prose compressor (`Kompress-base`).** It needs HuggingFace/GPU weight
  and a second model deploy — operationally heavy given the single-embedder
  history (`single-embedder-pivot`). The structural compressors (§1) capture most
  of the win with none of that cost. Revisit only if A/B shows prose is the
  residual.
- **Adopting headroom as a sidecar/proxy.** A per-turn network hop fights Aimee's
  stateless-proxy latency story; reimplementing the two structural compressors in
  C is the cleaner path.
- **Cross-agent memory store / `SharedContext`.** Aimee's is more mature;
  importing headroom's would be a regression.

---

## §6 Validation

This is the gate for every default-on flip — do not flip without it, per the
flag-rollout-readiness bar.

- **Token A/B.** Extend `bench/ingress_token_bench.py` (it already runs
  pre-inject on/off against the Codex ingress) with compression on/off and a
  per-stage breakdown, reporting both *resident* tokens (§1) and *billed/cached*
  tokens (§2).
- **Accuracy A/B.** Reuse `benchmarks/learning/learning_replay.py` to confirm
  task outcomes hold under compression — Aimee's analog of headroom's
  GSM8K/SQuAD/BFCL "accuracy preserved at N% compression" claim. **Verify
  headroom's preservation numbers on Aimee's own corpora; do not trust them
  transitively.**
- **Prefix-stability test** (§2) and **byte-exact rehydration round-trip test**
  (§3) as hard invariants in the unit suite.

## §7 Phasing

- **P1 — §1 structural compression** behind `ingress_compress_enabled`, JSON
  folder first (highest-volume, lowest-risk), then the code folder. Token A/B
  only; no default flip yet.
- **P2 — §3 rehydration handle**, unified with recall-economy's pull-handle if it
  has landed (else handle-designed-to-accept-`fetch`). Unblocks aggressive folding
  + the accuracy A/B → candidate default flip for §1.
- **P3 — §2 cache-prefix alignment**, placement audit + invariant test +
  cached-token telemetry. Highest dollar payoff, touches the live ingress request
  shape, so it ships last and most carefully — re-confirm provider cache
  semantics first.
- **P4 — §4 failure-mined corrections** as a curator pass, default-off, promoted
  through the existing Gate-Promote path.

## §8 Risks

- **§2 changes the live request shape.** A wrong placement could break an ingress.
  Mitigated by the stateless-proxy-preserving design (inject after the last cache
  breakpoint), the prefix-stability test, and shipping it last.
- **Compression that loses signal** would degrade answers invisibly. Mitigated by
  reversibility (§3) + the accuracy A/B gate (§6); never flip a default on token
  count alone.
- **Provider cache semantics drift.** §2 depends on current Anthropic/OpenAI
  caching rules — re-verify against the live API reference before implementing,
  not from memory.

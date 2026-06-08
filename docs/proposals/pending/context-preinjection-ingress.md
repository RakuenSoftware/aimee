# Proposal: Context pre-injection and confidence-gated retrieval for the model ingresses

- **State:** draft — pending review
- **Author:** JBailes
- **Date:** 2026-06-08
- **Scope:** `src/server/openai_chat.c`, `src/server/openai_shape.c`
  (Codex/OpenAI ingress — already runs `agent_execute`, has a seam),
  `src/client_integrations.c` (hook + MCP install for Claude Code & Codex —
  already exists, extended), `src/memory_graph_fusion.c` (recall → confidence),
  `src/server/server_mcp.c` (`find_symbol` / `get_context_block` handlers),
  `src/kb/kb_service_code_embed.c` + `src/headers/index.h` (surface symbol
  spans), `src/server/token_tracker.c`, new Claude Code hooks
  (UserPromptSubmit/PreCompact/PreToolUse), a token A/B bench harness, unit +
  integration tests, docs. No new long-lived service.
  **The Anthropic `/v1/messages` proxy stays untouched on the wire** — see §A.

## Goal

Make Aimee's existing fusion recall and code graph actually **reduce the tokens
an external agent spends**, by turning the model ingresses (Codex
`/v1/responses`, Claude Code / Anthropic `/v1/messages`, OpenAI `/v1/chat`) from
transparent proxies into *context-aware* proxies, and by making **Aimee the
single context surface** so that even when the agent needs more than the
pre-injected context, it explores *through Aimee* rather than re-grepping the
raw filesystem:

1. **Pre-inject** the files/symbols Aimee already knows are relevant for the
   turn, so the agent stops re-exploring the repo on every prompt.
2. Attach a **confidence score and exploration caps** that steer the agent to
   Aimee's graph-aware exploration tools (below) instead of raw `grep`/`read`.
3. Use **symbol-scoped reads** so a single function costs a function, not a
   whole file — the code index already stores symbol spans
   (`kb/kb_service_code_embed.c`), but they are **not yet surfaced** through
   `find_symbol` (which today returns only `{file, line, kind}`), so this is
   real wiring, not free.

Today Aimee has the expensive half built and **already exposes a broad
exploration surface** — see §0. Graph-code fusion is permanently on
(`memory_graph_fusion.c`, #2721), the code index stores symbol spans, there is a
working `get_context_block` pre-injection primitive
(`server_mcp.c:491` → `kb_client_memory_context_block`), and three ingresses
work end-to-end. What's missing is the cheap, high-leverage glue: *putting that
retrieval in front of the agent before it spends tokens, and keeping every
subsequent lookup inside Aimee.* The thesis is that token cost should **compound
downward** across a session — each turn the agent is told what is relevant, and
any gap is filled by a cheap, graph-aware Aimee query rather than a raw
repository scan.

This is the retrieval-side sibling of the Codex and Anthropic ingress work
(`docs/proposals/pending/codex-frontend-ingress.md`,
`claude-code-anthropic-ingress.md`): those made Aimee *speak the wire*; this
makes Aimee *earn its keep* on that wire.

## §0 Principle: Aimee is the single context surface

The strongest version of this proposal is not "pre-inject and hope the agent
greps less." It is: **the agent never needs to touch the raw filesystem to
understand the code, because Aimee already answers every exploration question
better.** Aimee today exposes (as first-class MCP tools, verified against the
golden surface net #2733):

- `find_symbol` — locate a symbol (today: file + line + kind; §C surfaces the
  span so it reads *only* that function).
- `lsp_definition` / `lsp_references` / `lsp_diagnostics` — precise
  go-to-def / find-refs / errors, no guessing.
- `ast_grep_search` — structural (AST) search, not line-grep.
- `search_graph`, `get_entity`, `get_entity_edges` — graph traversal /
  neighbors / impact.
- `get_context_block`, `session_context_search`, `session_context_expand` —
  packed, relevant context on demand.
- `preview_blast_radius` — what a change touches, before it's made.
- `search_memory` / `memory_recall` / `search_docs` — durable "how/why".

So the design has **two reachability paths** and they converge on the same
surface:

- **MCP path (Aimee-as-MCP-server).** Claude Code and Codex **already have Aimee
  registered as an MCP server** (`client_integrations.c` writes
  `mcpServers.aimee` / `.mcp.json` today), so the agent already *can* call all of
  the above. The work here is purely (a) pre-inject the starting context via
  `get_context_block`, and (b) ship a policy/preamble — extending the existing
  Codex `defaultPrompt` and Claude slash-commands — that tells the agent to
  explore with these tools instead of raw grep/read.
- **Wire-ingress path (Codex `/v1/responses`, Claude `/v1/messages`).** Through
  the wire the external agent runs *its own* tool loop in *its own* sandbox.
  Because the MCP server is co-registered alongside the ingress (above), its
  fallback exploration *can* route through Aimee — the lever is to pre-inject
  heavily (most turns then need no exploration) and let policy steer the rest to
  the co-registered tools.

The practical consequence: the "supplementary greps" cap in earlier drafts is
recast. We are not budgeting raw greps — we are **redirecting** exploration into
Aimee's already-registered tools and only falling back to raw scanning as a
last resort the policy actively discourages. **No new co-registration work is
required** — it ships today.

## Motivation

The ingresses are currently stateless wire-format proxies. When Codex or Claude
Code asks Aimee a question, Aimee runs the turn on its primary model and streams
back — but the external agent still does its own file exploration (grep, read,
list) because nothing in the response tells it "you don't need to; here are the
three files that matter." That exploration is pure token waste, and it repeats
every turn because the external agent has no durable picture of the repo.

Aimee already computes that picture. Fusion recall + the code graph can answer
"which files/symbols are relevant to this prompt" cheaply and locally. We are
paying to build the map and then not handing it to the traveler.

## Current Aimee baseline (verified against the code)

An audit of the relevant paths, so the delta is honest:

| Capability | Status today | Gap this proposal fills |
|---|---|---|
| `get_context_block` pre-injection primitive | **Exists** — `server_mcp.c:491`, RPC `memory.context_block`, returns a packed text block | Call it *automatically* at turn start; today nothing invokes it |
| Fusion recall scoring | **Exists** — `memory_graph_expand_from_seeds()` returns `graph_score` (double) + `hops` per hit | No `confidence` enum; derive high/med/low by discretizing `graph_score` |
| MCP co-registration with external agents | **Already done** — `client_integrations.c` registers `mcpServers.aimee` (→ `aimee mcp-serve`) for **both** Claude Code and Codex (`.mcp.json`) | Nothing to add; rely on it for the "explore-through-Aimee" fallback |
| Client-side guidance seam | **Exists** — Codex plugin `defaultPrompt` ("Search aimee memory before answering…"), Claude slash-commands | Extend with the explore-with policy; no new mechanism |
| Hook install in setup | **Exists** — `client_integrations.c:862` installs **PostToolUse** hooks (CWD tracking) | Add UserPromptSubmit (pre-inject), PreCompact (re-prime), PreToolUse (guard) to the same installer |
| `find_symbol` symbol span | **Partial** — returns `{file, line, kind}` (`index.h` `term_hit_t`), **no end-line or body** | Surface `line_start`/`line_end` + span text (the code index has the spans) |
| Anthropic `/v1/messages` ingress | **Pure stateless proxy by design** — `anthropic_http.c:1` "does NOT run memory… those would corrupt the context Claude Code builds" | Do **not** mutate this wire; pre-inject for Claude Code via a hook instead (§A) |
| Codex/OpenAI `/v1/*` ingress | Runs `agent_execute` / `agent_execute_messages` — **not** a pure proxy; has a system-prompt seam | Inject the envelope at that seam |
| Per-file session attention (recency-weighted) | **Does not exist** — `session_context_*` tracks tool *chains*, not per-file read/edit recency | New lightweight attention log (§E) |

The two load-bearing corrections versus a naive plan: (1) the Anthropic proxy is
*deliberately* stateless, so Claude Code pre-injection must go through a **hook**,
not the wire; (2) MCP co-registration is **already shipped**, so the
"explore-through-Aimee" fallback needs no new plumbing — only policy that points
the agent at it.

## Design

### A. Pre-injection envelope (core)

The envelope is the same shape everywhere; *where it is emitted differs by
client*, because the Anthropic proxy is intentionally pure (see baseline). The
serializer is shared:

```
<aimee-context confidence="high|medium|low">
recommended:
  - src/server/openai_chat.c::handle_responses_turn   # why: matches "responses turn"
  - src/server/openai_shape.c
session-touched:
  - src/server/server_http.c   (edited this session)
explore-with:                  # when more is needed, use these — not raw grep:
  - find_symbol, lsp_references, ast_grep_search, search_graph, get_context_block
</aimee-context>
```

- **Builder.** A new `fusion_recall_context_block()` near `memory_graph_fusion.c`
  that seeds fusion recall from the turn text and packs the result by reusing the
  existing `get_context_block` path (`kb_client_memory_context_block`), returning
  a struct. This is an extension of an existing primitive, not a new retrieval
  path. Confidence is derived by discretizing the `graph_score` the recall
  already returns (no new scoring).

Three emission seams, by client — **the Anthropic `/v1/messages` wire is not
touched**:

1. **Claude Code → SessionStart / UserPromptSubmit hook.** Because the Anthropic
   ingress is a deliberately stateless proxy (injecting on the wire "would
   corrupt the context Claude Code builds", `anthropic_http.c:1`), pre-injection
   for Claude Code is delivered as a hook that emits the envelope into Claude
   Code's own context at turn start. The hook calls Aimee for the block. This
   respects the existing design and is installed by the same setup that already
   writes PostToolUse hooks (§D).
2. **Codex / OpenAI `/v1/*` → server-side seam.** These handlers already run
   `agent_execute` / `agent_execute_messages` and parse a system prompt from
   `instructions` (`openai_chat.c`, `openai_shape.c`), so the envelope is
   injected server-side as a system/developer preamble at that existing seam.
3. **Codex plugin `defaultPrompt`** carries the standing explore-with policy
   (extends the existing prompt, no new mechanism).

- **Opt-in, default off** behind config `ingress_preinject_enabled`, plus a
  per-request override header (`x-aimee-preinject: 0`) on the Codex/OpenAI path
  and an env toggle on the hook path, so it can be A/B'd live (see §F) without a
  redeploy.

### B. Confidence + exploration steering (not raw-grep budgets)

`fusion_recall_context_block()` returns `confidence` (derived from top-k score
spread / margin already available in the recall result). Confidence sets how
hard the preamble steers:

- `high` → "the recommended set is sufficient; do not explore further."
- `medium` → "if you need more, call `find_symbol` / `lsp_references` /
  `search_graph` (Aimee's tools); do **not** raw-grep the tree."
- `low` → "explore via Aimee's tools first (`ast_grep_search`, `search_graph`),
  starting from the recommended set."

The cap that matters is therefore a **last-resort raw-scan cap**, not a tool
budget: Aimee tool calls are cheap and graph-aware and are *encouraged*; the
only thing we limit is the fallback to raw `grep`/recursive `read` against the
filesystem (config `ingress_max_raw_scans`, default 0 when Aimee MCP is
co-registered, small otherwise). These are *advice* over the wire (we can't
force a third-party agent), but for **Claude Code specifically** the PreToolUse
hook (§D) can enforce the last-resort cap locally — e.g. nudge a raw `grep`
toward `ast_grep_search`/`find_symbol`.

### C. Symbol-scoped reads (real wiring — spans exist, aren't surfaced)

The code index *stores* symbol spans (`kb/kb_service_code_embed.c`), but
`find_symbol` returns only `{file, line, kind}` (`index.h` `term_hit_t`) — no
end-line, no body. So symbol-granular reading is **not** available today and is
genuine, if small, work:

- Add `line_start` / `line_end` to the symbol lookup result and have
  `find_symbol` (or a sibling read) return the span text for `path::symbol`,
  falling back to whole-file when the symbol is unknown. This changes the
  `find_symbol` tool signature → **regen `test_mcp_tools_golden.inc`** via
  `DUMP_TOOLS=1` (surface net #2733).
- Emit `recommended` entries in `file::symbol` form whenever the recall hit is
  symbol-granular, so a pre-injected reference points at the function, not the
  file, and the agent's follow-up read costs a function.

### D. Claude Code hook bundle (extends the existing installer)

`claude-proxy enable` already wires Aimee as Claude Code's Anthropic backend
**and already installs PostToolUse hooks + the `mcpServers.aimee` registration**
(`client_integrations.c:862`, `:797`). This adds three more hooks to the same
installer — the seam already exists:

0. **SessionStart / UserPromptSubmit pre-inject** — emits the §A envelope into
   Claude Code's context at turn start (the Claude-Code delivery path for §A,
   since the wire is left pure).
1. **PreCompact re-prime** — on Claude Code's `PreCompact` event, re-emit the
   current session's context block so the right files survive the compaction
   that the agent is about to do. (This is the original point of the ingress
   being *compact-aware*: don't lose the map when the window is squeezed.)
2. **Attention-weighted destructive guard (PreToolUse)** — intercept
   `Write`/`Edit`/`Bash`/`NotebookEdit`. Score the target file's session
   attention with recency decay:

   ```
   score = Σ kind_weight × 2^(-age_hours)
   kind_weight: register_edit=8, edit_observation=5, read=2, cache_hit=1
   ```

   - `rm -rf` / truncate / overwrite of a high-attention file → exit 2 (hard
     block, shown to user).
   - Any other destructive op on a high-attention file → exit 1 (warn, confirm).
   - Otherwise → exit 0 (allow silently).
   - Bypass via env (`AIMEE_GUARD=0`).

   The same PreToolUse hook also nudges raw exploration toward Aimee: a bare
   `grep -r` / recursive read past `ingress_max_raw_scans` is intercepted with a
   message pointing at `ast_grep_search` / `find_symbol` / `search_graph`.

   Attention comes from a lightweight per-session action log (see §E). This
   complements — does not replace — the existing git-write guard and the
   `aimee-blast-radius` skill; it's *recency/attention*-weighted and op-level.

These hooks are install-time artifacts under the user's Claude Code config, not
new server code paths — low blast radius, easy to gate behind `claude-proxy`.

### E. Session attention log

Net-new: today `session_context_*` tracks tool *chains* (tools used, stub bytes),
**not** per-file read/edit recency, so there is no existing signal to reuse. This
adds a compact, decaying per-file action log feeding §B ranking and §D scoring:
reads, cache hits, edit observations, registered edits, each timestamped. Lives
alongside the existing session/presence state (server-side) and is exposed to
the Claude Code hooks via a small read endpoint. Recency-weighted; pruned. This
is deliberately *not* the durable episode store (deep-curator already owns
long-term "how"); it's the live "what did this session just touch" signal. The
PostToolUse hook already installed (§D) is the natural feed for edit events.

### F. Token-savings A/B harness (proof)

A bench harness that runs a fixed prompt suite twice against Aimee's **Codex
ingress** — `ingress_preinject_enabled` on vs off (via the `x-aimee-preinject`
header) — and captures real token usage from `codex exec --json`
(`turn.completed.usage` = input + cached_input + output). Reports per-prompt and
aggregate reduction. This gives us a hard, reproducible number for "fusion +
pre-injection actually saves N% of tokens through the ingress," which we cannot
claim today. Reuses `token_tracker.c` accounting for the Aimee-internal side.

### G. Code-health audit (follow-on, lower priority)

A graph-derived `aimee code audit` over the existing code index: dead exports
(no inbound edges), untested files (stem match), circular deps (DFS), clone
detection (group symbols by body hash, span ≥ N lines), TODO/FIXME orphans. Emit
a debt score + a short `AUDIT_CONTEXT` that the ingress can pre-inject for a
bounded window so the agent fixes known debt without re-prompting. Listed for
completeness; can split to its own proposal if §A–F land first.

## Phasing

- **P1 — Pre-injection envelope + confidence steering (§A, §B).** The core
  lever: builder over fusion recall + `get_context_block`; confidence from
  `graph_score`; Codex/OpenAI server-side seam **and** the Claude Code
  SessionStart/UserPromptSubmit hook; explore-with policy on the existing Codex
  `defaultPrompt`. MCP co-registration is **already shipped** — no work. Off by
  default, header/env-overridable. Ship with the A/B harness (§F) so the first
  thing we do is measure it.
- **P2 — Symbol-span reads (§C).** Surface `line_start`/`line_end` + span text
  through `find_symbol`; emit `file::symbol` in the envelope. Small but real
  (the span isn't exposed today); regen the tools golden.
- **P3 — Claude Code hooks (§D) + session attention log (§E).** PreCompact
  re-prime + the destructive guard, added to the existing hook installer; the
  net-new per-file attention log.
- **P4 — Code-health audit (§G).** Optional; may spin out.

## Files touched

| File | Change |
|---|---|
| `src/memory_graph_fusion.c` | `fusion_recall_context_block()` builder + `graph_score`→confidence discretization |
| `src/server/openai_chat.c`, `openai_shape.c` | inject envelope at the existing system-prompt seam (Codex `/v1/responses`, OpenAI `/v1/chat`) |
| `src/server/anthropic_ingress.c`, `anthropic_http.c` | **unchanged** — wire stays a pure stateless proxy (Claude Code gets the envelope via a hook) |
| `src/server/server_mcp.c` | extend `find_symbol` to return `line_start`/`line_end` + span text (regen golden) |
| `src/kb/kb_service_code_embed.c`, `src/headers/index.h` | surface stored symbol spans through the lookup result (`term_hit_t`) |
| `src/client_integrations.c` | add SessionStart/UserPromptSubmit + PreCompact + PreToolUse hooks to the existing installer; extend Codex `defaultPrompt` policy (MCP reg already present) |
| `src/server/token_tracker.c` | A/B accounting hook |
| new: hook scripts | pre-inject + PreCompact re-prime + PreToolUse attention guard (+ raw-grep → Aimee-tool nudge) |
| new: `bench/ingress_token_bench.py` | Codex on/off A/B harness |
| config | `ingress_preinject_enabled`, `ingress_max_raw_scans` |

## Testing

- Unit: envelope serializer (confidence-tier steering text, `explore-with`
  tool list, `file::symbol` emission, empty-recall → no block); `find_symbol` /
  `get_context_block` span fidelity (known/unknown symbol, fallback); attention
  scoring (decay, kind weights, thresholds); guard exit-code matrix incl. the
  raw-grep → Aimee-tool nudge.
- Integration: Codex/OpenAI server-side seam — envelope present when enabled,
  absent when header-disabled; assert `test_anthropic_ingress.c` stays a pure
  proxy (envelope **not** on the `/v1/messages` wire); Claude Code hook emits a
  well-formed block.
- Surface: regen `test_mcp_tools_golden.inc` (`DUMP_TOOLS=1`) for the extended
  `find_symbol` (P2).
- Proof: run `bench/ingress_token_bench.py` against the Codex ingress; record
  the on/off delta in the proposal before promotion.
- `aimee git verify` (full `-Werror` build — catches `.inc` callers, cf.
  fusion-wiring lesson) + full CI incl. e2e-docker (flaky on HF download → rerun).

## Risks / non-goals

- **We can't force a third-party agent** to obey the steering; the envelope is
  advisory. Enforcement only exists for Claude Code via the local hook. Aimee's
  MCP server is already co-registered with both Claude Code and Codex, so the
  "explore-through-Aimee" fallback is reachable by default — but whether the
  agent *chooses* it over its own grep is still advisory. Even so, advisory
  pre-injection removes most re-exploration, and the A/B harness quantifies it.
- **Pre-injection seam differs by client** (server-side for Codex/OpenAI, hook
  for Claude Code) because the Anthropic proxy is deliberately stateless. This
  is a feature, not a workaround — it keeps the wire contract clean — but it
  does mean two code paths emit the same envelope, so the **serializer must be
  shared** and tested once.
- **Stale recommendations.** If recall is wrong, pre-injection wastes a little
  context. Mitigated by confidence gating (low confidence → smaller/no block)
  and by keeping the block compact.
- **Not** Aimee running its own internal tool loop — the ingresses stay
  stateless wire-format proxies onto the primary model; this only enriches the
  request preamble and the read surface.
- **Default off.** Nothing changes for existing ingress users until
  `ingress_preinject_enabled` is set, so promotion is low-risk.

## Open questions

- Envelope placement: for Codex/OpenAI, developer- vs system-role at the
  `instructions` seam; for Claude Code, SessionStart vs UserPromptSubmit hook
  output (per-turn freshness vs once-per-session cost). Confirm which the agents
  weight most without it being treated as user content.
- Where the session attention log physically lives (presence registry vs a
  dedicated small store) given the Claude Code hook needs read access from
  outside the server process.
- Whether the Claude Code SessionStart/UserPromptSubmit hook should call the
  running server (one source of truth, needs server up) or compute the block
  locally via `aimee mcp-serve` (works offline, may diverge).

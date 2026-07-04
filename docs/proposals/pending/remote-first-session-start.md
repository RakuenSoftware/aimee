# Proposal: Remote-first session-start — deliver the full brief over /v1

## Thesis

Aimee runs as a **thin client against a remote aimee-server**, and that is the *only* topology we
support — **there is never a co-located server**. But SessionStart was built co-located-first:
`handle_session_start` assembles the rich brief (`build_session_context`: persona principles,
learned Rules, skills index, local convention discovery, key facts) only when it finds a *local*
server socket, and silently falls back to a **recall-only** remote path
(`handle_session_start_remote` → `POST /v1/memory/recall`) otherwise. Since the local socket never
exists in our deployment, the degraded recall-only path is the *only* one that ever runs — and when
it has nothing to return, the hook emits **nothing at all** (observed 2026-07-04: a fresh session
got no persona, no rules, no identity, no preferences).

"Requires a co-located server" is therefore not a caveat — it is a bug. This proposal makes the
**remote path first-class** and delivers the **full** brief over `/v1`, so a thin client gets the
same brief a co-located server would have assembled.

*(Companion proposal: `memory-db1-db2-architecture.md` fixes what the brief is built **from** —
this proposal fixes **delivery**; they compose but ship independently.)*

## Goal

A remote thin client receives a complete SessionStart brief — persona + learned Rules + curated
memory + local conventions — with no co-located dependency and no silent-empty output.

## §0 What already exists

- **`build_session_context()`** (`src/cmd_session_lifecycle.c:190`) assembles the full brief today.
- **`/v1/hooks/session_start`** already routes to it (`server_http_routes.c:1974` →
  `hooks.session_start` → `session_start_emit` → `build_session_context`). **The full brief is
  already remotely reachable — the thin client just doesn't call it.**
- **`handle_session_start_remote()`** (`src/cli_session_start.c:80`) — the recall-only path, now
  hardened (bounded retry + the NULL-evloop server crash fix, PR #1032).

The plumbing is present; the client points at the wrong endpoint.

## §1 Remote path fetches the full server-assembled brief

Change `handle_session_start_remote` to call `POST /v1/hooks/session_start` (which runs
`build_session_context` server-side) and emit its output as the hook `additionalContext`, instead
of rendering only `/v1/memory/recall`. Curated-memory recall becomes one *section* of that brief,
not the whole thing. Reorder `handle_session_start` so the remote endpoint is the primary path, not
a fallback after a missing local socket.

## §2 Workspace-dependent brief when the server has no client files

`build_session_context` is **workspace-coupled**: the skills index and `context_discover` read
files under the client's cwd (`.aimee-rules`, `AGENTS.md`, `CONTRIBUTING.md`), which do **not**
exist on a remote server. Split the brief:

- **Server assembles the workspace-independent half:** persona principles, learned Rules, skills
  **catalog**, curated memory, key facts.
- **Client assembles the workspace-dependent half:** the thin client already has the repo, so it
  runs local `.aimee-rules`/`AGENTS.md`/convention discovery itself and concatenates.

*(Roundtable alternatives: A1c — client greps its conventions and ships the text in the hook
payload for the server to merge; A1b — reuse the `launch.run` reverse channel so server-side
`context_discover` reads the client tree. A1a/client-side is recommended: cleanest, no server file
access, no channel coupling for a lightweight hook.)*

## §3 Never silently empty

Keep soft-fail (the hook must never block the prompt), but **always emit at least the
workspace-independent brief** (persona + Rules + Aimee hints) — static server config that is
effectively always available. An empty brief must be impossible unless the server is truly
unreachable after the bounded retries.

## §4 Make the topology assumption explicit

Remove/relabel the "server unavailable → co-located" branch and the docstrings implying a local
server is the norm. Document remote-thin-client as *the* deployment. Add a CI/doc guard that the
SessionStart path has no route that only works co-located.

## Phasing

1. **§1 + §3** — remote path fetches the full workspace-independent brief; never silently empty.
   Immediate, visible win; small and self-contained.
2. **§2** — the workspace-dependent (client-side) half.
3. **§4** — topology cleanup + CI guard.

## Non-goals

- Standing up a co-located server (explicitly out — the topology is remote-only).
- Changing *what* memory feeds the brief — see the companion db1/db2 proposal.
- Token-budget / retrieval-ranking changes to recall.

## Risks / honest limits

- **Brief size / cost:** the full brief is larger than recall-only; keep it within the token
  budget the recall path already respects (`limit_tokens`).
- **Workspace split (§2):** client-side discovery is only as good as what the thin client can read
  locally; a client with no repo checkout gets the server half only (acceptable — never empty).
- **Latency:** `build_session_context` does more work than recall; it already runs behind the
  bounded-retry + soft-fail envelope so a slow assembly never blocks the prompt.

## Tests

- Remote SessionStart with **no local socket** returns a non-empty full brief (persona + rules) —
  the direct regression for "requires co-located server".
- The emitted brief includes persona principles + learned Rules + a recall section end-to-end.
- Client-side convention discovery (§2) merges `.aimee-rules`/`AGENTS.md` into the brief.
- Soft-fail: an unreachable server after retries yields exit 0 and no prompt block.

## Open questions for the roundtable

1. Workspace-dependent brief: A1a (client-side) vs A1c (payload bundle) vs A1b (reverse-channel)?
2. Should the co-located path be **removed** entirely, or kept as a dev-only convenience behind an
   explicit flag?

## Review revisions (R1)

From the design roundtable (7 participants, 58 findings, reviewed jointly with the db1/db2
proposal). Folded in:

- **Deployment model made explicit** — the brief is assembled on the user's **own** aimee-server
  (1:1 per user), which reads its per-user db1 + the shared kb/db2. `build_session_context` already
  runs there; there is no thin-client-side db and no cross-instance access. This is the same
  invariant §0 of the db1/db2 proposal establishes.
- **Cross-proposal phasing clarified** (blocking, 3×) — P1's "never silently empty" guarantee rests
  **only on persona + learned Rules** (server config, no memory seeding required); the
  memory-populated sections (identity/preferences/context) light up only once the db1/db2 proposal
  seeds them. P1 Phase 1 therefore ships a non-empty brief *without* depending on P2, but the
  *fully-populated* brief requires both. Stated as an explicit dependency, not "ships independently
  with equal value."
- **`/v1/hooks/session_start` response contract pinned** (finding 30) — the thin client now depends
  on this endpoint for prompt-critical startup, so §1 must define its response schema, a version
  field, and forward/backward-compatible behavior (old client ↔ new server and vice-versa).
- **Full-brief token budget bounded** (finding 6) — the recall-only `limit_tokens` does not bound the
  larger full assembly; §1 adds an explicit budget for the whole brief so it can't blow the Claude
  Code `additionalContext` size.
- **Trust boundary for repo-file conventions** (findings 27/49) — `.aimee-rules`/`AGENTS.md`/
  `CONTRIBUTING.md` text merged in §2 is **untrusted repo data, not agent instructions**, and is
  marked at a lower trust tier so a malicious/compromised repo file cannot inject agent-shaping
  instructions at full primacy. (Aligned with the provenance boundary in the db1/db2 proposal.)
- **"Never empty" failure modes made precise** (findings 5/33/58) — §3 enumerates the reachable-but-
  contentful cases: server reachable but no memory (→ still emit persona+rules), unseeded persona
  (→ emit a minimal built-in default, never nothing), auth failure and partial-assembly failure
  (→ emit whatever assembled + a diagnostic marker). Truly-empty only on unreachable-after-retries.
- **Merge format specified** (finding 34) — server-half ⊕ client-half concatenation defines section
  ordering, de-dup, and precedence so the two halves can't produce duplicate/contradictory/unordered
  instructions.

Deferred / open: workspace-dependent approach A1a vs A1c vs A1b (OQ1); whether to keep a dev-only
co-located path (OQ2).

## Review revisions (R2)

Second-pass roundtable confirmed the prior blocking themes are closed and flagged refinements to the
R1 additions. Folded in:

- **Single precedence lattice, shared with the db1/db2 proposal** — the repo-file "untrusted data"
  tier from R1 is the **bottom** of the one unified lattice defined in the memory proposal's R2
  (hard org rules ▸ operator user captures ▸ soft org defaults ▸ untrusted repo-file/auto-extracted).
  The server⊕client merge resolves all cross-tier conflicts by that single order — no separate P1
  trust model.
- **Merge dedup key specified** — server-half ⊕ client-half de-dups on `(normalized-key, kind)`;
  on collision the higher lattice tier wins (e.g. a captured rule beats a duplicate repo-file
  convention; a server skill beats a client-discovered duplicate). Removes the "duplicate/
  contradictory instructions" ambiguity.
- **Fallback persona text made explicit** (§3) — the never-empty default for an unseeded server is a
  **fixed, deliberately-neutral built-in** (identity/role only, no behavioral norms), so the fallback
  can't silently shape the agent. Its exact text is committed in the proposal, not left open.
- **`/v1/hooks/session_start` response schema drafted** (§1) — a versioned envelope
  `{ schema_version, sections: [{ id, title, trust_tier, body }], truncated }` so the thin client can
  render/merge deterministically and P2's merged db1+db2 recall bundle conforms to the same contract.
  Forward/back-compat: unknown `section.id`s render as-is; missing sections are skipped.

Cross-proposal note: §1's response schema and the db1/db2 proposal's merged recall bundle are **one
shared contract** — they must land together (tracked as a joint integration point, not two
independently-drafted schemas).

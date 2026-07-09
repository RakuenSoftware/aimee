# Proposal: Hashline edit core + a token-lean websearch

- **State:** proposed (pending — not started)
- **Charter roles:** Rewrite / Extract / Rank-Fuse / Enforce / Gate-Promote
- **Prompted by:** ["The Harness Problem"](https://blog.can.ac/2026/02/12/the-harness-problem/) (can.ac, 2026-02-12)

## Roundtable disposition (rev. 4)

Three multi-provider roundtable rounds, all **"ship with changes"**; the core
thesis — *stable server-side identifiers + token-lean tools matter as much as
model quality* — was endorsed throughout, and each round's findings narrowed from
fundamental design → mechanism gaps → concurrency/TOCTOU edge cases.

**Round 1** raised four blocking footguns (identical-line anchor collisions,
multi-edit anchor churn, extractive `web_read` hiding literal needles, and
`edit_symbol` identifier ambiguity). Rev. 2 folded all four into the spec below.

**Round 3** confirmed rev. 3 resolved every round-2 issue, and raised two
final blockers with standard mitigations, which **rev. 4** folds in:
- **Stale-read race (blocking, correctness):** digests keyed by a mutable
  `(path, ordinal)` map let a second read clobber a first read's digests. Fixed
  with **immutable snapshot binding** — each read mints a `snapshot_id`; edits
  carry it; verification runs against that snapshot. (§Part I.1)
- **DNS rebinding (blocking, security):** "resolve-then-check" is TOCTOU. Fixed by
  **pinning the connection to the validated IP** (per redirect hop), not
  re-resolving. (§Part II)
- Plus the round-3 spec gaps: line-ending **tie-break defined** (majority; tie →
  LF), the **re-anchor error response given a concrete schema**, and
  **interacting-but-non-overlapping batch ops checked at planning time**.

**Round 2** accepted the design intent of all four round-1 fixes and raised
second-order issues, which **rev. 3** resolved:
- **Hash strength (was blocking):** a 2–3 hex tag is too short to *verify*
  freshness (1/256–1/4096 collision on a changed line → silent corruption). Fixed
  by making the short tag **display-only** and verifying against a **full-length
  server-side digest**. (§Part I.1, §I.4, Risks)
- **SSRF on `web_read`'s raw-URL path (was blocking):** server-side fetch of an
  arbitrary URL. Fixed with a **strict egress policy** — http/https only,
  resolved-IP deny-list, per-hop redirect re-validation. (§Part II)
- **Write-back line-ending integrity (high):** unchanged lines keep original bytes
  verbatim; the server normalizes only edited regions to the file's dominant
  terminator. (§Part I.1)
- **Literal-leg budget displacement (medium):** the literal leg gets a **reserved
  budget slice** and reports omissions with a `mode:"literal"` retrieval path.
  (§Part II)
- **`edit_symbol` single-but-wrong resolve (medium):** unique-but-incorrect
  resolution (parent vs. override) handled by **echoing the resolved signature +
  index version and requiring FQN or confirmation**. (§Part III #4)

The rollout is re-sequenced so the transactional edit core is proven before any
semantic tool ships:

1. **Anchor collisions on identical lines** → the anchor is a **composite**
   (line ordinal *and* content hash); the ordinal disambiguates identical
   content, the hash verifies freshness. Ambiguous re-anchors return context
   rows. (§Part I.2, Risks)
2. **Anchor churn in multi-edit batches** → promoted from a Risk to a
   first-class **transactional batch** spec: snapshot-and-rebase, server
   resolves offsets against the as-read snapshot, applied bottom-first, per-op
   failure isolation. (§Part I "Transactional batches")
3. **Extractive `web_read` hiding literal needles** (API names, error strings,
   version numbers) → a **mandatory exact-substring pass fused ahead of**
   semantic ranking, plus a **full-page escape hatch** (`mode:"full"`, spilled
   by ref). (§Part II)
4. **`edit_symbol` ambiguity** on overloaded/shadowed identifiers → require a
   **fully-qualified name or a mandatory disambiguation protocol** that returns
   candidate signatures as anchored outline rows; never a blind resolve. (§Part
   III #4)

Accepted suggestions also folded in: anchor **canonicalization** rules
(line-endings/whitespace/BOM), `web_read` spans **marked untrusted** for
injection safety, `run_tests` keeps **pass counts + capped spill**, and the
Evaluation now **quantifies the net token delta** (hash overhead vs. span
savings) as a ship gate rather than a footnote.

## Thesis

The linked post makes one argument worth acting on: for a coding agent, the
**harness** — the edit tool, its schema, its error handling — determines as much
of the observed quality as the model does, and every mainstream edit tool is
built on a fragile foundation. `str_replace` / `apply_patch` / whole-file rewrite
all force the model to *reproduce content it already read* (exact whitespace,
unique surrounding context) with **no stable, verifiable identifier** for the
lines it wants to change. The failure modes — "old_string not found", "occurs N
times", silent stale-file corruption — are structural, not model bugs. The post's
fix ("hashline") gives every read line a short content-hash tag the model edits
*by reference*; across 16 models / 180 tasks the weakest models gained ~10× in
edit success and strong models shed ~20% output tokens (fewer retry loops).

**Aimee's `edit_file` is a textbook `str_replace`** (`old_string` / `new_string`
/ `replace_all`, `posix/agent_tools_dispatch.c:223`), with exactly those error
strings (`:273`, `:280`). And **aimee is the harness most exposed to this class
of failure**: it deliberately runs cheap local and open-weight models as
delegates (`cmd_agent_delegate.c`, `provider_client.c`) — the very population the
post shows gaining the most from hashline. A harness win here compounds across
every delegate turn, not just the flagship's.

This proposal has three parts, all direct applications of the post's core idea —
*give the model stable server-side identifiers instead of round-tripping content
through its context window*:

- **Part I — Hashline edit core.** Replace the read/edit/write trio with an
  anchor-based edit protocol. Reuses `diff.c`, the code index, and the existing
  spill machinery; no new heavy subsystem.
- **Part II — Lean websearch.** Today `web_search` returns a compact result block
  but there is **no `web_fetch` tool at all** (confirmed: `g_builtin_tools`,
  `server/agent_tools.c:907`), so to actually *read* a hit the model shells out
  `curl … | …` and dumps whole-page HTML/markdown into context. That is the token
  sink. Replace it with search-returns-handles + a server-side extractive
  `web_read` that ranks page spans on aimee's **local embedder + BM25 fuse**
  (`kb_ranker`) and returns only the query-relevant spans, cited by id — the web
  analog of a hashline anchor.
- **Part III — the adjacent core tools that share the disease.** `read`, `edit`,
  and page-reading are not the only tools that force content through the context
  window or return unstable references. `grep`, large-file reads, symbol-scoped
  reads, whole-function rewrites, and test runs all leak the same way. Part III
  extends the anchor + extract-server-side philosophy across the rest of the core
  toolset — each item reusing a surface aimee already has (the code index,
  tree-sitter, `guardrails_blast_radius`, the condense/spill contract).

## §0 What already exists (DRY map — do not rebuild the left column)

| Mechanism the post/websearch needs | Aimee's existing surface | Verdict |
| --- | --- | --- |
| Structured diff + unified-diff rendering for edit results | `diff.c` — `diff_compute`, `diff_format_unified`, `diff_result_to_json`; `write_file` already returns `{status,path,summary,diff,unified_diff}` (`posix/agent_tools.c:1266`) | **Have it.** Reuse verbatim for hashline write-back. |
| Sub-agent output token blowup ("Claude Code leaks raw JSONL… hundreds of thousands of tokens") | `tool_condense.c` + `tool_output_get` spill-ref (`server/agent_tools.c:915`) — condense in context, fetch full on demand by ref | **Already ahead.** The websearch design reuses this exact spill pattern. |
| Cheap line/symbol hashing | tree-sitter symbol spans (`code_treesitter.c`, `code_collect.c`); FNV/xxhash already in tree | **Have the inputs.** Hashing lines is trivial. |
| Chunk ranking against a query (for `web_read`) | local embedder (`Dockerfile.embedder`, `kb/kb_service_code_embed.c`), hybrid vector+graph ranker (`kb_ranker.h`, `memory_graph_fusion.c`) | **Have it.** Point it at fetched web text. |
| Cheap extractive compression off the hot path | delegate lane / CPU index lane (`#1163`, `#1169`), `cmd_agent_delegate.c` | **Have it.** Run extraction on the CPU lane, not the flagship's context. |
| Read-only-delegate + parent-worktree write guards on any new edit path | `agent_tools_readonly_delegate_blocks()`, `agent_tools_parent_write_guard_blocks()` (`posix/agent_tools.c:1230`) | **Have it.** New verbs route through the same gates. |
| Result/URL cache with TTL, local-only | DB1 (SQLite local store), Two-DB split | **Have it.** `web_search` handle→URL map lives here. |

**Clean gaps this proposal fills:** (1) no stable line identifier — `read_file`
returns raw bytes with **no line numbers and no hashes** (`tool_read_file`,
`posix/agent_tools.c:1124`), so `edit_file` *must* string-match; (2) no
`web_fetch`, so page reading escapes to `curl` and spends unbounded tokens.

---

## Part I — Hashline edit core

### The problem, concretely

`edit_file` today (`tp_edit_file`, `server/agent_tools.c:623`) requires
`old_string` to "match the file exactly (including whitespace/indentation) and be
unique." When the model's recall of whitespace is imperfect, or the line appears
twice, or the file changed since it was read, the edit is rejected (or, worse,
applied to the wrong occurrence). Every rejection is a retry loop: re-read,
re-emit the block, hope. `read_file` gives the model nothing to anchor to.

### The change

Give reads stable anchors; let edits reference them.

**1. `read_file` gains anchored output (opt-out, default on for edit surfaces).**
Each line is prefixed `LINE:HASH|`, where the displayed `HASH` = first 2–3 hex
chars of a fast content hash (FNV-1a/xxhash) of the line's **canonicalized** bytes:

```
  11:a3| function hello() {
  12:f1|   return "world";
  13:0e| }
```

Overhead ≈ 6 chars/line, recovered many times over by not re-emitting blocks on
edit. `offset`/`limit` semantics unchanged. A `raw:true` flag restores today's
output for non-edit consumers (grep pipelines, binary sniffing).

**Display digest ≠ verification digest (blocking fix — hash strength).** The
short `HASH` is a *display* aid only; it is never what the server trusts to verify
freshness. On every anchored read the server records the **full-length digest**
(≥64-bit, e.g. xxh64) of each line's canonical bytes in the read snapshot. Edit-time
verification (§4) compares the *full* current-line digest against the *full*
recorded digest — never the 2–3 hex display tag. So a changed line cannot pass
verification by colliding on 8–12 display bits (a 1/256–1/4096 event); the
collision space is the full digest and silent wrong-apply is not reachable through
hash truncation. The short tag exists purely to give the model a stable,
glanceable token; correctness rides on the server-side full digest.

**Snapshot binding — digests are immutable, not per-path-mutable (blocking fix —
stale-read race).** The recorded digests are **not** stored in a mutable map keyed
by `(path, ordinal)` — that has a race: a *second* read of the same file would
overwrite the first read's digests, and an edit built from the first read would
then verify against the second read's content (silent wrong-apply, the very class
this design eliminates). Instead, each anchored read mints an **immutable snapshot
identity** (`snapshot_id` = read token + file content hash at read time) and
records that snapshot's per-line digests under it. The `edit_file` call **carries
the `snapshot_id`** its anchors came from; verification runs against *that
snapshot's* digests, plus a check that the file on disk still matches the snapshot
(or the intervening delta touches none of the edited ordinals). Concurrent reads
mint independent snapshots and never clobber each other. A snapshot the file has
diverged from yields the same structured re-anchor response as §4, never a blind
apply. Snapshots are TTL/LRU-evicted; an edit citing an evicted snapshot is told to
re-read, not applied on faith.

**Canonicalization (blocking-suggestion fix).** The hash is computed over a
canonical form so trivial encoding differences never cause a spurious mismatch:
line-endings normalized to `\n` (the trailing `\r` of a CRLF file is not hashed),
a leading UTF-8 BOM on line 1 is stripped before hashing, and remaining bytes are
hashed verbatim (trailing-whitespace changes *are* real edits and must be caught).
The rule is fixed and documented so the server and any client compute identical
anchors. Write-back preserves the file's original line-ending and BOM; only the
*hash input* is canonical, never the stored bytes.

**Write-back byte preservation (HIGH fix — line-ending integrity).** The
write-back engine never re-serializes the whole file. Unchanged lines keep their
**original raw bytes verbatim** (their exact line terminator, whatever it was);
only the specific edited/inserted regions are written. For those regions the
server **normalizes the model's `text:` payload to the file's detected dominant
line-ending** before splicing — so a model that emits LF into a CRLF file cannot
introduce mixed endings, and a file with no trailing newline keeps that property.
The terminator is thus a property of the file, not of the model's payload; the
model supplies content, the server owns terminators. **"Dominant" is defined
concretely** (HIGH fix — tie-break): the strict majority terminator among the
file's existing lines; on an exact tie, or a file with no existing terminator (0–1
lines), default to `LF`. A file already mixed is not "repaired" — normalization
applies only to newly written regions, so the server never rewrites lines the edit
did not touch.

**2. The anchor is a COMPOSITE — ordinal is the key, hash is the check (blocking
fix #1).** An anchor is the pair `(line-ordinal, content-hash)`, rendered
`12:f1`. The **ordinal is the primary key** and *by construction disambiguates
identical lines* — two identical bodies at lines 12 and 40 are `12:f1` and
`40:f1`, distinct anchors. The **hash only verifies** the ordinal still points at
the bytes the model read (against the server-side *full* digest of §1, not the
short display tag). So identical-line collision — the failure the roundtable
flagged — cannot re-introduce `str_replace`'s "occurs N times" ambiguity, because
the model never selects by content; it selects by ordinal and the server checks
the full digest. If a resolve is ever ambiguous (see re-anchor below), the server
returns candidate rows *with surrounding context*, never a blind pick.

**3. `edit_file` gains anchor verbs (primary path); `old_string` kept one release
as fallback.** New schema:

```jsonc
{
  "path": "src/foo.c",
  "snapshot_id": "s3f9…",           // the read snapshot these anchors came from
  "edits": [
    { "op": "replace",      "at": "12:f1",              "text": "  return \"world!\";" },
    { "op": "replace_range","from": "20:a3","to":"24:0e","text": "…" },
    { "op": "insert_after", "at": "13:0e",              "text": "  // note" },
    { "op": "delete_range", "from": "30:1c","to":"31:9b" }
  ]
}
```

**4. The anchor IS the verification.** Before applying, the server re-hashes the
target line(s) in the *current* file (canonical form) and compares to the
snapshot's **full-length recorded digest** (§1), not the short display tag.
Mismatch ⇒ the file moved under the model ⇒ reject **before** corruption,
returning a **structured re-anchor payload** (MEDIUM fix — defined schema), not a
prose string:

```jsonc
{
  "status": "stale_anchor",
  "path": "src/foo.c",
  "failed": [{ "op_index": 0, "anchor": "12:f1", "reason": "hash_mismatch" }],
  "snapshot_id": "<fresh snapshot minted for this response>",
  "context": [                       // the current contested range, re-anchored
    { "anchor": "8:b2",  "text": "…" },
    { "anchor": "9:44",  "text": "…" }
    // … through the edited range ± a few lines of margin
  ],
  "hint": "file changed since read; retry edits against snapshot_id using these anchors"
}
```

The model re-anchors from `context` without a blind full re-read, and the
`snapshot_id` lets it retry immediately against a valid snapshot. This is the
property `str_replace` cannot give: a successful apply *proves* the model edited
the bytes it saw.

**5. Write-back and result rendering reuse `diff.c` unchanged** — same
`{status,summary,diff,unified_diff}` payload `write_file`/`edit_file` already
return, so downstream (slop advisory, guardrails, TUI diff) is untouched.

**6. `write_file` stays** for new files and full replacements (schema unchanged).

### Transactional batches (blocking fix #2)

A multi-edit call is **all-or-nothing against the as-read snapshot**, so an early
edit can never invalidate a later edit's anchor within the same batch — the "anchor
churn" failure:

- Every anchor in the batch resolves against the **snapshot the model read**, not
  the incrementally-mutated buffer. The model's anchors are all valid
  simultaneously because they were all computed from one read.
- The server **plans** the batch, then applies edits **bottom-of-file-first**, so
  applying one edit never renumbers a not-yet-applied edit's target ordinal.
- **Per-op isolation, batch-level atomicity:** each op is validated (anchor still
  matches the snapshot) before *any* write. If one op fails validation, the batch
  is rejected wholesale with that op flagged — no partial application, no silent
  corruption. (A `partial:true` opt-in may later allow good-ops-through with a
  per-op status vector, but the safe default is atomic.)
- Overlapping/contradictory ranges in one batch (e.g. a `replace_range` and a
  `delete_range` that intersect) are a planning-time rejection with both ops named.
- **Interacting-but-non-overlapping ops are checked at planning time too** (HIGH
  fix): two `insert_after` ops targeting the *same* anchor (ambiguous resulting
  order), or an insert whose target ordinal falls inside another op's deleted
  range, are flagged before any write. Because every op is expressed against the
  as-read snapshot's anchors and applied bottom-first, non-adjacent edits never
  interact; the planner's job is only to reject the genuinely ambiguous adjacencies
  (same-anchor inserts, insert-into-deleted-range), and it does so explicitly
  rather than silently picking an order.

The model, therefore, never has to reason about post-edit offsets — the server
owns offset arithmetic entirely, which is the whole point of moving the identifier
server-side.

### Why aimee gains more than a single-model harness

The post's headline: *"the weakest models gain the most"* (Grok Code Fast 1:
6.7% → 68.3%). Aimee routes summarize/review/boilerplate to the cheapest capable
model and to local GPUs. Those delegates are precisely the low end of the post's
curve, so hashline lifts aimee's *median delegate turn*, not just chat. Fewer
retry loops also means fewer delegate re-invocations — a direct bill reduction on
top of the token reduction.

### Scope / migration

- Additive schema; `old_string` path retained behind a deprecation flag for one
  release so no in-flight session breaks.
- Anchored reads default **on** for `TSURF_ALL` edit surfaces, off for `raw`
  consumers.
- All new write paths route through the existing read-only-delegate and
  parent-worktree guards — no new bypass.

---

## Part II — Lean websearch (`web_search` + extractive `web_read`)

### The problem, concretely

`web_search` (`server/web_search.c`) already returns a bounded block — up to 10 ×
(title + url + snippet), 8 KB cap. That part is fine. The waste is **downstream**:
there is no `web_fetch`, so once the model picks a hit it runs `curl`/`lynx`
through `bash` and pastes the entire page (frequently 5–50 KB of markdown, most
of it irrelevant) into its context. Mainstream harnesses have the same shape —
`WebFetch` hands back whole-page markdown. That is where the tokens go.

### The change — the web analog of a hashline anchor

Keep search cheap; make *reading* extractive and referenceable. Two tools, all
heavy lifting on the CPU/local lane:

**1. `web_search(query, k)` → a compact ranked index with stable handles.**
Trim each result to `r<i>  <title>  ·  <host>  ·  ≤120-char snippet`; drop the
blank-line-padded block formatting. The `handle → full-URL` map is held
server-side in DB1 (TTL-scoped). The model refers to results as `r1…rk` and
**never re-emits a URL**.

```
r1  Zig 0.14 release notes  ·  ziglang.org  ·  async removed; new IO interface…
r2  std.Io migration guide  ·  ziglang.org  ·  passing an Io implementation to…
```

**2. `web_read(ref, query?, span?, mode?)` → only the query-relevant spans, cited
by id.** Server fetches the page (once; cached), strips to text, chunks it, and
ranks chunks against `query` (defaulting to the original search query). Returns
the top-N spans (target ≤1.5 KB), each tagged:

```
r2#1  (std.Io migration guide § "Constructing an Io")
      Pass an Io implementation explicitly; the previous global event loop is gone…
r2#3  (§ "Blocking vs. evented")
      …
```

**Hybrid ranking with a mandatory literal pass (blocking fix #3).** Pure semantic
ranking can bury exactly the tokens an agent came for — an exact API name, an
error string, a version number, a flag. So `web_read` fuses two legs, literal
*first*:

- **Literal leg (mandatory, runs first):** if `query` (or quoted sub-phrases /
  identifier-shaped tokens within it) occurs verbatim in the page, those spans are
  **guaranteed into the result set**, above the semantic spans. Exact-substring and
  identifier matches are never displaced by a higher embedding score. This reuses
  the same lexical/BM25 leg `kb_ranker` already fuses for code search.
  - **Budget reservation + omission signal (MEDIUM fix).** The literal leg gets a
    **reserved share of the ≤1.5 KB budget** (default ~60%, tunable) that semantic
    spans cannot encroach on, so semantic relevance never crowds a literal needle
    out. If literal matches *themselves* exceed that reserve, they are ranked by
    match density / proximity to the query and the surplus is **explicitly
    reported** (`literal_matches: N, shown: M, omitted: N−M`) with the exact
    retrieval path — `web_read(ref, mode:"literal")` returns *all* literal spans
    (spilled by ref if large). The agent is never silently told "no more matches"
    when the budget, not the page, ran out.
- **Semantic leg:** the embedder ranks the remaining chunks for topical relevance,
  filling the budget beneath the guaranteed literal spans.
- **Full-page escape hatch:** `web_read(ref, mode:"full")` returns the entire
  stripped page via the **`tool_output_get` spill ref** (not inline) — the agent
  can always bypass extraction when it must, at a token cost it chose explicitly.
  Every extractive result advertises the spill ref and total span count so the
  agent knows extraction happened and how to get the rest.

The full stripped page is retained server-side. Want a specific dropped span?
`web_read(ref="r2", span=7)` pulls span 7 on demand — the **exact
`tool_output_get` spill pattern** already in the tree (`server/agent_tools.c:915`),
so no new retention machinery. Optionally a cheap local delegate does one
extractive-compression pass on the top spans; that runs on the CPU lane, never in
the flagship's window.

**SSRF egress policy on the raw-URL path (blocking fix — server-side fetch).**
Because `web_read` accepts a raw URL and fetches it *server-side*, it is an SSRF
surface: a coerced agent could aim it at cloud metadata (`169.254.169.254`),
`localhost`, or private-network services. The fetcher enforces a strict egress
policy on **both** handle-derived and raw URLs: scheme restricted to `http`/`https`
only. The host is **resolved once, the resolved IP validated** against a deny-list
(RFC1918, loopback, link-local `169.254/16`, IPv6 ULA/link-local, and other
reserved ranges), and — critically — the connection is then **pinned to that exact
validated IP** rather than re-resolved. Resolve-then-connect without pinning is a
TOCTOU hole: a hostile resolver can return a public IP at check time and a private
IP at connect time (**DNS rebinding**). Pinning to the checked IP closes it — the
socket connects to the address that was validated, not to a fresh lookup. Redirects
are capped and **each hop is independently resolved, validated, and pinned** the
same way (no redirect to an internal host, no rebind between hops); response size
and time are bounded. This reuses the same egress posture the search backends
already sit behind; the raw-URL convenience does not open a new hole.

**Untrusted-content marking (accepted security suggestion).** Extraction
concentrates web text into highly-salient snippets, which raises prompt-injection
exposure. Every `web_read` span is returned **fenced and labeled as untrusted
retrieved content**, carrying the standing guidance that retrieved text is data,
never instructions, and cannot override system/developer/user/repo directives.
This mirrors the treatment already applied to external content elsewhere in the
harness; the ranking change does not relax it.

### Token comparison (illustrative, one "read the docs" step)

| Path | Bytes into the model's context |
| --- | --- |
| Today: `bash curl … \| html2text` on one page | ~5–50 KB whole-page markdown |
| Mainstream `WebFetch` | whole-page markdown |
| Proposed `web_read` (top spans, cited) | ≤ ~1.5 KB, all query-relevant |
| Search block itself (trimmed vs. current) | ~40–60% smaller |

The saving is structural: ranking and extraction happen on aimee's cheap local
lane; only the spans that survive ranking cross into the expensive context. Same
philosophy as Part I — the identifier (`r2#3`) travels, the content stays home.

### Scope

- New builtin `web_read`; `web_search` output trimmed and handle-cached.
- `web_read` accepts **either a search handle (`r2`) or a raw URL** — the agent
  often already has a URL (from an error message, a stored memory, a config
  value) and should get the same extractive, cited-span treatment without a
  throwaway search first.
- Backends unchanged (DuckDuckGo / SearXNG / Tavily); Tavily's `content` field
  seeds the first spans for free.
- Reuses `kb_ranker` (literal + semantic fuse) + embedder + the `tool_output_get`
  spill contract — no new heavy subsystem.

---

---

## Part III — The adjacent core tools, redesigned for agents

**Design axis for every tool below:** minimize tokens crossing into the model's
context, maximize impact per token, and return **stable references the agent can
act on directly** — no re-read, no re-emit, no guess. A tool built for a human
optimizes for a readable dump; a tool built for an agent optimizes for the *next
action* the agent will take. That reframing is what drives each change.

Prioritized by (tokens saved × how often an agent hits the path):

| # | Tool | Today (human-shaped) | Agent-shaped redesign | Payoff | Reuses |
| --- | --- | --- | --- | --- | --- |
| 1 | **`grep`** | returns `file:line: text` — to edit a hit the agent must then `read_file` for the anchor/context | return `file:LINE:HASH│ text`; every hit is a **ready edit anchor** consumable by `edit_file` with no intervening read | kills the grep→read round-trip on the most-used search path | hashline anchors (Part I), `tp_grep` |
| 2 | **`read_file` outline mode** | large files must be read whole (offset/limit guesswork) to find the target | `mode:"outline"` returns the tree-sitter symbol skeleton — signatures + `LINE:HASH` anchors only, no bodies | one cheap call maps a 2k-line file; agent then reads/edits one span by anchor instead of paging the file | `code_treesitter.c`, Part I anchors |
| 3 | **`read_symbol`** | to see one function the agent reads the enclosing file | fetch just a symbol's def span (by identifier, via `aimee index find`), anchored | "show me `foo`" costs the span, not the file | `find_symbol`, `aimee index find`, `code_collect.c` |
| 4 | **`edit_symbol`** | whole-function rewrite = reproduce the whole body as `old_string`/`new_string`, or line-anchor juggling | `{op:"replace_body", symbol:"pkg::Type::foo", text:"…"}` — server resolves the span from the index and swaps it; **FQN required or disambiguation forced** | robust whole-symbol rewrites with **no reliance on line-hash recall**; the highest-value single edit an agent makes | code index span resolution, `diff.c` |
| 5 | **`run_tests`** | agents run tests via `bash`; a green suite still dumps thousands of lines into context | structured run: `{passed, failed, skipped, failures:[{name, file:line, message}]}` — **counts + failures only**; full log spillable by ref | turns a multi-KB test dump into a few hundred tokens; the pass case costs ~1 line | `tool_condense.c` + `tool_output_get` spill, workflow test-command parser (`test_workflow_parse_test_command`) |
| 6 | **`edit_file` dry-run / blast preview** | edits apply blind; blast radius is a separate skill invocation | optional `dry_run:true` returns the unified diff **plus** the pre-computed blast radius before writing | lets the agent self-gate a risky multi-file batch in one call instead of edit-then-discover | `guardrails_blast_radius.c`, `aimee index blast-radius`, `diff.c` |
| 7 | **Multi-file edit batch** | one `edit_file` call per file → N tool round-trips for one logical change | `edit_file` accepts `files:[{path, edits:[…]}]`, applied all-or-nothing with per-op anchor validation (transactional semantics from Part I) | one refactor = one call + one diff; fewer turns, one atomic guard check | Part I anchor engine + transactional batches, write guards |

### Short specs for the two highest-impact additions

**`edit_symbol` (#4) — with a mandatory disambiguation protocol (blocking fix
#4).** The single most common non-trivial edit an agent makes is "rewrite this
function." Under `str_replace` that means echoing the entire old body (to match)
*and* the entire new body — double the tokens and maximal recall risk. But naive
symbol resolution is its own footgun: `foo` may be overloaded, shadowed, or defined
in several modules. So resolution is **strict**:

- The agent supplies a **fully-qualified name** (module/type path, e.g.
  `pkg::Type::foo` or `foo.c:free_form_helper`) whenever the bare name is not
  provably unique in the index.
- If the supplied name resolves to **more than one** definition, the server does
  **not** guess — it returns the candidate signatures as **anchored outline rows**
  (Part I anchors, one per definition site), and the agent re-issues with the FQN
  or falls back to a hashline `replace_range` on the chosen candidate.
- A resolve to exactly one definition proceeds; the replaced span is verified with
  the same anchor-hash check as Part I before write-back, so an index that has gone
  stale relative to the file is caught, not blindly trusted.
- **Identity confirmation on the single-resolve case (MEDIUM fix).** "Exactly one
  match" is not "the right match" — a bare name can resolve to a parent-class method
  when the agent meant the override, or vice versa. So the response **echoes the
  resolved target's identity** — fully-qualified signature, file:line, and the
  index version it resolved against — and `replace_body` requires the agent to have
  supplied an FQN *or* to confirm against that echoed signature; a bare-name resolve
  that is unique but whose echoed signature the agent did not assert is returned for
  confirmation rather than applied blind. The index version is bound into the
  anchor-hash check so a resolve against a stale index cannot silently edit a moved
  definition.

It composes with `find_symbol`: locate → (disambiguate/confirm) → replace, no
read in between.

**`run_tests` (#5).** Test output is the worst offender for tokens-per-impact: on
success the agent needs one bit (green) plus the counts to know scope, on failure
it needs the failing names, locations, and messages — never the full log.
Returning `{passed, failed, skipped, failures:[…]}` with the raw log behind a spill
ref (`tool_output_get`) means a passing suite costs a line and a failing suite costs
only the parts that inform the next action. Pass/skip counts are retained (a
roundtable ask) so the agent can tell "everything green" from "half the suite got
skipped by a collection error." This is the `tool_condense` philosophy applied to
the one output agents hit on every iteration of a fix loop.

### Explicitly *not* redesigned (already agent-shaped — DRY)

- **`bash` / `execute_script` output** — already condensed with spill refs
  (`tool_condense.c`). Do nothing.
- **`git_diff` / `git_status` / `git_log`** — already compact, structured, and
  reference-stable (commit SHAs, porcelain). Do nothing.
- **`search_memory` / `search_docs`** — already hybrid-ranked and budgeted
  (`memory_context.c`, `kb_ranker`); `search_docs` already caps passages. Do
  nothing.
- **`tool_output_get`** — the spill contract every tool above leans on. Keep as-is.

## Evaluation (mirror the post's method; use aimee's replay harness)

The post's credibility comes from its benchmark (16 models × 180 React-codebase
mutation tasks × 3 runs, measuring pass@1 and output tokens). Aimee already has
the scaffolding for this style of eval (`tools/*_replay.py`, `tests/test_agent.c`
edit tests, the model registry in `model_registry.c` / `models_dev`).

**Part I gate (must pass before `old_string` is removed):**
- Fixture set of N single- and multi-edit mutations over a real repo checkout,
  **including fixtures with duplicated identical lines and stale-file drift** (the
  two collision/churn footguns) so the composite-anchor and transactional-batch
  guarantees are exercised, not just asserted.
- Replay `str_replace` vs. hashline across the delegate model roster; report
  pass@1 and mean output tokens per task.
- **Net token-delta gate (accepted suggestion):** report per-model *total* token
  delta = anchored-read overhead (≈6 chars/line × lines read) **minus** edit-block
  savings, and require it net-negative overall and never worse than +2% on any
  single model. Hash overhead must not silently eat the win.
- Ship criteria: hashline ≥ `str_replace` pass@1 on every model, strictly better
  on the local/open-weight delegates, and net token-negative overall.

**Part II gate:**
- Set of "answer needs one web page" tasks; compare context tokens and answer
  correctness for `curl`-dump vs. `web_read`.
- **Literal-recall assertion:** for tasks whose answer hinges on an exact API
  name / error string / version, assert the literal leg surfaced that span (the
  needle is never ranked out).
- Ship criteria: ≥50% fewer context tokens at equal-or-better answer accuracy;
  spill (`web_read span=` / `mode:"full"`) recovers any span the ranker dropped.

**Part III gate (per tool; each must be token-negative at equal task success):**
- `grep`-anchored, `read_symbol`, outline: measure tokens + tool round-trips to
  complete a fixed set of "locate then edit" tasks vs. today's grep→read→edit.
- `edit_symbol`: pass@1 + output tokens on whole-function-rewrite fixtures vs.
  `str_replace` and vs. hashline anchors — **including overloaded/shadowed-symbol
  fixtures that must trigger the disambiguation protocol, not a wrong resolve.**
- `run_tests`: context tokens on a passing suite and on a suite with K failures
  vs. raw `bash` test output; assert no failing case is dropped from the summary
  and pass/skip counts are present.

## Risks & mitigations

- **Anchor churn on large multi-edit batches** (early edits renumber later
  anchors). **Resolved in spec** (§Part I "Transactional batches"): anchors
  validate and apply **against the as-read snapshot**, bottom-of-file-first; the
  server, not the model, resolves offsets. A batch that references a line an
  earlier edit deleted fails that one op with a precise message; the atomic default
  rejects the whole batch rather than partially applying.
- **Hash collision.** The 2–3 hex tag is a *display* aid only; verification uses
  the full-length server-side digest (§1), so a changed line cannot pass by
  colliding on the truncated tag. The `LINE:` ordinal is the primary key and
  disambiguates identical content; the full digest *verifies* freshness. Silent
  wrong-apply via hash truncation is therefore not reachable.
- **Extraction hides the needle in `web_read`.** **Resolved in spec** (§Part II):
  the literal leg guarantees exact-substring / identifier spans into the result
  above semantic spans, `mode:"full"` is always available via spill, and every
  result advertises the span index + section headers so any dropped span is
  pullable by id.
- **Prompt injection via concentrated web spans.** Spans are fenced and labeled
  untrusted with standing "data-not-instructions" guidance; extraction does not
  relax the external-content contract.
- **SSRF / DNS rebinding via the `web_read` raw-URL path.** **Resolved in spec**
  (§Part II SSRF egress policy): http/https only, resolved-IP deny-list for
  private/reserved ranges, **connection pinned to the validated IP** (per redirect
  hop) so no rebind between check and connect, bounded size/time — the server-side
  fetcher cannot be aimed at metadata/localhost/internal services.
- **Stale-read race on anchor digests.** **Resolved in spec** (§Part I.1 snapshot
  binding): reads mint an immutable `snapshot_id`; edits verify against that
  snapshot, so a concurrent read cannot clobber the digests an in-flight edit
  relies on.
- **Migration breakage.** Dual-path release; `old_string` and `curl`-based
  reading keep working until the eval gates pass.

## Phasing (re-sequenced per roundtable: prove the transactional core first)

1. **P1 — anchored `read_file`** (composite anchors, canonicalization, snapshot
   binding with full-length digests, `raw` opt-out) + fixture eval harness (incl.
   duplicate-line, drift, and concurrent-read fixtures). Non-behavioral for
   existing edits.
2. **P2 — `edit_file` anchor verbs + transactional batches + `dry_run`/blast
   preview** (dual-path with `old_string`), write-back via `diff.c`. Dry-run/blast
   moves *into* the core-safety tier (was P4) so the edit core ships with its
   self-gate. Run the Part I gate.
3. **P3 — low-risk Part III tools:** `grep` anchors, `read_symbol`, `read_file`
   outline, structured `run_tests`. All fall out of the Part I anchor engine +
   existing spill; each behind its own token-negative gate; none blocks the others.
4. **P4 — semantic tier (only after the core is proven):** `web_search` handles +
   extractive `web_read` (literal-first fuse, `mode:"full"`) on `kb_ranker` +
   spill, and `edit_symbol` (FQN + disambiguation protocol). These are the
   highest-footgun items and ship last, each behind its gate.
5. **P5 — remove deprecated `old_string` path** once the P2 gate is green across
   the roster.

## Non-goals

- No separate "edit-fixer" model (the post cites Cursor's 70B fixer as a symptom,
  not a cure).
- No change to backends, guardrails semantics, or the memory/KB contracts.
- No new provider or protocol surface.

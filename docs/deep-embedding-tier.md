# Deep embedding tier (4B)

An optional 4B embedding tier that is **authoritative** over the live 0.6B tier.
The 0.6B (run at reduced precision — int8 — for speed) gives the user a *fast
initial result*; the 4B is the **higher-quality source of truth** and supersedes
the 0.6B's view once its background pass has re-embedded the corpus. The design is
"fast 0.6B preview now, authoritative 4B answer once complete."

This authority shows up two ways:
1. **Precision guards** — wherever the 0.6B makes a decision (a merge, a
   contradiction, a duplicate), the 4B confirms or vetoes it. Where they disagree,
   **the 4B wins.** Already live across memory and the curator (second half of
   this page).
2. **Deep recall** — `--deep` searches the authoritative 4B index directly. Today
   this is opt-in because a 4B *query* embed is ~2.7 s; see
   [Recall: making the 4B authoritative](#recall-making-the-4b-authoritative).

**Default OFF** (`memory_deep_embedding_enabled`). A lite deployment runs the 0.6B
alone (~0.5 GB RAM, served fp32 for full quality since nothing backstops it). The
deep tier adds a ~16 GB resident 4B model + a second index, lazy-loaded, so it is
opt-in for hosts with the RAM. For the live tier itself (0.6B model, reranker,
int8, halfvec) see [retrieval-stack.md](retrieval-stack.md).

## How it works

| Column | Dim | Model | Written by | Authority |
|---|---|---|---|---|
| `embedding` | `halfvec(1024)` | 0.6B (live, int8) | every memory write | fast interim — used immediately, and where the 4B hasn't filled in yet |
| `embedding_deep` | `halfvec(2560)` | 4B (deep) | background backfill | **authoritative** — supersedes the 0.6B once populated |

Two columns rather than a dimension swap, because a query must embed with the same
model as the column it searches (0.6B = 1024-dim, 4B = 2560-dim) and the 4B is
populated lazily. The 0.6B is never *deleted* — it's the fast path while the 4B
catches up — but once a memory has a `embedding_deep`, the 4B is the truth for it.
Both columns are `halfvec` (fp16); 2560 exceeds pgvector's 2000-dim `vector`-index
cap, so halfvec is required (it also halves index memory at negligible quality
cost).

- **Embedder** serves the 4B on `/embed_deep` (env `DEEP_MODEL`, default
  `perplexity-ai/pplx-embed-v1-4b`), lazy-loaded on first call.
- **Backfill** is a throttled `memory_maintenance` mode (`MEMORY_MAINTENANCE_MODE_DEEP_EMBED`):
  each cycle deep-embeds up to 25 memories that lack `embedding_deep`, oldest
  first, so a large corpus fills in gradually. The *first* batch triggers the
  embedder's one-time ~150 s 4B load.
- **Deep recall**: `aimee memory search --deep <query>` embeds the query with the
  4B (~seconds) and searches `embedding_deep`. It reuses the existing
  `memory.find_facts_scoped` `/v1` endpoint with a `deep` flag — no new RPC.

## Enabling it

1. **pgvector ≥ 0.7** is required (halfvec). The whole stack already requires it
   once embeddings are unified on halfvec.
2. Ensure the embedding columns are halfvec: a fresh schema apply already is; an
   existing `vector(N)` database runs `deploy/migrations/2026-embed-halfvec.sql`
   (a cast, no re-embed).
3. Point the embedder at a 4B-capable host and pair the live tier with int8 (the
   4B backstops the small drift):
   ```
   AIMEE_EMBEDDER_QUANTIZE=int8        # 0.6B served int8 (~3.3x faster)
   DEEP_MODEL=perplexity-ai/pplx-embed-v1-4b
   ```
4. Flip the gate and point the deep-embed command at the sidecar:
   ```
   aimee config set memory_deep_embedding_enabled 1
   aimee config set memory_deep_embedding_command "python3 /opt/aimee/scripts/embed-deep-remote.py"
   ```
5. The backfill runs on the normal maintenance cycle. Until `embedding_deep` is
   populated, `--deep` recall returns empty; live recall is unaffected throughout.

## Recall: making the 4B authoritative

The intent is that the 4B is authoritative for recall too: the user gets a fast
0.6B (int8) answer immediately, and the 4B result — once the corpus is backfilled
— is the one to trust.

The tension is latency: a 4B *query* embed is ~2.7 s on CPU, so it can't run
synchronously on every turn. The shape that satisfies both is **progressive
recall** — return the 0.6B candidates instantly, then re-resolve/re-rank against
the authoritative 4B `embedding_deep` index and update the result. The two stored
columns (fast 0.6B, authoritative 4B) are exactly what that needs.

Today the building blocks exist but the auto-progression is not yet wired: the
4B index is reachable via `aimee memory search --deep` (the authoritative
answer, paid ~2.7 s up front), and the live per-turn path is the 0.6B. Wiring the
0.6B→4B progression into the default recall is the remaining step to make recall
fully 4B-authoritative.

The **precision guards** (below) are already fully 4B-authoritative — they always
let the 4B override the 0.6B.

- The backfill is deliberately slow (throttled). It's a "take your time" pass.
- Until a memory has an `embedding_deep`, its authoritative view is still the 0.6B;
  `--deep` recall only sees backfilled memories.
- On a lite (0.6B-only) deployment, leave the tier off and serve fp32
  (`EMBEDDER_QUANTIZE` unset) for full quality, since nothing backstops the 0.6B.

---

# The deep tier as a precision guard

Beyond `--deep` recall, the 4B space is used to **confirm or veto** decisions the
live 0.6B embedding makes. The pattern is uniform across every site:

- The live embedding generates a **candidate** (a recall hit, a merge, a
  contradiction, a duplicate).
- When the deep tier is on **and both sides have a deep embedding**, the 4B cosine
  must agree before the candidate is acted on.
- The guard is **never destructive on its own**: it can only *prevent* a merge or
  *withhold* a link — it can never collapse, drop, or invent.
- **Partial coverage degrades gracefully**: if either side lacks a deep embedding,
  the guard is a no-op and prior behaviour holds.
- Everything is gated by `memory_deep_embedding_enabled` (default off ⇒ exact
  prior behaviour) and reuses `memory_deep_embedding_command` (the 4B
  `/embed_deep`). Thresholds are deliberately conservative — **tune them on a real
  corpus before relying on the guards.**

## Memory: contradiction pre-filter

`memory_scan_retroactive_conflicts` finds candidate contradiction pairs by lexical
term overlap, then runs an expensive `is_contradiction` check. With the deep tier
on, a pair whose two memories are deep-embedded but clearly *unrelated* in the 4B
space (cosine < 0.35) is skipped — the term overlap was coincidental, not a real
contradiction. Fewer false contradictions, fewer LLM calls.

## Curator deep tier

The deep-curator's four vector tables each gain a deep column + a guard:

| Table | Decision | Deep role | Threshold |
|---|---|---|---|
| `curator_entity_vectors` | `resolve_entities` merge | reject a confident live merge the 4B disagrees with — cannot collapse distinct canonical entities | live ≥ 0.85, reject if deep < 0.60 |
| `curator_claim_vectors` | `detect_contradictions` | **fuzzy mining** (new): paraphrased contradictions (same-meaning subject+attribute, different value) the exact self-join misses, each *required* to be deep-confirmed before linking | live ≥ 0.80, link if deep ≥ 0.75 |
| `curator_narrative_vectors` | index time | **similarity links** (new): additive `similar_to` edges between near-duplicate narratives | live ≥ 0.85, link if deep ≥ 0.85 |
| `curator_code_unit_vectors` | index time | **clone links** (new): additive `similar_to` edges between near-duplicate code bodies | live ≥ 0.85, link if deep ≥ 0.85 |

Notes:
- **Entity** is merge-*preventing* (the only guard that changes an existing
  decision); the others are purely *additive* (new links) or *new* detection.
- The deep columns are stored at curate/index time when the tier is on (the
  curator is a batch process, so the extra 4B embed is acceptable). Entities and
  claims committed before the tier was enabled lack deep embeddings until
  re-curated, so their guards no-op until then.
- `curator_claim_vectors.subj_attr_deep_vec` holds the deep subject+attribute
  embedding; `curator_code_unit_vectors.body_deep_vec` holds the deep body
  embedding (the clone signal); narratives use `embedding_deep`.
- All `similar_to` / `contradicts` links are written via `db2_artifact_link`,
  which is idempotent, so re-curation is safe.

## Migrations

Enabling the deep tier on an existing database needs the deep columns + indexes.
`deploy/migrations/2026-embed-deep-halfvec.sql` adds all of them (memory +
curator) idempotently and fails loudly if pgvector lacks `halfvec`. A fresh schema
apply already includes them (exception-guarded, so a pgvector without halfvec just
NOTICEs and skips).

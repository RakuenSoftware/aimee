# Proposal: structured PDF ingestion + coordinate-anchored evidence layer (KB)

- **State:** reviewed — **roundtable sign-off (R3)**. Three review rounds
  (R1: 10 blockers → all CLOSED in R2; R2: 2 new security blockers → both CLOSED
  in R3). R3 verdict: reviewer / architect / data-model **Pass**, security
  **Conditional Pass** (residual = explicitly-deferred automated PII detection, a
  defensible v1 boundary), **no remaining blocking findings — READY FOR
  SIGN-OFF.** Ready for implementation per the phasing below, pending human
  sign-off. See **Review revisions (R1)** / **(R2)** / **(R3)**.
- **Author:** JBailes
- **Date:** 2026-06-13
- **Charter roles:** Ingest (PDF → structured pages/blocks/lines/chunks with
  geometry), Recall (coordinate-anchored retrieval + neighbor/page/table
  expansion over the KB `/v1` surface), Calibrate (corpus-level answerability
  signal returned with results), Curate (table regions promoted into the typed-
  fact layer), Gate-Promote (default-off flag rollout per the readiness
  program).
- **Scope — entirely aimee-KB-side except one thin client call:**
  a new PDF extraction front-end (`src/kb/kb_doc_pdf.{c,h}` — parse to pages /
  blocks / lines / chunks + per-line geometry, OCR/TSR via optional local
  sidecars, same pattern as the embedder/reranker), additive geometry on the
  existing chunk spine (`src/db2/schema_sqlite.sql` + the Postgres equivalent:
  `page_start` / `page_end` columns on `kb_documents`; a new fine-grained
  `kb_doc_regions` table keyed by chunk id for per-line `page_no` + normalized
  `bbox` + `quote`; a `kb_doc_assets` table for figure/table crops), reuse of
  the existing heading-chunker / `prev_chunk_id`–`next_chunk_id` links / `kb_fts`
  FTS5 / `kb_async_jobs` embedding path in `src/kb/kb_ingest_normalize.c` +
  `src/kb/kb_service_kb.c`, a read-only document-evidence retrieval surface on
  the KB `/v1` API (handlers in `src/kb/kb_service_kb.c` + the KB HTTP routes),
  ingest acceptance of `application/pdf` in `src/server/kb_client_docs.c`, and a
  **thin** consumer hook in `src/server/ingress_preinject.c` (one new
  `kb_client_*` call returning chunks + citations; the server's existing
  per-user confidence/steer logic is untouched). Unit + integration tests.
  **No new datastore, no new service** (sidecars follow the existing optional-
  model deployment pattern); the table-facts path *feeds* the existing
  typed-fact layer rather than duplicating it.

## Goal

Let the **company knowledge base** ingest PDFs into the same inspectable,
retrievable spine it already uses for code and docs — but with **page and
bounding-box geometry retained on every chunk**, so retrieval can return
citations that point back to the exact region of the exact page a fact came
from. Tables become structured facts; figures/tables become retrievable crops;
scanned PDFs are reachable via OCR.

This is an **aimee-KB feature.** The KB is the shared, canonical, multi-user
store: one PDF is ingested once for the whole company, deduped, access-
controlled, and read by *any* person's aimee-server through `kb_client`. The
personal agent (aimee-server) gains only a thin client call and keeps its
existing per-user steering — it does not parse PDFs and does not hold the
evidence model. See [[kb-vs-server-tenancy-boundary]].

The design deliberately rides the spine we already have rather than standing up
a parallel pipeline, and it closes the loop with two pending KB proposals: it
supplies the **coordinates** that `auditable-correctness-for-the-kb` needs to
make provenance verifiable, and it supplies a high-yield **table → typed-fact**
source for `typed-fact-knowledge-layer`.

## §0 What already exists (so we don't rebuild it)

The `kb_documents` spine is already structure-aware. The PDF work *extends* it;
it does not replace it.

- **Structure-aware chunking.** `kb_documents` already carries
  `heading_path`, `chunk_index`, `chunk_strategy` (default `'heading'`),
  `doc_kind`, and `chunk_context`. Chunking on structural boundaries (sections /
  headings) is the existing default, not something to invent.
- **Neighbor links.** `prev_chunk_id` / `next_chunk_id` columns already thread
  chunks into reading order, with `idx_kb_docs_prev` indexed. "Open the
  neighbors of this chunk" is a column lookup we already support.
- **Lexical search.** `kb_fts` is an FTS5 virtual table over
  `kb_documents(content, heading_path)`. Lexical chunk search exists.
- **Whole + chunk retention.** Per the standing "always keep origin artifact"
  rule, the whole document is retained alongside its chunks; aggregate checks
  retrieve rather than re-ingest. PDFs follow the same rule.
- **Async embedding.** `kb_async_jobs` (`kind='embed_raw'`) already drains
  chunks to the embedder. New PDF chunks enqueue the same way — embeddings are
  one retrieval path among several, not a precondition for ingest. **Embeddings
  stay text-only** (chunk `content`); geometry is *not* embedded and does not
  participate in ranking — so there is no new embedding model and the "no new
  service" claim holds. Geometry is used only for citation resolution (§2, §5).
- **Document upload/push surface.** `src/server/kb_client_docs.c`
  (`..._manifest_json` / `..._upload_json` / `..._push_json`) already does
  client-push document ingest with hashing and dedup.
- **Optional local model sidecars.** The embedder and reranker already deploy as
  optional local models the KB calls out to. OCR and table-structure-recognition
  models follow exactly that pattern — no new service class.

**The genuine gaps for PDFs:** (1) nothing parses a PDF into text + geometry;
(2) the chunk spine stores text-line ranges (`line_start`/`line_end`), not
`page` + `bbox`; (3) no table-region extraction; (4) no figure/table crop
storage; (5) no OCR fallback; (6) retrieval does not surface page/bbox/quote
citations; (7) no read-only "open page / open neighbors / inspect structure /
look up table" tool surface over `/v1`.

## §1 PDF extraction front-end (`kb_doc_pdf`)

A new KB-side module parses an uploaded PDF into the structures the spine
expects, **plus geometry**:

- **pages → blocks → lines**, each line carrying a `page_no` and a *normalized*
  `bbox` (`[x0,y0,x1,y1]` in `[0,1]` page-relative coordinates, so they survive
  re-rendering at any zoom).
- a **deterministic structure tree** (heading levels → section paths) that
  populates the existing `heading_path` / `chunk_strategy='heading'` columns.
- **chunks** assembled on structural boundaries from the lines, threaded with
  `prev_chunk_id` / `next_chunk_id`, exactly as the existing chunker does for
  markdown — so semantic chunks land in `kb_documents` unchanged in shape.

**Chunks may span pages (decided).** A heading at the bottom of page N whose body
starts on page N+1 produces one chunk with `page_start=N`, `page_end=N+1`. We
deliberately **preserve the existing heading-based chunker** rather than forcing
a page-boundary split — chunk shape stays identical to markdown, and the
per-line geometry in `kb_doc_regions` (§2) carries the precise page for every
line regardless of where the chunk boundaries fall. The consequence, made
explicit for consumers: `open_neighbors` can return content from an adjacent
page, and a chunk's citations can carry more than one `page_no`. Citation
precision is recovered at the *line* level, not the chunk level, so the coarser
chunk span costs nothing for citations.

**Chunk-size bound.** To keep the line-level citation join (§5) bounded, a chunk
is capped at a maximum line count (~100); a heading section longer than the cap
splits on paragraph (then page) boundaries while preserving its `heading_path`,
so an outsized section never produces a single chunk whose region join dominates
`search_chunks`.

**Heading-extraction fallback.** Heading detection from PDF tags / font
heuristics is flaky across producers. When detection is inconsistent for a
document, extraction degrades to **page-boundary chunking** for that document
and flags it for manual review, rather than emitting unstable heading-based
boundaries that would scramble citations.

Text extraction uses a vendored/optional PDF text+geometry library. Extraction
is deterministic and runs entirely KB-side at ingest time; the result is the
same `kb_documents` rows the rest of the system already understands, with the
new geometry attached (§2). PDF-derived chunks set the existing `doc_kind`
column to `'pdf'`.

## §2 Coordinate-anchored citations

A chunk spans multiple lines and can cross a page boundary, so chunk-level
geometry is too coarse for a citation. Two additive pieces:

- **`page_start` / `page_end`** columns on `kb_documents` (coarse, for "which
  pages does this chunk touch"). These are a *cache* — the authoritative page
  set is `MIN/MAX(page_no)` over a chunk's `kb_doc_regions` rows — populated at
  ingest in the same transaction that writes the regions, so they cannot drift.
- a new **`kb_doc_regions`** table, one row per extracted line:
  `(id, chunk_id INTEGER FK→kb_documents(id) ON DELETE CASCADE, document_key
  TEXT, page_no INTEGER, x0 REAL, y0 REAL, x1 REAL, y1 REAL, quote TEXT,
  line_index INTEGER, content_type TEXT)`. **`bbox` is four `REAL` columns**, not
  a `TEXT` blob, so spatial predicates (overlap/containment) need no per-row
  parsing and an R*-tree index is available on SQLite. `document_key` is
  carried denormalized so access-control filtering needs no join (§ Tenancy);
  `content_type ∈ {text, table_cell, figure_caption}` lets a consumer parse
  `quote` correctly. This is the precise evidence index.

**bbox convention (fixed to avoid citation misalignment):** origin top-left,
normalized **per page** to `[0,1]`; `(x0,y0)` is the top-left corner, `(x1,y1)`
the bottom-right; values clamped to `[0,1]`.

A **citation** is `{ document_key, page_no, bbox, quote }`, resolved at **line
granularity**: `search_chunks` runs FTS5/embedding to get candidate chunks,
joins to `kb_doc_regions` for *all* lines of each chunk (so the consumer has
surrounding context, not just the matched line), and flags the line(s) whose
`quote` overlaps the query terms as the primary highlight. A single FTS match
spanning multiple lines yields multiple flagged line citations. This is exactly
the artifact `auditable-correctness-for-the-kb` wants on the provenance path — a
fact traceable to a highlightable region, not just a relevant document. The two
proposals share this representation; this one *produces* the coordinates, that
one *audits* against them.

## §3 Tables → typed facts; figures/tables → retrievable crops

- **Table Structure Recognition (TSR).** An *optional* local TSR model (ONNX-
  class, deployed like the embedder/reranker sidecars) converts detected table
  regions into structured cells. Cells become **searchable table facts** that
  feed the existing typed-fact layer (`typed-fact-knowledge-layer`) rather than
  a new fact store — tables are the highest-yield typed-fact source in real
  documents. When the sidecar is absent, table regions degrade gracefully to
  text chunks with geometry (still retrievable, just not cell-structured).
- **Visual evidence.** Detected figure/table regions are rendered to crops and
  stored in a new **`kb_doc_assets`** table
  `(id, document_key TEXT, page_no INTEGER, x0/y0/x1/y1 REAL, kind TEXT,
  caption TEXT, blob_ref TEXT, created_at)`. The crop is retrievable as
  source-grounded evidence alongside the surrounding text chunk — the on-ramp to
  multimodal KB entries.
  - **`blob_ref` storage is a new surface, scoped explicitly to Phase 4.** Crops
    are binary and don't belong inline in the DB. They are written to a
    content-addressed blob store under `AIMEE_HOME` (filesystem, one file per
    `sha256`), and `blob_ref` is that `sha256`. Content-addressing gives free
    dedup (identical crops across re-ingests collapse). This is an honestly-new
    storage dependency — called out here, not hidden under "no new datastore"
    (which refers to relational stores).
  - **Blob access is gated, never direct (R2-S1).** The `sha256` is a
    **KB-internal** identifier — it is *never* returned to a client, and the
    blob directory is on the KB host's private filesystem, served by **no**
    static/file route. The sole read path is **`open_asset`**, which takes an
    opaque `kb_doc_assets.id`, applies the same `document_key` access check as
    every other endpoint (§5), and only then streams the bytes. A caller cannot
    construct a path or fetch a crop by guessing/knowing a hash, so the
    content-addressed store does not create an access-control bypass and the
    tenancy boundary holds for binary evidence exactly as it does for text.
  - **Keying / lifecycle.** `kb_documents` has no single "document" row (a
    document is the set of chunk rows sharing `(project, file_path, file_hash)`),
    so `document_key` is that logical identity, not a single-row FK.
    `kb_doc_assets` is filtered for access control by `document_key` exactly as
    chunks are. Orphan cleanup is **not** a relational cascade: when a document's
    chunks are deleted/re-ingested, the same maintenance path deletes its assets
    and dereferences their blobs. Concretely, a periodic maintenance job scans
    `kb_doc_assets` for `blob_ref` values no row references and unlinks those
    files; the job is covered by the integration suite. This is stated so the
    cleanup is designed, not assumed.

## §4 OCR fallback

For scanned / image-only PDFs (no extractable text layer), an optional local
OCR sidecar produces text + per-line geometry, feeding the same §1/§2 path.
Detection is "page has no/low text layer → OCR." OCR availability is a deploy-
time capability, not a hard dependency; without it, image-only PDFs ingest as
asset-only documents (crops, no text chunks).

## §5 Read-only document-evidence retrieval surface (KB `/v1`)

A small set of **read-only** retrieval primitives on the KB `/v1` API, served by
`src/kb/kb_service_kb.c`. These are the company-KB tools any consumer (a
person's aimee-server, a delegate, the roundtable) can call:

- **`search_chunks`** — two-stage: (1) FTS5 + text-embedding candidate retrieval
  over chunk `content`; (2) citation resolution by joining candidates to
  `kb_doc_regions` (§2). Returns chunks with line-level `{page_no, bbox, quote}`
  citations. Optional `--page N` filter.
- **`open_page`** — all regions for one page of a document, ordered by
  `line_index`.
- **`open_neighbors`** — by default the **linear** `prev_chunk_id`/`next_chunk_id`
  walk; an optional `hierarchical=true` mode returns sibling chunks under the
  same `heading_path` (more useful for context around a citation). The mode is a
  parameter, not two endpoints.
- **`inspect_structure`** — the heading/section tree for a document.
- **`lookup_table`** — structured cells for a table region (when TSR ran).
- **`open_asset`** — fetch a figure/table crop by `blob_ref`.

**Access control (every endpoint).** All six endpoints apply the *same*
document-level access check the KB already enforces on `kb_documents`, using the
caller's auth context, and filter on the denormalized `document_key` (no join
required). A caller that cannot read a document cannot `open_page`, enumerate
its regions, or fetch its assets. This is enforced KB-side, not left to the
consumer.

**Answerability signal (contract, §5-A).** `search_chunks` returns a
corpus-level answerability signal with a fixed contract so consumers can depend
on it: a **`float score ∈ [0,1]`** plus an enum **`label ∈ {NONE, LOW, MEDIUM,
HIGH}`** (default thresholds `<0.15`→NONE, `<0.40`→LOW, `<0.66`→MEDIUM, else
HIGH — config-overridable per deployment; the defaults mirror the server's
existing confidence tiers so behaviour is familiar, and the rationale is
documented with them).
Inputs are deterministic and KB-side: max FTS rank of the top-k hits, query-term
coverage across matched chunks, and presence of table-facts for query entities.
A reference test pins it (e.g. "query Q over fixture corpus C ⇒ score ≥ 0.7,
label HIGH"). It is a *shared, document-level* judgment and stays in the KB; it
is **not** folded into the personal server's per-user confidence tier (see
boundary note). It may later be exposed as a standalone, cheaper
`/v1/answerability?q=…` endpoint that runs only signal computation; that is
additive and does not change the `search_chunks` contract.

## §6 PII and sensitivity policy

PDFs are the highest-risk format for PII (contracts, HR, financial, medical), and
this is a *company-wide shared* store — so the policy is stated up front, not
deferred. The v1 position, explicitly:

- **Access control is the primary control.** A PDF is only ever as visible as the
  `document_key`-level permissions allow (§5, Tenancy boundary). Ingesting a
  document does not widen its audience beyond who could already read it.
- **PII detection/redaction is out of scope for v1.** Uploaders are responsible
  for classifying document sensitivity; this is enforced by the upload surface
  refusing documents without a declared sensitivity class (see below), not by an
  automated scanner. A PII-scanner integration is named as **future work**, not
  promised in v1.
- **Sensitivity tagging + quarantine.** The upload path requires a sensitivity
  class on each document (`public | internal | restricted`), stored on the
  document and propagated to its regions/assets for filtering.
  - **Upload-time enforcement (R2-S2, Phase 1).** The `src/server/kb_client_docs.c`
    upload handler requires a `sensitivity_class` field in the upload metadata
    and **rejects the upload** (4xx, no rows written) if it is absent or not one
    of the three allowed values — there is no implicit default that silently
    under-tags a document. The validated class is persisted on the document at
    ingest and copied to every `kb_doc_regions` / `kb_doc_assets` row so
    retrieval filters on it without a join.
  - **Quarantine flow.** A `restricted` document enters `pending_quarantine`,
    which withholds it from `search_chunks` / `open_*` / ingress; an operator
    action transitions `pending_quarantine → confirmed` (available) or
    `→ rejected` (purged). The operator surface is a small KB `/v1` admin route;
    the state machine exists in v1 even if the default for `public`/`internal`
    is immediate availability.
- **Sidecar trust perimeter.** OCR and TSR sidecars run **inside the KB's trusted
  perimeter**, make **no external network calls**, and **retain no document
  content** after processing (any model cache is local, access-controlled, and
  documented). They are not a data-egress path.

This section is a blocker-clearing statement of intent; the precise class enum,
the quarantine state machine, and the upload-time enforcement land in Phase 1
(ingest) and Phase 2 (retrieval filtering), behind the same default-off flag.

## Tenancy boundary (the load-bearing scoping decision)

| Concern | Lives in | Why |
| --- | --- | --- |
| PDF parsing, geometry, table-facts, crops, OCR | **aimee-kb** | shared canonical content; ingested once for the whole company; dedup + access control |
| Evidence/retrieval tools (`open_page`, `search_chunks`, …) | **aimee-kb** `/v1` | read by *any* person's agent; one source of truth |
| "Is there sufficient corpus evidence?" (answerability) | **aimee-kb** | a document/corpus-level judgment, shared |
| Per-turn context assembly + "how hard to steer *this* user" | **aimee-server** | personal; the existing `ingress_preinject.c` confidence/steer/explore-with logic |

The personal server change is intentionally minimal: `ingress_preinject.c`
already issues `kb_client_index_code_search` and `kb_client_memory_context_block`
and folds their results into the `<aimee-context>` envelope. We add **one** more
of the same shape — a `kb_client` call to `search_chunks` — and `open_page`,
`open_neighbors`, `inspect_structure`, `lookup_table` join the envelope's
existing `explore-with:` set so a co-registered agent escalates to *multi-step*
document retrieval through the KB exactly as it already does for code symbols
(naming only `search_chunks` would seed the agent but starve the escalation the
goal calls for). **No new sufficiency logic on the server.** We do *not* fold
the KB's corpus-level answerability into the server's per-user confidence tier —
that would drag company-KB judgment into the personal agent.

## Data model summary

Additive only — no new *relational* datastore, no table rebuilt. (The crop blob
store, §3, is a deliberately-acknowledged new filesystem surface in Phase 4.)

- `kb_documents`: **+ `page_start`, `page_end`** (nullable; non-PDF docs leave
  them null; a write-time cache of `MIN/MAX(page_no)` over the chunk's regions,
  set in the ingest transaction so it cannot drift). Everything else
  (`heading_path`, `chunk_index`, `prev/next_chunk_id`, `chunk_strategy`,
  `doc_kind`='pdf') is reused as-is.
- **new `kb_doc_regions`** `(id, chunk_id INTEGER FK→kb_documents(id) ON DELETE
  CASCADE, document_key TEXT, page_no INTEGER, x0 REAL, y0 REAL, x1 REAL,
  y1 REAL, quote TEXT, line_index INTEGER, content_type TEXT)`.
  Indexes: `(chunk_id, line_index)` for ordered in-chunk retrieval and the
  search→citation join; `(document_key, page_no)` for `open_page` and
  access-control filtering; R*-tree on `(x0,y0,x1,y1)` on SQLite for future
  spatial predicates.
- **new `kb_doc_assets`** `(id, document_key TEXT, page_no INTEGER, x0/y0/x1/y1
  REAL, kind TEXT, caption TEXT, blob_ref TEXT, created_at)`, index on
  `document_key`. Keyed by logical `document_key` (no single-row FK exists);
  orphan + blob cleanup is a maintenance-path step, not a relational cascade
  (§3).
- **Volume / scale.** One region row per line: a 300-page PDF (~50 lines/page)
  ≈ 15k rows; a 10k-PDF corpus ≈ 150M rows. The composite indexes above keep the
  three hot queries (`open_page`, in-chunk join, table filter) index-served.
  Row-count for the *target* corpus must be measured and a partition/archive
  decision taken **before** default-on promotion (gating criterion, not
  after-the-fact).
- table cells: written through the **existing** typed-fact / entity store, not a
  new table.
- `kb_fts`, `kb_async_jobs`: unchanged; new chunks flow through them.

## Surface

- KB `/v1`: `search_chunks`, `open_page`, `open_neighbors`, `inspect_structure`,
  `lookup_table`, `open_asset` (all read-only).
- Ingest: `src/server/kb_client_docs.c` accepts `application/pdf` and routes to
  the new extractor; same hash/dedup/push flow.
- CLI: `aimee doc search [--page N]` / `aimee doc page` / `aimee doc cite` thin
  wrappers over the `/v1` surface (read-only).
- Server: one `kb_client` call in `ingress_preinject.c`; document tools added to
  the envelope `explore-with:` line.

## Phasing (each independently shippable, default-off)

1. **Extractor + geometry.** `kb_doc_pdf` text+geometry extraction →
   `kb_documents` + `kb_doc_regions`; PDF accepted by the upload surface with a
   required sensitivity class (§6); chunks embed via the existing async path.
   (Text-layer PDFs only.)
2. **Citation retrieval + access control.** `search_chunks` / `open_page` /
   `open_neighbors` / `inspect_structure` returning line-level
   `{page_no, bbox, quote}` citations, each gated by the `document_key` access
   check and sensitivity filtering (§5, §6); the thin server consumer call plus
   the four escalation tools in the envelope `explore-with:` set. This is enough
   for `auditable-correctness` to bind provenance to coordinates.
3. **Tables.** TSR sidecar → table cells into the typed-fact layer +
   `lookup_table`. Graceful text-chunk degradation when absent.
4. **Visual + OCR.** `kb_doc_assets` crops + `open_asset`; OCR sidecar for
   scanned PDFs.

## Flags

Default-off behind a KB-side `kb.pdf_ingest_enabled` (and per-sidecar capability
gates for TSR/OCR), promoted per the flag rollout-readiness program once the
six-criterion bar is met. The server consumer call is a no-op until the KB
returns document evidence.

## Non-goals

- Layout-preserving PDF *translation*. Orthogonal to the KB's purpose.
- A discovery/"trending papers" feed. Not a KB concern.
- Replacing embeddings. The evidence layer is lexical+structural+geometric and
  *complements* the existing halfvec/HNSW path; vectors remain one retrieval
  path among several.
- Any per-user document logic on aimee-server beyond the thin consumer call.

## Risks / honest limits

- **Extraction quality varies.** Multi-column, dense-math, and badly-tagged PDFs
  produce noisy blocks/lines. Geometry is best-effort; citations point at the
  extracted region, which may be coarser than ideal. Mitigation: retain the
  whole origin PDF (origin-artifact rule) so re-extraction with a better parser
  is always possible.
- **Sidecar surface.** TSR + OCR add two optional local models to the deploy
  matrix. Both must degrade gracefully to "text + geometry only" / "asset only"
  when absent, and that degradation path must be tested, not assumed.
- **Geometry storage volume.** One `kb_doc_regions` row per line is row-heavy at
  corpus scale (~150M rows at 10k PDFs); the composite indexes keep hot queries
  served, but the target-corpus row count must be measured and a partition/
  archive decision taken before default-on (see Data model).
- **PII in a shared store.** v1 relies on access control + uploader-declared
  sensitivity, not automated detection (§6); a misclassified document is exposed
  to whoever its declared class permits. Automated PII scanning is future work.
- **Heading-extraction stability.** Heading detection from PDF tags / font
  heuristics is flaky across producers (LaTeX, Word, Docs, OCR) — unstable chunk
  boundaries degrade citations; mitigated by the producer-diversity tests below.
- **Boundary creep.** The temptation to compute answerability once and reuse the
  server's confidence tier is real and wrong; keep the two judgments at their
  own layers (see boundary note).

## Tests

- Unit: extractor geometry normalization (bbox in `[0,1]`, top-left origin,
  per-page), structure-tree → `heading_path`, chunk neighbor threading,
  **line-granularity citation resolution** from `kb_doc_regions` (incl. a
  multi-line FTS match flagging multiple lines), cross-page chunk →
  `page_start≠page_end`, TSR-absent and OCR-absent degradation paths,
  answerability score/label thresholds against a fixture corpus.
- Access control: a caller without read on a document gets empty results from
  `search_chunks`/`open_page`/`open_neighbors`/`open_asset` (no enumeration);
  a `restricted`/quarantined document is withheld from `search_chunks`; an
  attempt to reach a crop by its `sha256` outside `open_asset` is denied (the
  hash is never exposed and no file route serves the blob dir); an upload
  missing/with an invalid `sensitivity_class` is rejected with no rows written.
- Performance: a large chunk (~500 lines) exercises the line-granularity join in
  `search_chunks` to confirm the chunk-size bound keeps it off the hot path.
- Producer diversity: ingest the same paper exported from ≥3 producers (LaTeX,
  Word, Google Docs) and assert stable chunk boundaries + citations.
- Integration: ingest a known PDF → assert chunks + regions + citations; KB
  `/v1` round-trip with citations; table region → typed-fact write; blob-store
  round-trip + orphan/blob cleanup on document delete; the thin server consumer
  call folds a cited chunk into the envelope without touching per-user
  confidence.
- Deploy-matrix: PDF ingest with and without each sidecar.

## Review revisions (R1)

Folded in from a four-lens delegate roundtable (reviewer / architect /
security-privacy / data-model). Security returned **Fail**, the other three
**Conditional Pass**; all ten blocking findings are resolved. Each blocker and
where it landed:

- **R1-S1 — no access-control spec for new tables/endpoints** *(security)*:
  added `document_key` (denormalized) to `kb_doc_regions`; §5 now states every
  endpoint applies the existing `kb_documents` document-level check and filters
  on `document_key` with no join; Tenancy table unchanged but enforcement made
  explicit.
- **R1-S2 — no PII / sensitivity policy** *(security)*: new **§6** — access
  control as primary control, uploader-declared sensitivity class
  (`public|internal|restricted`) required at upload, quarantine state, PII
  detection explicitly out of scope for v1 (named future work). Risks now lists
  the residual exposure.
- **R1-R1 — embedding/geometry interaction unspecified** *(reviewer)*: §0
  async-embedding bullet now states embeddings stay **text-only**, geometry is
  not embedded and not ranked → no new model, "no new service" holds.
- **R1-R2 — FTS→citation path undefined** *(reviewer)*: §2 + §5 now specify
  **line-granularity** resolution (FTS/embedding candidate chunks → join all
  lines → flag query-overlapping lines; multi-line matches flag multiple lines).
- **R1-R3 — cross-page chunk policy conflicts with the chunker** *(reviewer)*:
  §1 decides **chunks may span pages** (heading chunker preserved;
  `page_start≠page_end` allowed; `open_neighbors` may cross pages; precision
  recovered at the line level).
- **R1-A1 — `blob_ref` storage unspecified** *(architect)*: §3 specifies a
  content-addressed filesystem blob store under `AIMEE_HOME`
  (`blob_ref`=`sha256`), called out as a **new storage surface scoped to Phase
  4**, and the "no new datastore" claim narrowed to *relational* stores.
- **R1-A2 — answerability signal had no contract** *(architect)*: §5-A defines
  `score∈[0,1]` + enum `{NONE,LOW,MEDIUM,HIGH}` with thresholds, deterministic
  inputs, and a reference test; optional standalone `/v1/answerability` noted as
  additive.
- **R1-D1 — `bbox` as TEXT forecloses spatial queries** *(data-model)*: changed
  to four `REAL` columns (`x0,y0,x1,y1`) on both tables + R*-tree on SQLite;
  normalization convention (top-left, per-page, clamped) fixed in §2.
- **R1-D2 — per-line volume unplanned** *(data-model)*: Data model now carries
  row estimates (~150M at 10k PDFs), the three composite indexes for the hot
  queries, and a **measure-before-default-on** gating criterion.
- **R1-R4 — `kb_doc_assets` keying/FK inconsistent** *(reviewer)*: keying
  standardized on logical `document_key`; `kb_doc_regions` gets `chunk_id` FK
  with `ON DELETE CASCADE`; assets cleanup made an explicit maintenance-path
  step (no single-document row exists to FK against — stated honestly).

**Non-blocking suggestions** — folded: producer-diversity + access-control tests
(§Tests), `content_type` on regions (§2), `open_neighbors` hierarchical mode +
the three escalation tools added to the server consumer (§5, Tenancy),
`--page N` on the CLI (§Surface), `doc_kind='pdf'` reuse (§1), bbox
normalization convention (§2), sidecar data-retention (§6). **Deferred with
rationale:** dropping `page_start/page_end` in favour of a derived view
(suggestion 1) — kept as a drift-proof write-time cache instead, since the join
cost on the hot `open_page` path is the thing we're avoiding; revisit if the
cache proves a maintenance burden.

### R2 (second review round)

A fresh four-lens pass over the R1 revision marked **all ten R1 findings CLOSED**
and confirmed the tenancy boundary still holds — with one exception that became
a new blocker (the blob store). Verdicts: reviewer / architect / data-model
**Conditional Pass**, security **Fail** (on R2-S1). Both new blockers folded:

- **R2-S1 — blob store bypasses document access control** *(security)*: a
  content-addressed crop readable by its `sha256` would sidestep `open_asset`'s
  check. §3 now states the `sha256` is **KB-internal, never returned to a
  client**, the blob dir is served by **no** file route, and `open_asset` (taking
  an opaque asset id + applying the `document_key` check) is the **sole** read
  path — so binary evidence is gated exactly as text is. Tests assert a by-hash
  fetch outside `open_asset` is denied.
- **R2-S2 — upload-time sensitivity enforcement undefined** *(security)*: §6 now
  specifies the `kb_client_docs.c` upload handler **requires** a valid
  `sensitivity_class` and **rejects** (4xx, no rows) when it is absent/invalid —
  no silent default — with the class persisted and propagated to regions/assets;
  Phase 1. A test covers the rejection.

**Non-blocking suggestions** — folded: chunk-size bound (~100 lines, split
preserving `heading_path`) + the large-chunk join load test (§1, §Tests),
heading-extraction fallback to page-boundary chunking with manual-review flag
(§1), configurable answerability thresholds with documented defaults (§5-A),
the explicit quarantine state machine + operator route (§6), the periodic
orphan-blob cleanup job + its integration test (§3). **Deferred with rationale:**
dropping the `quote` column to derive line text at query time — kept, because
`quote` is the natural, bbox-aligned join target for a citation and re-slicing
chunk `content` by `line_index` at query time would re-introduce the per-row work
the four `REAL` columns were chosen to avoid; the storage cost is accepted.
Region-level (sub-document) access control is noted as a future extensibility
path, not v1.

### R3 (third review round) — convergence

A third four-lens pass confirmed both R2 security blockers **CLOSED** (the
`sha256` never crosses the trust boundary; access rides on the opaque
`kb_doc_assets.id` + `document_key` check — and upload enforcement is a hard 4xx
with no silent default) and moved the lenses to **reviewer Pass / architect Pass
/ security Conditional Pass / data-model Pass**, with **no remaining blocking
findings** and an explicit **READY FOR SIGN-OFF**. Security's one residual — no
automated PII detection in v1 — is the documented v1 boundary (uploader-declared
class + access control), with scanning named as future work, judged "a
defensible v1 position, not a blocker."

The panel flagged three **execution-discipline** items for implementation (not
proposal defects, recorded so they aren't lost): (1) `sha256` log/error-message
hygiene so hashes don't leak into client-visible errors; (2) the eventual-
consistency window between chunk deletion and the maintenance-path asset/blob
cleanup (integration-tested, but operationally a brief orphan window); (3)
keeping asset-id access checks on `document_key` regardless of id guessability.
No further rounds are warranted — converged.

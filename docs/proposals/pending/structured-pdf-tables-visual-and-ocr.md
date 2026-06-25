# Proposal: structured-PDF Phase 3–4 — tables → typed facts, visual evidence, OCR, and retrieval-quality (vector + answerability)

- **State:** proposed — successor to the now-filed structured-PDF evidence
  proposal ([[structured-pdf-ingestion-and-evidence-layer]], in `done/`), which
  delivered Phases 1–2: text+geometry ingest, the access-controlled citation
  retrieval surface (`search_chunks` / `open_page` / `open_neighbors` /
  `inspect_structure`), §6 sensitivity + quarantine, and the four agent-callable
  MCP tools (#702/#706/#712/#715/#717/#720). This proposal carries the remaining
  scope the parent explicitly deferred as **deploy-tier**: table-structure
  recognition, visual crops + blob store, OCR fallback, and the retrieval-quality
  work (vector candidates + the §5-A answerability signal). Not yet implemented.
- **Why a separate proposal.** Every remaining item depends on infrastructure the
  parent's first two phases did not: **local ONNX sidecars** (table-structure
  recognition, OCR) deployed the way the embedder/reranker already are, a **new
  content-addressed blob filesystem** under `AIMEE_HOME`, the **embedder** wired
  to the PDF chunk path, and a **deployed KB with a real corpus** to measure
  retrieval quality and the per-line region row-count against the
  measure-before-default-on gate. None of it can be built or validated in a
  code-only environment, so it is split out rather than left as dangling
  "Phase 3–4" bullets on a shipped proposal.
- **Author:** JBailes
- **Date:** 2026-06-25
- **Charter roles:** Ingest (table regions + OCR text → the existing structured
  spine), Recall (cell-level + visual-crop retrieval over `/v1`), Curate (table
  cells promoted into the typed-fact layer), Calibrate (corpus-level
  answerability signal returned with `search_chunks`), Gate-Promote (per-sidecar
  capability flags + the row-count gate).
- **Scope — entirely aimee-KB-side, additive on the Phase 1–2 spine:** an
  optional TSR sidecar client and an optional OCR sidecar client (same
  call-out pattern as the embedder/reranker; `src/kb/kb_doc_pdf.{c,h}` +
  new sidecar client files), a new **`kb_doc_assets`** relational table + a
  **content-addressed blob store** under `AIMEE_HOME` with a maintenance-path
  orphan/blob cleanup job, two new read-only `/v1` endpoints (`lookup_table`,
  `open_asset`) and their MCP tools, the embedder wired to the PDF chunk path
  behind a doc_kind='pdf' + quarantine filter, and the §5-A answerability signal
  folded into the existing `search_chunks` response. **No new service, no new
  relational datastore beyond `kb_doc_assets`** (sidecars and the blob store
  follow existing optional-deploy patterns); table cells feed the **existing**
  typed-fact layer rather than a parallel store.

## §0 What Phases 1–2 already established (the foundation this rides)

From [[structured-pdf-ingestion-and-evidence-layer]] — do not rebuild:

- **The chunk + geometry spine.** `kb_documents` (`doc_kind='pdf'`,
  `chunk_strategy='page'`, `page_start`/`page_end`) and the per-line
  **`kb_doc_regions`** table (`page_no`, `x0/y0/x1/y1` normalized `[0,1]`,
  `quote`, `line_index`, `content_type`, denormalized `document_key`).
- **The access-controlled retrieval surface.** `search_chunks` and the three
  escalation reads, each gated by the `document_key`-level check and withholding
  `quarantine_state='pending'` documents; all four reachable as project-scoped
  MCP tools.
- **§6 sensitivity + quarantine.** Upload requires a `sensitivity_class`
  (`public|internal|restricted`); the class is propagated to every region row;
  `restricted → quarantine_state='pending'`; owner-only confirm/reject admin.
- **The tenancy boundary.** All PDF parsing, geometry, retrieval, and (now)
  table-facts/crops/OCR live in **aimee-kb**; the personal server holds only the
  thin `kb_client` calls. Answerability is a **KB-side, corpus-level** judgment
  and is **never** folded into the server's per-user confidence tier.
- **The security invariants** the three R-rounds settled (carried verbatim into
  §3–§4 below): blob access is gated and never by-hash; sidecars run inside the
  KB trust perimeter with no egress and no content retention; sensitivity
  filters ride the denormalized class with no join.

## §A Retrieval quality: vector candidates + the answerability signal

Phase 2 shipped `search_chunks` as **lexical-only** (case-insensitive content
match), and PDF chunks are deliberately **un-embedded** so they stay invisible to
the vector-only `/v1/search`. This phase completes the parent's §5 design.

- **A1 — Embed PDF chunks into a structurally-isolated PDF-vector relation
  (RT-S1).** Wire the existing `kb_async_jobs` (`kind='embed_raw'`) path to PDF
  chunks, but write the vectors to a **dedicated PDF-only vector relation with no
  general-vector-search read path** — *structural* isolation, not a runtime
  `WHERE doc_kind='pdf'` filter on the shared table. A query-time predicate is a
  data-classification filter, not an access-control boundary: a future code path
  that forgets it would leak withheld content. By giving PDF vectors their own
  relation that the general `/v1/search` transport never reads, the isolation is
  enforced by the schema/module boundary; the `quarantine_state<>'pending'`
  predicate then rides *inside* the PDF surface as defense-in-depth. This is the
  single load-bearing safety property and gets a direct adversarial regression
  test: a general-search caller (and a `pending` document) must be
  vector-unreachable by **every** path. **Engine-level backstop is required, not
  optional (RT2-S1):** the general-search code path connects under a
  **least-privilege DB role with no grant on the PDF-vector relation** (and/or a
  Postgres row-security policy on that relation), so the structural boundary is
  enforced by the database engine even if an application JOIN is mis-written — the
  app-layer separation and the engine-level privilege boundary are *both* present,
  neither alone is trusted.
- **A2 — Two-stage `search_chunks` (join semantics + ingest ordering, RT-C1).**
  Make retrieval the parent's intended shape: (1) FTS/lexical **and**
  text-embedding candidate retrieval over chunk `content`; (2) citation
  resolution against `kb_doc_regions`. The candidate→region join is a **LEFT
  JOIN** that surfaces a **`has_citation` flag** (and a null citation struct) so a
  candidate whose regions are not yet present degrades gracefully instead of being
  silently dropped (INNER) or returned as a bare confusing row. **Ingest ordering
  is a stated guarantee:** the ingest transaction commits `kb_doc_regions` before
  the chunk's `embed_raw` job is enqueued, so a vector-retrievable chunk always
  has its citations — and the LEFT-JOIN path is the backstop if that ever races.
  The **symmetric missing-embed case is not an atomicity violation (RT2-C1):**
  embedding is intentionally **async** (`kb_async_jobs`), so a chunk whose
  `embed_raw` fails or is still pending simply has regions-but-no-vector — it stays
  **fully lexically retrievable** with citations, and the job system **retries**
  the embed; no ingest transaction is left half-applied because the vector is never
  part of it. The response contract (chunks + citations) is otherwise unchanged and
  backward-compatible; lexical-only stays the graceful-degradation path when the
  embedder is absent. Observability: retrieval emits latency + region-miss-rate
  metrics, and the embed-job backlog/failure rate is already monitored.
- **A3 — Per-query answerability signal (§5-A contract; RT-C2).** `search_chunks`
  returns an answerability judgment with the **fixed contract** the parent
  specified: a `float score ∈ [0,1]` plus an enum `label ∈ {NONE, LOW, MEDIUM,
  HIGH}` (default thresholds `<0.15`→NONE, `<0.40`→LOW, `<0.66`→MEDIUM, else HIGH;
  config-overridable; defaults mirror the server's confidence tiers). **Naming
  correction (the parent called it "corpus-level," which is imprecise):** the
  signal is **per-query *over* the corpus** — "given *this* query, how well can
  the KB answer it" — so the same document scores differently across queries *by
  design*, and that is not a contradiction. Its inputs are split and documented so
  the value is reproducible: **query-scoped** components computed at search time
  (max FTS rank of the top-k hits, query-term coverage across matched chunks) and
  **corpus-scoped** components (presence of table-facts (§B) for query entities,
  FTS index health), combined by a **documented deterministic function with fixed
  default weights** (pinned by a reference test: query Q over fixture corpus C ⇒
  score ≥ 0.7, label HIGH). The invariant that actually matters for the tenancy
  boundary holds: it is computed **KB-side and stays a shared judgment** — it is
  **not** folded into the server's per-user confidence/steer tier, and the API
  keeps the two as distinct fields so clients cannot conflate them. A standalone
  `/v1/answerability?q=…` (signal-only) may follow as a purely additive endpoint.

## §B Phase 3 — tables → typed facts (`lookup_table`)

- **TSR sidecar.** An *optional* local table-structure-recognition model
  (ONNX-class, deployed like the embedder/reranker) converts table regions
  detected during ingest into structured cells, closing the loop with
  [[typed-fact-knowledge-layer]] (tables are the highest-yield typed-fact source
  in real documents).
- **Cell provenance + discriminator (RT-C3).** Reusing the typed-fact store is the
  goal, but table cells need provenance the generic fact row does not carry, and
  `lookup_table` must return *only* table cells — so each cell fact records: a
  **`source_type='table_cell'` discriminator** (indexed, so `lookup_table` filters
  to cells and never returns unrelated facts), its **(row, col) position**, the
  **TSR confidence**, and a link to the **source `kb_doc_regions` region** (and
  thus `document_key`/`page_no`/bbox). Whether that is a few additive columns on
  the typed-fact row or a thin `kb_table_cells` adjunct keyed to the fact + region
  is an implementation decision validated against the actual typed-fact schema
  during Phase 3; the **provenance contract above is the requirement**, the
  storage shape is not pre-committed. Cell facts remain first-class typed facts
  (entity-linkable, searchable) — the discriminator narrows, it does not wall them
  off.
- **Graceful degradation (must be tested, not assumed).** When the sidecar is
  absent, a table region degrades to a normal text chunk with geometry — still
  retrievable via `search_chunks`, just not cell-structured. The **user-visible
  contract is explicit, not silent (RT-sug):** `lookup_table` on a
  TSR-absent/region returns an empty cell set **with a `tsr_status` marker**
  (`ran` | `unavailable` | `not_a_table`) so a caller can tell "no cells" from
  "TSR never ran." The capability is behind its own deploy-time gate.
- **`lookup_table` endpoint + MCP tool.** A read-only `/v1` endpoint returning the
  structured cells for a table region (when TSR ran), surfaced as a project-scoped
  MCP tool alongside the four shipped ones. **Gating is the full parent ACL, not a
  parameter echo (RT-sug):** like every PDF read it applies the caller's auth
  context + the `document_key`-level access check (a guessed `document_key` for a
  document the caller cannot read returns empty), not merely a check that the
  request named a `document_key`.

## §C Phase 4 — visual evidence: crops + content-addressed blob store (`open_asset`)

- **`kb_doc_assets` table.** New relational table
  `(id, document_key TEXT, page_no INTEGER, x0/y0/x1/y1 REAL, kind TEXT,
  caption TEXT, blob_ref TEXT, created_at)`, index on `document_key`. **Cardinality
  + keying (RT-D1):** `document_key` is the logical document identity and the
  access-control key (no single-document row exists to FK against); one document
  has **N** asset rows. Access control filters on `document_key` exactly as chunks
  do.
- **Sensitivity is a denormalized filter, not the authority (RT-D1).** As with
  `kb_doc_regions` in the parent, the class is copied onto each asset row so
  retrieval filters without a join — but the **authoritative** control is the
  live `document_key`-level permission + `quarantine_state`, *not* the cached
  per-row class. A sensitivity change re-propagates to all of a document's region
  **and** asset rows through the **same maintenance path** that already
  re-propagates regions (bounded, documented lag); because the live ACL is what
  `open_asset`/`search_chunks` actually gate on, a momentarily-stale cached class
  is a filtering optimization, never a security hole. This keeps the asset table
  consistent with the parent's region denormalization rather than inventing a new
  rule.
- **Crop rendering.** Detected figure/table regions are rendered to image crops
  (poppler `pdftoppm`-class rasterization over the same operator-installed
  process boundary as `pdftotext` — no new linked dependency).
- **Content-addressed blob store (new filesystem surface).** Crops are binary and
  do not belong inline in the DB. They are written to a content-addressed store
  under `AIMEE_HOME` (one file per `sha256`); `blob_ref` is that `sha256`.
  Content-addressing gives free dedup across re-ingests. The location is
  **configurable** (defaults under `AIMEE_HOME`) so deployments are not pinned to
  one filesystem layout (RT-sug). This is an honestly-new storage dependency
  (filesystem, not relational).
- **Blob access is gated, never direct (R2-S1, carried + sharpened RT-S3).** The
  `sha256` is a **KB-internal identifier — never returned to a client, never in a
  URL/log/error** — and the blob directory is served by **no** static/file route.
  The sole read path is **`open_asset`**, whose handle is explicitly the **opaque
  `kb_doc_assets.id` (the row id — *never* sha256-derived);** it applies the
  caller's **auth context + the `document_key`-level access check** (the same ACL
  every PDF read uses, not a bare id lookup) and an **access audit log entry**
  (success/failure) before streaming bytes. The id being unguessable is *defense
  in depth* — the `document_key` ACL is the authority regardless of id
  guessability (R3 execution item #3). A caller cannot construct a path or fetch a
  crop by knowing a hash. **Engine-level backstop (RT2-S3):** since the live ACL is
  the primary control, `kb_doc_assets` also carries a **row-security policy /
  least-privilege role** so a query that omits the application check still cannot
  read another tenant's asset rows — the same both-layers posture as the
  PDF-vector relation (§A1). The audit log is append-only and stored under a role
  distinct from the asset rows it records.
- **Cross-sensitivity dedup is safe (RT-S2 clarification).** Two documents that
  contain the *byte-identical* crop share one blob, but each has its **own
  `kb_doc_assets` row** carrying its **own `document_key` + sensitivity**, and
  `open_asset` checks the *row*, never the blob — so dedup never widens access
  (the bytes are identical by definition of content-addressing; there is nothing
  "more sensitive" to leak). A blob persists as long as *any* row references it,
  which is correct: a public document legitimately keeps showing its crop after a
  restricted document with the same crop is purged.
- **Orphan + blob lifecycle (atomicity + ordering, RT-S2).** Writes are ordered so
  the failure modes are safe: the **blob is written and fsync-durable *before* its
  `kb_doc_assets` row is inserted**, so a crash can only ever leave an *orphan
  blob* (harmless, reclaimed by cleanup) — never a row pointing at a missing blob.
  Deletion is **not** a relational cascade: when a document's chunks are
  deleted/re-ingested, the same maintenance path deletes its asset rows first,
  then a periodic reconciliation job scans `kb_doc_assets` for `blob_ref` values
  **no row references** and unlinks those files (refcount-by-scan, so shared blobs
  survive until the last referrer is gone). The brief eventual-consistency window
  between row deletion and blob unlink (R3 execution item #2) is a *bounded orphan*
  window with no correctness or access impact, and is integration-tested.
  **Cadence + alarm are specified, not left implicit (RT2-perf):** the
  reconciliation job runs on a **configurable interval** (default hourly) and can
  also be triggered on demand; an **orphan-bytes alarm** (configurable threshold)
  fires if reclaimable bytes grow unbounded — so a lagging or failing sweep under
  crash load is observable and bounded, not a silent path to storage exhaustion.

## §D OCR fallback (scanned / image-only PDFs)

For PDFs with no/low extractable text layer, an *optional* local OCR sidecar
produces text + per-line geometry, feeding the **same §1/§2 ingest path** as
native text (so citations work identically). Detection is "page has no/low text
layer → OCR." OCR availability is a deploy-time capability, not a hard
dependency: without it, image-only PDFs ingest as **asset-only** documents (crops
from §C, no text chunks).

- **OCR-absent is a documented limitation, not a silent drop (RT-sug).** A scanned
  PDF ingested without OCR yields **no searchable text and no quote-bearing
  citations** — it is reachable only as visual crops via `open_asset`. The upload
  path **surfaces this** (an asset-only ingest is reported, not silently
  accepted as if text-indexed), so an operator is never misled into thinking a
  scanned contract is searchable. Re-ingesting once OCR is deployed upgrades it
  in place (origin-artifact rule).
- **Subprocess hardening (RT-S4).** Crop rasterization (`pdftoppm`-class) and the
  OCR sidecar run over the **same hardened operator-process harness shipped in
  Phase 1b** for `pdftotext`: a separate process across the boundary (aimee never
  links/bundles it), child `RLIMIT_CPU/AS/FSIZE`, a wall-clock deadline + output
  byte-cap, `setsid` + process-group kill on timeout, and `0600 mkstemp` scratch
  unlinked on every path. `pdftoppm` is a *different* binary than `pdftotext` and
  is treated as its own untrusted subprocess (its own rlimits/deadline), not
  assumed-safe by analogy. Both sidecars run **inside the KB trust perimeter** with
  **no external network** and **no document-content retention** after processing.
- **The byte-consuming PDF parser *is* the threat surface (RT2-S4).** The
  components that consume the user-supplied PDF bytes — `pdftotext` (Phase 1),
  `pdftoppm` (crop rasterization), and the OCR sidecar's image decoder — are
  exactly the CVE-rich parsers, and they are exactly what the hardened harness
  wraps; there is no *separate* "trusted parser" upstream of them. Hardening is
  named concretely: `RLIMIT_AS` is tuned to bound **in-memory decompression**
  (PDF/image compression bombs) *before* output is written, the byte-cap bounds
  **subprocess stdout + scratch output**, and scratch is unlinked on **every**
  exit path including signal/group-kill. A **syscall-filter profile**
  (seccomp-bpf, or the platform sandbox equivalent) on these subprocesses is a
  named hardening upgrade to the shared harness — evaluated as part of this phase,
  since this phase adds two more untrusted parsers to it.

## Security (carried from the parent's R1–R3; the deltas this phase introduces)

The phase introduces three genuinely-new attack surfaces; each inherits a settled
control:

1. **Vectorized PDF content (§A1).** PDF vectors live in a **dedicated relation
   the general vector search never reads** — *structural* isolation, with the
   `quarantine_state<>'pending'` predicate as defense-in-depth inside the PDF
   surface (a runtime filter alone was rejected as a classification, not an
   access, boundary). Tested adversarially: a general-search caller and a
   `pending` doc are vector-unreachable by every path.
2. **Binary crops in a content-addressed store (§C).** `open_asset` — keyed on the
   opaque **`kb_doc_assets.id` row id (never sha256)**, applying the caller's auth
   context + the live `document_key` ACL + an audit-log entry — is the **sole**
   gated read path; the `sha256` never crosses the trust boundary, appears in no
   URL/log/error, and no file route serves the blob dir (R2-S1). Content-addressed
   dedup does not widen access (gating is on the per-document asset row, not the
   shared bytes).
3. **Two new untrusted subprocesses/sidecars (§B, §D).** TSR + OCR + the
   `pdftoppm` rasterizer run over the **Phase-1b hardened-exec harness** (separate
   process, `RLIMIT_CPU/AS/FSIZE`, deadline + byte-cap, group-kill, 0600 scratch),
   **inside the KB trust perimeter**, with **no external network** and **no
   document-content retention**. They are not a data-egress path.

PII posture is unchanged from the parent: access control + uploader-declared
sensitivity is the v1 control; automated PII detection remains named future work.

## Data model (additive)

- **new `kb_doc_assets`** as above — the one new relational table; the blob store
  is filesystem, not relational.
- **table cells** are written through the **existing** typed-fact / entity store,
  tagged with the `source_type='table_cell'` discriminator (§B) on an indexed
  column so `lookup_table` is index-served and table-cell facts are **isolatable**
  (partition / dedicated index by `source_type` or `document_key`) to bound their
  blast radius on the shared store — no new *fact* table, but the cells are not
  allowed to silently bloat the general fact indexes (RT-perf).
- **`kb_documents` / `kb_doc_regions` / `kb_fts` / `kb_async_jobs`:** unchanged;
  PDF chunks now additionally enqueue `embed_raw` (§A) and may spawn asset rows
  (§C).
- **Row-count gate — threshold + runtime behavior (RT-perf).** Per-line region
  volume (~150M rows at a 10k-PDF corpus) must be **measured on the target corpus
  and a partition/archive decision taken before default-on** — a hard gating
  criterion this phase inherits. The gate is now given *behavior*, not just a
  measure step: (a) a configured row/size threshold that **blocks default-on
  promotion** until a partition/archive plan is in place; and (b) a per-ingest
  budget so that when a deployment is over budget, table-sidecar/region expansion
  **falls back to text-only ingest** (a logged degradation) rather than unbounded
  growth. Hot queries (`open_page`, the in-chunk citation join, the table-cell
  filter) stay index-served by the composite indexes from the parent plus the
  `source_type` index above.

## Surface

- KB `/v1` (read-only, additive): `lookup_table`, `open_asset`; the
  answerability fields on the existing `search_chunks` response.
- MCP: `pdf_lookup_table`, `pdf_open_asset` (project-scoped, registered the same
  3-site way as the four shipped PDF tools — note the golden tool-surface test
  must be regenerated).
- Ingest: the existing PDF upload path additionally renders crops and runs
  TSR/OCR when those sidecars are present; behaviour is unchanged when absent.
- No new server-side logic beyond the existing thin `kb_client` calls.

## Phasing (each independently shippable, default-off)

1. **§A retrieval quality** — embed PDF chunks behind the access filter; two-stage
   `search_chunks`; the answerability signal. (Needs the embedder; no new sidecar.)
2. **§B tables** — TSR sidecar → typed-fact cells + `lookup_table`; text-chunk
   degradation when absent.
3. **§C visual** — `kb_doc_assets` + blob store + `open_asset` + orphan cleanup.
4. **§D OCR** — OCR sidecar for scanned PDFs; asset-only fallback when absent.

## Flags

Default-off behind the existing `kb.pdf_ingest_enabled` plus **per-sidecar
capability gates** for TSR and OCR and a gate for §A vector retrieval. Each
capability degrades gracefully to the Phase 1–2 behaviour when its dependency is
absent, and that degradation path is tested, not assumed.

## Non-goals

- Replacing embeddings or the existing vector search. §A *adds* a PDF-scoped
  vector path; it does not touch the general halfvec/HNSW path.
- Automated PII detection/redaction (still future work).
- Region-level (sub-document) access control — noted by the parent as a future
  extensibility path, not in scope here.
- Layout-preserving translation, discovery feeds, or any per-user document logic
  on aimee-server (all parent non-goals, restated).

## Risks / honest limits

- **Sidecar deploy surface.** TSR + OCR add two optional local models to the
  deploy matrix; both must degrade to "text + geometry only" / "asset only" when
  absent, tested across the deploy matrix.
- **Blob-store operability.** A new filesystem surface with its own
  orphan/dedup/cleanup lifecycle and an eventual-consistency unlink window;
  designed in §C, must be integration-tested, not assumed.
- **Vector access leak.** The single most dangerous new property is PDF-vector
  isolation. §A1 makes it **structural** (a dedicated relation the general search
  never reads) precisely because a runtime predicate could be dropped by a future
  code path; the `quarantine_state` predicate is defense-in-depth. Both layers get
  a direct, adversarial regression test.
- **Geometry volume at corpus scale.** The row-count gate (carried) becomes a
  hard, measured promotion criterion in this phase.
- **Extraction quality variance** (multi-column, dense-math, OCR noise) — same as
  the parent; mitigated by retaining the origin PDF for re-extraction.

## Tests

- Unit: TSR-absent / OCR-absent / embedder-absent degradation paths;
  answerability score/label thresholds against a fixture corpus (and that the
  per-query split yields the same score for the same (query, corpus) state);
  crop content-addressing + dedup; the §A2 LEFT-JOIN `has_citation` path
  (candidate with missing regions degrades, not dropped); the §B `tsr_status`
  marker and `source_type='table_cell'` discriminator (lookup_table returns only
  cells); asset-row sensitivity re-propagation on a class change.
- Access control (the load-bearing suite): a general-search caller **cannot reach
  PDF vectors** and a `pending`/quarantined document is un-retrievable by
  **vector** as well as lexical (adversarial, §A1); a by-`sha256` fetch outside
  `open_asset` is denied (hash never exposed, no file route); `open_asset` keyed
  on a guessed/foreign `kb_doc_assets.id` for an unreadable document returns empty;
  a caller without document read gets empty `lookup_table` / `open_asset`.
- Lifecycle: blob-write-before-row-insert ordering (a crash leaves only an orphan
  blob, never a dangling row); the reconciliation job unlinks only
  zero-referrer blobs (a shared/deduped blob survives until its last referrer is
  deleted).
- Integration: ingest a table PDF → TSR cells land as typed facts → `lookup_table`
  round-trip; ingest a figure PDF → crop in blob store → `open_asset` round-trip →
  document delete dereferences and unlinks the blob; ingest a scanned PDF with OCR
  on → text chunks + citations, with OCR off → asset-only (and the asset-only
  status is surfaced, not silent).
- Deploy-matrix: PDF ingest with and without each of {embedder, TSR, OCR}.

## Relationship to other proposals

- Supplies the high-yield **table → typed-fact** source for
  [[typed-fact-knowledge-layer]].
- Completes the **coordinate-anchored** evidence (cells + crops + OCR geometry)
  that [[auditable-correctness-for-the-kb]] binds provenance to.
- Direct successor to [[structured-pdf-ingestion-and-evidence-layer]] (done); all
  R1–R3 review history and the §0–§6 foundations live there.

## Review revisions (RT1)

A four-lens delegate roundtable (reviewer / architect / security-privacy /
data-model) over the first draft returned **9 blocking findings**; all are folded
in here. (The panel still lacks a codex seat server-side — a known limitation;
codex review applies to code diffs via `/code-review`, not proposal text.)

- **RT-S1 — pgvec filter is classification, not access control** *(security)*:
  §A1 now mandates a **dedicated PDF-vector relation with no general-search read
  path** (structural isolation), with the `quarantine_state` predicate as
  defense-in-depth and an adversarial regression test; the runtime-filter-only
  design is explicitly rejected.
- **RT-S2 — blob lifecycle atomicity + cross-sensitivity dedup** *(security)*: §C
  now states blob-durable-before-row-insert ordering (a crash can only orphan a
  blob, never dangle a row), refcount-by-scan cleanup, and that content-addressed
  dedup cannot widen access because gating is on the per-document asset row, not
  the shared bytes.
- **RT-S3 — `open_asset` identity/audit + opaque-id definition** *(security)*: the
  handle is defined as the **`kb_doc_assets.id` row id (never sha256-derived)**;
  `open_asset` applies the caller auth context + the live `document_key` ACL + an
  access audit-log entry; id-unguessability is defense-in-depth only.
- **RT-S4 — sidecar isolation under-specified** *(security)*: §D pins TSR/OCR and
  the `pdftoppm` rasterizer to the **Phase-1b hardened-exec harness** (rlimits,
  deadline, byte-cap, group-kill, 0600 scratch), each treated as its own untrusted
  subprocess.
- **RT-C1 — two-stage join semantics + ingest ordering** *(correctness)*: §A2
  specifies a **LEFT JOIN + `has_citation` flag** and a regions-before-embeddings
  commit guarantee.
- **RT-C2 — answerability "corpus-level" vs query-level** *(correctness)*: §A3
  reframed as an explicitly **per-query-over-corpus** signal with split
  query/corpus inputs and a documented deterministic combiner; the only invariant
  asserted is that it stays KB-side and is not the server's per-user tier.
- **RT-C3 — table-cell provenance + discriminator** *(correctness)*: §B requires a
  `source_type='table_cell'` discriminator, (row,col) position, TSR confidence,
  and a source-region link, so `lookup_table` returns only cells and provenance is
  preserved; the storage shape (columns vs adjunct table) is left to
  implementation against the real typed-fact schema.
- **RT-Perf — row-count gate behavior + typed-fact bloat** *(performance)*: the
  gate now has a threshold that **blocks default-on** and an over-budget
  **text-only ingest fallback**; table-cell facts are isolated (partition/index by
  `source_type`) so they cannot bloat the general fact indexes.
- **RT-D1 — denormalized sensitivity staleness + cardinality** *(data-model)*: §C
  states `document_key` is the N-assets access key, the cached per-row class is a
  filter (the live ACL is authority), and a class change re-propagates via the
  same maintenance path the parent uses for regions.

**Suggestions folded:** explicit `tsr_status` / asset-only-ingest markers (no
silent degradation), `lookup_table` full-ACL gating (not a parameter echo),
configurable blob-store location, and the distinct answerability-vs-confidence API
fields. Residual (non-blocking): automated PII detection remains the documented v1
boundary, inherited from the parent.

### Review revisions (RT2)

A second round confirmed the RT1 fixes and pushed on engine-level enforcement and
operability; four further hardenings are folded in:

- **RT2-S1 / RT2-S3 — structural isolation needs an engine-level privilege
  boundary, not just module discipline** *(security)*: §A1 (PDF-vector relation)
  and `open_asset`/`kb_doc_assets` (§C) now **require** a least-privilege DB role
  and/or row-security policy as the engine-level backstop to the app-layer ACL —
  both layers present, neither alone trusted.
- **RT2-perf — orphan-blob cleanup cadence/alarm** *(performance/ops)*: §C names a
  configurable sweep interval (default hourly) + on-demand trigger + an
  orphan-bytes alarm, so a lagging sweep is observable and bounded, not a silent
  storage-exhaustion path.
- **RT2-C1 — symmetric missing-embed case** *(correctness)*: §A2 states embedding
  is async by design, a failed/pending embed leaves a chunk lexically retrievable
  (with citations) and is retried by `kb_async_jobs` — no ingest atomicity is
  violated because the vector is never part of the ingest transaction.
- **RT2-S4 — the byte-consuming parser is the threat surface** *(security)*: §D
  states `pdftotext`/`pdftoppm`/the OCR image decoder *are* the untrusted parsers
  (no separate trusted parser exists), tunes `RLIMIT_AS` against decompression
  bombs, bounds stdout+scratch, unlinks scratch on every exit, and names a
  seccomp-bpf/sandbox syscall-filter profile as a harness upgrade evaluated in
  this phase.

**Methodology note.** The roundtable reviews the *brief*, not the full proposal
text, so several RT2 items asked for detail already present in the body (the
combiner spec, the `source_type` index shape, the `document_key` denormalization)
or for implementation-tier specifics (audit-log WORM/hash-chaining, exact
thresholds, multi-UID scratch perms under Kubernetes) that are correctly settled
at build time against the real schema and deploy target, not pre-committed in a
design proposal. Those are recorded as **execution-discipline items** for the
implementation, not proposal blockers. With the four engine-level/operability
hardenings above folded, the design has **converged**: the security posture is
both-layers (structural + engine-level), every degradation path has an explicit
contract, and the residual is the parent's documented PII boundary.

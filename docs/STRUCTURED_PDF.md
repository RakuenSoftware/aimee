# Structured PDF: coordinate-anchored evidence, tables, crops, and OCR

aimee-kb can ingest a PDF as **coordinate-anchored evidence** instead of a flat
blob of text. Every retrieved snippet carries the page and bounding box it came
from, so an answer can be traced back to the exact place on the page. On top of
that spine the KB optionally recognises **table cells**, renders **visual crops**
of figures/tables/pages, and **OCRs scanned pages** — each behind its own
capability flag, each degrading cleanly to the layer below when its dependency is
absent.

Everything here lives entirely in **aimee-kb** (the personal `aimee-server` holds only
thin `kb_client` calls). It is **off by default**: a PDF upload is handled by the legacy
flat-text path until you enable the structured extractor, and each higher layer is a
separate opt-in.

---

## What you get

| Layer | Capability | Surfaced as |
|-------|-----------|-------------|
| **Spine** | Text + per-line geometry; page-boundary chunks; line-level `{page_no, bbox, quote}` citations | `pdf_search_chunks`, `pdf_open_page`, `pdf_open_neighbors`, `pdf_inspect_structure` |
| **§A retrieval** | Vector candidates over an **isolated** PDF-only embedding relation + per-query **answerability** signal | the same `pdf_search_chunks` (now lexical **and** vector), with `score` / `matched_via` / `answerability` |
| **§B tables** | Table-structure recognition → structured `{row, col, text, confidence}` cells | `pdf_lookup_table` |
| **§C visual** | Figure/table/page crops in a content-addressed blob store | `pdf_list_assets`, `pdf_open_asset` |
| **§D OCR** | Scanned / image-only pages → text + geometry through the **same** citation path | When OCR recovers text it is transparent (the spine surfaces above carry it; upload reports `text_layer='ocr'`); when OCR is off/absent the upload reports `text_layer='none'` + `asset_only` |

Every read surface honours the **same access control** as the spine
(`document_key`-level check + quarantine), and table cells / crops are gated at read
time by a join to the authoritative document — never by a cached flag. Structured-PDF
content is reachable **only** through these `pdf_*` tools — it is deliberately kept out
of the general `/v1/search` (both the vector path and the convention/derived-artifact
sweeps), so a PDF cannot leak into an unscoped search.

---

## Enabling it

Each capability is a config flag (set with `aimee config set <key> <value>` or in
`aimee.yaml`); the recognition layers also need a local sidecar or poppler binary.
**Defaults are off.** Two kinds of dependency: the **poppler binaries**
(`pdftotext`, `pdftoppm`) are *hard* — if a flag that needs one is on but the binary
is missing, that step **errors** (e.g. a PDF upload returns an extraction error)
rather than silently degrading; the **sidecars/embedder** are *soft* — when absent
the feature degrades to the layer below (see [Degradation](#degradation)).

```bash
aimee config set kb_pdf_ingest_enabled true
aimee config set kb_pdf_vector_enabled true
aimee config set kb_pdf_tsr_enabled    true
aimee config set tsr_command           https://tsr.local:9000/recognize   # an HTTP URL
aimee config set kb_pdf_assets_enabled true
aimee config set kb_pdf_ocr_enabled    true
aimee config set ocr_command           https://ocr.local:9000/recognize   # an HTTP URL
```

| Flag | Default | What it turns on | Needs |
|------|---------|------------------|-------|
| `kb_pdf_ingest_enabled` | off | Route `.pdf` uploads through the structured extractor (geometry spine) instead of flat `pdftotext` | `pdftotext` (poppler) |
| `kb_pdf_vector_enabled` | off | Embed PDF chunks into the isolated `kb_pdf_embeddings` relation + add the vector leg to `search_chunks` | the deployment's embedder |
| `kb_pdf_tsr_enabled` + `tsr_command` | off | Run the table-structure-recognition sidecar at ingest → `kb_table_cells` | a TSR sidecar (`AIMEE_TSR_URL` env fallback) |
| `kb_pdf_assets_enabled` | off | Render figure/table/page crops to the blob store + `kb_doc_assets` | `pdftoppm` (poppler) |
| `kb_pdf_ocr_enabled` + `ocr_command` | off | OCR scanned / no-text-layer PDFs and ingest the text + geometry through the normal path | an OCR sidecar (`AIMEE_OCR_URL` env fallback) |

Blob-store / reconciliation knobs (Phase §C):

| Flag | Default | Meaning |
|------|---------|---------|
| `kb_pdf_blob_dir` | `<kb-config-dir>/kb-blobs` | Blob store root override (`<kb-config-dir>` is the aimee-kb config dir under `AIMEE_HOME`) |
| `kb_pdf_blob_recon_secs` | `3600` | Orphan-blob reconciliation sweep interval (`<=0` disables) |
| `kb_pdf_blob_orphan_alarm_mb` | `1024` | Warn when reclaimable orphan blob bytes exceed this many MB (`<=0` disables) |

A typical full-feature deployment sets all five capability flags on and points
`tsr_command` / `ocr_command` (or `AIMEE_TSR_URL` / `AIMEE_OCR_URL`) at the local
sidecars, with `pdftotext` + `pdftoppm` installed in the aimee-kb image.

### The sidecars

TSR and OCR are **optional local HTTP sidecars**, deployed the same way as the
embedder/reranker — a service aimee-kb POSTs to. Despite the `_command` suffix
(inherited from `embedding_command`), `tsr_command` / `ocr_command` take the
sidecar's **HTTP URL**, not a binary path (the `AIMEE_TSR_URL` / `AIMEE_OCR_URL`
env vars are the fallback).

These sidecars must be **deployed inside the KB trust perimeter** — the operator runs
them with no external network and no document-content retention. That is a deploy-time
obligation, not something aimee enforces at runtime: aimee-kb hands the sidecar
document-derived content (page text/geometry, or a rendered page image), so a sidecar
that egresses or retains is a data-exposure path. aimee never links or bundles them.

`pdftotext` and `pdftoppm` are operator-installed poppler binaries run as hardened
subprocesses (separate process, `RLIMIT_CPU/AS/FSIZE`, a wall-clock deadline + output
byte-cap, `setsid` + process-group kill on timeout, and `0600` scratch unlinked on
every path — `RLIMIT_AS` is what bounds an image/decompression-bomb before output is
written). aimee never links or bundles them either.

---

## Uploading

`POST /v1/kb/docs` (multipart) with `file`, a `scope` (project), and — **required** for
a PDF under `kb_pdf_ingest_enabled` — a `sensitivity_class` of `public`, `internal`, or
`restricted` (a PDF upload with a missing/invalid class is a 400, no rows written). A
`.pdf` whose bytes are not a real PDF (no `%PDF-` header) is rejected. A document's
identity is `(scope, document_key)` where `document_key` is its file path, so
re-uploading under the same scope + name **replaces** the prior version in place
(re-extraction; the layers re-derive).

A `restricted` document is **quarantined** (`quarantine_state='pending'`, withheld from
every read surface) until an **owner** releases it:

```
POST /v1/pdf/quarantine   { "project": "...", "document_key": "...",
                            "action": "confirm" }   # release (now retrievable)
                                  # or "reject"     # purge the document + its derived rows
```

Owner authorization is enforced before the handler runs; the response reports the
number of pending chunks acted on.

The upload response reports what was ingested, including the §D status:

```json
{ "doc_kind": "pdf", "chunks": 12, "regions": 480, "assets": 12,
  "text_layer": "native", "asset_only": false, "sensitivity_class": "internal" }
```

- `text_layer` ∈ `native` (real text layer) | `ocr` (recovered by the OCR sidecar) |
  `none` (scanned, no text recovered).
- `asset_only` is `true` when a scanned PDF yielded **no text** but **did** yield
  visual crops — so an operator is never misled into thinking a scanned contract is
  text-searchable. Re-uploading once OCR is deployed upgrades it in place.

---

## Retrieval surface

Read-only, access-gated. Reachable over `/v1` and as project-scoped MCP tools (the
caller's token scope is enforced before dispatch; restricted/quarantined documents
are withheld).

| MCP tool | `/v1` route | Returns |
|----------|-------------|---------|
| `pdf_search_chunks` | `GET /v1/pdf/search` | Matching chunks with line-level `{page_no, bbox, quote}` citations, a relevance `score`, `matched_via` (`lexical`\|`vector`), `has_citation`, and a per-query `answerability` object |
| `pdf_open_page` | `GET /v1/pdf/page` | Every region on one page, ordered by line |
| `pdf_open_neighbors` | `GET /v1/pdf/neighbors` | The prev/next reading-order chunks of a hit |
| `pdf_inspect_structure` | `GET /v1/pdf/structure` | The document's chunk/page outline |
| `pdf_lookup_table` | `GET /v1/pdf/lookup_table` | Recognised table cells `{row, col, text, tsr_confidence}` + a `tsr_status` marker (`ran` \| `not_a_table` \| `unavailable`) |
| `pdf_list_assets` | `GET /v1/pdf/assets` | A document's visual crops — each an **opaque `asset_id`** + page/bbox/kind/caption |
| `pdf_open_asset` | `GET /v1/pdf/open_asset` | One crop's image bytes (base64) for an opaque `asset_id` |
| — | `POST /v1/pdf/quarantine` | Owner-only: confirm (release) or reject (purge) a pending restricted document |

### Answerability

`pdf_search_chunks` returns a per-query-over-corpus **answerability** judgment —
"given *this* query, how well can the KB answer it" — as a `score ∈ [0,1]` plus a
`label ∈ {NONE, LOW, MEDIUM, HIGH}` (default cutoffs `<0.15`→NONE, `<0.40`→LOW,
`<0.66`→MEDIUM, else HIGH; config-overridable). It is a **KB-side shared signal**,
deliberately a
*distinct field* from any server-side per-user confidence tier — clients must not
conflate the two. The combiner is a documented deterministic function of query-scoped
inputs (top relevance, query-term coverage, hit saturation) so the same `(query,
corpus state)` always yields the same score.

---

## Access control & security

- **Sensitivity + quarantine.** Every upload carries a `sensitivity_class`; it is
  stamped on each chunk/region/cell/asset row. `restricted` documents are withheld
  until an owner confirms them. Region-level (sub-document) access control is out of
  scope; the `document_key` is the access unit.
- **PDF vectors are structurally isolated.** PDF chunk embeddings live in a
  **dedicated `kb_pdf_embeddings` relation that the general `/v1/search` transport
  never reads** — isolation by schema/module boundary, not a runtime `WHERE` filter
  (which would be a classification, not an access, boundary). A `quarantine_state`
  predicate rides inside the PDF surface as defense-in-depth.
- **Table cells and crops gate on the live document.** `lookup_table`,
  `open_asset`, and the asset list resolve through a join to the authoritative
  `kb_documents` row (`doc_kind='pdf'` + quarantine + project, bound to the real
  `file_path`) — a guessed/foreign/withheld key returns empty, never the cached
  denormalised value.
- **The blob store never exposes the hash.** Crops are content-addressed by sha256
  (free dedup), but the sha is a **KB-internal identifier**: never returned to a
  client, never in a URL/log/error, and the blob directory is served by **no** file
  route. The sole read path is `open_asset`, keyed on the **opaque `kb_doc_assets`
  row id** (never the hash), which applies the document ACL and writes a structured
  access audit log entry (allowed/denied) before streaming bytes. (Today that entry
  is a structured log line; a durable/WORM, hash-chained audit store is a tracked
  enhancement — see the limits below.) Content-addressed dedup never widens access —
  gating is on the per-document asset row, not the shared bytes.
- **Sidecars are not an egress path.** TSR/OCR and `pdftoppm` run inside the
  perimeter with no external network and no content retention.

PII posture: access control + uploader-declared sensitivity is the control;
automated PII detection is not in scope.

---

## Degradation

Each layer is independent. A **flag off** falls back to the layer below; a **soft
dep** (embedder/sidecar) absent degrades; a **hard dep** (poppler binary) absent
errors that step.

| Condition | Result |
|-----------|--------|
| `kb_pdf_ingest_enabled` off | PDF goes through the legacy flat-text path (no geometry) |
| `kb_pdf_ingest_enabled` on, `pdftotext` **absent** | The PDF upload returns an extraction error (422); no rows written (hard dep) |
| `kb_pdf_vector_enabled` off, or embedder absent | `search_chunks` is lexical-only (still cited) |
| `kb_pdf_tsr_enabled` off, or TSR sidecar unreachable | Table regions stay normal text chunks; `lookup_table` reports `tsr_status='unavailable'` |
| `kb_pdf_assets_enabled` off | No visual crops; text retrieval unaffected |
| `kb_pdf_assets_enabled` on, `pdftoppm` **absent** | No crops are produced (rendering fails best-effort); text/ingest unaffected (hard dep, but non-fatal here) |
| `kb_pdf_ocr_enabled` off (or OCR sidecar unreachable) on a scanned PDF | Ingested **asset-only** if crops were rendered (`text_layer='none'`, `asset_only=true`); otherwise text-less with no crops (`text_layer='none'`, `asset_only=false`) |
| `kb_pdf_ocr_enabled` on, sidecar recovers text | Text + geometry ingest transparently — same chunks/regions/citations as native text (`text_layer='ocr'`) |

---

## Operational notes & current limits

These ship today but are **gated to default-off** pending deploy-tier validation:

- **Sidecar/binary deploys.** The TSR + OCR models and the poppler binaries are
  deployed on the aimee-kb image; recognition/render quality is validated on the
  deploy target (the code paths degrade safely when they are absent, but their
  output is not exercised in code-only CI).
- **Corpus-scale geometry volume.** Per-line `kb_doc_regions` (and `kb_table_cells`)
  volume grows with the corpus; measure it on the target corpus and take a
  partition/archive decision **before** flipping a capability default-on.
- **Engine-level privilege boundary.** The in-code structural isolation of
  `kb_pdf_embeddings` / `kb_doc_assets` is the load-bearing control; a hardened
  deploy additionally provisions a least-privilege DB role and/or a row-security
  policy on those relations (both layers present, neither alone trusted).
- **`open_asset` transfer.** Crops are returned inline as base64 with a size cap; a
  binary-streaming path for very large crops, and a durable/WORM access-audit store,
  are tracked enhancements.
- **Re-ingest is the upgrade path.** Re-uploading a document once a higher layer
  (OCR, TSR) is deployed upgrades it in place; the original PDF is retained so
  extraction can be re-run.

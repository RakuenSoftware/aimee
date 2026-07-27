# Structured PDF

Structured PDF ingestion keeps evidence that plain text extraction loses: page coordinates, reading
order, tables, visual regions, OCR, and page-linked citations.

## Layers

The base spine stores page text and coordinates. Optional layers add:

| Layer | Setting | Result |
| --- | --- | --- |
| dense PDF retrieval | `kb_pdf_vector_enabled` | isolated PDF vectors and answerability signal |
| table recognition | `kb_pdf_tsr_enabled` | cells and table lookup |
| visual assets | `kb_pdf_assets_enabled` | content-addressed page/figure/table crops |
| OCR | `kb_pdf_ocr_enabled` | scanned-page text through the same citation path |

Each layer has its own dependency and failure state. Enabling assets does not imply OCR or tables.
Use [generated configuration](gen/configuration.md) for current sidecar commands and bounds.

## Upload

```bash
aimee kb docs push ./document.pdf
aimee kb ingest status
```

The thin client uploads bytes. The KB validates type, size, scope, and content hash before commit.
Temporary extraction files stay outside the corpus and are removed after the bounded job.

## Evidence model

A cited span keeps:

- document and content hash;
- page number;
- bounding box and reading order;
- extracted or OCR text;
- table/cell identity where present;
- linked asset digest where present;
- extractor/model identity and confidence.

Text, table, and image records point back to the same immutable source version. Re-ingesting changed
bytes creates a new version; it does not move old coordinates under a new file.

## Retrieval

Normal document search can return PDF spans. Structured tools add table lookup and guarded asset
opening. Answerability states whether the retrieved PDF evidence is strong enough for the question;
it does not manufacture an answer when coverage is weak.

Assets are opened through an access-checked, audited route. A result never returns an arbitrary
filesystem path.

## Degradation

| Missing dependency | Behavior |
| --- | --- |
| vector backend | coordinate/lexical evidence remains; dense signal marked absent |
| table recognizer | page text remains; no table structure claim |
| renderer | text remains; no asset URL |
| OCR | born-digital text remains; scanned page may be asset-only |
| synthesis | evidence returns without generated answer |

A stage error remains visible in ingest status and can be retried. The base document should not be
discarded because one optional layer failed.

## Security

PDFs are untrusted input. Extraction runs with body, page, pixel, time, memory, process, and output
bounds. External sidecars receive only their required file/version and no provider or database
credentials.

OCR text and visual assets inherit the document's scope. Asset IDs are content-addressed but not
public. Every open checks the current principal and writes an audit record.

## Operations

- Keep source PDFs until derived evidence has passed verification.
- Monitor queue age, page failures, OCR rate, asset bytes, and vector backfill.
- Rebuild derived layers after extractor or embedding changes; do not rewrite source identity.
- Test rotated pages, multi-column text, malformed xrefs, image-only scans, large tables, and mixed
  scripts before enabling a corpus.

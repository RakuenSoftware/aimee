# MR-06 implementation contract — 2026-09-25

MR-06 is complete; see the [closeout and evidence](memory-mr06-closeout-2026-09-25.md).

## Ranking and packing

Diagnostic recall captures native PostgreSQL lexical and cosine scores where
those arms execute, RRF arm ranks and contributions, candidate ordering, negation
and PageRank effects. A stage describes its resulting score; earlier native scores
are not additional votes. Ordinary recall and automatic ingress previews leave
optional detailed ranking capture disabled. Diagnostic invocations receive fresh
trace identities, including nested calls. Candidates are capped at 256 and the
serialized projection at 128 KiB, with explicit truncation.

The candidate universe is the bounded set admitted by the scoped retrieval owner.
SQL-filtered hidden records are not enumerated. Eligibility is owner admission;
source family is explicitly `not_assessed` when retrieval supplied no claim-specific
lineage projection. This neither certifies independent support nor promises an
exhaustive database search. Available source revisions are observed in the same
read. Retained and caller-limited candidates preserve the steps actually executed.
Schema 44 adds content-free candidate metadata to existing scoped recall traces;
`memory explain` receives that stored metadata through the existing trace reader.

Outer assembly reports `assembled` or `budget_dropped` for supplied ordinary
memories, code, versioned facts and each typed reference, including observations
and approved procedures. Assembly does not claim provider delivery. Before
preparation, the host durably records observed retrieved/assembled metadata and
its projection commitment when a source-bound assembly exists. Metadata larger
than 12,000 bytes becomes an explicitly truncated commitment. An unversioned body
receipt states the unavailable source coverage instead of inventing references.

## Durable dispatch and lookup

The existing audit owner holds an exclusive process-lifetime flock beside its
SQLite ledger. A new random dispatcher identity is available only after exclusive
ownership is acquired. Preparation and admission remain synchronous durable WORM
appends before provider handoff. Binding schema 2 adds dispatcher, renderer,
policy and exact assembly identity to the existing canonical request/body/source
commitments. Binding schema 1 remains readable.

`dispatch_started` records that a concrete provider request-write invocation
returned; it does not prove remote execution. A response can produce an
acknowledgement even when its optional started append failed. An admitted attempt
without a durable acknowledgement remains `outcome_unknown`, including crashes
before handoff, after send, and after response but before acknowledgement storage.
Only preparation without admission under a resolved former dispatcher can become
`prepared_without_dispatch`. A live dispatcher leaves that state unresolved.

`aimee memory receipt <request-id> --json` and POST `/v1/memory/receipt` read only
host receipts for the authenticated principal and exact request ID. Lookup first
checks the ledger chain and refuses truncated event sets (64 rows / 192 KiB detail).
The Go owner interprets complete bounded events. Schema validity, local producer,
source availability, payload correspondence, decision replay, chain inclusion,
external comparison and effects remain separate fields. A local checkpoint is
not an external comparison; an HTTP acknowledgement is not proof of an effect.
Caller-supplied `memory.verify_receipt` still cannot authenticate a producer or
assert ledger inclusion.

## Retention and rollback

`AIMEE_MEMORY_RECEIPT_RETENTION=commitment_only` is the default. It retains metadata,
references and hashes and cannot reconstruct the payload. Optional `replayable`
retention stores the exact final provider body as base64 in the existing encrypted
per-principal vault, under `memory-receipts/<attempt-id>`, before preparation.
The raw payload cap is 48 KiB because the vault value cap is 64 KiB. Missing vault
custody, storage failure or oversized payload refuses governed dispatch.

Replay access expires one hour after preparation. Expired ciphertext is removed
lazily on receipt lookup; this is an access window, not a claim of background
physical deletion. POST `/v1/memory/receipt/forget` requires memory-write authority
and removes owned replay payloads immediately. Failed removal remains pending.
`--replay` explicitly requests the base64 payload in lookup output. Default lookup
reports availability without returning it. Each loaded payload must match the
recorded byte count and SHA-256. Missing or deleted ciphertext reports replay
unavailable while preserving valid receipt inclusion. Exact payload reconstruction
does not claim the ranking decision was replayed.

The existing vault's custody and principal-scoped storage controls govern retained bodies;
no plaintext payload is appended to WORM. Rollback may disable detailed diagnostics
or return retention to commitment-only, but must preserve required durable receipt
stages and their schema-1/schema-2 read support. Candidates are validated on CT109;
production CT100 remains on released 0.4.5.

# Knowledge

`aimee-kb` turns sessions, documents, code, and explicit user facts into scoped durable knowledge.
It stores source evidence as well as the record derived from it.

## Scope

Knowledge is scoped as global, workspace, project, or user. Scope is an authorization boundary.
Promotion into a broader scope is an explicit audited write; sharing a KB does not make every private
record global.

Use one KB for any corpus whose members should share retrieval and curation policy. Separate KBs when
retention, trust, or legal scope differs.

## Record types

The KB can hold:

- memories and rules;
- typed facts and relationships;
- documents, chunks, pages, tables, and assets;
- entities, claims, decisions, and contradictions;
- code symbols, references, calls, dependencies, and co-change;
- retrieval evidence, provenance, feedback, and lifecycle state.

Raw text is not treated as a fact merely because a model extracted it. Derived records keep source,
confidence, class, time, and review state.

## Ingest and curation

```text
content -> validate -> store source -> chunk/index -> extract candidates
        -> resolve/link -> contradict/dedupe -> review/promote -> maintain/decay
```

The fast path commits usable source and lexical evidence. Background workers add embeddings, typed
artifacts, links, contradiction checks, and synthesis. Workers claim durable DB2 queue rows, so a
restart or second KB process does not duplicate the same unit.

Curator stages are independently enabled and split between model-bound and index-bound lanes. A
missing optional model degrades that stage; it does not relabel unprocessed content as curated.

See [Curator pipeline](CURATOR_PIPELINE.md).

## Recall

Recall can combine:

- lexical matches;
- dense similarity;
- entity and relationship graph;
- code graph;
- scope, recency, confidence, and lifecycle state;
- optional synthesis.

The result keeps the evidence needed to explain why it ranked. A query with weak support can abstain.
Synthesis does not erase its citations or turn an inference into a user-stated fact.

See [Retrieval stack](retrieval-stack.md).

## Typed facts

Typed facts are relationship triples checked against an ontology. A relation declares allowed
subject/object kinds, inverse or symmetry, correction policy, and sensitivity.

Candidates pass one write gate:

- a known relation with valid kinds can commit;
- a kind mismatch is rejected;
- a novel relation stays provisional until it earns promotion;
- a failed write is deferred, never reported as stored.

Provenance classes protect authority. An inferred correction cannot delete a user-stated fact.
Superseded values remain auditable. Sensitive or unknown relations fail toward the restrictive
context policy.

Typed facts are gated by configuration. See the generated reference for the current key.

## Memory lifecycle

Session context begins as local DB1 evidence. Durable, shareable knowledge belongs in DB2 after the
owning write or promotion contract accepts it. Useful records strengthen through recall and feedback;
stale or contradicted records can decay without deleting their history.

Working memory is not a lower-quality KB. It is session scratch and should not be promoted by
accident.

### Archival is a recall filter, not an access control

Archived rows are hidden from the default recall surface only when **both** flags are set:

| Key (`memory.lifecycle.*` in the config file) | Default | Effect |
| --- | --- | --- |
| `enabled` | `0` | `0` leaves every row looking `active` and the TTL sweep never runs — byte-identical behaviour for operators who have not opted in. `1` runs the sweep each maintenance cycle and tags commitment-shaped statements `pending`. |
| `hide_archived` | `0` | `1` filters `archived` rows out of default recall (`memory_list`, scored recall). `0` keeps them in recall. |

Both default to `0`, so **archival hides nothing out of the box**. `hide_archived` is also inert on
its own: it is read only after `enabled` passes, so setting it alone changes nothing.

Even with both set, this is a **relevance filter, not a confidentiality boundary**. `memory_get()`
returns archived rows by id regardless, by design — the filter keeps stale records out of ranked
recall, it does not restrict who may read them. Anything that must actually be unreadable needs a
deletion or retention contract, not archival.

These two keys are parsed from the config file only (`memory.lifecycle` object). They are not
registered in the flat config table, so they are not reachable through `aimee config get/set` and do
not appear in the generated [configuration reference](gen/configuration.md).

## Commands

```bash
aimee memory store <kind> <text>
aimee memory search <query>
aimee memory list
aimee memory get <id>
aimee memory read

aimee kb docs push <file>
aimee kb ingest status
aimee curator contradictions
```

Remote file commands upload content from the thin client. The server never opens a path on the
client's machine.

## Audit and privacy

Memory mutations publish through the owning daemon's event bus. Agent-controlled keys are
fingerprinted before audit so a key cannot inject personal data or forged log lines. The source
record itself remains subject to its KB scope and retention policy.

Model roles receive only the evidence allowed by the selected KB's provider and egress policy.
Embedding and synthesis can run inside that KB container or at its configured remote endpoint. Use
internal placement when the corpus must not cross the container boundary. There is no standalone
inference service beside the KB.

## What exists now

Scoped memory, typed facts, hybrid retrieval, curation, graph links, contradiction handling,
evidence, and the current single-KB model path are implemented. Multi-KB routing, connectors for
every company data source, and free-form cross-domain synthesis are not automatic; they need an
integrated routing or ingest path, scope policy, and evidence contract.

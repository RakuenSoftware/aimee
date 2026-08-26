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

## Temporal recall

Assertions carry two independent times. World time is when the thing was true. Belief time is when
the KB came to believe it. Asking what a value was last March and asking what the KB thought last
March are different queries, and they return different rows.

Recall returns current assertions by default. Historical recall is a request-level opt-in, so a
superseded value never arrives beside a live one without being asked for.

A model-extracted claim carries the exact byte span it came from and a hash of that region. A
candidate that cannot name its evidence is refused rather than stored unsourced.

Lexical and vector legs fuse late under a similarity floor. Graph expansion is bounded, temporally
filtered, and scope-checked at every hop. Each result carries the trace that explains its rank.

## Observations and reviewed procedures

An episode that fails, or recovers, once is a single event. The same shape across two independent
sessions becomes an observation: a deterministic, evidence-linked record of a recurring failure,
recovery, commitment, or state transition. The threshold is two sessions because one session cannot
distinguish a pattern from a coincidence.

An observation can raise a procedural proposal. It enters the same review and promotion gate the
learning module already uses, carrying applicability, expiry, evidence, and rollback metadata. A
proposal never promotes itself, and a rejected one rolls back without leaving an attribution row.

Application and outcome are attributed back to the record that was used, with cost, latency, turn,
and tool metrics. Retrieval sufficiency is scored separately from answer correctness, so a wrong
answer over good evidence and a wrong answer over missing evidence are told apart.

Context assembly is typed. Current assertions, historical assertions, episodes, summaries,
observations, reviewed procedures, and recent working context each occupy their own channel with a
budget, a packing trace, a watermark, and a stated trust boundary.

Semantic recall, active observations, reviewed procedures, and the typed assembler are on by
default. The master assembler and every channel keep a request-level opt-out; historical recall
stays opt-in. See the
[validation report](validation/temporal-assertion-learning-loop.md) for the deployment evidence.

## Memory lifecycle

Session context begins as local DB1 evidence. Durable, shareable knowledge belongs in DB2 after the
owning write or promotion contract accepts it. Useful records strengthen through recall and feedback;
stale or contradicted records can decay without deleting their history.

Working memory is session scratch. It is not a lower-quality KB, and it should not be promoted by
accident.

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

Scoped memory, typed facts, temporal recall, evidence-backed observations, reviewed procedural
learning, hybrid retrieval, curation, graph links, contradiction handling,
evidence, and the current single-KB model path are implemented. Multi-KB routing, connectors for
every company data source, and free-form cross-domain synthesis are not automatic; they need an
integrated routing or ingest path, scope policy, and evidence contract.

# Curator pipeline

The curator turns committed source into reviewed, linked knowledge in the background. It runs inside
`aimee-kb`; DB2 is the queue and source of truth.

## Lanes

| Lane | Work |
| --- | --- |
| model | document/code extraction, entity resolution, synthesis, entity promotion |
| index | narrative/claim/code embedding, contradiction detection, artifact linking, document and evidence indexing |

The lanes run independently. Index work can drain while a model call is in flight. Workers claim
rows with database locking and a bounded batch, then back off when idle or on error.

## Stage contract

Each stage has a stable name, enable predicate, lane, batch budget, and one-unit worker. A unit
returns work done, idle, or error. An error stops that pass and leaves durable work for retry.

The ordered registry owns scheduling. Adding a stage should not add another private drain loop.

## Normal flow

```text
source commit
  -> chunk and lexical index
  -> extract claims/entities/code units
  -> resolve canonical entities
  -> embed artifacts
  -> detect duplicates and contradictions
  -> link evidence and code
  -> synthesize or promote reviewed knowledge
```

For a contradiction, both claims remain. The pipeline links them, preserves their sources, and lets
review/policy decide the current value. It does not overwrite the older claim silently.

## Configuration

Curator stages and worker counts are descriptor-backed settings. Use
[generated configuration](gen/configuration.md). Disabling a stage stops new work for that stage; it
does not delete its existing artifacts.

## Add a stage

1. Define the input queue/state and idempotent unit of work.
2. Add the stage descriptor with lane and bounded budget.
3. Return explicit idle/error status.
4. Preserve source and scope on every derived artifact.
5. Add retry, concurrent claim, malformed output, and shutdown tests.
6. Expose honest health and backlog metrics.

Model output needs a schema and negative tests. Never parse a fenced prose answer as a committed
artifact.

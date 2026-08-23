# control-plane

Owns what is specific to aimee-kb.

## Why it exists

Three modules divide what used to be one tree, and the division is by *whose
behaviour it is*:

| module | owns |
|---|---|
| `postgres` | PostgreSQL itself: the pool, transactions, schema versioning. Nothing domain-specific. |
| `aimee` | What is specific to aimee-server. |
| `control-plane` | What is specific to aimee-kb: the corpus, ingest, sketches, documents, the KB's own tenancy. |

The split is not organisational tidiness. It decides where an operation runs,
and therefore what has to cross a wire.

A count-min sketch is exactly 1 MiB. The KB's index build loads it, mutates it
once per file, and saves it at the end. Expressed as a database operation that
is over 2 MiB of traffic per build, for work that belongs where the data is,
and it does not fit in a single reply. Owned here, beside the loop that uses
it, it never crosses anything.

The same reasoning applies to `db2_memory_alloc_all_ids`, which is
`SELECT id FROM memories` feeding a loop that asks a question per id: fine
in-process, one round trip per memory once the caller is remote.

## What it is not

`control-web` is a different module and a much narrower one: a bounded
proxy-route authorizer for the control-web process, one stage answering one
question. This is not an extension of it.

## Storage

From the `postgres` module, over the event bus. This module opens no pool and
imports no driver — what it owns is meaning, not connections.

Its schema is versioned under owner `control-plane`, independent of `db1`'s and
`db2`'s histories, applied through the storage module's `MIGRATE` operation with
explicit version numbers and checksums.

## Stages

| stage | name | event kind |
|---|---|---|
| 1 | `control-plane-health` | 12289 |

Principal ref 32. Event kinds follow the registry rule,
`4097 + 256 * ref + (stage - 1)`, and the module's own test derives the number
rather than transcribing it, so a kind that drifts from the registry fails here
rather than being discovered by a caller that gets no answer.

## State

The module exists and serves health. The KB-specific operations move in as
batches, each one leaving db2 rather than being copied — 157 of the 195
uncatalogued db2 symbols are consumed only by `src/kb` and belong here.

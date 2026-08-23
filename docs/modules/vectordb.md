# vectordb

The contract an external vector database implements to become Aimee's vector
store. Optional: without one, vectors live in PostgreSQL.

## Why it exists

The default is not a placeholder. The `postgres` module owns pgvectorscale
natively, and for ordinary embedding widths that is the whole story: the vectors
sit in the same database as the rows they belong to, in the same transaction,
with no second system to operate, back up or keep consistent.

What the default cannot do is very large vectors. Measured against
pgvectorscale 0.9.0 on PostgreSQL 17:

| index | element type | largest dimension that builds |
|---|---|---|
| `diskann` | `vector` | 16000 |
| `hnsw` | `vector` | 2000 |
| `hnsw` | `halfvec` | 4000 |

The `vector` type itself stops at 16000 columns. So a deployment that embeds at
16k is at the ceiling, and one that wants 32k, 128k or 256k cannot use
PostgreSQL for its vectors at all. That is the case this contract serves, and
the only one. It is not a portability layer, and it is not an abstraction the
built-in path is implemented behind.

## What it is not

**Not a provider interface with two implementations.** pgvectorscale is not a
DB3 provider. It is what the `postgres` module does, in the module, in SQL.
There is no adapter, no dispatch table and no interface it satisfies. When no
external module is present, nothing about the vector path involves this contract
at all.

**Not a place the rest of the schema moves to.** Rows, transactions, foreign
keys, tenancy and schema versioning stay in PostgreSQL whatever is installed. A
vector database holds vectors and the labels needed to filter them.

**Not a way to run two stores at once.** One vector store is authoritative. The
route selects it; it does not merge results from both.

## How it is selected

The `postgres` module looks for a module that has announced DB3 capabilities. If
one is there and ready, vector operations it can serve are sent to it. If none
is, they are served in-module by pgvectorscale.

Detection is the whole of the switching logic. There is no configuration key
that names a provider, because a named provider that is not installed is a
deployment that fails at the first search rather than at boot.

`aimee_db3_route_select()` takes a `fallback_enabled` flag, which decides the
one genuinely ambiguous case: an external store that is selected but errors. A
deployment whose vectors only exist in the external store must not fall back,
because falling back would search a corpus that is missing them and return a
short answer that looks like a complete one. A deployment mid-migration, whose
vectors are in both, may.

## Protocol

Protocol id 3, wire version 1. Event kinds carry the protocol flag
(`0x80000000`) and are constants of the contract rather than registry-derived
stage kinds, because a vector store is addressed as a protocol peer:

| event | kind | direction |
|---|---|---|
| `CAPABILITIES` | `0x80030001` | provider announces what it can serve |
| `APPLY` | `0x80030002` | upsert, delete or tombstone a point |
| `APPLIED` | `0x80030003` | provider acknowledges a generation |
| `SEARCH` | `0x80030004` | nearest neighbours for a vector |
| `ROUTE` | `0x80030005` | which store is authoritative |

Every message is a fixed header followed by variable-length fields, checked
against a magic and a declared length. The generated constants in
`src/modules/db2/include/aimee/db2/db3_contract.h` are authoritative and carry a
SHA-256 of the contract they were generated from.

### Bounds

| bound | value |
|---|---|
| dimension | 4096 |
| top-k | 256 |
| labels per point | 16 |
| label key / value | 32 / 256 bytes |
| all labels | 4096 bytes |
| workspace, project | 64 bytes |
| record type, collection | 32 bytes |

**The dimension bound is not the one that matters most.** The wire encodes
dimension as a `uint16` at offset 30 of the search request, so 65535 is the
protocol's real maximum however the constant is set. Raising
`AIMEE_DB3_MAX_DIM` covers 16k and 32k. 128k and 256k need that field widened to
a `uint32`, which is a version 2 of this protocol and a change to the generator,
the Go side and the baselines. Raising the constant alone would move the refusal
from encode time to a truncated dimension nobody noticed.

### Vectors are caller-owned

The request and apply structs carry `const float *vector` and a dimension, not
an embedded array. The codec allocates nothing: encode reads through the
pointer, and decode writes into a buffer the caller supplies and refuses a
dimension larger than it.

This is a consequence of the ceiling above. An embedded array makes the struct's
size the dimension ceiling, so a 32k-capable contract would mean 128 KB structs
passed and copied by value.

## What a provider must do

1. **Announce capabilities.** Dimensions, distance metrics and label filtering
   it supports. A provider that cannot serve a request's dimension must say so
   in its capabilities, not fail the search.
2. **Be generation-aware.** Every apply carries a generation; every search
   carries a `required_generation`. A provider that has not yet applied that
   generation must refuse the search rather than answer from an older state.
   Answering early is the failure this field exists to prevent: a search that
   silently misses the writes it was supposed to see.
3. **Return point ids, not rows.** Candidates are `(point_id, score)`. The
   caller authorizes every candidate against the workspace and project before it
   is used, so a provider that leaks across tenants cannot turn that into a
   disclosure.
4. **Refuse rather than truncate.** Every bound above is a refusal, not a clamp.

## Trust boundary

A vector store is a separate process holding embeddings. It is not trusted to
enforce tenancy: `aimee_db3_memory_candidates_search()` runs every returned
candidate through an authorize callback before the result is used. A provider
that returned another workspace's point ids would have them dropped.

Scores are validated finite, point ids positive, and duplicates in a reply are
refused, so a provider cannot make one point appear repeatedly to crowd out
others.

## State

The contract, its codec and the route are implemented and tested. No provider
module exists yet, which is why the struct layout could still change: once one
exists it cannot.

Related: [postgres](postgres.md) owns the default path and the detection,
[memory](memory.md) is the largest consumer of vector search.

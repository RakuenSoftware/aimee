# vectordb

The contract an external vector database implements to become Aimee's vector
store. Optional: without one, vectors live in PostgreSQL.

## Why it exists

The default is not a placeholder. The `postgres` module owns pgvectorscale
natively, and for ordinary embedding widths that is the whole story: the vectors
sit in the same database as the rows they belong to, in the same transaction,
with no second system to operate, back up or keep consistent.

What the default cannot do is wide vectors. Measured against
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

## What v1 cannot ask for

None of the fourteen vector searches DB2 holds can cross this contract today,
and the blocker is the search model rather than a shortage of fields.

**The model is: filter by attributes carried on the point.** Every DB2 vector
search instead filters by a relational predicate, in two families:

| family | the predicate | who |
|---|---|---|
| currency | `JOIN projects`, `lifecycle_state = 'current'`, `generation = p.current_generation` | code, kb, kb-pdf, kb-scoped |
| scope membership | rows in `memory_scopes` and `memory_workspaces`: active project, active workspace, `global`, `_shared`, and the *absence* of any scope row meaning legacy-untagged and therefore shared | every memory search |

A join is not an attribute, and the absence of a row is not a value.

Four searches additionally want filters the message has no field for at all: an
exact-label filter on search (apply carries labels; search carries none), a
collection naming which vector column on a row to rank against (apply names one;
search does not), negation (`exclude_project`), and set membership (`kinds`).

**The two families fail in opposite directions and neither is recoverable.**
Drop currency and the provider returns points from superseded generations,
scored and ranked among the current ones. Approximate scope membership with
workspace and project equality and the provider never returns the global,
`_shared` and legacy-untagged rows at all. Authorisation only removes
candidates, so it repairs neither: too many and plausible, or too few and
indistinguishable from complete.

Closing this is a real design rather than a field. DB3's model allows only one
answer -- denormalise the visibility decision into labels at apply time -- and
that is not free: a generation bump rewrites the labels of every point in the
project, and the legacy-untagged case has to be computed because it is the
absence of rows rather than the presence of one. Worth deciding before a
provider exists rather than discovering while wiring one.

`vector-portability.json` records all fourteen as `portable-after-db3-v2`.

**A missing filter is refused at the build, not dropped.** This is the one rule
worth stating twice. A request built without a filter it was given is
well-formed: a provider answers it, scores it, ranks it, and the result cannot
be told apart from a correct one. It simply has more rows in it.

Everything else here is loud about skew, because framing is exact-length and a
provider handed a message it does not understand refuses to decode it. That
protection lives on the reading side; this failure happens on the writing side,
where there is no wire to be strict about. So
`aimee_db3_search_request_build()` takes the full filter set a caller has and
refuses if any of it has nowhere to go, and
`aimee_db3_search_filters_expressible()` answers the same question for a caller
deciding whether to route externally at all.

That guard covers filters a caller PASSES, which is not enough on its own. The
two relational predicates above are applied by the SQL and passed by nobody, so
an adapter would build a request from workspace, project and record_type --
every one expressible -- and drop scope membership without ever holding it. They
are therefore named in the filter struct and refused like the rest: declaring
what the query actually does is what gets the request refused. A filter you never
named cannot be one you notice dropping.

Labels on search and the collection apply already names are a v2. Negation and
set membership are a filter language, which is more than this contract should
answer at v1 -- once there is NOT and IN, the next question is OR and the one
after is precedence. The two relational families are the larger question above,
and they gate every search rather than four of them.

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

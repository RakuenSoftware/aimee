# db3: external vector providers

## Purpose and non-goals

`db3` is the provider-neutral contract for vector search served by something
other than the relational store. A Qdrant, Milvus, remote pgvector, or other
adapter is an **implementation of this contract**, admitted like any other
module -- not a new API and not a storage tier with its own name.

It exists because in-database indexing has ceilings. pgvector's HNSW fits a
vector into one 8 KB index page, so it caps at 2000 dimensions for `vector`;
pgvectorscale's StreamingDiskANN is a separate access method with its own
limits. A deployment whose embedder exceeds what both can index is a supported
configuration, and this is what supports it.

**It does not own the canonical data.** The relational store always keeps the
authoritative vector copy, content hash, model/version/dimension, generation,
tombstone and outbox. A provider returns **candidates only**: the store
rehydrates each one and repeats the scope, lifecycle, quarantine and
classification checks before anything is returned. A provider that goes away, or
was never installed, costs recall -- never correctness.

## When you need one

Aimee's default embedders run at 384 and 768 dimensions, where both index methods
work and no provider is needed. You need one when your embedder's dimension
exceeds what the installed index methods accept. The schema tells you at apply
time rather than leaving you to infer it from a missing index:

```
aimee: memory_embeddings.embedding has no in-database vector index
       (column cannot have more than 2000 dimensions for hnsw index).
       Exact search still works but does not scale. If this is a dimension
       limit, attach an external vector provider module ...
```

Exact search keeps working meanwhile -- it just scans.

## Identity: how a provider attaches

A provider is **dynamically provisioned**, like a plugin module instance, so it
cannot pick its own identity. `DB3Router` keys providers by principal ref, and a
ref is not just an id: event kinds are carved from it as
`4096 + principal_ref*256 + stage`, so a ref reserves a whole 256-kind block.

Provider refs therefore come from a band reserved in
`tests/baselines/modules/canonical-inventory.yaml`:

| Band | Refs | For |
| --- | --- | --- |
| canonical modules | 1–30 | compiled-in modules |
| `plugin_principal_ref_band` | 200–455 | MCP/pluggy plugin instances |
| `db3_provider_principal_ref_band` | **456–511** | DB3 vector providers |

`check_module_inventory.py` refuses to assign any module a ref inside a reserved
band, and refuses bands that overlap. `DB3Router.ObserveCapabilities` refuses an
out-of-band principal outright.

This is enforced rather than documented because the failure is silent: a provider
attaching as ref 28 would derive `postgres`'s event kinds, and
`bus_host_serve_kind()` binds one kind to exactly one serving slot -- so the
**core module** would be the one denied at attach, with nothing in its own log to
explain why. That exact defect shipped once, with the plugin event range sitting
on top of postgres's block, and a live `aimee-kb` is what found it.

## Provisioning one

```sh
python3 scripts/provision-plugin-module.py \
    --kind db3-provider \
    --instance qdrant \
    --argv '["/usr/local/bin/aimee-db3-qdrant"]' \
    --module-bin /usr/local/libexec/aimee-modules/aimee-module \
    --config-dir ~/.config/aimee
```

That allocates a ref from the provider band, derives its kinds, and writes
`db3-qdrant.grant`. The same tool provisions plugin instances (`--kind plugin`,
the default) from their own band -- one allocator, so the two cannot drift apart.

A provider and a plugin may share a NAME; they get separate grants and
non-colliding refs.

Grants load once, at daemon start: provision before starting, or restart after.

## Public contracts

The wire contract is generated -- `src/modules/db2/include/aimee/db2/db3_contract.h`,
`server-go/db3/contract_generated.go`, and the pinned
`tests/baselines/modules/db3-wire-v1.json` -- by `scripts/gen_db3_contract.py`,
gated by `make -C src db3-contract-check`.

`server-go/db3/principal.go` carries the identity rules: `ValidateProviderRef`
and `ProviderKind`.

A provider has **no entry in `src/modules/process-contracts.json`**, and should
not: that file lists the 30 compile-time components, and a provider is installed
by an operator at runtime. Its contract is its grant plus the generated wire
contract -- the same arrangement plugin instances have.

What may cross the boundary is constrained by the operation catalog. **No DB3
event carries SQL, pgvector table names or operators, raw provider query JSON,
DB2 connection handles, or authority-bearing source rows.** An operation is
DB3-eligible only when its contract can be expressed with opaque point IDs,
vectors, a typed scope/filter expression, and bounded results.

| Stays in the relational store | May be served by a provider |
| --- | --- |
| canonical vector copy, content hash, model/version/dimension, generation, tombstone, outbox | provider-native ANN indexes and collection layout |
| schema and dimension migrations, drift refusal, re-embed inventory | idempotent batch upsert/delete from committed outbox records |
| mutations coupled to canonical rows, project lifecycle, WORM evidence, tenant transactions | bounded top-K candidate search |
| SQL/RLS joins and final tenant/project/classification checks | provider-native prefilters via the closed filter grammar |

## Which searches can route today

The apply side is complete on the DB2 end: `db3_capture_vector_row` triggers on
all eleven vector projections enqueue every committed insert, update and delete
into `db3_outbox` inside the writing transaction, and enqueue is a no-op while
no provider is `backfilling` or `active`, so nothing accumulates when nothing is
attached.

On the search side, what decides whether a search can route is whether every
condition it applies can be expressed on the wire. `SearchRequest` carries
`workspace`, `project`, `record_type`, and a closed list of exact-match filters
(wire v2). A condition outside that grammar cannot travel, and a search routed
without it would answer a wider question while looking correct.

| Search | Status | Why |
| --- | --- | --- |
| `pgvec_memory_search`, no active request scope | routes | scope and record type are the fixed fields |
| `pgvec_memory_search`, active request scope | pgvector | computes a visibility rank from `memory_scopes` and `memory_workspaces`, including rows tagged in neither. A join against canonical rows, not a label match |
| `pgvec_code_search`, named project | routes | `ce.generation = p.current_generation` becomes one exact filter once DB2 resolves the project's current generation |
| `pgvec_code_search`, no project | pgvector | every project at its own current generation is a per-row condition no single filter expresses |
| the four curator searches | route | single-table exact filters on labels the projections already capture |
| `pgvec_kb_search_scoped`, `pgvec_kbpdf_search` | pgvector | filter on `kb_documents.generation`, which is not a column on the embedding row, so the capture trigger has nothing to label it with |

Two shapes remain inexpressible for any collection, and both are grammar limits
rather than missing wiring: a kind filter (set membership) and a project
exclusion (negation). The filter grammar is exact match only.

### Resolving a join into a filter

The code search is the worked example. Its pgvector form is a join:

    JOIN projects p ON p.name = ce.project
    WHERE p.lifecycle_state = 'current' AND ce.generation = p.current_generation

A provider cannot join. But `projects.name` is unique, so the join selects
exactly one row, and resolving that row's `current_generation` up front turns
both conditions into a single `generation = N` filter. A project that is not
current resolves to nothing — and then the search must stay on pgvector rather
than route without the condition, because routing it would return rows from the
generation the project was detached at.

That is the general shape: a join can become a filter when it is functionally
determined and DB2 can resolve it before asking. It cannot when the condition
varies per row, which is why an unscoped code search and a scoped memory search
both stay behind.

### Routes are per collection

A DB3 route selects one provider and a provider serves one collection.
`record_type` cannot select one: the projection catalog stores it as a *label*
beside the vector (`memory_embeddings` maps it as one of five; `kb_embeddings`
pins it to the constant `kb`). A single process-wide route could therefore serve
exactly one collection, and a kb search sent through a memory provider's route
would return memory candidates that looked like kb answers.

## Dependencies and consumers

- `config` supplies the provider's endpoint and credentials, per instance.
- `module-runtime` authenticates the executable, UID, principal and event-kind
  grant on the bus it attaches to.
- The consumer is the relational store's retrieval path, which treats a provider
  hit as a candidate and re-validates it.

More than one admitted provider may be active at once; `DB3Router` selects the
deployed default as the lowest eligible principal unless control installs an
explicit override.

## Related

- `docs/proposals/pending/one-store-postgres-and-pgvectorscale-everywhere.md` --
  why vector columns are `vector` rather than `halfvec`, and where this fits.
- `docs/proposals/pending/db2-as-a-go-module.md` §3.2–3.3 -- the correctness split
  above, and the observer/routing contract.

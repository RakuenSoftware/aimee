# vectordb module

## Purpose and non-goals

`vectordb` is a DB3 vector provider: the other end of the portability split
described in [db3](db3.md). DB2 keeps the canonical rows and every authority
decision; this module serves the portable half, which is candidate search over
vectors plus the upsert, delete, and tombstone stream fanned out from DB2's
committed outbox.

What it deliberately does not do matters as much as what it does. It stores no
payload beyond the labels a filter matches on, it returns nothing but opaque
point ids and scores, and it makes no authorization decision. A provider that
returned content would let a caller read rows DB2 would have refused it, and the
whole point of the split is that an external index cannot become a way around
DB2's authority. DB2 rehydrates every candidate and repeats the scope,
lifecycle, quarantine, and classification checks before anything is returned.

It is not the canonical store, and it is not a fallback for one. A provider that
goes away, or was never installed, costs recall and never correctness: DB2
answers the same search from pgvector instead.

## Public contracts

The wire is the generated DB3 contract, not an API of this module's own:
`src/modules/db2/include/aimee/db2/db3_contract.h`,
`server-go/db3/contract_generated.go`, and the pinned
`tests/baselines/modules/db3-wire-v1.json`, all produced by
`scripts/gen_db3_contract.py`.

A provider serves exactly one collection, named at construction. The search
request carries no collection field, only a `record_type`, which the projection
catalog defines as a LABEL within a collection rather than the collection
itself. Reading `record_type` as the collection would search a namespace the
caller never asked for and return confident results from the wrong corpus, so
this module treats it as one more exact filter and keeps the collection out of
the request entirely.

Filters are exact equality on captured labels, and the encoder requires strictly
ascending keys. That closed shape is what a provider can answer without a join,
and it is why a search whose condition DB2 cannot flatten into filters stays on
pgvector rather than routing with the condition dropped.

## One module, many stores

Which vector database sits behind the DB3 contract is a deployment choice, so
the store is an interface (`vectordb.Backend`) and the provider does not know
which one it has. Adding Milvus means another package beside `qdrant/` and
nothing else.

A provider per store was the alternative, and a forked provider is how two of
them come to disagree about scope filtering. That is the one part which must
never differ, because it is all that keeps one workspace's vectors out of
another's answers. `Backend` is narrow for the same reason: authorize,
rehydrate, and return-payload are absent by construction rather than by
convention, so no backend can offer them.

Two backends ship:

- **memory**, the in-process index. It needs no external service, which makes it
  right for a smoke test and wrong for anything else: it loses everything on
  restart.
- **qdrant**, over Qdrant's REST API. Selected with `AIMEE_DB3_BACKEND=qdrant`
  and `AIMEE_DB3_QDRANT_URL`.

Qdrant differs from the contract in three places, and each would have been
silent:

- it has no tombstone, only points and their absence. DB2's tombstone keeps the
  identity while making the point unreachable, so a delete would preserve half
  of it and let the id be reused. It is stored as a payload flag every search
  excludes, under a prefixed key so a projected label cannot resurrect a point.
- it returns a DISTANCE for Euclid and a similarity for the other metrics.
  Passing the distance through would invert every ranking while still returning
  plausible ids. It is negated, as the in-process index already does.
- it reports no version of its own. The contract requires one, so the backend
  counts its own generation, and only on a write that actually landed.

`scripts/validation/db3/run-qdrant-e2e.sh` checks all three against a real
Qdrant, because a fake will confirm whatever the client believes about them.

## Dependencies and consumers

- `config` supplies the index dimension, metric, and collection.
- `module-runtime` authenticates the executable, UID, principal, and event-kind
  grant on the local module bus.

The consumer is DB2. Nothing else calls this module, and it calls nothing back.

## Providers and readiness

A provider is dynamically provisioned rather than compiled in, so it does not
pick its own identity. Its principal ref is allocated from
`db3_provider_principal_ref_band` in
`tests/baselines/modules/canonical-inventory.yaml` by
`scripts/provision-plugin-module.py --kind db3-provider`.

That is why `vectordb` has no `src/modules/vectordb/module.yaml` and no entry in
the canonical inventory, unlike every compiled-in module. Adding one would
require `src/modules/process-contracts.json` to carry a component with a
declared ref and the event kinds carved from it, and a running provider serves
none of those kinds: it attaches on the ref provisioning gave it. The contract
would describe a process that does not exist.
`scripts/check-module-descriptor-sources.py` records this as a stated exemption
rather than a silent gap, and enforces the tree the moment a descriptor appears.

Readiness is the capability exchange. `DB3Router.ObserveCapabilities` refuses an
out-of-band principal outright, and DB2 shapes its outbox fan-out to the batch
size the provider advertises rather than discovering the limit through
rejections.

## Configuration and activation

Optional and off by default. `runtime_toggle.supported` is `false`: a provider
is attached by provisioning a grant, which is read once at daemon start, so
there is no runtime switch to flip. Provision before starting, or restart after.
Removing the provider returns every routed search to pgvector, which is the same
degradation as never having installed it.

## Data and migrations

The module owns no schema. Its index is built from the label projections DB2
captures, listed in `db3_projection` in `src/modules/db2/c/schema.sql`, and a
label a projection does not capture is a filter this module cannot be asked for.

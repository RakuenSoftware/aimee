# Cut over to `aimee-llm`

This maintenance operation moves embedding and synthesis to the unified inference
service. A model, pooling, prefix, or embedding-width change requires controlled corpus
re-embedding; the KB enforces the first three through the `/health` `serving_id` guard and the
last through the recorded dimension (docs/retrieval-stack.md).

## Gate in staging

- pin the image and model digests;
- reproduce the old retrieval baseline on the real corpus;
- require the new stack to meet the declared nDCG/MRR/rank-agreement floor;
- verify structured curator output, degradation, restart, and mixed-load slot stability;
- rehearse forward and rollback migrations from a fresh database restore.

Do not cut over after a failed quality gate.

## Prepare

1. Back up DB2 and verify the backup.
2. Record source/vector row counts, model identity, dimension, config, and baseline queries.
3. Put KB writes into maintenance.
4. Start `aimee-llm` on the deployment network with the chosen tier.
5. Probe the exact model identity and embedding dimension at the URL the KB will use.

An internal unauthenticated inference endpoint must remain private. Use service auth before exposing
it beyond the trusted deployment network.

## Re-embed

Use the checked-in migration/runbook for the exact derived vector tables:

1. drop and recreate the dimensioned derived tables in one controlled migration;
2. update the recorded model identity and dimension only with the schema change;
3. replay source rows through the new embedder in bounded batches;
4. keep the KB degraded/maintenance until parity passes.

Deleting rows alone does not change a PostgreSQL vector column's dimension.

## Verify

- source and vector counts match expected coverage;
- every vector has the new dimension and no NaN/all-zero value;
- sampled source IDs map to the correct vector rows;
- baseline recall stays above the quality gate;
- synthesis identifies the intended model;
- a killed inference child produces explicit degradation and recovery;
- queue backlog drains after restart;
- latency and resident memory remain within the tier budget.

## Roll back

Rollback needs the old service image/model and a reverse re-embed from source or verified snapshot.
It is another maintenance operation, not an environment toggle. Keep the old image and snapshot for
the same declared window and rehearse the reverse path before production cutover.

See [Local inference](../LOCAL_INFERENCE.md) and [Retrieval](../retrieval-stack.md).

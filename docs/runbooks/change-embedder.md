# Change the KB embedder without mixing vector spaces

An embedder change for one KB corpus is a data migration, not a role toggle. Model weights,
dimension, pooling, and query/document prefixes define the vector space. If any of them changes
while old vectors remain, retrieval can return plausible but incorrect rankings.

The selected KB runs its embedder in the KB image, in its selected embedder sidecar, or at the remote
endpoint configured for that role. The synthesis sidecar is a separate role and does not participate
in the vector migration.

## Prove the candidate before changing production

1. Record the current model, serving identity, dimension, source-row count, vector-row count, and
   representative retrieval queries.
2. Back up DB2 and restore that backup into a disposable deployment.
3. Run the candidate with its production pooling and prefixes.
4. Compare recall, rank agreement, latency, and memory use against the recorded baseline.
5. Stop the candidate and confirm the KB reports the dependency failure instead of silently changing
   models.

Do not continue when the candidate misses the declared quality bar or omits its dimension and serving
identity.

## Choose the migration by vector width

`aimee kb reembed` is specifically a dimension-change reset. It drops and recreates the known
derived vector tables only when the target dimension differs from the recorded dimension. For a
same-dimension model, pooling, or prefix change, it prints `No dim change needed` and does not reset
the corpus or replace its recorded serving identity.

For a same-dimension change, provision a fresh DB2, configure the candidate before first ingest,
re-ingest the authoritative document, code, memory, curator, and evidence sources, validate the new
store, then cut traffic over. Keep the old DB2 unchanged until the new store passes verification.
There is no supported in-place whole-corpus reset for this case in the current release.

## Change dimensions with the guarded reset

The dimension-change route is disabled until the KB's own `$AIMEE_HOME/aimee.yaml` sets:

```yaml
kb:
  reembed_on_dim_change: true
```

Stop or drain writers. Record the candidate dimension, then inspect the server-side plan while the
current KB is still reachable:

```bash
aimee kb reembed --dry-run --target-dim <new-dimension>
```

Read the recorded and target dimensions and the tables the plan will reset. If they match the intended
migration, run the attended destructive path:

```bash
aimee kb reembed --confirm --target-dim <new-dimension>
```

The command asks you to type the target dimension before it drops and rebuilds the derived vector
store. It sets a maintenance marker and requeues document, curator, PDF, and evidence work. Switch
the deployment to the new embedder before allowing that work to complete, then rebuild memory
vectors explicitly:

```bash
aimee memory embed --all
```

`--force` bypasses the confirmation prompt and foreign-key protections; reserve it for a reviewed
recovery where the dry-run output explains why it is required.

## Verify before reopening writes

- **Match source and vector counts.** Explain every source row that is intentionally not embedded.
- **Check the active identity.** The loaded model, dimension, pooling, and prefixes must match the
  candidate you measured.
- **Run the saved queries.** Compare recall and rank movement with the pre-change baseline.
- **Check health and backlog.** `aimee kb status` must show the expected store, embedder, queue state,
  and no stuck re-embedding maintenance marker.
- **Restart once.** The KB must accept the recorded serving identity after restart.

If a failed run leaves search in maintenance after the underlying problem is repaired, inspect the
status first. `aimee kb reembed --clear-maintenance` only clears the marker; it does not repair or
rebuild vectors. When recorded and running dimensions differ, the command refuses unless `--force`
makes that risk explicit.

See [Retrieval stack](../retrieval-stack.md) for the selected internal or remote model contract and
vector-space identity guard.

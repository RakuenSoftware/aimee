# Standalone indexing and instance fusion validation

Validated on 2026-09-08 for PR #2972, targeting 0.4.3. The candidate used the
published 0.4.2 application, PostgreSQL, and embedder images with the PR's native
binaries, existing Go modules, runtime-web, and frontend built from source.
Native binaries and runtime-web were built inside the Bookworm application image.
No module repository, module release, or image publication was required.

The environment was a fresh disposable Debian 13 LXC (9440) on the .253 host,
with independent Server and KB Compose projects. The configured user instance
103 was inspected to establish the defect and was not upgraded during validation.

## Results

- The published 0.4.2 Linux client passed `client-pairing-e2e.py`: two independent
  certificates, concurrent memory writes, shared-owner recall, invitation expiry
  and replay rejection, owner isolation, individual revocation, persisted pairing
  after restart, and explicit global revocation. Bootstrap accounts cannot claim
  client ownership before the permanent account step.
- A Server with no KB discovered an existing repository after restart, extracted
  definitions and calls, and embedded all fixture files using the real embedder.
  Definition, caller, structure, hybrid, investigation, and blast-radius APIs
  returned the expected sources. Investigation attached code and a generation.
- Automatic default-branch refresh indexed a new symbol, removed a deleted file,
  and regenerated embeddings without an explicit index request.
- The code index and personal memory counts remained separate. The empty memory
  store was healthy; after the pairing test, four persisted personal records were
  reported from the Server's local store. No delegation history was reported idle.
- Stopping PostgreSQL made readiness fail and the dashboard report a database
  error. Starting it again restored readiness without restarting the application.
- `AIMEE_GRAPH_FUSION=off` retained direct code retrieval and removed graph
  expansion. The setting survived restart; restoring the default restored fusion.
- A Server with fusion on and an independent KB with fusion off ran concurrently.
  Enabling fusion on the KB added the graph signal despite a request carrying
  `graph_code_fusion_state=off`. Restoring KB off removed that signal. The Server's
  setting remained on throughout.
- Client-supplied file ingestion completed through the asynchronous operation API
  and published the private code snapshot without requiring a server filesystem path.

PostgreSQL integration tests cover atomic staged publication, incomplete manifests,
stale sessions, project confinement, traversal rejection, deletion, stale content
and model vectors, edits during embedding, and KB graph visibility/suppression.
Native tests cover readiness, benchmark override rejection and ordinary success,
and selection of private indexing only when no KB connection is configured.
Frontend tests/build, module descriptor and placement checks, config boundary,
API coverage, documentation checks, and build integrity passed.

## Reproduction

Use explicitly disposable containers. The pairing test changes accounts and
revokes credentials. The standalone test creates repositories, stops PostgreSQL,
and recreates the Server to test its instance configuration.

```sh
python3 tests/e2e/client-pairing-e2e.py \
  --server "$DISPOSABLE_SERVER" --store-db "$DISPOSABLE_DB" \
  --client "$RELEASE_CLIENT"
python3 tests/e2e/standalone-index-e2e.py \
  --server "$DISPOSABLE_SERVER" --store-db "$DISPOSABLE_DB" \
  --compose-dir "$DISPOSABLE_COMPOSE_DIR"
```

Both scripts refuse containers whose Compose project does not begin
`aimee-pairing-e2e-`. The standalone fixture checks retrieval correctness and
recovery; it is not a throughput measurement of the user's 89 repositories.

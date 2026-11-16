# Memory extraction owner regression

This qualifies the native owner after removal of the retired Cognee/Hillock
serving paths. Source starts at Aimee testing `35418e47e13a1d690e402a093d72002c0e1ad7b3`
with the extraction branch changes identified by the source hashes. It does not
claim a complete aims-db replacement or equivalence of every application workflow.

A fresh, owned PostgreSQL 18.0 cluster used UTF8, UTC and the packaged schema at
384 embedding dimensions. A separate empty database served evaluation fixtures.
The actual `aimee_store_runtime` login was NOSUPERUSER, NOINHERIT, NOCREATEDB,
NOCREATEROLE and NOBYPASSRLS. All required PostgreSQL gates were enabled. The owned
cluster stopped successfully after testing; installed services were untouched.

The race-enabled full run passed **1,244 test/subtest events**, with no failures.
Eight packages passed and three command/helper packages had no tests. One bus
interop test initially skipped because its C host was not configured. That
original result is retained. A freshly built in-source C host then passed that
exact Go test under the race detector in a separate recorded follow-up.
There are no unexecuted named test skips after that follow-up. Counts refer to
test/subtest events, not independent guarantees.

The core native memory owner passed 1,062 events and memory-eval passed 20.
The other tested packages cover public contracts, module command dispatch, egress,
bus, identity and supervisor behavior. The complete package list and per-package
counts are in `validation.json`. The command was:

```sh
go test -race -json -count=1 -p=1 -timeout=20m \
  ./modules/memory/... ./memory ./cmd/aimee-module ./modules/egress \
  ./bus ./modules/module-runtime/...
```

The fixture supplied disposable connections through `AIMEE_KB_STORE_REPLAY_URL`,
`AIMEE_MEMORY_RUNTIME_URL`, `AIMEE_MEMORY_EVAL_URL`, `AIMEE_DB_TEST_URL`,
`AIMEE_TEST_PG_URL` and `AIMEE_KB_STORE_EVAL_URL`. Required replay/evaluation
flags were set. Connection values, database files, credentials, configs and
executables are excluded from this evidence.

All Go and packaged schema source hashes were identical before and after the
main run. Separate hashes identify the C conformance fixture and bus sources.
The manifest records compressed and uncompressed hashes, verified after writing.
Raw test output retains the initial bus-fixture skip and its successful follow-up.

# Hillock scenarios and first-start memory selection — 2026-10-11

The fetched `rc/v0.10.0` tip `78364232eab6d030e90dec3ebf77b21a0616ff46`
passed the expanded real-engine suite against Aimee implementation commit
`253c50cbd14fd3bdb2dc24e29025944515de18f8`. The hourly scanner accepts all
`rc/v<major>.<minor>.<patch>` branches and qualifies each immutable upstream tip.
Its `v3` receipts invalidate earlier, narrower qualification results. Triggers
remain upstream scanning and manual dispatch.

[Discovery](hillock-scenarios-memory-setup-2026-10-11/discovery.json),
[qualification summary](hillock-scenarios-memory-setup-2026-10-11/summary.json),
[required live test output](hillock-scenarios-memory-setup-2026-10-11/contract.log),
and [scale results](hillock-scenarios-memory-setup-2026-10-11/scale.json) contain
synthetic fixtures only. All four required live tests executed twice, across a
Hillock service restart, under the Go race detector. Scale retrieval passed at
100, 1,000 and 10,000 records, including concurrent readers and bounded deadlines.
The production pin `1edd166ead75b85a9ab95cd6ba4faf7011ad567c` also passed the expanded
runner with scale from this source worktree before the implementation commit.

The candidate tests exercise Aimee's production external catalog, owner handler,
public command interface and recall composition with the real HDC engine. They
assert current content after correction, idempotence, metadata/history, scope
isolation, import/export, retirement, retained erasure and stale-source refusal.
Adapter tests include Unicode, kind/tier filtering, cancellation and concurrent
queries. The shipping egress policy and HTTP executor send real health/ranking
requests with a fixture credential resolver and reject foreign callers, unsupported
routes and revoked credentials; rotation and recovery are exercised. This is not
a claim of a full deployed Server/KB/Discord or real Vault integration run.

First-start setup offers native, Cognee and Hillock. It validates service origins,
seals credentials through the existing Vault-only API, and saves one non-secret
`memory.backend` object. Administrator checks reject unauthorized changes and
dotted/flat credential aliases. Memory and egress read the saved selection at
startup; explicit environment selection takes precedence. Applying the choice
requires appliance restart. Existing installations default to native. No backend
installation or migration is performed, and KB configuration is independent.

Additional local checks passed:

- Frontend type check, production build and all 246 tests.
- Built Chromium wizard journeys selecting native, Cognee API-key and Hillock bearer,
  plus existing project-clone recovery and first-boot login regressions. HTTP/Vault
  endpoints in these browser checks are fixtures.
- Shipping config RPC boundary and secret-classification C tests.
- Real C-host/Go-process startup and kill/restart parity in Server and KB placement,
  and egress startup with the config snapshot contract. These are transport fixtures;
  PostgreSQL/egress operations outside the fixture remain unavailable deliberately.
- Go memory/adapter/catalog/egress/module-runtime and multicall race tests.
- Server and KB builds using `EXTRA_C_FLAGS=-std=gnu17`, matching the older C language
  mode used by CI; this host's compiler defaults to C23 and rejects pre-existing
  const-pointer assignments elsewhere in the repository.
- Module ownership, documentation, process contracts, bus/egress boundaries,
  standalone Go module bundle, test registration and workflow lint.

The generated schema ownership digest was refreshed only after reproducing the
same output with Python 3.12 and 3.14 and verifying unchanged generator/SQL input
hashes. Production Hillock pins and independently published repository pins did
not change. The gate qualifies the supported HDC/subword integration, not upstream
chat/graph/learning or arbitrary model answer quality. Scale uses one lexical
needle and measures bounded retrieval, not broad semantic quality.

## CI fixture review

The MR-18 release manifest's hash for `server-go/cmd/aimee-module/main_test.go`
was reviewed against the testing baseline. Its three frozen registry/stage/invalid
argument cases are unchanged. The file adds only imports and
`TestMemoryBackendSetupPersistsAcrossConfigOwnerRestart`; the manifest's groups,
cases and thresholds remain unchanged. The refreshed hash keeps input drift
checking enabled while admitting this additional configuration persistence test.

The native JSON adapter fixture now links the production startup configuration
capture. Fresh child processes exercise native, Cognee and Hillock, plus a saved
Cognee selection, with verified cleanup and unavailable-provider refusal. Each
case also verifies that subsequent saved/environment changes cannot reroute the
running owner before restart. Normal and ASan/UBSan executions passed locally.

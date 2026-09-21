# Private conditional retirement and durable retry identity

Private `memory.delete` remains non-destructive retirement: current recall stops
serving the record, retained history remains governed, and the result reports
`destroyed: false`. It now accepts `expected_version`; authenticated callers may
also supply an `idempotency_key`. HTTP, public Go commands and MCP `mutate` with
`verb: forget` retain the same verified-authority boundary. A model cannot retire
user-authored or unknown-origin content by supplying an authority field.

Go compares the expected owner/ID/revision under the existing row lock. Keyed
retirement serializes on the owner, verified principal and hashed key, and commits
canonical retirement, history, invalidation and the content-free receipt together.
A replay returns that same receipt only while the retired canonical revision still
matches. Changed payloads conflict; reactivation, later changes and erasure cannot
repeat the retirement or release the key. No retired content is returned by the
mutation. A new request for a missing target preserves the ordinary `not_found` contract.

Migration 32 adds a mutation-kind discriminator to the existing private receipt
table. Existing receipts retain the `supersede` kind, hashes and commit IDs. The
durable guard requires a retired matching result and an active historical target
for a retirement receipt. Existing receipt immutability and runtime ACLs remain.
The old schema-31 migration is unchanged.

Only keyed calls take the additional retry lock and receipt lookup. Unkeyed
retirement keeps its existing SQL update; conditional retirement obtains its new
revision from `UPDATE ... RETURNING`, avoiding a separate revision query. These
are query-path properties, not a measured latency improvement.

Local verification covers real PostgreSQL runtime roles, receipt-write rollback,
replay on a new connection, concurrent duplicate retirement, stale versions,
changed retry payloads, cross-verb reuse, authority preservation, reactivation,
erasure and public/runtime command forwarding. A separate schema-31 upgrade test
preserves an existing committed correction receipt and history, then rejects a
forged retirement receipt for an active record. The deployment fixture adds real
HTTP/MCP, Server restart and failure-injection checks in both placements.

Independent export validation found that the Go schema owner's descriptor omitted
all embedded SQL migrations and its generated entry point called a nonexistent
handler. The descriptor now carries all 32 migrations, including migration 32;
CMake tracks those compiler inputs. Bundled and exported executables share the
existing Go database/schema/peer startup assembly. The export regression builds
the standalone executable and checks argument handling and unavailable-store
startup refusal. The source lock remains unchanged.

Local checks passed: the required PostgreSQL memory suite and migration-upgrade
test, targeted retirement race tests, native build and memory route tests, all 17
S1 guard tests, all 77 repository lint gates, shared process/assembly tests, and the export regression suite
(19 tests, one existing skip). Fresh deployment results will be recorded against
the exact committed image revision.

Fresh application/harness `e697581ac912900961a65dbd3891406258051457` passes
**1,441/1,441 checks** on the owned `.253` CT 9498: **868 enrolled T2** and
**573 standalone T3**. Each topology includes 195 local-memory checks, with
27 new retirement checks, plus 298 provider-boundary and 37 native asynchronous
checks. Both fresh private databases apply migration 32 through the shared Go startup
assembly. The new cases exercise receipt-write rollback, exact-version retirement,
one committed retry identity across a Server restart, changed-payload refusal,
governed history, reactivation/erasure and model-authority refusal through actual
HTTP and MCP adapters.

- [T2 verdicts](memory-private-retirement-2026-09-21/fresh-t2-e697581ac9.json)
- [T3 verdicts](memory-private-retirement-2026-09-21/fresh-t3-e697581ac9.json)
- [Exact image identities](memory-private-retirement-2026-09-21/image-identities-e697581ac9.json)

Application image: `sha256:242f192843b4999f7f4047dbbdfe330146632b3a1924afa286a5eb9398dc58d0`.
The pinned PostgreSQL and embedder images are unchanged. All nine container
identities were verified; all three application containers enforce the actual
32 KiB provider ceiling. Largest native wire requests were 26,558 bytes in T2
and 25,980 bytes in T3. Native refresh, owner outage and restart, zero-budget
refusal and quoted provider-error serialization continue to pass. These are
correctness/size results, not a new latency claim or complete provider-token gate.

After evidence collection, all nine owned containers and nine empty owned
networks were removed. Images, volumes and raw receipts remain available on
CT 9498 for follow-up inspection.

Private creation retries, shared deletion
preconditions/retries, complete consumer/checkpoint/retention behavior and the
remaining MR-02 acceptance clauses remain open. This does not implement permanent
erasure or retire unrelated DB2 consumers. The C bus is unchanged.

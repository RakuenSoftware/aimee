# Supplied receipt verification — 2026-09-23

The Go memory owner now declares `memory.verify_receipt` for public RPC in both
Server and KB placements. It accepts a schema-1 `prepared_receipt` object and
optional canonical padded `payload_base64` (at most 512 KiB decoded). The dynamic
HTTP route is `POST /v1/commands/memory.verify_receipt`.

Verification compares the canonical metadata binding, nested source commitment,
and separately supplied exact payload bytes and count. It returns independent
evidence dimensions, with no overall “verified” bit. Supplied matching hashes
cannot authenticate a producer, establish ledger inclusion, prove current source
availability, replay a decision, or confirm provider effects. Those dimensions
remain explicitly unavailable or not checked. Commitment-only retention cannot
reconstruct a missing payload. This operation reads no private memory or audit
records and never returns the supplied body or source references.

The decoder rejects duplicate names at every nesting level, unknown fields,
case aliases, missing required fields, null scalar coercions, unsupported schema
versions and invalid source-version shapes. It preserves integer precision and
accepts JSON object reordering and whitespace without changing canonical hashes.
Changing model, build, limits, counts or source commitments detects a binding
mismatch; recomputing a forged binding still cannot claim producer authentication.

[Targeted Go race tests](memory-receipt-verification-2026-09-23/contracts-race.txt)
pass in both placements, including binary/Unicode payloads, mismatches, malformed
base64 and ambiguous metadata. The [exported owner build](memory-receipt-verification-2026-09-23/export.txt)
passes. New disposable native-provider assertions check durable preparation and
admission at provider arrival, public verification of actual bytes, independent
attempt IDs and acknowledgement uncertainty through the deliberate owner outage.
The harness unwraps the dynamic HTTP route's `result` envelope before checking
the owner evidence. Its follow-up kills the owned Server with SIGKILL, starts it
again, and compares sequence/event/detail commitments through independent
read-only ledger connections. It also checks that recovery adds no acknowledgement
to the intentionally unresolved admission. This models host loss after recorded
work, not a claim that every crash instruction boundary has been exercised.
Fresh deployment validation is pending; prior receipts do not certify these new
assertions.

Authorized stored-receipt lookup, dispatch ownership recovery, ledger verification,
external checkpoints and CLI/MCP/ACP exposure remain separate work. This slice
does not complete MR-06 or the 18-proposal program. CT100 remains on released 0.4.5.


## Native unversioned coverage follow-up

The first fresh verifier experiment at `773272f7e` failed the new ledger-at-arrival
assertion: native automatic recall supplied unversioned memory and consequently
had no source-release handle. Its provider request therefore did not participate
in the earlier receipt gate. The [failed receipt](memory-receipt-verification-2026-09-23/unversioned-coverage-failure/T3/native-async.json)
is retained; this is a coverage gap, not an audit-path lookup error. The complete
Go/PostgreSQL race suite for that verifier candidate passed in 186.601 seconds.

The follow-up marks accepted nonempty Go memory projections in host request
state, preserving the marker in normal asynchronous context copies. Such a
request now requires synchronous body receipts even when its memory sources
lack version handles. Go labels these `no_versioned_source_handle`, with empty
source references/check/scope metadata; this never claims that there was no
memory input or that eligibility was revalidated. A nonempty invalid/expired
handle still refuses instead of downgrading to this gap. The public verifier
returns the source-coverage label independently of payload correspondence.
Memory-disabled passthrough and calls without host request context retain their
existing behavior. Fully versioned native recall remains MR-01 acceptance work.

[Targeted race tests](memory-receipt-verification-2026-09-23/unversioned-contracts-race.txt)
cover separate attempts, explicit gaps, foreign observation and malformed handle
refusal. [Native ingress tests](memory-receipt-verification-2026-09-23/unversioned-ingress.txt)
use the real Go owner and WORM store for both source-versioned and unversioned
body receipts. Stream, actual Anthropic handler, request-context copy and agent
refusal tests pass, as does the [exported owner build](memory-receipt-verification-2026-09-23/unversioned-export.txt).
The repair's focused fresh deployment at `7fad8fce1` passed **48/48 checks**,
with [actual receipt evidence](memory-receipt-verification-2026-09-23/native-host-crash/native-async.json)
and [three image identities](memory-receipt-verification-2026-09-23/native-host-crash/image-identities.json).
Application image `sha256:4b05d948ce6b33388169dfacba1be6e263cb2b2e6ab6043faf948f4be8d0dd09`
recorded 15 distinct provider attempts and 44 durable stages. Independent reads
at provider arrival observed preparation/admission. The public verifier matched
the actual bytes and rejected a changed body. One deliberate owner outage left
admission unresolved. SIGKILL/restart preserved every selected sequence/event/
detail commitment exactly and invented no acknowledgement. This focused native
fixture does not replace the full T2/T3 matrix or prove every crash boundary.


## Bounded transient cache follow-up

The host now confirms the exact observation digest to Go only after its native
WORM append succeeds. Under count or byte pressure, Go may reclaim the oldest
such confirmed cache entry; pending admissions and observations without successful
persistence confirmation remain protected from pressure eviction. Existing expiry
still applies independently of durable ledger retention. Reclaiming a cached plan
never deletes ledger rows or changes the recorded outcome, including an explicitly
recorded unknown outcome. A later observation request for a reclaimed entry returns
unavailable rather than inventing a new event. Ordinary retries remain idempotent
while the entry is cached.

[Race tests](memory-receipt-verification-2026-09-23/cache-contracts-race.txt)
fill all 1,024 slots, prove pending and unpersisted entries cannot be displaced,
reject wrong/foreign confirmations, then admit another distinct attempt after
confirmed persistence without exceeding cache accounting. [Native tests](memory-receipt-verification-2026-09-23/cache-native.txt)
inject an observation append failure and prove no confirmation is sent; after
retry succeeds, a confirmation outage preserves the successful append and its
idempotent retry. The [exported build](memory-receipt-verification-2026-09-23/cache-export.txt)
and ownership/bus guards pass. Fresh full-matrix validation of this follow-up is
pending. It adds one bounded owner confirmation per persisted observation; no
matched latency or throughput claim has yet been established.

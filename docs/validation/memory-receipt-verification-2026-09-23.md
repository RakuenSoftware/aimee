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
Their fresh deployment run is pending; they are not certified by prior receipts.

Authorized stored-receipt lookup, dispatch ownership recovery, ledger verification,
external checkpoints and CLI/MCP/ACP exposure remain separate work. This slice
does not complete MR-06 or the 18-proposal program. CT100 remains on released 0.4.5.

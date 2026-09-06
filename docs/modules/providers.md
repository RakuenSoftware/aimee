# Providers

The required Go process in `server-go/modules/providers` owns provider
connections, model configuration, catalog data, discovery, diagnostics, and
recovery. Browser, HTTP, and CLI management calls all reach this owner. It builds
both in the multicall runtime and as an independently exported Go module.

## Connection and model identity

A provider connection has an immutable name, protocol, endpoint, authentication
method, and Vault credential. Multiple connections can use the same vendor URL
with different credentials. Models refer to the connection by `registration`.
Editing a connection updates its attached models. Removing a connection requires
explicit confirmation when models are attached, removes those models and their
routing references, and queues removal of the connection's credentials. Removing
the last model leaves the provider available for reuse.

The Providers GUI manages connections. The Models GUI manages discovery/manual
model IDs, limits, prices, and routing preferences. A blank credential during an
edit preserves the existing key. Blank model prices withdraw a declaration;
zero explicitly declares a free price.

## Migration and persistence

The Go store reads `models.json`, with `agents.json` as the legacy fallback. It
adopts legacy model registrations as saved connections without grouping by URL
or parsing a name prefix. Existing model expansion, vendor identity, reasoning
timeouts, published limits, cost tiers, and operator overrides are preserved.
Unknown fields survive ordinary mutations and native ABI round trips.

The Go owner is the only roster writer. It serializes mutations with a mutex and
stable advisory lock, validates the full roster, fsyncs a temporary file, renames
it atomically, and fsyncs the directory. Native snapshots carry a revision and
use compare-and-swap, preventing stale native saves from overwriting newer GUI
edits. Empty-roster replacement also requires explicit removal authorization.

Credential deletion and model-concurrency projections are recorded with the
roster mutation and replayed on the next request after a process restart.
Reusing a deleted connection name first completes its credential cleanup.
Legacy literal credentials move into Vault before a management request proceeds.
New literals never enter the roster. Credential rotation follows validation and
file preparation; failure before the roster rename restores the previous key.
Vault and roster commits remain separate resource transactions; a process kill
between a successful key rotation and the roster rename can leave the rotated
key on the old connection. Retrying the same edit converges.

## Runtime boundaries

The providers module contains no C implementation. The removed C code includes
its declaration adapter, all seven built-in profiles, model catalog parsing and
downloads, and provider/model management and roster persistence. Remaining native
entrypoints marshal the existing ABI and forward management requests to Go.
Pre-bus credential bootstrap invokes the same Go roster parser in a short-lived
lookup mode; it does not introduce another parser or writer.

The existing core Vault resource remains the credential storage boundary.
`server-go/modules/providers/vaultresource` communicates with its attested pipe helper; provider
selection and mutations occur in the Go owner. Literal credentials never travel
in process arguments or environment variables. The audit bus records management
metadata but excludes credential-bearing management payloads from raw capture.

HTTP discovery and probes go through the egress module. Credentials are sealed
for a named account, caller, destination origin, and authentication scheme, with
expiry and authenticated encryption. The management process has no ambient
network access. CLI probes run in an isolated Go worker with a fixed diagnostic
prompt, temporary working directory, output bound, and process-group timeout.
Existing native inference execution, per-turn authentication, and interactive
OAuth login workers remain separate resource/execution boundaries; this change
does not rewrite those runtime families.

Principal 33 serves stages 1 (resolve), 2 (validate), and 3 (manage), on event
kinds 12545–12547. The obsolete unregistered C declaration events collided with
economizer and are retired. Principal 74 requests egress and config services.
The binary declaration wire layout remains unchanged.

## Verification

Go tests cover connection isolation, concurrent writes, malformed rosters,
legacy adoption, stale snapshot rejection, unknown-field retention, secret
migration, deletion/reuse recovery, concurrency recovery, pricing, metadata,
authenticated discovery, pagination, and bounded CLI diagnostics. Native tests
use the real Go manager through a pipe fixture and check ABI compatibility.
The fixture's public test Vault is only for native unit tests and is never used
by a production module.

See `docs/validation/providers-gui.md` and
`scripts/validation/providers/README.md` for real browser acceptance and cleanup.

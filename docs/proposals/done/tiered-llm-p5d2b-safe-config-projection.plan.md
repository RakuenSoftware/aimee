# P5-D2b bounded safe configuration projection and P5 close-out

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** completed and roundtable-converged; validated on real PostgreSQL 17, the CT260/CT262
  required-mTLS topology, ASAN/UBSAN, and deterministic fuzz gates.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §§2–4.
- **Depends on:** completed P5-D2a management-read trust core and bounded agents projection, plus
  P5-D2b0's C-memory-model-safe active-config snapshot reader barrier.
- **Followed by:** P5 close-out only. Any new selector, management write, bulk read, or fan-out is a
  new proposal and adversarial roundtable boundary.

## Boundary

Add exactly one second consumer of D2a's management-read primitive:

- external: `GET /v1/servers/{server_id}/config?team=<canonical positive int64>`;
- management listener: exact empty-body `POST /v1/management/read/config/challenge` followed by
  `GET /v1/management/read/config`; and
- console: an OIDC-only, read-only safe-config drill-down.

The response is constructed from five typed policy/posture values that already exist in
`legacy_config_record`. It never returns raw configuration and never redacts a larger object. This is the
terminal P5 read selector. The closed selector set becomes exactly `{agents,config}`; widening it
requires a new plan plus an adversarial roundtable even if a future change appears to be only a
CHECK-constraint edit.

## Frozen projection

Return exactly this envelope, with `additionalProperties: false` at both object levels:

```json
{
  "server_id": "server-a",
  "team": 42,
  "config": {
    "mtls": "required",
    "remote_writes": "off",
    "client_transport": "socket",
    "cli_session_forwarding": false,
    "require_aimee_git": true
  }
}
```

The five config fields and their source mappings are frozen:

| Wire key | Source | Closed wire value |
|---|---|---|
| `mtls` | `legacy_config_record.server_api_mtls` | `off`, `optional`, or `required` for source 0, 1, or 2 |
| `remote_writes` | `legacy_config_record.server_api_remote_writes` | `off`, `data`, or `full` for source 0, 1, or 2 |
| `client_transport` | `legacy_config_record.server_api_client_transport` | empty or `socket` emits `socket`; otherwise exactly `http` or `auto` |
| `cli_session_forwarding` | `legacy_config_record.server_api_cli_session_forwarding` | JSON boolean; source must be exactly 0 or 1 |
| `require_aimee_git` | `legacy_config_record.require_aimee_git` | JSON boolean; source must be exactly 0 or 1 |

Any unrecognized source enum, unterminated string, non-boolean integer, or config load failure
fails the complete response as `unavailable`; it never substitutes a default other than the
documented empty-client-transport=`socket` canonical default. `server_id` retains D2a's exact
ASCII identifier grammar and team remains a positive signed-64-bit integer. The response is
encoded completely in memory, capped at 32 KiB, emitted once, and never truncated or streamed.
JSON member order is the exact order in the example so fixtures are deterministic.

The projector receives a dedicated record containing only the five source values. In the running
server, the source loader calls the existing `legacy_config_read(legacy_config_record *)` API. That API is explicitly
the public active-config read: after `config_snapshot_init` it delegates to
`config_snapshot_get`, which copies one coherent POD `legacy_config_record` slot under the existing seqlock
and retries if a reload publication races it. It therefore reports the active validated runtime
configuration, not an independently reread disk file, and does not parse, normalize, resolve
environment, run commands, mutate globals, traverse a caller path, or publish configuration.
`config_load_file` is forbidden on this path. Before the server snapshot is initialized the
management listener is unavailable, so the loader never falls back to file semantics here.

P5-D2b0 must first repair the existing two-slot snapshot implementation: a sequence retry alone
does not prevent a second consecutive publication from reusing an ordinary `legacy_config_record` slot while
a reader is still copying it, which is a C data race even if the reader later retries. D2b may rely
on `config_snapshot_get` only after D2b0 adds reader lifetime pinning (or an equivalently reviewed
RCU/locking design) and a ThreadSanitizer gate with consecutive publishers and full-structure
readers. This prerequisite is a separate, tightly scoped merge so the concurrency primitive is
validated independently of the disclosure path.

After the coherent, reader-pinned `legacy_config_read` copy succeeds, the loader copies only the five named members into
the dedicated record and cleanses the local `legacy_config_record`. No filename, source selector, reload
request, or caller-selected input exists in this API. The projector never receives `legacy_config_record`, a
`cJSON` source node, YAML text, a filename, or a generic key/value map. Adding a field to
`legacy_config_record` cannot widen the wire type. Tests race snapshot publication against reads and require
each result to match one complete before-or-after five-field tuple, never a mixture. They also
prove the getter cannot cause a publish/reload, file read, command, network call, or environment
resolution. Canary tests place values in bearer token, client-CA path, provider/model endpoints,
commands, environment-shaped members, prompts, personas, tool policy, and other source members and
prove none is reachable through the getter or JSON round trip.

The authoritative source declarations are exactly `legacy_config_record.server_api_mtls`,
`server_api_remote_writes`, `server_api_client_transport[16]`,
`server_api_cli_session_forwarding`, and `require_aimee_git` in
`src/modules/config/config.h`. Their parser keys are respectively `aimee.api.mtls`,
`aimee.api.remote_writes`, `aimee.api.client_transport`,
`aimee.api.cli_session_forwarding` in `config_parse_server_api`, and top-level
`require_aimee_git` in `config_load_file`. No nearby listener, bearer, path, generic transport,
or console setting may substitute for one of them.

Explicitly excluded, including from errors and WORM metadata: bearer tokens, API keys and key
references, certificate/key material, CA names or paths, endpoints, URLs, hosts, ports, listener
addresses, commands or argv, environment names or values, headers, cookies, filesystem paths,
globs, mounts, working directories, provider/model configuration, personas, roles, prompts,
instructions, tool policy, templates, cron state or jobs, telemetry configuration, raw YAML/JSON,
and secret-shaped placeholders. Numeric rate/event-stream settings are not in this slice. The
roundtable's suggested `log_level`, config-level `max_parallel`, feature/cron flags, telemetry
redaction flag, and HTTP timeout fields do not exist in the authoritative `legacy_config_record` contract and
are rejected rather than invented. A `cron_jobs_present` flag would itself add a side channel and
is also excluded.

## Reuse of the D2a trust protocol

D2b adds no lighter or parallel authorization path. It reuses, in the same order:

1. composite actor authentication and org-admin/active-team-lead authorization;
2. one primary snapshot binding actor, team, active target, endpoint, enrolled management cert,
   revocation state, and this kb installation's admitted management identity;
3. the pinned no-redirect mTLS session and a same-session 15-second single-use challenge;
4. a primary immutable `kb_management_read_intent` plus the shared cross-kind namespace;
5. isolated token-authority admission and retained-byte readback for `remote_reads`;
6. a fresh nonce-bound primary status proof;
7. live peer/token/path/digest/team/certificate/generation verification and durable JTI consume;
8. complete local load, safe projection, and in-memory encoding;
9. the C3 primary checkpoint immediately before the first response byte; and
10. one atomic bounded response.

The D2a error envelope, HTTP mapping, failure ordering, deadline, response cap, no reconnect/no
retry rule, OIDC credential selection, endpoint policy, and first-failure semantics remain
unchanged. Once the nonce or JTI is consumed it stays consumed even when load, projection,
checkpoint, or delivery fails. Break-glass, console-admin, missing/restarted OIDC vault state,
ordinary membership, inactive lead, cross-team access, revoked identity, rollback, replica-only
state, dependency outage, and malformed authenticated upstream material all preserve D2a's closed
results and zero-data behavior.

## Selector, purpose, digest, intent, and audit binding

Generalize `server_mgmt_read_digest_input_t` with a closed selector enum containing exactly agents
and config. The shared encoder derives both selector string and canonical external path from that
enum; callers cannot provide an arbitrary path. Agents continues to encode the existing string
`agents` and path `/v1/servers/{validated-id}/agents` byte-for-byte, preserving D2a's 165-byte KAT
and SHA-256 `c66354428fbcdb9648b532b8de71b748e0d058711cfb441c608f5460564efcbf`.
Config uses string `config` and path `/v1/servers/{validated-id}/config` in the same existing
length-prefixed transcript. A checked-in config KAT freezes its full bytes and SHA-256 before
implementation proceeds. There is no protocol-version bump because the existing transcript
already contains the selector and path fields; the change only makes the previously closed
single-value input explicit.

Use the second exact route `POST /v1/management/read/config/challenge`, with no query and exact
empty body, to issue the config challenge. That route's compile-time constant derives purpose
`management.read.config.v1` for issuance, status signing, status verification, and nonce
consumption. The D2a route `POST /v1/management/read/challenge` remains byte-for-byte and derives
only `management.read.v1` for agents. No body, query, header, selector parameter, or caller-supplied
purpose selects between them. Encoded/trailing/sibling aliases and non-empty bodies fail before
nonce issuance. An agents-purpose proof/nonce cannot satisfy config and the inverse cannot satisfy
agents.

Extend the PostgreSQL read-intent selector CHECK and start/admit/finalize invariants to exactly
`selector IN ('agents','config')`. Derive the required canonical path from selector inside the
definer functions and reject every mismatch. Keep capability exactly `remote_reads`, kind exactly
`read`, and all namespace, lease, database-time, retained-token, key-use, RLS, role, and grant
semantics unchanged. SQLite remains a shape-only mirror.

Do not add a JWT selector claim: the already-signed `request_sha256` binds the selector, canonical
external path, nonce, team, both certificate identities, and both generations; the server
recomputes that digest from the concrete route's closed selector before loading data. A second
claim would duplicate the same cryptographic fact while changing the shipped D2a token wire shape.
Focused confusion tests prove a valid agents token/digest fails on config and the inverse fails on
agents. The WORM key-use record already has `selector`; require and test exact `config` there so
capability widening is visible to audit without storing response bytes or config values.

## Routes and console

The external kb parser accepts only exact `GET /v1/servers/{id}/config` plus exactly one canonical
`team` query. Reuse D2a rejection of missing, duplicate, empty, signed, zero/leading-zero,
overflow, extra, encoded/aliased, overlong, trailing, and sibling forms before authorization or
network I/O. Register the exact route in the C kb ACL.

The management listener registers only exact empty-body
`POST /v1/management/read/config/challenge` and exact `GET /v1/management/read/config`, both with
no query. Generic `/v1/config/*` and the data-plane config routes remain absent from that listener.
The challenge route selects its purpose by code constant; the endpoint passes its route's closed
selector into the shared verifier/projector. Neither accepts a selector or arbitrary loader from
wire input.

Expose the console proxy path `/api/v1/servers/{id}/config` only through the fleet credential,
never the console-admin credential. As on existing Fleet routes, `proxyAPI` strips `/api` before
`fleetAllows` matches `/v1/servers/{id}/config`; the upstream kb path remains that same `/v1`
path. The Fleet
drill-down adds a `Load config` action beside `Load agents`, uses only the verified OIDC fleet
credential, deduplicates an in-flight request, and clears stale data on team/server change. It
renders a fixed five-row read-only posture view, not recursive JSON. There is no edit control,
ACK, mutation, fan-out, templating, or fallback credential.

## Implementation packets

1. **Shared/server core:** selector enum, preserved agents KAT plus new config KAT; five-field
   source record/getter/projector; selector-aware endpoint; config purpose; exact challenge and
   read management routes.
2. **Authority/kb runtime:** selector-aware PostgreSQL functions and C DB/runtime records; generic
   bounded runtime read driven by an internal closed selector; exact external handler and C ACL;
   strict config response validator; WORM selector assertion.
3. **Console/docs:** Go ACL and tests, OIDC-only drill-down rendering, OpenAPI and generated route
   descriptors, delivery-status reconciliation.

The packets form one mergeable D2b branch after the separately merged D2b0 barrier. Avoid duplicating the D2a state machine or copying its
runtime function wholesale: factor only narrow selector-derived constants while preserving the
executable ten-step ordering.

## Tests and validation

- Shared KATs prove the agents digest is byte-identical and config differs; mutate every input
  field, selector, and path component and require a different digest or rejection.
- Projector fixtures cover all enum values, empty/socket canonicalization, invalid enums and
  booleans, unterminated input, output-too-small, deterministic exact JSON, and every privacy
  canary. Fuzz the five-field record and require only closed success or bounded failure under
  ASAN/UBSAN.
- Active-snapshot tests initialize two distinct validated configs, race publication/reload with
  the getter, and accept only a coherent old or new five-field tuple. Assert the getter uses
  `legacy_config_read`/`config_snapshot_get`, never `config_load_file`, performs no publish or global
  mutation, and triggers no file, environment, command, or network source action.
- The prerequisite D2b0 gate runs the full-slot reader against multiple consecutive publications
  under ThreadSanitizer (or an equivalent data-race detector), not only functional tuple checks.
- Endpoint tests cover wrong selector/purpose/path/capability/digest/team/audience/certificate,
  invalid proof, generation rollback, JTI replay, loader failure, projector failure, checkpoint
  race, and zero bytes before the checkpoint.
- Challenge tests cover the two exact empty-body routes, wrong method/body/query, route aliases,
  cross-purpose nonce/proof use in both directions, same-session binding, replay, and restart
  invalidation. No generic purpose/selector input is accepted.
- Route/ACL tests cover the full D2a hostile grammar matrix, OIDC-only credential selection,
  break-glass/console-admin denial before upstream I/O, stale-clear, and in-flight dedupe.
- PostgreSQL 17 tests cover the extended selector CHECK, canonical selector/path enforcement,
  kind namespace collision, RLS/grants, authority admission/finalization, concurrency, expiry and
  row-lock equality boundaries, and exact WORM `selector='config'` with no values.
- CT260/CT262 exercises both selectors over the real pinned two-node mTLS topology: org-admin and
  active-team-lead success; ordinary member/cross-team/revoked/rollback/outage denial; exact config
  envelope; canary absence; agents regression; same-connection nonce and durable JTI behavior.
- Run repository lint/build/unit/integrity gates, production server and kb builds, real PG17 gate,
  sanitizer/fuzz gates, and an adversarial complete-diff roundtable before PR merge.

## Deferred and P5 completion

No config writes, generic proxying, bulk/fan-out reads, config push, templates, orchestration,
streaming, SAML, certificate-rotation scheduling, new capability, or new selector. In particular
`policies`, `secrets`, `tools`, `personas`, `cron`, `telemetry`, and raw `config` are not aliases or
follow-on enum additions; each is a new disclosure proposal.

P5 closes only when D2a and D2b are merged, the shared OpenAPI/ACL/console/WORM contracts agree,
both exact selectors pass real PG17 and CT260/CT262 validation, and the final adversarial branch
review converges. Deferred items do not block P5 and must not be smuggled into its close-out.

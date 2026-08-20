# P8c: durable thin-client mTLS ramp

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

## Scope

Complete the bounded `optional` to `required` transition described by
`tiered-llm-p8-thinclient-mtls.md`. This slice does not change the thin-client
enrollment protocol or grant network callers new capabilities.

## Durable model

- Extend `pki_certs` with `last_presented_at`. A presentation counts only after
  the existing per-request durable certificate check returns `VALID`.
- Add a singleton `pki_mtls_ramp` row containing `ramp_state`, `roster_hash`, and
  `last_advance_ts`. State is `optional` or `required`.
- Compute `roster_hash` from the canonical, ordered set of active, unexpired
  enrollment rows. The hash and readiness decision are made in one SQLite
  transaction so concurrent issuance or revocation cannot advance a stale
  roster.
- Issuance and revocation invalidate readiness by updating the durable roster
  hash in the same transaction as the roster mutation.

## Transitions

1. Startup ensures/migrates both tables and validates the singleton row. A
   malformed or unavailable row holds the configured posture; it never weakens
   `required` to `optional`.
2. In configured `optional` mode, every valid mTLS request records the leaf
   serial's first/current presentation durably.
3. The readiness transaction advances only when the active roster is non-empty,
   every active row has presented, and its freshly computed hash matches the
   persisted hash. The update is idempotent and monotonic.
4. Before the durable commit, the server builds and fully validates a replacement
   SSL context with `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`, without
   publishing it. After the commit it performs only the infallible pointer swap.
   Existing connections retain their old context but still face the per-request
   durable cert check; new bearer-only handshakes fail.
5. On restart, a durable `required` state overrides a configured `optional`
   migration posture. A configured `required` posture is never downgraded.

## Seams

- `pki_mtls_ramp_init(configured_mode)` returns the effective startup mode.
- `pki_mtls_note_presentation(serial, now)` records a verified presentation and
  returns whether the durable state newly advanced.
- `pki_mtls_ramp_get` exposes state/hash/timestamp for tests and observability.
- `server_tls_prepare_required()` builds an unpublished required-mode context
  from the saved paths; `server_tls_activate_required()` performs the monotonic
  pointer swap and consumes it.

## Failure and concurrency rules

- All SQLite prepare/bind/step/commit failures hold the ramp and are logged.
- Empty rosters never auto-advance.
- Revoked or expired rows do not block the active roster, but cannot count as a
  valid presentation.
- A new enrollment after readiness was computed changes the roster hash before
  any advance commit can succeed.
- An unreadable client CA is fatal to context construction whenever mTLS is
  enabled; it must never silently degrade to bearer-only TLS.
- Context construction precedes the durable commit; the commit precedes the
  infallible pointer swap. A build failure holds optional state, while a process
  crash after commit restarts directly in required mode.

## Validation

- Unit: migrations, empty roster, partial roster, complete roster, issuance race,
  revocation/expiry, malformed state, idempotent replay, restart persistence, and
  fail-closed SQLite errors.
- TLS unit: monotonic optional-to-required context swap; never downgrade.
- CT260: enroll two clients in optional mode, present one (hold), present the
  second (advance), restart, prove bearer-only refusal and both cert handshakes;
  revoke one and prove next request refusal while the other remains accepted.
- Final CT260 gate also proved cert-only authentication without the shared
  bearer, invalid-bearer independence, network write denial, and required-mode
  persistence across restart.

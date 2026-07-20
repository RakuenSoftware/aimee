# P5 implementation plan — registry and kb→server management

## Slice boundaries

This slice adds the DB2 server registry, authenticated heartbeat/list/read operations,
and the reverse management transport skeleton. It does not add bulk orchestration or
change the server's existing `remote_writes` policy.

## Invariants

1. Registry rows are tenant-scoped and authoritative in PostgreSQL; runtime code never
   trusts a client-supplied team, owner, endpoint, or certificate identity.
2. Enrollment binds both role certificates (`clientAuth` and `serverAuth`) and is
   idempotent under a unique server identity; partial external CA work remains `pending`.
3. Every kb→server dial validates the enrolled management certificate identity and
   endpoint address policy before connect; redirects and unpinned peers are rejected.
4. Heartbeats update only the enrolled server row and are accepted only from the row's
   client certificate principal. Fleet reads are team-scoped through DB2 definer APIs.
5. Operator identity propagation is a separate short-lived, audience-bound token;
   the server still enforces its own capability and `remote_writes` gates.

## Deliverables

- `db2/server_registry.{c,h}` typed APIs for enroll-finalize, heartbeat, team-scoped
  list, and single-row primary lookup.
- SECURITY DEFINER functions and RLS policies for those APIs; SQLite schema remains a
  compatibility shape only.
- kb HTTP `/v1/servers` list and `/v1/servers/{id}/health` read routes, with console ACL
  containment and request-context tenant scope.
- reverse mTLS client helper using the existing TLS client-cert eligibility and enrolled
  certificate pin; no bearer-only fallback.
- server-side management-token verification hook bound to the mTLS peer and target
  audience, preserving existing remote-write policy.

## Gates

- `make -j$(nproc) server` and focused route/registry tests.
- real PG17 RLS/definer tests: cross-team list denied, wrong-cert heartbeat denied,
  pending enrollment retry, and revoked row rejected.
- CT260 two-node mTLS integration: heartbeat, list, health, write refusal with
  `remote_writes=off`, and operator identity visible in server audit.
- adversarial branch roundtable over this plan plus the complete diff before merge.

## Explicitly deferred

Bulk fleet actions, config fan-out, SAML, and automatic certificate rotation scheduling.

# P5-B2b primary management-instance lineage

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** completed; roundtable-converged and validated locally plus CT260 real PG17 and production-profile leaves.
- **Depends on:** P5-B1c online status authority; P5-B2a workload provider;
  P5-A management certificate profile and enrollment substrate.
- **Followed by:** P5-B2c CA signing, custody-wrapped local bundle lifecycle and
  renewal orchestrator; P5-B3 live challenge/status/health exchange.

## Boundary and decisions

B2b adds only PostgreSQL-authoritative management-instance grants, exact workload
binding, certificate-lineage state, fixed `SECURITY DEFINER` transitions, a typed
DB2 adapter, and real PG17/CT260 validation. It adds no HTTP route, CA call, CSR or
certificate parser, private-key/bundle persistence, listener, status request, or
health exchange. B2c cryptographically verifies the CSR/certificate/profile and
stages the custody-wrapped local bundle before asking B2b to activate its verified
public metadata. B3 composes the active leaf with the live management exchange.

The converged choices are:

1. Initial authority is a dedicated owner-provisioned instance grant. A synthetic
   `workload:` row is forbidden because P1 canonical memberships are only verified
   OIDC, certificate, and owner principals. Org-wide implicit management authority
   is also forbidden. The offline/operator grant fixes one team, one installation
   id, the current enrollment CA identity, and the exact expected B2a tuple.
2. The stable instance key is an operator-generated 128-bit lowercase-hex
   `installation_id` from the OS CSPRNG. Operation ids are independently generated
   by the OS CSPRNG and never reused as JWT ids, nonces or storage identifiers. The
   initial installation id also roots its replacement lineage and is independent of hostname and workload
   identity, then permanently bound at first use to exact issuer, subject, proof
   anchor and custody anchor. Reusing it with any changed component is integrity
   conflict, never renewal or migration.
3. A legitimate hardware/workload replacement is explicit, not inferred. A fresh
   random installation id and fresh exact grant names `replaces_installation_id`
   while preserving the initial installation id as the immutable lineage root.
   The owner-only replacement-grant transaction locks and revokes the old active
   management enrollment through the canonical revocation path, marks the old
   instance replaced, and creates the new grant with the preserved
   `replacement_lineage_id`. The caller-supplied lineage value is only an exact
   assertion against the locked old instance, never new authority. Ordinary grant
   creation roots lineage at its own installation id. A same-id changed tuple remains forbidden. External
   workload/KMS revocation remains an operator prerequisite and outside what SQL
   can prove.
4. Leaves have a 3600-second target validity. Verified
   `not_after - not_before` must be 3540..3660 seconds; `not_before` may be at most
   60 seconds in the future; initial/new activation requires 3000..3660 seconds
   remaining. Renewal cannot begin until the current leaf has at most 1200 seconds
   remaining, bounding simultaneous validity to 1200 seconds. There is no grace at
   or after `not_after`.
5. PostgreSQL stores verified public metadata and fixed-size digests only. No PEM,
   private/wrapped key or bundle, AEAD nonce/tag, JWT/token hash, raw token, helper
   proof/signature, CSR bytes, or storage receipt is stored or audited. B2b records a
   public CSR/SPKI digest and public bundle digest supplied by B2c but makes no claim
   that a digest alone proves local durability.
6. B2b activates externally issued metadata only and never invokes the CA. A begin
   commit authorizes exact public digests for ten minutes. B2c signs and durably
   stages the local bundle outside that transaction; activation rechecks that the
   verified leaf SPKI digest exactly equals the pending CSR SPKI digest. CA success
   followed by failure leaves only a bounded pending row; exact replay resumes it.
   Activation is attempted only after B2c has recoverably staged the bundle, but
   B2c/B2c tests own that durability claim.

## Canonical binding

The workload binding digest is SHA-256 over the literal ASCII domain
`aimee.p5.management-instance.binding.v1`, followed by u32-network-order-length-
prefixed issuer and subject, then u32 lengths of 32 and the exact 32-byte proof and
custody anchors. No terminal NUL participates and deterministic vectors pin every
byte. Both the typed C adapter and the SQL
function recompute it from the full tuple and reject a supplied mismatch. The full
tuple remains authoritative; digest equality never substitutes for component-wise
equality. The same custody anchor cannot back two active or pending installation
grants. Proof-anchor rotation, workload subject change, and custody-anchor change
all require explicit replacement, not renewal.

The grant also pins the exact enrollment CA issuer DN and CA certificate DER
SHA-256 fingerprint. Activation accepts only a B2c-verified
`p5-kb-management`/clientAuth/management-marker leaf chaining to that CA, with the
same pinned issuer and CA fingerprint. SQL enforces the public pin/metadata
equality; B2c owns chain, signature, EKU and marker verification.

This explicitly selects lineage option A. Option B adds a second operator-provisioned
128-bit value without adding entropy because the installation id is already random;
option C mutates identity on first replacement. This slice is greenfield, so there
is no deployed-row migration. DDL checks enforce the root from the first row, and
validation includes a three-hop chain plus distinct successor-fork attempts.

## PostgreSQL state

Add three primary-only tables to `src/modules/db2/c/schema.sql`, with SQLite shape mirrors
only:

- `kb_management_instance_grant`: installation id, replacement lineage id,
  independently random 64-hex replacement operation id,
  optional replaced installation id, team id, exact workload tuple and recomputed
  digest, expected CA issuer/fingerprint, `pending|consumed|revoked|expired` state,
  creator canonical identity, creation/expiry/consumption timestamps. Every
  SHA-256/id/anchor column has an exact lowercase-hex length check. Grants are
  bounded, expire after 24 hours unless consumed, and never return to pending.
  Partial unique indexes on custody anchor and workload digest cover
  `state IN ('pending','consumed')`; explicit replacement first revokes the old
  consumed grant before inserting the new binding.
- `kb_management_instance`: installation id, replacement lineage id, stable random
  enrollment `authority_id`, immutable team and exact B2a tuple/digest, current
  generation and enrollment id, `active|revoked|replaced` state and timestamps.
  Unique constraints cover installation id, active/pending custody anchor,
  authority id, and the exact tuple/digest where appropriate. Multiple independent
  kb instances per team are expected and allowed.
- `kb_management_instance_issue`: 64-hex operation id, installation id,
  `initial|renew`, generation, exact previous enrollment identity for renewal,
  exact 64-hex CSR/SPKI and public bundle digests, ten-minute pending expiry,
  verified leaf issuer/normalized-serial/fingerprint/SPKI/not-before/not-after, and
  `pending|active|expired|quarantined` state. Unique constraints cover operation id,
  `(installation_id,generation)`, certificate issuer+serial and fingerprint.

The runtime role receives no direct table or sequence privilege. The offline grant
functions remain executable only through the owner/migration provisioning path;
PUBLIC and `aimee_kb_runtime` are explicitly revoked. All three tables use FORCE
RLS. Because the existing `aimee_kb_owner` is intentionally `NOLOGIN
NOBYPASSRLS`, each table has one GUC-free owner policy with exact
`USING (current_user = 'aimee_kb_owner')` and the identical `WITH CHECK`. This is
not a runtime or tenant policy: broad/default runtime grants are explicitly revoked
for these objects, and the runtime crosses them only through fixed definer
functions owned by `aimee_kb_owner`. No new `BYPASSRLS` role is introduced.
Definer functions set `search_path=pg_catalog,pg_temp` and fully qualify public objects.
The activation path relies on the grant/instance foreign keys for team liveness
instead of consulting tenant-GUC RLS. Two additive GUC-free membership policies
permit `current_user='aimee_kb_owner'` to select only `cert:%` rows and insert only
non-default `cert:%` rows; the function rejects any cross-team row before insert.
The role-free schema load keeps the policy role list PUBLIC, while the predicate,
negative table ACLs and fixed definer ownership make only the owner context eligible.
Table-owner-proof triggers forbid DELETE and post-consumption/post-activation
mutation. A pending issue may change only once to active, expired or quarantined and
fill the fixed certificate/activation columns; an active issue changes no column.
The instance exact tuple, team, authority id and lineage id never change; only the
fixed current generation/enrollment, lifecycle state and timestamps may advance via
the functions. Every successful transition appends one metadata-only WORM event.

## Fixed transitions and lock graph

Every B2b function first takes a transaction advisory lock derived from the exact
installation id. Initial begin then locks the grant and inserts the not-yet-existing
instance/issue. Renewal and activation lock, in order, the current
`kb_enrollments` row, instance row, issue row and grant row. Replacement-grant
creation locks the old enrollment before the old instance and new grant. B2b does
not directly lock or update `kb_cert_revocation_generation` for ordinary issuance;
the existing canonical enrollment revocation function/trigger owns generation
advance during explicit replacement. No B2b path locks generation before an
enrollment, so it cannot invert the existing enrollment-before-generation paths.

Add these exact functions:

- Owner/offline-role `kb_management_instance_grant_create(...)` creates an initial
  exact grant. Exact replay returns the row; any field mismatch is conflict. It is
  callable only by the non-runtime provisioning role and has no HTTP route.
- Owner/offline-role `kb_management_instance_replacement_grant_create(...)`
  requires an exact active old instance and enrollment, revokes that enrollment in
  the same transaction through the canonical path, marks the old instance replaced,
  then creates the new-id grant with the preserved replacement lineage. Exact
  replay keyed by the replacement operation id returns the committed replacement;
  a different replacement tuple conflicts.
- Runtime `kb_management_instance_begin_initial(...)` recomputes and consumes the
  exact unexpired grant, creates generation-one instance state and one pending
  intent, and returns its persisted snapshot.
- Runtime `kb_management_instance_begin_renewal(...)` requires the same exact B2a
  tuple, current active/unrevoked/unexpired enrollment, current generation+1 and the
  renewal window; it creates or returns one pending intent.
- Runtime `kb_management_instance_activate(...)` rechecks exact tuple, CA pins,
  intent digests, leaf-SPKI equality, current enrollment/revocation, validity bounds,
  re-normalized serial and certificate uniqueness against both issue and enrollment
  stores. In one transaction it inserts the exact
  `p5-kb-management` enrollment with stable authority id and exact expiry; creates
  the P1 canonical `cert:<percent-encoded issuer>:<normalized serial>` team
  membership for the single grant team; marks the issue active; advances current
  generation/enrollment exactly once; and appends WORM evidence. It never calls
  generic `kb_enrollment_renew`. Exact active replay returns the snapshot; mismatch
  fails without writes.
- Runtime `kb_management_instance_snapshot(...)` executes one primary query over
  the instance/current issue/enrollment and returns only current public metadata.
  MVCC statement atomicity prevents a half-activation view. Missing, expired,
  revoked or replaced state is never reported active.
- A bounded expiry function moves time-expired pending grants/issues with no
  contrary authority event to `expired`. Revocation, replacement, changed authority
  or invariant mismatch moves outstanding pending issues to `quarantined`. Both
  paths append WORM evidence; rows are never deleted or reused.

The renewal threshold is inclusive: exactly 1200 seconds remaining may begin and
1201 may not; activation at `not_after` or later is denied. Grant/activation rechecks the grant team still exists and is still authorized at
commit. Replacement and any revoke-before-activate quarantine outstanding pending
issues. No two operation ids can activate the same generation. Exact idempotency is
the full tuple `(operation_id, installation_id, kind, generation, every public
digest and previous enrollment identity)`; only an exact match is replay success.

## Typed adapter and statuses

Add `src/modules/db2/c/management_client_instance.{h,c}` to the KB-only source closure. Public
structs use fixed bounds and enums, reject truncation, clear outputs on every
failure, recompute the binding digest, and never log tuple values, anchors, digests
or certificate identities. No server/status-authority binary links this adapter.

Map exact SQLSTATE/result categories:

- success and exact replay: `OK` with an explicit `replayed` flag;
- `22023`: `INVALID`;
- `28000`/`42501`: `DENIED`;
- deliberate replay/uniqueness `23505`: `CONFLICT`;
- `40001`/serialization/deadlock retry: `RETRY`;
- deliberate invariant `55000` or impossible persisted shape: `INTEGRITY`;
- connection, prepare, primary-read/write refusal, unexpected SQLSTATE: `UNAVAILABLE`.

PostgreSQL absence fails closed. Callers may retry only `RETRY` and
`UNAVAILABLE`; conflict/integrity requires operator repair. A replica or
read-only/non-primary transaction is `UNAVAILABLE`, including snapshot reads.

## Validation

Unit tests cover transcript/digest vectors, typed bounds, output clearing, status
mapping, replay flag and target isolation. ASAN/UBSAN/leak gates cover the adapter
and transcript codec. Build/link gates prove KB-only closure and no HTTP route.

A real PG17 runtime-role script covers schema/grant sync; the exact three owner-only
policy expressions; `relrowsecurity` plus `relforcerowsecurity`; zero runtime/PUBLIC
table and sequence ACLs after broad/default grants are applied; direct runtime
SELECT/INSERT/UPDATE/DELETE denial; definer access through only the granted fixed
functions; owner/migration provisioning access; exact initial grant/begin/activate
replay and mismatch; expired/revoked
grant; two nodes on one team; shared custody/tuple/installation substitution;
concurrent initial/renewal; renew-too-early; validity/overlap bounds; leaf-SPKI and
CA-pin mismatch; revoke between begin/activate; old-id changed tuple denial;
explicit replacement with new id and preserved lineage; replacement revocation
generation advance; crash after begin/CA success/local staging; activation retry;
no partial enrollment/membership/issue/audit; immutable active rows; pending
quarantine; lock-order stress against revoke/status admission; primary refusal and
replica/read-only refusal.

CT260 uses real PG17 plus the existing CA/profile code to generate two independent
management leaves for two simulated B2a tuples. It proves restart, primary outage,
revocation race, concurrent renewal, old-id changed-tuple denial, explicit new-id
replacement, exact membership/status-admission visibility and no private/PEM/token/
proof bytes in PostgreSQL or WORM output. CT262/listener behavior remains B2c/B3.

Finally run an adversarial full-branch roundtable, bake all valid minority findings,
and converge before merge.

# P5-C1c primary/WORM management-action journal

- **State:** DONE — implementation, adversarial branch review, production/ASAN gates, and real PostgreSQL 17 authority/concurrency validation passed on CT103 and CT260.
- **Parent:** `tiered-llm-p5-oidc-control-plane.md`, §3.
- **Depends on:** P5-B2b/B2c active outbound management-certificate lineage, P5-A target registry, P5-C1a strict target verification/replay, and P5-C1b canonical mint core.
- **Followed by:** P5-C2 custody-backed management signing key plus authenticated non-rollback JWKS, then P5-C3 action composition.

## Boundary and dependency ruling

Add the primary-authoritative, tenant-scoped journal for one privileged `remote_writes` action. The journal durably binds authorization, the complete C1b mint tuple, request digest, and both management-certificate identities to one correlation id and JTI before a later packet may create a token or send a network byte. A terminal outcome is appended with a second atomic WORM record.

The design roundtable retained the accepted order C1c then C2. C1c validates and replay-protects a syntactically valid `kid`, but **does not make that kid or the returned tuple dispatch-eligible**. C2 must bind the exact kid to an admitted active custody key; C3 must obtain it through that authority, recheck the admitted snapshots, mint/sign, and dispatch. No C1c caller can sign or dispatch, and `/v1/management/action` remains unconditional `503`.

Use two immutable append tables rather than a mutable `intent -> dispatching -> terminal` row. A committed intent proves authorization existed, not whether an irreversible remote effect occurred. Intent-only therefore means unresolved and is never automatically redispatched. A definite result appends one outcome; a transport-ambiguous result appends terminal `indeterminate`. Later reconciliation, if required, is a new append-only event in a later packet and never rewrites history.

This packet has no signing key, private-key API, JWKS, network client, endpoint selection, action handler, automatic recovery worker, or server-side route change.

## PostgreSQL authority and immutable records

Add `kb_management_action_intent` keyed by a 64-character lowercase-hex `correlation_id`, with a unique 64-character lowercase-hex `jti`. Its positive allowlist is:

- `team_id`, DB-derived canonical `actor_identity`, fixed `capability='remote_writes'`, `target_server_id`, and lowercase-hex `request_sha256`;
- exact C1b token values `token_issuer`, `audience` (exactly the target server id), `kid`, integer epoch-second `issued_at`, and `expires_at` with `1..90` second lifetime;
- local client lineage: `installation_id`, `installation_generation`, `installation_enrollment_id`, and its exact issuer, normalized lowercase-hex serial, and fingerprint;
- target lineage: exact management issuer, normalized lowercase-hex serial, fingerprint, and enrollment id;
- the primary revocation generation and database timestamp.

Add `kb_management_action_outcome`, keyed by and foreign-keyed to the intent correlation id, with denormalized team id protected by a composite foreign key, one closed result in `succeeded|denied|failed|indeterminate`, a bounded closed result class, optional bounded numeric status code, optional lowercase-hex response SHA-256, and database completion time. No endpoint, raw action/config/request/response, JWT, authorization header, certificate body, private material, provider error text, or other free-form field is stored. A field not in this allowlist is not journaled.

Both tables use exact case-sensitive text comparisons; no trimming, case folding, NULL/empty substitution, Unicode normalization, or timestamp recomputation occurs. IDs, digests, serials, and JTI have one checked lowercase-hex spelling; times are integer epoch seconds. Primary-key lookup gives O(1) correlation replay and the unique JTI index prevents cross-correlation reuse.

Enable and force RLS with owner-only policies for `aimee_kb_owner`. Install BEFORE UPDATE/DELETE/TRUNCATE guards on both tables. Revoke all table and sequence privileges from PUBLIC and the runtime role. PUBLIC receives no function execution. The runtime role receives EXECUTE only on the narrow facade functions through `schema_grants.sql`; direct `kb_audit_worm_append` access remains forbidden.

## Admission and outcome functions

Add hardened `SECURITY DEFINER` functions owned by `aimee_kb_owner`, with `search_path=pg_catalog,pg_temp` and schema-qualified objects.

`kb_management_action_intent_start` accepts the caller-generated correlation id and JTI, team, target server, fixed capability, canonical request digest, token issuer, syntactic kid, bounded TTL, and selected local installation id. It:

1. rejects `pg_is_in_recovery()` and a read-only transaction, validates every input exactly, requires nonempty `aimee.principal`, and requires `aimee.team` to equal the requested team;
2. takes a transaction advisory lock on correlation id, then checks for an existing row before reading mutable authority; an exact retry by the same DB-derived actor compares every caller-governed input and returns the entire persisted tuple without another audit append, while any mismatch raises `23505`;
3. for a new intent, requires a platform admin or active team lead, locks the target registry row first and then the selected local management-instance row, and keeps that global lock order;
4. requires the target to be active on the same team and its exact `p5-server-management` enrollment to be active, unrevoked, unexpired, and identical to the registry issuer/serial/fingerprint;
5. requires the selected local instance to be active on the same team and its current issue/enrollment to be active, unrevoked, unexpired, and identical to its current generation/enrollment lineage for `p5-kb-management`;
6. snapshots the singleton revocation generation, derives integer `iat` from primary database time and `exp=iat+ttl`, inserts the structured intent, then calls `kb_audit_worm_append` with action `management.action.intent` and canonical bounded JSON detail in the same transaction; and
7. returns `replayed`, the complete persisted C1b tuple and authority snapshots, explicitly tagged by the C facade as journaled-only/non-dispatch-eligible.

The exact retry path intentionally returns the committed snapshot even if mutable authority changed after admission; C2/C3 must reject dispatch unless their fresh rechecks still match it. A retry never receives a recomputed time or fresh identifier.

`kb_management_action_outcome_append` accepts correlation id, closed result/class, optional status, and optional response digest. It derives tenant and actor from transaction scope, locks the referenced intent, rejects cross-team access or an absent intent, then locks/checks an existing outcome. Exact replay returns it without another audit event; any changed field raises `23505`. A new row and `management.action.outcome` WORM append commit atomically. `indeterminate` is terminal just like every other outcome.

Canonical audit detail uses fixed `json_build_object` member order and only the positive allowlist required to identify the correlation, team, target, capability, JTI, digests, kid, result, and certificate pins. The global WORM append stays inside the same caller transaction: failure of either insert or audit append rolls back both.

## C facade

Add `src/modules/db2/c/management_action_journal.{h,c}` with fixed-capacity typed input/output structures and closed enums. The facade validates all bounded arrays including the full unused zero tail, generates correlation/JTI with the OS CSPRNG when asked to create a fresh operation, computes or accepts only an already canonical SHA-256 digest through an explicit API, begins the tenant transaction, calls the exact SQL function, copies every column with strict type/length checks, and commits before releasing output. It clears outputs on every failure or commit ambiguity.

Ambiguous start retry must reuse the exact same correlation id, JTI, and caller inputs. The API therefore exposes the generated identifiers before the first database attempt through a caller-owned operation object and never silently regenerates them. A confirmed journal result has no signer callback, bearer token, endpoint, socket, or dispatch method. Outcome commit ambiguity is retried with the exact same outcome; it is never converted to `indeterminate` merely because the database acknowledgement was lost.

Wire the object into the kb/server DB2 closure and add a shape-only SQLite schema representation with no false authority implementation.

## Tests and gates

Add focused C tests for generation, all field bounds/grammars, zero-tail and embedded-NUL rejection, typed result classes, output clearing, exact retry object reuse, malformed SQL result rejection, and commit-failure handling. Keep the production action route at unconditional `503`, and assert no signer, JWKS, custody, or network dependency enters this object.

Add a real PostgreSQL 17 gate that provisions owner/runtime roles and realistic two-team target/local certificate lineage, then proves:

1. one start creates exactly one intent plus one byte-verifiable WORM row atomically; one outcome creates exactly one outcome plus one WORM row;
2. sequential and 32-way concurrent exact retries converge to one structured/audit pair, while every governed-field mutation, correlation collision, JTI reuse under another correlation, and conflicting outcome fails;
3. ordinary members, cross-team leads, unset/wrong GUCs, replica/read-only mode, inactive/wrong-team local instances, stale generations, and inactive/revoked/expired/mismatched target or local enrollments create neither structured nor audit rows;
4. owner UPDATE/DELETE/TRUNCATE, runtime direct DML/sequence access, PUBLIC execution, raw WORM execution, and cross-team reads are denied;
5. injected failures after structured insert and during WORM append roll back both; start/outcome commit ambiguity can be retried exactly without duplicate rows;
6. an intent-only crash fixture remains unresolved and is never selected by any recovery/dispatch API, and `indeterminate` cannot be replaced by a later known result;
7. every SQL and C bound is tested, audit JSON contains only its allowlist, no forbidden raw material is present, and mixed SQL/C audit-chain verification succeeds.

Run production server and kb builds, lint, focused tests, existing P5-B2/B3 and C1a/C1b regressions, fresh ASAN/UBSAN, and the real PG17 gate on CT103. Run an adversarial full-branch roundtable with the complete diff/context, bake in genuinely valid findings, validate the exact candidate in the integration environment where applicable, then merge to `testing` through the normal PR flow.

## Packet completion versus later gates

C1c is complete only when the immutable schema/functions/facade, exact replay, atomic WORM behavior, ACL/RLS, crash semantics, and PG17 concurrency/negative gates above pass. C2 owns custody-backed active signing-key admission, signed JWKS generation/rotation/HWM, and binding authoritative kid to signer. C3 owns fresh snapshot rechecks, C1b composition, mTLS dispatch, outcome classification, and route enablement. Those later requirements cannot be used to claim a C1c tuple is currently dispatch-eligible.

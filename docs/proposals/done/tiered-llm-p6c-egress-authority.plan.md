# P6c-egress authority slice — catalog target resolution and policy region semantics

**State:** delivered and validated on PostgreSQL 17 (focused units, ASAN/UBSAN,
server/kb builds, static gates, and the full tenant/RLS gate).

This is the first bounded delivery unit of the pending P6c-egress integration plan. It
closes the authority boundary that every later serializer, signer, and dispatcher must
consume. It does not add network I/O, a public egress route, credential acquisition, or
native InvokeModel support.

## Why this slice comes first

The current `kb_bedrock_egress` scaffold accepts a caller-built target. The catalog stores
most of the Bedrock tuple, but it has no explicit SigV4 invocation region and exposes no
actor-and-team-bound target resolver. The current policy derivation also emits profile ARNs
once per destination region, implicitly treating array order/content as the invocation
region. Signing or dispatch cannot safely build on that ambiguity.

## Binding decisions

1. Add catalog-owned `aws_invoke_region`. It is the credential-scope, API endpoint, and
   profile-ARN region. `aws_region_set` remains the target/destination region set. No code
   infers the invocation region from the first array element.
2. The runtime resolver is a `SECURITY DEFINER` function accepting only `(team_id,
   model_id)`. It derives the actor from `current_setting('aimee.principal', true)` and
   proves membership in that exact team, entitlement of that exact team, enabled state,
   provider/wire `bedrock`, `bedrock_api='converse'`, and a complete supported tuple.
   It also requires `current_setting('aimee.team', true)` to equal the supplied team, so a
   multi-team actor cannot resolve team B while handling a team-A-scoped request. The caller
   cannot nominate an actor and the runtime retains no direct catalog `SELECT`.
3. Bedrock catalog endpoints must be empty in production catalog state. Later CT mock
   dispatch uses a test-compiled loopback-only transport override, never a stored endpoint.
4. Arrays are one-dimensional, nonempty where required, contain no NULL elements, and are
   capped at 64 entries at both write and resolve boundaries. Every scalar/element has a
   fixed owned C bound; truncation is an error, never success. For non-profile targets the
   destination set is exactly the singleton invocation region. For profiles, every
   underlying foundation-model ARN region must be in `aws_region_set` and every listed
   destination region must appear in at least one underlying ARN; duplicates may not widen
   the set.
5. The resolver returns arrays as JSON text from PostgreSQL. The C adapter parses them with
   cJSON into owned fixed arrays and rejects malformed JSON, wrong types, over-cap counts,
   raw JSON `\u0000` escapes before parsing, decoded control data, and any scalar that does
   not fit. PostgreSQL text-array syntax is not hand-parsed. (PostgreSQL TEXT itself cannot
   contain NUL; the raw-escape check closes the injectable/fake adapter boundary.)
6. `bedrock_target_t` gains `invoke_region`. Foundation/provisioned/custom resource ARNs and
   application/cross-region profile ARNs use that region exactly once. Profile destination
   foundation-model ARNs remain the authoritative `underlying_fm_arns`; no profile ARN is
   synthesized for every destination region. The policy resource set capacity becomes 65
   (one profile plus up to 64 underlying ARNs), while each catalog array stays capped at 64.
7. Existing Bedrock rows are not silently backfilled from array order. The idempotent schema
   migration leaves their new field NULL; the resolver fails closed until an admin re-upserts
   them through the new signature. The obsolete upsert overload is explicitly revoked and
   dropped so it cannot bypass the new required field.

## Implementation

- `src/modules/db2/c/schema.sql`
  - add nullable migration column `aws_invoke_region` plus coarse scalar checks;
  - replace `org_catalog_bedrock_upsert` with a signature that requires invocation region,
    rejects nonempty endpoint, enforces array rank/cardinality/NULL-element rules, and
    WORM-audits only content-free tuple metadata;
  - add `org_catalog_bedrock_target(team_id, model_id)` as the sole runtime authority
    resolver, returning the complete bounded tuple and JSON arrays;
  - define the new definer with `search_path=pg_catalog,public,pg_temp`, schema-qualify its
    objects/helper calls, and let schema ownership remain with the provisioned DB owner;
  - revoke/drop the obsolete overload; revoke every new signature from PUBLIC before grants.
- `src/modules/db2/c/schema_grants.sql`
  - grant only the new upsert and resolver signatures to `aimee_kb_runtime`; retain no direct
    catalog read.
- `src/modules/db2/c/org_model_catalog.[ch]`
  - add an owned `db2_bedrock_target_t` with 64-entry arrays and explicit caps;
  - add `db2_model_bedrock_target_resolve(team_id, model_id, out)` with a public enum:
    `OK`, `UNAVAILABLE` (one indistinguishable result for missing, wrong-team, unentitled,
    disabled, wrong-provider/api, or incomplete), `INVALID` (malformed adapter row), and
    `ERROR` (connection/statement failure). No catalog-existence or SQL-text oracle;
  - convert the resolver row strictly and clear `out` on every non-success.
- `src/modules/aws/bedrock_policy.[ch]`
  - add `invoke_region`, validate it, and correct per-target ARN construction;
  - retain deterministic resource ordering and fail-closed wildcard protections.
- Tests
  - extend `scripts/p6c_bedrock_catalog_test.sql` for the new signature, migration-null
    fail-closed behavior, endpoint rejection, array rank/count/NULL rejection, exact-team
    membership and entitlement, disabled/wrong-provider/invoke rejection, cross-actor denial,
    no direct runtime SELECT, and resolver tuple equality;
  - extend `src/tests/test_aws_auth.c` to prove invocation region is independent of
    destination ordering and profile ARN appears exactly once in the invocation region;
  - add a mandatory focused injected-row C resolver test for wrong JSON types, `\u0000`,
    decoded controls, NULLs, oversized arrays/scalars, partial output clearing, and exact
    typed-result mapping; valid PostgreSQL rows alone cannot exercise this hostile boundary.

## Gates

- regenerate `schema_data.h` through the build; never format SQL with clang-format;
- focused policy unit tests plus server/kb builds, lint, schema-sync, module-boundary, and
  kb-target-isolation;
- real PostgreSQL 17 catalog gate on CT103, including runtime-role grants/RLS and resolver
  assertions;
- ASAN/UBSAN for the C JSON-to-owned-target conversion;
- adversarial full-branch roundtable review, then merge to `testing` before the request/
  response and transport sub-slices.

## Explicitly deferred

Request serialization, endpoint/Host/path construction, SigV4 header assembly, structured
HTTP transport, non-stream Converse JSON-to-IR, rolling eventstream-to-delta conversion,
the independent CT260 mock Bedrock peer, public P2b admission, STS/vault acquisition, native
InvokeModel adapters, and Bedrock pricing rows.

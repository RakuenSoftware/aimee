# P6c-catalog plan: Bedrock catalog routing fields + adapter-registry validation (P6 §3)

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

Slice of P6 (Bedrock). Branch off `testing`. Completes the **authoritative-target boundary**
that P6a documented as a P6b/c invariant: `bedrock_session_policy(target)` (merged P6a) and
the §2 adapter selection MUST read the target's type / partition / region-set / ARNs /
`(bedrock_api, model_family)` from **primary-authoritative catalog config, never a client
string**. Today the merged P2a `org_model_catalog` has no such source. This slice adds those
proposal-mandated fields to the catalog and, at catalog WRITE, **rejects a Bedrock model
whose `(bedrock_api, model_family)` pair has no registered adapter, or whose routing tuple is
incomplete, fail-closed, before any reservation or egress** (P6 §3 + the AC "a model whose
`(bedrock_api, model_family)` pair has no registered adapter is rejected at catalog validation
/ before reservation, never routed to the openai catch-all"); real-PG-testable on CT103,
extends the familiar P2a catalog. The egress that READS these fields is the deferred P6c-egress
(needs P2b), this slice ships the authoritative SOURCE + its validation gate.

## Verified substrate

- **P2a `org_model_catalog`** (merged): `id, model_id UNIQUE, display_name, provider, wire
  CHECK IN(anthropic/openai/responses/gemini), endpoint, enabled` + `org_catalog_upsert(
  model_id, display_name, provider, wire, endpoint, enabled)` (admin-gated definer,
  WORM-audited) + `org_catalog_entitled()`. ENABLE-not-FORCE RLS, runtime no direct DML.
- **P6a `bedrock_target_t`** (merged, the exact tuple to MIRROR, non-speculative): `{type
  ∈ 5 target types, partition ∈ {aws,aws-us-gov,aws-cn}, region_set[], account, id,
  underlying_fm_arns[]}` + a `bedrock_api`/streaming flag. `bedrock_policy` already fail-closes
  on an incomplete target. This slice makes the catalog produce ONLY complete, valid targets.
- CT103 PG17 real-PG gate (run-p1-rls-gate.sh + p2a_catalog_rls_test.sql) + the schema-grants
  re-push discipline.

## Design decisions

1. **Bedrock fields on `org_model_catalog` (nullable; required+validated only when
   `provider='bedrock'`).** Mirror P6a's tuple exactly, nothing more:
   - `bedrock_api TEXT` CHECK IN ('converse','invoke'), the §2 adapter/wire selector.
   - `model_family TEXT`: e.g. 'anthropic','amazon-nova','amazon-titan','meta-llama',
     'mistral','cohere'.
   - `bedrock_target_type TEXT` CHECK IN ('foundation','provisioned','custom',
     'application-inference-profile','cross-region-inference-profile').
   - `aws_partition TEXT` CHECK IN ('aws','aws-us-gov','aws-cn').
   - `aws_account TEXT`, `aws_region_set TEXT[]`, `underlying_fm_arns TEXT[]`.
   A non-bedrock row leaves them NULL/empty. (The existing `wire` CHECK stays; a bedrock row
   sets `provider='bedrock'` and carries these fields. The IR wire mapping is P6c-egress, not
   this slice.)
2. **`provider='bedrock'` becomes valid.** `org_catalog_upsert` currently rejects unknown
   providers via its `p_provider` shape check only; 'bedrock' passes the shape check already
   (a non-empty string), so no CHECK loosening needed. The NEW gate is the Bedrock-specific
   validation below.
3. **Adapter registry (fixed, authoritative) + fail-closed validation at write.** A definer
   helper `org_bedrock_adapter_supported(p_api, p_family) RETURNS BOOLEAN`:
   - `converse` supports every listed family (the normalized path, proposal §2).
   - `invoke` (native) supports ONLY the allowlisted native families (P6b/c has a native
     adapter only for `anthropic`; others via converse), so `(invoke, anthropic)` is
     supported, `(invoke, meta-llama)` is NOT.
   `org_catalog_upsert` (extended, or a companion `org_catalog_bedrock_upsert`), when
   `provider='bedrock'`, REJECTS (RAISE 22023, fail-closed, no row written) if: `bedrock_api`
   ∉ {converse,invoke}; `model_family` empty; `(bedrock_api, model_family)` not
   adapter-supported; `bedrock_target_type` invalid; `aws_partition` invalid; `aws_region_set`
   empty; a `*-inference-profile` target with an empty `underlying_fm_arns` OR a non-foundation
   target missing `aws_account`. (Mirrors P6a `bedrock_policy`'s fail-closed completeness; the
   catalog can only store a target `bedrock_policy` will accept.) A non-bedrock provider skips
   all Bedrock validation (its bedrock_* stay NULL). Each mutation stays WORM-audited.
4. **Read surface unchanged in shape.** `org_catalog_entitled()` continues to return the
   entitled roster; the bedrock_* fields are admin/definer-readable (the egress reads them via
   a definer in P6c). No client-facing field leak beyond what P2a already exposed.

## Scope (P6c-catalog)

1. **DB2 schema** (`schema.sql` + sqlite mirror + grants): the 7 bedrock_* columns on
   `org_model_catalog` (+ CHECK constraints); `org_bedrock_adapter_supported(api, family)`
   definer; extend `org_catalog_upsert` (add the bedrock params + the fail-closed validation)
   OR a companion `org_catalog_bedrock_upsert`. DECIDE at plan-review (extending keeps one
   authoritative write path; a companion avoids a 6→13 param signature churn, lean companion
   `org_catalog_bedrock_upsert(model_id, display_name, api, family, target_type, partition,
   account, region_set[], underlying_fm_arns[], endpoint, enabled)` that sets provider=bedrock
   + validates, leaving org_catalog_upsert for the non-bedrock providers). WORM-audited.
2. **C access layer** (`db2/org_model_catalog.c` + header): a typed
   `db2_catalog_bedrock_upsert(...)` mirroring the existing `db2_model_catalog_upsert`, passing
   the arrays as PG array literals (bounded, charset-validated per element like P9a's allowlist
   builder, no injection).
3. **HTTP/CLI** (optional, minimal): extend the admin catalog route/CLI to accept the bedrock
   fields, OR defer the surface to P6c-egress and land only the schema+validation+C-layer +
   test in this slice. DECIDE at review (lean: schema+validation+C-layer+test; the admin HTTP
   surface can ride with P6c-egress since nothing consumes it yet).
4. **Tests**: unit (adapter-registry truth table: (converse,*)→ok, (invoke,anthropic)→ok,
   (invoke,meta-llama)→reject; the array-literal builder charset) + real-PG gate
   `scripts/p6c_bedrock_catalog_test.sql` wired into run-p1-rls-gate.sh: (a) a valid bedrock
   foundation model (converse, anthropic) upserts + reads back; (b) `(invoke, meta-llama)` →
   REJECTED (unsupported adapter), no row; (c) a cross-region-inference-profile with an empty
   underlying_fm_arns → REJECTED (fail-closed); (d) an invalid partition/target_type →
   REJECTED; (e) a non-bedrock provider still upserts fine (bedrock_* NULL); (f) admin-only +
   WORM-audit row on a bedrock upsert; (g) RLS: the bedrock_* fields are not tenant-readable
   beyond the existing catalog read policy.

## Explicitly deferred (P6c-egress)

The egress that READS these fields, builds the `bedrock_target_t` from the catalog row, calls
P6a `bedrock_session_policy` + SigV4/STS, drives the P6b eventstream decoder, and maps to IR;
the IR↔Converse serializer; the admin HTTP surface for the bedrock fields (if not landed here);
pricing rows (P3/org_model_pricing Bedrock entries). This slice is the authoritative SOURCE +
its fail-closed validation gate.

## Gate

- `make -j server` clean; `make schema-sync-check` green (sqlite mirror updated); `make lint`
  green; the OpenAPI/coverage checks green if a route is added (else unaffected).
- Unit + the real-PG p6c gate pass on CT103 (reject-unsupported + fail-closed-incomplete are
  the headline). **Re-push the UPDATED schema_grants.sql to CT103.** Existing gates unchanged.

## Non-goals

No egress, no target-struct read/wiring, no IR/Converse serializer, no SigV4 call, no pricing
rows, no live Bedrock. Pure authoritative catalog fields + the adapter-registry fail-closed
validation gate that guarantees the catalog only ever stores a Bedrock target the merged P6a
`bedrock_policy` will accept, real-PG-proven.

## v2 refinements (roundtable-converged; close the write bypass + true fail-closed registry)

- **The plain `org_catalog_upsert` MUST REJECT `provider='bedrock'` (RAISE 22023).** Decision:
  a **companion `org_catalog_bedrock_upsert`** is the ONLY write path for a bedrock row. To
  make that airtight, `org_catalog_upsert` (the non-bedrock path) now fail-closes on
  `p_provider='bedrock'` ("use org_catalog_bedrock_upsert"). Otherwise a caller could write a
  bedrock row via the plain path with NULL routing fields, bypassing ALL validation, the
  single most important fix. (Closes the recurring bypass finding.)
- **The adapter registry is a FIXED ALLOWLIST for BOTH apis, `converse` is NOT "any
  non-empty family".** `org_bedrock_adapter_supported(api, family)` (a plain **IMMUTABLE**
  function, NOT SECURITY DEFINER. It is a pure predicate) knows a fixed set of KNOWN families
  {anthropic, amazon-nova, amazon-titan, meta-llama, mistral, cohere, ai21}. `converse` →
  supported for every KNOWN family (an unknown/typo family is REJECTED, not admitted).
  `invoke` (native) → supported ONLY for the native-adapter subset {anthropic}. So
  `(converse, cohere)`→ok, `(converse, typo-family)`→reject, `(invoke, anthropic)`→ok,
  `(invoke, meta-llama)`→reject. A real fail-closed registry.
- **Completeness validation exactly matches P6a `bedrock_policy` fail-closed (no gap).**
  Reject when: `bedrock_api` ∉ {converse,invoke}; `model_family` not adapter-supported;
  `bedrock_target_type` ∉ the 5; `aws_partition` ∉ {aws,aws-us-gov,aws-cn}; `aws_region_set`
  empty OR ANY element empty/whitespace; a `*-inference-profile` target with empty
  `underlying_fm_arns` OR any element not a well-formed `arn:<partition>:bedrock:…foundation-
  model/…` (mirror P6a `fm_arn_valid`); a NON-foundation target with `aws_account` empty or
  not 12 digits. The target **`id` = the catalog `model_id`** (P6a's target id; already
  CHECK'd 1..200 + used as the profile/model id), no separate column. Each array element is
  charset-validated (`[A-Za-z0-9_.:/-]`, non-empty) so the target the catalog stores is
  exactly one `bedrock_policy` accepts.
- **Scope tightened: SCHEMA + DEFINERS + real-PG test ONLY, NO C access layer, NO HTTP/CLI
  this slice.** Since the egress + admin surface are both deferred, a `db2_catalog_bedrock_
  upsert` C wrapper would be an unused interface with no production caller (a real smell the
  panel flagged). The DEFINER is the authoritative, testable gate; the C layer + admin surface
  land with P6c-egress when there is a consumer. This is leaner and non-speculative.
- **Column exposure is admin/definer-only (verify, don't assume RLS covers columns).** RLS
  filters rows, not columns, but P2a already REVOKED the runtime role's DIRECT `SELECT` on
  `org_model_catalog` (runtime reads only via the `org_catalog_entitled()` definer). So the new
  bedrock_* columns are reachable ONLY by admin/definer. `org_catalog_entitled()`'s projection
  is UNCHANGED (it does NOT add the bedrock_* fields), so no tenant read path exposes account/
  ARNs/region. The gate asserts the runtime role gets `permission denied` on a direct
  `SELECT bedrock_api FROM org_model_catalog` (same posture P2a proved).
- **PG array-literal safety (explicit).** `aws_region_set` / `underlying_fm_arns` are built as
  `'{elem,elem}'::text[]` ONLY after each element passes the `[A-Za-z0-9_.:/-]{1,256}` charset
  check (no quote/brace/comma/backslash can appear), identical to the P9a allowlist-array
  builder; a failing element → reject the whole upsert. (No element is concatenated raw.)

### Gate additions

- (h) the PLAIN `org_catalog_upsert` with `provider='bedrock'` → REJECTED (the bypass is
  closed); (i) `(converse, unknown-family)` → REJECTED (registry is fail-closed, not
  accept-any); (j) a `underlying_fm_arns` element with a non-foundation-model / wildcard /
  cross-service ARN → REJECTED; (k) `aws_account='abc'` on a provisioned target → REJECTED.

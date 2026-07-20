# P6a implementation plan — AWS auth core (SigV4 + STS + session-policy + cache isolation)

Slice P6a of P6 (Bedrock + vendor breadth). Branch off `testing`. P6 splits into:
- **P6a (THIS)** — the PURE, OFFLINE, unit-testable AWS-auth core the proposal's own
  Testing §Unit enumerates: SigV4 signing (against AWS's published test-suite vectors),
  STS request construction for BOTH federation modes + response parse + web-identity
  iss/aud validation, per-target-type least-privilege session-policy derivation
  (fail-closed), and the tenant-and-policy-complete STS cache-key with isolation
  negative tests. No network, no DB, no egress — a self-contained `src/modules/aws/`
  library with a dedicated unit-test suite. This is the security-critical foundation the
  deferred egress will call.
- **P6b/P6c (DEFERRED)** — the Bedrock driver + IR↔Converse serializer + native adapters +
  `application/vnd.amazon.eventstream` decoder; catalog schema fields (bedrock_api,
  model_family, target type, partition, region-set) + validation reject-unsupported;
  DB2 org_model_pricing Bedrock rows; the live HTTP dispatch of AssumeRole/InvokeModel;
  end-to-end egress. These need the P2b egress path (deferred) + a live/mock Bedrock
  endpoint + the IR wire. P6a builds+tests the crypto/authz core they depend on.

## Verified substrate

- OpenSSL HMAC + SHA256 already linked (org_telemetry_fmt.c, kb/pki.c, kb/auth_oidc.c use
  them). No existing sigv4/sts/bedrock code (grep clean) — a clean module.
- OIDC verify patterns exist (kb/auth_oidc.c, kb/kb_oidc_jwks_fleet.c) for the
  web-identity token iss/aud validation shape — mirror, don't re-invent, but P6a's
  validator only needs iss/aud/exp claim checks on an already-obtained token (signature
  verification against the source JWKS is the token SOURCE's job / a P6b concern).
- Unit-test registration: `TEST_TARGETS += $(TESTPREFIX)/unit-test-<name>` in
  tests/Rules.mk, links only the module .o + OpenSSL (TEST_L_FLAGS). Pattern:
  unit-test-org-telemetry (P9a).

## Design decisions

1. **`src/modules/aws/` — a pure library, no I/O.** Every function is a pure transform:
   inputs → bytes/struct → outputs. NO socket, NO curl, NO DB. The live HTTP dispatch is
   the caller's job (deferred P6b). This makes the whole slice deterministically
   unit-testable and keeps the security-critical crypto isolated + auditable.
2. **SigV4 signer (`aws_sigv4.{c,h}`)** — the AWS Signature Version 4 algorithm:
   canonical request (method, canonical URI, canonical query, canonical+signed headers,
   hashed payload) → string-to-sign (ISO8601 date, credential scope) → derived signing key
   (HMAC chain: kDate/kRegion/kService/kSigning) → signature → `Authorization` header.
   Also the `X-Amz-Date` / `X-Amz-Security-Token` header emission. Tested against the
   **published AWS `aws-sig-v4-test-suite` vectors** (canonical request + string-to-sign +
   signature for the known key `AKIDEXAMPLE`/`wJalrXUtnFEMI...` at 2015-08-30). This is the
   gold-standard offline correctness anchor.
3. **STS request builders (`aws_sts.{c,h}`)** — construct (do NOT dispatch) the two
   MODE-DISTINCT calls, never conflated:
   - **mode (a) `AssumeRoleWithWebIdentity`** — a form-encoded POST body
     (Action, RoleArn, RoleSessionName, WebIdentityToken, ExternalId?, DurationSeconds≤900,
     Policy=<session-policy>), UNSIGNED (the web-identity token is the credential). Plus
     `aws_webidentity_validate(token, expected_iss, expected_aud, now)` — parses the JWT
     claims and checks iss==expected, aud contains expected, exp>now, iat sane. The token
     is NEVER persisted (a caller-held ephemeral).
   - **mode (b) `AssumeRole`** — same form params but with `Principal:AWS` semantics, and the
     request is **SigV4-signed** (reuses aws_sigv4) with the vault-held IAM key.
   - **response parse** `aws_sts_parse_assume_response(xml)` → {access_key, secret_key,
     session_token, expiration} (STS returns XML) with strict bounds + a missing-field →
     error. A fixture request+response pair drives the tests; NO live call.
4. **Least-privilege session-policy derivation (`bedrock_policy.{c,h}`)** — pure
   `bedrock_session_policy(target)` where target = {type ∈ {foundation, provisioned,
   custom, application-inference-profile, cross-region-inference-profile}, partition
   ∈ {aws, aws-us-gov, aws-cn}, region-set[], account, model-id/profile-id, underlying-fm-
   arns[], bedrock_api ∈ {converse, invoke}} → an IAM policy JSON granting ONLY the invoke
   actions matching the wire (`bedrock:Converse`/`ConverseStream` or `bedrock:InvokeModel`/
   `InvokeModelWithResponseStream`) over the EXACT resource-ARN set for the target type
   (per §1: foundation→fm ARN; profile→profile ARN + every destination-region fm ARN; etc.).
   **FAIL-CLOSED**: an unknown target type, an unresolved profile routing, a missing region
   set, or a missing ARN → return an ERROR (no policy), NEVER a broad `Resource:*` /
   `InvokeModel*`. All ARNs/partition/regions come from the (caller-supplied, but in
   production primary-authoritative catalog) target struct — a test asserts a client cannot
   widen the resource set.
5. **STS session cache (`sts_cache.{c,h}`)** — an instance-local, explicitly
   NON-authoritative cache of minted sessions. The **cache key is tenant-and-policy-
   complete**: org/team, provider key-slot + credential generation, RoleArn, ExternalId,
   AWS partition + sorted region set, the model-or-profile target, a normalized session-
   policy hash (SHA-256 of the canonical policy JSON), and (mode a) the workload-identity
   subject. A `≤900s` (15 min) TTL. `sts_cache_get(key, now)` returns a live entry ONLY on
   an EXACT key match AND not-expired AND generation-current; a generation bump (rotation /
   entitlement revocation) invalidates. **Negative isolation tests**: a session minted for
   one (team | model | region | role | credential-generation | policy-hash | subject) is
   NEVER returned for a different value of any of those — one differing field → miss.

## Scope (P6a)

1. `src/modules/aws/aws_sigv4.{c,h}` — SigV4 canonical request / string-to-sign / signing
   key / signature / Authorization header + X-Amz-* headers.
2. `src/modules/aws/aws_sts.{c,h}` — AssumeRole (signed) + AssumeRoleWithWebIdentity
   (unsigned) request construction; web-identity iss/aud/exp validation; STS XML response
   parse. (Construction + parse only — no dispatch.)
3. `src/modules/aws/bedrock_policy.{c,h}` — per-target-type least-privilege session-policy
   derivation, fail-closed.
4. `src/modules/aws/sts_cache.{c,h}` — the tenant-and-policy-complete cache-key + cache with
   generation/TTL invalidation.
5. Wire the four .o into the Makefile SERVER + KB object lists (the module is shared;
   like the vault core it links into both — confirm the module-boundary check passes; if
   AWS auth is kb-egress-only, put it in KB_SRCS only + assert no server linkage, mirroring
   how org_telemetry/org_rate are kb-only). DECIDE at implementation: AWS signing is a
   kb-egress authority concern (invariant #1 — org creds never on server), so **kb-only**
   is the correct placement; assert server has no aws symbols (mirror kb-target-isolation).
6. `src/tests/test_aws_auth.c` — unit tests: (a) SigV4 against the published AWS test-suite
   vectors (≥3 canonical cases incl. a query-string + a header-normalization case);
   (b) AssumeRoleWithWebIdentity body shape + iss/aud/exp validation (accept valid, reject
   wrong iss, wrong aud, expired); (c) AssumeRole SigV4-signed body; (d) STS XML response
   parse (valid + a missing-field → error); (e) session-policy derivation for ALL FIVE
   target types + fail-closed on unknown-type / missing-region-set / missing-ARN (assert NO
   Resource:* ever emitted); (f) STS cache isolation — the full negative matrix (differing
   team/model/region/role/generation/policy-hash/subject each → cache miss), TTL expiry,
   generation-bump invalidation. Registered as `unit-test-aws-auth`.

## Explicitly deferred (P6b/P6c — need egress/IR/live-or-mock AWS)

The Bedrock backend + driver dispatch; IR↔Converse serializer + native InvokeModel
adapters; the `application/vnd.amazon.eventstream` binary decoder (CRC + bounds + AWS error
frames — its own memory-safety-critical slice, fuzz-tested); catalog schema fields
(bedrock_api / model_family / target-type / partition / region-set) + validation
reject-unsupported-pair; org_model_pricing Bedrock rows (region+model+profile keyed);
the WORM-audited-signing egress path (P7 §6); the actual STS HTTP call + Bedrock invoke;
end-to-end attribution/budget. P6a is the crypto/authz FOUNDATION these call.

## Gate

- `make -j server` links clean (builds server + kb); the aws module is **kb-only** —
  `make kb-target-isolation-check` + `make module-boundary-check` green (assert no aws
  symbols in aimee-server).
- `make lint` (line-check, module-boundary, etc.) green; no .sql touched (no schema change
  in P6a). `make schema-sync-check` unaffected.
- `unit-test-aws-auth` builds + PASSES — the SigV4-vector, mode-correct-federation, policy-
  fail-closed, and cache-isolation assertions are the headline.
- No DB, no real-PG gate needed (P6a is pure/offline); the existing run-p1-rls-gate.sh is
  untouched.

## Non-goals (P6a)

No network I/O, no live AWS/STS/Bedrock call, no IR mapping, no eventstream decode, no
catalog/pricing schema, no egress wiring, no Vertex/Azure. Pure AWS SigV4 + STS-construction
+ least-privilege-policy + cache-isolation library, vector-tested offline, kb-only, that the
deferred Bedrock egress (P6b/c) will call.

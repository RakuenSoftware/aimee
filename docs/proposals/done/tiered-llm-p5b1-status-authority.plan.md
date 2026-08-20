# P5-B1: custodial management-status authority

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

- **State:** DONE — delivered scope archived 2026-07-26.
- **Depends on:** P5-B foundation and P7 signed-HWM KMS custody.

## Boundary

Deliver a separate, required-mTLS `aimee-kb-status` process that alone may query
management status and use one dedicated P7-custodied Ed25519 status key. This slice
does not add workload auto-enrollment, the KB health orchestrator, operator JWTs,
console routes, or management writes. `/management/action` remains unconditional 503.

## Linearized primary authority

- Add a platform-scoped status-key registry and immutable status-key-use intent table.
  Do not fake a tenant/team or grant generic org-vault key use.
- Add fixed SECURITY DEFINER candidate, admit, startup and use-guard functions, owned
  by the schema owner with definition-time safe search paths. They accept only a fixed
  platform slot, `management.status.sign.v1`, canonical digest/use-id, activated
  current version, open seal epoch and freshly verified signed-HWM version.
- The initial `kb_management_status_lookup` is tentative. Admission revalidates and
  locks the exact caller enrollment, target registry/enrollment and generation, and
  compares the expected generation and target fingerprint before use. This serializes
  issuance against revocation without holding a transaction over external HWM access.
  A changed generation restarts lookup and transcript construction.
- In that same short transaction, admission appends the use row and a secret-free
  `kb_audit_worm_append` event binding use id, transcript digest, key/version, HWM and
  seal epoch before returning one existing-format P7 encrypted envelope. Audit failure
  rolls back. Exact replay returns no envelope and a terminal conflict; changed replay
  inputs fail atomically.
- `aimee_kb_status` receives EXECUTE only on the fixed functions and existing lookup.
  It has no table access, ownership, role inheritance, generic vault/WORM privileges,
  BYPASSRLS, server-file or server-program privileges. Runtime roles cannot call the
  status functions.

## Custody and cross-process sealing

- Reuse `vault_hwm_read/verify`, `vault_use_epoch_snapshot/begin/end`, the existing
  P7 envelope parser and the protected mlock/MADV_DONTDUMP decrypt/use/cleanse block.
  Extract that sensitive block for shared internal use rather than duplicating it.
- The signing callback independently reconstructs the existing domain-separated
  status transcript, hashes it, derives the use id from a separate fixed domain, and
  admits exactly 32 raw Ed25519 seed bytes. There is no raw-key getter, caller-selected
  label/domain, PEM input, or generic sign API. It signs with `kb_mgmt_status_sign` and
  uses non-elidable cleansing on all exits; secrets never reach logs or responses.
- Because the status authority is a separate process, add a durable shared use guard:
  after admission, hold the fixed database seal/use lock while decrypting, signing and
  cleansing, and verify the admitted seal epoch again. Keep the local vault-use guard
  as well. Startup mirrors the durable control startup handshake and forces the local
  provider sealed when primary state is sealed. Provider, HWM, guard, audit, unwrap or
  signing errors have no fallback.
- Current live support is explicitly KMS signed-HWM only; TPM2/PKCS11 must not borrow
  the TPM root PolicyNV counter or pretend to implement per-key HWM.
- Add an owner-only, overwrite-refusing one-shot provision command that generates an
  Ed25519 seed, encrypts/activates it through the existing P7 envelope and configured
  KMS provider, outputs only key id/public key, and cleanses with a non-elidable call.

## Narrow service

- Build a minimal `aimee-kb-status` binary with its own listener/deployment unit and no
  linkage to the main KB router, schema migration, SQLite, CA signer or enrollment.
  Its PostgreSQL adapter uses `aimee_pg_open` directly and fails startup unless the
  effective/session roles are the intended nonsuperuser/non-BYPASSRLS status login.
- Expose only `POST /v1/management/status`. Extract the strict HTTP/1.1 reader from
  `kb_tls_serve.c` into a shared configurable parser. The authority uses 4 KiB status
  bodies, explicit request-line/header/header-count/read-deadline/connection/queue and
  in-flight-sign limits, rejects TE, duplicate/noncanonical CL, surplus bytes, unknown
  JSON fields/routes/methods and closes every response. Saturation is 503/close.
- Use a dedicated TLS context with `SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT`,
  a pinned CA, no generated fallback, and the repository TLS minimum. Require valid
  time, clientAuth, CN `p5-kb-management`, and exactly one noncritical profile OID
  `1.3.6.1.4.1.55555.5.1` whose value is `aimee-p5-kb-management-v1`.
- Derive issuer, normalized serial and DER fingerprint only from the verified leaf;
  request JSON carries only nonce, target, target-management fingerprint and purpose.
  Call `kb_mgmt_status_authority_issue` with the primary lookup and custody callbacks.
- Startup requires explicit TLS identity, status-role PG DSN, status key/public key,
  KMS custody and signed-HWM configuration. Missing or unhealthy dependencies at
  startup exit nonzero; mid-request PG/custody/HWM/audit/seal errors return generic 503,
  emit no status and close. Run non-root with core dumps disabled and hardened service
  limits; no plaintext listener, keepalive, bearer, redirect, proxy or admin route.

## Files and gates

- Schemas/grants plus new `db2/management_status_key.{c,h}`;
  `kb/kb_mgmt_status_custody.{c,h}`, `kb_mgmt_status_service.{c,h}` and
  `kb_mgmt_status_main.c`; shared strict HTTP/TLS identity helpers; minimal Makefile,
  install, container and systemd integration.
- Unit tests cover transcript/use-id, revocation/admission linearization, exact replay,
  audit-before-envelope, seal/HWM rollback, cleanup, strict framing and verified peer.
- Real PG17 proves grants and negative table/function access plus concurrency with
  revocation/seal. ASAN/UBSAN/leak and fuzz gates cover parsers and sensitive exits.
- CT260 uses real TLS, primary PostgreSQL and KMS signed-HWM custody. Validate happy
  issuance and wrong CA/EKU/profile/time, revoked caller, PG/service/custody/audit
  outage, seal, HWM rollback, replay, overload and malformed framing fail closed.
- Assert symbol/dependency isolation, no status key access from ordinary KB, no action
  route in the status binary, and unconditional 503 from server management action.

## Follow-ons

The owner-only KMS provision command is intentionally deferred to the dedicated
authority-process/bootstrap unit. Its prerequisite is the process-owned status-role
PostgreSQL connection and startup role verification plus an owner-only bootstrap
channel that can invoke P7 rotation without granting the online status role any
generic vault function. The command must create the fixed `org:p5-status` /
`management` / `ed25519` slot, publish only wire key id/public key, require an empty
registry and absent current slot, activate through signed-HWM KMS custody, and refuse
overwrite. Until that unit lands the registry remains empty/disabled, so the isolated
authority core has no reachable live signing path.

P5-B2 owns verified platform workload identity, distinct per-instance management-leaf
bootstrap/renewal, custodied identity storage, and renewal-expiry repair. P5-B3 owns
challenge -> authority -> health orchestration and CT260/CT262. P5-C owns management
JWT/JWKS/jti and WORM action intent/outcome; P5-D owns human OIDC/console/fleet UX.

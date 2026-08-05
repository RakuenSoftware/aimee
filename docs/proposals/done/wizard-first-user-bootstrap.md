# Proposal: Wizard-owned first-user bootstrap

- **State:** DONE — implemented, validated, and archived 2026-08-04.

> **Archived as complete.** The first-install/first-write bootstrap contract shipped in
> `498ebab2d` and `a8b1a5d47`, and the executed evidence recorded below covers the scoped
> administrator race, identity custody, restart, and first-write journey. Additional-user and
> credential-lifecycle work remains outside this proposal's deliberately narrow scope.

Implemented and validated 2026-07-28; frozen source diff and executed evidence are
  submitted with this decision record. Production `.210` state was not mutated; the managed stack was
  validated there in an isolated project.
- **Charter roles:** Authenticate / Bind / Grant / Enforce.
- **Depends on:** setup wizard webuser attestation, native TLS + client PKI, DB1, per-request write
  tiers.
- **Companion:** [per-user remote-write authorization](per-user-remote-writes-authz.md). This is the
  deliberately narrow first-administrator bootstrap carve-out; additional users still use the
  authority-managed PAM/OIDC/JWT path.

## Decision

Completing the setup wizard is a set of identity transactions, not merely `docker compose up`.

The authenticated browser user who first presses **Deploy** becomes the immutable first owner of that
appliance. The wizard creates an additive, random enrollment bearer for that user. The bearer can
authenticate the one CSR-signing operation but has no write tier. The client generates its private key
locally; when aimee-server signs the CSR, it binds the resulting certificate serial to the wizard
principal and activates an explicit `full` grant. A later request receives that tier only after the TLS
stack has verified the exact, non-revoked client certificate.

When the selected topology creates a local `aimee-llm`, the deploy boundary also creates a separate
256-bit KB service identity. It persists the bearer in the server's private volume, supplies the same
value and exact unified endpoint/role configuration to `aimee-kb` and `aimee-llm`, and makes every
embed, rerank, and synth client authenticate. The LLM refuses wildcard startup without that
credential. This token is never returned to the browser and cannot authorize server or KB routes.

The retired process-global `aimee.api.remote_writes` setting remains `off` and cannot participate in
the decision.

The four lifecycle decisions are closed:

1. The certificate-bound local first-administrator grant is the intentional appliance-bootstrap
   carve-out. Additional users and distributed installations use the full KB team/JWT authority.
2. This release scope is first installation and first write, not unattended certificate lifecycle.
   The 90-day certificate and UDS recovery model are explicit; renewal, additional devices, and owner
   transfer are release blockers before the product claims unattended operation beyond that window.
3. mTLS begins optional only for enrollment and automatically ramps to `required` after the durable
   roster has presented successfully. Additional-device enrollment therefore belongs to the scoped
   lifecycle work rather than leaving bearer fallback open indefinitely.
4. The automatic quickstart is Linux-only in this release. macOS and Windows remain supported as
   read clients but automatic first-owner CSR enrollment is a declared non-goal until native
   certificate-store implementations land; the UI and docs make no cross-platform quickstart claim.

## Root cause

The wizard previously started services but created no remote identity, certificate binding, or grant.
The user therefore completed setup with a shared bearer but no subject-specific authorization and the
first write failed.

The same container-only assumption existed one hop downstream: managed Compose exposed an optional
LLM token variable but left it empty, and the KB's native embedder, reranker sidecar, and curator
provider sent no bearer. Consequently setup created containers, not an authenticated KB-to-LLM
relationship. The service credential is now created before Compose starts; credential persistence or
propagation failure aborts deployment rather than falling back to a keyless gateway.

An additive-bearer change alone was also insufficient. The recovery-bootstrap gate checked whether the
server's *primary configured bearer* was still `aimee-local-dev`, not which credential authenticated
the current request. That caused every wizard-minted additive bearer to be rejected as
`enrollment_required` until somebody rotated the unrelated recovery credential. Credential
classification now happens inside the same locked authorization decision: only a request actually
authenticated by the recovery bearer is rotate-only.

The managed image also forced the retired global authorizer to `full`, contradicting the per-user
model and producing misleading diagnostics. The image and seeded config now pin it to `off`.

## Protocol and state

1. runtime-web forwards the logged-in PAM user over the local UDS using `server.token` plus
   `X-Aimee-Webuser`; aimee-server resolves `webuser:<name>`.
2. `POST /v1/deploy/apply` claims `remote_first_user(singleton=1)` **before** starting Docker. Another
   principal can never replace it.
3. The server generates 256 random bits. Cleartext is stored only in the existing protected Aimee
   config because bearer authentication needs it; DB1 stores its SHA-256 digest with the owner and
   proposed `full` tier. The deploy response is `Cache-Control: no-store`.
4. Re-entry by the same user returns the same pending enrollment command. A crash between the DB1
   claim and config persistence is detected and the never-authenticatable pending row is abandoned.
5. `aimee remote set` pins the server certificate. On Linux it creates a P-256 private key locally and
   sends only its signed CSR to `POST /v1/cert/sign` over native TLS with the enrollment bearer.
6. After PKI issuance is durable, the server binds the bearer digest to the issued certificate serial.
   A second serial for the same bearer is rejected and the just-issued certificate is revoked.
7. Each request first completes normal mTLS verification/revocation. DB1 may then resolve the verified
   serial to `webuser:<name>, full`; this tier is combined by maximum with any valid short-lived
   KB-signed tier. Lookup failure never widens access.
8. The resolved wizard principal replaces the transport-level `cert:<CN>` label in request context,
   vault attribution, and audit so setup and later activity name the same user.
9. For a local LLM profile, the server loads or atomically creates a private stable service bearer,
   removes any inherited empty/stale duplicate, and passes one authoritative value to Compose.
10. Compose gives the bearer and endpoint to the KB, requires the bearer for the LLM, enforces the
    gateway bind guard, and orders KB startup behind LLM health. Native embedding, batch embedding,
    reranking, Tier-A extraction, and Tier-B synthesis all use the credential and fail closed if it
    is absent. A pinned embedding dimension reaches both services and is enforced by the gateway.
11. Before reporting deploy success, the orchestrator executes an authenticated `/auth/verify` from
    inside the KB container. This proves the actual endpoint and bearer installed in the KB, without
    putting the secret in the host command line or browser response.

### Managed KB-to-LLM configuration contract

| Service | Exact managed configuration | Meaning |
| --- | --- | --- |
| `aimee-kb` | `AIMEE_LLM_URL=http://aimee-llm:8742` | unified embed/rerank/synth base |
| `aimee-kb` | `AIMEE_LLM_AUTH_TOKEN=<stable 256-bit bearer>` | service identity on every inference request |
| `aimee-kb` | `AIMEE_LLM_AUTH_REQUIRED=1` | missing service identity is a configuration failure, never a keyless downgrade |
| `aimee-kb` | `AIMEE_LLM_MODEL` (default `aimee-synth`) | chat/synthesis model label |
| `aimee-kb` | `LLM_API_KEY=$AIMEE_LLM_AUTH_TOKEN` | compatibility alias for curator sidecars |
| `aimee-kb` | `AIMEE_EMBEDDING_DIM` when pinned | DB2 vector width; otherwise derived from gateway health |
| `aimee-llm` | same `AIMEE_LLM_AUTH_TOKEN`, `AIMEE_LLM_STRICT_BIND=1` | same identity and fail-closed wildcard bind |
| `aimee-llm` | `AIMEE_LLM_{EMBED,RERANK,SYNTH}_{MODE,TIER,URL}` | per-role local/external/off routing and selected artifact tier |
| `aimee-llm` | `AIMEE_LLM_SYNTH_MODEL=$AIMEE_LLM_MODEL` | shared model label |
| `aimee-llm` | `AIMEE_EMBEDDING_DIM` when pinned | enforce the same vector width as the KB; empty derives the model-native width |

The KB calls `/embed`, `/embed_batch`, `/rerank`, `/v1/chat/completions`, and `/auth/verify` on the
unified base. If any role is external while another is local, the gateway receives and proxies that
role's explicit URL. Wizard-emitted values replace inherited values by key before Compose starts, so
an inherited empty/non-empty token or stale tier cannot shadow saved configuration. Deliberate
migration to an already-credentialed local gateway uses the distinct
`AIMEE_MANAGED_LLM_AUTH_TOKEN_OVERRIDE`; inherited `AIMEE_LLM_AUTH_TOKEN` is never interpreted as that
operator decision.

## Invariants

- The bearer alone never writes. During enrollment it reaches reads and CSR signing only; privileged
  routes return `403`. After the roster ramps to required, a bearer-only connection is rejected at the
  transport.
- The first owner is immutable across refresh, restart, and redeploy.
- DB1 contains no clear enrollment secret.
- CSR proof of private-key possession precedes activation of `full`.
- Only the exact verified certificate serial receives the local grant.
- Revoking that serial through PKI immediately removes its transport authority.
- `remote_writes=full`, a shared bearer, a session key, or an unverified certificate cannot produce the
  grant.
- UDS retains its existing un-lockout-able local-operator precedence.
- The KB-to-LLM token is distinct from every user credential, stored mode `0600`, stable across
  redeploys, absent when no local LLM is created, and never rendered by the wizard.
- A managed local LLM cannot start with authentication disabled.
- A managed KB cannot issue embed, batch, rerank, or synth requests without its service bearer.
- A pinned embedding dimension is identical on KB and LLM; model-output drift is rejected.

## User experience

Deploy displays one copyable command:

```text
aimee remote set https://<host>:<tls-port> <wizard-bearer>
```

On successful Linux enrollment, the first user can immediately use memory, indexing, agent, delegate,
and workspace routes at `full`. No team IDs, JWKS paths, SQL, manual bearer rotation, or grant command
are part of first-run setup. Additional users follow the authority-managed grant flow.

Automatic client-key/CSR enrollment is currently Linux-only. macOS and Windows remain fail-closed and
must not be documented as having completed first-user write setup until their native certificate-store
implementations exist.

Failure behavior is retryable and explicit. The wizard remains in `ready` and continues displaying the
same command until the certificate is bound. `aimee remote set` preserves `remote.conf`, never sends or
deletes the client private key, removes failed temporary CSR outputs, and reports whether TLS pinning,
verification, and mTLS enrollment completed. A network failure, unexpected certificate, CSR rejection,
or interrupted request before server commit does not activate the grant; after correcting connectivity
or verifying the fingerprint, the owner reruns the same command. If partial final key/certificate
evidence already exists, the client preserves it and fails closed for operator inspection instead of
overwriting it. The ambiguous window where the server durably binds a serial but the client loses the
successful response is not falsely described as retry-safe: it requires the local recovery authority
and belongs to the same renewal/reset lifecycle scope called out below.

## Recovery and lifecycle boundary

- Before pairing: the same wizard user can reload/redeploy and recover the same command.
- After pairing: Deploy returns `state=paired` and never returns the bearer again.
- Different wizard user: `403`, without revealing the owner credential.
- Lost/revoked/expired first-user certificate: local UDS remains the explicit recovery root. Automated
  renewal, owner-approved additional devices, and transfer/reset of first ownership are tracked
  lifecycle scope and must land before claiming unattended operation beyond 90 days. They are not part
  of this first-install/first-write change.

## Acceptance evidence

`make -C src wizard-bootstrap-e2e` starts a fresh real server and client with isolated state and proves:

- the server has no `AIMEE_SERVER_TEAM_ID`, server registry ID, or management JWKS bundle; the local
  first-owner path is deliberately independent of the later KB-issued team-token authority;
- wizard Deploy returns a pending bearer for `webuser:alice`;
- bearer-only persona write is `403`;
- `aimee remote set` generates the key/CSR and enrolls mTLS;
- the same write with the bound certificate is `200`;
- alice re-entry is paired and returns no bearer;
- bob cannot replace alice;
- DB1 records exactly one immutable owner and one bound `full` grant;
- DB1 stores the enrollment bearer digest, never cleartext;
- revoking the bound serial makes the next request fail at TLS/transport before grant lookup.

Unit tests separately cover transaction idempotence, conflicting owner refusal, hash-only DB state,
one-serial binding, authorization classification, certificate-tier resolution, and fail-closed lookup.
Frontend and runtime-web tests cover the rendered command and `no-store`; full lint and all shipping
binary boundary checks must remain green.

Managed-service tests additionally prove credential generation, `0600` persistence, restart
stability, explicit operator override, absence outside the local-LLM profile, removal of inherited
empty duplicates, gateway denial for absent/wrong bearers, client-side refusal when managed auth is
missing, bearer propagation through embed, rerank, and curator clients, and embedding-dimension drift
rejection.

### Executed validation

- `make -C src wizard-bootstrap-e2e` — PASS: bearer-only write denied, bound mTLS write accepted,
  hash-only DB1 state verified, ownership conflict denied, and revoked certificate denied.
- `src/build/obj/tests/unit-test-deploy-apply` — PASS: generated service identity is private, stable,
  scoped, override-safe, and probed without a host-argv secret.
- `src/build/obj/tests/unit-test-kb-curator-provider` — PASS: unified endpoint and service bearer reach
  both curator tiers.
- `src/build/obj/tests/unit-test-memory-embed-http-auth` — PASS: the native embed/batch client attaches
  its bearer and refuses missing or oversized managed credentials before transport.
- `python3 scripts/check-sidecar-clients.py` — PASS: embed and rerank clients attach the bearer and
  fail before transport when managed auth is required but absent.
- `python3 -m unittest -v scripts.tests.test_gateway scripts.tests.test_gateway_security` — 56 PASS,
  2 dependency-only skips; every inference route rejects absent/wrong managed tokens, the correct
  token succeeds, and a pinned dimension mismatch is a typed `503`.
- `python3 scripts/check-kb-container-packaging.py --root .` and `--plant-test` — PASS: the complete
  managed role/identity contract is now a guarded packaging invariant.
- `frontend` tests/check, `runtime-web` Go tests, `make -C src all`, `make -C src lint`,
  `make -C src aimee-home-check`, and `git diff --check` — PASS.
- Isolated `.210` Docker validation — PASS using a unique project, network, and fresh volumes:
  propagated identity and exact role config; missing/wrong tokens 401; authenticated embed, batch,
  rerank, and synth; empty-token wildcard bind refused. Test containers, image, network, volumes, and
  transferred source were removed; the production `aimee` project and volumes were untouched.

## Frozen implementation map

- First-owner transaction and recovery-bearer classification:
  `src/server/server_bearer_auth.c`, `src/server/server_http_config_routes.c`,
  `src/db1/remote_client_grant.c`, `src/db1/schema.sql`.
- Verified-cert tier/principal enforcement and revocation recheck:
  `src/server/server_http.c`, `src/server/server_http_identity.c`, `src/server/request_context.c`,
  `src/server/server_cert.c`.
- Wizard response and easy command: `runtime-web/deploy.go`, `frontend/src/setup/DeployPanel.tsx`,
  `api/openapi-server-v1.yaml`.
- Managed KB-to-LLM identity/config: `src/server/deploy_apply.c`,
  `deploy/container/aimee-managed.compose.yaml`, `scripts/aimee_llm_gateway.py`,
  `src/modules/memory/memory_core_helpers_b.c`, `src/kb_curator_provider.c`,
  `scripts/embed-remote.py`, `scripts/rerank-remote.py`.
- Regression proof: `scripts/test-wizard-first-user-bootstrap.sh`,
  `scripts/validate-managed-kb-llm-compose.sh`, `src/tests/test_remote_client_grant.c`,
  `src/tests/test_server_http.c`, `src/tests/test_deploy_apply.c`,
  `src/tests/test_kb_curator_provider.c`, `src/tests/test_memory_embed_http_auth.c`, and
  gateway/sidecar tests.

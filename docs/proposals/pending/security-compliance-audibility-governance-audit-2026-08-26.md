# Proposal: Security, compliance, audibility, and governance assurance program

- **State:** pending; source audit complete, remediation proposed
- **Audit baseline:** `origin/testing` at
  `a01a71c495fdb10a28821b154242cb7d0eb1d271` (2026-08-26)
- **Audit scope:** the tracked source tree, build and release automation, deployment artifacts,
  runtime trust boundaries, data stores, operator surfaces, and checked-in tests and evidence
- **Output type:** point-in-time source audit plus an implementation proposal; this is not a legal
  opinion, penetration-test certification, or third-party compliance attestation
- **Owner:** unassigned

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
- Findings will distinguish static evidence, executable test evidence, and live-deployment evidence.
  Source presence alone cannot establish operational effectiveness.

## Finding ledger

This table is updated as findings are verified. Candidate issues stay out of the severity totals.

| ID | Severity | Domain | Finding | Status |
| --- | --- | --- | --- | --- |
| ACG-001 | Critical | Authentication / deployment | Shipped KB topologies expose an authentication-off owner surface | verified; release-blocking |
| ACG-002 | High | Audit / governance | Governed-action WORM capture is default-off, best-effort, and may be silently lost | verified |
| ACG-003 | High | Audit integrity | WORM hashes do not bind timestamps or later identity/tenant attribution columns | verified |
| ACG-004 | High | Supply chain | Release workflows trust mutable actions and publish unattested artifacts | verified |
| ACG-005 | High | Dependency security | `server-go` reaches a known infinite-loop vulnerability in `golang.org/x/text` | verified |
| ACG-006 | Medium | Audit integrity | Audit-key failure emits a valid-looking all-zero argument-hash sentinel | verified |
| ACG-007 | Medium | Control-plane egress | Operator-editable JWKS URLs permit private-address and DNS-rebinding requests | verified |
| ACG-008 | Medium | Database hardening | Shipped DB1 uses a static password, plaintext transport, and the bootstrap superuser at runtime | verified |
| ACG-009 | Medium | Supply chain | Container builds fetch executable inputs without complete immutable verification | verified |
| ACG-010 | Low | Dependency security | The frontend development lock contains vulnerable `nanoid` 3.3.16 | verified |
| ACG-011 | High | Tenant isolation | Cross-project content and memory reads are fail-open until operator-gated scope controls are enabled | verified; release-blocking for multi-user deployment |
| ACG-012 | High | Audit confidentiality | Every authenticated bearer receives global dashboard and audit views without actor or tenant isolation | verified |
| ACG-013 | High | Filesystem authorization | `skill.show` trusts caller-selected roots and follows symlinks, permitting cross-workspace server-side file reads | verified |
| ACG-014 | High | Agentic security | Autonomous document-triggered execution is default-on while the integrity gate protects only one learning ingress | verified |
| ACG-015 | High | Audit completeness | Several live enforcement and privileged-action paths never reach the tamper-evident chain | verified |
| ACG-016 | High | Artifact trust | Project skills and other executable agent artifacts are unsigned, unpinned, and loaded as authoritative instructions | verified |
| ACG-017 | Medium | Accountability | Delegate and hook identity collapses to coarse or spoofable principals | verified |
| ACG-018 | Medium | Egress governance | Modules can make direct outbound calls and can silently omit the required governance event | verified |
| ACG-019 | Medium | Change governance | Sensitive-code ownership and independent approval are prose-only or incomplete in CODEOWNERS | verified |
| ACG-020 | Medium | Compliance readiness | Required organizational, privacy-lifecycle, and audit-evidence artifacts are absent from the source evidence set | verified source-evidence gap |
| ACG-021 | Medium | Build parity | Make and CMake products expose different audit/WORM capabilities | verified |
| ACG-022 | Medium | Availability | A hostile MCP SSE peer can drive unbounded buffering or an indefinitely blocked response drain | verified |
| ACG-023 | Medium | Gateway identity | Gateway pairing codes are predictable/non-unique and pairing state updates are not atomic across processes | verified |
| ACG-024 | Medium | Security testing | The checked-in nightly fuzz workflow is broken by a referenced but absent target source | verified by execution |
| ACG-025 | Medium | Assurance claims | The published security model states guarantees that shipped defaults and reachable paths contradict | verified |

Current verified total: **1 critical, 10 high, 13 medium, and 1 low**. Severity can move only when
new exploitability or compensating-control evidence is recorded in this document.

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

## Control assessment

Ratings describe the source baseline, including its shipped defaults. `Deficient` means a verified
gap prevents the stated objective; it does not mean no useful control exists.

| Domain | Rating | Effective controls and evidence | Material gaps |
| --- | --- | --- | --- |
| Security architecture and trust boundaries | Partial | Split client/server/KB processes, generated route inventory, binary link-boundary checks, documented hostile inputs | KB auth-off owner mode; optional direct module egress; managed topology grants Docker host control |
| Identity, authentication, and authorization | Deficient | Central server route-to-capability mapping, bearer enrollment, signed-write tiers, TLS/mTLS and certificate-pinning support | ACG-001, ACG-011, ACG-012, ACG-013, ACG-017 |
| Tool, delegate, workspace, and egress containment | Partial | Delegate containers can be networkless and resource-bounded; degraded sandbox fallback is refused; server tool policy exists | Unsigned instruction artifacts, incomplete ingress integrity, direct egress, skill root escape |
| Credential custody and cryptography | Partial | Vault ingestion scrubs bootstrap environment, hardened custody providers exist, secret fingerprints replace raw values in several logs, TLS 1.2 minimum is enforced where TLS is enabled | Auth-free KB path, weak DB1 secret, optional plaintext service links, audit hash coverage/key failure, no complete cryptographic agility or key-lifecycle evidence |
| Data protection, privacy, retention, and deletion | Deficient | Tenant-aware tables, export/delete functions, configurable redaction, encrypted secret custody | Read isolation is not default-on; global audit views; no authoritative data/purpose/retention/deletion/hold matrix or deletion verification |
| Audit completeness and tamper evidence | Deficient | Append-only triggers, hash chain, checkpoints, seals, witness support, verification APIs, default operational audit log | ACG-002, ACG-003, ACG-006, ACG-015, ACG-021; no proof that surviving records are complete |
| Governance, policy, approvals, and accountability | Deficient | Ownership prose, proposal lifecycle, module boundaries, generated API conformance, release environment hooks | No mechanical separation of duties, incomplete control ownership/evidence, coarse agent identity, no governed exception register |
| Secure development and supply chain | Deficient | Large native test suite, sanitizers, module boundary lint, locked Go/npm dependency graphs, digest assembly of multi-arch images | Reachable Go vulnerability, mutable CI actions, unattested releases, unverified build downloads, broken fuzz gate |
| Deployment and infrastructure hardening | Deficient | Rootless/delegated isolation options, split deployment, core-dump suppression, bootstrap secret scrubbing, health probes | Critical default KB exposure, superuser DB1, plaintext internal links, auth/rate limits vary by plane |
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

Do not promote the audited commit as an exposed or production-ready deployment while ACG-001 is
open. Do not represent it as safely multi-user or multi-tenant while ACG-011 through ACG-013 are
open. Do not represent its governed-action ledger as complete tamper-evident evidence while
ACG-002, ACG-003, ACG-006, ACG-015, or ACG-021 is open. No product source audit can itself support
a claim of SOC 2, ISO 27001, privacy-law, or other legal compliance.

For an already-running instance, containment precedes redesign: unpublish and network-isolate the
KB listener; set and rotate its bearer; prohibit anonymous owner/enrollment paths; restrict
dashboard and `skill.show` access; disable `scope=all` for ordinary identities; disable autonomous
triggers and live forge for shared/untrusted repositories; and upgrade `golang.org/x/text`. Any
instance that was reachable with an empty KB bearer must be treated as potentially owner-enrolled:
review enrollment/audit state and rotate dependent credentials.

### Phase 0 — containment and release freeze (0–72 hours)

| Work | Accountable owner | Evidence required to exit |
| --- | --- | --- |
| Block all auth-off KB topologies, remove default port publication, mint a least-privilege server credential, and isolate internal networks | Platform Security | Anonymous/sibling/host negative tests for every image/Compose profile; credential rotation record |
| Remove ordinary access to global dashboards, caller-selected skill roots and `scope=all` | Identity & Data Authorization | Route-policy diff plus two-user/two-project negative tests |
| Disable autonomous triggers/live forge in shared deployments until all content ingresses are gated | Agent Security | Configuration migration, startup posture report and hostile-ingress smoke tests |
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

Phase 1 depends on defining identity and canonical evidence schemas before broad migration. Roll out
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
- dependency vulnerability gates, all fuzz target builds/runs, secret/history scan, SAST, release
  input verification, SBOM, signed provenance and consumer signature verification pass;
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
| Targeted credential-pattern scan of tracked content | No apparent live credential verified | This is not a historical/entropy or external secret-platform scan |
| Semgrep, Trivy, Grype, osv-scanner, Gitleaks, ShellCheck, Bandit, cppcheck, clang-tidy, pip-audit, Syft and cargo-audit | Tools unavailable in audit environment | Their absence is an evidence limitation, not a clean result |

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

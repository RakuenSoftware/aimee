# What's new in 0.4.0

0.4.0 is a one-way upgrade. It removes the combined image, the work queue, the generic inference gateway,
the interactive TUI, and the generic RPC transport, and it will not read a 0.2 deployment back.
Read [Upgrading](UPGRADING.md) before you start, not after.

Everything below is measured against **v0.2.192**, the last public release.

This work was prepared inside the 0.3 series and ships as 0.4.0. A cycle that put a shared-memory
event bus under every daemon, moved the workflow control plane to Go, and retired five surfaces was
the wrong shape for a 0.3 patch, so `AIMEE_VERSION_SERIES` moved to `0.4` and the first release in
the series is `0.4.0`. Nothing was released as 0.3.0, and the baseline below is unchanged.

Two tags, `v0.2.196` dated 2026-07-27 and `v0.3.0`, appeared on the repository part-way through this
cycle. Neither is a release. Both were promoted mid-cycle in error, neither was announced, and the
work below continued for thousands of commits after them. If you installed from either, you have an
untested mid-cycle build rather than 0.4.0, and you are missing the fixes under
[If you installed from a mid-cycle tag](#if-you-installed-from-a-mid-cycle-tag).

## The event bus is the change everything else rests on

Every daemon now has a bounded shared-memory event bus. It is the largest architectural change in
this cycle.

- One C host routes typed events between private client queue pairs.
- C and pure-Go clients share frozen wire vectors; Go needs no cgo.
- Small payloads stay inline. Large payloads use generation-checked arena leases.
- Requests, replies, cancellation, fan-out, backpressure, overflow, and client reap have explicit
  contracts.
- The host stamps one sequence before routing and exposes one full-stream tap.
- Capture materializes payloads into CRC-checked records for exact observational replay.
- The measured dispatch path is about 134 ns per event against a 1,000 ns gate on the reference
  host.

The audit path is the first load-bearing consumer. Governed actions, memory mutations, semantic
guardrail events, vault access, sandbox isolation degradation, MCP activity, and tool outcomes now
use the bus instead of direct side logs. Accepted events drain to the WORM ledger or their typed
store. Graceful shutdown drains the rings and flushes capture; queue failure is counted and logged.

The same contract opens the next set of changes: separately shipped modules, external attachment,
workflow events, uniform policy checks, and full-stream telemetry. Those consumers land separately;
the bus does not pretend they are all complete today.

See [Event bus](EVENT_BUS.md).

## The C core is splitting into modules, and the control plane moved to Go

- The C core is being split into owned source modules with narrow public headers, dependency
  checks, descriptors, and generated module documentation.
- The workflow control plane moved to Go. The C server remains the runtime and storage owner while
  Go owns workflow scheduling and browser control-plane work.
- The old plugin loader is gone. Optional modules attach through explicit contracts instead of
  loading arbitrary code into the core.
- The MCP adapter can be installed into either server or KB and reports tool activity through the
  event bus.
- Configuration fields, `/v1` operations, and provider messages are moving to table-driven,
  versioned contracts instead of duplicate switch statements.

## Audit is hash-chained, and remote writes are refused until identity is configured

- The action audit store is hash-chained and checkpointed. Verification, sealing, snapshots,
  provenance, retrieval traces, and fidelity checks use the same WORM surface.
- Capture coverage moved from scattered call-site logging to the event-bus tap for migrated paths.
- Vault reads, sandbox posture, MCP calls, tool completion, and memory writes now carry a principal,
  verdict, and bounded evidence into the audit path.
- The credential vault is the single server-side store for agent keys and OAuth tokens. Plaintext
  agent-key files and per-session credential pushes are retired.
- mTLS enrollment issues an identity per thin client. Revocation is checked per request.
- Remote user writes now require a KB-signed identity, server/team/JWKS trust, and an exact subject
  grant. The old global `remote_writes` setting authorizes nothing.
- Organization catalogs add model allowlists, budgets, rates, spend reports, AWS Bedrock, and
  egress authority.
- TPM 2, PKCS#11, KMS, reseal recovery, and external WORM witnesses are available for hardened
  deployments.

## Workflows are typed and validated before a run starts

- The Go workflow engine schedules parallel slices, retries, review loops, live forge work, and
  merge recovery.
- Typed workflow blocks replace implicit pipeline steps. Validation checks edge targets, input ports,
  artifact types, required parameters, and roundtable structure before a run starts.
- Watched-proposal triggers can create runs. Trigger mode is recorded, but current Go scheduling does
  not change between `autonomous` and `interactive`.
- A human gate always parks. Autonomous mode cannot approve one.
- Roundtable findings feed the next author pass instead of disappearing at a boolean verdict.
- A complete pre-supplied proposal can advance without being rewritten; a failed attempt with no
  proposal still fails.
- Agent admission limits apply globally and per workflow. Saturated agents are routed around.
- A merge conflict, missing commit, lost replay, or exhausted gate returns a named terminal or
  parked state instead of silently advancing.

## Delegates run sandboxed, and a roundtable's findings feed the next pass

- New installs create their first agent in the wizard and ship one canonical default roundtable.
- Delegates route by role and persona, then retry another viable agent unless a seat is pinned.
- Roundtable seats run in parallel, can pin a model or choose randomly, and require repository
  evidence. A reasoning chair removes unsupported findings before the final result.
- Model context and output limits come from the registry. Retry preserves the tool contract.
- Write-capable delegates get isolated worktrees. Container delegates default to no network, no
  credentials, bounded processes, and an explicit toolchain.
- Every new session gets its own branch and worktree at session start, cut from the repository's
  default branch. This now covers MCP sessions, which previously ran against the shared checkout.
- Session worktree keys are derived from the whole session id. They were the first 16 characters,
  which collided for ids built on a shared prefix and let concurrent sessions overwrite each other in
  one checkout. Existing worktrees move to the new key on next session start; a clean one is
  reclaimed automatically, and one holding uncommitted or unpushed work is kept and reported.
- Package installation goes through a mediated cache and policy gate. Custom delegate images,
  package sets, and Dockerfiles are supported without giving the agent the Docker socket.
- Local CLI agents execute on the thin client when the workspace is remote. Their login and working
  tree stay there.
- Claude CLI delegation is opt-in because unattended use may not fit a personal subscription's
  terms.
- The host AI's own sub-agent launchers can be blocked so delegated work keeps the same policy,
  budget, worktree, and audit path.
- Roundtable cost caps are optional. When set, they include every seat and chair call.

## Routing is table-driven, and context is budgeted rather than truncated

- All provider traffic passes through one canonical request and response IR.
- OpenAI Chat Completions, OpenAI Responses, Anthropic Messages, Gemini, Mistral, Bedrock, and local
  OpenAI-compatible servers share the same policy and accounting stages.
- Response parsing accepts provider-specific wire shapes only at the edge. The core sees canonical
  text, reasoning, tool calls, usage, and errors.
- The economizer adds deterministic folding, provider-aware cache alignment, and command-aware tool
  output condensation. Full output is spilled for recovery.
- Model metadata, provider catalogs, quota, cost, cache, and fallback decisions are visible through
  the CLI and dashboard.

## Retrieval answers with evidence, and abstains when it has none

- The KB container can own a private PostgreSQL 18 cluster with pgvector and pgvectorscale. An
  export helper moves that data to an external PostgreSQL server.
- The KB owns embedding, retrieval, curation, and code-index storage. Embedding runs in the KB or at
  its configured endpoint; local synthesis uses a model-specific `aimee-llm-e2b` or
  `aimee-llm-e4b` sidecar, and remote synthesis uses its configured endpoint.
- Cross-repository symbol and dependency edges now feed caller lookup, search, and blast radius.
- CSS migration analysis adds a style graph, dead/conflicting-rule checks, and an optional isolated
  Chromium sidecar for computed-style verification.
- Thin clients push workspace and document bytes. The server does not read a remote client's path.
- Structured PDF ingestion can preserve coordinates, tables, visual crops, OCR, assets, and page
  citations behind separate feature gates.
- Retrieval gained typed facts, contradiction tracking, abstention, evidence audits, progressive
  disclosure, and configurable fusion.

## Recall reads the assertion store, and a failure that repeats becomes a reviewed procedure

The pieces for this existed before: bitemporal assertions, authority-ranked corrections, auditable
evidence mentions, candidate quarantine, reviewed promotion. What was missing was a loop joining
them that could be measured end to end. That loop now exists, and it is default-on for prompt
assembly.

- Recall searches the canonical assertion store directly. World time and belief time are independent
  axes, so "what was true then" and "what we believed then" are separate questions.
- Current assertions are what recall returns by default. Historical recall is opt-in, so a retired
  value never arrives mixed in with a live one.
- A model-extracted claim carries the exact byte span and a hash of the region it came from. A claim
  that cannot name its evidence does not commit.
- Lexical and vector legs fuse late, under a similarity floor, with bounded graph expansion that is
  temporally filtered and scope-checked at every hop. Each result carries its retrieval trace.
- Repeated failures and successful recoveries become evidence-linked observations once two
  independent sessions show the same thing. One session is a coincidence and does not qualify.
- An observation can raise a procedural proposal, which goes through the review gate already used
  for learning, carrying applicability, expiry, evidence, and rollback metadata. Nothing promotes
  itself.
- Context assembly is typed. Current assertions, historical assertions, episodes, summaries,
  observations, reviewed procedures, and recent working context each get a channel with its own
  budget, packing trace, watermark, and trust boundary.
- Retrieval sufficiency is scored separately from answer correctness, so a wrong answer over
  sufficient evidence and a wrong answer over missing evidence stop looking alike in the results.

The master assembler and each channel keep a request-level opt-out. See
[Knowledge](KNOWLEDGE.md) for the contract and the
[validation report](validation/temporal-assertion-learning-loop.md) for the deployment evidence.

## One managed stack replaces the combined image

- The all-in-one `aimee-combined` image is retired. Use the managed server or the split server and
  KB stack.
- New KB containers run PostgreSQL privately when `AIMEE_DB2_URL` is not set. Existing external
  databases remain supported.
- The server generates a dashboard login on first boot when the deployment supplies none, and prints
  it once to the container log. That login is a real local PAM account, not a separate credential
  store, so replacing it later is an ordinary account change.
- To choose the login yourself, seal `AIMEE_WEBCHAT_USER` and `AIMEE_WEBCHAT_PASSWORD` with
  `scripts/aimee-compose-vault-bootstrap.sh` before the first `up`. Exporting the two variables and
  running `docker compose up` does not work on the managed compose file: it keeps them out of the
  server's `environment:` block on purpose, because anything listed there stays in `Config.Env` for
  the life of the deployment. Supply both or neither.
- Linux, macOS, and Windows use the same DB-free thin client and native TLS backend.
- Server-to-KB mTLS pooling and resident thin-client HTTPS keep-alive now default on. The measured
  compression flags remain off because they saved bytes but missed the latency gate.
- A configured remote is exclusive. The client no longer falls back to a local Unix socket for a
  subset of hooks, optimization, or delegate probes.
- `aimee remote set` stores the supplied bearer, pins the server certificate, and enrolls Linux
  mTLS clients. It does not rotate the bearer. Verify the fingerprint out of band.
- The browser adds projects, git credentials, OAuth, SSH cloning, workflows, logs, settings, a live
  graph, and per-user VS Code.
- The dashboard is panel-based and user-configurable; operational logs moved to their own page.
  Settings now edits the allowlisted typed config instead of a copied subset.
- Remote index and workspace operations upload content from the thin client. Claude CLI execution
  can stay on that client with its existing login and worktree.
- The attention guard is inert unless enabled. Remote writes are fail-closed until identity trust
  and per-user grants are configured.

## What is gone, and what to use instead

- Interactive `aimee chat` and the bare-command TUI. Use the browser, MCP, ACP, or a compatible API
  front end.
- The `aimee work` queue and its routes, tools, and database tables. Export old rows before upgrade
  if you need them.
- `aimee migrate v2`, whose server operation had already been removed.
- The combined appliance image and its compose file.
- The generic `aimee-llm` gateway and the separate reranker. Embedding is a KB role. Local synthesis
  uses a model-specific `aimee-llm-e2b` or `aimee-llm-e4b` sidecar instead of a generic gateway.
- The legacy KB Unix-socket autostart path.
- Client-held plaintext agent credentials and the session credential-push endpoint.
- The generic `/v1/rpc` transport. Named `/v1` routes are authoritative.

## If you installed from a mid-cycle tag

The `v0.2.196` and `v0.3.0` tags were promoted in error part-way through this cycle and are not
releases. The cycle continued for more than 3,500 commits after the earlier one, so an installation
taken from either is missing the following. Each is a case where the deployment came up healthy and
did nothing useful, which is why they are listed here rather than folded into the sections above.

- **The generic `aimee-llm` gateway is retired.** Embedding is owned by the selected KB and can run
  inside it or at its configured endpoint. Local synthesis uses a model-specific sidecar; remote
  synthesis uses the KB's configured endpoint. After the wizard selects the
  bundled embedder, a fresh install embeds with no download and no second service. Set the embedder
  before you ingest. A later change is a data migration: the guarded reset handles a dimension
  change, while a same-dimension vector-space change needs a fresh DB2 and source re-ingestion.
- **A clean install could enrol no identity and store zero vectors.** The published config snapshot
  did not match what `legacy_config_read` returned on the cached path, so first-user enrolment failed
  silently and env-var deployments indexed nothing. Both are fixed, and the write and guarded
  dimension-reset routes are now reachable through the managed server.
- **An operator-supplied dashboard login was ignored.** The entrypoint sealed
  `AIMEE_WEBCHAT_USER`/`AIMEE_WEBCHAT_PASSWORD` into Vault and scrubbed them from the environment
  before the PAM account was provisioned, so every install generated a random account instead. The
  supplied pair is now recovered from Vault and provisioned as the account you asked for.
- **Tool-using delegates could not read their own worktree on a compose deploy.** `aimee-server`
  drives a sibling Docker daemon, so a workspace bind source expressed in the server's own container
  path does not exist on the daemon's host and Docker mounts an empty directory in its place. The
  entrypoint now derives the translation from its own mounts.
- **A model that accepts exactly one temperature was sent another.** Provider profiles could only
  supply a default, which any caller overrode, so an agent with no wire provider named had no way to
  pin the value its endpoint requires and every delegated call returned HTTP 400.
- **A delegated shell was gated on config read from disk rather than the live snapshot.** The
  sandbox accessor loaded a whole config on each call, so a containment decision could be made on
  state the published snapshot had not adopted.

Fixes for the KB connection pool, KB error surfacing, ingest durability, shared-cluster entrypoint
reuse, agent removal, session branch enforcement and workflow branch aliasing also landed in this
window.

## Do these seven things, in this order

1. Back up DB1, the KB database, `aimee.yaml`, `agents.json`, vault material, and TLS state.
2. Dump the old sibling PostgreSQL volume before moving to the embedded KB database. The compose
   change does not import it.
3. Export any old work-queue rows before starting the new server; the migration removes the tables.
4. Replace combined-image deployments with the managed or split stack.
5. Re-enroll thin clients and verify the presented certificate fingerprint.
6. Configure server/team/JWKS trust, then grant remote write tiers per exact subject.
7. Run `aimee audit verify`, `aimee status`, `aimee kb status`, and one read/write smoke test after
   the upgrade.

See the [Quickstart](QUICKSTART.md), [Security model](SECURITY.md), and
[generated configuration reference](gen/configuration.md) for the current contracts.

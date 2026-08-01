# What's new

This is the current testing tree compared with **v0.2.192**, the last public release.

## Event bus

Every daemon now has a bounded shared-memory event bus. It is the largest architectural change in
this cycle.

- One C host routes typed events between private client queue pairs.
- C and pure-Go clients share frozen wire vectors; Go needs no cgo.
- Small payloads stay inline. Large payloads use generation-checked arena leases.
- Requests, replies, cancellation, fan-out, backpressure, overflow, and client reap have explicit
  contracts.
- The host stamps one sequence before routing and exposes one full-stream tap.
- Capture materializes payloads into CRC-checked records for exact observational replay.
- Event-bus performance reports measure host enqueue through client dequeue. Publish a result only
  with its host, command, and raw output.

The audit path is the first load-bearing consumer. Governed actions, memory mutations, semantic
guardrail events, vault access, sandbox isolation degradation, MCP activity, and tool outcomes now
use the bus instead of direct side logs. Accepted events drain to the WORM ledger or their typed
store. Graceful shutdown drains the rings and flushes capture; queue failure is counted and logged.

The same contract opens the next set of changes: separately shipped modules, external attachment,
workflow events, uniform policy checks, and full-stream telemetry. Those consumers land separately;
the bus does not pretend they are all complete today.

See [Event bus](EVENT_BUS.md).

## Runtime and module boundaries

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

## Audit, identity, and policy

- The action audit store is hash-chained and checkpointed. Verification, sealing, snapshots,
  provenance, retrieval traces, and fidelity checks use the same WORM surface.
- Capture coverage moved from scattered call-site logging to the event-bus tap for migrated paths.
- Vault reads, sandbox posture, MCP calls, tool completion, and memory writes now carry a principal,
  verdict, and bounded evidence into the audit path.
- The credential vault is the single server-side store for agent keys and OAuth tokens. Plaintext
  agent-key files and per-session credential pushes are retired.
- mTLS enrollment issues an identity per thin client. Revocation is checked per request.
- Remote writes require both deployment posture and per-user authorization. A global
  `remote_writes` setting alone grants nothing.
- Organization catalogs add model allowlists, budgets, rates, spend reports, AWS Bedrock, and
  egress authority.
- TPM 2, PKCS#11, KMS, reseal recovery, and external WORM witnesses are available for hardened
  deployments.

## Workflows and autonomous development

- The Go workflow engine schedules parallel slices, retries, review loops, live forge work, and
  merge recovery.
- Typed workflow blocks replace implicit pipeline steps. Validation checks edges, inputs, loops,
  gates, and version hashes before a run starts.
- Triggers and cron can create runs. Trigger mode chooses autonomous or interactive handling.
- A human gate always parks. Autonomous mode cannot approve one.
- Roundtable findings feed the next author pass instead of disappearing at a boolean verdict.
- Agent admission limits apply globally and per workflow. Saturated agents are routed around.
- A merge conflict, missing commit, lost replay, or exhausted gate returns a named terminal or
  parked state instead of silently advancing.

## Delegates and roundtables

- Delegates route by role and persona, then retry another viable agent unless a seat is pinned.
- Roundtable seats run in parallel, can pin a model or choose randomly, and require repository
  evidence. A reasoning chair removes unsupported findings before the final result.
- Model context and output limits come from the registry. Retry preserves the tool contract.
- Write-capable delegates get isolated worktrees. Container delegates default to no network, no
  credentials, bounded processes, and an explicit toolchain.
- Package installation goes through a mediated cache and policy gate. Custom delegate images,
  package sets, and Dockerfiles are supported without giving the agent the Docker socket.
- Local CLI agents execute on the thin client when the workspace is remote. Their login and working
  tree stay there.
- Claude CLI delegation is opt-in because unattended use may not fit a personal subscription's
  terms.

## Models, routing, and context

- All provider traffic passes through one canonical request and response IR.
- OpenAI Chat Completions, OpenAI Responses, Anthropic Messages, Gemini, Mistral, Bedrock, and local
  OpenAI-compatible servers share the same policy and accounting stages.
- Response parsing accepts provider-specific wire shapes only at the edge. The core sees canonical
  text, reasoning, tool calls, usage, and errors.
- The economizer adds deterministic folding, provider-aware cache alignment, and command-aware tool
  output condensation. Full output is spilled for recovery.
- Model metadata, provider catalogs, quota, cost, cache, and fallback decisions are visible through
  the CLI and dashboard.

## Knowledge and code

- The KB container can own a private PostgreSQL 18 cluster with pgvector and pgvectorscale. An
  export helper moves that data to an external PostgreSQL server.
- The KB owns embedding, retrieval, curation, and code-index storage; inference stays in the
  separate `aimee-llm` service.
- Cross-repository symbol and dependency edges now feed caller lookup, search, and blast radius.
- CSS migration analysis adds a style graph, dead/conflicting-rule checks, and an optional isolated
  Chromium sidecar for computed-style verification.
- Thin clients push workspace and document bytes. The server does not read a remote client's path.
- Structured PDF ingestion can preserve coordinates, tables, visual crops, OCR, assets, and page
  citations behind separate feature gates.
- Retrieval gained typed facts, contradiction tracking, abstention, evidence audits, progressive
  disclosure, and configurable fusion.

## Deployment and clients

- The all-in-one `aimee-combined` image is retired. Use the managed server or the split server, KB,
  and inference stack.
- New KB containers run PostgreSQL privately when `AIMEE_DB2_URL` is not set. Existing external
  databases remain supported.
- `aimee-llm` chooses CPU or GPU tiers at runtime. CPU images can be pre-baked for offline use; GPU
  tiers keep models in a persistent volume.
- Linux, macOS, and Windows use the same DB-free thin client and native TLS backend.
- `aimee remote set` pins the server certificate, rotates the bootstrap bearer, and enrolls Linux
  mTLS clients. Verify the fingerprint out of band.
- The browser adds projects, git credentials, OAuth, SSH cloning, workflows, logs, settings, a live
  graph, and per-user VS Code.

## Removed

- Interactive `aimee chat` and the bare-command TUI. Use the browser, MCP, ACP, or a compatible API
  front end.
- The `aimee work` queue and its routes, tools, and database tables. Export old rows before upgrade
  if you need them.
- `aimee migrate v2`, whose server operation had already been removed.
- The combined appliance image and its compose file.
- The legacy KB Unix-socket autostart path.
- Client-held plaintext agent credentials and the session credential-push endpoint.
- The generic `/v1/rpc` transport. Named `/v1` routes are authoritative.

## Upgrade notes

1. Back up DB1, the KB database, `aimee.yaml`, `agents.json`, vault material, and TLS state.
2. Dump the old sibling PostgreSQL volume before moving to the embedded KB database. The compose
   change does not import it.
3. Export any old work-queue rows before starting the new server; the migration removes the tables.
4. Replace combined-image deployments with the managed or split stack.
5. Re-enroll thin clients and verify the presented certificate fingerprint.
6. Grant remote write tiers per user. Do not rely on `remote_writes` alone.
7. Run `aimee audit verify`, `aimee status`, `aimee kb status`, and one read/write smoke test after
   the upgrade.

See the [Quickstart](QUICKSTART.md), [Security model](SECURITY.md), and
[generated configuration reference](gen/configuration.md) for the current contracts.

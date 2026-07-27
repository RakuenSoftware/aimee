# Delegates

A delegate is a bounded agent selected for one role. It runs through aimee's admission, credential,
workspace, tool, budget, and audit paths.

```bash
aimee delegate review --persona reviewer "Review the current diff"
aimee delegate diagnose --persona engineer "Find the cause of this failure"
aimee delegate code --persona engineer "Implement the accepted fix"
```

## Roles and personas

The role says what the delegate may do. The persona says how it should approach the work.

Common roles:

| Role | Normal use | Tools |
| --- | --- | --- |
| `review` | code, plan, security, or documentation review | read-only |
| `diagnose` | trace a failure and identify the cause | read and verify |
| `explain` | summarize code or a decision | read-only |
| `search` | repository or knowledge research | read-only |
| `draft` | prose, proposal, or structured artifact | limited write when requested |
| `code` | implementation | read, write, verify |
| `refactor` | bounded structural change | read, write, verify |
| `validate` | tests and acceptance checks | read and execute |
| `execute` | operator-defined task | explicit toolset |

Aliases such as `implement` → `code` and `test` → `validate` are normalized by the CLI. Use the
installed help for the exact list.

Personas are named instruction sets such as engineer, reviewer, security, architect, or QA. They
also staff roundtable seats and workflow nodes. A persona does not add capabilities the role lacks.

## Routing

For an unpinned request, the router:

1. finds enabled agents serving the role;
2. removes agents that are down, saturated, over budget, or incompatible with the required tools;
3. prefers the configured route;
4. dispatches and records the attempt;
5. retries another viable agent when the failure is retryable.

The old fixed fallback chain is not authoritative. Routing follows current viability.

A pinned agent or model is different: it either runs that exact target or fails. This keeps a
roundtable seat, benchmark, or controlled workflow from changing identity silently.

Global and per-workflow admission limits apply before provider work starts. A stuck delegate cannot
hold a slot forever; heartbeat and reap recover abandoned admission.

## Configure agents

```bash
aimee agent list
aimee agent add --help
aimee agent local --help
aimee agent probe <name>
aimee agent enable <name>
aimee agent disable <name>
aimee agent remove <name>
```

`agent local` registers an OpenAI-compatible endpoint such as Ollama or llama.cpp. `agent add`
handles API providers, OAuth-backed adapters, local CLI agents, SSH, and configured container
backends. Probe checks the endpoint, model, execution slots, and a short completion.

Provider and model inspection:

```bash
aimee provider list --available
aimee provider show <name>
aimee provider test <name>
aimee model show <model>
```

Keep agent routing data in `agents.json`. Keep secrets in the vault.

## Credentials

The server resolves API keys and OAuth tokens only after it admits the turn. Plaintext agent keys in
client files are refused.

```bash
aimee vault unlock
aimee vault set <agent> <name> <secret>
aimee vault list
```

The workflow engine, prompt, delegate container, and result never receive vault material unless an
explicit tool contract grants one credential. Vault reads are audited through the event bus.

Local CLI agents use a different boundary. Their installed binary and login remain on the execution
host. For a detached remote workspace, aimee can send the command to the thin-client runner so the
working tree and login stay on the client.

Claude CLI is primary-only by default. Set `claude_cli_delegate_enabled` only after checking the
provider's terms for unattended use.

## Source authority

A delegate gets one of these source views:

- current read-only worktree;
- isolated writable worktree;
- detached workspace served by an authorized thin client;
- explicitly configured SSH or container workspace.

Write authority is not inferred from the prompt. It comes from the role, session/workflow, remote
grant, and backend. Canonical path checks keep access inside the assigned root.

Write-capable delegates branch from the current accepted base. A session or workflow owns the
worktree; unrelated delegates do not share it. Garbage collection removes abandoned worktrees after
the configured age, never an active one.

## Sandbox

The container backend defaults to:

- no network;
- no ambient provider or git credentials;
- a narrow workspace mount;
- bounded processes, memory, CPU, and time;
- an explicit image and toolchain;
- mediated package access.

Package requests go through a cache and policy gate. Custom images, package sets, and Dockerfiles are
operator inputs, not something an untrusted agent gets to build with the host Docker socket.

If isolation cannot be applied, execution fails unless the deployment explicitly permits a degraded
mode. Degradation publishes an audit event.

See [Delegate sandbox](DELEGATE_SANDBOX.md).

## Tool loop

A tool-capable turn repeats:

```text
canonical request -> model -> canonical tool call -> schema/policy check
                  -> backend execution -> bounded canonical result -> model
```

Tool calls and outcomes are audited through the event bus. Output limits come from the model
registry. Large command output is condensed when the economizer permits it; the full output is
spilled for recovery.

Retries preserve the tool contract. A retry does not remove tools just because a provider returned
an empty or malformed response.

## Durable jobs

Long tasks have durable state and heartbeat:

```bash
aimee jobs list
aimee jobs status <job-id>
aimee jobs logs <job-id>
aimee jobs cancel <job-id>
```

Cancellation is cooperative, then bounded by backend cleanup. Container and process reapers clean up
leaks even when the runtime cannot filter them by name.

Coordinated packet plans use `delegate plan`, `delegate launch`, and `job`. Workflows are preferred
when the job needs typed artifacts, gates, forge operations, and restart recovery.

## Roundtables

```bash
aimee ensemble aggregate --help
aimee ensemble roundtable --help
```

Roundtables run through the same delegate core. Seats execute in parallel, require evidence, and may
pin a model or choose a random viable reviewer. Cost folds into the originating session or workflow.

See [Roundtables](ENSEMBLE.md).

## When to delegate

Good delegate work has a bounded output and a clear acceptance condition:

- inspect a subsystem and report evidence;
- review a diff from one lens;
- reproduce a failure;
- write tests for an established contract;
- implement one independent slice;
- summarize a long artifact;
- compare provider or benchmark results.

Keep authority with the primary when the task needs an unresolved product decision, broad production
access, or constant cross-slice coordination.

## Failure behavior

| Failure | Result |
| --- | --- |
| no eligible agent | explicit admission failure |
| random target fails | retry another viable target within budget |
| pinned target fails | fail; no substitution |
| provider returns no usable content | diagnostic plus bounded retry |
| tool schema or policy fails | tool denied and audited |
| workspace path escapes | hard deny |
| sandbox isolation fails | fail or explicitly audited degradation |
| heartbeat stops | attempt reaped; durable job remains inspectable |
| budget or rate exhausted | no new dispatch; job fails or parks by owner contract |

Use `aimee agent probe`, `aimee jobs status`, provider diagnostics, and the server audit log. Preserve
the first failed attempt; later retries can hide the original cause.

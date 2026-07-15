# CLI Command Reference

> Auto-generated from `src/cli_help_data.h` by `scripts/gen-reference-docs.py`.
> Do not edit by hand; run `make -C src docs-gen` to regenerate.

`aimee` is a thin client: each command either runs a small local operation or forwards a typed request to `aimee-server`. Server-backed commands accept `--json` for machine-readable output. Run `aimee help <command>` for per-command help, or `aimee help --all` for every tier.

Total commands: 61

## Core commands

### `aimee chat`

Start the aimee primary-agent chat.

Subcommands:

```
  [message...]     Send one message, or start an interactive chat with no message
```

### `aimee config`

View and update configuration.

Subcommands:

```
  show             Show all config values
  get <key>        Get one config value
  set <key> <val>  Set one config value
```

### `aimee delegate`

Delegate a task to a sub-agent.

Subcommands:

```
  <role> "prompt"   Run a delegate in <role>: code, review, explain,
                   refactor, draft, execute, summarize, format, search,
                   diagnose, validate. Aliases: implement/build -> code,
                   test/check -> validate, inspect -> diagnose,
                   research -> execute. REQUIRES --persona NAME (e.g.
                   engineer, qa, security, reviewer, architect). --tools
                   enables tool use for roles that do not enable it by
                   default. See `aimee delegate <role> --help` for the full
                   flag set (--persona, --context-file, --via, etc.).
  plan             Generate read-only work packets from a proposal
  launch <plan>    Queue a reviewed packet plan into a coord job
  status <job_id> [job_id...]  Check background delegate status

Use `aimee jobs list|status|logs|cancel` for durable background delegate jobs.
```

### `aimee help`

Show help for a command.

### `aimee hooks`

Pre/post tool hooks.

### `aimee index`

Code indexing.

Subcommands:

```
  find             Find a symbol or identifier
  overview         List indexed projects
  list             List indexed projects
  scan             Scan workspaces and (re)build the index (--force)
  blast-radius     Show files affected by changes to a file
  structure        Show file structure
  callers          Find callers of a symbol
```

### `aimee init`

Run server initialization.

### `aimee insights`

Token usage totals over the last N days (--days N, default 30).

### `aimee kb`

Project knowledge base.

Subcommands:

```
  search <query>   Search the knowledge base
  build            Build the knowledge base for a project
  update           Update the knowledge base
  ingest           Ingest documents (status: ingest status)
  status           Show knowledge-base status
  docs push        Push docs into the knowledge base
```

### `aimee manuscript`

Novel-mode manuscript tools.

### `aimee mcp`

MCP registry and OSV package gate.

Subcommands:

```
  audit            List registered MCP servers and last OSV verdict
  recheck [name]   Force a fresh OSV query for all servers or one name
```

### `aimee memory`

Stored memory.

Subcommands:

```
  search           Search stored memory
  store            Store a memory
  list             List memories
  get              Read a memory by id
  read             Assemble current memory context
```

### `aimee rules`

Rule management (list, generate, delete).

Subcommands:

```
  list             List active rules
  generate         Generate a rules prompt
  delete           Delete one rule
```

### `aimee session-start`

SessionStart hook entry point.

### `aimee skill`

Project-scoped skill context injection.

Subcommands:

```
  list             List available skills
  show <name>      Print a skill body or support file
  create           Create a project skill from a markdown file
  patch            Patch a project skill by string replacement
  lifecycle        Apply stale/archive lifecycle transitions
  autostub         Propose capability skills for uncovered tools
```

### `aimee toolset`

Composable named toolsets.

Subcommands:

```
  list             List named toolsets
  show <name>      Show a toolset definition
  resolve <name>   Print the resolved tool list
```

### `aimee vault`

Per-user encrypted agent credentials.

Subcommands:

```
  unlock                       Unlock the vault (creates a local root key)
  set <agent> <name> <secret>  Store an encrypted credential
  list                         List stored credential names (no secrets)
  delete <agent> <name>        Remove a credential
  lock                         Lock the vault (evict the cached key)
```

### `aimee version`

Print version.

### `aimee wm`

Working memory (session-scoped scratch).

Subcommands:

```
  set              Store a value
  get              Read a value
  list             List values
```

## Advanced commands

### `aimee agent`

Sub-agent management.

Subcommands:

```
  list             List configured delegates
  add              Add or update a delegate provider
  local            Register/update a local OpenAI-compatible delegate
                   (--provider openai|llama-eval for request shaping)
  remove           Remove a configured delegate
  enable           Enable a configured delegate
  disable          Disable a configured delegate
  probe            Probe delegate endpoint, slots, and execution
```

### `aimee api`

Inspect the public /v1 HTTP API (aimee.api.*).

Subcommands:

```
  status           Show the loopback /v1 listener config and emit VS Code /
                   OpenAI-compatible model-provider setup snippets
```

### `aimee audit`

WORM audit store and retrieval evidence.

Subcommands:

```
  verify           Verify the WORM audit chain + checkpoint MACs
                   (exit 0=green, 1=amber, 2=red); the default with no subcommand
  checkpoint       Append a checkpoint committing the current chain head
  seal             Export an immutable, verifiable snapshot of the WORM store
  snapshot         Append a hash-chained metric.snapshot row
  trace            Audit a retrieval-evidence trace
  provenance       Audit source provenance for a retrieval event
  fidelity         Audit answer fidelity for a retrieval event
```

### `aimee aux`

Auxiliary model routing.

Subcommands:

```
  config           Show resolved aux task->provider/model mapping
  test <task> "<prompt>"
                   Execute a single auxiliary task call
```

### `aimee claude-proxy`

Route Claude Code through aimee's primary model.

Subcommands:

```
  enable [url] [token]  Point Claude Code at aimee's /v1/messages ingress
                        (url/token default to AIMEE_SERVER_URL/AIMEE_SERVER_TOKEN);
                        reroutes ALL Claude Code sessions to your primary agent
  disable               Restore Claude Code to its default endpoint
```

### `aimee code`

Code-health audit.

Subcommands:

```
  audit [dir] [--json] [--fix]   File-health audit (untested files,
                         TODO/FIXME markers, debt score) over the tree;
                         --fix is non-mutating and reports no safe fixes yet
  audit --graph [--project P] [--json]   Graph-derived checks via aimee-kb
                         (dead exports, import cycles, exact/near clones);
                         requires a configured server, kb, and code index
```

### `aimee cron`

Cron jobs and watchdog runs.

Subcommands:

```
  list             List configured cron jobs
  add <id>         Add or update a cron job
  show <id>        Show one configured cron job
  run <id>         Run one cron job now
  history <id>     Show recent cron job runs
  enable <id>      Enable a cron job
  disable <id>     Disable a cron job (--all for rollback)
  remove <id>      Remove a cron job
```

### `aimee curator`

Knowledge curator queries.

Subcommands:

```
  implements <topic>   What implements a topic
  synthesize <topic>   Synthesize knowledge on a topic
  contradictions       List contradictions
```

### `aimee delegate-backend`

Inspect/drive delegate execution backends.

Subcommands:

```
  list             List registered backends (local, ssh, docker, ...)
  exec             Run a command through a backend
                   --backend X --task-id Y [--image I] [--host H]
                   [--no-hibernate] "<cmd>"
```

### `aimee dogfood`

Qualitative memory/dogfood review reports.

Subcommands:

```
  tag              Label one dogfood record
  review           Summarize review state and close armed reminders
  report           Build a monthly dogfood report (--month YYYY-MM, --json)
```

### `aimee ensemble`

A panel of agents (mixture-of-agents, roundtable).

Subcommands:

```
  aggregate        Mixture-of-Agents ensemble aggregate
  roundtable       Multi-round agent roundtable
```

### `aimee episode`

Delegation episodes.

Subcommands:

```
  list             List recent delegation episodes
```

### `aimee graph`

Code-graph projection and explain.

Subcommands:

```
  sync-code        Project the code graph
  explain          Explain a code-graph relationship
```

### `aimee hud`

Real-time session status and HUD.

### `aimee identity`

Charter and working-profile inspection.

Subcommands:

```
  show             Show charter, local operator, and working profile
  snapshot         Write a working-profile snapshot (--out DIR)
  diff             Compare two snapshots (--flip-threshold N)
```

### `aimee job`

Coordinated parallel job management.

Subcommands:

```
  start <plan_id>  Create a coordinated job from an execution plan
  list             List recent coordinated jobs (--limit N)
  status <job_id>  Show coordinated job progress and tasks
  show <job_id>    Alias for status
  cancel <job_id>  Cancel a coordinated job and pending tasks
```

### `aimee jobs`

Durable delegate job inspection.

Subcommands:

```
  list             List recent delegate jobs (--limit N)
  status <job_id>  Show one delegate job, including heartbeat/tool state
  show <job_id>    Alias for status
  logs <job_id>    Print the recorded delegate result/log body
  cancel <job_id>  Cooperatively cancel a queued or running delegate job
```

### `aimee model`

Model capability metadata.

Subcommands:

```
  list             List known models (--capability <name>, --open-weights)
  show <model>     Show context, cost, flags, cutoff, deprecation
  refresh          Refresh model metadata cache
```

### `aimee notes`

Investigation notes.

Subcommands:

```
  search           Search investigation notes by content or title
```

### `aimee optimize`

Bandit optimization loop.

Subcommands:

```
  points                          List registered decision points
  baseline --point <name>         Show current arm posteriors for a point
  replay --point <name>           Emit a point's closed-decision log for replay
  replay-record --point <n> --file <f>  Record a replay result (benchmark_trace)
  run [--suite <s>] [--arm <a>]   Run the offline benchmark suite (ranks baseline vs on)
  compare --baseline <a> --candidate <b>  Per-metric delta between two arms
  promote --point <p> --candidate <a> [--guarded] [--apply]  Gate/apply a promotion (credible interval)
```

### `aimee pipeline`

Roundtable authoring pipelines.

Subcommands:

```
  start            Start an authoring pipeline from a one-line idea
  status           Show a pipeline's state, phase, latest review digest and gate
  list             List roundtable authoring pipelines
  advance          Drive one tick of the pipeline loop
  gate             Resolve a human gate (pass|fail)
  resume           Resume a pipeline from the durable ledger
  cancel           Cancel a pipeline and any in-flight roundtable
```

### `aimee profile`

Manage aimee profiles (create/list/show/delete/current).

Subcommands:

```
  create <name>    Create a profile directory
  list             List profiles
  show <name>      Show profile details
  delete <name>    Delete a profile (--force for non-interactive use)
  current          Print the active profile name
```

### `aimee provider`

Model provider profiles and catalogs.

Subcommands:

```
  list             List registered providers (--available, --json)
  show <name>      Show provider profile details
  models <name>    Fetch provider model catalog (--json)
  test <name>      Probe provider credentials and connectivity
  quota [name]     Show process-local credential pool quota state
```

### `aimee remote`

Point the thin client at a remote aimee-server.

Subcommands:

```
  set <url> [token]  Persist a remote server target
  status             Show the resolved transport and a health probe
  clear              Revert to the local Unix socket
```

### `aimee server`

Manage the local aimee-server.

Subcommands:

```
  start            Spawn aimee-server if not running
                   (use systemctl --user start aimee-server on systemd
                    boxes; this command is the cross-platform fallback)
  restart          SIGTERM the running server and spawn a fresh one
                   (run after update.sh, or when versions drift)
```

### `aimee session`

Session history.

Subcommands:

```
  list             List recent sessions
  show             Show one session
  close            Close one session
  brief            Show a persisted session-start brief
```

### `aimee status`

System health overview.

### `aimee trajectory`

Replayable session trajectories.

Subcommands:

```
  export           Export a session trajectory
  batch            Export trajectories in batch
```

### `aimee trigger`

Event-triggered autopilot runs.

Subcommands:

```
  fire             Queue a trigger run
  list             List trigger runs
  status           Show one trigger run
  cancel           Cancel a queued trigger run
```

### `aimee work`

Inter-session work queue.

Subcommands:

```
  add              Add a work item to the queue
  add-batch        Batch-add items (--from-proposals)
  claim            Claim the next pending item
  complete         Mark claimed item as done
  fail             Mark claimed item as failed
  list             List work items
  cancel           Cancel a pending item
  release          Release a claimed item back to pending
  clear            Remove items by status
  gc               Release stale claims
  sync-proposals   Close items whose proposal moved out of pending/
  stats            Show queue statistics
```

### `aimee workers`

Server worker-pool status.

### `aimee workflow`

Inspect & validate development workflows.

Subcommands:

```
  blocks                 List the composable block catalog
  validate <file.yaml>   Typed-graph validate a workflow definition
  show <file.yaml>       Print the canonical form + version hash
  list                   List workflows under $AIMEE_HOME/workflows
  new <file.yaml>        Scaffold a starter workflow
```

### `aimee workspace`

Workspace management (add, list, remove).

Subcommands:

```
  add <path>       Register a directory as a workspace and index its projects
  list             List configured workspaces and their indexed projects
  remove <path>    Unregister a workspace
```

### `aimee worktree`

Manage session worktrees (gc abandoned ones).

Subcommands:

```
  gc               Garbage-collect abandoned session worktrees
                   (--days N, default 14; --force; --dry-run)
```

## Admin commands

### `aimee acp-serve`

ACP stdio server (Agent Client Protocol) for editors like Zed.

### `aimee cert`

mTLS client certificate lifecycle (operator).

Subcommands:

```
  issue <cn> [--days N]        Issue a client cert (CN identity); key returned once
  list                         List issued certs (serial, CN, validity, revoked)
  revoke <serial>              Revoke a client cert by serial
```

### `aimee clean`

Remove local aimee configuration and integrations.

### `aimee eval`

Eval harness.

Subcommands:

```
  run <suite_dir>  Run an eval suite (--ablation <preset|all>, --runs N)
  results [suite]  Show recent eval results
```

### `aimee git`

Git helpers.

Subcommands:

```
  verify           Verify the current changes before merge
```

### `aimee mcp-serve`

MCP stdio bridge to aimee-server.

### `aimee migrate`

Data migration utility.

Subcommands:

```
  v2               Run the v2 data migration (long-running)
```

### `aimee repo`

Per-repo cross-repo trust.

Subcommands:

```
  trust            Set per-repo cross-repo trust
```

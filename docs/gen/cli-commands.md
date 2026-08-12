# CLI Command Reference

> Auto-generated from `src/cli_help_data.h` by `scripts/gen-reference-docs.py`.
> Do not edit by hand; run `make -C src docs-gen` to regenerate.

`aimee` is a thin client: each command either runs a small local operation or forwards a typed request to `aimee-server`. Server-backed commands accept `--json` for machine-readable output. Run `aimee help <command>` for per-command help, or `aimee help --all` for every tier.

Total commands: 67

## Core commands

### `aimee config`

View and update configuration.

Subcommands:

```
  show             Show all config values
  get <key>        Get one config value
  set <key> <val>  Set one config value
  deploy-env       Emit the compose env for this backend record
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
                   default. --scope bounded|whole_task caps how open-ended
                   the task may be (enforced against each agent's max_scope).
                   See `aimee delegate <role> --help` for the full flag set
                   (--persona, --context-file, --via, --scope, etc.).
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
  watch <name> <root>  Install git hooks that re-index after branch changes
  blast-radius     Show files affected by changes to a file
  structure        Show file structure
  span <file> [start] [end]  Read an exact line range (chainable with &&)
  callers          Find callers of a symbol
  investigate "<question>" [...]  Ask the index; several questions, one call
  hybrid "<phrase>" [...]  Search for a phrase, not a symbol (--scope all)
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
  grant set        Set one subject's write tier (--server, --team, --subject, --tier)
  grant show       Show one subject's grant (--server, --team, --subject)
  grant list       List grants (--server, --team, [--include-revoked])
  grant revoke     Revoke one subject's grant (--server, --team, --subject)
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
  get              Read a memory by id (--as-of <ts>: was it in force then?)
  read             Assemble current memory context
```

### `aimee persona`

Persona management.

Subcommands:

```
  list             List available personas
  show <name>      Print one persona
  edit <name>      Edit or create a persona in $EDITOR
  add <name>       Alias for edit
  rm <name>        Reset a built-in or remove a custom persona
```

### `aimee rules`

Rule management (list, generate, delete).

Subcommands:

```
  list             List active rules
  generate         Generate a rules prompt
  delete           Delete one rule
```

### `aimee self-update`

Update this thin client to the server release.

Subcommands:

```
  --check          Report whether an update is available
  --version vX.Y.Z  Install a specific release without downgrading
  --yes            Do not ask before replacing the binary
  --require-verify  Fail if the published SHA-256 cannot be verified
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

Deprecated alias for `model`.

Subcommands:

```
  Every subcommand of `aimee model`, kept working under the old name.
  A roster entry is one (endpoint, model) target, so it is now a MODEL;
  `aimee catalog` is the separate per-model capability metadata.
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

### `aimee catalog`

Model capability metadata.

Subcommands:

```
  list             List catalogued models (--capability <name>, --open-weights)
  show <model>     Show context, cost, flags, cutoff, deprecation
  refresh          Refresh model metadata cache
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

### `aimee codex`

Codex OAuth recovery.

Subcommands:

```
  reauth           Re-authenticate Codex after refresh is rejected
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

### `aimee css`

CSS migration analysis and render verification.

Subcommands:

```
  report           Show a CSS-health overview
  dead-rules | conflicts | duplicate-declarations | duplicate-selectors
  unresolved | important-audit | high-specificity | unused-vars | token-candidates
  migrate-enumerate | migrate-list | rules-doc | assert-conventions | conventions
  render-store <project> <unit> <before|after> <snapshot.json>
  render-capture <project> <unit> <before|after> <html-file> <css-file>
  render-verify <project> <unit>
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

### `aimee doctor`

Diagnose server runtime state.

Subcommands:

```
  forensics       Show current process generation and interrupted async runs
```

### `aimee dogfood`

Qualitative memory/dogfood review reports.

Subcommands:

```
  tag              Label one dogfood record
  review           Summarize review state and close armed reminders
  report           Build a monthly dogfood report (--month YYYY-MM, --json)
```

### `aimee economizer`

Economizer telemetry.

Subcommands:

```
  stats            Gateway-mutation counters, tool-condense savings, avoided-$
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

Model roster management.

Subcommands:

```
  list             List configured models
  add              Add or update a model
  setup            Run a provider's attended OAuth setup
  local            Register/update a local OpenAI-compatible model
                   (--provider openai|llama-eval for request shaping)
  remove           Remove a configured model
  enable           Enable a configured model
  disable          Disable a configured model
  probe            Probe a model's endpoint, slots, and execution
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
  list             List configured providers (--all, --available, --json)
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
  enroll            Rotate the bearer and enroll this client certificate
  trust             Pin the configured server certificate again
  status             Show the resolved transport and a health probe
  clear              Revert to the local Unix socket
```

### `aimee roles`

Delegate role templates.

Subcommands:

```
  list             List role templates
  show <role>      Print one role template
  edit <role>      Edit or create a template in $EDITOR
  rm <role>        Reset a built-in or remove a custom template
```

### `aimee roundtable`

Review an artifact with a configured roundtable.

Subcommands:

```
  review <artifact>  Run the configured roundtable review
                     --roundtable NAME selects a saved preset
                     --original-request TEXT supplies the governing request
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

### `aimee workers`

Server worker-pool status.

### `aimee workflow`

Author, validate, run, and inspect workflow graphs.

Subcommands:

```
  blocks                 List the composable block catalog
  validate <file.yaml>   Typed-graph validate a workflow definition
  show <file.yaml>       Print the canonical form + version hash
  list                   List workflows under $AIMEE_HOME/workflows
  new <file.yaml>        Scaffold a starter workflow
  run <name> --proposal <file> --repo <path> [--watch]
  status <id> [--events] Inspect one durable run
```

### `aimee workspace`

Workspace management (add, list, remove).

Subcommands:

```
  add <path>       Register a directory as a workspace and index its projects
  list             List configured workspaces and their indexed projects
  remove <path>    Unregister a workspace
  serve <id>       Run the authorized remote-workspace request loop
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

Git and GitHub operations (run on aimee-server).

Subcommands:

```
  Every command takes an optional primary word then key=value pairs:
    aimee git merge origin/testing      aimee git pr create title="..."
    aimee git sync                      aimee git log count=5

  status           Working tree status
  add <paths|-A>   Stage changes (-A includes new files)
  commit <msg>     Stage tracked changes and commit
  push [-f]        Push the session's branch
  pull / fetch     Bring refs down from a remote
  sync [base]      Make this branch current with its base (fetch + rebase)
  merge <ref>      Merge a ref in; conflicts are named and undone by default
  rebase <base>    Rebase onto a branch, same conflict handling
  cherry-pick <r>  Apply a commit here
  revert <ref>     Back a commit out
    ... any of the five above also take: continue | abort | skip
  switch <branch>  Move to a branch
  checkout <paths> Restore paths from a ref
  restore <paths>  Restore or unstage paths
  reset <ref>      soft/mixed/hard reset
  branch <action>  create/switch/list/delete/claim/orphan
  stash <action>   push/pop/apply/list/drop
  tag <action>     create/list/delete
  log / diff       History and diff summaries
  pr <action>      create/view/list/edit/checks/merge_status/merge/ready
                   (create writes its own title and body from your commits)
  issue list       Open issues
  clone <url>      Clone a repository
  verify           Verify the current changes before merge
```

### `aimee mcp-serve`

MCP stdio bridge to aimee-server.

### `aimee repo`

Per-repo cross-repo trust.

Subcommands:

```
  trust            Set per-repo cross-repo trust
```

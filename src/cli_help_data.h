/* cli_help_data.h: client command help table entries for cli_main.c.
 *
 * Extracted verbatim from the `client_help[]` initializer to keep cli_main.c
 * under the source line-count limit (mirrors agent_help_data.h). Included once,
 * inside the array initializer in cli_main.c; the client_help_t type and the
 * surrounding `static const client_help_t client_help[] = { ... };` live there.
 */
{"init", "Run server initialization", CLIENT_TIER_CORE, 1, NULL},
    {"doctor", "Diagnose server runtime state", CLIENT_TIER_ADVANCED, 1,
     "  forensics       Show current process generation and interrupted async runs\n"},
    {"manuscript", "Novel-mode manuscript tools", CLIENT_TIER_CORE, 0, NULL},
    {"claude-proxy", "Route Claude Code through aimee's primary model", CLIENT_TIER_ADVANCED, 0,
     "  enable [url] [token]  Point Claude Code at aimee's /v1/messages ingress\n"
     "                        (url/token default to AIMEE_SERVER_URL/AIMEE_SERVER_TOKEN);\n"
     "                        reroutes ALL Claude Code sessions to your primary agent\n"
     "  disable               Restore Claude Code to its default endpoint\n"},
    {"remote", "Point the thin client at a remote aimee-server", CLIENT_TIER_ADVANCED, 0,
     "  set <url> [token]  Persist a remote server target\n"
     "  enroll            Rotate the bearer and enroll this client certificate\n"
     "  trust             Pin the configured server certificate again\n"
     "  status             Show the resolved transport and a health probe\n"
     "  clear              Revert to the local Unix socket\n"},
    {"wm", "Working memory (session-scoped scratch)", CLIENT_TIER_CORE, 0,
     "  set              Store a value\n"
     "  get              Read a value\n"
     "  list             List values\n"},
    {"index", "Code indexing", CLIENT_TIER_CORE, 0,
     "  find             Find a symbol or identifier\n"
     "  overview         List indexed projects\n"
     "  list             List indexed projects\n"
     "  scan             Scan workspaces and (re)build the index (--force)\n"
     "  watch <name> <root>  Install git hooks that re-index after branch changes\n"
     "  blast-radius     Show files affected by changes to a file\n"
     "  structure        Show file structure\n"
     "  span <file> [start] [end]  Read an exact line range (chainable with &&)\n"
     "  callers          Find callers of a symbol\n"
     "  investigate \"<question>\" [...]  Ask the index; several questions, one call\n"
     "  hybrid \"<phrase>\" [...]  Search for a phrase, not a symbol (--scope all)\n"},
    {"workspace", "Workspace management (add, list, remove)", CLIENT_TIER_ADVANCED, 0,
     "  add <path>       Register a directory as a workspace and index its projects\n"
     "  list             List configured workspaces and their indexed projects\n"
     "  remove <path>    Unregister a workspace\n"
     "  serve <id>       Run the authorized remote-workspace request loop\n"},
    {"memory", "Stored memory", CLIENT_TIER_CORE, 0,
     "  search           Search stored memory\n"
     "  store            Store a memory\n"
     "  list             List memories\n"
     "  get              Read a memory by id (--as-of <ts>: was it in force then?)\n"
     "  read             Assemble current memory context\n"},
    {"economizer", "Economizer telemetry", CLIENT_TIER_ADVANCED, 0,
     "  stats            Gateway-mutation counters, tool-condense savings, avoided-$\n"},
    {"session", "Session history", CLIENT_TIER_ADVANCED, 0,
     "  list             List recent sessions\n"
     "  show             Show one session\n"
     "  close            Close one session\n"
     "  brief            Show a persisted session-start brief\n"},
    {"rules", "Rule management (list, generate, delete)", CLIENT_TIER_CORE, 0,
     "  list             List active rules\n"
     "  generate         Generate a rules prompt\n"
     "  delete           Delete one rule\n"},
    {"skill", "Project-scoped skill context injection", CLIENT_TIER_CORE, 0,
     "  list             List available skills\n"
     "  show <name>      Print a skill body or support file\n"
     "  create           Create a project skill from a markdown file\n"
     "  patch            Patch a project skill by string replacement\n"
     "  lifecycle        Apply stale/archive lifecycle transitions\n"
     "  autostub         Propose capability skills for uncovered tools\n"},
    {"toolset", "Composable named toolsets", CLIENT_TIER_CORE, 0,
     "  list             List named toolsets\n"
     "  show <name>      Show a toolset definition\n"
     "  resolve <name>   Print the resolved tool list\n"},
    {"mcp", "MCP registry and OSV package gate", CLIENT_TIER_CORE, 0,
     "  audit            List registered MCP servers and last OSV verdict\n"
     "  recheck [name]   Force a fresh OSV query for all servers or one name\n"},
    {"hooks", "Pre/post tool hooks", CLIENT_TIER_CORE, 1, NULL},
    {"worktree", "Manage session worktrees (gc abandoned ones)", CLIENT_TIER_ADVANCED, 0,
     "  gc               Garbage-collect abandoned session worktrees\n"
     "                   (--days N, default 14; --force; --dry-run)\n"},
    {"insights", "Token usage totals over the last N days (--days N, default 30)", CLIENT_TIER_CORE,
     0, NULL},
    {"server", "Manage the local aimee-server", CLIENT_TIER_ADVANCED, 0,
     "  start            Spawn aimee-server if not running\n"
     "                   (use systemctl --user start aimee-server on systemd\n"
     "                    boxes; this command is the cross-platform fallback)\n"
     "  restart          SIGTERM the running server and spawn a fresh one\n"
     "                   (run after update.sh, or when versions drift)\n"},
    {"api", "Inspect the public /v1 HTTP API (aimee.api.*)", CLIENT_TIER_ADVANCED, 0,
     "  status           Show the loopback /v1 listener config and emit VS Code /\n"
     "                   OpenAI-compatible model-provider setup snippets\n"},
    {"session-start", "SessionStart hook entry point", CLIENT_TIER_CORE, 1, NULL},
    {"delegate", "Delegate a task to a sub-agent", CLIENT_TIER_CORE, 0,
     "  <role> \"prompt\"   Run a delegate in <role>: code, review, explain,\n"
     "                   refactor, draft, execute, summarize, format, search,\n"
     "                   diagnose, validate. Aliases: implement/build -> code,\n"
     "                   test/check -> validate, inspect -> diagnose,\n"
     "                   research -> execute. REQUIRES --persona NAME (e.g.\n"
     "                   engineer, qa, security, reviewer, architect). --tools\n"
     "                   enables tool use for roles that do not enable it by\n"
     "                   default. --scope bounded|whole_task caps how open-ended\n"
     "                   the task may be (enforced against each agent's max_scope).\n"
     "                   See `aimee delegate <role> --help` for the full flag set\n"
     "                   (--persona, --context-file, --via, --scope, etc.).\n"
     "  plan             Generate read-only work packets from a proposal\n"
     "  launch <plan>    Queue a reviewed packet plan into a coord job\n"
     "  status <job_id> [job_id...]  Check background delegate status\n"
     "\n"
     "Use `aimee jobs list|status|logs|cancel` for durable background delegate jobs.\n"},
    {"jobs", "Durable delegate job inspection", CLIENT_TIER_ADVANCED, 0,
     "  list             List recent delegate jobs (--limit N)\n"
     "  status <job_id>  Show one delegate job, including heartbeat/tool state\n"
     "  show <job_id>    Alias for status\n"
     "  logs <job_id>    Print the recorded delegate result/log body\n"
     "  cancel <job_id>  Cooperatively cancel a queued or running delegate job\n"},
    {"catalog", "Model capability metadata", CLIENT_TIER_ADVANCED, 0,
     "  list             List catalogued models (--capability <name>, --open-weights)\n"
     "  show <model>     Show context, cost, flags, cutoff, deprecation\n"
     "  refresh          Refresh model metadata cache\n"},
    {"job", "Coordinated parallel job management", CLIENT_TIER_ADVANCED, 0,
     "  start <plan_id>  Create a coordinated job from an execution plan\n"
     "  list             List recent coordinated jobs (--limit N)\n"
     "  status <job_id>  Show coordinated job progress and tasks\n"
     "  show <job_id>    Alias for status\n"
     "  cancel <job_id>  Cancel a coordinated job and pending tasks\n"},
    {"delegate-backend", "Inspect/drive delegate execution backends", CLIENT_TIER_ADVANCED, 0,
     "  list             List registered backends (local, ssh, docker, ...)\n"
     "  exec             Run a command through a backend\n"
     "                   --backend X --task-id Y [--image I] [--host H]\n"
     "                   [--no-hibernate] \"<cmd>\"\n"},
    {"model", "Model roster management", CLIENT_TIER_ADVANCED, 0,
     "  list             List configured models\n"
     "  add              Add or update a model\n"
     "  setup            Run a provider's attended OAuth setup\n"
     "  local            Register/update a local OpenAI-compatible model\n"
     "                   (--provider openai|llama-eval for request shaping)\n"
     "  remove           Remove a configured model\n"
     "  enable           Enable a configured model\n"
     "  disable          Disable a configured model\n"
     "  probe            Probe a model's endpoint, slots, and execution\n"},
    {"agent", "Deprecated alias for `model`", CLIENT_TIER_ADVANCED, 0,
     "  Every subcommand of `aimee model`, kept working under the old name.\n"
     "  A roster entry is one (endpoint, model) target, so it is now a MODEL;\n"
     "  `aimee catalog` is the separate per-model capability metadata.\n"},
    {"codex", "Codex OAuth recovery", CLIENT_TIER_ADVANCED, 0,
     "  reauth           Re-authenticate Codex after refresh is rejected\n"},
    {"persona", "Persona management", CLIENT_TIER_CORE, 0,
     "  list             List available personas\n"
     "  show <name>      Print one persona\n"
     "  edit <name>      Edit or create a persona in $EDITOR\n"
     "  add <name>       Alias for edit\n"
     "  rm <name>        Reset a built-in or remove a custom persona\n"},
    {"roles", "Delegate role templates", CLIENT_TIER_ADVANCED, 0,
     "  list             List role templates\n"
     "  show <role>      Print one role template\n"
     "  edit <role>      Edit or create a template in $EDITOR\n"
     "  rm <role>        Reset a built-in or remove a custom template\n"},
    {"provider", "Model provider profiles and catalogs", CLIENT_TIER_ADVANCED, 0,
     "  list             List configured providers (--all, --available, --json)\n"
     "  show <name>      Show provider profile details\n"
     "  models <name>    Fetch provider model catalog (--json)\n"
     "  test <name>      Probe provider credentials and connectivity\n"
     "  quota [name]     Show process-local credential pool quota state\n"},
    {"version", "Print version", CLIENT_TIER_CORE, 0, NULL},
    {"self-update", "Update this thin client to the server release", CLIENT_TIER_CORE, 0,
     "  --check          Report whether an update is available\n"
     "  --version vX.Y.Z  Install a specific release without downgrading\n"
     "  --yes            Do not ask before replacing the binary\n"
     "  --require-verify  Fail if the published SHA-256 cannot be verified\n"},
    {"help", "Show help for a command", CLIENT_TIER_CORE, 1, NULL},

    {"status", "System health overview", CLIENT_TIER_ADVANCED, 0, NULL},
    {"hud", "Real-time session status and HUD", CLIENT_TIER_ADVANCED, 0, NULL},
    {"identity", "Charter and working-profile inspection", CLIENT_TIER_ADVANCED, 0,
     "  show             Show charter, local operator, and working profile\n"
     "  snapshot         Write a working-profile snapshot (--out DIR)\n"
     "  diff             Compare two snapshots (--flip-threshold N)\n"},
    {"dogfood", "Qualitative memory/dogfood review reports", CLIENT_TIER_ADVANCED, 0,
     "  tag              Label one dogfood record\n"
     "  review           Summarize review state and close armed reminders\n"
     "  report           Build a monthly dogfood report (--month YYYY-MM, --json)\n"},
    {"eval", "Eval harness", CLIENT_TIER_ADMIN, 1,
     "  run <suite_dir>  Run an eval suite (--ablation <preset|all>, --runs N)\n"
     "  results [suite]  Show recent eval results\n"},
    {"trigger", "Event-triggered autopilot runs", CLIENT_TIER_ADVANCED, 0,
     "  fire             Queue a trigger run\n"
     "  list             List trigger runs\n"
     "  status           Show one trigger run\n"
     "  cancel           Cancel a queued trigger run\n"},
    {"aux", "Auxiliary model routing", CLIENT_TIER_ADVANCED, 0,
     "  config           Show resolved aux task->provider/model mapping\n"
     "  test <task> \"<prompt>\"\n"
     "                   Execute a single auxiliary task call\n"},
    {"audit", "WORM audit store and retrieval evidence", CLIENT_TIER_ADVANCED, 0,
     "  verify           Verify the WORM audit chain + checkpoint MACs\n"
     "                   (exit 0=green, 1=amber, 2=red); the default with no subcommand\n"
     "  checkpoint       Append a checkpoint committing the current chain head\n"
     "  seal             Export an immutable, verifiable snapshot of the WORM store\n"
     "  snapshot         Append a hash-chained metric.snapshot row\n"
     "  trace            Audit a retrieval-evidence trace\n"
     "  provenance       Audit source provenance for a retrieval event\n"
     "  fidelity         Audit answer fidelity for a retrieval event\n"},
    {"ensemble", "A panel of agents (mixture-of-agents, roundtable)", CLIENT_TIER_ADVANCED, 0,
     "  aggregate        Mixture-of-Agents ensemble aggregate\n"
     "  roundtable       Multi-round agent roundtable\n"},
    {"roundtable", "Review an artifact with a configured roundtable", CLIENT_TIER_ADVANCED, 0,
     "  review <artifact>  Run the configured roundtable review\n"
     "                     --roundtable NAME selects a saved preset\n"
     "                     --original-request TEXT supplies the governing request\n"},
    {"pipeline", "Roundtable authoring pipelines", CLIENT_TIER_ADVANCED, 0,
     "  start            Start an authoring pipeline from a one-line idea\n"
     "  status           Show a pipeline's state, phase, latest review digest and gate\n"
     "  list             List roundtable authoring pipelines\n"
     "  advance          Drive one tick of the pipeline loop\n"
     "  gate             Resolve a human gate (pass|fail)\n"
     "  resume           Resume a pipeline from the durable ledger\n"
     "  cancel           Cancel a pipeline and any in-flight roundtable\n"},
    {"notes", "Investigation notes", CLIENT_TIER_ADVANCED, 0,
     "  search           Search investigation notes by content or title\n"},
    {"workers", "Server worker-pool status", CLIENT_TIER_ADVANCED, 0, NULL},
    {"repo", "Per-repo cross-repo trust", CLIENT_TIER_ADMIN, 0,
     "  trust            Set per-repo cross-repo trust\n"},
    {"cron", "Cron jobs and watchdog runs", CLIENT_TIER_ADVANCED, 0,
     "  list             List configured cron jobs\n"
     "  add <id>         Add or update a cron job\n"
     "  show <id>        Show one configured cron job\n"
     "  run <id>         Run one cron job now\n"
     "  history <id>     Show recent cron job runs\n"
     "  enable <id>      Enable a cron job\n"
     "  disable <id>     Disable a cron job (--all for rollback)\n"
     "  remove <id>      Remove a cron job\n"},

    {"git", "Git and GitHub operations (run on aimee-server)", CLIENT_TIER_ADMIN, 0,
     "  Every command takes an optional primary word then key=value pairs:\n"
     "    aimee git merge origin/testing      aimee git pr create title=\"...\"\n"
     "    aimee git sync                      aimee git log count=5\n"
     "\n"
     "  status           Working tree status\n"
     "  add <paths|-A>   Stage changes (-A includes new files)\n"
     "  commit <msg>     Stage tracked changes and commit\n"
     "  push [-f]        Push the session's branch\n"
     "  pull / fetch     Bring refs down from a remote\n"
     "  sync [base]      Make this branch current with its base (fetch + rebase)\n"
     "  merge <ref>      Merge a ref in; conflicts are named and undone by default\n"
     "  rebase <base>    Rebase onto a branch, same conflict handling\n"
     "  cherry-pick <r>  Apply a commit here\n"
     "  revert <ref>     Back a commit out\n"
     "    ... any of the five above also take: continue | abort | skip\n"
     "  switch <branch>  Move to a branch\n"
     "  checkout <paths> Restore paths from a ref\n"
     "  restore <paths>  Restore or unstage paths\n"
     "  reset <ref>      soft/mixed/hard reset\n"
     "  branch <action>  create/switch/list/delete/claim/orphan\n"
     "  stash <action>   push/pop/apply/list/drop\n"
     "  tag <action>     create/list/delete\n"
     "  log / diff       History and diff summaries\n"
     "  pr <action>      create/view/list/edit/checks/merge_status/merge/ready\n"
     "                   (create writes its own title and body from your commits)\n"
     "  issue list       Open issues\n"
     "  clone <url>      Clone a repository\n"
     "  verify           Verify the current changes before merge\n"},
    {"clean", "Remove local aimee configuration and integrations", CLIENT_TIER_ADMIN, 0, NULL},
    {"mcp-serve", "MCP stdio bridge to aimee-server", CLIENT_TIER_ADMIN, 1, NULL},
    {"acp-serve", "ACP stdio server (Agent Client Protocol) for editors like Zed",
     CLIENT_TIER_ADMIN, 1, NULL},
    {"profile", "Manage aimee profiles (create/list/show/delete/current)", CLIENT_TIER_ADVANCED, 0,
     "  create <name>    Create a profile directory\n"
     "  list             List profiles\n"
     "  show <name>      Show profile details\n"
     "  delete <name>    Delete a profile (--force for non-interactive use)\n"
     "  current          Print the active profile name\n"},
    {"config", "View and update configuration", CLIENT_TIER_CORE, 1,
     "  show             Show all config values\n"
     "  get <key>        Get one config value\n"
     "  set <key> <val>  Set one config value\n"
     "  deploy-env       Emit the compose env for this backend record\n"},
    {"vault", "Per-user encrypted agent credentials", CLIENT_TIER_CORE, 0,
     "  unlock                       Unlock the vault (creates a local root key)\n"
     "  set <agent> <name> <secret>  Store an encrypted credential\n"
     "  list                         List stored credential names (no secrets)\n"
     "  delete <agent> <name>        Remove a credential\n"
     "  lock                         Lock the vault (evict the cached key)\n"},
    {"cert", "mTLS client certificate lifecycle (operator)", CLIENT_TIER_ADMIN, 0,
     "  issue <cn> [--days N]        Issue a client cert (CN identity); key returned once\n"
     "  list                         List issued certs (serial, CN, validity, revoked)\n"
     "  revoke <serial>              Revoke a client cert by serial\n"},
    {"kb", "Project knowledge base", CLIENT_TIER_CORE, 1,
     "  search <query>   Search the knowledge base\n"
     "  build            Build the knowledge base for a project\n"
     "  update           Update the knowledge base\n"
     "  ingest           Ingest documents (status: ingest status)\n"
     "  status           Show knowledge-base status\n"
     "  docs push        Push docs into the knowledge base\n"
     "  grant set        Set one subject's write tier (--server, --team, --subject, --tier)\n"
     "  grant show       Show one subject's grant (--server, --team, --subject)\n"
     "  grant list       List grants (--server, --team, [--include-revoked])\n"
     "  grant revoke     Revoke one subject's grant (--server, --team, --subject)\n"},
    {"graph", "Code-graph projection and explain", CLIENT_TIER_ADVANCED, 0,
     "  sync-code        Project the code graph\n"
     "  explain          Explain a code-graph relationship\n"},
    {"css", "CSS migration analysis and render verification", CLIENT_TIER_ADVANCED, 0,
     "  report           Show a CSS-health overview\n"
     "  dead-rules | conflicts | duplicate-declarations | duplicate-selectors\n"
     "  unresolved | important-audit | high-specificity | unused-vars | token-candidates\n"
     "  migrate-enumerate | migrate-list | rules-doc | assert-conventions | conventions\n"
     "  render-store <project> <unit> <before|after> <snapshot.json>\n"
     "  render-capture <project> <unit> <before|after> <html-file> <css-file>\n"
     "  render-verify <project> <unit>\n"},
    {"optimize", "Bandit optimization loop", CLIENT_TIER_ADVANCED, 0,
     "  points                          List registered decision points\n"
     "  baseline --point <name>         Show current arm posteriors for a point\n"
     "  replay --point <name>           Emit a point's closed-decision log for replay\n"
     "  replay-record --point <n> --file <f>  Record a replay result (benchmark_trace)\n"
     "  run [--suite <s>] [--arm <a>]   Run the offline benchmark suite (ranks baseline vs on)\n"
     "  compare --baseline <a> --candidate <b>  Per-metric delta between two arms\n"
     "  promote --point <p> --candidate <a> [--guarded] [--apply]  Gate/apply a promotion "
     "(credible interval)\n"},
    {"workflow", "Author, validate, run, and inspect workflow graphs", CLIENT_TIER_ADVANCED, 0,
     "  blocks                 List the composable block catalog\n"
     "  validate <file.yaml>   Typed-graph validate a workflow definition\n"
     "  show <file.yaml>       Print the canonical form + version hash\n"
     "  list                   List workflows under $AIMEE_HOME/workflows\n"
     "  new <file.yaml>        Scaffold a starter workflow\n"
     "  run <name> --proposal <file> --repo <path> [--watch]\n"
     "  status <id> [--events] Inspect one durable run\n"},
    {"code", "Code-health audit", CLIENT_TIER_ADVANCED, 0,
     "  audit [dir] [--json] [--fix]   File-health audit (untested files,\n"
     "                         TODO/FIXME markers, debt score) over the tree;\n"
     "                         --fix is non-mutating and reports no safe fixes yet\n"
     "  audit --graph [--project P] [--json]   Graph-derived checks via aimee-kb\n"
     "                         (dead exports, import cycles, exact/near clones);\n"
     "                         requires a configured server, kb, and code index\n"},
    {"curator", "Knowledge curator queries", CLIENT_TIER_ADVANCED, 0,
     "  implements <topic>   What implements a topic\n"
     "  synthesize <topic>   Synthesize knowledge on a topic\n"
     "  contradictions       List contradictions\n"},
    {"trajectory", "Replayable session trajectories", CLIENT_TIER_ADVANCED, 0,
     "  export           Export a session trajectory\n"
     "  batch            Export trajectories in batch\n"},
    {"episode", "Delegation episodes", CLIENT_TIER_ADVANCED, 0,
     "  list             List recent delegation episodes\n"},
    {NULL, NULL, 0, 0, NULL},

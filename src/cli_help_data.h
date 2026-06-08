/* cli_help_data.h: client command help table entries for cli_main.c.
 *
 * Extracted verbatim from the `client_help[]` initializer to keep cli_main.c
 * under the source line-count limit (mirrors agent_help_data.h). Included once,
 * inside the array initializer in cli_main.c; the client_help_t type and the
 * surrounding `static const client_help_t client_help[] = { ... };` live there.
 */
{"init", "Run server initialization", CLIENT_TIER_CORE, 1, NULL},
    {"manuscript", "Novel-mode manuscript tools", CLIENT_TIER_CORE, 0, NULL},
    {"claude-proxy", "Route Claude Code through aimee's primary model", CLIENT_TIER_ADVANCED, 0,
     "  enable [url] [token]  Point Claude Code at aimee's /v1/messages ingress\n"
     "                        (url/token default to AIMEE_SERVER_URL/AIMEE_SERVER_TOKEN);\n"
     "                        reroutes ALL Claude Code sessions to your primary agent\n"
     "  disable               Restore Claude Code to its default endpoint\n"},
    {"remote", "Point the thin client at a remote aimee-server", CLIENT_TIER_ADVANCED, 0,
     "  set <url> [token]  Persist a remote server target\n"
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
     "  blast-radius     Show files affected by changes to a file\n"
     "  structure        Show file structure\n"
     "  callers          Find callers of a symbol\n"},
    {"workspace", "Workspace management (add, list, remove)", CLIENT_TIER_ADVANCED, 0,
     "  add <path>       Register a directory as a workspace and index its projects\n"
     "  list             List configured workspaces and their indexed projects\n"
     "  remove <path>    Unregister a workspace\n"},
    {"memory", "Stored memory", CLIENT_TIER_CORE, 0,
     "  search           Search stored memory\n"
     "  store            Store a memory\n"
     "  list             List memories\n"
     "  get              Read a memory by id\n"
     "  read             Assemble current memory context\n"},
    {"chat", "Start the aimee primary-agent chat", CLIENT_TIER_CORE, 0,
     "  [message...]     Send one message, or start an interactive chat with no message\n"},
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
    {"work", "Inter-session work queue", CLIENT_TIER_ADVANCED, 0,
     "  add              Add a work item to the queue\n"
     "  add-batch        Batch-add items (--from-proposals)\n"
     "  claim            Claim the next pending item\n"
     "  complete         Mark claimed item as done\n"
     "  fail             Mark claimed item as failed\n"
     "  list             List work items\n"
     "  cancel           Cancel a pending item\n"
     "  release          Release a claimed item back to pending\n"
     "  clear            Remove items by status\n"
     "  gc               Release stale claims\n"
     "  sync-proposals   Close items whose proposal moved out of pending/\n"
     "  stats            Show queue statistics\n"},
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
     "                   default. See `aimee delegate <role> --help` for the full\n"
     "                   flag set (--persona, --context-file, --via, etc.).\n"
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
    {"model", "Model capability metadata", CLIENT_TIER_ADVANCED, 0,
     "  list             List known models (--capability <name>, --open-weights)\n"
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
    {"agent", "Sub-agent management", CLIENT_TIER_ADVANCED, 0,
     "  list             List configured delegates\n"
     "  add              Add or update a delegate provider\n"
     "  local            Register/update a local OpenAI-compatible delegate\n"
     "                   (--provider openai|llama-eval for request shaping)\n"
     "  remove           Remove a configured delegate\n"
     "  enable           Enable a configured delegate\n"
     "  disable          Disable a configured delegate\n"
     "  probe            Probe delegate endpoint, slots, and execution\n"},
    {"provider", "Model provider profiles and catalogs", CLIENT_TIER_ADVANCED, 0,
     "  list             List registered providers (--available, --json)\n"
     "  show <name>      Show provider profile details\n"
     "  models <name>    Fetch provider model catalog (--json)\n"
     "  test <name>      Probe provider credentials and connectivity\n"
     "  quota [name]     Show process-local credential pool quota state\n"},
    {"version", "Print version", CLIENT_TIER_CORE, 0, NULL},
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
    {"cron", "Cron jobs and watchdog runs", CLIENT_TIER_ADVANCED, 0,
     "  list             List configured cron jobs\n"
     "  add <id>         Add or update a cron job\n"
     "  show <id>        Show one configured cron job\n"
     "  run <id>         Run one cron job now\n"
     "  history <id>     Show recent cron job runs\n"
     "  enable <id>      Enable a cron job\n"
     "  disable <id>     Disable a cron job (--all for rollback)\n"
     "  remove <id>      Remove a cron job\n"},

    {"git", "Git helpers", CLIENT_TIER_ADMIN, 0,
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
     "  set <key> <val>  Set one config value\n"},
    {"kb", "Project knowledge base", CLIENT_TIER_CORE, 1,
     "  search <query>   Search the knowledge base\n"
     "  build            Build the knowledge base for a project\n"
     "  update           Update the knowledge base\n"
     "  ingest           Ingest documents (status: ingest status)\n"
     "  status           Show knowledge-base status\n"
     "  docs push        Push docs into the knowledge base\n"},
    {"graph", "Code-graph projection and explain", CLIENT_TIER_ADVANCED, 0,
     "  sync-code        Project the code graph\n"
     "  explain          Explain a code-graph relationship\n"},
    {"optimize", "Bandit optimization loop", CLIENT_TIER_ADVANCED, 0,
     "  points                          List registered decision points\n"
     "  baseline --point <name>         Show current arm posteriors for a point\n"
     "  replay --point <name>           Emit a point's closed-decision log for replay\n"
     "  replay-record --point <n> --file <f>  Record a replay result (benchmark_trace)\n"
     "  run [--suite <s>] [--arm <a>]   Run the offline benchmark suite (ranks baseline vs on)\n"
     "  compare --baseline <a> --candidate <b>  Per-metric delta between two arms\n"
     "  promote --point <p> --candidate <a> [--guarded]  Evaluate the promotion gate (credible interval)\n"},
    {"code", "Code-health audit", CLIENT_TIER_ADVANCED, 0,
     "  audit [dir] [--json]   File-health audit (untested files, TODO/FIXME\n"
     "                         markers, debt score) over the working tree\n"
     "  audit --graph [--project P] [--json]   Graph-derived checks via aimee-kb\n"
     "                         (dead exports, import cycles, clones)\n"},
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

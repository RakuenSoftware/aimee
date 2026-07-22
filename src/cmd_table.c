/* cmd_table.c: command table and help, shared by monolith and server */
#include "aimee.h"
#include "cmd_review.h"
#include "commands.h"

int command_is_alias(const char *name)
{
   return strcmp(name, "+") == 0 || strcmp(name, "-") == 0 || strcmp(name, "quickstart") == 0 ||
          strcmp(name, "queue") == 0;
}

int command_is_hidden_default(const char *name)
{
   return command_is_alias(name) || strcmp(name, "hooks") == 0 ||
          strcmp(name, "session-start") == 0 || strcmp(name, "launch") == 0 ||
          strcmp(name, "wrapup") == 0 || strcmp(name, "help") == 0;
}

/* Print commands at exactly one tier level to stderr.
 * Skips aliases and hidden-default commands.  Used by usage() and cmd_help(). */
void print_commands_for_tier(cmd_tier_t tier)
{
   for (int i = 0; commands[i].name != NULL; i++)
   {
      if (commands[i].tier != tier)
         continue;
      if (command_is_hidden_default(commands[i].name))
         continue;
      fprintf(stderr, "  %-16s %s\n", commands[i].name, commands[i].help);
   }
}

void cmd_help(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* help --all: list every command grouped by tier */
   if (argc >= 1 && strcmp(argv[0], "--all") == 0)
   {
      fprintf(stderr, "Core commands:\n");
      print_commands_for_tier(CMD_TIER_CORE);
      fprintf(stderr, "\nAdvanced commands:\n");
      print_commands_for_tier(CMD_TIER_ADVANCED);
      fprintf(stderr, "\nAdmin commands:\n");
      print_commands_for_tier(CMD_TIER_ADMIN);
      fprintf(stderr, "\nRun 'aimee help <command>' for details on any command.\n");
      return;
   }

   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee help <command>\n");
      fprintf(stderr, "       aimee help --all          Show all commands\n");
      fprintf(stderr, "Common shortcuts: aimee use <provider>, aimee provider [name], "
                      "aimee verify on|off\n");
      return;
   }

   const char *target = argv[0];
   if (strcmp(target, "use") == 0 || strcmp(target, "provider") == 0)
   {
      fprintf(stderr, "aimee use <provider>\n"
                      "aimee provider [name]\n"
                      "\n"
                      "Shortcuts for reading or updating config.provider.\n"
                      "Examples:\n"
                      "  aimee use claude\n"
                      "  aimee provider codex\n"
                      "  aimee provider\n");
      return;
   }
   if (strcmp(target, "verify") == 0)
   {
      fprintf(stderr, "aimee verify [on|off|enable|disable|config]\n"
                      "\n"
                      "Human shortcut aliases:\n"
                      "  aimee verify on   -> aimee verify enable\n"
                      "  aimee verify off  -> aimee verify disable\n");
      return;
   }

   for (int i = 0; commands[i].name != NULL; i++)
   {
      if (strcmp(target, commands[i].name) == 0)
      {
         fprintf(stderr, "aimee %s: %s\n", commands[i].name, commands[i].help);

         const subcmd_t *subs = NULL;
         if (strcmp(target, "memory") == 0)
            subs = get_memory_subcmds();
         else if (strcmp(target, "agent") == 0)
            subs = get_agent_subcmds();
         else if (strcmp(target, "index") == 0)
            subs = get_index_subcmds();
         else if (strcmp(target, "wm") == 0)
            subs = get_wm_subcmds();
         else if (strcmp(target, "db") == 0)
            subs = get_db_subcmds();
         else if (strcmp(target, "session") == 0)
            subs = get_session_subcmds();
         else if (strcmp(target, "ensemble") == 0)
            subs = get_ensemble_subcmds();
         else if (strcmp(target, "branch") == 0)
            subs = get_branch_subcmds();
         else if (strcmp(target, "cancel") == 0)
            subs = get_cancel_subcmds();
         else if (strcmp(target, "job") == 0)
            subs = get_job_subcmds();
         else if (strcmp(target, "skill") == 0)
            subs = get_skill_subcmds();
         else if (strcmp(target, "autopilot") == 0)
            subs = get_autopilot_subcmds();
         else if (strcmp(target, "kb") == 0)
            subs = get_kb_subcmds();
         else if (strcmp(target, "trigger") == 0)
            subs = get_trigger_subcmds();
         else if (strcmp(target, "mcp") == 0)
            subs = get_mcp_subcmds();
         else if (strcmp(target, "aux") == 0)
            subs = get_aux_subcmds();

         if (subs)
         {
            fprintf(stderr, "\nSubcommands:\n");
            for (int j = 0; subs[j].name; j++)
            {
               if (subs[j].help)
                  fprintf(stderr, "  %-16s %s\n", subs[j].name, subs[j].help);
               else
                  fprintf(stderr, "  %s\n", subs[j].name);
            }
         }
         return;
      }
   }
   fprintf(stderr, "Unknown command: %s\n", target);
}

const command_t commands[] = {
    /* Core: session, hooks, memory, rules, config, index, delegate */
    {"init", "Initialize aimee database and config", cmd_init, CMD_TIER_CORE},
    {"setup", "Discover and index workspace projects", cmd_setup, CMD_TIER_CORE},
    {"quickstart", "Discover and index workspace projects", cmd_setup, CMD_TIER_CORE},
    {"wm", "Working memory (session-scoped scratch)", cmd_wm, CMD_TIER_CORE},
    {"index", "Code indexing (scan, overview, find, blast-radius, ...)", cmd_index, CMD_TIER_CORE},
    {"graph", "Code-graph projection and explain (sync-code, explain)", cmd_graph, CMD_TIER_CORE},
    {"memory", "Tiered memory management", cmd_memory, CMD_TIER_CORE},
    {"rules", "Rule management (list, generate, delete)", cmd_rules, CMD_TIER_CORE},
    {"roadmap", "Spec-driven roadmaps (new, show, list, status, rebuild, validate)", cmd_roadmap,
     CMD_TIER_ADVANCED},
    {"auto", "Autonomous roadmap dispatch loop (start, pause, resume, stop, status)", cmd_auto,
     CMD_TIER_ADVANCED},
    {"learning", "Learning proposals (list, status, accept, reject)", cmd_learning, CMD_TIER_CORE},
    {"review", "Review proposed artifacts across all surfaces (or aimee <surface> review)",
     cmd_review_all, CMD_TIER_CORE},
    {"dogfood", "Qualitative log review (tag, review, report)", cmd_dogfood, CMD_TIER_CORE},
    {"identity",
     "Inspect operator charter and working profile (show, working-profile, snapshot, diff)",
     cmd_identity, CMD_TIER_CORE},
    {"onboard", "Guided first-run: setup + doctor + smoke", cmd_onboard, CMD_TIER_CORE},
    {"diagnose",
     "Evidence-driven diagnosis (start, observe, hypothesize, evidence, probe, status, list, "
     "conclude)",
     cmd_diagnose, CMD_TIER_CORE},
    {"clarify", "Clarification pipeline for vague tasks (start, status, answer, spec)", cmd_clarify,
     CMD_TIER_CORE},
    {"feedback", "Record feedback (alias: +, -)", cmd_feedback_raw, CMD_TIER_CORE},
    {"+", "Record positive feedback", cmd_feedback_plus, CMD_TIER_CORE},
    {"-", "Record negative feedback", cmd_feedback_minus, CMD_TIER_CORE},
    {"hooks", "Pre/post tool hooks", cmd_hooks, CMD_TIER_CORE},
    {"session-start", "Start a new session", cmd_session_start, CMD_TIER_CORE},
    {"launch", "Session start + return launch metadata", cmd_launch, CMD_TIER_CORE},
    {"wrapup", "End-of-session processing", cmd_wrapup, CMD_TIER_CORE},
    {"mode", "Get/set session mode", cmd_mode, CMD_TIER_CORE},
    {"primary", "Pick which pool agent is this session's primary (<name>, --show, --clear)",
     cmd_primary, CMD_TIER_CORE},
    {"tdd", "Toggle TDD enforcement (on, on --strict, off)", cmd_tdd, CMD_TIER_CORE},
    {"plan", "Switch to plan mode", cmd_plan, CMD_TIER_CORE},
    {"implement", "Switch to implement mode", cmd_implement, CMD_TIER_CORE},
    {"run", "Run agent non-interactively with structured output (for scripts and CI)", cmd_run,
     CMD_TIER_CORE},
    {"delegate", "Delegate a task to a sub-agent (for AI tools)", cmd_delegate, CMD_TIER_CORE},
    {"verify", "Cross-verify changes (delegate reviews tool, tool reviews delegate)", cmd_verify,
     CMD_TIER_CORE},
    {"config", "View and update configuration", cmd_config, CMD_TIER_CORE},
    {"version", "Print version", cmd_version, CMD_TIER_CORE},
    {"help", "Show help for a command", cmd_help, CMD_TIER_CORE},
    {"notify", "Event notification hooks (on, off, test, status)", cmd_notify, CMD_TIER_CORE},
    {"roles", "Delegate role prompt templates (list, show, reset, install)", cmd_roles,
     CMD_TIER_CORE},
    {"toolset", "Composable named toolsets (list, show, resolve)", cmd_toolset, CMD_TIER_CORE},
    {"skill", "Project-scoped skill context injection (list, show)", cmd_skill, CMD_TIER_CORE},
    {"mcp", "MCP registry and OSV package gate (audit, recheck)", cmd_mcp, CMD_TIER_CORE},

    /* Advanced: workspace, worktree, agents, work queue, status */
    {"workspace", "Workspace management (add, list, remove)", cmd_workspace, CMD_TIER_ADVANCED},
    {"agent", "Sub-agent management and execution", cmd_agent, CMD_TIER_ADVANCED},
    {"context", "Print assembled execution context", cmd_context, CMD_TIER_ADVANCED},
    {"dispatch", "Run multiple tasks in parallel via agents", cmd_dispatch, CMD_TIER_ADVANCED},
    {"queue", "Deprecated alias for dispatch", cmd_queue, CMD_TIER_ADVANCED},
    {"sweep", "Deepening sweep: find duplication-across-call-sites seams (analysis-only)",
     cmd_sweep, CMD_TIER_ADVANCED},
    {"plugin", "Plugin management (install, list, enable, disable, remove)", cmd_plugin,
     CMD_TIER_ADVANCED},
    {"cancel", "Cancel active workflows and clean up orphaned state", cmd_cancel,
     CMD_TIER_ADVANCED},
    {"rewind", "Session file-snapshot checkpoints (list, create, add, restore, prune)", cmd_rewind,
     CMD_TIER_ADVANCED},
    {"trace", "Execution trace viewer", cmd_trace, CMD_TIER_ADVANCED},
    {"trajectory", "Export replayable session trajectories", cmd_trajectory, CMD_TIER_ADVANCED},
    {"jobs", "Agent job management", cmd_jobs, CMD_TIER_ADVANCED},
    {"job", "Coordinated parallel execution (start, status, cancel)", cmd_job, CMD_TIER_ADVANCED},
    {"autopilot", "Autonomous end-to-end pipeline (start, advance, status, ...)", cmd_autopilot,
     CMD_TIER_ADVANCED},
    {"trigger", "Event-triggered autopilot runs (list, status, cancel, fire)", cmd_trigger,
     CMD_TIER_ADVANCED},
    {"plans", "Execution plan management (list, show, replay)", cmd_plans, CMD_TIER_ADVANCED},
    {"session", "Session management (list, clean)", cmd_session, CMD_TIER_ADVANCED},
    {"ensemble", "A panel of agents: aggregate (MoA), roundtable, or templated session",
     cmd_ensemble, CMD_TIER_ADVANCED},
    {"status", "System health overview", cmd_status, CMD_TIER_ADVANCED},
    {"hud", "Real-time session status and HUD (--watch for live mode)", cmd_hud, CMD_TIER_ADVANCED},
    {"usage", "Token usage statistics", cmd_usage, CMD_TIER_ADVANCED},
    {"manifest", "List or show manifests", cmd_manifest, CMD_TIER_ADVANCED},
    {"contract", "Show project contract for current directory", cmd_contract, CMD_TIER_ADVANCED},
    {"describe", "Auto-describe projects via agent analysis", cmd_describe, CMD_TIER_ADVANCED},
    {"env", "Detect environment capabilities", cmd_env, CMD_TIER_ADVANCED},
    {"doctor", "Run diagnostic checks on all subsystems", cmd_doctor, CMD_TIER_ADVANCED},
    {"audit", "Verify/checkpoint the WORM audit store (verify, checkpoint)", cmd_audit,
     CMD_TIER_ADVANCED},
    {"provider", "Provider profile registry (list, show, models, quota)", cmd_provider,
     CMD_TIER_ADVANCED},
    {"model", "Model capability metadata (list, show, refresh)", cmd_model, CMD_TIER_ADVANCED},
    {"aux", "Auxiliary model routing (config show, test <task> \"<prompt>\")", cmd_aux,
     CMD_TIER_ADVANCED},
    {"slop", "Scan files for AI-slop patterns (attribution, hollow TODOs, etc.)", cmd_slop,
     CMD_TIER_ADVANCED},
    {"guardrails", "Semantic guardrail event review (guardrails review)", cmd_guardrails,
     CMD_TIER_ADVANCED},
    {"kb", "Project knowledge base (build, update, search, status, clear)", cmd_kb,
     CMD_TIER_ADVANCED},
    {"wiki", "Memory wiki (render --out <dir>)", cmd_wiki, CMD_TIER_ADVANCED},

    /* Admin: eval, import/export, db, branch, git */
    {"dashboard", "Serve the dashboard UI", cmd_dashboard, CMD_TIER_ADMIN},
    {"webchat", "Manage the aimee-webchat service (enable/disable/status)", cmd_webchat,
     CMD_TIER_ADMIN},
    {"gateway", "Ambient-presence gateway management (pair list/issue/approve/revoke)", cmd_gateway,
     CMD_TIER_ADMIN},
    {"eval", "Eval harness (run, results)", cmd_eval, CMD_TIER_ADMIN},
    {"export", "Export state to portable JSONL format", cmd_export, CMD_TIER_ADMIN},
    {"import", "Import state from exported directory", cmd_import, CMD_TIER_ADMIN},
    {"db", "Database diagnostics and maintenance", cmd_db, CMD_TIER_ADMIN},
    {"branch", "Branch conflict analysis and cascading merge", cmd_branch, CMD_TIER_ADMIN},
    {"git", "Git and PR operations", cmd_git, CMD_TIER_ADMIN},
    {"clean", "Remove all aimee data and hooks (use --force)", NULL, CMD_TIER_ADMIN},

    {NULL, NULL, NULL, 0} /* sentinel */
};

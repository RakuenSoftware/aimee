/* cmd_memory.c: dispatcher + shared helpers for the `aimee memory`
 * subcommand family. Individual subcommand handlers live in:
 *   cmd_memory_core.c   — CRUD, stats, task/decide, link/tag
 *   cmd_memory_embed.c  — embed, reembed, diagnose, answer, eval suite
 *   cmd_memory_vector.c — reindex/repair/rebuild/reconcile/verify + workflows
 */
#include "aimee.h"
#include "cmd_review.h"
#include "cmd_memory_internal.h"
#include "session_compact.h"
#include "agent_eval.h"
#include "commands.h"
#include "db1.h"
#include "cJSON.h"
#include "dogfood.h"
#include "json_fluent.h"
#include "kb_client.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include "kb.h"
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

/* File-scope config, loaded once by cmd_memory before dispatch */
config_t s_mem_cfg;
/* memory_score_parts_to_json prototype in cmd_memory_internal.h */

double cmd_memory_env_weight(const char *name, double fallback)
{
   const char *v = getenv(name);
   if (!v || !v[0])
      return fallback;
   char *end = NULL;
   double d = strtod(v, &end);
   if (!end || end == v)
      return fallback;
   return d;
}

void cmd_memory_apply_rerank_mode(const opt_parsed_t *opts)
{
   if (opt_get_flag(opts, "slow-rerank"))
      platform_setenv("AIMEE_MEMORY_RERANK_MODE", "slow");
   else if (opt_get_flag(opts, "fast-rerank"))
      platform_setenv("AIMEE_MEMORY_RERANK_MODE", "fast");
}

const char *cmd_memory_scope_type(const opt_parsed_t *opts)
{
   const char *scope_type = opt_get(opts, "scope-type");
   const char *scope_value = opt_get(opts, "scope");
   if ((!scope_type || !scope_type[0]) && scope_value && scope_value[0])
      return "workspace";
   return scope_type;
}

const char *cmd_memory_scope_value(const opt_parsed_t *opts)
{
   return opt_get(opts, "scope");
}

void cmd_memory_build_filter(const opt_parsed_t *opts, memory_filter_t *out)
{
   memory_filter_from_scope(cmd_memory_scope_type(opts), cmd_memory_scope_value(opts), out);
}

int cmd_memory_find_facts(const opt_parsed_t *opts, const char *query, int limit, memory_t *facts,
                          int max)
{
   /* Graph-code fusion is always on for recall; the scoped client carries
    * graph_code_fusion_state="on" to the kb handler. */
   int n = kb_client_memory_find_facts_scoped(query, cmd_memory_scope_type(opts),
                                              cmd_memory_scope_value(opts), limit, facts, max);
   if (n > 0 && facts)
   {
      int64_t ids[32];
      int id_count = n < 32 ? n : 32;
      for (int i = 0; i < id_count; i++)
         ids[i] = facts[i].id;
      dogfood_log_moment_live("memory_search", query, ids, id_count, NULL);
   }
   else if (n >= 0)
   {
      dogfood_log_moment_live("memory_search", query, NULL, 0, NULL);
   }
   return n;
}

int cmd_memory_diagnose_query(const opt_parsed_t *opts, const char *query, int limit,
                              memory_diagnostic_t *rows, int max)
{
   return kb_client_memory_diagnose_scoped(query, cmd_memory_scope_type(opts),
                                           cmd_memory_scope_value(opts), limit, rows, max);
}

int cmd_memory_ask(const opt_parsed_t *opts, const char *query, int limit,
                   memory_answer_result_t *out)
{
   int rc = kb_client_memory_ask(query, cmd_memory_scope_type(opts), cmd_memory_scope_value(opts),
                                 limit, out);
   if (rc == 0 && out)
      dogfood_log_moment_live("memory_ask", query, out->citation_ids, out->citation_count, NULL);
   return rc;
}

void cmd_memory_require_runtime(int rc, const char *op)
{
   if (rc < 0)
      fatal("%s failed: memory retrieval index unavailable; server-side maintenance is required",
            op);
}

/* --- memory subcommand handlers --- */

/* --- memory subcommand table --- */

static const subcmd_t memory_subcmds[] = {
    {"store", "Store a memory (key, content, --tier, --kind)", mem_store},
    {"get", "Retrieve a memory by key", mem_get},
    {"delete", "Delete a memory by ID", mem_delete},
    {"list", "List memories (--tier, --kind, --limit)", mem_list},
    {"search", "Hybrid retrieval across all memories", mem_search},
    {"plan", "Show the deterministic retrieval plan for a query", mem_plan},
    {"stats", "Show memory tier statistics", mem_stats},
    {"scan", "Scan conversations for learnable patterns", mem_scan},
    {"queue", "Show async memory queue status", mem_queue},
    {"drain", "Drain async memory queue (--timeout N)", mem_drain},
    {"edges", "Show memory graph edges", mem_edges},
    {"compact", "Compact old memory windows", mem_compact},
    {"conflicts", "Show conflicting memories", mem_conflicts},
    {"maintain",
     "Run a scheduled memory maintenance cycle (replay/compact/prune/summarize). "
     "Flags: --modes CSV (default replay,compact,prune), --dry-run, --force, --watch SECS, --json",
     mem_maintain},
    {"briefing", "Proactive start-of-session context bundle (--limit-tokens N)", mem_briefing},
    {"remind",
     "Arm a prospective memory: --when \"...\" --do \"...\" [--entity X] [--file P]"
     " [--recur once|repeat] [--valid-until TS]",
     mem_remind},
    {"reminders",
     "List armed/triggered reminders (--state X, --limit N, --complete <id>, --expire-sweep)",
     mem_reminders},
    {"alerts",
     "Surface stale pending commitments, unresolved contradictions, and newly superseded "
     "memories in one bundle (--since ISO8601 for newly-superseded window)",
     mem_alerts},
    {"recall",
     "Proactive recall bundle — identity, preferences, active context, open commitments, "
     "reminders, directives (--task STR, --session-start, --limit-tokens N, --explain)",
     mem_recall},
    {"directives",
     "List epistemic directives (--state open|suppressed|resolved|expired, --cause X,"
     " --limit N, --resolve <id> [--with-memory N] [--note X], --suppress <id>,"
     " --expire-sweep, --json)",
     mem_directives},
    {"directive",
     "Create an epistemic directive: --question \"...\" [--topic X] [--entity Y]"
     " [--file P] [--cause contradiction|retrieval_failure|missing_config|user_follow_up]"
     " [--priority N] [--valid-until TS]",
     mem_directive},
    {"curiosity",
     "Curiosity backlog: list, create, sweep (from failed_queries), rescore, "
     "route [--limit N], suppress <id>, resolve <id>",
     mem_curiosity},
    {"health", "Show memory system health metrics (7-day rolling)", mem_health},
    {"provenance", "Show provenance chain for a memory (or --stale)", mem_provenance},
    {"task", "Create or update a task memory", mem_task},
    {"decide", "Record a decision", mem_decide},
    {"decisions", "List recorded decisions", mem_decisions},
    {"antipattern", "Record an anti-pattern", mem_antipattern},
    {"supersede", "Supersede an old memory with a new one", mem_supersede},
    {"history", "Show memory mutation history", mem_history},
    {"checkpoint", "Save a session checkpoint", mem_checkpoint},
    {"style", "Show learned style preferences", mem_style},
    {"read", "Read a memory's full content", mem_read},
    {"link", "Link a memory to an artifact (file, commit, pr)", mem_link},
    {"mlink", "Create a memory-to-memory link (depends_on, etc.)", mem_mlink},
    {"mlinks", "Show memory-to-memory links for a given ID", mem_mlinks},
    {"munlink", "Remove a memory-to-memory link", mem_munlink},
    {"tag", "Tag a memory with a workspace scope", mem_tag},
    {"embed", "Generate embeddings for memories (--all or <id>)", mem_embed},
    {"reembed",
     "Online re-embed corpus with dual-index rollover (--start | --status | --cutover | --rollback "
     "<v>)",
     mem_reembed},
    {"diagnose", "Inspect why memory retrieval ranked results for a query", mem_diagnose},
    {"improve", "Run improve passes: --dedupe, --summarise, --all, --dry-run", mem_improve},
    {"cognify", "Run LLM-driven cognification on a memory unit (--unit <id>)", mem_cognify},
    {"episode", "View or generate episode cards for a session (--generate)", mem_episode},
    {"assemble", "Show assembled context (--explain for token costs and selection)", mem_assemble},
    {"answer", "Return a deterministic answer from memory retrieval", mem_answer},
    {"ask", "Return a structured answer with confidence and citations", mem_answer},
    {"reflect", "Deep-reasoning synthesis across scopes with contradiction detection", mem_reflect},
    {"audit", "Evaluate labeled retrieval cases and bucket failure modes", mem_audit},
    {"calibrate", "Tune retrieval weight multipliers from labeled cases", mem_calibrate},
    {"benchmark", "Run memory retrieval benchmarks (corpus, live, locomo, longmemeval)",
     mem_benchmark},
    {"repair", "Rebuild vector state ([id] --limit N, --failed-only, --reset-stuck)", mem_repair},
    {"rebuild", "Recreate the memory vector index and re-upsert all points (--version <v>)",
     mem_rebuild},
    {"reconcile", "Delete orphan vector points with no DB2 backing (--dry-run)", mem_reconcile},
    {"verify", "Verify vector index consistency against DB2 (--repair, --detail, --timings)",
     mem_verify},
    {"reindex", "Rebuild derived memory indexes (--limit N)", mem_reindex},
    {"profile", "Show entity profile card (<entity> or --refresh)", mem_profile},
    {"cite", "Show provenance and lineage chain for a memory ID", mem_cite},
    {"scene", "List or show memory scenes (list, show <id>)", mem_scene},
    {"ontology", "Dump typed graph schema (list) or walk typed graph (walk <entity>)",
     mem_ontology},
    {"approve", "Approve a memory for L3→L4 promotion (<id> [--note S] [--approver S])",
     mem_approve},
    {"op",
     "Typed mutation verb: store | update <id> <content> | supersede <old_id> <content> | "
     "forget <id> | affirm <id> | reject <id> [--reason S]",
     mem_op},
    {"pack", "Profile pack operations: list | show <name> | validate <path> | use <name>",
     mem_pack},
    {"lint", "Read-only consistency checks: orphan memories, concept gaps, stale cross-references",
     mem_lint},
    {"review",
     "Review proposed memory artifacts from the charter pipeline (--limit N, --confidence-min F, "
     "--json)",
     mem_review},
    {NULL, NULL, NULL},
};

const subcmd_t *get_memory_subcmds(void)
{
   return memory_subcmds;
}

void mem_review(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_review_surface("memory", ctx, argc, argv);
}

/* --- cmd_memory --- */

void cmd_memory(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("memory", memory_subcmds);
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   /* "plan" and "pack" have no DB dependency. Everything else either touches
    * DB1 directly (decisions) or runs a memory pipeline that reaches DB1
    * through session_state_* / memory_*. */
   int needs_no_db = (strcmp(sub, "plan") == 0 || strcmp(sub, "pack") == 0);
   if (!needs_no_db)
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("cannot initialize DB1");
   }

   config_load(&s_mem_cfg);

   /* compact --focus <topic>: generate a structured 11-section summary locally. */
   if (strcmp(sub, "compact") == 0)
   {
      for (int i = 0; i < argc - 1; i++)
      {
         if (strcmp(argv[i], "--focus") == 0)
         {
            char summary[SESSION_COMPACT_SUMMARY_MAX];
            if (session_compact_focused(argv[i + 1], summary, sizeof(summary)) != 0)
            {
               fprintf(stderr, "compact --focus: failed\n");
               return;
            }
            if (ctx->json_output)
            {
               cJSON *j = cJSON_CreateObject();
               cJSON_AddStringToObject(j, "status", "ok");
               cJSON_AddStringToObject(j, "focus", argv[i + 1]);
               cJSON_AddStringToObject(j, "summary", summary);
               emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
            }
            else
            {
               printf("%s\n", summary);
            }
            return;
         }
      }
   }

   if (subcmd_dispatch(memory_subcmds, sub, ctx, argc, argv) != 0)
      fatal("unknown memory subcommand: %s", sub);
}

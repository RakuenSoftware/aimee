/* cmd_agent_delegate.c: delegate, verify, and delegate-status CLI commands */
#include "aimee.h"
#include "agent.h"
#include "log.h"
#include "db1.h"
#include "otel.h"
#include "platform_path.h"
#include "platform_process.h"
#include "commands.h"
#include "cmd_agent_delegate_impl.h"
#include "prompts.h"
#include "persona.h"
#include "role_templates.h"
#include "skill.h"
#include "events.h"
#include "agent_coord.h"
#include "delegate_role.h"
#include "delegate_plan.h"
#include "delegate_launch.h"
#include "delegate_economics.h"
#include "delegate_ensemble.h"
#include "memory_platform.h"
#include "workspace.h"
#include "guardrails.h"
#include "delegate_source_authority.h"
#include "toolset.h"
#include "liveness.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char *delegate_read_prompt_stdin(void);
static const char *DELEGATE_COORD_STATUS_NOTE =
    "coord jobs are queued packet plans. `aimee jobs status` and `aimee delegate status` show "
    "durable background delegate jobs, not coord jobs";

#include "cmd_agent_delegate_toolset.inc"
void delegate_worktree_restore(const char *orig_cwd, const char *git_root, const char *work_name,
                               int keep)
{
   if (orig_cwd[0])
      (void)chdir(orig_cwd);
   if (!git_root[0])
      return;
   if (keep)
   {
      char wt_path[MAX_PATH_LEN];
      worktree_sibling_path(git_root, session_id(), work_name, wt_path, sizeof(wt_path));
      LOG_INFO("delegate", "keeping delegate worktree at %s", wt_path);
      return;
   }
   worktree_cleanup(git_root, session_id(), work_name);
}

/* Called by AI tools to offload work to sub-agents.
 * The AI tool runs: aimee delegate <role> "prompt"
 * aimee routes to the cheapest agent, executes, returns result on stdout.
 * This allows Claude Code / Gemini / Codex to delegate expensive work
 * to cheaper or local LLMs. */
void generate_task_id(char *buf, size_t len)
{
   static unsigned long fallback_counter = 0;
   unsigned char rand_bytes[8];
   if (platform_random_bytes(rand_bytes, sizeof(rand_bytes)) != 0)
   {
      unsigned long seed =
          (unsigned long)time(NULL) ^ ((unsigned long)getpid() << 16) ^ ++fallback_counter;
      for (size_t i = 0; i < sizeof(rand_bytes); i++)
      {
         seed = seed * 1103515245u + 12345u;
         rand_bytes[i] = (unsigned char)((seed >> 16) & 0xffu);
      }
   }
   snprintf(buf, len, "aimee-task-%02x%02x%02x%02x%02x%02x%02x%02x", rand_bytes[0], rand_bytes[1],
            rand_bytes[2], rand_bytes[3], rand_bytes[4], rand_bytes[5], rand_bytes[6],
            rand_bytes[7]);
}

static void delegate_list_roles(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
   {
      fprintf(stderr, "No agents configured. Add agents to agents.json.\n");
      return;
   }

   fprintf(stderr, "Available roles (from agents.json):\n");
   for (int i = 0; i < cfg.agent_count; i++)
   {
      fprintf(stderr, "  %s [%s", cfg.agents[i].name, cfg.agents[i].provider);
      if (cfg.agents[i].model[0])
         fprintf(stderr, "/%s", cfg.agents[i].model);
      fprintf(stderr, "]: ");
      for (int r = 0; r < cfg.agents[i].role_count; r++)
      {
         if (r > 0)
            fprintf(stderr, ", ");
         fprintf(stderr, "%s", cfg.agents[i].roles[r]);
      }
      if (cfg.agents[i].tools_enabled)
         fprintf(stderr, " (tools)");
      fprintf(stderr, "\n");
   }
}

static void delegate_plan_usage(void)
{
   fprintf(stderr, "Usage: aimee delegate plan <proposal.md> [--json] [--output PATH] "
                   "[--launch] [--parallel N]\n"
                   "\n"
                   "Generate read-only delegate work packets from a proposal.\n");
}

static void delegate_launch_usage(void)
{
   fprintf(stderr, "Usage: aimee delegate launch <plan.json> [--json] [--parallel N]\n"
                   "\n"
                   "Queue reviewed delegate work packets into a coordinated job.\n"
                   "Inspect queued packet progress with `aimee job status <job_id>`.\n"
                   "`aimee jobs status` and `aimee delegate status` are for durable "
                   "background delegate jobs.\n");
}

static void delegate_plan_slug(const char *path, char *buf, size_t len)
{
   const char *base = strrchr(path ? path : "", '/');
   base = base ? base + 1 : (path ? path : "delegate-plan");

   char tmp[256];
   snprintf(tmp, sizeof(tmp), "%s", base[0] ? base : "delegate-plan");
   char *dot = strrchr(tmp, '.');
   if (dot)
      *dot = '\0';

   size_t n = 0;
   for (const char *p = tmp; *p && n + 1 < len; p++)
   {
      char c = (char)tolower((unsigned char)*p);
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
         buf[n++] = c;
      else if (n > 0 && buf[n - 1] != '-')
         buf[n++] = '-';
   }
   while (n > 0 && buf[n - 1] == '-')
      n--;
   buf[n] = '\0';
   if (buf[0] == '\0')
      snprintf(buf, len, "delegate-plan");
}

static int delegate_plan_write_json(const char *path, cJSON *plan)
{
   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (slash)
   {
      *slash = '\0';
      if (platform_mkdir_p(parent, 0755) != 0)
         return -1;
   }

   char *json = cJSON_Print(plan);
   if (!json)
      return -1;
   FILE *f = fopen(path, "w");
   if (!f)
   {
      free(json);
      return -1;
   }
   fputs(json, f);
   fputc('\n', f);
   fclose(f);
   free(json);
   return 0;
}

static int delegate_plan_json_int(cJSON *plan, const char *name)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(plan, name);
   return cJSON_IsNumber(v) ? v->valueint : 0;
}

static char *delegate_read_file_limited(const char *path, size_t limit, char *err, size_t err_len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
   {
      snprintf(err, err_len, "cannot open %s", path ? path : "(null)");
      return NULL;
   }
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      snprintf(err, err_len, "cannot seek %s", path);
      return NULL;
   }
   long sz = ftell(f);
   if (sz <= 0 || (size_t)sz > limit)
   {
      fclose(f);
      snprintf(err, err_len, "plan file is empty or too large");
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      snprintf(err, err_len, "out of memory reading plan");
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   if (n != (size_t)sz && ferror(f))
   {
      fclose(f);
      free(buf);
      snprintf(err, err_len, "failed reading plan file");
      return NULL;
   }
   fclose(f);
   buf[n] = '\0';
   return buf;
}

static void delegate_print_launch_json(const delegate_launch_result_t *result)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddNumberToObject(resp, "plan_id", result->plan_id);
   cJSON_AddNumberToObject(resp, "job_id", result->job_id);
   cJSON_AddNumberToObject(resp, "tasks", result->tasks);
   cJSON_AddNumberToObject(resp, "max_concurrent", result->max_concurrent);
   cJSON_AddStringToObject(resp, "job_status", "pending");
   cJSON_AddStringToObject(resp, "status_command", "aimee job status <job_id>");
   cJSON_AddStringToObject(resp, "status_note", DELEGATE_COORD_STATUS_NOTE);
   char *json = cJSON_Print(resp);
   cJSON_Delete(resp);
   if (json)
   {
      printf("%s\n", json);
      free(json);
   }
}

static void delegate_print_launch_summary(const delegate_launch_result_t *result)
{
   printf("Queued coord job #%d from delegate plan #%d: %d tasks, max %d concurrent\n",
          result->job_id, result->plan_id, result->tasks, result->max_concurrent);
   printf("Inspect packet progress with: aimee job status %d\n", result->job_id);
   printf("Note: %s.\n", DELEGATE_COORD_STATUS_NOTE);
}

static void cmd_delegate_launch(int argc, char **argv)
{
   const char *path = NULL;
   int json_output = 0;
   int parallel = 3;

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         delegate_launch_usage();
         return;
      }
      if (strcmp(argv[i], "--json") == 0)
      {
         json_output = 1;
         continue;
      }
      if (strcmp(argv[i], "--parallel") == 0 && i + 1 < argc)
      {
         parallel = atoi(argv[++i]);
         continue;
      }
      if (!path)
      {
         path = argv[i];
         continue;
      }
      fatal("unexpected delegate launch argument: %s", argv[i]);
   }

   if (!path)
   {
      delegate_launch_usage();
      exit(2);
   }

   char err[256] = "";
   char *text = delegate_read_file_limited(path, 2 * 1024 * 1024, err, sizeof(err));
   if (!text)
      fatal("delegate launch failed: %s", err[0] ? err : "cannot read plan");
   cJSON *plan = cJSON_Parse(text);
   free(text);
   if (!cJSON_IsObject(plan))
   {
      cJSON_Delete(plan);
      fatal("delegate launch failed: invalid plan JSON");
   }
   delegate_launch_result_t result;
   if (delegate_launch_coord_job(plan, parallel, getcwd((char[MAX_PATH_LEN]){0}, MAX_PATH_LEN),
                                 &result, err, sizeof(err)) != 0)
   {
      cJSON_Delete(plan);
      fatal("delegate launch failed: %s", err[0] ? err : "unknown error");
   }
   cJSON_Delete(plan);
   if (json_output)
      delegate_print_launch_json(&result);
   else
      delegate_print_launch_summary(&result);
}
static void cmd_delegate_plan(int argc, char **argv)
{
   const char *proposal = NULL;
   const char *output_path = NULL;
   int json_output = 0;
   int launch = 0;
   int parallel = 3;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         delegate_plan_usage();
         return;
      }
      if (strcmp(argv[i], "--json") == 0)
      {
         json_output = 1;
         continue;
      }
      if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
      {
         output_path = argv[++i];
         continue;
      }
      if (strcmp(argv[i], "--launch") == 0)
      {
         launch = 1;
         continue;
      }
      if (strcmp(argv[i], "--parallel") == 0 && i + 1 < argc)
      {
         parallel = atoi(argv[++i]);
         continue;
      }
      if (!proposal)
      {
         proposal = argv[i];
         continue;
      }
      fatal("unexpected delegate plan argument: %s", argv[i]);
   }

   if (!proposal)
   {
      delegate_plan_usage();
      exit(2);
   }

   char err[256] = "";
   cJSON *plan = delegate_plan_build_from_file(proposal, err, sizeof(err));
   if (!plan)
      fatal("delegate plan failed: %s", err[0] ? err : "unknown error");

   if (json_output && !launch)
   {
      char *json = cJSON_Print(plan);
      if (!json)
      {
         cJSON_Delete(plan);
         fatal("delegate plan failed: could not serialize JSON");
      }
      printf("%s\n", json);
      free(json);
      cJSON_Delete(plan);
      return;
   }

   char default_path[MAX_PATH_LEN];
   if (!output_path)
   {
      char slug[256];
      delegate_plan_slug(proposal, slug, sizeof(slug));
      snprintf(default_path, sizeof(default_path), ".aimee/delegate-plans/%s.json", slug);
      output_path = default_path;
   }

   if (delegate_plan_write_json(output_path, plan) != 0)
   {
      cJSON_Delete(plan);
      fatal("delegate plan failed: could not write output file");
   }

   int packet_count = delegate_plan_json_int(plan, "packet_count");
   int parallel_safe = delegate_plan_json_int(plan, "parallel_safe_count");
   int needs_sequencing = delegate_plan_json_int(plan, "needs_sequencing_count");
   cJSON *reviewer = cJSON_GetObjectItemCaseSensitive(plan, "reviewer_packet_included");
   cJSON *conflicts = cJSON_GetObjectItemCaseSensitive(plan, "conflicts");
   int conflict_count = cJSON_IsArray(conflicts) ? cJSON_GetArraySize(conflicts) : 0;
   cJSON *missing_owned = cJSON_GetObjectItemCaseSensitive(plan, "missing_owned_files");
   int missing_owned_count = cJSON_IsArray(missing_owned) ? cJSON_GetArraySize(missing_owned) : 0;
   cJSON *new_owned = cJSON_GetObjectItemCaseSensitive(plan, "new_owned_files");
   int new_owned_count = cJSON_IsArray(new_owned) ? cJSON_GetArraySize(new_owned) : 0;

   if (!json_output)
   {
      printf("Generated %d work packets\n", packet_count);
      printf("Parallel-safe: %d\n", parallel_safe);
      printf("Needs sequencing: %d\n", needs_sequencing);
      printf("Potential file conflicts: %s\n", conflict_count ? "present" : "none");
      printf("Missing owned files: %d\n", missing_owned_count);
      printf("Declared new owned files: %d\n", new_owned_count);
      printf("Reviewer packet: %s\n", cJSON_IsTrue(reviewer) ? "included" : "not included");
      printf("Plan saved: %s\n", output_path);
      if (!launch)
      {
         if (missing_owned_count > 0)
            printf("Launch blocked until missing owned files are reviewed or marked new.\n");
         else
            printf("Launch command: aimee delegate launch %s --parallel %d\n", output_path,
                   parallel > 0 ? parallel : 3);
      }
   }

   if (launch)
   {
      delegate_launch_result_t result;
      if (delegate_launch_coord_job(plan, parallel, getcwd((char[MAX_PATH_LEN]){0}, MAX_PATH_LEN),
                                    &result, err, sizeof(err)) != 0)
      {
         cJSON_Delete(plan);
         fatal("delegate launch failed: %s", err[0] ? err : "unknown error");
      }
      if (json_output)
         delegate_print_launch_json(&result);
      else
         delegate_print_launch_summary(&result);
   }

   cJSON_Delete(plan);
}

/* Nested delegate creation is denied at runtime; do not add a prompt block that
 * tells delegates to fan out work they cannot actually create. */
char *delegate_build_tier_context(const char *via_name, int tier_override, const char *role)
{
   (void)via_name;
   (void)tier_override;
   (void)role;
   return NULL;
}

void cmd_delegate(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
   {
      delegate_print_help();
      return;
   }

   /* Handle --help */
   if (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0)
   {
      delegate_print_help();
      return;
   }

   /* Handle --list-roles */
   if (strcmp(argv[0], "--list-roles") == 0)
   {
      delegate_list_roles();
      return;
   }

   /* Handle subcommand: aimee delegate status <task_id> */
   if (strcmp(argv[0], "status") == 0)
   {
      cmd_delegate_status(ctx, argc - 1, argv + 1);
      return;
   }

   if (strcmp(argv[0], "plan") == 0)
   {
      cmd_delegate_plan(argc - 1, argv + 1);
      return;
   }

   if (strcmp(argv[0], "launch") == 0)
   {
      cmd_delegate_launch(argc - 1, argv + 1);
      return;
   }

   if (strcmp(argv[0], "aggregate") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "error: aggregate requires a prompt\n");
         return;
      }
      config_t cfg;
      config_load(&cfg);
      agent_config_t acfg;
      if (agent_load_config(&acfg) != 0)
      {
         fprintf(stderr, "error: could not load agent config\n");
         return;
      }
      delegate_ensemble_result_t result;
      if (delegate_ensemble_run(&acfg, &cfg, argv[1], &result) != 0)
      {
         fprintf(stderr, "error: ensemble failed\n");
         return;
      }
      if (!result.success)
         fprintf(stderr, "warning: %s\n",
                 result.degraded ? "degraded to best candidate" : "ensemble produced no output");
      if (result.response[0])
         printf("%s\n", result.response);
      return;
   }

   if (strcmp(argv[0], "result") == 0 || strcmp(argv[0], "log") == 0 ||
       strcmp(argv[0], "logs") == 0)
   {
      fatal("unknown delegate subcommand '%s'. Use `aimee jobs logs <job_id>` for output or "
            "`aimee delegate status <job_id>` for status.",
            argv[0]);
   }

   if (argc < 1)
   {
      fprintf(stderr, "error: missing role\n\n");
      delegate_print_help();
      return;
   }

   {
      const char *parent_env = getenv("AIMEE_PARENT_DELEGATION_ID");
      const char *depth_env = getenv("AIMEE_DELEGATE_DEPTH");
      config_t dcfg;
      config_load(&dcfg);
      int known = db1_init(dcfg.db1_path) == 0;
      int active = known ? db1_delegation_spawn_is_active(parent_env) : 0;
      if (delegate_chain_env_should_clear(depth_env, parent_env, known, active))
      {
         platform_setenv("AIMEE_PARENT_DELEGATION_ID", "");
         platform_setenv("AIMEE_DELEGATE_DEPTH", "");
      }
      int max_depth = dcfg.max_delegation_depth > 0 ? dcfg.max_delegation_depth
                                                    : CONFIG_DEFAULT_MAX_DELEGATION_DEPTH;
      char depth_err[256];
      if (delegate_check_chain_depth(max_depth, depth_err, sizeof(depth_err)) != 0)
      {
         fprintf(stderr, "aimee: %s\n", depth_err);
         exit(1);
      }
   }
   static const char *bool_flags[] = {"json", "background",   "durable",      "coordination",
                                      "plan", "dry-run",      "tools",        "keep-worktree",
                                      "loop", "handoff-json", "prompt-stdin", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   const char *role = opt_pos(&opts, 0);
   const char *prompt = opt_pos(&opts, 1);
   const char *sys_prompt = opt_get(&opts, "system");
   int max_tokens = opt_get_int(&opts, "max-tokens", 0);
   int max_turns = opt_get_int(&opts, "max-turns", -1);
   int token_budget_override = opt_get_int(&opts, "token-budget", 0);
   int json_output = opt_has(&opts, "json");
   int background = opt_has(&opts, "background");
   int durable = opt_has(&opts, "durable");
   int coordination = opt_has(&opts, "coordination");
   int vote_count = opt_get_int(&opts, "vote", 0);
   int plan_mode = opt_has(&opts, "plan");
   int dry_run = opt_has(&opts, "dry-run");
   int explicit_tools = opt_has(&opts, "tools");
   const char *explicit_toolset = opt_get(&opts, "tools");
   int handoff_json = opt_has(&opts, "handoff-json");
   const char *files_arg = opt_get(&opts, "files");
   int timeout_ms = opt_get_int(&opts, "timeout", 0);
   const char *prompt_file = opt_get(&opts, "prompt-file");
   int prompt_stdin = opt_has(&opts, "prompt-stdin");
   int retry_count = opt_get_int(&opts, "retry", 0);
   const char *verify_cmd = opt_get(&opts, "verify");
   const char *context_dir = opt_get(&opts, "context-dir");
   const char *output_path = opt_get(&opts, "output");
   const char *channel = opt_get(&opts, "channel");
   const char *worktree_branch = opt_get(&opts, "worktree");
   const char *context_syms = opt_get(&opts, "context");
   const char *via_agent_name = opt_get(&opts, "via");
   const char *provider_override = opt_get(&opts, "provider");
   const char *model_override = opt_get(&opts, "model");
   const char *persona_override = opt_get(&opts, "persona");
   int tier_override = opt_get_int(&opts, "tier", -1);
   int loop_mode = opt_has(&opts, "loop");
   int keep_worktree = opt_has(&opts, "keep-worktree");
   int max_iter = opt_get_int(&opts, "max-iter", AGENT_LOOP_MAX_ITER_DEFAULT);
   int completion_threshold =
       opt_get_int(&opts, "completion-threshold", AGENT_LOOP_THRESHOLD_DEFAULT);
   /* --max-iter implies --loop */
   if (opt_get(&opts, "max-iter") || opt_get(&opts, "completion-threshold"))
      loop_mode = 1;

   /* Collect repeatable --context-file flags */
   const char *context_files[OPT_MAX_FLAGS];
   int context_file_count = 0;
   for (int i = 0; i < opts.flag_count && context_file_count < OPT_MAX_FLAGS; i++)
   {
      if (strcmp(opts.flags[i].name, "context-file") == 0 && opts.flags[i].value)
         context_files[context_file_count++] = opts.flags[i].value;
   }

   if (prompt_stdin && prompt_file && prompt_file[0])
      fatal("--prompt-stdin and --prompt-file are mutually exclusive");

   /* A persona is required for every delegate: it sets the delegate's identity
    * and principles. It resolves from the built-ins or a user-level persona
    * file; an unresolved name warns and falls back to the engineer principles. */
   if (!persona_override || !persona_override[0])
      fatal("aimee delegate requires --persona <name> (e.g. engineer, qa, security, reviewer, "
            "architect, or a custom persona)");
   if (!persona_is_builtin(persona_override))
   {
      char ppath[PERSONA_PATH_MAX];
      if (persona_path(NULL, persona_override, ppath, sizeof(ppath)) != 0)
         fprintf(stderr, "aimee: warning: unknown persona '%s'; using engineer principles\n",
                 persona_override);
   }

   explicit_toolset = delegate_toolset_override_arg(&opts, argc, argv, &prompt);

   /* --prompt-file / --prompt-stdin: avoid shell quoting and ARG_MAX limits. */
   char *file_prompt = NULL;
   if (prompt_stdin)
   {
      file_prompt = delegate_read_prompt_stdin();
   }
   else if (prompt_file && prompt_file[0])
   {
      FILE *pf = fopen(prompt_file, "r");
      if (!pf)
         fatal("cannot open prompt file: %s", prompt_file);
      fseek(pf, 0, SEEK_END);
      long psz = ftell(pf);
      fseek(pf, 0, SEEK_SET);
      if (psz <= 0 || psz > 2 * 1024 * 1024)
      {
         fclose(pf);
         fatal("prompt file too large or empty (max 2MB)");
      }
      file_prompt = malloc((size_t)psz + 1);
      if (!file_prompt)
      {
         fclose(pf);
         fatal("out of memory reading prompt file");
      }
      size_t pnread = fread(file_prompt, 1, (size_t)psz, pf);
      file_prompt[pnread] = '\0';
      fclose(pf);
   }

   delegate_prompt_plan_t prompt_plan;
   if (!role || delegate_resolve_prompt_inputs(prompt, file_prompt, &prompt_plan) != 0)
      fatal("usage: aimee delegate <role> [\"prompt\"] [--tools] [--prompt-file "
            "PATH|--prompt-stdin]");

   const char *task_prompt = prompt_plan.task_prompt;
   prompt = prompt_plan.user_prompt;

   {
      const char *canonical = delegate_role_canonicalize(role);
      if (canonical != role)
         LOG_INFO("delegate", "delegate: role alias '%s' -> '%s'", role, canonical);
      role = canonical;
   }

   int force_tools = delegate_role_auto_tools_for_invocation(role, max_turns, explicit_tools);
   if (explicit_toolset && explicit_toolset[0])
   {
      char resolved[TOOLSET_MAX_TOOLS][TOOLSET_TOOL_MAX], toolset_err[TOOLSET_ERROR_MAX] = "";
      if (toolset_resolve_effective(explicit_toolset, resolved, TOOLSET_MAX_TOOLS, toolset_err,
                                    sizeof(toolset_err)) < 0)
         fatal("toolset '%s': %s", explicit_toolset,
               toolset_err[0] ? toolset_err : "failed to resolve");
      force_tools = 1;
      platform_setenv("AIMEE_ACTIVE_TOOLSET", explicit_toolset);
   }

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
      fatal("no agents configured. Run 'aimee agent local <name> <endpoint> --model MODEL' first.");

   {
      char route_err[256];
      unsigned required_caps = 0;
      int min_context = 0;
      int drop_deprecated = !(via_agent_name && via_agent_name[0]) &&
                            !(provider_override && provider_override[0]) &&
                            !(model_override && model_override[0]);
      delegate_infer_capability_requirements(prompt, force_tools, &required_caps, &min_context);
      if (delegate_apply_route_overrides(&cfg, role, via_agent_name, tier_override,
                                         provider_override, model_override, route_err,
                                         sizeof(route_err)) != 0)
         fatal("%s", route_err);
      if (delegate_filter_route_capabilities(&cfg, role, required_caps, min_context,
                                             drop_deprecated, route_err, sizeof(route_err)) != 0)
         fatal("%s", route_err);
      if (delegate_route_preflight(&cfg, role, route_err, sizeof(route_err)) != 0)
         fatal("%s", route_err);
   }
   if (timeout_ms > 0)
   {
      for (int i = 0; i < cfg.agent_count; i++)
         cfg.agents[i].timeout_ms = timeout_ms;
   }
   delegate_apply_max_turns_policy(&cfg, role, max_turns);

   /* Load role template as system prompt when none was explicitly provided */
   char *template_sys_prompt = NULL;
   char cwd_for_template[MAX_PATH_LEN];
   if (!getcwd(cwd_for_template, sizeof(cwd_for_template)))
      cwd_for_template[0] = '\0';
   if (!sys_prompt)
   {
      template_sys_prompt = role_template_build(cwd_for_template, role, task_prompt, NULL);
      if (template_sys_prompt)
      {
         sys_prompt = template_sys_prompt;

         /* Log which template source was used */
         char tpath[ROLE_TEMPLATE_PATH_MAX];
         if (role_template_path(cwd_for_template, role, tpath, sizeof(tpath)) == 0)
            LOG_INFO("delegate", "delegate [%s]: using template from %s", role, tpath);
         else
            LOG_INFO("delegate", "delegate [%s]: using built-in template", role);
      }
   }

   /* Fall back to tier-based prompt when no explicit prompt or role template */
   if (!sys_prompt)
   {
      config_t delegate_cfg;
      config_load(&delegate_cfg);
      prompt_tier_t dtier = delegate_cfg.delegate_prompt_tier[0]
                                ? prompt_tier_from_string(delegate_cfg.delegate_prompt_tier)
                                : PROMPT_MINIMAL;
      template_sys_prompt = prompt_build(dtier, cwd_for_template, NULL);
      if (template_sys_prompt)
         sys_prompt = template_sys_prompt;
   }
   if (template_sys_prompt)
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      if (config_load(&cfg) == 0)
      {
         char *with_dispositions = prompt_apply_dispositions(template_sys_prompt, &cfg);
         if (with_dispositions)
         {
            free(template_sys_prompt);
            template_sys_prompt = with_dispositions;
            sys_prompt = template_sys_prompt;
         }
      }
   }

   /* Auto-inject a skill matching the role name, if one exists */
   char *skill_sys_prompt = NULL;
   {
      char skill_path_buf[SKILL_PATH_MAX];
      if (skill_path(cwd_for_template, role, skill_path_buf, sizeof(skill_path_buf)) == 0)
      {
         skill_sys_prompt = skill_inject(cwd_for_template, sys_prompt, role);
         if (skill_sys_prompt)
         {
            /* skill_inject returned a new string; update sys_prompt pointer */
            if (template_sys_prompt)
               free(template_sys_prompt);
            template_sys_prompt = skill_sys_prompt;
            sys_prompt = skill_sys_prompt;
            skill_sys_prompt = NULL; /* ownership transferred to template_sys_prompt */
         }
      }
   }

   /* Inject tier orchestration context for tools-enabled mid-tier delegates */
   {
      char *tier_ctx = delegate_build_tier_context(via_agent_name, tier_override, role);
      if (tier_ctx)
      {
         size_t base_len = sys_prompt ? strlen(sys_prompt) : 0;
         size_t ctx_len = strlen(tier_ctx);
         char *combined = malloc(base_len + ctx_len + 1);
         if (combined)
         {
            if (sys_prompt)
               memcpy(combined, sys_prompt, base_len);
            memcpy(combined + base_len, tier_ctx, ctx_len + 1);
            if (template_sys_prompt)
               free(template_sys_prompt);
            template_sys_prompt = combined;
            sys_prompt = combined;
         }
         free(tier_ctx);
      }
   }

   /* Compose the required persona's identity + principles onto the assembled
    * role/skill prompt (kept at the top even for explicit --system prompts), so
    * e.g. `--persona security` reviews with the security reviewer's framing. */
   if (sys_prompt)
   {
      char *with_principles =
          persona_compose_delegate_prompt(persona_override, cwd_for_template, sys_prompt);
      if (with_principles)
      {
         if (template_sys_prompt)
            free(template_sys_prompt);
         template_sys_prompt = with_principles;
         sys_prompt = with_principles;
      }
   }

   /* Apply token budget: truncate sys_prompt if it exceeds the configured limit.
    * --token-budget N overrides; otherwise reads delegate_token_budget from
    * ~/.config/aimee/projects/<name>/project.yaml; defaults to 20K tokens. */
   if (sys_prompt)
   {
      int budget = token_budget_override > 0 ? token_budget_override
                                             : delegate_token_budget_load(cwd_for_template, role);
      char *limited = delegate_prompt_limit(sys_prompt, budget);
      if (limited)
      {
         if (template_sys_prompt)
            free(template_sys_prompt);
         template_sys_prompt = limited;
         sys_prompt = limited;
      }
   }

   /* Build enhanced prompt with file contents if --files specified */
   char *effective_prompt = prompt_plan.owned_user_prompt;
   /* Collect paths from --files for named-file drift guard.  Capped at 32. */
   const char *named_files[32];
   char named_files_storage[32][MAX_PATH_LEN];
   int named_file_count = 0;
   char source_paths[128][MAX_PATH_LEN];
   int source_path_count = 0;
   if (files_arg && files_arg[0])
   {
      /* Parse comma-separated file paths */
      const char *base = effective_prompt ? effective_prompt : prompt;
      size_t plen = strlen(base);
      size_t bufcap = plen + 256;
      char files_copy[MAX_PATH_LEN * 4];
      snprintf(files_copy, sizeof(files_copy), "%s", files_arg);

      /* First pass: accumulate file contents */
      char *file_block = malloc(128 * 1024); /* 128KB for file contents */
      size_t fpos = 0;
      if (file_block)
      {
         char *saveptr = NULL;
         char *path = strtok_r(files_copy, ",", &saveptr);
         while (path)
         {
            while (*path == ' ')
               path++;
            FILE *fp = fopen(path, "r");
            if (fp)
            {
               fpos += (size_t)snprintf(file_block + fpos, 128 * 1024 - fpos,
                                        "\n--- File: %s ---\n", path);
               size_t nread = fread(file_block + fpos, 1, 128 * 1024 - fpos - 1, fp);
               fpos += nread;
               file_block[fpos] = '\0';
               fclose(fp);
               /* Track for drift check */
               if (named_file_count < 32)
               {
                  snprintf(named_files_storage[named_file_count],
                           sizeof(named_files_storage[named_file_count]), "%s", path);
                  named_files[named_file_count] = named_files_storage[named_file_count];
                  named_file_count++;
               }
               delegate_source_path_add(source_paths, &source_path_count, 128, path);
            }
            else
            {
               fpos += (size_t)snprintf(file_block + fpos, 128 * 1024 - fpos,
                                        "\n--- File: %s (not found) ---\n", path);
               fprintf(stderr,
                       "aimee: warn: explicitly named file '%s' does not exist "
                       "(delegate will see a placeholder)\n",
                       path);
            }
            path = strtok_r(NULL, ",", &saveptr);
         }

         bufcap = plen + fpos + 128;
         char *combined = malloc(bufcap);
         if (combined)
         {
            snprintf(combined, bufcap, "%s\n\n# Source Packet: Pre-loaded File Contents\n%s", base,
                     file_block);
            free(effective_prompt);
            effective_prompt = combined;
         }
         free(file_block);
      }
   }

   /* --context-dir: bundle all source files from a directory into the prompt */
   if (context_dir && context_dir[0])
   {
      const char *base = effective_prompt ? effective_prompt : prompt;
      size_t plen = strlen(base);
      char *ctx_block = malloc(512 * 1024);
      size_t cpos = 0;
      if (ctx_block)
      {
         DIR *d = opendir(context_dir);
         if (d)
         {
            static const char *src_exts[] = {".c",   ".h",    ".js",   ".ts", ".py", ".go",
                                             ".rs",  ".java", ".cs",   ".rb", ".sh", ".yaml",
                                             ".yml", ".json", ".toml", ".md", NULL};
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && cpos < 480 * 1024)
            {
               if (ent->d_name[0] == '.')
                  continue;
               const char *dot = strrchr(ent->d_name, '.');
               if (!dot)
                  continue;
               int is_src = 0;
               for (int si = 0; src_exts[si]; si++)
               {
                  if (strcmp(dot, src_exts[si]) == 0)
                  {
                     is_src = 1;
                     break;
                  }
               }
               if (!is_src)
                  continue;

               char fpath[MAX_PATH_LEN];
               snprintf(fpath, sizeof(fpath), "%s/%s", context_dir, ent->d_name);
               struct stat fst;
               if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_size > 64 * 1024)
                  continue;

               FILE *fp = fopen(fpath, "r");
               if (!fp)
                  continue;
               cpos += (size_t)snprintf(ctx_block + cpos, 512 * 1024 - cpos, "\n--- File: %s ---\n",
                                        ent->d_name);
               size_t nread = fread(ctx_block + cpos, 1, 512 * 1024 - cpos - 1, fp);
               cpos += nread;
               ctx_block[cpos] = '\0';
               fclose(fp);
               delegate_source_path_add(source_paths, &source_path_count, 128, fpath);
            }
            closedir(d);
         }

         if (cpos > 0)
         {
            size_t bufcap = plen + cpos + 128;
            char *combined = malloc(bufcap);
            if (combined)
            {
               snprintf(combined, bufcap,
                        "%s\n\n# Source Packet: Pre-loaded Directory Contents (%s)\n%s", base,
                        context_dir, ctx_block);
               free(effective_prompt);
               effective_prompt = combined;
            }
         }
         free(ctx_block);
      }
   }

   /* --context-file: append individually-specified files to the prompt.
    * Applied after --files and --context-dir in the order provided. */
   if (context_file_count > 0)
   {
      const char *base = effective_prompt ? effective_prompt : prompt;
      size_t blen = strlen(base);
      char *cf_block = malloc(128 * 1024);
      size_t cfpos = 0;
      if (cf_block)
      {
         for (int i = 0; i < context_file_count && cfpos < 120 * 1024; i++)
         {
            FILE *fp = fopen(context_files[i], "r");
            if (fp)
            {
               cfpos += (size_t)snprintf(cf_block + cfpos, 128 * 1024 - cfpos,
                                         "\n--- File: %s ---\n", context_files[i]);
               size_t nread = fread(cf_block + cfpos, 1, 128 * 1024 - cfpos - 1, fp);
               cfpos += nread;
               cf_block[cfpos] = '\0';
               fclose(fp);
               delegate_source_path_add(source_paths, &source_path_count, 128, context_files[i]);
            }
            else
            {
               cfpos += (size_t)snprintf(cf_block + cfpos, 128 * 1024 - cfpos,
                                         "\n--- File: %s (not found) ---\n", context_files[i]);
            }
         }

         if (cfpos > 0)
         {
            size_t bufcap = blen + cfpos + 128;
            char *combined = malloc(bufcap);
            if (combined)
            {
               snprintf(combined, bufcap, "%s\n\n# Source Packet: Context Files\n%s", base,
                        cf_block);
               free(effective_prompt);
               effective_prompt = combined;
            }
         }
         free(cf_block);
      }
   }

   /* --context <symbols>: resolve comma-separated symbol names via the index
    * and inject relevant source excerpts into the prompt before spawning.
    * This eliminates Turn 1 discovery for known symbols. */
   if (context_syms && context_syms[0])
   {
      const char *base = effective_prompt ? effective_prompt : prompt;
      size_t blen = strlen(base);
      char *sym_block = malloc(128 * 1024);
      size_t sympos = 0;
      if (sym_block)
      {
         char syms_copy[2048];
         snprintf(syms_copy, sizeof(syms_copy), "%s", context_syms);
         char *tok = strtok(syms_copy, ",");
         while (tok && sympos < 120 * 1024)
         {
            while (*tok == ' ')
               tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && (*end == ' ' || *end == '\r' || *end == '\n'))
               *end-- = '\0';

            if (*tok)
            {
               /* shell_escape the symbol: it is interpolated into a popen()
                * command, so a raw single quote in the token would break out of
                * the quotes and inject shell commands. */
               char *esc_tok = shell_escape(tok);
               char find_cmd[512];
               snprintf(find_cmd, sizeof(find_cmd), "aimee index find '%s' 2>/dev/null", esc_tok);
               free(esc_tok);
               FILE *fp = popen(find_cmd, "r");
               if (fp)
               {
                  char iline[1024];
                  /* Take first "definition" match; fall back to any match */
                  char best_file[512] = "";
                  int best_lineno = 0;
                  while (fgets(iline, sizeof(iline), fp) && !best_lineno)
                  {
                     /* Format: "  <path>:<lineno>  <type>  [<proj>]" */
                     char *p = iline;
                     while (*p == ' ')
                        p++;
                     char *colon = strrchr(p, ':');
                     if (!colon)
                        continue;
                     char *sp = colon + 1;
                     int lineno = (int)strtol(sp, NULL, 10);
                     if (lineno <= 0)
                        continue;
                     *colon = '\0';
                     snprintf(best_file, sizeof(best_file), "%s", p);
                     best_lineno = lineno;
                  }
                  pclose(fp);

                  if (best_file[0] && best_lineno > 0)
                  {
                     FILE *src = fopen(best_file, "r");
                     if (src)
                     {
                        int start = best_lineno > 5 ? best_lineno - 5 : 1;
                        sympos +=
                            (size_t)snprintf(sym_block + sympos, 128 * 1024 - sympos,
                                             "\n## %s (%s:%d)\n```\n", tok, best_file, best_lineno);
                        int cur = 1;
                        char srcline[1024];
                        while (fgets(srcline, sizeof(srcline), src) && cur < start + 65 &&
                               sympos < 120 * 1024)
                        {
                           if (cur >= start)
                           {
                              size_t sll = strlen(srcline);
                              if (sympos + sll + 8 < 128 * 1024)
                              {
                                 memcpy(sym_block + sympos, srcline, sll);
                                 sympos += sll;
                              }
                           }
                           cur++;
                        }
                        fclose(src);
                        delegate_source_path_add(source_paths, &source_path_count, 128, best_file);
                        if (sympos < 120 * 1024)
                           sympos +=
                               (size_t)snprintf(sym_block + sympos, 128 * 1024 - sympos, "```\n");
                     }
                  }
               }
            }
            tok = strtok(NULL, ",");
         }

         if (sympos > 0)
         {
            size_t bufcap = blen + sympos + 128;
            char *combined = malloc(bufcap);
            if (combined)
            {
               snprintf(combined, bufcap, "%s\n\n# Source Packet: Pre-loaded Symbol Context\n%s",
                        base, sym_block);
               free(effective_prompt);
               effective_prompt = combined;
            }
         }
         free(sym_block);
      }
   }

   int delegate_allows_writes = delegate_prompt_allows_writes(prompt);
   if (worktree_branch && worktree_branch[0] && !delegate_allows_writes)
      fatal("--worktree is only valid for write-capable delegates; read-only delegates must "
            "use the parent worktree");
   int delegate_needs_worktree = delegate_allows_writes;
   effective_prompt =
       delegate_maybe_append_validation_bundle(role, cwd_for_template, effective_prompt, prompt);
   if (source_path_count > 0)
   {
      const char *base = effective_prompt ? effective_prompt : prompt;
      char *with_source_note = delegate_append_source_authority_note(base, source_path_count);
      if (with_source_note)
      {
         free(effective_prompt);
         effective_prompt = with_source_note;
      }
   }
   const char *final_prompt = effective_prompt ? effective_prompt : prompt;
   char *handoff_prompt = NULL;
   if (handoff_json)
   {
      handoff_prompt = delegate_handoff_append_contract(final_prompt, NULL);
      if (handoff_prompt)
         final_prompt = handoff_prompt;
   }

   /* Worktree: read-only delegates inspect the parent workspace directly.
    * Only write-capable delegates get an isolated sibling worktree. */
   char worktree_path[MAX_PATH_LEN] = "";
   char original_cwd[MAX_PATH_LEN] = "";
   char delegate_git_root[MAX_PATH_LEN] = "";
   char delegate_work_name[32] = "";
   /* Deterministic work-name from the session id so this dispatch path and the
    * server compute path resolve to the SAME sibling worktree (one per
    * delegation) instead of each spawning its own random one (which previously
    * produced two worktrees per delegate, one stale/orphaned). */
   worktree_delegate_work_name(session_id(), delegate_work_name, sizeof(delegate_work_name));
   {
      char cwd_buf[MAX_PATH_LEN];
      if (getcwd(cwd_buf, sizeof(cwd_buf)) &&
          git_repo_root(cwd_buf, delegate_git_root, sizeof(delegate_git_root)) == 0)
      {
         snprintf(original_cwd, sizeof(original_cwd), "%s", cwd_buf);
         const char *sid = session_id();

         if (delegate_needs_worktree && is_aimee_worktree_path(cwd_buf))
         {
            char managed_root[MAX_PATH_LEN];
            if (worktree_managed_git_root(cwd_buf, managed_root, sizeof(managed_root)) == 0)
            {
               snprintf(delegate_git_root, sizeof(delegate_git_root), "%s", managed_root);
            }
            else
            {
               /* Fallback for legacy worktree layouts that are not under the managed root. */
               char wt_cmd[MAX_PATH_LEN + 64];
               snprintf(wt_cmd, sizeof(wt_cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null",
                        cwd_buf);
               int wt_rc;
               char *wt_out = run_cmd(wt_cmd, &wt_rc);
               if (wt_rc == 0 && wt_out && wt_out[0])
               {
                  wt_out[strcspn(wt_out, "\r\n")] = '\0';
                  snprintf(worktree_path, sizeof(worktree_path), "%s", wt_out);
                  delegate_git_root[0] = '\0';
                  LOG_INFO("delegate", "delegate sharing primary worktree %s", worktree_path);
               }
               free(wt_out);
            }
         }
         if (delegate_needs_worktree && delegate_git_root[0])
         {
            if (worktree_create_sibling_from_anchor(delegate_git_root, sid, delegate_work_name,
                                                    cwd_buf) == 0)
            {
               worktree_sibling_path(delegate_git_root, sid, delegate_work_name, worktree_path,
                                     sizeof(worktree_path));

               /* Compute equivalent subpath inside the worktree. Managed worktree
                * callers map to the new delegate root instead of nesting .aimee. */
               size_t root_len = strlen(delegate_git_root);
               const char *suffix = "";
               if (!is_aimee_worktree_path(cwd_buf) &&
                   strncmp(cwd_buf, delegate_git_root, root_len) == 0)
                  suffix = cwd_buf + root_len;
               char target[MAX_PATH_LEN];
               snprintf(target, sizeof(target), "%s%s", worktree_path, suffix);

               struct stat tst;
               if (stat(target, &tst) == 0 && S_ISDIR(tst.st_mode))
               {
                  if (chdir(target) != 0)
                     LOG_WARN("delegate", "could not chdir to worktree: %s", target);
                  else
                     LOG_INFO("delegate", "delegate running in worktree %s", worktree_path);
               }
               else if (chdir(worktree_path) != 0)
               {
                  LOG_WARN("delegate", "could not chdir to worktree: %s", worktree_path);
               }
               else
               {
                  LOG_INFO("delegate", "delegate running in worktree %s", worktree_path);
               }

               /* If --worktree BRANCH was specified, check out that branch */
               if (worktree_branch && worktree_branch[0])
               {
                  char cmd[MAX_PATH_LEN + 256];
                  snprintf(cmd, sizeof(cmd), "git -C '%s' checkout '%s' 2>&1", worktree_path,
                           worktree_branch);
                  int git_rc;
                  char *git_out = run_cmd(cmd, &git_rc);
                  if (git_rc != 0)
                     LOG_WARN("delegate", "could not checkout branch '%s': %s", worktree_branch,
                              git_out ? git_out : "unknown");
                  free(git_out);
               }
            }
            else
            {
               LOG_WARN("delegate", "could not create sibling worktree for %s", delegate_git_root);
               delegate_git_root[0] = '\0';
            }
         }
      }
   }

   {
      char authority_root[MAX_PATH_LEN] = "";
      if (worktree_path[0])
         snprintf(authority_root, sizeof(authority_root), "%s", worktree_path);
      else if (getcwd(authority_root, sizeof(authority_root)) == NULL)
         authority_root[0] = '\0';
      delegate_source_authority_env_set((const char(*)[MAX_PATH_LEN])source_paths,
                                        source_path_count, authority_root);
   }

   if (dry_run)
   {
      /* Use same routing logic as real execution (respects --tier and --via pinning) */
      agent_t *ag = agent_route(&cfg, role);
      if (!ag)
         fatal("no agent available for role '%s'", role);

      char *assembled = agent_build_exec_context(ag, &cfg.network, sys_prompt);
      fprintf(stderr, "--- Dry Run ---\n");
      fprintf(stderr, "Agent:  %s\n", ag->name);
      fprintf(stderr, "Model:  %s\n", ag->model);
      fprintf(stderr, "Role:   %s\n", role);
      fprintf(stderr, "Tools:  %s\n", force_tools ? "enabled" : "off");
      fprintf(stderr, "\n--- System Prompt ---\n%s\n", assembled ? assembled : "(none)");
      fprintf(stderr, "\n--- User Prompt ---\n%s\n", final_prompt);
      if (worktree_path[0])
         fprintf(stderr, "\n--- Worktree: %s ---\n", worktree_path);
      free(assembled);
      return;
   }

   char launch_worktree_path[MAX_PATH_LEN] = "";
   if (worktree_path[0])
      snprintf(launch_worktree_path, sizeof(launch_worktree_path), "%s", worktree_path);
   else
      (void)getcwd(launch_worktree_path, sizeof(launch_worktree_path));
   char launch_head[64] = "";
   (void)delegate_git_head(launch_worktree_path, launch_head, sizeof(launch_head));
   char parent_worktree_path[MAX_PATH_LEN] = "";
   if (original_cwd[0])
      snprintf(parent_worktree_path, sizeof(parent_worktree_path), "%s", original_cwd);
   else
      snprintf(parent_worktree_path, sizeof(parent_worktree_path), "%s", launch_worktree_path);
   char parent_worktree_head[64] = "";
   (void)delegate_git_head(parent_worktree_path, parent_worktree_head,
                           sizeof(parent_worktree_head));
   char parent_worktree_fingerprint[64] = "";
   (void)delegate_git_worktree_fingerprint(parent_worktree_path, parent_worktree_fingerprint,
                                           sizeof(parent_worktree_fingerprint));

   if (background)
   {
      /* Fork a child to run the agent, parent returns immediately with task ID */
      char tasks_dir[MAX_PATH_LEN];
      snprintf(tasks_dir, sizeof(tasks_dir), "%s/tasks", config_default_dir());
      platform_mkdir_p(tasks_dir, 0700);

      char task_id[64];
      generate_task_id(task_id, sizeof(task_id));
      char result_path[MAX_PATH_LEN];
      snprintf(result_path, sizeof(result_path), "%s/%s.json", tasks_dir, task_id);

      platform_delegate_run_background(
          tasks_dir, task_id, result_path, json_output, &cfg, role, sys_prompt, final_prompt,
          max_tokens, force_tools, original_cwd, delegate_git_root, delegate_work_name,
          keep_worktree, launch_worktree_path, launch_head, parent_worktree_path,
          parent_worktree_head, parent_worktree_fingerprint, effective_prompt, file_prompt);
      return;
   }

   agent_http_init();
   {
      config_t db1_cfg;
      config_load(&db1_cfg);
      if (db1_init(db1_cfg.db1_path) != 0)
         fatal("cannot open database");
   }

   /* Create durable job record if requested */
   int job_id = 0;
   if (durable)
   {
      {
         char pid[32];
         snprintf(pid, sizeof(pid), "%d", (int)getpid());
         job_id = db1_agent_job_create(role, task_prompt, cfg.default_agent, pid);
      }
      if (job_id > 0)
      {
         char lease_owner[32];
         snprintf(lease_owner, sizeof(lease_owner), "%d", (int)getpid());
         if (db1_agent_job_take_lease(job_id, lease_owner) != 0)
         {
            db1_agent_job_update(job_id, "failed", 0, "failed to take durable job lease");
            fatal("failed to take durable job lease");
         }
         agent_set_durable_job(job_id);
         if (json_output)
            fprintf(stderr, "aimee: durable job created: %d\n", job_id);
      }
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = -1;
   int attempts = retry_count + 1;

   /* Self-correcting loop state (only used when loop_mode is set) */
   agent_loop_t ag_loop;
   if (loop_mode)
      agent_loop_init(&ag_loop, max_iter, completion_threshold, verify_cmd);

   /* Generate a delegation ID for checkpoint tracking */
   char deleg_id[32];
   generate_task_id(deleg_id, sizeof(deleg_id));

   /* OTel: start a delegation span covering the full delegate execution */
   otel_span_t otel_deleg_span;
   otel_delegation_start(&otel_deleg_span, role, task_prompt);

   /* Working prompt that may be augmented with prior attempt context */
   char *working_prompt = NULL;
   int bg_embed_suppressed_prev = platform_memory_background_embed_set_suppressed(1);

   for (int attempt = 0; attempt < attempts; attempt++)
   {
      if (attempt > 0)
      {
         fprintf(stderr, "aimee: retrying delegate (attempt %d/%d)...\n", attempt + 1, attempts);
         agent_http_cleanup();
         agent_http_init();

         /* Inject prior attempt context from checkpoint */
         char ckpt_steps[2048] = "";
         char ckpt_error[512] = "";
         char ckpt_output[2048] = "";
         if (db1_delegation_checkpoint_load(deleg_id, ckpt_steps, sizeof(ckpt_steps), ckpt_error,
                                            sizeof(ckpt_error), ckpt_output,
                                            sizeof(ckpt_output)) == 0)
         {
            free(working_prompt);
            size_t cap = strlen(final_prompt) + 4096;
            working_prompt = malloc(cap);
            if (working_prompt)
            {
               snprintf(working_prompt, cap,
                        "%s\n\n"
                        "[Prior attempt %d failed. Error: %s. "
                        "Partial progress: %s. "
                        "You may resume from where it left off or restart.]",
                        final_prompt, attempt, ckpt_error, ckpt_steps);
            }
         }
         memset(&result, 0, sizeof(result));
      }

      const char *prompt_to_use =
          (working_prompt && working_prompt[0]) ? working_prompt : final_prompt;

      if (coordination)
      {
         rc = agent_coordinate(&cfg, prompt_to_use, &result);
      }
      else if (vote_count > 0)
      {
         rc = agent_vote(&cfg, role, prompt_to_use, vote_count, &result);
      }
      else if (plan_mode)
      {
         agent_t *ag = agent_route(&cfg, role);
         if (!ag)
            fatal("no agent available for role '%s'", role);
         rc = agent_execute_with_plan(ag, &cfg.network, sys_prompt, prompt_to_use, max_tokens, 0.3,
                                      &result);
      }
      else if (force_tools)
      {
         rc = agent_run_with_tools(&cfg, role, sys_prompt, prompt_to_use, max_tokens, &result);
      }
      else if (loop_mode)
      {
         rc = agent_loop_run(&cfg, role, sys_prompt, prompt_to_use, max_tokens, &ag_loop, &result);
         if (rc == 0)
            fprintf(stderr, "aimee: loop done after %d iteration(s), completion=%d%%\n",
                    ag_loop.current_iteration, ag_loop.last_completion);
      }
      else
      {
         rc = agent_run(&cfg, role, sys_prompt, prompt_to_use, max_tokens, &result);
      }

      if (rc == 0)
         break;

      /* Save checkpoint on failure for potential retry */
      {
         char steps_json[2048];
         snprintf(steps_json, sizeof(steps_json), "[\"turns: %d\", \"tool_calls: %d\"]",
                  result.turns, result.tool_calls);
         const char *last_out = result.response ? result.response : result.error;
         /* Truncate last_output to 1024 chars for checkpoint */
         char truncated[1024];
         snprintf(truncated, sizeof(truncated), "%.1023s", last_out);
         db1_delegation_checkpoint_save(deleg_id, "", attempt, steps_json, truncated, result.error);
      }

      if (strstr(result.error, "HTTP") || strstr(result.error, "timeout"))
      {
         if (attempt < attempts - 1)
         {
            free(result.response);
            result.response = NULL;
            continue;
         }
      }
      break;
   }

   free(working_prompt);
   if (loop_mode)
      agent_loop_free(&ag_loop);
   if (rc == 0 && worktree_path[0])
      delegate_check_file_drift(worktree_path, named_files, named_file_count);
   delegate_apply_review_evidence_guard(
       role, worktree_path[0] ? worktree_path : (original_cwd[0] ? original_cwd : cwd_for_template),
       &rc, &result);
   delegate_handoff_validation_t handoff_validation;
   memset(&handoff_validation, 0, sizeof(handoff_validation));
   int handoff_checked = 0;
   if (handoff_json && rc == 0)
   {
      handoff_checked = 1;
      (void)delegate_handoff_validate_text(result.response, NULL, 1, &handoff_validation);
      if (!handoff_validation.valid)
      {
         char *repair_prompt =
             delegate_handoff_repair_prompt(result.response, handoff_validation.error);
         if (repair_prompt)
         {
            agent_result_t repaired;
            memset(&repaired, 0, sizeof(repaired));
            int repair_rc = force_tools ? agent_run_with_tools(&cfg, role, sys_prompt,
                                                               repair_prompt, max_tokens, &repaired)
                                        : agent_run(&cfg, role, sys_prompt, repair_prompt,
                                                    max_tokens, &repaired);
            free(repair_prompt);
            if (repair_rc == 0 && repaired.response)
            {
               free(result.response);
               result.response = repaired.response;
               repaired.response = NULL;
               result.turns += repaired.turns;
               result.tool_calls += repaired.tool_calls;
               if (repaired.agent_name[0])
                  snprintf(result.agent_name, sizeof(result.agent_name), "%s", repaired.agent_name);
               int repair_attempted = 1;
               (void)delegate_handoff_validate_text(result.response, NULL, 1, &handoff_validation);
               handoff_validation.repair_attempted = repair_attempted;
            }
            free(repaired.response);
         }
      }
      if (!handoff_validation.valid)
      {
         rc = 2;
         snprintf(result.error, sizeof(result.error), "invalid delegate handoff: %s",
                  handoff_validation.error[0] ? handoff_validation.error : "validation failed");
      }
   }
   agent_http_cleanup();
   platform_memory_background_embed_set_suppressed(bg_embed_suppressed_prev);

   if (rc == 0 &&
       liveness_reject_degenerate_response(&result.response, result.error, sizeof(result.error),
                                           "delegate returned raw tool-call markup or "
                                           "another degenerate response"))
   {
      rc = -1;
      result.success = 0;
   }

   /* Update durable job with result */
   if (durable && job_id > 0)
   {
      db1_agent_job_update(job_id, rc == 0 ? "done" : "failed", result.turns,
                           result.response ? result.response : result.error);
      agent_set_durable_job(0);
   }

   /* Fire completion event */
   if (rc == 0)
   {
      char notify_msg[256];
      snprintf(notify_msg, sizeof(notify_msg), "delegate %s complete", role);
      event_notify(AIMEE_EVENT_DELEGATE_COMPLETE, notify_msg);
   }
   else
   {
      char notify_msg[256];
      snprintf(notify_msg, sizeof(notify_msg), "delegate %s failed: %s", role, result.error);
      event_notify(AIMEE_EVENT_DELEGATE_FAILED, notify_msg);
   }

   /* Channel mirroring: if --channel was given and delegate succeeded, advance
    * the workflow session bound to that channel using the delegate's output.
    * Failures are advisory — a missing or ended session is a warning, not an error. */
   if (channel && channel[0] && rc == 0 && result.response && result.response[0])
   {
      char wf_err[256];
      int wf_id = 0;
      int wf_rc =
          db1_workflow_session_find_current_by_channel(channel, &wf_id, wf_err, sizeof(wf_err));
      if (wf_rc != 0)
      {
         LOG_WARN("delegate", "channel mirror: no active session on channel '%s': %s", channel,
                  wf_err);
      }
      else
      {
         workflow_session_info_t wf_out;
         char *wf_next_prompt = NULL;
         char advance_err[256];
         /* Use the delegate's role as the speaker for workflow advancement */
         int adv_rc =
             db1_workflow_session_advance(wf_id, role, result.response, &wf_out, &wf_next_prompt,
                                          advance_err, sizeof(advance_err));
         free(wf_next_prompt);
         if (adv_rc != 0)
            LOG_WARN("delegate", "channel mirror: session_advance on channel '%s' failed: %s",
                     channel, advance_err);
         else
            LOG_INFO("delegate", "channel '%s': session %d advanced to phase %d (turn %d/%d)",
                     channel, wf_out.id, wf_out.current_phase, wf_out.current_turn,
                     wf_out.turns_in_phase);
      }
   }

   /* --verify: run a verification command after successful delegation.
    * Skipped when --loop is set because the loop already ran the verify command
    * as part of its completion criteria. */
   if (rc == 0 && verify_cmd && verify_cmd[0] && !loop_mode)
   {
      const char *verify_argv[] = {"/bin/sh", "-c", verify_cmd, NULL};
      char *verify_out = NULL;
      int verify_rc = safe_exec_capture(verify_argv, &verify_out, AGENT_TOOL_OUTPUT_MAX);
      free(verify_out);
      if (verify_rc != 0)
      {
         event_notify(AIMEE_EVENT_VERIFY_FAIL, "delegate verify failed");
         fprintf(stderr, "aimee: verify command failed (exit %d): %s\n", verify_rc, verify_cmd);
         if (json_output)
         {
            cJSON *obj = agent_result_to_json(&result);
            delegate_economics_add_agent_result_json(obj, &cfg, role, &result,
                                                     agent_route(&cfg, role));
            cJSON_AddBoolToObject(obj, "verify_passed", 0);
            cJSON_AddNumberToObject(obj, "verify_exit_code", verify_rc);
            if (handoff_checked)
               delegate_handoff_add_validation_json(obj, &handoff_validation);
            char *json = cJSON_Print(obj);
            if (json)
            {
               printf("%s\n", json);
               free(json);
            }
            cJSON_Delete(obj);
         }
         else
         {
            if (result.response)
               printf("%s\n", result.response);
            fprintf(stderr, "aimee: WARN: delegate output above may contain errors "
                            "(verify failed)\n");
         }
         otel_delegation_end(&otel_deleg_span, 1);
         delegate_worktree_restore(original_cwd, delegate_git_root, delegate_work_name,
                                   keep_worktree);
         free(result.response);
         free(handoff_prompt);
         free(effective_prompt);
         free(file_prompt);
         free(template_sys_prompt);
         db1_shutdown();
         exit(3);
      }
      else
      {
         event_notify(AIMEE_EVENT_VERIFY_PASS, "delegate verify passed");
         if (!json_output)
            fprintf(stderr, "aimee: verify passed: %s\n", verify_cmd);
      }
   }
   else if (rc == 0)
   {
      /* Auto-verify from app config: tool verifies delegate's changes */
      config_t app_cfg;
      config_load(&app_cfg);
      if (app_cfg.cross_verify && app_cfg.verify_cmd[0])
      {
         fprintf(stderr, "aimee: cross-verify: running %s\n", app_cfg.verify_cmd);
         const char *cv_argv[] = {"/bin/sh", "-c", app_cfg.verify_cmd, NULL};
         char *cv_out = NULL;
         int cv_rc = safe_exec_capture(cv_argv, &cv_out, AGENT_TOOL_OUTPUT_MAX);
         free(cv_out);
         if (cv_rc != 0)
         {
            event_notify(AIMEE_EVENT_VERIFY_FAIL, "cross-verify failed");
            fprintf(stderr, "aimee: cross-verify FAILED (exit %d)\n", cv_rc);
         }
         else
         {
            event_notify(AIMEE_EVENT_VERIFY_PASS, "cross-verify passed");
            if (!json_output)
               fprintf(stderr, "aimee: cross-verify passed\n");
         }
      }
   }

   int delegate_applied_changes = -1;
   char delegate_apply_error[512] = "";
   char delegate_parent_root[MAX_PATH_LEN] = "";
   if (rc == 0 && delegate_role_is_write(role) && delegate_allows_writes && worktree_path[0] &&
       delegate_git_root[0])
   {
      if (worktree_apply_delegate_changes_checked(
              worktree_path, original_cwd, launch_head, &delegate_applied_changes,
              delegate_parent_root, sizeof(delegate_parent_root), delegate_apply_error,
              sizeof(delegate_apply_error)) != 0)
      {
         rc = -1;
         result.success = 0;
         snprintf(result.error, sizeof(result.error), "delegate %s: %s", role,
                  delegate_apply_error[0] ? delegate_apply_error
                                          : "failed to apply changes to parent worktree");
      }
      else if (!json_output)
         fprintf(stderr, "aimee: applied %d delegate change(s) to %s\n", delegate_applied_changes,
                 delegate_parent_root);
   }

   if (json_output)
   {
      cJSON *obj = agent_result_to_json(&result);
      delegate_economics_add_agent_result_json(obj, &cfg, role, &result, agent_route(&cfg, role));
      if (verify_cmd && verify_cmd[0] && rc == 0 && !loop_mode)
         cJSON_AddBoolToObject(obj, "verify_passed", 1);
      if (loop_mode)
      {
         cJSON_AddNumberToObject(obj, "loop_iterations", ag_loop.current_iteration);
         cJSON_AddNumberToObject(obj, "loop_completion", ag_loop.last_completion);
      }
      if (handoff_checked)
         delegate_handoff_add_validation_json(obj, &handoff_validation);
      if (delegate_applied_changes >= 0)
         cJSON_AddNumberToObject(obj, "applied_changes", delegate_applied_changes);
      if (delegate_apply_error[0])
         cJSON_AddStringToObject(obj, "apply_error", delegate_apply_error);
      delegate_checkout_add_result_ex(obj, launch_worktree_path, launch_head, parent_worktree_path,
                                      parent_worktree_head, parent_worktree_fingerprint);
      char *json = cJSON_Print(obj);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(obj);
   }
   else if (rc == 0 && result.response)
   {
      if (output_path && output_path[0])
      {
         /* --output: write response to file, creating parent dirs if needed */
         char parent[MAX_PATH_LEN];
         snprintf(parent, sizeof(parent), "%s", output_path);
         char *sl = strrchr(parent, '/');
         if (sl && sl != parent)
         {
            *sl = '\0';
            /* Simple recursive mkdir: walk forward creating each component */
            for (char *p = parent + 1; *p; p++)
            {
               if (*p == '/')
               {
                  *p = '\0';
                  platform_mkdir_p(parent, 0755);
                  *p = '/';
               }
            }
            platform_mkdir_p(parent, 0755);
         }
         FILE *of = fopen(output_path, "w");
         if (!of)
         {
            fprintf(stderr, "aimee: cannot write to --output path: %s\n", output_path);
            otel_delegation_end(&otel_deleg_span, 1);
            delegate_worktree_restore(original_cwd, delegate_git_root, delegate_work_name,
                                      keep_worktree);
            free(result.response);
            free(handoff_prompt);
            free(effective_prompt);
            free(file_prompt);
            free(template_sys_prompt);
            db1_shutdown();
            exit(2);
         }
         fputs(result.response, of);
         fputc('\n', of);
         fclose(of);
         fprintf(stderr, "%s\n", output_path);
      }
      else
      {
         printf("%s\n", result.response);
      }
      if (handoff_checked && (!handoff_validation.valid || handoff_validation.error[0]))
         fprintf(stderr, "aimee: handoff %s: %s\n", handoff_validation.status,
                 handoff_validation.error[0] ? handoff_validation.error : "ok");
   }
   else
   {
      fprintf(stderr, "aimee delegate failed: %s\n", result.error);
      otel_delegation_end(&otel_deleg_span, 1);
      delegate_worktree_restore(original_cwd, delegate_git_root, delegate_work_name, keep_worktree);
      free(result.response);
      free(handoff_prompt);
      free(effective_prompt);
      free(file_prompt);
      free(template_sys_prompt);
      db1_shutdown();
      exit(2);
   }

   otel_delegation_end(&otel_deleg_span, 0);
   delegate_worktree_restore(original_cwd, delegate_git_root, delegate_work_name, keep_worktree);

   free(result.response);
   free(handoff_prompt);
   free(effective_prompt);
   free(file_prompt);
   free(template_sys_prompt);
   db1_shutdown();
}

/* --- cmd_verify: cross-verification ---
 * Two modes:
 *   aimee verify             -- delegate verifies AI tool's changes (uses config)
 *   aimee verify --cmd CMD   -- run CMD to verify delegate's changes
 *   aimee verify enable      -- enable cross-verification
 *   aimee verify disable     -- disable cross-verification
 *   aimee verify config      -- show current cross-verify config
 *   aimee verify config --verify-cmd CMD --role R --prompt P  -- configure
 */

void cmd_verify(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   config_t cfg;
   config_load(&cfg);

   if (argc >= 1 && strcmp(argv[0], "enable") == 0)
   {
      cfg.cross_verify = 1;
      config_save(&cfg);
      printf("cross-verification enabled\n");
      return;
   }

   if (argc >= 1 && strcmp(argv[0], "disable") == 0)
   {
      cfg.cross_verify = 0;
      config_save(&cfg);
      printf("cross-verification disabled\n");
      return;
   }

   if (argc >= 1 && strcmp(argv[0], "config") == 0)
   {
      if (argc == 1)
      {
         printf("cross_verify: %s\n", cfg.cross_verify ? "enabled" : "disabled");
         printf("verify_cmd: %s\n", cfg.verify_cmd[0] ? cfg.verify_cmd : "(not set)");
         printf("verify_role: %s\n", cfg.verify_role[0] ? cfg.verify_role : "review");
         printf("verify_prompt: %s\n", cfg.verify_prompt[0] ? cfg.verify_prompt : "(default)");
         return;
      }
      /* Configure: --verify-cmd, --role, --prompt */
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--verify-cmd") == 0 && i + 1 < argc)
            snprintf(cfg.verify_cmd, sizeof(cfg.verify_cmd), "%s", argv[++i]);
         else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc)
            snprintf(cfg.verify_role, sizeof(cfg.verify_role), "%s", argv[++i]);
         else if (strcmp(argv[i], "--prompt") == 0 && i + 1 < argc)
            snprintf(cfg.verify_prompt, sizeof(cfg.verify_prompt), "%s", argv[++i]);
      }
      config_save(&cfg);
      printf("cross-verify config updated\n");
      return;
   }

   /* Default: delegate verifies current changes */
   if (!cfg.cross_verify)
   {
      fprintf(stderr, "cross-verification is disabled. Run: aimee verify enable\n");
      return;
   }

   /* Step 1: run verify_cmd if set (compilation/tests) */
   if (cfg.verify_cmd[0])
   {
      fprintf(stderr, "aimee: running verify command: %s\n", cfg.verify_cmd);
      const char *cmd_argv[] = {"/bin/sh", "-c", cfg.verify_cmd, NULL};
      char *cmd_out = NULL;
      int cmd_rc = safe_exec_capture(cmd_argv, &cmd_out, AGENT_TOOL_OUTPUT_MAX);
      if (cmd_rc != 0)
      {
         fprintf(stderr, "aimee: verify command failed (exit %d)\n", cmd_rc);
         if (cmd_out && cmd_out[0])
            fprintf(stderr, "%s\n", cmd_out);
         free(cmd_out);
         exit(3);
      }
      free(cmd_out);
      fprintf(stderr, "aimee: verify command passed\n");
   }

   /* Step 2: delegate a review to an agent */
   const char *role = cfg.verify_role[0] ? cfg.verify_role : "review";

   /* Build the review prompt: get the current staged + unstaged diff as context. */
   const char *diff_argv[] = {"git", "diff", "HEAD", NULL};
   char *diff_out = NULL;
   safe_exec_capture(diff_argv, &diff_out, AGENT_TOOL_OUTPUT_MAX);

   char *review_prompt = NULL;
   const char *base_prompt =
       cfg.verify_prompt[0]
           ? cfg.verify_prompt
           : "Review these code changes for bugs, security issues, and correctness. "
             "If everything looks good, say LGTM. If you find problems, list them.";

   if (diff_out && diff_out[0])
   {
      size_t plen = strlen(base_prompt) + strlen(diff_out) + 32;
      review_prompt = malloc(plen);
      if (review_prompt)
         snprintf(review_prompt, plen, "%s\n\nDiff:\n%s", base_prompt, diff_out);
   }
   free(diff_out);

   const char *final_prompt = review_prompt ? review_prompt : base_prompt;

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
   {
      fprintf(stderr, "no agents configured, skipping delegate review\n");
      free(review_prompt);
      return;
   }

   agent_http_init();

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = agent_run(&acfg, role, NULL, final_prompt, 0, &result);
   agent_http_cleanup();

   if (rc == 0 && result.response)
   {
      printf("%s\n", result.response);
   }
   else
   {
      fprintf(stderr, "aimee: delegate review failed: %s\n", result.error);
   }

   free(result.response);
   free(review_prompt);
}

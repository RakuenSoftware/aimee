/* cmd_data.c: data management commands (db, export, import, config) */
#include "aimee.h"
#include "db1.h"
#include "db2/memory_payload.h"
#include "db2/memory_query.h"
#include "kb_client.h"
#include "lifecycle.h"
#include "log.h"
#include "platform_path.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "workspace.h"
#include "commands.h"
#include "dashboard.h"
#include "memory.h"
#include "cJSON.h"
#include "dstr.h"
#include "headers/mcp_git.h"
#include "headers/git_verify.h"
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

#define CMD_DATA_ERRBUF 256

void cmd_db(app_ctx_t *ctx, int argc, char **argv)
{
   const char *sub = (argc > 0) ? argv[0] : NULL;
   if (argc > 0)
   {
      argc--;
      argv++;
   }

   if (subcmd_dispatch(get_db_subcmds(), sub, ctx, argc, argv) != 0)
      subcmd_usage("db", get_db_subcmds());
}

/* --- cmd_db: status and pragma subcmds --- */

void cmd_export(app_ctx_t *ctx, int argc, char **argv)
{
   const char *category = "all";
   const char *output = "aimee-export";

   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--category=", 11) == 0 || strncmp(argv[i], "--category", 10) == 0)
      {
         if (argv[i][10] == '=')
            category = argv[i] + 11;
         else if (i + 1 < argc)
            category = argv[++i];
      }
      else if (strncmp(argv[i], "--output=", 9) == 0 || strncmp(argv[i], "--output", 8) == 0)
      {
         if (argv[i][8] == '=')
            output = argv[i] + 9;
         else if (i + 1 < argc)
            output = argv[++i];
      }
      else if (strcmp(argv[i], "--help") == 0)
      {
         fprintf(stderr, "Usage: aimee export [--category <cat>] [--output <path>]\n"
                         "Categories: all, memories, rules, config, decisions\n");
         return;
      }
   }

   (void)ctx;

   /* Create output directory */
   platform_mkdir_p(output, 0755);

   int total = 0;

   /* Manifest */
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/manifest.json", output);
      cJSON *m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "version", AIMEE_VERSION);
      cJSON_AddStringToObject(m, "category", category);

      char ts[32];
      now_utc(ts, sizeof(ts));
      cJSON_AddStringToObject(m, "exported_at", ts);
      cJSON_AddStringToObject(m, "platform", "linux");

      char *json = cJSON_Print(m);
      cJSON_Delete(m);
      if (json)
      {
         FILE *f = fopen(path, "w");
         if (f)
         {
            fprintf(f, "%s\n", json);
            fclose(f);
         }
         free(json);
      }
   }

   /* Memories */
   if (strcmp(category, "all") == 0 || strcmp(category, "memories") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/memories.jsonl", output);
      int n = kb_client_memory_export_jsonl(path);
      if (n >= 0)
      {
         printf("Exported %d memories\n", n);
         total += n;
      }
   }

   /* Rules */
   if (strcmp(category, "all") == 0 || strcmp(category, "rules") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/rules.jsonl", output);
      int n = kb_client_rules_export_jsonl(path);
      if (n >= 0)
      {
         printf("Exported %d rules\n", n);
         total += n;
      }
   }

   /* Decisions */
   if (strcmp(category, "all") == 0 || strcmp(category, "decisions") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/decisions.jsonl", output);
      int n = kb_client_memory_decisions_export_jsonl(path);
      if (n >= 0)
      {
         printf("Exported %d decisions\n", n);
         total += n;
      }
   }

   /* Config */
   if (strcmp(category, "all") == 0 || strcmp(category, "config") == 0)
   {
      config_t cfg;
      if (config_load(&cfg) == 0)
      {
         char path[4096];
         snprintf(path, sizeof(path), "%s/config.json", output);
         cJSON *c = cJSON_CreateObject();
         cJSON_AddStringToObject(c, "guardrail_mode", config_guardrail_mode(&cfg));
         /* Redact sensitive fields */
         cJSON_AddStringToObject(c, "note", "Sensitive fields redacted. Re-configure on target.");

         char *json = cJSON_Print(c);
         cJSON_Delete(c);
         if (json)
         {
            FILE *f = fopen(path, "w");
            if (f)
            {
               fprintf(f, "%s\n", json);
               fclose(f);
            }
            free(json);
         }
         printf("Exported config\n");
      }
   }

   printf("Export complete: %d items to %s/\n", total, output);
}

void cmd_import(app_ctx_t *ctx, int argc, char **argv)
{
   const char *input = NULL;
   const char *category = "all";
   const char *conflict = "skip";

   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--category=", 11) == 0)
         category = argv[i] + 11;
      else if (strncmp(argv[i], "--conflict=", 11) == 0)
         conflict = argv[i] + 11;
      else if (strcmp(argv[i], "--help") == 0)
      {
         fprintf(stderr,
                 "Usage: aimee import <path> [--category <cat>] [--conflict skip|overwrite]\n");
         return;
      }
      else if (argv[i][0] != '-')
      {
         input = argv[i];
      }
   }

   if (!input)
   {
      fprintf(stderr,
              "Usage: aimee import <path> [--category <cat>] [--conflict skip|overwrite]\n");
      return;
   }

   /* Read manifest */
   char manifest_path[4096];
   snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", input);
   dstr_t mbuf;
   dstr_init(&mbuf);
   if (dstr_read_file(&mbuf, manifest_path) != 0)
   {
      fprintf(stderr, "error: cannot open %s (is this an aimee export directory?)\n",
              manifest_path);
      dstr_free(&mbuf);
      return;
   }
   cJSON *manifest = cJSON_Parse(dstr_cstr(&mbuf));
   dstr_free(&mbuf);
   if (!manifest)
   {
      fprintf(stderr, "error: invalid manifest.json\n");
      return;
   }
   cJSON *jver = cJSON_GetObjectItemCaseSensitive(manifest, "version");
   printf("Importing from %s (exported by aimee %s)\n", input,
          cJSON_IsString(jver) ? jver->valuestring : "unknown");
   cJSON_Delete(manifest);

   (void)ctx;

   int total_imported = 0;
   int total_skipped = 0;

   /* Import memories */
   if (strcmp(category, "all") == 0 || strcmp(category, "memories") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/memories.jsonl", input);
      FILE *f = fopen(path, "r");
      if (f)
      {
         char line[65536];
         int imported = 0, skipped = 0;
         while (fgets(line, sizeof(line), f))
         {
            cJSON *obj = cJSON_Parse(line);
            if (!obj)
               continue;

            cJSON *jkey = cJSON_GetObjectItemCaseSensitive(obj, "key");
            cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(obj, "content");
            cJSON *jtier = cJSON_GetObjectItemCaseSensitive(obj, "tier");
            cJSON *jkind = cJSON_GetObjectItemCaseSensitive(obj, "kind");
            cJSON *jconf = cJSON_GetObjectItemCaseSensitive(obj, "confidence");

            if (!cJSON_IsString(jkey) || !cJSON_IsString(jcontent))
            {
               cJSON_Delete(obj);
               continue;
            }

            /* Check for duplicate by key */
            if (strcmp(conflict, "skip") == 0 && kb_client_memory_key_exists(jkey->valuestring))
            {
               skipped++;
               cJSON_Delete(obj);
               continue;
            }

            memory_t m;
            const char *tier = cJSON_IsString(jtier) ? jtier->valuestring : "L2";
            const char *kind = cJSON_IsString(jkind) ? jkind->valuestring : "fact";
            double conf = cJSON_IsNumber(jconf) ? jconf->valuedouble : 1.0;
            kb_client_memory_insert(tier, kind, jkey->valuestring, jcontent->valuestring, conf,
                                    "import", &m);
            imported++;
            cJSON_Delete(obj);
         }
         fclose(f);
         printf("Memories: %d imported, %d skipped\n", imported, skipped);
         total_imported += imported;
         total_skipped += skipped;
      }
   }

   /* Import rules */
   if (strcmp(category, "all") == 0 || strcmp(category, "rules") == 0)
   {
      char path[4096];
      snprintf(path, sizeof(path), "%s/rules.jsonl", input);
      FILE *f = fopen(path, "r");
      if (f)
      {
         char rline[65536];
         int imported = 0, skipped = 0;
         while (fgets(rline, sizeof(rline), f))
         {
            cJSON *obj = cJSON_Parse(rline);
            if (!obj)
               continue;

            cJSON *jpol = cJSON_GetObjectItemCaseSensitive(obj, "polarity");
            cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(obj, "title");
            cJSON *jdesc = cJSON_GetObjectItemCaseSensitive(obj, "description");
            cJSON *jwt = cJSON_GetObjectItemCaseSensitive(obj, "weight");

            if (!cJSON_IsString(jtitle))
            {
               cJSON_Delete(obj);
               continue;
            }

            int rc = kb_client_rules_insert(cJSON_IsString(jpol) ? jpol->valuestring : "positive",
                                            jtitle->valuestring,
                                            cJSON_IsString(jdesc) ? jdesc->valuestring : "",
                                            cJSON_IsNumber(jwt) ? (int)jwt->valuedouble : 5);
            if (rc == 0)
               imported++;
            else
               LOG_ERROR("cmd_core", "rule import failed");
            cJSON_Delete(obj);
         }
         fclose(f);
         printf("Rules: %d imported, %d skipped\n", imported, skipped);
         total_imported += imported;
         total_skipped += skipped;
      }
   }

   printf("Import complete: %d imported, %d skipped\n", total_imported, total_skipped);
}

/* --- cmd_config --- */

typedef enum
{
   CFG_STRING,
   CFG_BOOL,
   CFG_INT,
   CFG_FLOAT
} config_field_type_t;

typedef struct
{
   const char *key;
   size_t offset;
   size_t size;
   int is_bool; /* 1 for bool fields */
   config_field_type_t type;
} config_field_t;

static const config_field_t config_fields[] = {
    {"db2_url", offsetof(config_t, db2_url), sizeof(((config_t *)0)->db2_url), 0, CFG_STRING},
    {"provider", offsetof(config_t, provider), sizeof(((config_t *)0)->provider), 0, CFG_STRING},
    {"claude_model", offsetof(config_t, claude_model), sizeof(((config_t *)0)->claude_model), 0,
     CFG_STRING},
    {"openai_endpoint", offsetof(config_t, openai_endpoint),
     sizeof(((config_t *)0)->openai_endpoint), 0, CFG_STRING},
    {"openai_model", offsetof(config_t, openai_model), sizeof(((config_t *)0)->openai_model), 0,
     CFG_STRING},
    {"openai_key_cmd", offsetof(config_t, openai_key_cmd), sizeof(((config_t *)0)->openai_key_cmd),
     0, CFG_STRING},
    {"guardrail_mode", offsetof(config_t, guardrail_mode), sizeof(((config_t *)0)->guardrail_mode),
     0, CFG_STRING},
    {"embedding_command", offsetof(config_t, embedding_command),
     sizeof(((config_t *)0)->embedding_command), 0, CFG_STRING},
    {"embedding_model", offsetof(config_t, embedding_model),
     sizeof(((config_t *)0)->embedding_model), 0, CFG_STRING},
    {"embedding_endpoint", offsetof(config_t, embedding_endpoint),
     sizeof(((config_t *)0)->embedding_endpoint), 0, CFG_STRING},
    {"embedding_dim", offsetof(config_t, embedding_dim), sizeof(((config_t *)0)->embedding_dim), 0,
     CFG_INT},
    {"memory_coref_mode", offsetof(config_t, memory_coref_mode),
     sizeof(((config_t *)0)->memory_coref_mode), 0, CFG_STRING},
    {"memory_coref_window", offsetof(config_t, memory_coref_window),
     sizeof(((config_t *)0)->memory_coref_window), 0, CFG_INT},
    {"memory_rerank_mode", offsetof(config_t, memory_rerank_mode),
     sizeof(((config_t *)0)->memory_rerank_mode), 0, CFG_STRING},
    {"memory_rerank_enabled", offsetof(config_t, memory_rerank_enabled), sizeof(int), 0, CFG_BOOL},
    {"memory_rerank_command", offsetof(config_t, memory_rerank_command),
     sizeof(((config_t *)0)->memory_rerank_command), 0, CFG_STRING},
    {"memory_rerank_top_k", offsetof(config_t, memory_rerank_top_k), sizeof(int), 0, CFG_INT},
    {"memory_query_expansion_mode", offsetof(config_t, memory_query_expansion_mode),
     sizeof(((config_t *)0)->memory_query_expansion_mode), 0, CFG_STRING},
    {"memory_query_expansion_k", offsetof(config_t, memory_query_expansion_k), sizeof(int), 0,
     CFG_INT},
    {"autonomous", offsetof(config_t, autonomous), sizeof(int), 1, CFG_BOOL},
    {"verify_enabled", offsetof(config_t, verify_enabled), sizeof(int), 1, CFG_BOOL},
    {"verify_cross_project", offsetof(config_t, verify_cross_project), sizeof(int), 1, CFG_BOOL},
    {"cross_verify", offsetof(config_t, cross_verify), sizeof(int), 1, CFG_BOOL},
    {"ecomode", offsetof(config_t, ecomode), sizeof(int), 1, CFG_BOOL},
    {"max_iterations", offsetof(config_t, max_iterations), sizeof(int), 0, CFG_INT},
    {"max_iterations_delegate", offsetof(config_t, max_iterations_delegate), sizeof(int), 0,
     CFG_INT},
    {"memory_maintenance_trigger_inserts", offsetof(config_t, memory_maintenance_trigger_inserts),
     sizeof(int), 0, CFG_INT},
    {"memory_maintenance_trigger_secs", offsetof(config_t, memory_maintenance_trigger_secs),
     sizeof(int), 0, CFG_INT},
    {"memory_rerank_enabled", offsetof(config_t, memory_rerank_enabled), sizeof(int), 0, CFG_BOOL},
    {"memory_rerank_command", offsetof(config_t, memory_rerank_command),
     sizeof(((config_t *)0)->memory_rerank_command), 0, CFG_STRING},
    {"memory_rerank_top_k", offsetof(config_t, memory_rerank_top_k), sizeof(int), 0, CFG_INT},
    {"memory_query_expansion_mode", offsetof(config_t, memory_query_expansion_mode),
     sizeof(((config_t *)0)->memory_query_expansion_mode), 0, CFG_STRING},
    {"memory_query_expansion_k", offsetof(config_t, memory_query_expansion_k), sizeof(int), 0,
     CFG_INT},
    {"memory_profile_cards_enabled", offsetof(config_t, memory_profile_cards_enabled), sizeof(int),
     0, CFG_BOOL},
    {"memory_profile_cards_min_obs", offsetof(config_t, memory_profile_cards_min_obs), sizeof(int),
     0, CFG_INT},
    {"memory_profile_cards_stale_secs", offsetof(config_t, memory_profile_cards_stale_secs),
     sizeof(int), 0, CFG_INT},
    {"memory_rewrite_enabled", offsetof(config_t, memory_rewrite_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_rewrite_command", offsetof(config_t, memory_rewrite_command),
     sizeof(((config_t *)0)->memory_rewrite_command), 0, CFG_STRING},
    {"memory_rewrite_hyde", offsetof(config_t, memory_rewrite_hyde), sizeof(int), 0, CFG_BOOL},
    {"memory_rewrite_decompose", offsetof(config_t, memory_rewrite_decompose), sizeof(int), 0,
     CFG_BOOL},
    {"memory_rewrite_max_subqueries", offsetof(config_t, memory_rewrite_max_subqueries),
     sizeof(int), 0, CFG_INT},
    {"memory_window_radius", offsetof(config_t, memory_window_radius), sizeof(int), 0, CFG_INT},
    {"memory_kb_neighbour_expand", offsetof(config_t, memory_kb_neighbour_expand), sizeof(int), 0,
     CFG_BOOL},
    {"kb_search_max_results", offsetof(config_t, kb_search_max_results), sizeof(int), 0, CFG_INT},
    {"memory_negation_enabled", offsetof(config_t, memory_negation_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_scenes_enabled", offsetof(config_t, memory_scenes_enabled), sizeof(int), 0, CFG_BOOL},
    {"memory_scenes_min_cluster_size", offsetof(config_t, memory_scenes_min_cluster_size),
     sizeof(int), 0, CFG_INT},
    {"memory_scenes_top_m", offsetof(config_t, memory_scenes_top_m), sizeof(int), 0, CFG_INT},
    {"memory_bm25_weight", offsetof(config_t, memory_bm25_weight), sizeof(double), 0, CFG_FLOAT},
    {"memory_semantic_weight", offsetof(config_t, memory_semantic_weight), sizeof(double), 0,
     CFG_FLOAT},
    {"memory_fetch_budget_enabled", offsetof(config_t, memory_fetch_budget_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"memory_fetch_budget_base", offsetof(config_t, memory_fetch_budget_base), sizeof(int), 0,
     CFG_INT},
    {"memory_fetch_budget_shape_aware", offsetof(config_t, memory_fetch_budget_shape_aware),
     sizeof(int), 0, CFG_BOOL},
    {"memory_hard_negative_log", offsetof(config_t, memory_hard_negative_log),
     sizeof(((config_t *)0)->memory_hard_negative_log), 0, CFG_STRING},
    {"dogfood_enabled", offsetof(config_t, dogfood_enabled), sizeof(int), 0, CFG_BOOL},
    {"dogfood_log_dir", offsetof(config_t, dogfood_log_dir),
     sizeof(((config_t *)0)->dogfood_log_dir), 0, CFG_STRING},
    {"dogfood_commit_raw", offsetof(config_t, dogfood_commit_raw), sizeof(int), 0, CFG_BOOL},
    {"dogfood_inline_tagging", offsetof(config_t, dogfood_inline_tagging), sizeof(int), 0,
     CFG_BOOL},
    {"dogfood_autolabel_repair", offsetof(config_t, dogfood_autolabel_repair), sizeof(int), 0,
     CFG_BOOL},
    {"dogfood_autolabel_continuation", offsetof(config_t, dogfood_autolabel_continuation),
     sizeof(int), 0, CFG_BOOL},
    {"dogfood_autolabel_repeat_question", offsetof(config_t, dogfood_autolabel_repeat_question),
     sizeof(int), 0, CFG_BOOL},
    {"learning_router_enabled", offsetof(config_t, learning_router_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"learning_proposal_ttl_days", offsetof(config_t, learning_proposal_ttl_days), sizeof(int), 0,
     CFG_INT},
    {"learning_max_commits_per_week", offsetof(config_t, learning_max_commits_per_week),
     sizeof(int), 0, CFG_INT},
    {"learning_implicit_citation_repair", offsetof(config_t, learning_implicit_citation_repair),
     sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_citation_continuation",
     offsetof(config_t, learning_implicit_citation_continuation), sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_repeat_question", offsetof(config_t, learning_implicit_repeat_question),
     sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_repeated_correction",
     offsetof(config_t, learning_implicit_repeated_correction), sizeof(int), 0, CFG_BOOL},
    {"learning_implicit_workflow_repetition",
     offsetof(config_t, learning_implicit_workflow_repetition), sizeof(int), 0, CFG_BOOL},
    {"identity_working_profile_injection_enabled",
     offsetof(config_t, identity_working_profile_injection_enabled), sizeof(int), 0, CFG_BOOL},
    {"integrity_enabled", offsetof(config_t, integrity_enabled), sizeof(int), 0, CFG_BOOL},
    {"integrity_dry_run", offsetof(config_t, integrity_dry_run), sizeof(int), 0, CFG_BOOL},
    {"virtual_context_enabled", offsetof(config_t, virtual_context_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"virtual_context_assembly_budget", offsetof(config_t, virtual_context_assembly_budget),
     sizeof(int), 0, CFG_INT},
    {"cache_aware_rewrite_enabled", offsetof(config_t, cache_aware_rewrite_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"guardrails_semantic_enabled", offsetof(config_t, guardrails_semantic_enabled), sizeof(int), 0,
     CFG_BOOL},
    {"guardrails_semantic_dry_run", offsetof(config_t, guardrails_semantic_dry_run), sizeof(int), 0,
     CFG_BOOL},
    {"guardrails_semantic_command", offsetof(config_t, guardrails_semantic_command),
     sizeof(((config_t *)0)->guardrails_semantic_command), 0, CFG_STRING},
    {"guardrails_semantic_warn_threshold", offsetof(config_t, guardrails_semantic_warn_threshold),
     sizeof(double), 0, CFG_STRING},
    {"guardrails_semantic_prompt_threshold",
     offsetof(config_t, guardrails_semantic_prompt_threshold), sizeof(double), 0, CFG_STRING},
    {"guardrails_semantic_block_threshold", offsetof(config_t, guardrails_semantic_block_threshold),
     sizeof(double), 0, CFG_STRING},
    {"guardrails_semantic_allow_ml_only_block",
     offsetof(config_t, guardrails_semantic_allow_ml_only_block), sizeof(int), 0, CFG_BOOL},
    {"kb_api_http_port", offsetof(config_t, kb_api_http_port), sizeof(int), 0, CFG_INT},
    {"kb_api_bearer_token", offsetof(config_t, kb_api_bearer_token),
     sizeof(((config_t *)0)->kb_api_bearer_token), 0, CFG_STRING},
    {"kb_mining_enabled", offsetof(config_t, kb_mining_enabled), sizeof(int), 0, CFG_BOOL},
    {"kb_mining_min_poll_s", offsetof(config_t, kb_mining_min_poll_s), sizeof(int), 0, CFG_INT},
    {NULL, 0, 0, 0, CFG_STRING},
};

static const config_field_t *config_field_lookup(const char *key)
{
   for (int i = 0; config_fields[i].key; i++)
   {
      if (strcmp(key, config_fields[i].key) == 0)
         return &config_fields[i];
   }

   return NULL;
}

static void cmd_config_dispositions_print_text(const config_t *cfg)
{
   if (!cfg || cfg->disposition_count == 0)
   {
      printf("No disposition traits configured.\n");
      return;
   }

   for (int i = 0; i < cfg->disposition_count; i++)
      printf(
          "%s\t%.2f\t%s\n", cfg->dispositions[i].name, cfg->dispositions[i].value,
          config_disposition_source_name((config_disposition_source_t)cfg->dispositions[i].source));
}

static void cmd_config_dispositions_print_json(const config_t *cfg)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return;

   if (cfg)
   {
      for (int i = 0; i < cfg->disposition_count; i++)
      {
         cJSON *row = cJSON_CreateObject();
         if (!row)
            continue;
         cJSON_AddStringToObject(row, "name", cfg->dispositions[i].name);
         cJSON_AddNumberToObject(row, "value", cfg->dispositions[i].value);
         cJSON_AddStringToObject(row, "source",
                                 config_disposition_source_name(
                                     (config_disposition_source_t)cfg->dispositions[i].source));
         cJSON_AddItemToArray(arr, row);
      }
   }

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   if (!json)
      return;
   printf("%s\n", json);
   free(json);
}

void cmd_config(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee config <show|get|set|dispositions>\n");
      fprintf(stderr, "  show              Show all config as JSON\n");
      fprintf(stderr, "  get <key>         Get a config value\n");
      fprintf(stderr, "  set <key> <value> Set a config value\n");
      fprintf(stderr, "  dispositions      Show effective disposition traits\n");
      fprintf(stderr, "\nShortcuts:\n");
      fprintf(stderr, "  aimee use <provider>\n");
      fprintf(stderr, "  aimee provider [name]\n");
      fprintf(stderr, "\nKeys: ");
      for (int i = 0; config_fields[i].key; i++)
         fprintf(stderr, "%s%s", i ? ", " : "", config_fields[i].key);
      fprintf(stderr, "\n");
      return;
   }

   const char *sub = argv[0];

   if (strcmp(sub, "show") == 0)
   {
      config_t cfg;
      if (config_load(&cfg) < 0)
      {
         fprintf(stderr, "Failed to load config\n");
         return;
      }
      if (access(config_default_path(), F_OK) != 0)
         config_save(&cfg); /* ensure file exists only when missing */

      /* Read and print the file directly for faithful JSON output */
      FILE *fp = fopen(config_default_path(), "r");
      if (!fp)
      {
         fprintf(stderr, "Cannot read %s\n", config_default_path());
         return;
      }
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
         fwrite(buf, 1, n, stdout);
      fclose(fp);
      (void)ctx;
      return;
   }

   if (strcmp(sub, "dispositions") == 0)
   {
      int json_output = 0;
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--json") == 0)
            json_output = 1;
         else
         {
            fprintf(stderr, "Usage: aimee config dispositions [--json]\n");
            return;
         }
      }

      config_t cfg;
      if (config_load(&cfg) < 0)
      {
         fprintf(stderr, "Failed to load config\n");
         return;
      }

      if (json_output)
         cmd_config_dispositions_print_json(&cfg);
      else
         cmd_config_dispositions_print_text(&cfg);
      (void)ctx;
      return;
   }

   if (strcmp(sub, "get") == 0)
   {
      if (argc < 2)
      {
         fprintf(stderr, "Usage: aimee config get <key>\n");
         return;
      }
      const char *key = argv[1];
      const config_field_t *f = config_field_lookup(key);
      if (!f)
      {
         fprintf(stderr, "Unknown config key: %s\n", key);
         return;
      }

      config_t cfg;
      if (config_load(&cfg) < 0)
      {
         fprintf(stderr, "Failed to load config\n");
         return;
      }

      if (f->is_bool || f->type == CFG_BOOL)
      {
         int val = *(int *)((char *)&cfg + f->offset);
         printf("%s\n", val ? "true" : "false");
      }
      else if (f->type == CFG_INT)
      {
         int val = *(int *)((char *)&cfg + f->offset);
         printf("%d\n", val);
      }
      else if (f->type == CFG_FLOAT)
      {
         double val = *(double *)((char *)&cfg + f->offset);
         printf("%g\n", val);
      }
      else
      {
         const char *val = (const char *)&cfg + f->offset;
         printf("%s\n", val[0] ? val : "(unset)");
      }
      (void)ctx;
      return;
   }

   if (strcmp(sub, "set") == 0)
   {
      if (argc < 3)
      {
         fprintf(stderr, "Usage: aimee config set <key> <value>\n");
         return;
      }
      const char *key = argv[1];
      const char *value = argv[2];
      const config_field_t *f = config_field_lookup(key);
      if (!f)
      {
         fprintf(stderr, "Unknown config key: %s\n", key);
         return;
      }

      config_t cfg;
      if (config_load(&cfg) < 0)
      {
         fprintf(stderr, "Failed to load config\n");
         return;
      }

      if (f->is_bool || f->type == CFG_BOOL)
      {
         int *ptr = (int *)((char *)&cfg + f->offset);
         if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
            *ptr = 1;
         else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
            *ptr = 0;
         else
         {
            fprintf(stderr, "Invalid boolean value: %s (use true/false)\n", value);
            return;
         }
      }
      else if (f->type == CFG_INT)
      {
         int *ptr = (int *)((char *)&cfg + f->offset);
         *ptr = atoi(value);
      }
      else if (f->type == CFG_FLOAT)
      {
         double *ptr = (double *)((char *)&cfg + f->offset);
         *ptr = atof(value);
      }
      else
      {
         char *ptr = (char *)&cfg + f->offset;
         snprintf(ptr, f->size, "%s", value);
      }

      if (config_save(&cfg) < 0)
      {
         fprintf(stderr, "Failed to save config\n");
         return;
      }
      fprintf(stderr, "%s = %s\n", key, value);

      /* Provider is now config-only, no manifest to update */
      (void)ctx;
      return;
   }

   fprintf(stderr, "Unknown subcommand: %s\nUsage: aimee config <show|get|set|dispositions>\n",
           sub);
}

/* --- db subcmds (moved from cmd_core.c) --- */

/* Removed `aimee db status` and `aimee db pragma`; the supported operator
 * surface is `aimee doctor db` plus native DB2 tooling for storage stats. */

static void db_subcmd_backup(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   config_t cfg;
   config_load(&cfg);
   const char *out = (argc > 0) ? argv[0] : NULL;
   if (db1_backup(cfg.db1_path, out) != 0)
   {
      LOG_ERROR("db", "db backup failed");
      exit(1);
   }
}

static void db_subcmd_check(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;
   config_t cfg;
   config_load(&cfg);
   if (db1_check(cfg.db1_path, 1) != 0)
      exit(1);
}

static void db_subcmd_recover(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   int force = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--force") == 0)
         force = 1;
   }
   config_t cfg;
   config_load(&cfg);
   if (db1_recover(cfg.db1_path, force) != 0)
      exit(1);
}

static const subcmd_t db_subcmds[] = {
    {"backup", "Create a manual database backup", db_subcmd_backup},
    {"check", "Run full integrity check", db_subcmd_check},
    {"recover", "Recover from most recent valid backup", db_subcmd_recover},
    {NULL, NULL, NULL},
};

const subcmd_t *get_db_subcmds(void)
{
   return db_subcmds;
}

/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* agent_policy.c: validation, policy, trace, metrics, env, manifest, contract */
#include "aimee.h"
#include "db1.h"
#include "db2/tool_registry.h"
#include "agent.h"
#include "agent_tools.h"
#include "kb_client.h"
#include "headers/memory.h"
#include "headers/agent_exec.h"
#include "compact.h"
#include "coord_closet.h"
#include "computer_use.h"
#include "config.h"
#include "mcp_client_registry.h"
#include "log.h"
#include "dstr.h"
#include "otel.h"
#include "platform_path.h"
#include "cJSON.h"
#include "git_verify.h"
#include "headers/agent_policy_intercept.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tool_prompts_data.h"

/* Look up an embedded per-tool prompt by tool name. Returns NULL if no file
 * exists for this tool in src/tool_prompts/. The table is generated at build
 * time from src/tool_prompts/ and sorted alphabetically. */
static const char *tool_prompts_lookup_embedded(const char *name)
{
   if (!name)
      return NULL;
   int lo = 0, hi = TOOL_PROMPTS_EMBEDDED_COUNT - 1;
   while (lo <= hi)
   {
      int mid = (lo + hi) / 2;
      int cmp = strcmp(TOOL_PROMPTS_EMBEDDED[mid].name, name);
      if (cmp == 0)
         return TOOL_PROMPTS_EMBEDDED[mid].prompt;
      if (cmp < 0)
         lo = mid + 1;
      else
         hi = mid - 1;
   }
   return NULL;
}

/* --- Argument alias table --- */

typedef struct
{
   const char *alias;
   const char *canonical;
} arg_alias_t;

static const arg_alias_t g_arg_aliases[] = {
    {"filepath", "path"},  {"file_path", "path"},  {"file", "path"}, {"filename", "path"},
    {"file_name", "path"}, {"cmd", "command"},     {"dir", "path"},  {"directory", "path"},
    {"q", "query"},        {"max", "max_results"}, {"cnt", "count"}, {"num", "count"},
    {"msg", "message"},    {NULL, NULL},
};

/* Normalize argument names: resolve aliases, coerce "123" to 123 for integer
 * fields. Modifies args in-place. Returns the number of normalizations. */
static int normalize_args(cJSON *args, cJSON *schema_props)
{
   if (!args || !cJSON_IsObject(args))
      return 0;
   int count = 0;

   /* Alias resolution: rename aliased keys to canonical names */
   for (const arg_alias_t *a = g_arg_aliases; a->alias; a++)
   {
      cJSON *field = cJSON_GetObjectItem(args, a->alias);
      if (!field)
         continue;
      /* Only rename if the canonical name is not already present */
      if (cJSON_GetObjectItem(args, a->canonical))
         continue;
      /* cJSON doesn't support key rename, so detach and re-add */
      cJSON *detached = cJSON_DetachItemFromObject(args, a->alias);
      if (detached)
      {
         cJSON_AddItemToObject(args, a->canonical, detached);
         count++;
      }
   }

   /* Type coercion: string "123" → integer 123 for integer fields */
   if (schema_props && cJSON_IsObject(schema_props))
   {
      cJSON *prop = schema_props->child;
      while (prop)
      {
         cJSON *field = cJSON_GetObjectItem(args, prop->string);
         if (field && cJSON_IsString(field))
         {
            cJSON *type_spec = cJSON_GetObjectItem(prop, "type");
            if (type_spec && cJSON_IsString(type_spec) &&
                (strcmp(type_spec->valuestring, "integer") == 0 ||
                 strcmp(type_spec->valuestring, "number") == 0))
            {
               const char *s = field->valuestring;
               char *end = NULL;
               long val = strtol(s, &end, 10);
               if (end && *end == '\0' && s != end)
               {
                  cJSON_ReplaceItemInObject(args, prop->string, cJSON_CreateNumber(val));
                  count++;
               }
            }
         }
         prop = prop->next;
      }
   }
   return count;
}

static int validate_against_schema(const char *args_json, cJSON *schema, char *err_out,
                                   size_t err_len)
{
   if (!schema)
      return 0;

   cJSON *args = cJSON_Parse(args_json);
   if (!args)
   {
      snprintf(err_out, err_len, "invalid JSON arguments");
      return -1;
   }

   cJSON *props = cJSON_GetObjectItem(schema, "properties");
   normalize_args(args, props);

   cJSON *required = cJSON_GetObjectItem(schema, "required");
   if (required && cJSON_IsArray(required))
   {
      int n = cJSON_GetArraySize(required);
      for (int i = 0; i < n; i++)
      {
         cJSON *req = cJSON_GetArrayItem(required, i);
         if (req && cJSON_IsString(req))
         {
            cJSON *field = cJSON_GetObjectItem(args, req->valuestring);
            if (!field)
            {
               snprintf(err_out, err_len, "missing required field '%s'", req->valuestring);
               cJSON_Delete(args);
               return -1;
            }
         }
      }
   }

   if (props && cJSON_IsObject(props))
   {
      cJSON *prop = props->child;
      while (prop)
      {
         cJSON *field = cJSON_GetObjectItem(args, prop->string);
         if (field)
         {
            cJSON *type_spec = cJSON_GetObjectItem(prop, "type");
            if (type_spec && cJSON_IsString(type_spec))
            {
               const char *expected = type_spec->valuestring;
               int ok = 1;
               if (strcmp(expected, "string") == 0 && !cJSON_IsString(field))
                  ok = 0;
               else if (strcmp(expected, "integer") == 0 && !cJSON_IsNumber(field))
                  ok = 0;
               else if (strcmp(expected, "number") == 0 && !cJSON_IsNumber(field))
                  ok = 0;
               else if (strcmp(expected, "boolean") == 0 && !cJSON_IsBool(field))
                  ok = 0;
               if (!ok)
               {
                  snprintf(err_out, err_len, "field '%s' should be %s", prop->string, expected);
                  cJSON_Delete(args);
                  return -1;
               }
            }
         }
         prop = prop->next;
      }
   }

   cJSON_Delete(args);
   return 0;
}

/* --- Nearest-match tool suggestion --- */

/* Known delegate tool names for fuzzy matching */
static const char *g_tool_names[] = {"bash",          "read_file",   "write_file", "edit_file",
                                     "list_files",    "verify",      "git_log",    "grep",
                                     "git_diff",      "git_status",  "env_get",    "test",
                                     "request_input", "code_search", NULL};

/* Simple edit-distance (Levenshtein) for short strings. Max len 64. */
static int edit_distance(const char *a, const char *b)
{
   int la = (int)strlen(a), lb = (int)strlen(b);
   if (la > 64 || lb > 64)
      return 99;
   int dp[65][65];
   for (int i = 0; i <= la; i++)
      dp[i][0] = i;
   for (int j = 0; j <= lb; j++)
      dp[0][j] = j;
   for (int i = 1; i <= la; i++)
      for (int j = 1; j <= lb; j++)
      {
         int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
         int del = dp[i - 1][j] + 1;
         int ins = dp[i][j - 1] + 1;
         int sub = dp[i - 1][j - 1] + cost;
         dp[i][j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
      }
   return dp[la][lb];
}

/* Find the closest matching tool name. Returns NULL if no close match. */
const char *tool_suggest(const char *name)
{
   if (!name || !name[0])
      return NULL;
   const char *best = NULL;
   int best_dist = 4; /* threshold: max 3 edits */
   for (const char **t = g_tool_names; *t; t++)
   {
      int d = edit_distance(name, *t);
      if (d < best_dist)
      {
         best_dist = d;
         best = *t;
      }
   }
   return best;
}

/* --- Tool registry validation --- */

int tool_validate(const char *tool_name, const char *args_json, char *err_out, size_t err_len)
{
   if (!tool_name || !args_json)
      return 0;

   if (strchr(tool_name, ':') != NULL)
   {
      cJSON *remote_tool = NULL;
      if (mcp_client_registry_get_tool_schema(tool_name, 1000, &remote_tool, err_out, err_len) == 0)
      {
         cJSON *schema = cJSON_GetObjectItemCaseSensitive(remote_tool, "inputSchema");
         int rc = validate_against_schema(args_json, schema, err_out, err_len);
         cJSON_Delete(remote_tool);
         return rc;
      }
      return -1;
   }

   tool_registry_entry_t entry;
   memset(&entry, 0, sizeof(entry));
   int found = 0;
   if (kb_client_tool_registry_lookup(tool_name, entry.input_schema, sizeof(entry.input_schema),
                                      entry.side_effect, sizeof(entry.side_effect), &entry.enabled,
                                      &found) != 0 ||
       !found)
   {
      cJSON *builtin_schema = agent_tool_get_schema_cached(tool_name);
      if (builtin_schema)
         return validate_against_schema(args_json, builtin_schema, err_out, err_len);

      const char *suggestion = tool_suggest(tool_name);
      if (suggestion)
         snprintf(err_out, err_len, "unknown tool '%s'. Did you mean '%s'?", tool_name, suggestion);
      else
         snprintf(err_out, err_len, "unknown tool '%s'", tool_name);
      return -1;
   }

   if (!entry.enabled)
   {
      snprintf(err_out, err_len, "tool '%s' is disabled", tool_name);
      return -1;
   }

   if (!entry.input_schema[0])
      return 0;

   cJSON *schema = cJSON_Parse(entry.input_schema);
   if (!schema)
      return 0;
   int rc = validate_against_schema(args_json, schema, err_out, err_len);
   cJSON_Delete(schema);
   return rc;
}

const char *tool_side_effect(const char *tool_name)
{
   /* Fall through aimee-kb so the lookup works in the daemon (DB2-less)
    * process.  Thread-local buffer mirrors db2_tool_registry_side_effect's
    * legacy contract — caller can treat the pointer as valid until the
    * next tool_side_effect() call on this thread. */
   static __thread char buf[32];
   buf[0] = '\0';
   int found = 0;
   if (kb_client_tool_registry_lookup(tool_name ? tool_name : "", NULL, 0, buf, sizeof(buf), NULL,
                                      &found) != 0 ||
       !found || !buf[0])
      snprintf(buf, sizeof(buf), "%s", "read");
   return buf;
}

/* --- Tool prompt collection --- */

/* Collect usage prompts from all enabled tools that have a non-empty tool_prompt.
 * Returns a heap-allocated string of the form:
 *
 *   ## Tool Usage Notes
 *
 *   ### bash
 *   <prompt>
 *
 *   ### read_file
 *   <prompt>
 *
 * Returns NULL if no tools have prompts or the DB is unavailable. Caller must free(). */
struct tool_prompts_ctx
{
   dstr_t out;
   int count;
};

static int collect_tool_prompts_cb(const char *name, const char *prompt, void *user)
{
   struct tool_prompts_ctx *ctx = (struct tool_prompts_ctx *)user;
   if (!prompt || !*prompt)
      prompt = tool_prompts_lookup_embedded(name);
   if (!prompt || !*prompt)
      return 0;

   if (ctx->count == 0)
      dstr_append_str(&ctx->out, "## Tool Usage Notes\n");

   dstr_appendf(&ctx->out, "\n### %s\n%s\n", name, prompt);
   ctx->count++;
   return 0;
}

char *agent_collect_tool_prompts(void)
{
   struct tool_prompts_ctx ctx = {0};
   dstr_init(&ctx.out);

   /* Pull all enabled tool prompts via aimee-kb (DB2 owner). */
   char *envelope = kb_client_tool_registry_snapshot_json();
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   cJSON *prompts = resp ? cJSON_GetObjectItemCaseSensitive(resp, "prompts") : NULL;
   if (cJSON_IsArray(prompts))
   {
      cJSON *p;
      cJSON_ArrayForEach(p, prompts)
      {
         cJSON *name_j = cJSON_GetObjectItemCaseSensitive(p, "name");
         cJSON *prompt_j = cJSON_GetObjectItemCaseSensitive(p, "prompt");
         const char *name = cJSON_IsString(name_j) ? name_j->valuestring : "";
         const char *prompt = cJSON_IsString(prompt_j) ? prompt_j->valuestring : "";
         collect_tool_prompts_cb(name, prompt, &ctx);
      }
   }
   cJSON_Delete(resp);

   if (ctx.count == 0)
   {
      dstr_free(&ctx.out);
      return NULL;
   }
   return dstr_steal(&ctx.out);
}

/* --- Policy checking --- */

static char *g_policy_json = NULL;

int policy_load(void)
{
   const char *paths[] = {".aimee-policy.json", NULL};
   char global_path[MAX_PATH_LEN];
   snprintf(global_path, sizeof(global_path), "%s/policy.json", config_default_dir());
   paths[1] = global_path;

   for (int i = 0; i < 2; i++)
   {
      FILE *f = fopen(paths[i], "r");
      if (!f)
         continue;
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (sz > 0 && sz < 1024 * 1024)
      {
         free(g_policy_json);
         g_policy_json = malloc((size_t)sz + 1);
         if (!g_policy_json)
         {
            fclose(f);
            return -1;
         }
         size_t nread = fread(g_policy_json, 1, (size_t)sz, f);
         if (ferror(f) || (long)nread != sz)
         {
            free(g_policy_json);
            g_policy_json = NULL;
            fclose(f);
            return -1;
         }
         g_policy_json[nread] = '\0';
      }
      fclose(f);
      return 0;
   }
   return -1;
}

int policy_check_tool(const char *tool_name, const char *side_effect, const char *args_json,
                      char *reason_out, size_t reason_len)
{
   if (computer_use_is_tool_name(tool_name))
   {
      config_t cfg;
      config_load(&cfg);
      computer_use_policy_t cu_policy;
      computer_use_policy_from_config(&cfg, &cu_policy);
      computer_use_decision_t decision;
      char cu_reason[256] = "";
      if (computer_use_classify(&cu_policy, tool_name, args_json, &decision, cu_reason,
                                sizeof(cu_reason)) &&
          decision != COMPUTER_USE_DECISION_ALLOW)
      {
         if (reason_out && reason_len > 0)
            snprintf(reason_out, reason_len, "%s",
                     cu_reason[0] ? cu_reason : "computer-use action requires approval");
         return -1;
      }
   }

   /* Hardcoded discovery-shell intercept — fires unconditionally, no policy file required. */
   if (strcmp(tool_name, "bash") == 0 && args_json)
   {
      cJSON *args = cJSON_Parse(args_json);
      cJSON *cmd = args ? cJSON_GetObjectItem(args, "command") : NULL;
      if (cmd && cJSON_IsString(cmd) && policy_is_source_discovery(cmd->valuestring))
      {
         snprintf(reason_out, reason_len,
                  "Use `aimee index find <symbol>` or `aimee index overview` for code "
                  "discovery. Fall back to shell search only if aimee returns nothing.");
         cJSON_Delete(args);
         return -1;
      }
      cJSON_Delete(args);
   }

   if (!g_policy_json)
   {
      policy_load();
      if (!g_policy_json)
         return 0;
   }

   cJSON *policy = cJSON_Parse(g_policy_json);
   if (!policy)
      return 0;

   if (strcmp(tool_name, "bash") == 0 && args_json)
   {
      cJSON *args = cJSON_Parse(args_json);
      cJSON *cmd = args ? cJSON_GetObjectItem(args, "command") : NULL;

      cJSON *forbidden = cJSON_GetObjectItem(policy, "forbidden_commands");
      if (forbidden && cJSON_IsArray(forbidden) && cmd && cJSON_IsString(cmd))
      {
         int n = cJSON_GetArraySize(forbidden);
         for (int i = 0; i < n; i++)
         {
            cJSON *pat = cJSON_GetArrayItem(forbidden, i);
            if (pat && cJSON_IsString(pat) && strstr(cmd->valuestring, pat->valuestring))
            {
               snprintf(reason_out, reason_len, "command matches forbidden pattern: %s",
                        pat->valuestring);
               cJSON_Delete(args);
               cJSON_Delete(policy);
               return -1;
            }
         }
      }

      cJSON_Delete(args);
   }

   /* Check tool_rules path-prefix restrictions (Feature 4) */
   cJSON *tool_rules = cJSON_GetObjectItem(policy, "tool_rules");
   if (tool_rules && cJSON_IsArray(tool_rules) && args_json)
   {
      /* Extract path from tool args */
      cJSON *args = cJSON_Parse(args_json);
      const char *target_path = NULL;
      if (args)
      {
         cJSON *p = cJSON_GetObjectItem(args, "path");
         if (p && cJSON_IsString(p))
            target_path = p->valuestring;
         else
         {
            cJSON *cmd = cJSON_GetObjectItem(args, "command");
            if (cmd && cJSON_IsString(cmd))
               target_path = cmd->valuestring;
         }
      }

      if (target_path)
      {
         int n = cJSON_GetArraySize(tool_rules);
         for (int i = 0; i < n; i++)
         {
            cJSON *rule = cJSON_GetArrayItem(tool_rules, i);
            if (!rule)
               continue;
            cJSON *prefix = cJSON_GetObjectItem(rule, "path_prefix");
            if (!prefix || !cJSON_IsString(prefix))
               continue;

            size_t plen = strlen(prefix->valuestring);
            if (strncmp(target_path, prefix->valuestring, plen) == 0 &&
                (target_path[plen] == '/' || target_path[plen] == '\0'))
            {
               /* Path matches this rule; check if tool is allowed */
               cJSON *allowed = cJSON_GetObjectItem(rule, "allowed_tools");
               if (allowed && cJSON_IsArray(allowed))
               {
                  int found = 0;
                  int an = cJSON_GetArraySize(allowed);
                  for (int j = 0; j < an; j++)
                  {
                     cJSON *at = cJSON_GetArrayItem(allowed, j);
                     if (at && cJSON_IsString(at) && strcmp(at->valuestring, tool_name) == 0)
                     {
                        found = 1;
                        break;
                     }
                  }
                  if (!found)
                  {
                     snprintf(reason_out, reason_len, "tool '%s' not allowed for path %s",
                              tool_name, prefix->valuestring);
                     cJSON_Delete(args);
                     cJSON_Delete(policy);
                     return -1;
                  }
               }
               break; /* First matching prefix wins */
            }
         }
      }
      cJSON_Delete(args);
   }

   cJSON *levels = cJSON_GetObjectItem(policy, "approval_levels");
   if (levels && side_effect)
   {
      cJSON *level = cJSON_GetObjectItem(levels, side_effect);
      if (level && cJSON_IsString(level) && strcmp(level->valuestring, "block") == 0)
      {
         snprintf(reason_out, reason_len, "policy blocks %s operations", side_effect);
         cJSON_Delete(policy);
         return -1;
      }
   }

   cJSON_Delete(policy);
   return 0;
}

/* --- Execution trace --- */

void agent_trace_log(int plan_id, int turn, const char *direction, const char *content,
                     const char *tool_name, const char *tool_args, const char *tool_result,
                     const char *context_hash)
{
   /* Lazy OTel init: read config on first call so we don't need a separate
    * startup hook.  The double-checked pattern is safe because otel_init is
    * idempotent and the worst case is two initializations on the very first call. */
   {
      static int otel_initialized = 0;
      if (!otel_initialized)
      {
         otel_initialized = 1;
         config_t ocfg;
         config_load(&ocfg);
         if (ocfg.otel_endpoint[0])
            otel_init(ocfg.otel_endpoint,
                      ocfg.otel_service_name[0] ? ocfg.otel_service_name : "aimee", session_id());
      }
   }

   db1_execution_trace_insert_row_t row = {
       .plan_id = plan_id,
       .turn = turn,
       .direction = direction,
       .content = content,
       .tool_name = tool_name,
       .tool_args = tool_args,
       .tool_result = tool_result,
       .context_hash = context_hash,
   };
   (void)db1_execution_trace_insert(&row);

   /* OTel: emit spans alongside the DB trace */
   otel_on_trace(direction, tool_name, tool_args, tool_result, turn);
}

/* --- Confidence estimation --- */

int agent_estimate_confidence(const char *response_text)
{
   if (!response_text || !response_text[0])
      return 50;

   int confidence = 80;

   static const char *low_markers[] = {"I'm not sure", "not certain",   "unclear",      "I think",
                                       "might",        "possibly",      "I don't know", "uncertain",
                                       "may not",      "I cannot tell", "hard to say",  NULL};

   for (int i = 0; low_markers[i]; i++)
   {
      if (strcasestr(response_text, low_markers[i]))
      {
         confidence -= 15;
         break;
      }
   }

   static const char *high_markers[] = {"successfully", "completed",         "confirmed",
                                        "verified",     "all checks passed", NULL};

   for (int i = 0; high_markers[i]; i++)
   {
      if (strcasestr(response_text, high_markers[i]))
      {
         confidence += 10;
         break;
      }
   }

   if (confidence < 0)
      confidence = 0;
   if (confidence > 100)
      confidence = 100;

   return confidence;
}

/* --- Prometheus metrics --- */

static void prom_escape(const char *in, char *out, size_t out_len)
{
   size_t j = 0;
   for (size_t i = 0; in[i] && j < out_len - 1; i++)
   {
      if (in[i] == '\\' || in[i] == '"')
      {
         if (j + 2 >= out_len)
            break;
         out[j++] = '\\';
         out[j++] = in[i];
      }
      else if (in[i] == '\n')
      {
         if (j + 2 >= out_len)
            break;
         out[j++] = '\\';
         out[j++] = 'n';
      }
      else
      {
         out[j++] = in[i];
      }
   }
   out[j] = '\0';
}

void agent_write_metrics(void)
{
   const char *metrics_path = "/var/lib/prometheus/node-exporter/aimee.prom";
   FILE *f = fopen(metrics_path, "w");
   if (!f)
   {
      f = fopen("/tmp/aimee-metrics.prom", "w");
      if (!f)
         return;
      chmod("/tmp/aimee-metrics.prom", 0600);
   }

   fprintf(f, "# HELP aimee_delegations_total Total delegations by role and agent\n");
   fprintf(f, "# TYPE aimee_delegations_total counter\n");
   fprintf(f, "# HELP aimee_delegation_successes_total Successful delegations\n");
   fprintf(f, "# TYPE aimee_delegation_successes_total counter\n");
   fprintf(f, "# HELP aimee_delegation_latency_avg_ms Average delegation latency\n");
   fprintf(f, "# TYPE aimee_delegation_latency_avg_ms gauge\n");
   fprintf(f, "# HELP aimee_tokens_total Total tokens used\n");
   fprintf(f, "# TYPE aimee_tokens_total counter\n");
   fprintf(f, "# HELP aimee_tool_calls_total Total tool calls\n");
   fprintf(f, "# TYPE aimee_tool_calls_total counter\n");

   /* DB1 agent_log aggregates by (agent, role). */
   db1_agent_log_prometheus_t rows[256];
   int nrows = db1_agent_log_prometheus(rows, 256);
   for (int i = 0; i < nrows; i++)
   {
      const char *agent = rows[i].agent_name[0] ? rows[i].agent_name : "unknown";
      const char *role = rows[i].role[0] ? rows[i].role : "unknown";
      char safe_agent[128], safe_role[128];
      prom_escape(agent, safe_agent, sizeof(safe_agent));
      prom_escape(role, safe_role, sizeof(safe_role));
      fprintf(f, "aimee_delegations_total{agent=\"%s\",role=\"%s\"} %d\n", safe_agent, safe_role,
              rows[i].total);
      fprintf(f, "aimee_delegation_successes_total{agent=\"%s\",role=\"%s\"} %d\n", safe_agent,
              safe_role, rows[i].successes);
      fprintf(f, "aimee_delegation_latency_avg_ms{agent=\"%s\",role=\"%s\"} %d\n", safe_agent,
              safe_role, rows[i].avg_latency_ms);
      fprintf(f, "aimee_tokens_total{agent=\"%s\",role=\"%s\",type=\"prompt\"} %d\n", safe_agent,
              safe_role, rows[i].prompt_tokens);
      fprintf(f, "aimee_tokens_total{agent=\"%s\",role=\"%s\",type=\"completion\"} %d\n",
              safe_agent, safe_role, rows[i].completion_tokens);
      fprintf(f, "aimee_tool_calls_total{agent=\"%s\",role=\"%s\"} %d\n", safe_agent, safe_role,
              rows[i].tool_calls);
   }

   /* DB2 metrics (incl. pgvector) are owned by aimee-kb, not aimee-server. */

   fclose(f);
}

/* --- Environment introspection --- */

static void env_set(const char *key, const char *value)
{
   (void)db1_env_capability_set(key, value);
}
static int check_command(const char *cmd)
{
   const char *argv[] = {"which", cmd, NULL};
   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, 256);
   free(output);
   return rc == 0;
}

void agent_introspect_env(void)
{
   /* Toolchains */
   static const char *tools[] = {"gcc",  "clang", "make",  "dotnet", "node", "npm",    "python3",
                                 "pip3", "go",    "rustc", "java",   "mvn",  "gradle", "docker",
                                 "git",  "ssh",   "curl",  "jq",     NULL};

   for (int i = 0; tools[i]; i++)
   {
      env_set(tools[i], check_command(tools[i]) ? "available" : "missing");
   }

   /* Package managers */
   static const char *pkg_mgrs[] = {"apt-get", "brew", "dnf", "yum", "pacman", "apk", NULL};
   for (int i = 0; pkg_mgrs[i]; i++)
   {
      if (check_command(pkg_mgrs[i]))
      {
         env_set("package_manager", pkg_mgrs[i]);
         break;
      }
   }

   /* OS info — use safe argv execution instead of popen shell pipeline */
   {
      const char *argv[] = {"uname", "-s", "-r", "-m", NULL};
      char *out = NULL;
      if (safe_exec_capture(argv, &out, 256) == 0 && out)
      {
         size_t len = strlen(out);
         while (len > 0 && out[len - 1] == '\n')
            out[--len] = '\0';
         if (out[0])
            env_set("os", out);
      }
      free(out);
   }

   /* Disk space — use safe argv execution */
   {
      const char *argv[] = {"df", "-h", "/", NULL};
      char *out = NULL;
      if (safe_exec_capture(argv, &out, 1024) == 0 && out)
      {
         /* Parse last line, 4th field (Available) */
         char *last_line = out;
         char *nl;
         while ((nl = strchr(last_line, '\n')) != NULL && nl[1])
            last_line = nl + 1;
         /* Skip whitespace-separated fields to get 4th */
         int field = 0;
         char *p = last_line;
         while (*p && field < 3)
         {
            while (*p && *p != ' ' && *p != '\t')
               p++;
            while (*p == ' ' || *p == '\t')
               p++;
            field++;
         }
         if (*p)
         {
            char avail[64] = {0};
            size_t ai = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && ai < sizeof(avail) - 1)
               avail[ai++] = *p++;
            avail[ai] = '\0';
            if (avail[0])
               env_set("disk_free", avail);
         }
      }
      free(out);
   }

   /* Memory — read from /proc/meminfo if available, fall back to safe exec */
   {
      FILE *f = fopen("/proc/meminfo", "r");
      if (f)
      {
         char line[128];
         while (fgets(line, sizeof(line), f))
         {
            if (strncmp(line, "MemTotal:", 9) == 0)
            {
               /* Parse "MemTotal:    XXXX kB" */
               char *p = line + 9;
               while (*p == ' ')
                  p++;
               long kb = strtol(p, NULL, 10);
               char mem_str[32];
               if (kb > 1048576)
                  snprintf(mem_str, sizeof(mem_str), "%.1fGi", (double)kb / 1048576.0);
               else
                  snprintf(mem_str, sizeof(mem_str), "%ldMi", kb / 1024);
               env_set("total_memory", mem_str);
               break;
            }
         }
         fclose(f);
      }
      else
      {
         const char *argv[] = {"free", "-h", NULL};
         char *out = NULL;
         if (safe_exec_capture(argv, &out, 512) == 0 && out)
         {
            char *mem_line = strstr(out, "Mem:");
            if (mem_line)
            {
               char *p = mem_line + 4;
               while (*p == ' ' || *p == '\t')
                  p++;
               char total[32] = {0};
               size_t ti = 0;
               while (*p && *p != ' ' && *p != '\t' && *p != '\n' && ti < sizeof(total) - 1)
                  total[ti++] = *p++;
               total[ti] = '\0';
               if (total[0])
                  env_set("total_memory", total);
            }
         }
         free(out);
      }
   }
}

/* --- Change manifests --- */

void agent_write_manifest(const char *run_id, const agent_result_t *result, const char *role)
{
   if (!run_id || !result)
      return;

   char manifest_dir[MAX_PATH_LEN];
   snprintf(manifest_dir, sizeof(manifest_dir), "%s/manifests", config_default_dir());
   platform_mkdir_p(manifest_dir, 0700);

   /* Build manifest from execution trace */
   cJSON *manifest = cJSON_CreateObject();
   cJSON_AddStringToObject(manifest, "run_id", run_id);
   cJSON_AddStringToObject(manifest, "agent", result->agent_name);
   cJSON_AddStringToObject(manifest, "role", role ? role : "");

   char now[32];
   now_utc(now, sizeof(now));
   cJSON_AddStringToObject(manifest, "timestamp", now);

   cJSON_AddNumberToObject(manifest, "confidence", result->confidence);
   cJSON_AddNumberToObject(manifest, "turns", result->turns);
   cJSON_AddNumberToObject(manifest, "tool_calls", result->tool_calls);
   cJSON_AddBoolToObject(manifest, "success", result->success);

   /* Collect commands from trace */
   cJSON *commands = cJSON_CreateArray();
   db1_execution_trace_tool_call_t tool_calls[50];
   int tool_count = db1_execution_trace_list_tool_calls(tool_calls, 50);
   if (tool_count > 0)
   {
      for (int i = 0; i < tool_count; i++)
      {
         if (!tool_calls[i].tool_name[0])
            continue;
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "tool", tool_calls[i].tool_name);
         cJSON_AddStringToObject(entry, "args", tool_calls[i].tool_args);
         cJSON_AddItemToArray(commands, entry);
      }
   }
   cJSON_AddItemToObject(manifest, "tool_calls_detail", commands);

   char *json = cJSON_Print(manifest);
   cJSON_Delete(manifest);
   if (!json)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s.json", manifest_dir, run_id);
   FILE *fp = fopen(path, "w");
   if (fp)
   {
      fputs(json, fp);
      fputc('\n', fp);
      fclose(fp);
   }
   free(json);
}

/* --- Repo contract (~/.config/aimee/projects/<name>/project.yaml) --- */

char *agent_load_project_contract(const char *project_root)
{
   /* Reads from the global per-project config:
    *   ~/.config/aimee/projects/<name>/project.yaml
    * Worktrees and main checkout share one file. Fields are extracted by
    * the same hand-rolled top-level scanner used for the earlier in-repo
    * format, so any of name/language/build/test/lint/definition_of_done/
    * risky_paths that are present in the file get pulled into the contract;
    * missing fields are silently omitted. */
   char path[MAX_PATH_LEN];
   if (project_yaml_path(project_root, path, sizeof(path)) != 0)
      return NULL;

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   /* Parse simple YAML key: value pairs */
   char name[128] = {0}, lang[64] = {0};
   char build_cmd[512] = {0}, test_cmd[512] = {0}, lint_cmd[512] = {0};
   char dod[1024] = {0};
   char risky[1024] = {0};
   int in_dod = 0, in_risky = 0;

   char line[512];
   while (fgets(line, sizeof(line), f))
   {
      /* Strip trailing newline */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      /* Check for list items under definition_of_done or risky_paths */
      if (in_dod && line[0] == ' ' && strstr(line, "- "))
      {
         char *item = strstr(line, "- ") + 2;
         size_t dlen = strlen(dod);
         snprintf(dod + dlen, sizeof(dod) - dlen, "  - %s\n", item);
         continue;
      }
      else if (in_risky && line[0] == ' ' && strstr(line, "- "))
      {
         char *item = strstr(line, "- ") + 2;
         size_t rlen = strlen(risky);
         snprintf(risky + rlen, sizeof(risky) - rlen, "  - %s\n", item);
         continue;
      }
      in_dod = 0;
      in_risky = 0;

      /* Parse key: value */
      if (strncmp(line, "name:", 5) == 0)
         snprintf(name, sizeof(name), "%s", line + 6);
      else if (strncmp(line, "language:", 9) == 0)
         snprintf(lang, sizeof(lang), "%s", line + 10);
      else if (strncmp(line, "build:", 6) == 0)
         snprintf(build_cmd, sizeof(build_cmd), "%s", line + 7);
      else if (strncmp(line, "test:", 5) == 0)
         snprintf(test_cmd, sizeof(test_cmd), "%s", line + 6);
      else if (strncmp(line, "lint:", 5) == 0)
         snprintf(lint_cmd, sizeof(lint_cmd), "%s", line + 6);
      else if (strncmp(line, "definition_of_done:", 19) == 0)
         in_dod = 1;
      else if (strncmp(line, "risky_paths:", 12) == 0)
         in_risky = 1;
   }
   fclose(f);

   /* Build structured context */
   size_t out_len = 2048;
   char *out = malloc(out_len);
   if (!out)
      return NULL;

   size_t pos = 0;
   pos += (size_t)snprintf(out + pos, out_len - pos, "# Project Contract\n");
   if (name[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Name: %s\n", name);
   if (lang[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Language: %s\n", lang);
   if (build_cmd[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Build: %s\n", build_cmd);
   if (test_cmd[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Test: %s\n", test_cmd);
   if (lint_cmd[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Lint: %s\n", lint_cmd);
   if (dod[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Definition of Done:\n%s", dod);
   if (risky[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Risky Paths (require review):\n%s", risky);
   return out;
}

/* --- Tool result compression (#4) --- */

/* Build a compact_config_t from the application-level config_t.
 * Per-tool entries are stored as "tool_name=threshold" strings in config. */
static void compact_cfg_from_app_config(const config_t *app, compact_config_t *out)
{
   memset(out, 0, sizeof(*out));
   out->enabled = app->compact_enabled;
   out->threshold = app->compact_threshold;
   out->head_bytes = app->compact_head_bytes;
   out->tail_bytes = app->compact_tail_bytes;

   int count = app->compact_per_tool_count;
   if (count > COMPACT_MAX_PER_TOOL)
      count = COMPACT_MAX_PER_TOOL;

   for (int i = 0; i < count; i++)
   {
      char tool[64];
      int thresh = 0;
      if (sscanf(app->compact_per_tool[i], "%63[^=]=%d", tool, &thresh) == 2)
      {
         snprintf(out->per_tool[i].tool, sizeof(out->per_tool[i].tool), "%s", tool);
         out->per_tool[i].threshold = thresh;
         out->per_tool_count++;
      }
   }
}

/* Eager tool-result seam: shrink via the shared compact_body() core using
 * application config, conserve identifiers in the Coordinate Closet, and bound
 * the result to the per-result cap. This is the ONLY writer of compacted bodies
 * into history; the economizer's lazy lever (context_compress_view) shares the
 * same compact_body() core but operates on deep copies at request assembly.
 * Falls back to the resolved per-result cap (agent_tool_output_cap_clamp;
 * default AGENT_TOOL_OUTPUT_MAX) so oversized results are always bounded even
 * if compaction produces a larger-than-expected summary (e.g. malformed JSON
 * that fails parsing).
 *
 * tool_name may be NULL to skip per-tool overrides. */
char *agent_compress_tool_result(const char *raw, size_t raw_len, const char *tool_name)
{
   if (!raw)
      return strdup("");

   /* Load application config and convert to compact_config_t */
   config_t app_cfg;
   compact_config_t cfg;
   int loaded = (config_load(&app_cfg) == 0);
   if (loaded)
      compact_cfg_from_app_config(&app_cfg, &cfg);
   else
      memset(&cfg, 0, sizeof(cfg)); /* use defaults on load failure */

   /* Per-result model-visible cap (operator-configurable, default 32768).
    * Resolved ONCE here so the closet budget and the hard cap below agree. */
   size_t cap = agent_tool_output_cap_clamp(loaded ? app_cfg.tool_output_max_bytes : 0);

   /* Shrink via the single shared core. Size the buffer for the one strategy that
    * can exceed raw_len (a JSON structural summary of a tiny body); every other
    * path is <= raw_len, so this never truncates the shrink. The hard cap below is
    * applied by THIS caller (per-seam policy), not the core. */
   size_t out_cap = raw_len + COMPACT_JSON_SUMMARY_MAX + 1;
   char *out = malloc(out_cap);
   if (!out)
      return strdup("");
   size_t out_len = compact_body(raw, raw_len, tool_name, &cfg, out, out_cap);
   int compacted = (out_len < raw_len);

   /* Log compaction events for diagnostics */
   if (compacted)
      aimee_log(LOG_DEBUG, "compact", "tool=%s in=%llu out=%llu", tool_name ? tool_name : "(none)",
                (unsigned long long)raw_len, (unsigned long long)out_len);

   /* Fold §2 — Coordinate Closet: when the result was compacted, conserve the
    * verbatim identifiers from the pre-truncation raw so they survive. Render the
    * closet first (it is byte-bounded), then reserve room for it under the hard
    * cap so the combined result still never exceeds the resolved per-result cap.
    * Default-off; return-only wiring (no cross-turn persistence in P1). */
   char *closet = NULL;
   size_t closet_len = 0;
   if (loaded && app_cfg.coord_closet_enabled && compacted)
   {
      coord_set_t set;
      coord_set_init(&set);
      /* P1 return-only wiring: the closet conserves identifiers from THIS tool
       * result (the compacted body may drop them, which is exactly what the closet
       * guards against). The conserved values originate from the same tool result
       * as the body — same trust level — so stamping them lane AGENT elevates no
       * trust. Per-source lane assignment (user-pasted vs tool-origin) arrives with
       * the transcript fold in P2, where coord_provenance_t already carries it. */
      coord_provenance_t prov = {COORD_LANE_AGENT, -1, -1, -1};
      coord_closet_nominate(raw, raw_len, &prov, &set);
      /* Bound the closet budget so body + closet can never exceed the hard cap:
       * closet <= cap - min-body, leaving room for the body. */
      int hard_room = (int)cap - 256;
      if (hard_room < 0)
         hard_room = 0; /* tiny operator cap: no room for a closet */
      int cb = app_cfg.coord_closet_budget_bytes;
      if (cb > hard_room)
         cb = hard_room;
      coord_closet_config_t ccfg = {
          .enabled = 1,
          .budget_bytes = cb,
          .max_ratio_pct = app_cfg.coord_closet_max_ratio_pct,
          .denylist = app_cfg.coord_closet_denylist[0] ? app_cfg.coord_closet_denylist : NULL,
      };
      coord_evict_t why = COORD_EVICT_NONE;
      closet = coord_closet_render(&set, &ccfg, raw_len, &why);
      if (closet)
         closet_len = strlen(closet);
      /* Eviction is surfaced in the rendered text ("(partial — ... omitted)") and
       * logged at WARNING so the loss is never silent. */
      if (why == COORD_EVICT_FAIL)
         aimee_log(LOG_WARN, "compact", "coord_closet partial (budget) tool=%s",
                   tool_name ? tool_name : "(none)");
      coord_set_free(&set);
   }

   /* Hard cap: ensure the result never exceeds the resolved per-result cap
    * regardless of what the compaction summary produced. Reserve closet bytes
    * inside the cap. */
   size_t body_cap = cap;
   if (closet_len > 0 && closet_len + 1 < cap)
      body_cap = cap - closet_len - 1;
   if (out_len > body_cap)
   {
      out[body_cap] = '\0';
      out_len = body_cap;
   }

   if (closet_len > 0)
   {
      char *combined = malloc(out_len + 1 + closet_len + 1);
      if (combined)
      {
         memcpy(combined, out, out_len);
         combined[out_len] = '\n';
         memcpy(combined + out_len + 1, closet, closet_len);
         combined[out_len + 1 + closet_len] = '\0';
         free(out);
         out = combined;
      }
   }
   free(closet);
   return out;
}

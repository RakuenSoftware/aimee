/* Higher-level native callers composed from the pure-Go config bus contract. */
#include "config.h"
#include "aimee.h"
#include "config_client.h"
#include "aimee_home.h"
#include "platform_path.h"
#include "runtime_secret.h"
#include "config_embedder_dims.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static config_reapplier_fn g_reappliers[16];
static int g_reapplier_count;
static config_secret_writer_fn g_secret_writer;
__thread int g_aimee_compute_threads_override;

int config_snapshot_seed(void)
{
   return config_client_refresh();
}

int config_reload(void)
{
   if (config_client_refresh() != 0)
      return -1;
   for (int i = 0; i < g_reapplier_count; i++)
      g_reappliers[i]();
   return 1;
}

int config_reload_if_changed(void)
{
   int changed = config_client_changed();
   if (changed <= 0)
      return changed;
   return config_reload();
}

int config_present(void)
{
   return config_client_refresh() == 0;
}

int config_wait_ready(unsigned int timeout_ms)
{
   const unsigned int interval_ms = 25;
   unsigned int waited_ms = 0;
   do
   {
      if (config_present())
         return 1;
      if (waited_ms >= timeout_ms)
         break;
      unsigned int sleep_ms = timeout_ms - waited_ms;
      if (sleep_ms > interval_ms)
         sleep_ms = interval_ms;
      struct timespec pause = {.tv_sec = sleep_ms / 1000,
                               .tv_nsec = (long)(sleep_ms % 1000) * 1000000L};
      while (nanosleep(&pause, &pause) != 0 && errno == EINTR)
         ;
      waited_ms += sleep_ms;
   } while (1);
   return 0;
}

void config_reload_register_reapplier(config_reapplier_fn fn)
{
   if (fn && g_reapplier_count < (int)(sizeof(g_reappliers) / sizeof(g_reappliers[0])))
      g_reappliers[g_reapplier_count++] = fn;
}

const char *config_default_dir(void)
{
   const char *base = aimee_home();
   return base ? base : "/tmp/.config/aimee";
}

const char *config_output_dir(void)
{
   static _Thread_local char fallback[MAX_PATH_LEN];
   const char *dir = config_default_dir();
   if (platform_mkdir_p(dir, 0700) == 0 && access(dir, W_OK) == 0)
      return dir;
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = "/tmp";
   snprintf(fallback, sizeof(fallback), "%s/aimee", tmp);
   (void)platform_mkdir_p(fallback, 0700);
   return fallback;
}

int config_cache_disabled(void)
{
   const char *value = getenv("AIMEE_NO_CACHE");
   return value && value[0] && strcmp(value, "0") != 0;
}

const char *config_guardrail_mode(void)
{
   static _Thread_local char value[32];
   if (config_client_read_string("guardrail_mode", value, sizeof(value)) != 0 || !value[0])
      snprintf(value, sizeof(value), "approve");
   return value;
}

int config_sandbox_package_access_valid(const char *s)
{
   return s && (!strcmp(s, "proxy") || !strcmp(s, "off") || !strcmp(s, "gated") ||
                !strcmp(s, "governance"));
}

void config_sandbox(sandbox_config_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   cJSON *value = config_client_value_copy("sandbox");
   if (!cJSON_IsObject(value))
   {
      cJSON_Delete(value);
      return;
   }
   cJSON *mode = cJSON_GetObjectItemCaseSensitive(value, "mode");
   cJSON *network = cJSON_GetObjectItemCaseSensitive(value, "network_isolated");
   cJSON *paths = cJSON_GetObjectItemCaseSensitive(value, "allow_paths");
   out->mode = sandbox_mode_from_string(cJSON_IsString(mode) ? mode->valuestring : "off");
   out->network_isolated = cJSON_IsTrue(network);
   if (cJSON_IsArray(paths))
   {
      cJSON *path = NULL;
      cJSON_ArrayForEach(path, paths)
      {
         if (out->allow_path_count >= SANDBOX_MAX_ALLOW_PATHS)
            break;
         if (cJSON_IsString(path))
            snprintf(out->allow_paths[out->allow_path_count++], SANDBOX_MAX_PATH_LEN, "%s",
                     path->valuestring);
      }
   }
   cJSON_Delete(value);
}

int config_antipatterns_bypass(void)
{
   const char *v = getenv("AIMEE_ANTIPATTERNS_BYPASS");
   return v && (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") ||
                !strcasecmp(v, "yes"));
}

static const char *sidecar_endpoint(const char *configured, const char *env_name, char *out,
                                    size_t n)
{
   const char *value = configured && configured[0] ? configured : getenv(env_name);
   snprintf(out, n, "%s", value ? value : "");
   return out;
}

const char *config_tsr_endpoint(void)
{
   static _Thread_local char value[CONFIG_COPY_MAX];
   return sidecar_endpoint(config_tsr_command(), "AIMEE_TSR_URL", value, sizeof(value));
}

const char *config_ocr_endpoint(void)
{
   static _Thread_local char value[CONFIG_COPY_MAX];
   return sidecar_endpoint(config_ocr_command(), "AIMEE_OCR_URL", value, sizeof(value));
}

const char *config_embedder_command_current(const char *requested)
{
   static _Thread_local char value[CONFIG_COPY_MAX];
   const char *selected = requested && requested[0] ? requested : getenv("EMBEDDER_URL");
   if (!selected || !selected[0])
      selected = config_embedder_command_field();
   snprintf(value, sizeof(value), "%s", selected ? selected : "");
   return value;
}

const char *config_embedder_command_field(void)
{
   static _Thread_local char value[CONFIG_COPY_MAX];
   (void)config_client_read_string("embedder_command", value, sizeof(value));
   return value;
}

int config_module_enabled(int config_tristate, int env_default)
{
   return config_tristate == 0 || config_tristate == 1 ? config_tristate : !!env_default;
}

int config_module_roundtable_enabled(void)
{
   const char *env = getenv("AIMEE_MODULE_ROUNDTABLE");
   int fallback =
       env && (!strcasecmp(env, "1") || !strcasecmp(env, "true") || !strcasecmp(env, "on"));
   return config_module_enabled(config_module_roundtable(), fallback);
}

const char *econ_mode_name(int mode)
{
   if (mode == ECON_MODE_OFF)
      return "off";
   if (mode == ECON_MODE_AGGRESSIVE)
      return "aggressive";
   return "safe";
}

int econ_mode_parse(const char *s)
{
   if (s && !strcasecmp(s, "off"))
      return ECON_MODE_OFF;
   if (s && !strcasecmp(s, "safe"))
      return ECON_MODE_SAFE;
   if (s && !strcasecmp(s, "aggressive"))
      return ECON_MODE_AGGRESSIVE;
   return -1;
}

int econ_mode_current(void)
{
   return config_module_economizer() == 0 ? ECON_MODE_OFF : config_economizer_mode();
}

int econ_gateway_mutate_on_current(void)
{
   return econ_mode_current() == ECON_MODE_AGGRESSIVE;
}

void econ_preset_current(econ_preset_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   int mode = econ_mode_current();
   if (mode == ECON_MODE_OFF)
      return;
   out->json_compact = 1;
   if (mode == ECON_MODE_AGGRESSIVE)
   {
      out->history_fold = out->compress = out->command_filter = 1;
      out->freeze_guard_horizon = out->gateway_seam = 1;
      out->gateway_session_disable_ttl_ms = 3600000;
   }
}

const char *guardrails_semantic_mode_name(int mode)
{
   switch (mode)
   {
   case GSEM_MODE_DRY_RUN:
      return "dry_run";
   case GSEM_MODE_ADVISORY:
      return "advisory";
   case GSEM_MODE_ENFORCE:
      return "enforce";
   default:
      return "off";
   }
}

int guardrails_semantic_mode_parse(const char *s)
{
   if (s && (!strcasecmp(s, "dry_run") || !strcasecmp(s, "dryrun")))
      return GSEM_MODE_DRY_RUN;
   if (s && !strcasecmp(s, "advisory"))
      return GSEM_MODE_ADVISORY;
   if (s && !strcasecmp(s, "enforce"))
      return GSEM_MODE_ENFORCE;
   return GSEM_MODE_OFF;
}

static int parse_number(const char *text, double *out)
{
   if (!text || !out)
      return 0;
   char *end = NULL;
   errno = 0;
   double value = strtod(text, &end);
   if (errno || end == text || !end || *end)
      return 0;
   *out = value;
   return 1;
}

int config_set(const char *key, const char *value)
{
   if (!key || !key[0] || !value)
      return -1;
   if (!strcasecmp(value, "true"))
      return config_client_set_value(key, cJSON_CreateTrue());
   if (!strcasecmp(value, "false"))
      return config_client_set_value(key, cJSON_CreateFalse());
   double number = 0;
   if (parse_number(value, &number))
      return config_client_set_number(key, number);
   return config_client_set_string(key, value);
}

int config_persist_defaults(void)
{
   return config_client_operation("persist-defaults", NULL);
}

int config_workspace_add(const char *path, const char *provider, const char *remote,
                         const char *head)
{
   if (!path || !path[0])
      return -1;
   cJSON *value = cJSON_CreateObject();
   if (!value || !cJSON_AddStringToObject(value, "path", path))
   {
      cJSON_Delete(value);
      return -1;
   }
   if (provider)
      cJSON_AddStringToObject(value, "provider", provider);
   if (remote)
      cJSON_AddStringToObject(value, "remote", remote);
   if (head)
      cJSON_AddStringToObject(value, "head", head);
   return config_client_operation("workspace-add", value);
}

int config_workspace_remove(const char *path)
{
   if (!path || !path[0])
      return -1;
   cJSON *value = cJSON_CreateObject();
   if (!value || !cJSON_AddStringToObject(value, "path", path))
   {
      cJSON_Delete(value);
      return -1;
   }
   return config_client_operation("workspace-remove", value);
}

int config_apply_roundtable_preset(const config_roundtable_preset_t *p)
{
   if (!p || p->seat_count < 0 || p->seat_count > CONFIG_RT_PRESET_MAX_SEATS)
      return -1;
   cJSON *value = cJSON_CreateObject();
   cJSON *models = value ? cJSON_AddArrayToObject(value, "models") : NULL;
   cJSON *personas = value ? cJSON_AddArrayToObject(value, "personas") : NULL;
   if (!value || !models || !personas)
   {
      cJSON_Delete(value);
      return -1;
   }
   for (int i = 0; i < p->seat_count; i++)
   {
      cJSON_AddItemToArray(models, cJSON_CreateString(p->models[i]));
      cJSON_AddItemToArray(personas, cJSON_CreateString(p->personas[i]));
   }
#define ADD_PRESET_NUMBER(member) cJSON_AddNumberToObject(value, #member, p->member)
   ADD_PRESET_NUMBER(min_successful);
   ADD_PRESET_NUMBER(max_cost_usd);
   ADD_PRESET_NUMBER(max_rounds);
   ADD_PRESET_NUMBER(converge_threshold);
   ADD_PRESET_NUMBER(deadline_ms);
   ADD_PRESET_NUMBER(pipeline_max_passes);
   ADD_PRESET_NUMBER(pipeline_max_attempts_per_pass);
   ADD_PRESET_NUMBER(pipeline_max_cost_usd);
   ADD_PRESET_NUMBER(pipeline_max_total_cost_usd);
   ADD_PRESET_NUMBER(pipeline_gate_ttl_h);
   ADD_PRESET_NUMBER(pipeline_parked_releases_slot);
   ADD_PRESET_NUMBER(pipeline_unknown_context_tokens);
#undef ADD_PRESET_NUMBER
   cJSON_AddStringToObject(value, "turns", p->turns);
   cJSON_AddStringToObject(value, "pipeline_done_bar", p->pipeline_done_bar);
   cJSON_AddStringToObject(value, "name", p->name);
   return config_client_operation("apply-roundtable-preset", value);
}

/* No `enabled` parameter: the typed-fact layer has no master gate any more. It
 * used to default OFF, which made the whole layer a silent no-op on a stock
 * install, so the ability to set it has been removed rather than left as a
 * switch that can put a deployment back in that state. */
int config_set_typed_facts(int auto_promote, int promote_threshold)
{
   cJSON *value = cJSON_CreateObject();
   if (!value)
      return -1;
   if (auto_promote >= 0)
      cJSON_AddBoolToObject(value, "auto_promote", auto_promote);
   if (promote_threshold > 0)
      cJSON_AddNumberToObject(value, "promote_threshold", promote_threshold);
   return config_client_operation("set-typed-facts", value);
}

int config_set_api_http_listener(int http_port, int rate_limit_per_min)
{
   if (http_port <= 0 || rate_limit_per_min <= 0)
      return -1;
   cJSON *value = cJSON_CreateObject();
   if (!value)
      return -1;
   cJSON_AddNumberToObject(value, "http_port", http_port);
   cJSON_AddNumberToObject(value, "rate_limit_per_min", rate_limit_per_min);
   return config_client_operation("set-api-http-listener", value);
}

int config_disable_api_http_listener(void)
{
   return config_client_set_number("server.api.http_port", 0);
}

int config_set_model_concurrency(const char *model, int limit)
{
   if (!model || !model[0] || limit <= 0)
      return -1;
   cJSON *value = cJSON_CreateObject();
   if (!value)
      return -1;
   cJSON_AddStringToObject(value, "model", model);
   cJSON_AddNumberToObject(value, "limit", limit);
   int rc = config_client_operation("set-model-concurrency", value);
   return rc == -3 ? -2 : rc;
}

int config_remove_model_concurrency(const char *model)
{
   if (!model || !model[0])
      return -1;
   cJSON *value = cJSON_CreateObject();
   if (!value)
      return -1;
   cJSON_AddStringToObject(value, "model", model);
   return config_client_operation("remove-model-concurrency", value);
}

static int copy_json_string(cJSON *object, const char *name, char *out, size_t n)
{
   cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
   if (!cJSON_IsString(value))
      return -1;
   snprintf(out, n, "%s", value->valuestring);
   return 0;
}

static int copy_json_int(cJSON *object, const char *name)
{
   cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
   if (cJSON_IsBool(value))
      return cJSON_IsTrue(value);
   return cJSON_IsNumber(value) ? value->valueint : 0;
}

int config_cron_job_at(int index, cron_job_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *all = config_client_value_copy("cron_jobs");
   cJSON *row = cJSON_IsArray(all) ? cJSON_GetArrayItem(all, index) : NULL;
   if (!cJSON_IsObject(row))
   {
      cJSON_Delete(all);
      return -1;
   }
#define COPY_CRON_STRING(field) copy_json_string(row, #field, out->field, sizeof(out->field))
   (void)COPY_CRON_STRING(id);
   (void)COPY_CRON_STRING(schedule);
   (void)COPY_CRON_STRING(mode);
   (void)COPY_CRON_STRING(script);
   (void)COPY_CRON_STRING(prompt);
   (void)COPY_CRON_STRING(workdir);
   (void)COPY_CRON_STRING(context_from);
   (void)COPY_CRON_STRING(when_context_contains);
   (void)COPY_CRON_STRING(deliver_target);
#undef COPY_CRON_STRING
   out->deliver_only_if_changed = copy_json_int(row, "deliver_only_if_changed");
   out->deliver_first_run_silent = copy_json_int(row, "deliver_first_run_silent");
   out->pre_wake_gate = copy_json_int(row, "pre_wake_gate");
   out->enabled = copy_json_int(row, "enabled");
   cJSON *skills = cJSON_GetObjectItemCaseSensitive(row, "skills");
   cJSON *skill = NULL;
   cJSON_ArrayForEach(skill, skills)
   {
      if (out->skill_count >= CRON_JOB_MAX_SKILLS)
         break;
      if (cJSON_IsString(skill))
         snprintf(out->skills[out->skill_count++], CRON_JOB_MAX_SKILL_NAME, "%s",
                  skill->valuestring);
   }
   cJSON_Delete(all);
   return 0;
}

int config_trigger_rule_at(int index, trigger_rule_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *all = config_client_value_copy("trigger_rules");
   cJSON *row = cJSON_IsArray(all) ? cJSON_GetArrayItem(all, index) : NULL;
   if (!cJSON_IsObject(row))
   {
      cJSON_Delete(all);
      return -1;
   }
   (void)copy_json_string(row, "source", out->source, sizeof(out->source));
   (void)copy_json_string(row, "event", out->event, sizeof(out->event));
   (void)copy_json_string(row, "schedule", out->schedule, sizeof(out->schedule));
   (void)copy_json_string(row, "mode", out->mode, sizeof(out->mode));
   cJSON *pipeline = cJSON_GetObjectItemCaseSensitive(row, "pipeline");
   if (cJSON_IsObject(pipeline))
   {
      (void)copy_json_string(pipeline, "template", out->pipeline_template,
                             sizeof(out->pipeline_template));
      (void)copy_json_string(pipeline, "workspace", out->workspace, sizeof(out->workspace));
      cJSON *cost = cJSON_GetObjectItemCaseSensitive(pipeline, "max_spend_usd");
      if (cJSON_IsNumber(cost))
         out->max_spend_usd = cost->valuedouble;
   }
   cJSON_Delete(all);
   return 0;
}

int config_lsp_server_at(int index, config_lsp_server_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *all = config_client_value_copy("lsp_servers");
   cJSON *row = cJSON_IsArray(all) ? cJSON_GetArrayItem(all, index) : NULL;
   if (!cJSON_IsObject(row))
   {
      cJSON_Delete(all);
      return -1;
   }
   (void)copy_json_string(row, "name", out->name, sizeof(out->name));
   (void)copy_json_string(row, "command", out->command, sizeof(out->command));
   cJSON *args = cJSON_GetObjectItemCaseSensitive(row, "args");
   cJSON *arg = NULL;
   cJSON_ArrayForEach(arg, args)
   {
      if (out->arg_count >= CONFIG_LSP_MAX_ARGS)
         break;
      if (cJSON_IsString(arg))
         snprintf(out->args[out->arg_count++], sizeof(out->args[0]), "%s", arg->valuestring);
   }
   cJSON *extensions = cJSON_GetObjectItemCaseSensitive(row, "extensions");
   cJSON *extension = NULL;
   cJSON_ArrayForEach(extension, extensions)
   {
      if (out->extension_count >= CONFIG_LSP_MAX_EXTENSIONS)
         break;
      if (cJSON_IsString(extension))
         snprintf(out->extensions[out->extension_count++], sizeof(out->extensions[0]), "%s",
                  extension->valuestring);
   }
   cJSON_Delete(all);
   return 0;
}

int config_concurrency_per_model_at(int index, config_concurrency_entry_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *all = config_client_value_copy("concurrency_per_model");
   cJSON *row = cJSON_IsArray(all) ? cJSON_GetArrayItem(all, index) : NULL;
   if (!cJSON_IsObject(row))
   {
      cJSON_Delete(all);
      return -1;
   }
   (void)copy_json_string(row, "key", out->key, sizeof(out->key));
   out->limit = copy_json_int(row, "limit");
   cJSON_Delete(all);
   return 0;
}

int config_aux_task_at(int index, config_aux_task_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *all = config_client_value_copy("aux_tasks");
   cJSON *row = cJSON_IsArray(all) ? cJSON_GetArrayItem(all, index) : NULL;
   if (!cJSON_IsObject(row))
   {
      cJSON_Delete(all);
      return -1;
   }
   (void)copy_json_string(row, "task", out->task, sizeof(out->task));
   (void)copy_json_string(row, "provider", out->provider, sizeof(out->provider));
   (void)copy_json_string(row, "model", out->model, sizeof(out->model));
   out->max_tokens = copy_json_int(row, "max_tokens");
   cJSON_Delete(all);
   return 0;
}

int config_mcp_client_at(int index, config_mcp_client_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *all = config_client_value_copy("mcp_clients");
   cJSON *row = cJSON_IsArray(all) ? cJSON_GetArrayItem(all, index) : NULL;
   if (!cJSON_IsObject(row))
   {
      cJSON_Delete(all);
      return -1;
   }
   (void)copy_json_string(row, "name", out->name, sizeof(out->name));
   (void)copy_json_string(row, "cwd", out->cwd, sizeof(out->cwd));
   (void)copy_json_string(row, "url", out->url, sizeof(out->url));
   (void)copy_json_string(row, "bearer_token_env", out->bearer_token_env,
                          sizeof(out->bearer_token_env));
   const char *transport = "";
   cJSON *transport_value = cJSON_GetObjectItemCaseSensitive(row, "transport");
   if (cJSON_IsString(transport_value))
      transport = transport_value->valuestring;
   out->transport = !strcmp(transport, "stdio") ? CONFIG_MCP_TRANSPORT_STDIO
                    : !strcmp(transport, "sse") ? CONFIG_MCP_TRANSPORT_SSE
                                                : CONFIG_MCP_TRANSPORT_NONE;
   cJSON *install = cJSON_GetObjectItemCaseSensitive(row, "install");
   out->install = cJSON_IsString(install) && !strcmp(install->valuestring, "kb")
                      ? CONFIG_MCP_INSTALL_KB
                      : CONFIG_MCP_INSTALL_SERVER;
   cJSON *commands = cJSON_GetObjectItemCaseSensitive(row, "command");
   cJSON *command = NULL;
   cJSON_ArrayForEach(command, commands)
   {
      if (out->command_count >= CONFIG_MCP_MAX_COMMAND_ARGS)
         break;
      if (cJSON_IsString(command))
         snprintf(out->command[out->command_count++], sizeof(out->command[0]), "%s",
                  command->valuestring);
   }
   cJSON_Delete(all);
   return 0;
}

int config_server_api_bearer_extra_snapshot(char out[][256], int max)
{
   if (!out || max <= 0)
      return 0;
   int count = config_server_api_bearer_extra_count();
   if (count > max)
      count = max;
   for (int i = 0; i < count; i++)
      snprintf(out[i], 256, "%s", config_server_api_bearer_extra(i));
   return count;
}

int config_disposition_source(int index)
{
   double value = 0;
   (void)config_client_read_indexed_number("dispositions", index, "source", &value);
   return (int)value;
}

const char *config_disposition_source_name(config_disposition_source_t source)
{
   switch (source)
   {
   case CONFIG_DISPOSITION_SOURCE_GLOBAL:
      return "global";
   case CONFIG_DISPOSITION_SOURCE_WORKSPACE:
      return "workspace";
   case CONFIG_DISPOSITION_SOURCE_PROJECT:
      return "project";
   default:
      return "none";
   }
}

int config_conversation_dirs(char dirs[][MAX_PATH_LEN], int max_dirs)
{
   if (!dirs || max_dirs <= 0)
      return 0;
   const char *home = getenv("HOME");
   if (!home)
      return 0;
   const char *provider = config_provider();
   if (!strcmp(provider, "claude"))
      snprintf(dirs[0], MAX_PATH_LEN, "%s/.claude/projects", home);
   else if (!strcmp(provider, "codex"))
      snprintf(dirs[0], MAX_PATH_LEN, "%s/.codex/sessions", home);
   else
      return 0;
   return 1;
}

int config_autonomy_lookup(const char *env_name, long *out)
{
   if (!env_name || !out || strncmp(env_name, "AIMEE_AUTONOMY_", 15) != 0)
      return 0;
   const char *override = getenv(env_name);
   if (override && override[0])
   {
      char *end = NULL;
      long parsed = strtol(override, &end, 10);
      if (end && !*end)
      {
         *out = parsed;
         return 1;
      }
   }
   char key[128] = "autonomy_";
   size_t at = strlen(key);
   for (const char *p = env_name + 15; *p && at + 1 < sizeof(key); p++)
      key[at++] = (char)tolower((unsigned char)*p);
   key[at] = 0;
   double value = 0;
   if (config_client_read_number(key, &value) != 0)
      return 0;
   *out = (long)value;
   return 1;
}

void config_secret_writer_set(config_secret_writer_fn writer)
{
   g_secret_writer = writer;
}

int config_secret_store(const char *name, const char *value)
{
   return g_secret_writer ? g_secret_writer(name, value) : -1;
}

static aimee_mode_t mode_parse(const char *s)
{
   if (s && !strcasecmp(s, "novel"))
      return AIMEE_MODE_NOVEL;
   if (s && !strcasecmp(s, "songwriter"))
      return AIMEE_MODE_SONGWRITER;
   if (s && !strcasecmp(s, "qa"))
      return AIMEE_MODE_QA;
   if (s && !strcasecmp(s, "security"))
      return AIMEE_MODE_SECURITY;
   if (s && !strcasecmp(s, "reviewer"))
      return AIMEE_MODE_REVIEWER;
   if (s && !strcasecmp(s, "architect"))
      return AIMEE_MODE_ARCHITECT;
   if (s && !strcasecmp(s, "reviewer-constructive"))
      return AIMEE_MODE_REVIEWER_CONSTRUCTIVE;
   if (s && !strcasecmp(s, "technical-writer"))
      return AIMEE_MODE_TECH_WRITER;
   return AIMEE_MODE_ENGINEER;
}

static void mode_path(char *out, size_t n)
{
   snprintf(out, n, "%s/mode", config_default_dir());
}

void config_current_persona(char *out, size_t n)
{
   if (!out || n == 0)
      return;
   const char *env = getenv("AIMEE_MODE");
   if (env && env[0])
   {
      snprintf(out, n, "%s", env);
      return;
   }
   char path[MAX_PATH_LEN];
   mode_path(path, sizeof(path));
   FILE *file = fopen(path, "r");
   if (!file || !fgets(out, (int)n, file))
      snprintf(out, n, "engineer");
   if (file)
      fclose(file);
   out[strcspn(out, "\r\n")] = 0;
}

aimee_mode_t config_current_mode(void)
{
   char persona[64];
   config_current_persona(persona, sizeof(persona));
   return mode_parse(persona);
}

int config_persist_mode(const char *mode)
{
   if (platform_mkdir_p(config_default_dir(), 0700) != 0)
      return -1;
   char path[MAX_PATH_LEN];
   mode_path(path, sizeof(path));
   FILE *file = fopen(path, "w");
   if (!file)
      return -1;
   fprintf(file, "%s\n", mode && mode[0] ? mode : "engineer");
   return fclose(file) == 0 ? 0 : -1;
}

const char *config_default_db1_path(void)
{
   static _Thread_local char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/aimee.db", config_default_dir());
   return path;
}

int config_db2_url_effective(char *out, size_t n)
{
   if (!out || n == 0)
      return 0;
   out[0] = 0;
   return runtime_secret_get("AIMEE_DB2_URL", out, n) && out[0];
}

int config_embedder_dims_default(void)
{
   return CONFIG_EMBEDDER_DIMS_DEFAULT;
}

int config_resolve_embedder_dims_current(void)
{
   const char *env = getenv("EMBEDDER_DIMS");
   if (env && env[0])
   {
      char *end = NULL;
      long value = strtol(env, &end, 10);
      if (end && !*end && value >= 1 && value <= EMBED_MAX_DIM)
         return (int)value;
   }
   return config_embedder_dims();
}

int config_embedder_dims_current(void)
{
   int value = config_resolve_embedder_dims_current();
   return value > 0 ? value : CONFIG_EMBEDDER_DIMS_DEFAULT;
}

int config_embedder_dims_pinned_current(void)
{
   return config_resolve_embedder_dims_current() > 0;
}

int config_synth_chat_endpoint_current(char *out, size_t n)
{
   if (!out || n == 0)
      return 0;
   const char *endpoint = getenv("SYNTHESIS_ENDPOINT");
   if (!endpoint || !endpoint[0])
      endpoint = config_synthesis_endpoint();
   if (!endpoint || !endpoint[0])
   {
      out[0] = 0;
      return 0;
   }
   size_t len = strlen(endpoint);
   while (len && endpoint[len - 1] == '/')
      len--;
   if (len == 0)
   {
      out[0] = 0;
      return 0;
   }
   int has_v1 = len >= 3 && !strncmp(endpoint + len - 3, "/v1", 3);
   int wrote = snprintf(out, n, "%.*s%s", (int)len, endpoint, has_v1 ? "" : "/v1");
   if (wrote < 0 || (size_t)wrote >= n)
   {
      out[0] = 0;
      return 0;
   }
   return 1;
}

void config_emit_deploy_env_current(char *buf, size_t n)
{
   if (!buf || n == 0)
      return;
   buf[0] = 0;
   size_t pos = 0;
#define EMITF(...)                                                                                 \
   do                                                                                              \
   {                                                                                               \
      if (pos < n)                                                                                 \
         pos += (size_t)snprintf(buf + pos, n - pos, __VA_ARGS__);                                 \
   } while (0)

   const char *kb_mode = config_kb_mode();
   const char *kb_url = config_kb_client_url();
   const char *embedder_model = config_embedder_model();
   const char *embedder_url = config_embedder_url();
   const char *synthesis_model = config_synthesis_model();
   const char *synthesis_endpoint = config_synthesis_endpoint();
   int remote_kb = !strcmp(kb_mode, "remote");
   int local_synthesis = !remote_kb && synthesis_model[0] && !synthesis_endpoint[0];

   EMITF("COMPOSE_PROFILES=%s\n", remote_kb ? "" : (local_synthesis ? "kb,llm" : "kb"));
   if (remote_kb)
   {
      if (kb_url[0])
         EMITF("AIMEE_KB_API_URL=%s\n", kb_url);
      return;
   }

   const char *kb_variant =
       embedder_url[0] ? ""
                       : (!strcmp(embedder_model, "nomic-embed-text-v2-moe") ? "nomic" : "a25m");
   EMITF("AIMEE_KB_VARIANT=%s\n", kb_variant);
   if (embedder_model[0])
      EMITF("EMBEDDER_MODEL=%s\n", embedder_model);
   if (embedder_url[0])
      EMITF("EMBEDDER_URL=%s\n", embedder_url);
   if (synthesis_endpoint[0])
      EMITF("SYNTHESIS_ENDPOINT=%s\n", synthesis_endpoint);
   if (synthesis_model[0])
      EMITF("SYNTHESIS_MODEL=%s\n", synthesis_model);
   if (local_synthesis)
   {
      EMITF("AIMEE_LLM_VARIANT=%s\n", strstr(synthesis_model, "E2B") ? "e2b" : "e4b");
      EMITF("AIMEE_LLM_HOST=aimee-llm\n");
      EMITF("SYNTHESIS_ENDPOINT=https://aimee-llm:8761/v1\n");
      EMITF("SYNTHESIS_CA_FILE=/var/lib/aimee/synthesis-tls/ca.pem\n");
      EMITF("SYNTHESIS_CERT_FILE=/var/lib/aimee/synthesis-tls/client.pem\n");
      EMITF("SYNTHESIS_KEY_FILE=/var/lib/aimee/synthesis-tls/client.key\n");
   }
   if (config_embedder_dims_pinned_current() && config_embedder_dims_current() > 0)
      EMITF("EMBEDDER_DIMS=%d\n", config_embedder_dims_current());
#undef EMITF
}

static double config_number_or_zero(const char *key)
{
   double value = 0;
   (void)config_client_read_number(key, &value);
   return value;
}

#define CONFIG_FLAG(name, key)                                                                     \
   int name(void)                                                                                  \
   {                                                                                               \
      return (int)config_number_or_zero(key);                                                      \
   }
/* Governed-action WORM capture is a required control. The historical module
 * default is 0, so treat that legacy value as enabled too; disabling requires
 * an explicit break-glass environment marker that is visible to deployment
 * policy and cannot happen through an ordinary config edit. */
int config_audit_worm_enabled(void)
{
   const char *emergency = getenv("AIMEE_AUDIT_WORM_EMERGENCY_DISABLE");
   return !(emergency && strcmp(emergency, "I_ACKNOWLEDGE_AUDIT_LOSS") == 0);
}
CONFIG_FLAG(config_bandit_live_decision_enabled, "bandit_live_decision_enabled")
CONFIG_FLAG(config_css_style_graph_enabled, "css_style_graph_enabled")
CONFIG_FLAG(config_delegate_graph_context_enabled, "delegate_graph_context_enabled")
CONFIG_FLAG(config_drift_detect_shadow_enabled, "drift_detect_shadow_enabled")
CONFIG_FLAG(config_guardrails_blast_radius_advisory_enabled,
            "guardrails_blast_radius_advisory_enabled")
CONFIG_FLAG(config_ingress_usage_accounting_enabled, "ingress_usage_accounting_enabled")
CONFIG_FLAG(config_kb_pdf_vector_enabled, "kb_pdf_vector_enabled")
CONFIG_FLAG(config_memory_derive_facts_enabled, "memory_derive_facts_enabled")
CONFIG_FLAG(config_memory_routing_enabled, "memory_routing_enabled")
CONFIG_FLAG(config_transport_kb_pool_enabled, "transport_kb_pool_enabled")
CONFIG_FLAG(config_wfe_live_forge_enabled, "wfe_live_forge_enabled")
CONFIG_FLAG(config_ingress_audit_async, "ingress_audit_async")
#undef CONFIG_FLAG

double config_memory_semantic_floor_scale(void)
{
   return config_number_or_zero("memory_semantic_floor_scale");
}

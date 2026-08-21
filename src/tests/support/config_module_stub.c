/* In-process event-bus test double for native unit tests.
 *
 * Production callers never parse configuration. Unit binaries that exercise an
 * unrelated native component still need a deterministic peer for the config
 * caller, so this double answers the same JSON contract from the Go module's
 * checked-in golden defaults. End-to-end tests use the real module process. */
#include "module_json_call.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *g_values;
static char g_source_path[4096];
static struct stat g_source_stat;
static int g_source_known;
static uint64_t g_source_hash;
static int g_mutation_dirty;
static char g_profiles[32][128];
static int g_profile_count;

static int profile_index(const char *name)
{
   for (int i = 0; i < g_profile_count; i++)
      if (!strcmp(g_profiles[i], name))
         return i;
   return -1;
}

static int mutate_profile(cJSON *request, int create)
{
   cJSON *change = cJSON_GetObjectItemCaseSensitive(request, "value");
   const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "name"));
   if (!name || !name[0] || strlen(name) >= sizeof(g_profiles[0]) || strchr(name, '/'))
      return -1;
   int index = profile_index(name);
   if (!create)
      return index >= 0 ? 0 : -2;
   if (index >= 0)
      return 0;
   if (g_profile_count >= (int)(sizeof(g_profiles) / sizeof(g_profiles[0])))
      return -3;
   snprintf(g_profiles[g_profile_count++], sizeof(g_profiles[0]), "%s", name);
   return 0;
}

static uint64_t source_hash(const char *path)
{
   FILE *file = fopen(path, "rb");
   if (!file)
      return 0;
   uint64_t hash = UINT64_C(1469598103934665603);
   unsigned char buffer[4096];
   size_t n;
   while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
      for (size_t i = 0; i < n; i++)
      {
         hash ^= buffer[i];
         hash *= UINT64_C(1099511628211);
      }
   fclose(file);
   return hash;
}

static int config_source_path(char *out, size_t n)
{
   const char *path = getenv("AIMEE_CONFIG_PATH");
   if (path && path[0])
      return snprintf(out, n, "%s", path) > 0 ? 0 : -1;
   const char *base = getenv("AIMEE_HOME");
   if (base && base[0])
      return snprintf(out, n, "%s/aimee.yaml", base) > 0 ? 0 : -1;
   base = getenv("HOME");
   static const char default_suffix[] = ".config/aimee/aimee.yaml";
   return base && base[0] && snprintf(out, n, "%s/%s", base, default_suffix) > 0 ? 0 : -1;
}

static int source_stat_equal(const struct stat *a, const struct stat *b)
{
   return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
          a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

/* Refresh from the external module's own Go store when a legacy native test
 * rewrites its YAML fixture. This test peer never parses YAML: it consumes the
 * secret-filtered JSON fixture mode exported by the same implementation that
 * serves production over the event bus. */
static int refresh_owned_snapshot(void)
{
   const char *module = getenv("AIMEE_CONFIG_TEST_MODULE");
   const char *host_home = getenv("AIMEE_CONFIG_TEST_HOST_HOME");
   const char *configured_home = getenv("AIMEE_HOME");
   const char *process_home = getenv("HOME");
   char path[sizeof(g_source_path)];
   struct stat current;
   if (!module || !module[0] || strchr(module, '\'') || config_source_path(path, sizeof(path)) != 0)
      return 0;
   /* Never let an unrelated unit test read the developer's real config merely
    * because it toggled the historical no-cache switch. Fixture tests point
    * AIMEE_HOME/HOME at a private temporary tree (or set AIMEE_CONFIG_PATH). */
   if (!getenv("AIMEE_CONFIG_PATH") && host_home && host_home[0] &&
       (!configured_home || !configured_home[0]) && process_home &&
       !strcmp(process_home, host_home))
      return 0;
   int exists = stat(path, &current) == 0;
   uint64_t current_hash = exists ? source_hash(path) : 0;
   if (g_source_known && !strcmp(path, g_source_path))
   {
      /* A contract mutation is authoritative until an actual fixture file
       * appears or changes. Re-running the external snapshot reader for the
       * same absent path would reload defaults before every accessor and erase
       * the preceding set-versioned operation. */
      if (!exists || (g_mutation_dirty && current_hash == g_source_hash) ||
          (source_stat_equal(&current, &g_source_stat) && current_hash == g_source_hash))
         return 0;
   }
   if (!exists && !g_source_known)
      return 0;

   char command[8192];
   if (snprintf(command, sizeof(command), "'%s' --snapshot", module) <= 0)
      return -1;
   FILE *pipe = popen(command, "r");
   if (!pipe)
      return -1;
   size_t used = 0;
   size_t capacity = 65536;
   char *body = malloc(capacity);
   if (!body)
   {
      pclose(pipe);
      return -1;
   }
   while (!feof(pipe))
   {
      if (used + 4096 + 1 > capacity)
      {
         if (capacity >= 16u * 1024u * 1024u)
            break;
         capacity *= 2;
         char *grown = realloc(body, capacity);
         if (!grown)
            break;
         body = grown;
      }
      used += fread(body + used, 1, capacity - used - 1, pipe);
      if (ferror(pipe))
         break;
   }
   body[used] = 0;
   int status = pclose(pipe);
   cJSON *snapshot = status == 0 ? cJSON_ParseWithLength(body, used) : NULL;
   free(body);
   cJSON *values = cJSON_GetObjectItemCaseSensitive(snapshot, "values");
   cJSON *copy = cJSON_IsObject(values) ? cJSON_Duplicate(values, 1) : NULL;
   cJSON_Delete(snapshot);
   if (!copy)
      return -1;
   cJSON_Delete(g_values);
   g_values = copy;
   snprintf(g_source_path, sizeof(g_source_path), "%s", path);
   if (exists)
      g_source_stat = current;
   else
      memset(&g_source_stat, 0, sizeof(g_source_stat));
   g_source_known = 1;
   g_source_hash = current_hash;
   g_mutation_dirty = 0;
   return 0;
}

static void load_defaults(void)
{
   if (g_values)
      return;
   const char *defaults_path = getenv("AIMEE_CONFIG_TEST_DEFAULTS");
   FILE *file = defaults_path && defaults_path[0] ? fopen(defaults_path, "rb") : NULL;
   if (!file)
   {
      g_values = cJSON_CreateObject();
      return;
   }
   fseek(file, 0, SEEK_END);
   long size = ftell(file);
   rewind(file);
   char *body = size > 0 ? malloc((size_t)size + 1) : NULL;
   if (!body || fread(body, 1, (size_t)size, file) != (size_t)size)
   {
      free(body);
      fclose(file);
      g_values = cJSON_CreateObject();
      return;
   }
   fclose(file);
   body[size] = 0;
   g_values = cJSON_Parse(body);
   free(body);
   if (!cJSON_IsObject(g_values))
   {
      cJSON_Delete(g_values);
      g_values = cJSON_CreateObject();
   }
}

static void normalized_key(const char *key, char *out, size_t n)
{
   size_t at = 0;
   for (; key && *key && at + 1 < n; key++)
      out[at++] = *key == '.' || *key == '-' ? '_' : *key;
   out[at] = 0;
}

static void replace_value(const char *key, cJSON *value)
{
   if (!cJSON_ReplaceItemInObjectCaseSensitive(g_values, key, value))
      cJSON_AddItemToObject(g_values, key, value);
}

static cJSON *ensure_array(const char *key)
{
   cJSON *array = cJSON_GetObjectItemCaseSensitive(g_values, key);
   if (cJSON_IsArray(array))
      return array;
   array = cJSON_CreateArray();
   replace_value(key, array);
   return array;
}

static int workspace_index(const char *path)
{
   cJSON *paths = ensure_array("workspaces");
   cJSON *item = NULL;
   int index = 0;
   cJSON_ArrayForEach(item, paths)
   {
      if (cJSON_IsString(item) && path && !strcmp(item->valuestring, path))
         return index;
      index++;
   }
   return -1;
}

static void set_array_string(cJSON *array, int index, const char *value)
{
   while (cJSON_GetArraySize(array) <= index)
      cJSON_AddItemToArray(array, cJSON_CreateString(""));
   cJSON_ReplaceItemInArray(array, index, cJSON_CreateString(value ? value : ""));
}

static int mutate_workspace(cJSON *request, int remove)
{
   cJSON *change = cJSON_GetObjectItemCaseSensitive(request, "value");
   const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "path"));
   if (!path || !path[0])
      return -1;
   cJSON *paths = ensure_array("workspaces");
   cJSON *providers = ensure_array("workspace_providers");
   cJSON *remotes = ensure_array("workspace_vcs_remote");
   cJSON *heads = ensure_array("workspace_vcs_head");
   int index = workspace_index(path);
   if (remove)
   {
      if (index < 0)
         return -2;
      cJSON_DeleteItemFromArray(paths, index);
      cJSON_DeleteItemFromArray(providers, index);
      cJSON_DeleteItemFromArray(remotes, index);
      cJSON_DeleteItemFromArray(heads, index);
   }
   else if (index >= 0)
      return -3;
   else
   {
      index = cJSON_GetArraySize(paths);
      cJSON_AddItemToArray(paths, cJSON_CreateString(path));
      set_array_string(providers, index,
                       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "provider")));
      set_array_string(remotes, index,
                       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "remote")));
      set_array_string(heads, index,
                       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "head")));
   }
   replace_value("workspace_count", cJSON_CreateNumber(cJSON_GetArraySize(paths)));
   return 0;
}

static void copy_member(cJSON *from, const char *member, const char *key)
{
   cJSON *value = cJSON_GetObjectItemCaseSensitive(from, member);
   if (value)
      replace_value(key, cJSON_Duplicate(value, 1));
}

static int mutate_roundtable_preset(cJSON *request)
{
   cJSON *change = cJSON_GetObjectItemCaseSensitive(request, "value");
   cJSON *models = cJSON_GetObjectItemCaseSensitive(change, "models");
   cJSON *personas = cJSON_GetObjectItemCaseSensitive(change, "personas");
   const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "name"));
   if (!cJSON_IsArray(models) || !cJSON_IsArray(personas) || !name || !name[0] ||
       cJSON_GetArraySize(models) != cJSON_GetArraySize(personas))
      return -1;
   replace_value("ensemble_reference_models", cJSON_Duplicate(models, 1));
   replace_value("ensemble_reference_personas", cJSON_Duplicate(personas, 1));
   replace_value("ensemble_reference_count", cJSON_CreateNumber(cJSON_GetArraySize(models)));
   replace_value("ensemble_reference_persona_count",
                 cJSON_CreateNumber(cJSON_GetArraySize(personas)));
   static const struct
   {
      const char *member;
      const char *key;
   } fields[] = {{"min_successful", "ensemble_min_successful"},
                 {"max_cost_usd", "ensemble_max_cost_usd"},
                 {"max_rounds", "roundtable_max_rounds"},
                 {"converge_threshold", "roundtable_converge_threshold"},
                 {"deadline_ms", "roundtable_deadline_ms"},
                 {"pipeline_max_passes", "roundtable_pipeline_max_passes"},
                 {"pipeline_max_attempts_per_pass", "roundtable_pipeline_max_attempts_per_pass"},
                 {"pipeline_max_cost_usd", "roundtable_pipeline_max_cost_usd"},
                 {"pipeline_max_total_cost_usd", "roundtable_pipeline_max_total_cost_usd"},
                 {"pipeline_gate_ttl_h", "roundtable_pipeline_gate_ttl_h"},
                 {"pipeline_parked_releases_slot", "roundtable_pipeline_parked_releases_slot"},
                 {"pipeline_unknown_context_tokens", "roundtable_pipeline_unknown_context_tokens"},
                 {"turns", "roundtable_turns"},
                 {"pipeline_done_bar", "roundtable_pipeline_done_bar"}};
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      copy_member(change, fields[i].member, fields[i].key);
   replace_value("roundtable_default", cJSON_CreateString(name));
   return 0;
}

static int mutate_typed_facts(cJSON *request)
{
   cJSON *change = cJSON_GetObjectItemCaseSensitive(request, "value");
   if (!cJSON_IsObject(change))
      return -1;
   copy_member(change, "enabled", "typed_facts_enabled");
   copy_member(change, "auto_promote", "kb_typed_facts_auto_promote_enabled");
   copy_member(change, "promote_threshold", "kb_typed_facts_promote_threshold");
   return 0;
}

static int mutate_api_listener(cJSON *request)
{
   cJSON *change = cJSON_GetObjectItemCaseSensitive(request, "value");
   cJSON *port = cJSON_GetObjectItemCaseSensitive(change, "http_port");
   cJSON *rate = cJSON_GetObjectItemCaseSensitive(change, "rate_limit_per_min");
   if (!cJSON_IsNumber(port) || !cJSON_IsNumber(rate))
      return -1;
   replace_value("server_api_http_port", cJSON_Duplicate(port, 1));
   replace_value("server_api_rate_limit_per_min", cJSON_Duplicate(rate, 1));
   return 0;
}

static int model_concurrency_index(cJSON *entries, const char *model)
{
   cJSON *entry = NULL;
   int index = 0;
   cJSON_ArrayForEach(entry, entries)
   {
      const char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "key"));
      if (key && !strcmp(key, model))
         return index;
      index++;
   }
   return -1;
}

static int mutate_model_concurrency(cJSON *request, int remove)
{
   cJSON *change = cJSON_GetObjectItemCaseSensitive(request, "value");
   const char *model = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "model"));
   cJSON *limit = cJSON_GetObjectItemCaseSensitive(change, "limit");
   if (!model || !model[0] || (!remove && !cJSON_IsNumber(limit)))
      return -1;
   cJSON *entries = ensure_array("concurrency_per_model");
   int index = model_concurrency_index(entries, model);
   if (remove)
   {
      if (index < 0)
         return -2;
      cJSON_DeleteItemFromArray(entries, index);
      return 0;
   }
   cJSON *entry = cJSON_CreateObject();
   cJSON_AddStringToObject(entry, "key", model);
   cJSON_AddItemToObject(entry, "limit", cJSON_Duplicate(limit, 1));
   if (index >= 0)
      cJSON_ReplaceItemInArray(entries, index, entry);
   else
      cJSON_AddItemToArray(entries, entry);
   replace_value("concurrency_per_model_count", cJSON_CreateNumber(cJSON_GetArraySize(entries)));
   return 0;
}

cJSON *config_client_transport_call(uint32_t event_kind, uint32_t stage_id, cJSON *request,
                                    size_t max_body, int timeout_ms,
                                    aimee_module_call_result_t *result)
{
   (void)max_body;
   (void)timeout_ms;
   if (result)
      *result = AIMEE_MODULE_CALL_INVALID_REQUEST;
   if (event_kind != 4609u || stage_id != 1u || !cJSON_IsObject(request))
   {
      cJSON_Delete(request);
      return NULL;
   }
   pthread_mutex_lock(&g_lock);
   load_defaults();
   int refresh_rc = refresh_owned_snapshot();
   cJSON *response = cJSON_CreateObject();
   const char *operation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "operation"));
   if (refresh_rc != 0)
   {
      cJSON_AddBoolToObject(response, "ok", 0);
      cJSON_AddStringToObject(response, "error", "external config test fixture failed");
   }
   else if (operation && !strcmp(operation, "snapshot"))
   {
      cJSON_AddBoolToObject(response, "ok", 1);
      cJSON_AddItemToObject(response, "values", cJSON_Duplicate(g_values, 1));
      cJSON_AddStringToObject(response, "version",
                              "0000000000000000000000000000000000000000000000000000000000000000");
   }
   else if (operation && !strcmp(operation, "version"))
   {
      cJSON_AddBoolToObject(response, "ok", 1);
      cJSON_AddStringToObject(response, "version",
                              "0000000000000000000000000000000000000000000000000000000000000000");
   }
   else if (operation && !strcmp(operation, "value"))
   {
      char key[256];
      normalized_key(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "key")), key,
                     sizeof(key));
      cJSON *value = cJSON_GetObjectItemCaseSensitive(g_values, key);
      cJSON_AddBoolToObject(response, "ok", value != NULL);
      if (value)
         cJSON_AddItemToObject(response, "value", cJSON_Duplicate(value, 1));
      else
      {
         cJSON_AddStringToObject(response, "code", "not_found");
         cJSON_AddStringToObject(response, "error", "config key not found");
      }
   }
   else if (operation && !strcmp(operation, "set-versioned"))
   {
      char key[256];
      normalized_key(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "key")), key,
                     sizeof(key));
      cJSON *value = cJSON_GetObjectItemCaseSensitive(request, "value");
      if (key[0] && value)
      {
         cJSON *copy = cJSON_Duplicate(value, 1);
         replace_value(key, copy);
         g_mutation_dirty = 1;
      }
      cJSON_AddBoolToObject(response, "ok", key[0] && value);
   }
   else if (operation &&
            (!strcmp(operation, "workspace-add") || !strcmp(operation, "workspace-remove")))
   {
      int rc = mutate_workspace(request, !strcmp(operation, "workspace-remove"));
      if (rc == 0)
         g_mutation_dirty = 1;
      cJSON_AddBoolToObject(response, "ok", rc == 0);
      if (rc)
      {
         cJSON_AddStringToObject(response, "code",
                                 rc == -2   ? "not_found"
                                 : rc == -3 ? "exists"
                                            : "invalid");
         cJSON_AddStringToObject(response, "error", "workspace mutation failed");
      }
   }
   else if (operation && !strcmp(operation, "apply-roundtable-preset"))
   {
      int rc = mutate_roundtable_preset(request);
      g_mutation_dirty |= rc == 0;
      cJSON_AddBoolToObject(response, "ok", rc == 0);
   }
   else if (operation && !strcmp(operation, "set-typed-facts"))
   {
      int rc = mutate_typed_facts(request);
      g_mutation_dirty |= rc == 0;
      cJSON_AddBoolToObject(response, "ok", rc == 0);
   }
   else if (operation && !strcmp(operation, "set-api-http-listener"))
   {
      int rc = mutate_api_listener(request);
      g_mutation_dirty |= rc == 0;
      cJSON_AddBoolToObject(response, "ok", rc == 0);
   }
   else if (operation && (!strcmp(operation, "set-model-concurrency") ||
                          !strcmp(operation, "remove-model-concurrency")))
   {
      int rc = mutate_model_concurrency(request, !strcmp(operation, "remove-model-concurrency"));
      g_mutation_dirty |= rc == 0;
      cJSON_AddBoolToObject(response, "ok", rc == 0);
      if (rc)
      {
         cJSON_AddStringToObject(response, "code", rc == -2 ? "not_found" : "invalid");
         cJSON_AddStringToObject(response, "error", "model concurrency mutation failed");
      }
   }
   else if (operation &&
            (!strcmp(operation, "profile-create") || !strcmp(operation, "profile-present")))
   {
      int rc = mutate_profile(request, !strcmp(operation, "profile-create"));
      cJSON_AddBoolToObject(response, "ok", rc == 0);
      if (rc)
      {
         cJSON_AddStringToObject(response, "code",
                                 rc == -2   ? "not_found"
                                 : rc == -3 ? "full"
                                            : "invalid");
         cJSON_AddStringToObject(response, "error", "profile config operation failed");
      }
   }
   else
      cJSON_AddBoolToObject(response, "ok", operation != NULL);
   cJSON_Delete(request);
   pthread_mutex_unlock(&g_lock);
   if (result)
      *result = AIMEE_MODULE_CALL_OK;
   return response;
}

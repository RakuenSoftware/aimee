/* Native caller contract for the independently deployed pure-Go config module.
 * The transport seam captures the exact event/stage/JSON request and returns a
 * representative module response. No native config parser or store is linked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "config_client.h"
#include "config_database.h"
#include "module_json_call.h"

static cJSON *last_request;
static cJSON *last_mutation;
static uint32_t last_event_kind;
static uint32_t last_stage_id;
static size_t last_max_body;
static int last_timeout_ms;
static int changed_version;
static int snapshot_calls;

static void must(int condition, const char *message)
{
   if (!condition)
   {
      fprintf(stderr, "FAIL: %s\n", message);
      abort();
   }
}

static cJSON *success(void)
{
   cJSON *response = cJSON_CreateObject();
   must(response && cJSON_AddTrueToObject(response, "ok"), "allocate success response");
   return response;
}

cJSON *config_client_transport_call(uint32_t event_kind, uint32_t stage_id, cJSON *request,
                                    size_t max_body, int timeout_ms,
                                    aimee_module_call_result_t *result)
{
   cJSON_Delete(last_request);
   last_request = cJSON_Duplicate(request, 1);
   cJSON_Delete(request);
   last_event_kind = event_kind;
   last_stage_id = stage_id;
   last_max_body = max_body;
   last_timeout_ms = timeout_ms;
   *result = AIMEE_MODULE_CALL_OK;

   cJSON *operation = cJSON_GetObjectItemCaseSensitive(last_request, "operation");
   must(cJSON_IsString(operation), "request has operation");
   if (strcmp(operation->valuestring, "snapshot") && strcmp(operation->valuestring, "version"))
   {
      cJSON_Delete(last_mutation);
      last_mutation = cJSON_Duplicate(last_request, 1);
   }
   if (!strcmp(operation->valuestring, "snapshot"))
   {
      snapshot_calls++;
      cJSON *response = success();
      cJSON *values = cJSON_AddObjectToObject(response, "values");
      must(values && cJSON_AddNumberToObject(values, "max_iterations", 37), "snapshot values");
      must(cJSON_AddStringToObject(values, "kb_mode", "local") != NULL, "kb mode");
      must(cJSON_AddStringToObject(values, "kb_client_url", "") != NULL, "kb URL");
      must(cJSON_AddStringToObject(values, "embedder_model", "bekko-a25m") != NULL,
           "embedder model");
      must(cJSON_AddStringToObject(values, "embedder_url", "") != NULL, "embedder URL");
      must(cJSON_AddStringToObject(values, "synthesis_model", "") != NULL, "synthesis model");
      must(cJSON_AddStringToObject(values, "synthesis_endpoint", "") != NULL, "synthesis endpoint");
      must(cJSON_AddStringToObject(
               response, "version",
               changed_version
                   ? "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                   : "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") != NULL,
           "snapshot version");
      return response;
   }
   if (!strcmp(operation->valuestring, "version"))
   {
      cJSON *response = success();
      must(cJSON_AddStringToObject(
               response, "version",
               changed_version
                   ? "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                   : "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") != NULL,
           "version response");
      return response;
   }
   return success();
}

static cJSON *request_value(void)
{
   cJSON *value = cJSON_GetObjectItemCaseSensitive(last_mutation, "value");
   must(cJSON_IsObject(value), "request has object value");
   return value;
}

static void expect_operation(const char *name)
{
   cJSON *request =
       (!strcmp(name, "snapshot") || !strcmp(name, "version")) ? last_request : last_mutation;
   cJSON *operation = cJSON_GetObjectItemCaseSensitive(request, "operation");
   must(cJSON_IsString(operation) && !strcmp(operation->valuestring, name), name);
   must(last_event_kind == AIMEE_CONFIG_EVENT_KIND, "config event kind");
   must(last_stage_id == AIMEE_CONFIG_STAGE_ID, "config stage");
   must(last_max_body == 16u * 1024u * 1024u, "config response bound");
   must(last_timeout_ms == 5000, "config deadline");
}

int main(void)
{
   must(config_snapshot_seed() == 0, "seed snapshot through module");
   expect_operation("snapshot");
   double number = 0;
   must(config_client_read_number("max_iterations", &number) == 0 && number == 37,
        "snapshot is cached for accessors");
   char deploy_env[2048];
   config_emit_deploy_env_current(deploy_env, sizeof(deploy_env));
   must(strstr(deploy_env, "COMPOSE_PROFILES=kb\n") != NULL, "local KB compose profile");
   must(strstr(deploy_env, "AIMEE_KB_VARIANT=a25m\n") != NULL, "bundled KB variant");
   must(strstr(deploy_env, "EMBEDDER_MODEL=bekko-a25m\n") != NULL, "bundled embedder selection");
   must(strstr(deploy_env, "TOKEN=") == NULL && strstr(deploy_env, "API_KEY=") == NULL,
        "deploy environment excludes credentials");

   must(config_reload_if_changed() == 0, "equal version avoids reload");
   expect_operation("version");
   must(snapshot_calls == 1, "unchanged version does not refetch");
   changed_version = 1;
   must(config_reload_if_changed() == 1, "changed version reloads");
   expect_operation("snapshot");
   must(snapshot_calls == 2, "changed version refetches once");

   int rc = config_set_typed_facts(0, 4);
   if (rc != 0)
      fprintf(stderr, "typed-facts rc=%d error=%s\n", rc, config_client_last_error());
   must(rc == 0, "typed-facts mutation");
   expect_operation("set-typed-facts");
   cJSON *value = request_value();
   /* `enabled` must NOT be in the payload: the master gate is retired, and the
    * setter no longer has a parameter that could put a deployment back behind
    * it. Asserting its absence keeps it from being quietly reintroduced. */
   must(cJSON_GetObjectItemCaseSensitive(value, "enabled") == NULL, "no typed enabled key");
   must(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(value, "auto_promote")),
        "typed auto-promote");
   must(cJSON_GetObjectItemCaseSensitive(value, "promote_threshold")->valueint == 4,
        "typed threshold");

   must(config_set_api_http_listener(8088, 90) == 0, "API listener mutation");
   expect_operation("set-api-http-listener");
   value = request_value();
   must(cJSON_GetObjectItemCaseSensitive(value, "http_port")->valueint == 8088, "API port");
   must(cJSON_GetObjectItemCaseSensitive(value, "rate_limit_per_min")->valueint == 90,
        "API rate limit");

   must(config_set_model_concurrency("provider/model", 3) == 0, "model mutation");
   expect_operation("set-model-concurrency");
   value = request_value();
   must(!strcmp(cJSON_GetObjectItemCaseSensitive(value, "model")->valuestring, "provider/model"),
        "model name");
   must(cJSON_GetObjectItemCaseSensitive(value, "limit")->valueint == 3, "model limit");

   must(config_remove_model_concurrency("provider/model") == 0, "model removal");
   expect_operation("remove-model-concurrency");
   value = request_value();
   must(!strcmp(cJSON_GetObjectItemCaseSensitive(value, "model")->valuestring, "provider/model"),
        "removed model name");

   must(config_client_key_is_secret("db2_url"), "DB URL is not config data");
   must(
       !strcmp(config_client_secret_name("kb_client_bearer_token"), "AIMEE_KB_CLIENT_BEARER_TOKEN"),
       "KB client credential maps to runtime secret");
   must(!config_client_key_is_secret("max_iterations"), "ordinary key is public");

   cJSON_Delete(last_request);
   cJSON_Delete(last_mutation);
   puts("test_bus_config_autonomy: OK (native caller contract matches pure-Go module bus API)");
   return 0;
}

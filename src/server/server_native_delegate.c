/* Authenticated internal native execution. The bus identity and sealed source
 * owner are authority; the body contains only canonical task data and a handle. */
#include "server_native_delegate.h"
#include "server_native_delegate_source.h"
#include "server_native_history.h"
#include "agent_exec.h"
#include "request_context.h"
#include "persona.h"
#include "cmd_agent_delegate_impl.h"
#include <aimee/audit/obs_bus.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static obs_bus_module_service_t *service;
static const aimee_module_stage_t stages[] = {
    {SERVER_NATIVE_DELEGATE_EVENT, SERVER_NATIVE_DELEGATE_STAGE}};
int server_native_delegate_start(void)
{
   if (service)
      return 0;
   return obs_bus_module_service_start(stages, 1, server_native_delegate_handle, NULL, &service);
}
void server_native_delegate_stop(void)
{
   obs_bus_module_service_stop(&service);
}
static const cJSON *get(const cJSON *j, const char *key)
{
   return cJSON_GetObjectItemCaseSensitive(j, key);
}
static const char *text(const cJSON *j, const char *key)
{
   return cJSON_GetStringValue(get(j, key));
}
static int field_string(const cJSON *j, const char *key, size_t maximum, int required)
{
   const cJSON *v = get(j, key);
   const char *s = cJSON_GetStringValue(v);
   if (!v)
      return !required;
   if (!s || (required && !s[0]) || strnlen(s, maximum + 1) > maximum)
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if (*p < 32 || *p == 127)
         return 0;
   return 1;
}
static int integer(const cJSON *j, const char *key, int *value)
{
   const cJSON *v = get(j, key);
   if (!v)
      return 0;
   if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > INT_MAX ||
       v->valuedouble != v->valueint)
      return -1;
   *value = v->valueint;
   return 0;
}
static int fields(const cJSON *j)
{
   static const char *const keys[] = {
       "version", "operation", "execution_scope",     "role", "persona", "model", "ir", "workdir",
       "tools",   "max_turns", "execution_timeout_ms"};
   const cJSON *f;
   cJSON_ArrayForEach(f, j)
   {
      size_t i;
      for (i = 0; i < sizeof keys / sizeof *keys; i++)
         if (!strcmp(f->string, keys[i]))
            break;
      if (i == sizeof keys / sizeof *keys)
         return -1;
   }
   return 0;
}
static aimee_module_status_t trigger_scope(const cJSON *json, uint8_t *output, uint32_t capacity,
                                           uint32_t *length)
{
   if (cJSON_GetArraySize(json) != 4)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   const cJSON *version = get(json, "version");
   const char *digest = text(json, "configuration_sha256");
   const cJSON *registration = get(json, "registration");
   if (!cJSON_IsNumber(version) || version->valuedouble != 3 || !text(json, "action") ||
       strcmp(text(json, "action"), "trigger_scope") || !cJSON_IsObject(registration) || !digest ||
       strlen(digest) != 64 || strspn(digest, "0123456789abcdef") != 64)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   char handle[65] = "", error[512] = "";
   if (server_native_delegate_source_trigger(registration, digest, handle, error, sizeof error))
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   cJSON *reply = cJSON_CreateObject();
   if (!reply || !cJSON_AddStringToObject(reply, "execution_scope", handle))
   {
      cJSON_Delete(reply);
      return AIMEE_MODULE_STATUS_INTERNAL;
   }
   char *wire = cJSON_PrintUnformatted(reply);
   cJSON_Delete(reply);
   if (!wire)
      return AIMEE_MODULE_STATUS_INTERNAL;
   size_t bytes = strlen(wire);
   aimee_module_status_t status = AIMEE_MODULE_STATUS_INTERNAL;
   if (bytes <= capacity)
   {
      memcpy(output, wire, bytes);
      *length = (uint32_t)bytes;
      status = AIMEE_MODULE_STATUS_OK;
   }
   cJSON_free(wire);
   return status;
}
static int remaining_ms(const aimee_module_invocation_t *invocation)
{
   struct timespec now;
   if (!invocation->deadline_ns || clock_gettime(CLOCK_MONOTONIC, &now))
      return 0;
   uint64_t current = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
   if (current >= invocation->deadline_ns)
      return 0;
   uint64_t ms = (invocation->deadline_ns - current) / 1000000u;
   return ms > INT_MAX ? INT_MAX : (int)ms;
}
static aimee_module_status_t
native_delegate_handle_impl(const aimee_module_invocation_t *invocation, const uint8_t *body,
                            uint32_t length, uint8_t *output, uint32_t capacity,
                            uint32_t *output_length, void *unused)
{
   (void)unused;
   if (output_length)
      *output_length = 0;
   if (!invocation || invocation->stage_id != SERVER_NATIVE_DELEGATE_STAGE ||
       invocation->source_principal_ref != SERVER_NATIVE_DELEGATE_PRINCIPAL ||
       !invocation->source_handle || !body || !output || !output_length)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;
   cJSON *json = aimee_ir_json_parse_frame((const char *)body, length);
   if (cJSON_IsObject(json) && get(json, "action"))
   {
      aimee_module_status_t result = trigger_scope(json, output, capacity, output_length);
      cJSON_Delete(json);
      return result;
   }
   aimee_request_t request = {0}, effective = {0};
   aimee_response_t response = {0};
   agent_result_t result = {0};
   agent_config_t *config = NULL;
   server_native_delegate_source_scope_t *scope = NULL;
   cJSON *history = NULL, *receipts = NULL, *reply = NULL;
   aimee_module_status_t status = AIMEE_MODULE_STATUS_INVALID_REQUEST;
   const cJSON *version = get(json, "version"), *tools = get(json, "tools");
   const char *handle = text(json, "execution_scope");
   int turns = 0, timeout = 0;
   if (!cJSON_IsObject(json) || fields(json) || !cJSON_IsNumber(version) ||
       version->valuedouble != 3 || !field_string(json, "operation", 256, 1) ||
       !field_string(json, "execution_scope", 64, 1) || !handle || strlen(handle) != 64 ||
       strspn(handle, "0123456789abcdef") != 64 || !field_string(json, "role", 127, 1) ||
       !field_string(json, "model", MAX_AGENT_NAME - 1, 1) ||
       !field_string(json, "workdir", 4095, 1) || !field_string(json, "persona", 127, 0) ||
       !cJSON_IsBool(tools) || integer(json, "max_turns", &turns) ||
       integer(json, "execution_timeout_ms", &timeout) ||
       aimee_ir_request_parse(get(json, "ir"), &request))
      goto done;
   /* Complete admission precedes the deliberately narrower native capability.
    * No unsupported request control is silently discarded by an SDK launch. */
   if (!agent_request_with_tools_supported(&request))
      goto done;
   int remaining = remaining_ms(invocation);
   if (remaining <= 0)
   {
      status = AIMEE_MODULE_STATUS_DEADLINE_EXCEEDED;
      goto done;
   }
   if (timeout <= 0)
      goto done;
   config = calloc(1, sizeof *config);
   agent_t selected = {0};
   char error[512] = "";
   if (!config)
   {
      status = AIMEE_MODULE_STATUS_INTERNAL;
      goto done;
   }
   if (server_native_delegate_source_bind(
           handle, invocation->source_principal_ref, text(json, "operation"), text(json, "role"),
           text(json, "model"), text(json, "workdir"), &request, cJSON_IsTrue(tools), turns,
           timeout, &scope, config, &selected, error, sizeof error))
      goto done;
   const char *persona = text(json, "persona");
   if ((persona && persona[0] && !persona_exists(text(json, "workdir"), persona)) ||
       aimee_ir_request_clone(&request, &effective))
      goto done;
   char query[4096], *owned_system = NULL;
   aimee_ir_last_user_text(&request, query, sizeof query);
   const char *system = delegate_assemble_system_prompt(
       request.n_system ? request.system[0].text : NULL, text(json, "role"), query,
       text(json, "workdir"), persona, text(json, "workdir"), &owned_system);
   char *effective_system = system ? strdup(system) : NULL;
   free(owned_system);
   if (!effective_system)
      goto done;
   if (!effective.n_system)
   {
      effective.system = calloc(1, sizeof *effective.system);
      if (!effective.system)
      {
         free(effective_system);
         goto done;
      }
      effective.n_system = 1;
      effective.system[0].type = AIMEE_BLK_TEXT;
   }
   free(effective.system[0].text);
   effective.system[0].text = effective_system;
   if (aimee_ir_request_admit(&effective) ||
       server_native_delegate_source_admit(scope, persona, &effective, error, sizeof error))
      goto done;
   /* The sealed operation claim binds the requested bound, not a changing bus
    * deadline. Execution can only tighten that admitted request afterward. */
   remaining = remaining_ms(invocation);
   if (remaining <= 0)
   {
      status = AIMEE_MODULE_STATUS_DEADLINE_EXCEEDED;
      goto done;
   }
   if (remaining < timeout)
      timeout = remaining;
   if (selected.timeout_ms <= 0 || timeout < selected.timeout_ms)
      selected.timeout_ms = timeout;
   config->tool_loop_timeout_ms_cap = timeout;
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      goto done;
   int64_t execution_started_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
   config->tool_loop_deadline_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + timeout;
   int rc = agent_run_native_request_for_role(config, &selected, text(json, "role"),
                                              cJSON_IsTrue(tools), &effective, &response, &result);
   const request_context_t *context = request_context_get();
   int complete = rc == 0 && result.success && !aimee_ir_response_complete_valid(&response);
   /* History comes from the actual durable child capture. No reconstruction
    * from result.response or a manufactured assistant message is permitted. */
   if (!result.canonical_session_id[0] || !context || !context->vault_principal[0] ||
       server_native_history_load(result.canonical_session_id, context->vault_principal, NULL, 0,
                                  &history, &receipts))
   {
      status = AIMEE_MODULE_STATUS_INTERNAL;
      goto done;
   }
   reply = cJSON_CreateObject();
   cJSON *canonical = aimee_ir_response_json(&response);
   if (!reply || !canonical || !cJSON_AddItemToObject(reply, "ir", canonical))
   {
      cJSON_Delete(canonical);
      status = AIMEE_MODULE_STATUS_INTERNAL;
      goto done;
   }
   if (!cJSON_AddNumberToObject(reply, "version", 3) ||
       !cJSON_AddStringToObject(reply, "status", complete ? "done" : "failed") ||
       !cJSON_AddStringToObject(reply, "agent", selected.name) ||
       !cJSON_AddBoolToObject(reply, "cost_known", 0) ||
       (!complete &&
        !cJSON_AddStringToObject(reply, "error", "canonical native execution did not complete")) ||
       !cJSON_AddItemToObject(reply, "history", history))
   {
      status = AIMEE_MODULE_STATUS_INTERNAL;
      goto done;
   }
   if (!strcmp(result.stop_reason, "turn_cap") && result.turns > 0 && selected.max_turns > 0)
   {
      cJSON *termination = cJSON_AddObjectToObject(reply, "termination");
      if (!termination || !cJSON_AddStringToObject(termination, "kind", "turn_cap") ||
          !cJSON_AddNumberToObject(termination, "max_turns", selected.max_turns) ||
          !cJSON_AddNumberToObject(termination, "observed_turns", result.turns) ||
          !cJSON_AddNumberToObject(termination, "elapsed_ms",
                                   !clock_gettime(CLOCK_MONOTONIC, &now)
                                       ? (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 -
                                             execution_started_ms
                                       : 0))
      {
         history = NULL;
         status = AIMEE_MODULE_STATUS_INTERNAL;
         goto done;
      }
   }
   history = NULL;
   char *wire = cJSON_PrintUnformatted(reply);
   if (!wire)
   {
      status = AIMEE_MODULE_STATUS_INTERNAL;
      goto done;
   }
   size_t bytes = strlen(wire);
   if (bytes <= capacity)
   {
      memcpy(output, wire, bytes);
      *output_length = (uint32_t)bytes;
      status = AIMEE_MODULE_STATUS_OK;
   }
   else
      status = AIMEE_MODULE_STATUS_INTERNAL;
   cJSON_free(wire);
done:
   server_native_delegate_source_unbind(scope);
   free(config);
   cJSON_Delete(json);
   cJSON_Delete(history);
   cJSON_Delete(receipts);
   cJSON_Delete(reply);
   aimee_request_free(&request);
   aimee_request_free(&effective);
   aimee_response_free(&response);
   agent_result_clear_response(&result);
   return status;
}

/* One borrowed AMOD cancellation boundary covers source queries/issuance,
 * workspace binding, host composition and native execution alike. */
aimee_module_status_t server_native_delegate_handle(const aimee_module_invocation_t *invocation,
                                                    const uint8_t *body, uint32_t length,
                                                    uint8_t *output, uint32_t capacity,
                                                    uint32_t *output_length, void *context)
{
   agent_request_cancel_scope_t cancel = {.flag = aimee_module_invocation_cancel_flag(invocation),
                                          .parent = agent_request_cancel_scope_current()};
   const agent_request_cancel_scope_t *previous = agent_request_cancel_scope_bind(&cancel);
   aimee_module_status_t status = native_delegate_handle_impl(invocation, body, length, output,
                                                              capacity, output_length, context);
   agent_request_cancel_scope_bind(previous);
   return status;
}

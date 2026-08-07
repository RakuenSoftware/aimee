/* workflow_control_bus.c: the workflow control plane over the event bus.
 *
 * This replaced a private AF_UNIX socket that spoke HTTP to a Go listener
 * (wfe_http_proxy.c) -- a second transport doing what the bus already does,
 * with its own framing, timeouts and failure taxonomy. It is the same move
 * roundtable.review made, and the last one: with this gone, the C resource
 * plane reaches Go only over the bus.
 *
 * The engine did not move. The process that owns the workflow store, the
 * artifact store and the scheduler still owns them and still serves the same
 * mux; what changed is that reaching it no longer needs a socket of its own.
 * Correlation, AMOD framing, monotonic deadlines and cancellation belong to
 * aimee-core-c now.
 *
 * What stays here is the request SHAPING the proxy did -- method, path, query,
 * body and the two identity headers -- because that is contract, not transport.
 */
#include "workflow_control_bus.h"

#include "cJSON.h"

#include "headers/module_json_call.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The proxy allowed a 16 MiB response; the module body limit is the same. */
#define WORKFLOW_CONTROL_MAX_BODY AIMEE_MODULE_MESSAGE_MAX_BODY

/* The private socket used a 30s I/O timeout. Keeping it means a hung control
 * plane still fails in the time operators already expect. */
#define WORKFLOW_CONTROL_TIMEOUT_MS 30000

static int control_error(char *resp, int cap, int status, const char *message)
{
   if (resp && cap > 0)
      snprintf(resp, (size_t)cap, "{\"error\":\"%s\"}", message);
   return status;
}

static char *build_control_body(const char *method, const char *path, const char *query,
                                const char *body, int body_len, const char *principal,
                                int workflow_operator)
{
   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return NULL;
   cJSON_AddStringToObject(payload, "method", method);
   cJSON_AddStringToObject(payload, "path", path);
   cJSON_AddStringToObject(payload, "query", query ? query : "");
   /* The body is not NUL-terminated at the call site, and this cJSON has no
    * length-aware string constructor, so it is copied and terminated here. */
   if (body && body_len > 0)
   {
      char *terminated = malloc((size_t)body_len + 1);
      if (!terminated)
      {
         cJSON_Delete(payload);
         return NULL;
      }
      memcpy(terminated, body, (size_t)body_len);
      terminated[body_len] = '\0';
      cJSON *body_item = cJSON_CreateString(terminated);
      free(terminated);
      if (!body_item)
      {
         cJSON_Delete(payload);
         return NULL;
      }
      cJSON_AddItemToObject(payload, "body", body_item);
   }
   else
      cJSON_AddStringToObject(payload, "body", "");
   cJSON_AddStringToObject(payload, "principal", principal ? principal : "");
   cJSON_AddBoolToObject(payload, "workflow_operator", workflow_operator ? 1 : 0);

   char *wire = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   return wire;
}

int workflow_control_request(const char *method, const char *path, const char *query,
                             const char *body, int body_len, const char *principal,
                             int workflow_operator, char *resp, int resp_cap)
{
   if (!method || !path || !resp || resp_cap <= 0 || body_len < 0)
      return control_error(resp, resp_cap, 400, "invalid workflow control request");
   if (!obs_bus_module_available(AIMEE_WORKFLOWS_EVENT_CONTROL))
      return control_error(resp, resp_cap, 503,
                           "workflow control module is not attached to the event bus");
   if (!body)
      body_len = 0;

   char *wire =
       build_control_body(method, path, query, body, body_len, principal, workflow_operator);
   if (!wire)
      return control_error(resp, resp_cap, 500, "out of memory building workflow request");
   size_t wire_len = strlen(wire);
   if (wire_len > WORKFLOW_CONTROL_MAX_BODY)
   {
      free(wire);
      return control_error(resp, resp_cap, 413, "workflow request exceeds the module body limit");
   }

   aimee_module_call_result_t result = AIMEE_MODULE_CALL_OK;
   cJSON *parsed = aimee_module_json_call_raw(
       AIMEE_WORKFLOWS_EVENT_CONTROL, AIMEE_WORKFLOWS_STAGE_CONTROL, wire, wire_len,
       WORKFLOW_CONTROL_MAX_BODY, WORKFLOW_CONTROL_TIMEOUT_MS, &result);
   free(wire);

   if (result != AIMEE_MODULE_CALL_OK)
   {
      /* Name the outcome. Every proxy failure rendered as one of a handful of
       * socket messages, so a control call that ran its full deadline looked
       * exactly like a plane that was never started. */
      char reason[160];
      snprintf(reason, sizeof(reason), "workflow control failed: %s",
               aimee_module_call_result_name(result));
      return control_error(resp, resp_cap, 502, reason);
   }

   if (!parsed)
      return control_error(resp, resp_cap, 502, "workflow control returned an unparseable result");
   const cJSON *status = cJSON_GetObjectItemCaseSensitive(parsed, "status");
   const cJSON *reply = cJSON_GetObjectItemCaseSensitive(parsed, "body");
   if (!cJSON_IsNumber(status) || !cJSON_IsString(reply))
   {
      cJSON_Delete(parsed);
      return control_error(resp, resp_cap, 502, "workflow control returned an invalid result");
   }
   size_t reply_len = strlen(reply->valuestring);
   if (reply_len >= (size_t)resp_cap)
   {
      cJSON_Delete(parsed);
      return control_error(resp, resp_cap, 502, "workflow response exceeds the public API limit");
   }
   memcpy(resp, reply->valuestring, reply_len + 1);
   int http_status = status->valueint;
   cJSON_Delete(parsed);
   return http_status;
}

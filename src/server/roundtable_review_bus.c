/* roundtable_review_bus.c: roundtable.review over the event bus.
 *
 * This replaced a private AF_UNIX socket that spoke HTTP to a Go listener -- a
 * second transport doing what the bus already does, with its own framing,
 * timeouts and failure taxonomy. Everything around it had already migrated:
 * tools, skills, governance, workflows, response-composition, and roundtable's
 * own deliberate stage.
 *
 * The cost of keeping a private transport was not theoretical. The C side and
 * the Go side ended up with separate notions of the same panel settings,
 * reconciled nowhere, so a chair-synthesis guard added in C had no effect on
 * reviews at all.
 *
 * Correlation, AMOD framing, monotonic deadlines, cancellation and response
 * validation all belong to aimee-core-c now (obs_bus_module_call). What stays
 * here is the request SHAPING the proxy did -- artifact/prompt aliasing, brief
 * flattening, preset resolution, and keeping a caller's run_id distinct from the
 * transport's __run_id -- because that is contract, not transport.
 */
#include "roundtable_review_bus.h"

#include "cJSON.h"
#include "config.h"
#include "delegate_ensemble.h" /* ensemble_panel_from_config */
#include "roundtable_preset.h"
#include "util.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_protocol.h>
#include <aimee/roundtable/module_api.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Matches AIMEE_MODULE_MESSAGE_MAX_BODY and Go's MaxArtifactBytes. A review of a
 * 16 MiB artifact is the largest thing this carries. */
#define ROUNDTABLE_REVIEW_MAX_BODY AIMEE_MODULE_MESSAGE_MAX_BODY

static uint64_t monotonic_deadline_ns(int timeout_ms)
{
   if (timeout_ms <= 0)
      return 0; /* no deadline */
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec +
          (uint64_t)timeout_ms * 1000000ull;
}

/* Resolve the C-configured default panel so the MCP schema's documented default
 * can reach Go, which deliberately requires a named saved panel. */
static void resolve_default_panel(const cJSON *request, char *out, size_t out_len, int *timeout_ms)
{
   out[0] = '\0';
   ensemble_panel_t panel;
   ensemble_panel_from_config(&panel);
   const cJSON *preset = cJSON_GetObjectItemCaseSensitive(request, "roundtable");
   const char *requested = cJSON_IsString(preset) ? preset->valuestring : NULL;
   if (roundtable_preset_resolve_runtime(requested, &panel, out, out_len, NULL, 0) > 0)
   {
      roundtable_preset_t acquired;
      int chairman = roundtable_preset_load(out, &acquired) == 0 ? acquired.chairman_enabled : 0;
      *timeout_ms = roundtable_review_deadline_ms(panel.deadline_ms, chairman);
   }
   else
      *timeout_ms = roundtable_review_deadline_ms(0, 0);
}

/* Build the review body. Identical JSON to what the HTTP route carried: the
 * transport changed, the contract did not. */
static char *build_review_body(const cJSON *request, const char *resolved_panel)
{
   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return NULL;
   const cJSON *artifact = cJSON_GetObjectItemCaseSensitive(request, "artifact");
   const cJSON *prompt = cJSON_GetObjectItemCaseSensitive(request, "prompt");
   const cJSON *brief = cJSON_GetObjectItemCaseSensitive(request, "brief");
   const cJSON *preset = cJSON_GetObjectItemCaseSensitive(request, "roundtable");
   const cJSON *requested_run_id = cJSON_GetObjectItemCaseSensitive(request, "run_id");
   const cJSON *transport_run_id = cJSON_GetObjectItemCaseSensitive(request, "__run_id");
   const cJSON *original = cJSON_GetObjectItemCaseSensitive(request, "original_request");
   const cJSON *stage = cJSON_GetObjectItemCaseSensitive(request, "artifact_stage");
   const cJSON *requested_workdir = cJSON_GetObjectItemCaseSensitive(request, "workdir");

   const char *artifact_text = cJSON_IsString(artifact)
                                   ? artifact->valuestring
                                   : (cJSON_IsString(prompt) ? prompt->valuestring : NULL);
   if (!artifact_text)
   {
      cJSON_Delete(payload);
      return NULL;
   }
   cJSON_AddStringToObject(payload, "artifact", artifact_text);
   cJSON_AddStringToObject(payload, "artifact_stage",
                           cJSON_IsString(stage) ? stage->valuestring : "frozen_diff");
   if (cJSON_IsString(original))
      cJSON_AddStringToObject(payload, "original_request", original->valuestring);
   else if (cJSON_IsString(brief))
      cJSON_AddStringToObject(payload, "original_request", brief->valuestring);
   else if (cJSON_IsObject(brief))
   {
      char *text = cJSON_PrintUnformatted(brief);
      if (text)
      {
         cJSON_AddStringToObject(payload, "original_request", text);
         free(text);
      }
   }
   if (cJSON_IsString(preset))
      cJSON_AddStringToObject(payload, "roundtable", preset->valuestring);
   else if (resolved_panel && resolved_panel[0])
      cJSON_AddStringToObject(payload, "roundtable", resolved_panel);
   /* __run_id owns the asynchronous transport job. A caller-supplied run_id is
    * the review identity and must survive that wrapper unchanged. */
   if (cJSON_IsString(requested_run_id) && requested_run_id->valuestring[0])
      cJSON_AddStringToObject(payload, "run_id", requested_run_id->valuestring);
   else if (cJSON_IsString(transport_run_id))
      cJSON_AddStringToObject(payload, "run_id", transport_run_id->valuestring);
   const char *cwd =
       cJSON_IsString(requested_workdir) ? requested_workdir->valuestring : run_cmd_get_cwd();
   if (cwd && cwd[0])
      cJSON_AddStringToObject(payload, "workdir", cwd);

   char *wire = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   return wire;
}

int handle_roundtable_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *request)
{
   (void)ctx;
   if (!obs_bus_module_available(AIMEE_ROUNDTABLE_EVENT_REVIEW))
      return server_send_error(
          conn, "roundtable review module is not attached to the event bus", NULL);

   char resolved[RT_PRESET_NAME_MAX] = "";
   int timeout_ms = 0;
   resolve_default_panel(request, resolved, sizeof(resolved), &timeout_ms);

   char *wire = build_review_body(request, resolved);
   if (!wire)
      return server_send_error(conn, "roundtable artifact is required", NULL);
   size_t wire_len = strlen(wire);
   if (wire_len > ROUNDTABLE_REVIEW_MAX_BODY)
   {
      free(wire);
      return server_send_error(conn, "roundtable request exceeds the module body limit", NULL);
   }

   char *response = malloc(ROUNDTABLE_REVIEW_MAX_BODY);
   if (!response)
   {
      free(wire);
      return server_send_error(conn, "out of memory", NULL);
   }
   uint32_t response_len = 0;
   aimee_module_call_result_t result = obs_bus_module_call(
       AIMEE_ROUNDTABLE_EVENT_REVIEW, AIMEE_ROUNDTABLE_STAGE_REVIEW, 0,
       monotonic_deadline_ns(timeout_ms), wire, (uint32_t)wire_len, response,
       ROUNDTABLE_REVIEW_MAX_BODY, &response_len, NULL, NULL);
   free(wire);

   if (result != AIMEE_MODULE_CALL_OK)
   {
      /* Name the outcome. Every failure used to render as "Go roundtable service
       * is unreachable", so a review that ran its full deadline looked exactly
       * like a service that was never started. */
      char reason[160];
      snprintf(reason, sizeof(reason), "roundtable review failed: %s",
               aimee_module_call_result_name(result));
      free(response);
      return server_send_error(conn, reason, NULL);
   }

   cJSON *parsed = cJSON_ParseWithLength(response, response_len);
   free(response);
   if (!parsed)
      return server_send_error(conn, "roundtable review returned an unparseable result", NULL);
   return server_send_ok(conn, parsed);
}

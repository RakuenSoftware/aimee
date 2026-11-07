/* One canonical provider-step executor shared by the HTTP frontends. Native
 * body construction/transport/admission are one external boundary; intermediate
 * policy, reduction, persistence and cache hooks use Request/Response IR. */
#include "server_model_execution.h"
#include "agent_request_build.h"
#include "agent_config.h"
#include "config.h"
#include "gateway_mutate_wire.h"
#include "server_http_identity.h"
#include "server_responses_stream.h"
#include "wire_fence.h"
#include "http_retry.h"
#include "gw_stage_governance.h"
#include <aimee/gateway/gateway_policy.h>
#include "gw_stage_completion.h"
#include "agent_response_projection.h"
#include <aimee/delegates/delegate_driver.h>
#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int server_model_governance_enabled(void)
{
   int tri = config_present() ? config_module_governance() : -1;
   return config_module_enabled(tri, gw_response_governance_enabled());
}

static int server_model_post_ir(const agent_t *agent, aimee_request_t *request, aimee_wire_t wire,
                                const char *url, const char *auth, const char *headers,
                                char **reply, aimee_anthropic_replay_t *replay, char *error,
                                size_t capacity, const server_model_execution_options_t *options,
                                server_responses_stream_t *stream,
                                aimee_response_t *streamed_response)
{
   /* Completion repair may append a governed tool call after the answer. Its
    * existing owner requires buffering before any response is projected. */
   int incremental = stream && options->stream_post && wire == AIMEE_WIRE_RESPONSES &&
                     !server_model_governance_enabled() && !gateway_prevent_subagents_enabled() &&
                     !gw_ir_response_completion_armed(request);
   if (incremental)
      request->stream = 1;
   char *body = agent_render_gateway_request_ir(agent, request, wire);
   if (!body)
   {
      snprintf(error, capacity, "the selected provider cannot represent the canonical request");
      return SERVER_MODEL_RENDER_REFUSED;
   }
   wire_fence_route_t route = wire == AIMEE_WIRE_ANTHROPIC   ? WIRE_FENCE_ANTHROPIC_MESSAGES
                              : wire == AIMEE_WIRE_RESPONSES ? WIRE_FENCE_OPENAI_RESPONSES
                                                             : WIRE_FENCE_OPENAI_CHAT;
   wire_fence_t *snapshot = NULL;
   wire_fence_bytes_t bytes;
   if (wire_fence_select(econ_mode_current() != ECON_MODE_OFF, route, body, strlen(body), &snapshot,
                         &bytes))
   {
      snprintf(error, capacity, "%s", wire_fence_last_error());
      free(body);
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   if (wire == AIMEE_WIRE_ANTHROPIC &&
       aimee_anthropic_replay_prepare(bytes.data, bytes.len, replay))
   {
      wire_fence_destroy(snapshot);
      free(body);
      snprintf(error, capacity, "cannot bind native replay prefix");
      return SERVER_MODEL_RENDER_REFUSED;
   }
   int attempts = options && options->retry_attempts > 0 ? options->retry_attempts : 1;
   int base = options ? options->retry_base_ms : 0;
   int maximum = options ? options->retry_max_ms : 0;
   int status = incremental ? options->stream_post(stream, url, auth, bytes.data, bytes.len,
                                                   headers, agent->provider, agent->model,
                                                   agent->timeout_ms, streamed_response)
                            : wire_fence_post(url, auth, bytes.data, bytes.len, reply,
                                              agent->timeout_ms, headers, attempts, base, maximum,
                                              agent->provider, agent->model, NULL, route);
   wire_fence_destroy(snapshot);
   free(body);
   if (status == HTTP_RETRY_ADMISSION_REFUSED)
      snprintf(error, capacity, "%s", wire_fence_last_error());
   return status;
}

int server_model_execute_prepared(const agent_t *agent, aimee_request_t *request,
                                  aimee_response_t *out, char *error, size_t capacity,
                                  const server_model_execution_options_t *options)
{
   server_responses_stream_t *stream = options ? options->stream : NULL;
   int preserve_messages = options && options->preserve_messages;
   memset(out, 0, sizeof *out);
   snprintf(error, capacity, "canonical provider request failed");
   if (!agent || aimee_ir_request_admit(request))
      return -1;
   delegate_drivers_init();
   const delegate_driver_t *driver = delegate_driver_get(agent->provider);
   char url[MAX_ENDPOINT_LEN + 64], auth[MAX_API_KEY_LEN + 32], headers[2048];
   if (!driver || delegate_build_url(driver, agent, url, sizeof url))
   {
      snprintf(error, capacity, "upstream model provider or endpoint is unavailable");
      return SERVER_MODEL_CONNECTION_UNAVAILABLE;
   }
   if (agent_resolve_auth_headers(agent, auth, sizeof auth, headers, sizeof headers))
   {
      snprintf(error, capacity, "upstream model credentials are unavailable");
      return SERVER_MODEL_CONNECTION_UNAVAILABLE;
   }
   aimee_wire_t wire = (driver->name && !strcmp(driver->name, "anthropic")) ? AIMEE_WIRE_ANTHROPIC
                       : strstr(url, "/responses")                          ? AIMEE_WIRE_RESPONSES
                                                   : AIMEE_WIRE_OPENAI_CHAT;
   if (options && options->connection &&
       options->connection(agent, wire, url, sizeof url, headers, sizeof headers))
   {
      snprintf(error, capacity, "upstream model transport metadata is unavailable");
      return SERVER_MODEL_CONNECTION_UNAVAILABLE;
   }
   if (options && options->responses_stream)
      request->stream = wire == AIMEE_WIRE_RESPONSES;
   gw_mutate_ctx_t mutation;
   gw_mutate_ctx_init(&mutation);
   cJSON *container = NULL;
   if (!preserve_messages && gw_mutate_upstream_ok(wire == AIMEE_WIRE_ANTHROPIC))
   {
      container = cJSON_CreateObject();
      cJSON *history = aimee_ir_request_history_json(request);
      if (!container || !history || !cJSON_AddItemToObject(container, "messages", history))
      {
         cJSON_Delete(history);
         cJSON_Delete(container);
         return SERVER_MODEL_HISTORY_REFUSED;
      }
      gw_buffered_mutate(container, "messages", request->model, "",
                         server_http_identity_session_hdr(), server_http_identity_bearer(),
                         server_http_identity_principal(), &mutation);
      if (mutation.mutated && aimee_ir_request_history_replace(
                                  request, cJSON_GetObjectItemCaseSensitive(container, "messages")))
         goto refused;
   }
   if (options && options->admitted)
      options->admitted(request);
   if (options && options->cache_read)
   {
      int cached =
          options->cache_read(agent, request, out, error, capacity, options->cache_context);
      if (cached)
      {
         cJSON_Delete(container);
         gw_mutate_ctx_free(&mutation);
         return cached > 0 ? 0 : -1;
      }
   }
   char *reply = NULL;
   aimee_anthropic_replay_t replay = {0};
   int status = server_model_post_ir(agent, request, wire, url, auth, headers, &reply, &replay,
                                     error, capacity, options, stream, out);
   if (!(stream && stream->used) && status != HTTP_RETRY_ADMISSION_REFUSED &&
       status != SERVER_MODEL_RENDER_REFUSED && container &&
       gw_buffered_after_status(container, "messages", status, &mutation) == GW_POST_RESEND)
   {
      if (options && options->cache_invalidate)
         options->cache_invalidate(options->cache_context);
      free(reply);
      reply = NULL;
      if (aimee_ir_request_history_replace(request,
                                           cJSON_GetObjectItemCaseSensitive(container, "messages")))
         goto refused;
      status = server_model_post_ir(agent, request, wire, url, auth, headers, &reply, &replay,
                                    error, capacity, options, stream, out);
   }
   cJSON_Delete(container);
   gw_mutate_ctx_free(&mutation);
   if (status != 200 || (!reply && !(stream && stream->used)))
   {
      if (status != HTTP_RETRY_ADMISSION_REFUSED && status != SERVER_MODEL_RENDER_REFUSED)
      {
         if (options && options->upstream_error)
            options->upstream_error(status, reply, error, capacity);
         else
            snprintf(error, capacity, "primary provider call failed with status %d", status);
      }
      if (options && options->native_error && status != 200 &&
          status != HTTP_RETRY_ADMISSION_REFUSED && status != SERVER_MODEL_RENDER_REFUSED)
         options->native_error(status, reply, options->error_context);
      free(reply);
      return status == HTTP_RETRY_ADMISSION_REFUSED || status == SERVER_MODEL_RENDER_REFUSED
                 ? status
                 : -1;
   }
   int rc = stream && stream->used
                ? 0
                : aimee_backend_parse_wire(reply, strlen(reply), wire, out, error, capacity);
   free(reply);
   if (!rc && wire == AIMEE_WIRE_ANTHROPIC && aimee_anthropic_bind_response(&replay, out))
   {
      aimee_ir_session_fail_capture();
      rc = -1;
      snprintf(error, capacity, "cannot retain native replay binding");
   }
   if (rc)
      return -1;
   if (!(stream && stream->used) &&
       (!out->error_message || (options && options->capture_error_observations)) &&
       aimee_ir_session_emit_buffered_response(out, NULL))
   {
      snprintf(error, capacity, "canonical response persistence failed");
      return -1;
   }
   if (options && options->response_projection && options->response_projection(out))
      return -1;
   if (out->error_message)
   {
      snprintf(error, capacity, "%s", out->error_message);
      return -1;
   }
   rc = agent_response_govern(out, server_model_governance_enabled(),
                              gateway_prevent_subagents_enabled(), NULL, 0);
   if (rc >= 0 && !preserve_messages)
      rc = gw_ir_response_run_completion(out, request);
   if (rc < 0)
   {
      snprintf(error, capacity, "canonical response policy or persistence failed");
      return -1;
   }
   if (stream && stream->used &&
       (aimee_responses_frontend_matches(stream->frontend, out) ||
        aimee_ir_session_emit_response(out, stream->message_id)))
   {
      snprintf(error, capacity, "canonical streamed response policy or persistence failed");
      return -1;
   }
   if (options && options->cache_write)
      options->cache_write(agent, out, options->cache_context);
   return 0;
refused:
   cJSON_Delete(container);
   gw_mutate_ctx_free(&mutation);
   snprintf(error, capacity, "canonical economizer history is invalid");
   return SERVER_MODEL_HISTORY_REFUSED;
}

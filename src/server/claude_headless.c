/* Claude print-mode process lifecycle. Native frames terminate in translation;
 * callers own Vault materialization, resume metadata and durable IR delivery. */
#include "claude_headless.h"
#include "cli_claude_native.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "server_session_control.h"
#include <aimee/translation/harness_ir.h>
#include <aimee/translation/aimee_backend.h>
#include "cJSON.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static uint64_t mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static char *claude_input_line(cJSON *frame)
{
   char *json = frame ? cJSON_PrintUnformatted(frame) : NULL;
   cJSON_Delete(frame);
   if (!json)
      return NULL;
   size_t n = strlen(json);
   char *line = realloc(json, n + 2);
   if (!line)
   {
      free(json);
      return NULL;
   }
   line[n] = '\n';
   line[n + 1] = 0;
   return line;
}
static char *claude_initialize_line(void)
{
   cJSON *frame = cJSON_CreateObject();
   if (!frame)
      return NULL;
   cJSON_AddStringToObject(frame, "type", "control_request");
   cJSON_AddStringToObject(frame, "request_id", "aimee:init");
   cJSON *request = cJSON_AddObjectToObject(frame, "request");
   cJSON_AddStringToObject(request, "subtype", "initialize");
   cJSON_AddObjectToObject(request, "hooks");
   return claude_input_line(frame);
}
/* Readiness is transport scheduling only. Status/config/error metadata never
 * enters IR; the later actual system/init frame still proves the full profile. */
static char *claude_mcp_status_line(const char *id)
{
   cJSON *frame = cJSON_CreateObject();
   if (!frame)
      return NULL;
   cJSON_AddStringToObject(frame, "type", "control_request");
   cJSON_AddStringToObject(frame, "request_id", id);
   cJSON *request = cJSON_AddObjectToObject(frame, "request");
   cJSON_AddStringToObject(request, "subtype", "mcp_status");
   return claude_input_line(frame);
}
static int claude_mcp_ready(const cli_claude_host_launch_t *host, const cJSON *control)
{
   const cJSON *response = cJSON_GetObjectItemCaseSensitive(control, "response");
   const cJSON *servers = cJSON_GetObjectItemCaseSensitive(response, "mcpServers");
   if (!host || !host->catalog || !cJSON_IsObject(response) || !cJSON_IsArray(servers) ||
       cJSON_GetArraySize(servers) != (host->catalog->n_tools ? 1 : 0))
      return -1;
   if (!host->catalog->n_tools)
      return 1;
   const cJSON *server = cJSON_GetArrayItem(servers, 0);
   const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(server, "name"));
   const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(server, "status"));
   if (!cJSON_IsObject(server) || !name || strcmp(name, "aimee") || !status)
      return -1;
   if (!strcmp(status, "connected"))
      return 1;
   return !strcmp(status, "pending") ? 0 : -1;
}
static char *claude_user_line(const char *prompt, const char *resume)
{
   cJSON *frame = cJSON_CreateObject();
   if (!frame)
      return NULL;
   cJSON_AddStringToObject(frame, "type", "user");
   cJSON_AddStringToObject(frame, "session_id", resume ? resume : "");
   cJSON_AddNullToObject(frame, "parent_tool_use_id");
   cJSON *message = cJSON_AddObjectToObject(frame, "message");
   cJSON_AddStringToObject(message, "role", "user");
   cJSON_AddStringToObject(message, "content", prompt);
   return claude_input_line(frame);
}
int claude_headless_run(const provider_cli_cfg_t *cfg, claude_headless_session_fn bind_session,
                        void *context, agent_result_t *out)
{
   if (!cfg || !cfg->agent || !cfg->user_prompt || !cfg->user_prompt[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   uint64_t execution_started = mono_ms();
   const agent_t *agent = cfg->agent;
   const char *prompt = cfg->user_prompt;
   char bound[1025] = "";
   int in = -1, output = -1, status = 0, rc = -1, saw_result = 0, interrupted = 0, eof = 0,
       capture_failed = 0;
   pid_t pid = -1;
   aimee_harness_ir_t ir = {.host_model_capture = cfg->claude_host != NULL};
   int profile_seen = 0;
   char *input_line = NULL;
   int initialized = !cfg->control_channel, prompt_sent = !cfg->control_channel;
   int native_readiness = cfg->control_channel && cfg->claude_host;
   int mcp_ready =
       !native_readiness || (cfg->claude_host->catalog && !cfg->claude_host->catalog->n_tools);
   int mcp_waiting = 0;
   unsigned mcp_request = 0;
   char mcp_id[64] = "";
   uint64_t mcp_next = 0;
   server_control_scope_t *controls = server_control_scope_current();
   if (cfg->control_channel && !controls)
   {
      snprintf(out->error, sizeof out->error, "browser control owner is unavailable");
      goto finish;
   }
   if (claude_provider_cli_adapter.spawn(cfg, prompt, &in, &output, &pid) != 0)
   {
      snprintf(out->error, sizeof(out->error), "could not start claude -p");
      goto finish;
   }
   fcntl(in, F_SETFL, fcntl(in, F_GETFL, 0) | O_NONBLOCK);
   fcntl(output, F_SETFL, fcntl(output, F_GETFL, 0) | O_NONBLOCK);
   input_line = cfg->control_channel ? claude_initialize_line() : strdup(prompt);
   if (!input_line)
      goto stop;
   size_t sent = 0, length = strlen(input_line), used = 0;
   char *buffer = malloc(4 * 1024 * 1024 + 1);
   if (!buffer)
      goto stop;
   uint64_t last = mono_ms(), started = last;
   while (!eof)
   {
      if (cfg->claude_host && cfg->claude_host->progress &&
          cfg->claude_host->progress(cfg->claude_host->progress_context))
         goto capture_stop;
      if (agent_request_cancelled())
      {
         interrupted = 1;
         break;
      }
      if (cfg->control_channel && (!initialized || !mcp_ready) && mono_ms() - started > 60000)
      {
         snprintf(out->error, sizeof out->error, "Claude control initialization timed out");
         break;
      }
      if (cfg->control_channel && initialized && !input_line && in >= 0)
      {
         if (!mcp_ready)
         {
            if (!mcp_waiting && mono_ms() >= mcp_next)
            {
               snprintf(mcp_id, sizeof mcp_id, "aimee:mcp:%u", ++mcp_request);
               input_line = claude_mcp_status_line(mcp_id);
               if (!input_line)
                  goto capture_stop;
               mcp_waiting = 1;
            }
         }
         else if (!prompt_sent)
         {
            input_line = claude_user_line(prompt, cfg->resume_id);
            prompt_sent = 1;
            if (!input_line)
               goto capture_stop;
         }
         else
         {
            char *id = NULL, *kind = NULL;
            cJSON *request = NULL, *reply = NULL;
            int taken = server_control_take(controls, NULL, &id, &kind, &request, &reply);
            if (taken < 0)
               goto capture_stop;
            if (taken == 1)
            {
               cJSON *frame = aimee_claude_control_reply(id, request, reply);
               int canonical = frame != NULL;
               if (!frame)
               {
                  frame = cJSON_CreateObject();
                  cJSON_AddStringToObject(frame, "type", "control_response");
                  cJSON *error = cJSON_AddObjectToObject(frame, "response");
                  cJSON_AddStringToObject(error, "subtype", "error");
                  cJSON_AddStringToObject(error, "request_id", id);
                  cJSON_AddStringToObject(error, "error",
                                          "Operator reply did not satisfy this request");
               }
               int published = !frame      ? -1
                               : canonical ? aimee_harness_control_decision_ir(&ir, id, reply)
                                           : aimee_claude_frame_ir(frame, &ir);
               free(id);
               free(kind);
               cJSON_Delete(request);
               cJSON_Delete(reply);
               if (published != 0)
               {
                  cJSON_Delete(frame);
                  goto capture_stop;
               }
               input_line = claude_input_line(frame);
               if (!input_line)
                  goto capture_stop;
            }
         }
         if (input_line)
         {
            sent = 0;
            length = strlen(input_line);
         }
      }
      int idle = agent->cli_idle_timeout_ms > 0 ? agent->cli_idle_timeout_ms
                 : agent->timeout_ms > 0        ? agent->timeout_ms
                                                : 30 * 60 * 1000;
      idle = agent_timeout_cap_ms(idle, agent->tool_loop_timeout_ms_cap);
      if (mono_ms() - last > (uint64_t)idle)
      {
         snprintf(out->error, sizeof(out->error), "Claude structured stream timed out after %d ms",
                  idle);
         break;
      }
      struct pollfd fds[2] = {{.fd = output, .events = POLLIN},
                              {.fd = in, .events = input_line ? POLLOUT : 0}};
      if (poll(fds, 2, 100) < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (in >= 0 && (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)))
         break;
      if (in >= 0 && (fds[1].revents & POLLOUT))
      {
         ssize_t n = write(in, input_line + sent, length - sent);
         if (n > 0)
            sent += (size_t)n;
         else if (n < 0 && errno != EAGAIN && errno != EINTR)
            break;
         if (sent == length)
         {
            free(input_line);
            input_line = NULL;
            if (!cfg->control_channel)
            {
               close(in);
               in = -1;
            }
         }
      }
      if (fds[0].revents & (POLLIN | POLLHUP))
      {
         if (used == 4 * 1024 * 1024)
         {
            snprintf(out->error, sizeof(out->error), "Claude frame exceeds capture limit");
            break;
         }
         ssize_t n = read(output, buffer + used, 4 * 1024 * 1024 - used);
         if (n == 0)
            eof = 1;
         else if (n < 0)
         {
            if (errno == EAGAIN || errno == EINTR)
               continue;
            break;
         }
         else
         {
            used += (size_t)n;
            last = mono_ms();
         }
         buffer[used] = '\0';
         char *line = buffer, *end = NULL;
         while ((end = memchr(line, '\n', used - (size_t)(line - buffer))) ||
                (eof && (size_t)(line - buffer) < used))
         {
            size_t frame_length = end ? (size_t)(end - line) : used - (size_t)(line - buffer);
            /* Claude also writes diagnostic lines to stdout. They never enter
             * history. An object frame, however, must pass exact admission;
             * neither C-string truncation nor a partial parse can accept it. */
            const char *first = line;
            while (first < line + frame_length && strchr(" \t\r", *first))
               first++;
            if (first == line + frame_length || *first != '{')
            {
               line = end ? end + 1 : buffer + used;
               continue;
            }
            cJSON *frame = aimee_ir_json_parse_frame(line, frame_length);
            if (!cJSON_IsObject(frame))
            {
               cJSON_Delete(frame);
               snprintf(out->error, sizeof out->error, "invalid Claude JSON frame");
               goto capture_stop;
            }
            if (cJSON_IsObject(frame))
            {
               const cJSON *frame_type = cJSON_GetObjectItemCaseSensitive(frame, "type");
               const cJSON *control = cJSON_GetObjectItemCaseSensitive(frame, "response");
               const char *control_id =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(control, "request_id"));
               if (native_readiness && cJSON_IsString(frame_type) &&
                   !strcmp(frame_type->valuestring, "control_response") &&
                   (mcp_waiting || (control_id && !strncmp(control_id, "aimee:mcp:", 10)) ||
                    (initialized && !prompt_sent) ||
                    (!initialized && (!control_id || strcmp(control_id, "aimee:init")))))
               {
                  const char *subtype =
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(control, "subtype"));
                  int ready = -1;
                  if (initialized && mcp_waiting && control_id && !strcmp(control_id, mcp_id) &&
                      subtype && !strcmp(subtype, "success"))
                     ready = claude_mcp_ready(cfg->claude_host, control);
                  cJSON_Delete(frame);
                  if (ready < 0)
                  {
                     snprintf(out->error, sizeof out->error,
                              "Claude MCP readiness response was refused");
                     goto capture_stop;
                  }
                  mcp_waiting = 0;
                  mcp_ready = ready;
                  mcp_next = mono_ms() + 250;
                  goto next_line;
               }
               if (native_readiness && !prompt_sent && cJSON_IsString(frame_type) &&
                   !strcmp(frame_type->valuestring, "control_request"))
               {
                  cJSON_Delete(frame);
                  goto capture_stop;
               }
               if (cfg->control_channel && cJSON_IsString(frame_type) &&
                   !strcmp(frame_type->valuestring, "control_response") && control_id &&
                   !strcmp(control_id, "aimee:init"))
               {
                  const char *subtype =
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(control, "subtype"));
                  if (initialized || !subtype || strcmp(subtype, "success"))
                  {
                     cJSON_Delete(frame);
                     goto capture_stop;
                  }
                  initialized = 1;
                  if (native_readiness)
                  {
                     cJSON_Delete(frame);
                     goto next_line;
                  }
               }
               if (cfg->claude_host && cJSON_IsString(frame_type))
               {
                  const char *subtype =
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "subtype"));
                  if (!strcmp(frame_type->valuestring, "system") && subtype &&
                      !strcmp(subtype, "init"))
                  {
                     if (native_readiness && !prompt_sent)
                     {
                        cJSON_Delete(frame);
                        snprintf(out->error, sizeof out->error,
                                 "Claude native initialization preceded readiness");
                        goto capture_stop;
                     }
                     const char *reason =
                         profile_seen ? "duplicate initialization"
                                      : cli_claude_native_profile_error(cfg->claude_host, frame);
                     if (!reason && !cfg->claude_host->init)
                        reason = "missing host callback";
                     if (!reason && cfg->claude_host->init(frame, cfg->claude_host->context))
                        reason = "host callback";
                     if (reason)
                     {
                        cli_claude_profile_summary_t summary;
                        cli_claude_native_profile_summary(cfg->claude_host, frame, &summary);
                        aimee_log(LOG_ERROR, "native.claude",
                                  "init profile mismatch: expected_tools=%d reported_tools=%d "
                                  "mcp_servers=%d aimee_connected=%d aimee_status=%s plugins=%d "
                                  "missing_owned_sha256=%s unexpected_reported_sha256=%s",
                                  summary.expected_tools, summary.reported_tools,
                                  summary.mcp_servers, summary.aimee_connected,
                                  cli_claude_mcp_status_name(summary.aimee_status), summary.plugins,
                                  summary.missing_owned_sha256, summary.unexpected_reported_sha256);
                        cJSON_Delete(frame);
                        snprintf(out->error, sizeof out->error,
                                 "Claude native launch profile was refused (%s)", reason);
                        goto capture_stop;
                     }
                     profile_seen = 1;
                  }
                  else if (!profile_seen && (!strcmp(frame_type->valuestring, "assistant") ||
                                             !strcmp(frame_type->valuestring, "user") ||
                                             !strcmp(frame_type->valuestring, "stream_event") ||
                                             !strcmp(frame_type->valuestring, "result")))
                  {
                     cJSON_Delete(frame);
                     goto capture_stop;
                  }
               }
               if (aimee_claude_frame_ir(frame, &ir) != 0)
               {
                  cJSON_Delete(frame);
                  goto capture_stop;
               }
               const cJSON *sid = cJSON_GetObjectItemCaseSensitive(frame, "session_id");
               if (cJSON_IsString(sid) && sid->valuestring[0] && strcmp(bound, sid->valuestring) &&
                   (strlen(sid->valuestring) > 1024 ||
                    (bind_session && bind_session(sid->valuestring, context) != 0)))
               {
                  cJSON_Delete(frame);
                  goto capture_stop;
               }
               if (cJSON_IsString(sid) && strlen(sid->valuestring) <= 1024)
                  snprintf(bound, sizeof(bound), "%s", sid->valuestring);
               const cJSON *type = cJSON_GetObjectItemCaseSensitive(frame, "type");
               if (cJSON_IsString(type) && !strcmp(type->valuestring, "result"))
               {
                  saw_result =
                      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(frame, "is_error")) ? -1 : 1;
                  if (cfg->control_channel && in >= 0)
                  {
                     close(in);
                     in = -1;
                  }
               }
            }
            cJSON_Delete(frame);
         next_line:
            if (!end)
            {
               line = buffer + used;
               break;
            }
            line = end + 1;
         }
         size_t remaining = used - (size_t)(line - buffer);
         memmove(buffer, line, remaining);
         used = remaining;
      }
   }
   goto capture_done;
capture_stop:
   capture_failed = 1;
   aimee_ir_session_fail_capture();
capture_done:
   free(buffer);
stop:
   free(input_line);
   input_line = NULL;
   if (in >= 0)
   {
      close(in);
      in = -1;
   }
   /* SIGINT records an interrupted turn; SIGKILL is the bounded shutdown fallback. */
   if (pid > 0)
   {
      if (!eof || interrupted)
         kill(-pid, SIGINT);
      int reaped = 0;
      for (int i = 0; i < 30; i++)
      {
         pid_t w = waitpid(pid, &status, WNOHANG);
         if (w == pid || (w < 0 && errno == ECHILD))
         {
            reaped = 1;
            break;
         }
         struct timespec pause = {0, 100000000L};
         nanosleep(&pause, NULL);
      }
      if (!reaped)
      {
         kill(-pid, SIGKILL);
         while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
         {
         }
      }
   }
   if (eof && initialized && prompt_sent && !capture_failed && !interrupted && saw_result == 1 &&
       (!cfg->claude_host || profile_seen) && WIFEXITED(status) && WEXITSTATUS(status) == 0)
   {
      if (cfg->claude_host)
      {
         /* Only scheduler completion. The server factory still requires the
          * original canonical terminal response before reporting task success. */
         out->success = 1;
         out->turns = 1;
         rc = 0;
         goto finish;
      }
      /* Compatibility callers receive a projection of the admitted native
       * snapshot. A terminal result alone cannot invent model history. */
      aimee_response_t response = {0};
      char error[128];
      if (!ir.claude_message ||
          anthropic_backend_parse(ir.claude_message, &response, error, sizeof error) != 0)
      {
         aimee_response_free(&response);
         snprintf(out->error, sizeof out->error,
                  "Claude completed without canonical assistant output");
         capture_failed = 1;
         goto finish;
      }
      out->response = aimee_ir_response_text_alloc(&response);
      out->prompt_tokens = (int)response.usage_in;
      out->completion_tokens = (int)response.usage_out;
      out->cache_read_tokens = (int)response.usage_cache_read;
      out->cache_write_tokens = (int)response.usage_cache_write;
      out->canonical_response = response;
      if (!out->response)
      {
         snprintf(out->error, sizeof out->error, "cannot allocate canonical Claude response");
         capture_failed = 1;
         goto finish;
      }
      out->success = 1;
      out->turns = 1;
      rc = 0;
   }
   else if (!out->error[0])
      snprintf(out->error, sizeof(out->error), "%s",
               interrupted ? "Claude turn interrupted"
               : !capture_failed && saw_result < 0 && ir.failure == AIMEE_HARNESS_FAILURE_RATE_LIMIT
                   ? "Claude request was rate limited by the provider."
               : !capture_failed && saw_result < 0 &&
                       ir.failure == AIMEE_HARNESS_FAILURE_PROVIDER_REJECTED
                   ? "Claude provider rejected the request."
                   : "Claude structured turn failed");
finish:
{
   uint64_t elapsed = mono_ms() - execution_started;
   out->latency_ms = elapsed > INT_MAX ? INT_MAX : (int)elapsed;
}
   if (capture_failed)
   {
      rc = AGENT_RC_CAPTURE_FAILED;
      snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
   }
   if (in >= 0)
      close(in);
   if (output >= 0)
      close(output);
   aimee_harness_ir_free(&ir);
   return rc;
}

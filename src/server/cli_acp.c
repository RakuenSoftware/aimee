#include <aimee/translation/harness_ir.h>
#include "agent_exec.h"
#include "server_session_control.h"
/* cli_acp.c: outbound ACP delegate transport.
 *
 * Implements provider_cli_adapter_t for ACP-capable external agents
 * (claude agent, aider --acp, etc.) via JSON-RPC over stdio.
 *
 * Protocol (standard Agent Client Protocol, JSON-RPC over stdio):
 *   1. spawn subprocess (cli_cmd = "claude-code-acp", "aider --acp", etc.)
 *   2. initialize  {protocolVersion, clientCapabilities:{fs:{read,write}}}  id 1
 *   3. session/new {cwd, mcpServers:[]}  id 2  -> capture result.sessionId
 *   4. session/prompt {sessionId, prompt:[{type:text,text}]}  id 3
 *   5. read session/update notifications (accumulate agent_message_chunk text,
 *      count tool_call), admit client resource/control requests to their owners,
 *      until the session/prompt response (id 3, {stopReason}) lands
 *   6. populate agent_result_t and return
 *
 * acp_turn_consume still recognizes the legacy id-2 terminal + text/delta so a
 * simplified peer keeps working; the live protocol above is what real ACP
 * agents (Zed adapters, claude-code-acp) speak.
 */

#include "provider_cli_adapter.h"
#include "agent_config.h"
#include "util.h"

#include "cJSON.h"
#include "cli_acp.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ACP_DEFAULT_TIMEOUT_MS 300000
#define ACP_LINE_MAX           (256 * 1024)
#define ACP_ARG_MAX            64

typedef struct
{
   int in_fd;
   int out_fd;
   pid_t pid;
   char *buf;
   size_t buf_len;
   size_t buf_cap;
   int (*progress)(void *context);
   void *progress_context;
   int source_failed;
} acp_proc_t;

static void acp_proc_close(acp_proc_t *p)
{
   if (!p)
      return;
   if (p->in_fd >= 0)
   {
      close(p->in_fd);
      p->in_fd = -1;
   }
   if (p->out_fd >= 0)
   {
      close(p->out_fd);
      p->out_fd = -1;
   }
   if (p->pid > 0)
   {
      kill(-p->pid, SIGTERM);
      usleep(50000);
      kill(-p->pid, SIGKILL);
      waitpid(p->pid, NULL, 0);
      p->pid = 0;
   }
   free(p->buf);
   p->buf = NULL;
   p->buf_len = 0;
   p->buf_cap = 0;
}

static int acp_spawn(acp_proc_t *p, const provider_cli_cfg_t *cfg, const char *cli_cmd)
{
   char *tokens[ACP_ARG_MAX + 1] = {0};
   char err[128];
   const char *cmd = (cli_cmd && cli_cmd[0]) ? cli_cmd : "acp";
   int count = provider_cli_split_command(cmd, tokens, ACP_ARG_MAX, err, sizeof(err));
   if (count < 0)
   {
      LOG_ERROR("cli_acp", "failed to parse command '%s': %s", cmd, err);
      return -1;
   }
   tokens[count] = NULL;

   int stdin_fd = -1, stdout_fd = -1;
   if (provider_cli_spawn_argv(cfg, (char *const *)tokens, &stdin_fd, &stdout_fd, &p->pid) != 0)
   {
      LOG_ERROR("cli_acp", "failed to spawn '%s'", tokens[0]);
      provider_cli_free_tokens(tokens, count);
      return -1;
   }
   provider_cli_free_tokens(tokens, count);

   p->in_fd = stdin_fd;
   p->out_fd = stdout_fd;
   int flags = fcntl(p->in_fd, F_GETFL, 0);
   if (flags < 0 || fcntl(p->in_fd, F_SETFL, flags | O_NONBLOCK))
   {
      acp_proc_close(p);
      return -1;
   }
   p->progress = cfg ? cfg->native_progress : NULL;
   p->progress_context = cfg ? cfg->native_progress_context : NULL;

   p->buf_cap = 65536;
   p->buf = malloc(p->buf_cap);
   if (!p->buf)
   {
      acp_proc_close(p);
      return -1;
   }
   p->buf_len = 0;
   return 0;
}

static int acp_io_allowed(acp_proc_t *p, long long deadline_ms)
{
   if (agent_request_cancelled() || aimee_ir_session_capture_failed())
      return 0;
   if (p->progress && p->progress(p->progress_context))
   {
      p->source_failed = 1;
      return 0;
   }
   return util_now_ms() < deadline_ms;
}
static int acp_write_line(acp_proc_t *p, const char *msg, long long deadline_ms)
{
   size_t length = strlen(msg), written = 0;
   while (written <= length)
   {
      if (!acp_io_allowed(p, deadline_ms))
         return -1;
      const char *bytes = written < length ? msg + written : "\n";
      size_t count = written < length ? length - written : 1;
      ssize_t n = write(p->in_fd, bytes, count);
      if (n > 0)
      {
         written += (size_t)n;
         continue;
      }
      if (n < 0 && errno == EINTR)
         continue;
      if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
         return -1;
      long long remaining = deadline_ms - util_now_ms();
      if (remaining <= 0)
         return -1;
      if (remaining > 100)
         remaining = 100;
      fd_set writable;
      FD_ZERO(&writable);
      FD_SET(p->in_fd, &writable);
      struct timeval wait = {.tv_sec = remaining / 1000, .tv_usec = (remaining % 1000) * 1000};
      int ready = select(p->in_fd + 1, NULL, &writable, NULL, &wait);
      if (ready < 0 && errno != EINTR)
         return -1;
   }
   return 0;
}

/* Read one newline-terminated line. Returns malloc'd string (caller frees) or NULL on
 * timeout/error/EOF. */
static char *acp_read_line(acp_proc_t *p, long long deadline_ms)
{
   for (;;)
   {
      if (!acp_io_allowed(p, deadline_ms))
         return NULL;
      /* Check buffer for complete line */
      char *nl = memchr(p->buf, '\n', p->buf_len);
      if (nl)
      {
         size_t line_len = (size_t)(nl - p->buf);
         char *line = malloc(line_len + 1);
         if (!line)
            return NULL;
         memcpy(line, p->buf, line_len);
         line[line_len] = '\0';
         /* Consume from buffer */
         size_t tail = p->buf_len - line_len - 1;
         memmove(p->buf, nl + 1, tail);
         p->buf_len = tail;
         return line;
      }

      /* Grow buffer if needed */
      if (p->buf_len + 1 >= p->buf_cap)
      {
         if (p->buf_cap >= ACP_LINE_MAX)
            return NULL;
         size_t new_cap = p->buf_cap * 2;
         char *nb = realloc(p->buf, new_cap);
         if (!nb)
            return NULL;
         p->buf = nb;
         p->buf_cap = new_cap;
      }

      long long now = util_now_ms();
      if (now >= deadline_ms)
         return NULL;

      long long remain_ms = deadline_ms - now;
      if (remain_ms > 100)
         remain_ms = 100;
      struct timeval tv = {
          .tv_sec = (time_t)(remain_ms / 1000),
          .tv_usec = (suseconds_t)((remain_ms % 1000) * 1000),
      };

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(p->out_fd, &rfds);
      int sel = select(p->out_fd + 1, &rfds, NULL, NULL, &tv);
      if (!sel || (sel < 0 && errno == EINTR))
         continue;
      if (sel <= 0)
         return NULL; /* timeout or error */

      ssize_t r = read(p->out_fd, p->buf + p->buf_len, p->buf_cap - p->buf_len - 1);
      if (r <= 0)
         return NULL; /* EOF or error */
      p->buf_len += (size_t)r;
   }
}

/* Host-owned native lifecycle messages are built dynamically: opaque session
 * IDs and valid task prompts are never silently shortened into another call. */
static cJSON *acp_request(const char *method, int id, cJSON **params)
{
   cJSON *out = cJSON_CreateObject();
   *params = out ? cJSON_AddObjectToObject(out, "params") : NULL;
   if (!*params || !cJSON_AddStringToObject(out, "jsonrpc", "2.0") ||
       !cJSON_AddStringToObject(out, "method", method) || !cJSON_AddNumberToObject(out, "id", id))
   {
      cJSON_Delete(out);
      *params = NULL;
      return NULL;
   }
   return out;
}
static int acp_send_request(acp_proc_t *p, cJSON *owned, long long deadline_ms)
{
   char *wire = owned ? cJSON_PrintUnformatted(owned) : NULL;
   cJSON_Delete(owned);
   if (!wire)
      return -1;
   int rc = acp_write_line(p, wire, deadline_ms);
   free(wire);
   return rc;
}
static cJSON *acp_read_response(acp_proc_t *p, int id, long long deadline_ms)
{
   while (acp_io_allowed(p, deadline_ms))
   {
      char *line = acp_read_line(p, deadline_ms);
      cJSON *frame = line ? aimee_ir_json_parse_frame(line, strlen(line)) : NULL;
      free(line);
      if (!frame)
         return NULL;
      const cJSON *native_id = cJSON_GetObjectItemCaseSensitive(frame, "id");
      if (cJSON_IsNumber(native_id) && native_id->valuedouble == (double)id)
         return frame;
      cJSON_Delete(frame);
   }
   return NULL;
}

/* Append delta text to a growing heap buffer. Returns 0 on success, -1 on OOM. */
static int acp_accumulate(char **buf, size_t *len, const char *delta)
{
   if (!delta || !delta[0])
      return 0;
   size_t dlen = strlen(delta);
   char *nb = realloc(*buf, *len + dlen + 1);
   if (!nb)
      return -1;
   memcpy(nb + *len, delta, dlen + 1);
   *buf = nb;
   *len += dlen;
   return 0;
}

int acp_turn_consume(const char *line, acp_turn_state_t *st)
{
   if (!line || !st)
      return 0;
   cJSON *obj = cJSON_Parse(line);
   if (!obj)
      return 0; /* blank / non-JSON keepalive — ignore */

   cJSON *method_j = cJSON_GetObjectItem(obj, "method");
   const char *method = (method_j && cJSON_IsString(method_j)) ? method_j->valuestring : NULL;

   if (method)
   {
      if (strcmp(method, "text/delta") == 0)
      {
         /* Legacy aimee dialect: params.content is the delta string. */
         cJSON *params = cJSON_GetObjectItem(obj, "params");
         cJSON *content = params ? cJSON_GetObjectItem(params, "content") : NULL;
         if (content && cJSON_IsString(content))
            acp_accumulate(&st->text, &st->len, content->valuestring);
      }
      else if (strcmp(method, "session/update") == 0)
      {
         /* Standard ACP (and aimee's own inbound server): the update is carried
          * in params.update. Text chunks expose their content either as a plain
          * string (aimee dialect) or as a typed {type:"text",text:...} object
          * (Zed/real ACP). tool_call updates bump the tool counter. */
         cJSON *params = cJSON_GetObjectItem(obj, "params");
         cJSON *update = params ? cJSON_GetObjectItem(params, "update") : NULL;
         if (update)
         {
            cJSON *kind = cJSON_GetObjectItem(update, "sessionUpdate");
            if (!kind)
               kind = cJSON_GetObjectItem(update, "type");
            const char *ks = (kind && cJSON_IsString(kind)) ? kind->valuestring : NULL;
            if (ks && (strcmp(ks, "tool_call") == 0 || strcmp(ks, "tool_call_update") == 0))
            {
               st->tool_calls++;
               cJSON *tn = cJSON_GetObjectItem(update, "title");
               if (!tn)
                  tn = cJSON_GetObjectItem(update, "name");
               LOG_INFO("cli_acp", "tool call from ACP agent: %s",
                        (tn && cJSON_IsString(tn)) ? tn->valuestring : "(unknown)");
            }
            else
            {
               cJSON *content = cJSON_GetObjectItem(update, "content");
               if (content && cJSON_IsString(content))
                  acp_accumulate(&st->text, &st->len, content->valuestring);
               else if (content && cJSON_IsObject(content))
               {
                  cJSON *txt = cJSON_GetObjectItem(content, "text");
                  if (txt && cJSON_IsString(txt))
                     acp_accumulate(&st->text, &st->len, txt->valuestring);
               }
            }
         }
      }
      else if (strcmp(method, "tool/call") == 0)
      {
         st->tool_calls++;
         cJSON *params = cJSON_GetObjectItem(obj, "params");
         cJSON *name_j = params ? cJSON_GetObjectItem(params, "name") : NULL;
         LOG_INFO("cli_acp", "tool call from ACP agent: %s",
                  (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "(unknown)");
      }
      cJSON_Delete(obj);
      return 0;
   }

   /* No method -> a response. The terminal turn result is the reply to the
    * prompt request: id == prompt_id for standard ACP (session/prompt, whose
    * result is {stopReason} — text already streamed), or the legacy default 2
    * (prompt/submit, whose result may carry {content}). */
   int terminal_id = (st->prompt_id > 0) ? st->prompt_id : 2;
   cJSON *id_j = cJSON_GetObjectItem(obj, "id");
   cJSON *result_j = cJSON_GetObjectItem(obj, "result");
   if (id_j && cJSON_IsNumber(id_j) && (int)id_j->valuedouble == terminal_id && result_j)
   {
      /* Use result.content only if nothing was streamed via notifications
       * (legacy dialect); standard ACP carries the text in session/update. */
      if (!st->text)
      {
         cJSON *content_j = cJSON_GetObjectItem(result_j, "content");
         if (content_j && cJSON_IsString(content_j))
            acp_accumulate(&st->text, &st->len, content_j->valuestring);
      }
      st->done = 1;
   }
   cJSON *error_j = cJSON_GetObjectItem(obj, "error");
   if (error_j)
   {
      cJSON *msg_j = cJSON_GetObjectItem(error_j, "message");
      snprintf(st->error, sizeof(st->error), "acp error: %s",
               (msg_j && cJSON_IsString(msg_j)) ? msg_j->valuestring : "unknown");
      st->had_error = 1;
      cJSON_Delete(obj);
      return -1;
   }
   cJSON_Delete(obj);
   return 0;
}

/* True if any '/'-separated component of `p` is exactly "..". */
/* Native requests have already crossed translation and canonical capture. The
 * resource callback owns scope, source verification and exact byte/range
 * semantics; this transport never reads or writes the host filesystem. */
static int acp_resource_available(const provider_cli_cfg_t *cfg, const char *name)
{
   return cfg && cfg->tool_call && cfg->tool_catalog &&
          !aimee_ir_request_admit(cfg->tool_catalog) &&
          aimee_ir_find_function(cfg->tool_catalog, "acp", name);
}
int acp_dispatch_client_request(const cJSON *frame, const provider_cli_cfg_t *cfg,
                                aimee_harness_ir_t *ir, const cJSON *decision, cJSON **reply)
{
   if (!reply || !ir || !cJSON_IsObject(frame))
      return -1;
   *reply = NULL;
   const cJSON *id = cJSON_GetObjectItemCaseSensitive(frame, "id");
   const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "method"));
   if (!id || !method)
      return 0;
   if (!strcmp(method, "fs/read_text_file") || !strcmp(method, "fs/write_text_file"))
   {
      aimee_block_t call = {0}, result = {0};
      int valid = !aimee_acp_take_client_tool(ir, &call);
      if (!valid || agent_request_cancelled() || aimee_ir_session_capture_failed() ||
          (cfg && cfg->native_progress && cfg->native_progress(cfg->native_progress_context)))
         goto refused;
      if (!acp_resource_available(cfg, call.tool_name))
      {
         result = (aimee_block_t){
             .type = AIMEE_BLK_TOOL_RESULT,
             .tool_is_error = 1,
             .tool_id = strdup(call.tool_id),
             .tool_result = cJSON_CreateString("ACP client resource has no admitted host owner")};
         aimee_ir_block_set_context(&result, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_DIAGNOSTIC,
                                    AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL,
                                    NULL, 0);
      }
      else if (cfg->tool_call(&call, &result, cfg->tool_context))
         goto refused;
      *reply = aimee_acp_client_tool_reply(frame, &result);
      if (!*reply || !result.tool_id || strcmp(result.tool_id, call.tool_id) ||
          aimee_acp_client_tool_result_ir(ir, &result))
         goto refused;
      aimee_block_free_contents(&call);
      aimee_block_free_contents(&result);
      return 1;
   refused:
      cJSON_Delete(*reply);
      *reply = NULL;
      aimee_block_free_contents(&call);
      aimee_block_free_contents(&result);
      aimee_ir_session_fail_capture();
      return -1;
   }
   if (!strcmp(method, "session/request_permission"))
   {
      cJSON *request = cJSON_CreateObject();
      const cJSON *params = cJSON_GetObjectItemCaseSensitive(frame, "params");
      char *key = cJSON_PrintUnformatted(id);
      if (!request || !key || !decision ||
          !cJSON_AddItemReferenceToObject(request, "input", (cJSON *)params) ||
          !(*reply = aimee_acp_control_reply(id, request, decision)) ||
          aimee_harness_control_decision_ir(ir, key, decision))
      {
         cJSON_Delete(*reply);
         *reply = NULL;
         aimee_ir_session_fail_capture();
      }
      free(key);
      cJSON_Delete(request);
      return *reply ? 1 : -1;
   }
   *reply = aimee_acp_error_reply(id, -32601, "method has no ACP client owner");
   return *reply ? 1 : -1;
}

static cJSON *acp_permission_decision(const provider_cli_cfg_t *cfg, const cJSON *frame,
                                      long long deadline_ms)
{
   server_control_scope_t *scope = server_control_scope_current();
   cJSON *reply = NULL;
   if (!scope)
   {
      reply = cJSON_CreateObject();
      if (reply && !cJSON_AddStringToObject(
                       reply, "decision", cfg->agent && cfg->agent->autonomous ? "allow" : "deny"))
      {
         cJSON_Delete(reply);
         return NULL;
      }
      return reply;
   }
   char *key = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(frame, "id"));
   char *native_id = NULL, *kind = NULL;
   cJSON *request = NULL;
   int taken = 0;
   while (key && util_now_ms() < deadline_ms && !agent_request_cancelled() &&
          !aimee_ir_session_capture_failed())
   {
      if (cfg->native_progress && cfg->native_progress(cfg->native_progress_context))
         break;
      taken = server_control_take(scope, key, &native_id, &kind, &request, &reply);
      if (taken)
         break;
      struct timespec pause = {.tv_nsec = 100000000L};
      nanosleep(&pause, NULL);
   }
   if (taken == 1)
   {
      const cJSON *input = cJSON_GetObjectItemCaseSensitive(request, "input");
      const cJSON *original = cJSON_GetObjectItemCaseSensitive(frame, "params");
      if (!native_id || strcmp(native_id, key) || !kind || strcmp(kind, "permission") ||
          !cJSON_Compare(input, original, 1))
      {
         cJSON_Delete(reply);
         reply = NULL;
         aimee_ir_session_fail_capture();
      }
   }
   else
   {
      if (key)
         server_control_abandon(scope, key);
      cJSON_Delete(reply);
      reply = cJSON_CreateObject();
      if (reply)
         cJSON_AddStringToObject(reply, "decision", "cancel");
   }
   free(key);
   free(native_id);
   free(kind);
   cJSON_Delete(request);
   return reply;
}

static int acp_finish_failure(acp_proc_t *process, agent_result_t *out, long long deadline_ms)
{
   int rc = -1;
   if (process->source_failed || aimee_ir_session_capture_failed())
      rc = AGENT_RC_CAPTURE_FAILED;
   else if (agent_request_cancelled() || util_now_ms() >= deadline_ms)
   {
      rc = AGENT_RC_CONTEXT_REFUSED;
      snprintf(out->error, sizeof out->error, "%s",
               agent_request_cancelled() ? "ACP execution cancelled"
                                         : "ACP execution budget exhausted");
   }
   acp_proc_close(process);
   return rc;
}

static int acp_adapter_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!cfg || !cfg->agent || !cfg->user_prompt || !cfg->user_prompt[0])
   {
      snprintf(out->error, sizeof out->error, "acp adapter: missing agent config or prompt");
      return -1;
   }
   snprintf(out->agent_name, sizeof out->agent_name, "%s", cfg->agent->name);
   if (cfg->history || cfg->canonical_request || (cfg->resume_id && cfg->resume_id[0]))
   {
      snprintf(out->error, sizeof out->error, "ACP canonical replay owner is unavailable");
      return AGENT_RC_CONTEXT_REFUSED;
   }
   if (aimee_ir_session_capture_failed())
      return AGENT_RC_CAPTURE_FAILED;
   if (agent_request_cancelled() ||
       (cfg->native_progress && cfg->native_progress(cfg->native_progress_context)))
   {
      snprintf(out->error, sizeof out->error, "ACP execution source refused before launch");
      return AGENT_RC_CONTEXT_REFUSED;
   }
   long long started = util_now_ms();
   int timeout = cfg->agent->cli_idle_timeout_ms > 0 ? cfg->agent->cli_idle_timeout_ms
                 : cfg->agent->timeout_ms > 0        ? cfg->agent->timeout_ms
                                                     : ACP_DEFAULT_TIMEOUT_MS;
   timeout = agent_timeout_cap_ms(timeout, cfg->agent->tool_loop_timeout_ms_cap);
   long long deadline = started + timeout;
   acp_proc_t process = {.in_fd = -1, .out_fd = -1};
   aimee_harness_ir_t ir = {0};
   acp_turn_state_t turn = {.prompt_id = 3};
   char *session = NULL;
   int rc = -1, prompt_sent = 0;
   const char *command = cfg->agent->cli_cmd[0] ? cfg->agent->cli_cmd : "acp";
   if (acp_spawn(&process, cfg, command))
   {
      snprintf(out->error, sizeof out->error, "ACP launch failed");
      goto failed;
   }
   cJSON *params = NULL, *request = acp_request("initialize", 1, &params);
   cJSON *capabilities = params ? cJSON_AddObjectToObject(params, "clientCapabilities") : NULL;
   cJSON *fs = capabilities ? cJSON_AddObjectToObject(capabilities, "fs") : NULL;
   if (!fs || !cJSON_AddNumberToObject(params, "protocolVersion", 1) ||
       !cJSON_AddBoolToObject(fs, "readTextFile", acp_resource_available(cfg, "read_text_file")) ||
       !cJSON_AddBoolToObject(fs, "writeTextFile", acp_resource_available(cfg, "write_text_file")))
   {
      cJSON_Delete(request);
      snprintf(out->error, sizeof out->error, "ACP initialization allocation failed");
      goto failed;
   }
   if (acp_send_request(&process, request, deadline))
   {
      snprintf(out->error, sizeof out->error, "ACP initialization write failed");
      goto failed;
   }
   cJSON *response = acp_read_response(&process, 1, deadline);
   const cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(result, "protocolVersion");
   int initialized = cJSON_IsObject(result) &&
                     (!version || (cJSON_IsNumber(version) && version->valuedouble == 1));
   cJSON_Delete(response);
   if (!initialized)
   {
      snprintf(out->error, sizeof out->error, "ACP initialization refused or timed out");
      goto failed;
   }
   request = acp_request("session/new", 2, &params);
   if (!params || !cJSON_AddStringToObject(params, "cwd", cfg->cwd ? cfg->cwd : ".") ||
       !cJSON_AddArrayToObject(params, "mcpServers"))
   {
      cJSON_Delete(request);
      snprintf(out->error, sizeof out->error, "ACP session allocation failed");
      goto failed;
   }
   if (acp_send_request(&process, request, deadline))
   {
      snprintf(out->error, sizeof out->error, "ACP session write failed");
      goto failed;
   }
   response = acp_read_response(&process, 2, deadline);
   result = cJSON_GetObjectItemCaseSensitive(response, "result");
   const cJSON *native = cJSON_GetObjectItemCaseSensitive(result, "sessionId");
   if (cJSON_IsObject(result) && (!native || cJSON_IsString(native)))
      session =
          strdup(native ? native->valuestring : ""); /* Legacy single-session peers omit it. */
   cJSON_Delete(response);
   if (!session || (cfg->bind_session && cfg->bind_session(session, cfg->bind_context)))
   {
      snprintf(out->error, sizeof out->error, "ACP session identity unavailable");
      goto failed;
   }
   ir.acp_session_id = session;
   request = acp_request("session/prompt", 3, &params);
   cJSON *prompt = params ? cJSON_AddArrayToObject(params, "prompt") : NULL;
   cJSON *text = prompt ? cJSON_CreateObject() : NULL;
   if (!text || !cJSON_AddItemToArray(prompt, text))
   {
      cJSON_Delete(text);
      cJSON_Delete(request);
      snprintf(out->error, sizeof out->error, "ACP prompt allocation failed");
      goto failed;
   }
   if (!cJSON_AddStringToObject(params, "sessionId", session) ||
       !cJSON_AddStringToObject(text, "type", "text") ||
       !cJSON_AddStringToObject(text, "text", cfg->user_prompt))
   {
      cJSON_Delete(request);
      snprintf(out->error, sizeof out->error, "ACP prompt allocation failed");
      goto failed;
   }
   /* A failed write may already have delivered a complete prompt. Do not retry
    * opaque execution merely because its final byte/acknowledgement was lost. */
   prompt_sent = 1;
   if (acp_send_request(&process, request, deadline))
   {
      snprintf(out->error, sizeof out->error, "ACP prompt write failed");
      goto failed;
   }
   while (!turn.done && acp_io_allowed(&process, deadline))
   {
      char *line = acp_read_line(&process, deadline);
      cJSON *frame = line ? aimee_ir_json_parse_frame(line, strlen(line)) : NULL;
      if (!frame || aimee_acp_frame_ir(frame, &ir))
      {
         free(line);
         cJSON_Delete(frame);
         snprintf(out->error, sizeof out->error, "ACP canonical frame/capture failed");
         aimee_ir_session_fail_capture();
         goto failed;
      }
      const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, "method"));
      cJSON *decision = method && !strcmp(method, "session/request_permission")
                            ? acp_permission_decision(cfg, frame, deadline)
                            : NULL;
      cJSON *reply = NULL;
      int handled = acp_dispatch_client_request(frame, cfg, &ir, decision, &reply);
      cJSON_Delete(decision);
      cJSON_Delete(frame);
      if (handled)
      {
         free(line);
         if (handled < 0 || acp_send_request(&process, reply, deadline))
         {
            if (handled < 0)
               cJSON_Delete(reply);
            snprintf(out->error, sizeof out->error, "ACP host request source or delivery failed");
            aimee_ir_session_fail_capture();
            goto failed;
         }
         continue;
      }
      int consumed = acp_turn_consume(line, &turn);
      free(line);
      if (consumed < 0)
      {
         snprintf(out->error, sizeof out->error, "%s", turn.error);
         goto failed;
      }
   }
   if (!turn.done)
   {
      snprintf(out->error, sizeof out->error, "ACP completed without a prompt response");
      goto failed;
   }
   aimee_response_t final = {0};
   if (aimee_harness_ir_response(&ir, &final) || agent_result_adopt_response(out, &final))
   {
      aimee_response_free(&final);
      snprintf(out->error, sizeof out->error, "ACP completed without canonical output");
      aimee_ir_session_fail_capture();
      rc = AGENT_RC_CAPTURE_FAILED;
      goto done;
   }
   snprintf(out->stop_reason, sizeof out->stop_reason, "%s",
            out->canonical_response.raw_stop_reason
                ? out->canonical_response.raw_stop_reason
                : aimee_stop_reason_name(out->canonical_response.stop_reason));
   if (aimee_ir_response_complete_valid(&out->canonical_response))
   {
      snprintf(out->error, sizeof out->error,
               "ACP execution did not return a complete canonical answer");
      rc = AGENT_RC_CONTEXT_REFUSED;
      goto done;
   }
   out->success = 1;
   out->confidence = 80;
   rc = 0;
   goto done;
failed:
   rc = acp_finish_failure(&process, out, deadline);
   if (prompt_sent && rc == -1)
   {
      /* Opaque native execution may already have performed an approved effect.
       * A missing/failed terminal cannot authorize a fresh route retry. */
      aimee_ir_session_fail_capture();
      rc = AGENT_RC_CAPTURE_FAILED;
   }
done:
   out->latency_ms = (int)(util_now_ms() - started);
   out->turns = 1;
   out->tool_calls = turn.tool_calls;
   acp_proc_close(&process);
   aimee_harness_ir_free(&ir);
   free(session);
   free(turn.text);
   return rc;
}

const provider_cli_adapter_t acp_provider_cli_adapter = {
    .cli_kind = "acp",
    .display_name = "ACP agent transport",
    .caps = {.max_context_tokens = 0,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_STABLE,
             .write_confidence = 0.85f},
    .spawn = NULL,
    .execute = acp_adapter_execute,
};

#include <aimee/translation/harness_ir.h>
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
 *      count tool_call), serve agent fs/permission requests against the
 *      worktree, until the session/prompt response (id 3, {stopReason}) lands
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

#define ACP_FS_READ_CAP (16 * 1024 * 1024) /* max bytes served from a single fs/read */

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
      provider_cli_free_tokens(tokens, count);
      LOG_ERROR("cli_acp", "failed to spawn '%s'", tokens[0]);
      return -1;
   }
   provider_cli_free_tokens(tokens, count);

   p->in_fd = stdin_fd;
   p->out_fd = stdout_fd;

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

static int acp_write_line(acp_proc_t *p, const char *msg)
{
   size_t len = strlen(msg);
   size_t written = 0;
   while (written < len)
   {
      ssize_t n = write(p->in_fd, msg + written, len - written);
      if (n < 0)
      {
         if (errno == EINTR || errno == EAGAIN)
            continue;
         return -1;
      }
      written += (size_t)n;
   }
   /* Append newline */
   char nl = '\n';
   ssize_t n;
   do
   {
      n = write(p->in_fd, &nl, 1);
   } while (n < 0 && errno == EINTR);
   return (n > 0) ? 0 : -1;
}

/* Read one newline-terminated line. Returns malloc'd string (caller frees) or NULL on
 * timeout/error/EOF. */
static char *acp_read_line(acp_proc_t *p, long long deadline_ms)
{
   for (;;)
   {
      if (agent_request_cancelled())
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
      if (remain_ms > 100 && aimee_ir_session_get_sink(NULL))
         remain_ms = 100;
      struct timeval tv = {
          .tv_sec = (time_t)(remain_ms / 1000),
          .tv_usec = (suseconds_t)((remain_ms % 1000) * 1000),
      };

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(p->out_fd, &rfds);
      int sel = select(p->out_fd + 1, &rfds, NULL, NULL, &tv);
      if (sel == 0 && aimee_ir_session_get_sink(NULL))
         continue;
      if (sel <= 0)
         return NULL; /* timeout or error */

      ssize_t r = read(p->out_fd, p->buf + p->buf_len, p->buf_cap - p->buf_len - 1);
      if (r <= 0)
         return NULL; /* EOF or error */
      p->buf_len += (size_t)r;
   }
}

/* Drain and discard the subprocess response for a given JSON-RPC id. */
static void acp_drain_response(acp_proc_t *p, int msg_id, long long deadline_ms)
{
   for (int i = 0; i < 10; i++)
   {
      char *line = acp_read_line(p, deadline_ms);
      if (!line)
         break;
      cJSON *obj = cJSON_Parse(line);
      free(line);
      if (!obj)
         continue;
      cJSON *id_j = cJSON_GetObjectItem(obj, "id");
      int matched = id_j && cJSON_IsNumber(id_j) && (int)id_j->valuedouble == msg_id;
      cJSON_Delete(obj);
      if (matched)
         break;
   }
}

/* Read lines until the response for `msg_id` arrives, extracting
 * result.sessionId into sid. Returns 0 if a session id was captured, -1
 * otherwise (timeout/EOF/missing). */
static int acp_read_session_id(acp_proc_t *p, int msg_id, char *sid, size_t sid_n,
                               long long deadline_ms)
{
   if (sid && sid_n)
      sid[0] = '\0';
   for (int i = 0; i < 20; i++)
   {
      char *line = acp_read_line(p, deadline_ms);
      if (!line)
         break;
      cJSON *obj = cJSON_Parse(line);
      free(line);
      if (!obj)
         continue;
      cJSON *id_j = cJSON_GetObjectItem(obj, "id");
      int matched = id_j && cJSON_IsNumber(id_j) && (int)id_j->valuedouble == msg_id;
      if (matched)
      {
         cJSON *result = cJSON_GetObjectItem(obj, "result");
         cJSON *sj = result ? cJSON_GetObjectItem(result, "sessionId") : NULL;
         if (sj && cJSON_IsString(sj) && sid && sid_n)
            snprintf(sid, sid_n, "%s", sj->valuestring);
         cJSON_Delete(obj);
         return (sid && sid[0]) ? 0 : -1;
      }
      cJSON_Delete(obj);
   }
   return -1;
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
static int acp_path_has_dotdot(const char *p)
{
   const char *s = p;
   while (*s)
   {
      const char *seg = s;
      while (*s && *s != '/')
         s++;
      if ((size_t)(s - seg) == 2 && seg[0] == '.' && seg[1] == '.')
         return 1;
      while (*s == '/')
         s++;
   }
   return 0;
}

/* Resolve an ACP-supplied `path` to a location confined to `workdir`. Relative
 * paths join workdir; absolute paths must already sit under workdir. Any ".."
 * component is rejected outright. Returns 0 (out filled) on success, -1 on a
 * missing/escaping path. */
static int acp_resolve_in_workdir(const char *workdir, const char *path, char *out, size_t outsz)
{
   if (!workdir || !workdir[0] || !path || !path[0])
      return -1;
   if (acp_path_has_dotdot(path))
      return -1;
   if (path[0] == '/')
   {
      size_t wl = strlen(workdir);
      while (wl > 0 && workdir[wl - 1] == '/')
         wl--; /* ignore trailing slash on workdir */
      if (strncmp(path, workdir, wl) != 0 || (path[wl] != '/' && path[wl] != '\0'))
         return -1;
      if ((size_t)snprintf(out, outsz, "%s", path) >= outsz)
         return -1;
   }
   else if ((size_t)snprintf(out, outsz, "%s/%s", workdir, path) >= outsz)
      return -1;
   return 0;
}

/* Read up to ACP_FS_READ_CAP bytes of a file into a heap buffer (caller frees).
 * Returns NULL on error; sets *too_big when the file exceeds the cap. */
static char *acp_read_file_bounded(const char *path, int *too_big)
{
   if (too_big)
      *too_big = 0;
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz < 0 || (unsigned long)sz > ACP_FS_READ_CAP)
   {
      if (sz >= 0 && too_big)
         *too_big = 1;
      fclose(f);
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[n] = '\0';
   return buf;
}

/* mkdir -p the parent directory of `path` (components already confined to the
 * workdir by acp_resolve_in_workdir, so this only creates dirs under it). */
static void acp_mkdir_parents(const char *path)
{
   char tmp[PATH_MAX * 2];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s", path) >= sizeof(tmp))
      return;
   char *last = strrchr(tmp, '/');
   if (!last || last == tmp)
      return;
   *last = '\0';
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(tmp, 0755);
         *p = '/';
      }
   }
   mkdir(tmp, 0755);
}

static cJSON *acp_response_envelope(cJSON *id)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
   return resp;
}

static char *acp_error_response(cJSON *id, int code, const char *message)
{
   cJSON *resp = acp_response_envelope(id);
   cJSON *err = cJSON_AddObjectToObject(resp, "error");
   cJSON_AddNumberToObject(err, "code", code);
   cJSON_AddStringToObject(err, "message", message ? message : "error");
   char *s = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   return s;
}

/* Serve a client-side ACP request issued by the agent during a turn. The agent
 * acts on aimee's worktree (workdir) through these requests; we sandbox every
 * path to it. Recognized methods: fs/read_text_file, fs/write_text_file,
 * session/request_permission (auto-approved). Any other method+id request gets
 * a JSON-RPC method-not-found so the agent does not hang.
 *
 * Returns 1 when `line` was an agent request and *resp_out holds the heap
 * JSON-RPC response to write back (caller frees); 0 when `line` is not a
 * request (no id, or a response) and should be handled as turn content. */
int acp_serve_client_request(const char *line, const char *workdir, char **resp_out)
{
   if (resp_out)
      *resp_out = NULL;
   if (!line || !resp_out)
      return 0;
   cJSON *obj = cJSON_Parse(line);
   if (!obj)
      return 0;
   cJSON *method_j = cJSON_GetObjectItem(obj, "method");
   cJSON *id_j = cJSON_GetObjectItem(obj, "id");
   const char *method = (method_j && cJSON_IsString(method_j)) ? method_j->valuestring : NULL;
   /* A request has both a method and an id. (method+no-id = notification;
    * id+no-method = response — both belong to acp_turn_consume.) */
   if (!method || !id_j)
   {
      cJSON_Delete(obj);
      return 0;
   }
   cJSON *params = cJSON_GetObjectItem(obj, "params");

   if (strcmp(method, "fs/read_text_file") == 0)
   {
      cJSON *pj = params ? cJSON_GetObjectItem(params, "path") : NULL;
      char full[PATH_MAX * 2];
      if (!pj || !cJSON_IsString(pj) ||
          acp_resolve_in_workdir(workdir, pj->valuestring, full, sizeof(full)) != 0)
         *resp_out = acp_error_response(id_j, -32602, "invalid or out-of-workdir path");
      else
      {
         int too_big = 0;
         char *content = acp_read_file_bounded(full, &too_big);
         if (!content)
            *resp_out =
                acp_error_response(id_j, -32603, too_big ? "file too large" : "cannot read file");
         else
         {
            cJSON *resp = acp_response_envelope(id_j);
            cJSON *result = cJSON_AddObjectToObject(resp, "result");
            cJSON_AddStringToObject(result, "content", content);
            free(content);
            *resp_out = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
         }
      }
   }
   else if (strcmp(method, "fs/write_text_file") == 0)
   {
      cJSON *pj = params ? cJSON_GetObjectItem(params, "path") : NULL;
      cJSON *cj = params ? cJSON_GetObjectItem(params, "content") : NULL;
      char full[PATH_MAX * 2];
      if (!pj || !cJSON_IsString(pj) || !cj || !cJSON_IsString(cj) ||
          acp_resolve_in_workdir(workdir, pj->valuestring, full, sizeof(full)) != 0)
         *resp_out = acp_error_response(id_j, -32602, "invalid or out-of-workdir path");
      else
      {
         acp_mkdir_parents(full);
         FILE *f = fopen(full, "wb");
         int ok = 0;
         if (f)
         {
            const char *data = cj->valuestring;
            size_t len = strlen(data);
            ok = (fwrite(data, 1, len, f) == len);
            if (fclose(f) != 0)
               ok = 0;
         }
         if (!ok)
            *resp_out = acp_error_response(id_j, -32603, "cannot write file");
         else
         {
            cJSON *resp = acp_response_envelope(id_j);
            cJSON_AddNullToObject(resp, "result");
            *resp_out = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
         }
      }
   }
   else if (strcmp(method, "session/request_permission") == 0)
   {
      /* Auto-approve: the delegate already runs in an isolated worktree and the
       * operator opted in via --acp. Select the first "allow" option, else the
       * first option, else cancel. */
      const char *opt_id = NULL;
      cJSON *options = params ? cJSON_GetObjectItem(params, "options") : NULL;
      if (options && cJSON_IsArray(options))
      {
         cJSON *first = NULL, *o = NULL;
         cJSON_ArrayForEach(o, options)
         {
            cJSON *oid = cJSON_GetObjectItem(o, "optionId");
            cJSON *kind = cJSON_GetObjectItem(o, "kind");
            if (!oid || !cJSON_IsString(oid))
               continue;
            if (!first)
               first = oid;
            if (kind && cJSON_IsString(kind) && strncmp(kind->valuestring, "allow", 5) == 0)
            {
               opt_id = oid->valuestring;
               break;
            }
         }
         if (!opt_id && first)
            opt_id = first->valuestring;
      }
      cJSON *resp = acp_response_envelope(id_j);
      cJSON *result = cJSON_AddObjectToObject(resp, "result");
      cJSON *outcome = cJSON_AddObjectToObject(result, "outcome");
      if (opt_id)
      {
         cJSON_AddStringToObject(outcome, "outcome", "selected");
         cJSON_AddStringToObject(outcome, "optionId", opt_id);
      }
      else
         cJSON_AddStringToObject(outcome, "outcome", "cancelled");
      *resp_out = cJSON_PrintUnformatted(resp);
      cJSON_Delete(resp);
   }
   else
   {
      /* Unknown agent request: reply method-not-found so the agent proceeds. */
      *resp_out = acp_error_response(id_j, -32601, "method not supported by aimee ACP client");
   }

   cJSON_Delete(obj);
   return 1;
}

static int acp_adapter_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   if (!cfg || !cfg->agent)
   {
      snprintf(out->error, sizeof(out->error), "acp adapter: missing agent config");
      return -1;
   }
   if (!cfg->user_prompt || !cfg->user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "acp adapter: empty prompt");
      return -1;
   }

   snprintf(out->agent_name, sizeof(out->agent_name), "%s", cfg->agent->name);

   long long start_ms = util_now_ms();
   int timeout_ms = (cfg->agent->cli_idle_timeout_ms > 0) ? cfg->agent->cli_idle_timeout_ms
                                                          : ACP_DEFAULT_TIMEOUT_MS;
   long long deadline_ms = start_ms + timeout_ms;

   acp_proc_t p = {.in_fd = -1, .out_fd = -1, .pid = 0};
   const char *cli_cmd = cfg->agent->cli_cmd[0] ? cfg->agent->cli_cmd : "acp";
   if (acp_spawn(&p, cfg, cli_cmd) != 0)
   {
      snprintf(out->error, sizeof(out->error), "acp adapter: failed to spawn '%s'", cli_cmd);
      return -1;
   }

   /* Standard Agent Client Protocol handshake: initialize -> session/new ->
    * session/prompt. (The read side already understands the session/update
    * notifications + fs/permission client requests these agents emit.) */

   /* 1. initialize: advertise the client capabilities aimee serves (fs r/w). */
   {
      const char *msg = "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"params\":{"
                        "\"protocolVersion\":1,\"clientCapabilities\":{"
                        "\"fs\":{\"readTextFile\":true,\"writeTextFile\":true}}},\"id\":1}";
      if (acp_write_line(&p, msg) != 0)
      {
         snprintf(out->error, sizeof(out->error), "acp adapter: write initialize failed");
         acp_proc_close(&p);
         return -1;
      }
      /* drain the response without strict check — capabilities vary by agent */
      acp_drain_response(&p, 1, util_now_ms() + 5000);
   }

   /* 2. session/new: create a session rooted at the delegate worktree; capture
    * the agent-assigned sessionId. */
   char acp_session[256] = "";
   {
      char *cwd_esc = cJSON_PrintUnformatted(cJSON_CreateString(cfg->cwd ? cfg->cwd : "."));
      char msg[1024];
      snprintf(msg, sizeof(msg),
               "{\"jsonrpc\":\"2.0\",\"method\":\"session/new\","
               "\"params\":{\"cwd\":%s,\"mcpServers\":[]},\"id\":2}",
               cwd_esc ? cwd_esc : "\".\"");
      free(cwd_esc);
      if (acp_write_line(&p, msg) != 0)
      {
         snprintf(out->error, sizeof(out->error), "acp adapter: write session/new failed");
         acp_proc_close(&p);
         return -1;
      }
      /* A sessionId is preferred but not fatal if absent (some agents are
       * single-session and ignore it); proceed either way. */
      acp_read_session_id(&p, 2, acp_session, sizeof(acp_session), util_now_ms() + 10000);
   }

   /* 3. session/prompt: the prompt is an array of typed content blocks. */
   {
      char *text_esc = cJSON_PrintUnformatted(cJSON_CreateString(cfg->user_prompt));
      if (!text_esc)
      {
         snprintf(out->error, sizeof(out->error), "acp adapter: prompt escape failed");
         acp_proc_close(&p);
         return -1;
      }
      char *sid_esc = cJSON_PrintUnformatted(cJSON_CreateString(acp_session));
      char msg[65536];
      snprintf(msg, sizeof(msg),
               "{\"jsonrpc\":\"2.0\",\"method\":\"session/prompt\",\"params\":{"
               "\"sessionId\":%s,\"prompt\":[{\"type\":\"text\",\"text\":%s}]},\"id\":3}",
               sid_esc ? sid_esc : "\"\"", text_esc);
      free(text_esc);
      free(sid_esc);
      if (acp_write_line(&p, msg) != 0)
      {
         snprintf(out->error, sizeof(out->error), "acp adapter: write session/prompt failed");
         acp_proc_close(&p);
         return -1;
      }
   }

   /* 4. read events until turn complete (parsing is in pure acp_turn_consume).
    * The turn ends with the response to the session/prompt request (id 3). */
   acp_turn_state_t st = {.prompt_id = 3};
   aimee_harness_ir_t ir = {0};
   int capture_failed = 0;
   while (!st.done && util_now_ms() < deadline_ms)
   {
      char *line = acp_read_line(&p, deadline_ms);
      if (!line)
         break;
      cJSON *native = cJSON_Parse(line);
      int capture = cJSON_IsObject(native) ? aimee_acp_frame_ir(native, &ir) : 0;
      cJSON_Delete(native);
      if (capture != 0)
      {
         capture_failed = 1;
         free(line);
         break;
      }
      /* Agent-issued client requests (fs read/write, permission) act on aimee's
       * worktree and must be answered before the turn can complete. */
      char *client_resp = NULL;
      if (acp_serve_client_request(line, cfg->cwd, &client_resp))
      {
         if (client_resp)
         {
            cJSON *reply = cJSON_Parse(client_resp);
            int capture = aimee_codex_reply_ir(reply, &ir);
            cJSON_Delete(reply);
            if (capture != 0)
            {
               capture_failed = 1;
               free(client_resp);
               free(line);
               break;
            }
            acp_write_line(&p, client_resp);
            free(client_resp);
         }
         free(line);
         continue;
      }
      int rc = acp_turn_consume(line, &st);
      free(line);
      if (rc < 0)
      {
         snprintf(out->error, sizeof(out->error), "%s", st.error);
         acp_proc_close(&p);
         free(st.text);
         aimee_harness_ir_free(&ir);
         return -1;
      }
   }

   aimee_harness_ir_free(&ir);
   if (capture_failed)
   {
      acp_proc_close(&p);
      free(st.text);
      snprintf(out->error, sizeof(out->error), "ACP canonical capture failed");
      return -1;
   }
   acp_proc_close(&p);

   out->latency_ms = (int)(util_now_ms() - start_ms);
   out->turns = 1;
   out->tool_calls += st.tool_calls;

   if (!st.done)
   {
      snprintf(out->error, sizeof(out->error), "acp adapter: no response (timeout or EOF)");
      free(st.text);
      return -1;
   }

   out->response = st.text; /* caller/agent framework frees */
   out->success = 1;
   out->confidence = 80;
   return 0;
}

const provider_cli_adapter_t acp_provider_cli_adapter = {
    .cli_kind = "acp",
    .display_name = "ACP agent transport",
    .caps = {.max_context_tokens = 0,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_STABLE,
             .write_confidence = 0.85f},
    .spawn = NULL,
    .parse_line = NULL,
    .format_tool_result = NULL,
    .is_write_event = NULL,
    .build_prompt = NULL,
    .execute = acp_adapter_execute,
};

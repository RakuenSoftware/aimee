/* server_compute.c: POSIX chat.send_stream worker for primary-agent streaming. */
#include "aimee.h"
#include "agent_adapter.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_tools.h" /* agent_tools_set_tool_event_cb — stream tool events */
#include "cli_codex.h"
#include "config.h"
#include "primary_session_adapter.h"
#include "workspace_provider.h"

/* Defined in agent_runtime_tmux.c (no shared header; agent_runtime.c forward-
 * declares it the same way). Drives the standard CLI agent over a tmux session,
 * which runs on the client over the reverse channel for a detached workspace. */
int agent_execute_cli_session(const agent_t *agent, const agent_network_t *network,
                              const char *system_prompt, const char *user_prompt, int max_tokens,
                              double temperature, agent_result_t *out);
#include "prompts.h"
#include "persona.h"
#include "server_http.h"
#include "server_compute_impl.h"
#include "util.h"
#include "presence.h"
#include "turn_registry.h"
#include "workspace_turn.h"
#include "cJSON.h"
#include "token_tracker.h"
#include "reasoning_cap.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Resolve the absolute path to the 'claude' CLI binary.
 * aimee-server runs under systemd with a minimal PATH that excludes
 * ~/.local/bin, so execvp("claude", ...) silently fails. We probe:
 *   1. Same directory as the aimee-server binary itself
 *   2. $HOME/.local/bin/claude
 * Falling back to bare "claude" if neither exists (lets PATH work in
 * non-systemd environments). */
static const char *resolve_claude_bin(char *buf, size_t buf_len)
{
   struct stat st;

   /* Probe sibling of the running aimee-server binary */
   char exe[1024];
   ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   if (exe_len > 0)
   {
      exe[exe_len] = '\0';
      char *slash = strrchr(exe, '/');
      if (slash)
      {
         snprintf(buf, buf_len, "%.*s/claude", (int)(slash - exe), exe);
         if (stat(buf, &st) == 0 && (st.st_mode & S_IXUSR))
            return buf;
      }
   }

   /* Probe $HOME/.local/bin/claude */
   const char *home = getenv("HOME");
   if (home && home[0])
   {
      snprintf(buf, buf_len, "%s/.local/bin/claude", home);
      if (stat(buf, &st) == 0 && (st.st_mode & S_IXUSR))
         return buf;
   }

   return "claude";
}

/* Text-delta coalescing bounds for the presence ring (WP-1): flush accumulated
 * text as one turn_delta on either bound, capping ring-fill rate without
 * reordering relative to non-text events. */
#define RING_TEXT_COALESCE_BYTES 2048
#define RING_TEXT_COALESCE_MS    50

static uint64_t mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* All ring/coalescing helpers below run with cctx->write_mutex held, so the
 * whole emit (ring publish + buffer mutation + socket write) is atomic per
 * cctx — non-text events can never interleave ahead of buffered text. */

static void ring_flush_text_locked(compute_ctx_t *cctx)
{
   if (cctx->delta_len == 0)
      return;
   cJSON *d = cJSON_CreateObject();
   cJSON_AddStringToObject(d, "turn_id", cctx->presence_turn_id);
   cJSON_AddStringToObject(d, "kind", "text");
   cJSON_AddStringToObject(d, "content", cctx->delta_buf);
   char *dj = cJSON_PrintUnformatted(d);
   cJSON_Delete(d);
   if (dj)
   {
      presence_emit_turn_delta(cctx->presence_session, cctx->presence_turn_id, dj);
      free(dj);
   }
   cctx->delta_len = 0;
   if (cctx->delta_buf)
      cctx->delta_buf[0] = '\0';
}

static void ring_text_append_locked(compute_ctx_t *cctx, const char *text)
{
   size_t tlen = strlen(text);
   if (tlen == 0)
      return;
   if (cctx->delta_len == 0)
      cctx->delta_first_ms = mono_ms();
   size_t need = cctx->delta_len + tlen + 1;
   if (need > cctx->delta_cap)
   {
      size_t ncap = cctx->delta_cap ? cctx->delta_cap : 1024;
      while (ncap < need)
         ncap *= 2;
      char *nb = realloc(cctx->delta_buf, ncap);
      if (!nb)
      {
         /* OOM: flush what we have so we don't drop the stream silently. */
         ring_flush_text_locked(cctx);
         return;
      }
      cctx->delta_buf = nb;
      cctx->delta_cap = ncap;
   }
   memcpy(cctx->delta_buf + cctx->delta_len, text, tlen);
   cctx->delta_len += tlen;
   cctx->delta_buf[cctx->delta_len] = '\0';
   if (cctx->delta_len >= RING_TEXT_COALESCE_BYTES ||
       (mono_ms() - cctx->delta_first_ms) >= RING_TEXT_COALESCE_MS)
      ring_flush_text_locked(cctx);
}

/* Publish a non-text event to the ring as a turn_delta. Turn boundary events
 * (turn_start/turn_end/done) are published to the ring by the pooled worker as
 * turn_started/turn_done, so they are skipped here to avoid duplication. */
static void ring_publish_event_locked(compute_ctx_t *cctx, const char *event, const char *key,
                                      const char *value)
{
   if (!event || strcmp(event, "turn_start") == 0 || strcmp(event, "turn_end") == 0 ||
       strcmp(event, "done") == 0)
      return;
   cJSON *d = cJSON_CreateObject();
   cJSON_AddStringToObject(d, "turn_id", cctx->presence_turn_id);
   cJSON_AddStringToObject(d, "kind", event);
   if (key && value)
      cJSON_AddStringToObject(d, key, value);
   char *dj = cJSON_PrintUnformatted(d);
   cJSON_Delete(d);
   if (dj)
   {
      presence_emit_turn_delta(cctx->presence_session, cctx->presence_turn_id, dj);
      free(dj);
   }
}

/* Flush any buffered text to the ring (used at turn teardown). Takes the lock. */
static void stream_flush_text(compute_ctx_t *cctx)
{
   pthread_mutex_lock(cctx->write_mutex);
   ring_flush_text_locked(cctx);
   pthread_mutex_unlock(cctx->write_mutex);
}

/* Send a streaming event as newline-delimited JSON.
 *
 * The presence-event ring is the UNCONDITIONAL sink (so a turn survives a
 * dropped connection — server-owned turn lifecycle); the connection socket is a
 * best-effort, connection-scoped sink gated on conn_alive. Returns 0 while the
 * connection is alive, -1 once it has dropped (callers that care; most ignore). */
static int stream_event(compute_ctx_t *cctx, const char *event, const char *key, const char *value)
{
   pthread_mutex_lock(cctx->write_mutex);

   /* 1) Ring publish — unconditional, coalesced, never gated on conn_alive. */
   if (cctx->presence_emit_deltas && cctx->presence_session[0])
   {
      if (event && strcmp(event, "text") == 0 && value && value[0])
         ring_text_append_locked(cctx, value);
      else
      {
         ring_flush_text_locked(cctx); /* preserve order vs. buffered text */
         ring_publish_event_locked(cctx, event, key, value);
      }
   }

   /* 2) Socket write — best effort, only while the connection is alive. */
   if (cctx->conn_alive)
   {
      cJSON *evt = cJSON_CreateObject();
      cJSON_AddStringToObject(evt, "event", event);
      if (key && value)
         cJSON_AddStringToObject(evt, key, value);
      char *json_str = cJSON_PrintUnformatted(evt);
      cJSON_Delete(evt);
      if (json_str)
      {
         int r = write_all(cctx->conn_fd, json_str, strlen(json_str));
         if (r == 0)
            r = write_all(cctx->conn_fd, "\n", 1);
         free(json_str);
         if (r != 0)
            cctx->conn_alive = 0;
      }
   }

   /* The ring is the unconditional sink, so a dead socket is NOT a turn-ending
    * condition: report success whenever the event reached the ring, so no caller
    * aborts the turn on disconnect. Fall back to the connection-liveness signal
    * only for non-presence turns (ring inactive). */
   int ring_active = cctx->presence_emit_deltas && cctx->presence_session[0];
   int alive = cctx->conn_alive;
   pthread_mutex_unlock(cctx->write_mutex);
   return (ring_active || alive) ? 0 : -1;
}

static int stream_event_usage(compute_ctx_t *cctx, int in_tokens, int out_tokens, double cost)
{
   pthread_mutex_lock(cctx->write_mutex);

   /* 1) Ring publish — flush pending text first (ordering), then usage. */
   if (cctx->presence_emit_deltas && cctx->presence_session[0])
   {
      ring_flush_text_locked(cctx);
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "turn_id", cctx->presence_turn_id);
      cJSON_AddStringToObject(d, "kind", "usage");
      cJSON_AddNumberToObject(d, "in", (double)in_tokens);
      cJSON_AddNumberToObject(d, "out", (double)out_tokens);
      cJSON_AddNumberToObject(d, "cost", cost);
      char *dj = cJSON_PrintUnformatted(d);
      cJSON_Delete(d);
      if (dj)
      {
         presence_emit_turn_delta(cctx->presence_session, cctx->presence_turn_id, dj);
         free(dj);
      }
   }

   /* 2) Socket write — best effort. */
   if (cctx->conn_alive)
   {
      cJSON *evt = cJSON_CreateObject();
      cJSON_AddStringToObject(evt, "event", "usage");
      cJSON_AddNumberToObject(evt, "in", (double)in_tokens);
      cJSON_AddNumberToObject(evt, "out", (double)out_tokens);
      cJSON_AddNumberToObject(evt, "cost", cost);
      char *json_str = cJSON_PrintUnformatted(evt);
      cJSON_Delete(evt);
      if (json_str)
      {
         int r = write_all(cctx->conn_fd, json_str, strlen(json_str));
         if (r == 0)
            r = write_all(cctx->conn_fd, "\n", 1);
         free(json_str);
         if (r != 0)
            cctx->conn_alive = 0;
      }
   }

   int ring_active = cctx->presence_emit_deltas && cctx->presence_session[0];
   int alive = cctx->conn_alive;
   pthread_mutex_unlock(cctx->write_mutex);
   return (ring_active || alive) ? 0 : -1;
}

/* Cancellable, non-blocking line reader for the provider subprocess pipe.
 *
 * Replaces blocking fgets so the read loop can be interrupted promptly: each
 * poll tick re-checks the per-turn cancel flag (server-owned turn lifecycle —
 * a hung provider must not wedge a detached turn). The fd must be O_NONBLOCK.
 * Returns 1 with a NUL-terminated line (newline stripped) in out; 0 when no
 * more lines (sets *cancelled on cancel, *eof on provider EOF). A line longer
 * than the buffer is emitted truncated (caller may log). */
typedef struct
{
   char buf[CLAUDE_LINE_MAX];
   size_t len;
} cli_linereader_t;

static int cli_read_line(int fd, cli_linereader_t *lr, struct turn_entry *e, char *out,
                         size_t outmax, int *cancelled, int *eof)
{
   for (;;)
   {
      char *nl = memchr(lr->buf, '\n', lr->len);
      if (nl)
      {
         size_t linelen = (size_t)(nl - lr->buf);
         size_t copy = linelen < outmax - 1 ? linelen : outmax - 1;
         memcpy(out, lr->buf, copy);
         out[copy] = '\0';
         size_t rem = lr->len - (linelen + 1);
         memmove(lr->buf, nl + 1, rem);
         lr->len = rem;
         return 1;
      }
      if (lr->len >= sizeof(lr->buf) - 1)
      {
         /* Oversized line with no newline: emit truncated and reset. */
         memcpy(out, lr->buf, outmax - 1);
         out[outmax - 1] = '\0';
         lr->len = 0;
         LOG_WARN("chat", "provider line exceeded %zu bytes; truncated", sizeof(lr->buf));
         return 1;
      }
      if (turn_entry_cancelled(e))
      {
         *cancelled = 1;
         return 0;
      }
      struct pollfd pfd = {.fd = fd, .events = POLLIN};
      int pr = poll(&pfd, 1, 200);
      if (pr == 0)
         continue; /* tick: re-check cancel */
      if (pr < 0)
      {
         if (errno == EINTR)
            continue;
         *eof = 1;
         return 0;
      }
      if (turn_entry_cancelled(e))
      {
         *cancelled = 1;
         return 0;
      }
      if (pfd.revents & (POLLIN | POLLHUP | POLLERR))
      {
         /* Drain whatever is available. On POLLHUP the pipe is closed but may
          * still hold buffered bytes; read() delivers them before returning 0,
          * so always attempt the read rather than declaring EOF on POLLHUP. */
         ssize_t n = read(fd, lr->buf + lr->len, sizeof(lr->buf) - 1 - lr->len);
         if (n == 0)
         {
            if (lr->len > 0)
            {
               /* Final line without a trailing newline: deliver it, then EOF. */
               size_t copy = lr->len < outmax - 1 ? lr->len : outmax - 1;
               memcpy(out, lr->buf, copy);
               out[copy] = '\0';
               lr->len = 0;
               *eof = 1;
               return 1;
            }
            *eof = 1;
            return 0;
         }
         if (n < 0)
         {
            if (errno == EAGAIN || errno == EINTR)
               continue; /* not really ready / interrupted: re-poll */
            *eof = 1;
            return 0;
         }
         lr->len += (size_t)n;
      }
      else if (pfd.revents & POLLNVAL)
      {
         *eof = 1;
         return 0;
      }
   }
}

static char *read_optional_text_file(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return NULL;
   }
   long n = ftell(fp);
   if (n < 0 || n > 1024 * 1024)
   {
      fclose(fp);
      return NULL;
   }
   rewind(fp);
   char *buf = malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)n, fp);
   fclose(fp);
   buf[got] = '\0';
   return buf;
}

/* Resolve the active persona name for this chat turn. Precedence:
 *   1. the session's persona (set server-side via POST /v1/sessions/<id>/persona)
 *   2. a per-request "mode" field (legacy ephemeral channel)
 *   3. the process-wide durable default (config mode/env), as a built-in name. */
static void chat_ctx_persona(const compute_ctx_t *cctx, char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   if (cctx && cctx->req)
   {
      cJSON *sid = cJSON_GetObjectItemCaseSensitive(cctx->req, "aimee_session_id");
      if (cJSON_IsString(sid) && sid->valuestring[0] &&
          session_persona_get(sid->valuestring, out, n))
         return;
      cJSON *m = cJSON_GetObjectItemCaseSensitive(cctx->req, "mode");
      if (cJSON_IsString(m) && m->valuestring[0])
      {
         snprintf(out, n, "%s", m->valuestring);
         return;
      }
   }
   snprintf(out, n, "%s", aimee_mode_to_string(config_current_mode()));
}

static const char *chat_ctx_cwd(const compute_ctx_t *cctx)
{
   if (cctx && cctx->req)
   {
      cJSON *c = cJSON_GetObjectItemCaseSensitive(cctx->req, "cwd");
      if (cJSON_IsString(c) && c->valuestring[0])
         return c->valuestring;
   }
   return NULL;
}

/* Substitute the first "%s" in a custom persona's prose with cwd (no printf, so
 * stray % in a user file can't inject a format). */
static char *persona_apply_cwd(const char *text, const char *cwd)
{
   if (!text)
      return safe_strdup("");
   const char *pct = strstr(text, "%s");
   if (!pct)
      return safe_strdup(text);
   const char *c = (cwd && cwd[0]) ? cwd : ".";
   size_t prefix = (size_t)(pct - text), clen = strlen(c), suffix = strlen(pct + 2);
   char *out = malloc(prefix + clen + suffix + 1);
   if (!out)
      return safe_strdup("");
   memcpy(out, text, prefix);
   memcpy(out + prefix, c, clen);
   memcpy(out + prefix + clen, pct + 2, suffix + 1);
   return out;
}

static char *read_webchat_system_prompt(const compute_ctx_t *cctx)
{
   char name[PERSONA_NAME_MAX];
   chat_ctx_persona(cctx, name, sizeof(name));
   const char *cwd = chat_ctx_cwd(cctx);

   persona_t p;
   persona_load(NULL, name, &p);

   /* Config is the source of truth: prefer the persona's on-disk prose (set once
    * it is seeded/edited as <config>/personas/<name>.md). The built-in prose in
    * code is only the fallback when no file backs the persona. */
   aimee_mode_t mode = aimee_mode_from_string(name);
   const char *principles = p.principles_text ? p.principles_text : prompt_principles_text(mode);
   char *identity = NULL;
   if (mode == AIMEE_MODE_ENGINEER)
   {
      /* Webchat keeps its dedicated override file as the highest priority (a
       * long-standing customization knob), then the seeded/edited engineer
       * persona file. */
      char sys_path[MAX_PATH_LEN];
      snprintf(sys_path, sizeof(sys_path), "%s/webchat_system_prompt.txt", config_default_dir());
      char *file_prompt = read_optional_text_file(sys_path);
      if (file_prompt && file_prompt[0])
         identity = persona_apply_cwd(file_prompt, cwd);
      else if (p.persona_text)
         identity = persona_apply_cwd(p.persona_text, cwd);
      free(file_prompt);
   }
   else if (p.persona_text)
      identity = persona_apply_cwd(p.persona_text, cwd); /* config file */
   else if (p.builtin)
      identity = prompt_build_mode(mode, PROMPT_STANDARD, cwd, NULL); /* fallback */

   const char *idt = identity ? identity : "";
   size_t plen = strlen(principles), ilen = strlen(idt);
   char *result = malloc(plen + ilen + 1);
   if (result)
   {
      memcpy(result, principles, plen);
      memcpy(result + plen, idt, ilen + 1);
   }
   free(identity);
   persona_free(&p);
   return result ? result : safe_strdup("");
}

static int codex_stream_event_cb(const char *event, const char *value, void *userdata)
{
   compute_ctx_t *cctx = (compute_ctx_t *)userdata;
   if (!event)
      return 0;
   if (strcmp(event, "text") == 0 || strcmp(event, "thinking") == 0)
      return stream_event(cctx, event, "content", value ? value : "");
   if (strcmp(event, "session") == 0)
      return stream_event(cctx, "session", "id", value ? value : "");
   if (strcmp(event, "error") == 0)
      return stream_event(cctx, "error", "message", value ? value : "server error");
   return stream_event(cctx, event, NULL, NULL);
}

static int chat_model_passthrough_allowed(const char *model)
{
   return model && model[0] && strcmp(model, "aimee") != 0 && strncmp(model, "aimee:", 6) != 0;
}

static int chat_claude_effort_allowed(const char *effort)
{
   return effort && (strcmp(effort, "low") == 0 || strcmp(effort, "medium") == 0 ||
                     strcmp(effort, "high") == 0 || strcmp(effort, "xhigh") == 0 ||
                     strcmp(effort, "max") == 0);
}

static int chat_codex_effort_allowed(const char *effort)
{
   return effort && (strcmp(effort, "low") == 0 || strcmp(effort, "medium") == 0 ||
                     strcmp(effort, "high") == 0 || strcmp(effort, "xhigh") == 0);
}

static void chat_claude_stream_content(compute_ctx_t *cctx, cJSON *content, int *saw_content)
{
   if (!cJSON_IsArray(content))
      return;
   cJSON *block = NULL;
   cJSON_ArrayForEach(block, content)
   {
      cJSON *type = cJSON_GetObjectItem(block, "type");
      const char *bt = cJSON_IsString(type) ? type->valuestring : "";
      if (strcmp(bt, "text") == 0)
      {
         cJSON *text = cJSON_GetObjectItem(block, "text");
         if (cJSON_IsString(text) && text->valuestring[0])
         {
            if (saw_content)
               *saw_content = 1;
            stream_event(cctx, "text", "content", text->valuestring);
         }
      }
      else if (strcmp(bt, "thinking") == 0)
      {
         cJSON *text = cJSON_GetObjectItem(block, "thinking");
         if (cJSON_IsString(text) && text->valuestring[0])
         {
            if (saw_content)
               *saw_content = 1;
            stream_event(cctx, "thinking", "content", text->valuestring);
         }
      }
   }
}

static void chat_stream_worker_codex(compute_ctx_t *cctx, const char *message,
                                     const char *thread_id, const char *cwd, const char *model,
                                     const char *effort, const config_t *cfg)
{
   compute_pool_set_job(POOL_JOB_CHAT, "provider=codex thread=%s",
                        thread_id && thread_id[0] ? thread_id : "new");

   char *system_prompt = read_webchat_system_prompt(cctx);

   cli_codex_chat_request_t creq;
   memset(&creq, 0, sizeof(creq));
   creq.thread_id = thread_id && thread_id[0] ? thread_id : NULL;
   creq.cwd = cwd && cwd[0] ? cwd : NULL;
   creq.system_prompt = system_prompt;
   creq.user_prompt = message;
   const char *codex_model =
       chat_model_passthrough_allowed(model) ? model : (cfg ? cfg->codex_model : "");
   creq.model = codex_model && codex_model[0] ? codex_model : NULL;
   int codex_effort_explicit = chat_codex_effort_allowed(effort);
   const char *codex_effort =
       codex_effort_explicit ? effort : (cfg ? cfg->model_reasoning_effort : "");
   /* §5: cap (only lower) the config-derived effort by turn complexity. An
    * explicit per-request override is left untouched. */
   if (cfg && cfg->reasoning_cap_enabled && !codex_effort_explicit)
   {
      int score = reasoning_complexity_score(1, message ? strlen(message) : 0, 1);
      codex_effort = reasoning_effort_capped(codex_effort, score);
   }
   creq.reasoning_effort = chat_codex_effort_allowed(codex_effort) ? codex_effort : NULL;
   creq.timeout_ms = -1;
   creq.autonomous = cfg && cfg->autonomous;

   /* Release the compute budget slot before entering the blocking Codex
    * app-server stream. Codex may call back into aimee MCP tools, and those
    * tool workers also need compute budget; holding it here can deadlock the
    * chat worker against its own nested tool call. */
   compute_ctx_release_budget(cctx);

   cli_codex_chat_result_t result;
   int rc = cli_codex_chat_stream(&creq, codex_stream_event_cb, cctx, &result);
   free(system_prompt);

   if (rc != 0)
   {
      compute_error(cctx, result.error[0] ? result.error : "codex app-server failed");
      compute_ctx_free(cctx);
      return;
   }

   if (result.thread_id[0])
      stream_event(cctx, "session", "id", result.thread_id);
   stream_event(cctx, "done", NULL, NULL);
   compute_ok(cctx);
   compute_ctx_free(cctx);
}

static void chat_agent_add_default_roles(agent_t *ag)
{
   const char *roles[] = {"code", "review", "explain", "refactor", "draft", "execute"};
   ag->role_count = 0;
   for (int i = 0; i < 6 && ag->role_count < MAX_AGENT_ROLES; i++)
      snprintf(ag->roles[ag->role_count++], sizeof(ag->roles[0]), "%s", roles[i]);
}

static void chat_agent_add_legacy_openai(agent_config_t *acfg, const config_t *cfg)
{
   if (!acfg || !cfg || acfg->agent_count >= MAX_AGENTS || agent_find(acfg, "openai"))
      return;

   agent_t *ag = &acfg->agents[acfg->agent_count++];
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, sizeof(ag->name), "openai");
   snprintf(ag->endpoint, sizeof(ag->endpoint), "%s",
            cfg->openai_endpoint[0] ? cfg->openai_endpoint : "https://api.openai.com/v1");
   snprintf(ag->model, sizeof(ag->model), "%s",
            cfg->openai_model[0] ? cfg->openai_model : "gpt-4o");
   if (cfg->openai_key_cmd[0])
      snprintf(ag->auth_cmd, sizeof(ag->auth_cmd), "%s", cfg->openai_key_cmd);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "bearer");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->cost_tier = 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = AGENT_DEFAULT_TIMEOUT_MS;
   ag->enabled = 1;
   ag->tools_enabled = 1;
   ag->max_turns = -1;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;
   chat_agent_add_default_roles(ag);
   if (!acfg->default_agent[0])
      snprintf(acfg->default_agent, sizeof(acfg->default_agent), "openai");
}

static int chat_agent_has_provider(const agent_config_t *acfg, const char *provider)
{
   if (!acfg || !provider || !provider[0])
      return 0;
   for (int i = 0; i < acfg->agent_count; i++)
      if (strcmp(acfg->agents[i].provider, provider) == 0)
         return 1;
   return 0;
}

static void chat_agent_add_builtin_claude_code(agent_config_t *acfg)
{
   if (!acfg || acfg->agent_count >= MAX_AGENTS || agent_find(acfg, "claude-code") ||
       chat_agent_has_provider(acfg, "claude-code"))
      return;

   agent_t *ag = &acfg->agents[acfg->agent_count++];
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, sizeof(ag->name), "claude-code");
   snprintf(ag->auth_type, sizeof(ag->auth_type), "bearer");
   snprintf(ag->provider, sizeof(ag->provider), "claude-code");
   snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_TMUX_CLI);
   snprintf(ag->cli_kind, sizeof(ag->cli_kind), "claude-code");
   snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "claude");
   ag->cost_tier = 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = AGENT_DEFAULT_TIMEOUT_MS;
   ag->enabled = 1;
   ag->tools_enabled = 1;
   ag->max_turns = -1;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;
   ag->session_reuse = 1;
   chat_agent_add_default_roles(ag);
   if (!acfg->default_agent[0])
      snprintf(acfg->default_agent, sizeof(acfg->default_agent), "claude-code");
}

static void chat_agent_add_builtin_provider(agent_config_t *acfg, const char *provider,
                                            const config_t *cfg)
{
   if (provider && strcmp(provider, "openai") == 0)
      chat_agent_add_legacy_openai(acfg, cfg);
   else if (provider && strcmp(provider, "claude-code") == 0)
      chat_agent_add_builtin_claude_code(acfg);
}

static int chat_agent_select_provider(agent_config_t *acfg, const char *provider, char *selected,
                                      size_t selected_len)
{
   if (selected && selected_len > 0)
      selected[0] = '\0';
   if (!acfg || !provider || !provider[0])
      return 0;

   int by_name = -1;
   for (int i = 0; i < acfg->agent_count; i++)
      if (acfg->agents[i].enabled && strcmp(acfg->agents[i].name, provider) == 0)
      {
         by_name = i;
         break;
      }

   int by_provider = -1;
   if (by_name < 0)
   {
      for (int i = 0; i < acfg->agent_count; i++)
         if (acfg->agents[i].enabled && strcmp(acfg->agents[i].provider, provider) == 0)
         {
            by_provider = i;
            break;
         }
   }

   int match = by_name >= 0 ? by_name : by_provider;
   if (match < 0)
      return -1;

   for (int i = 0; i < acfg->agent_count; i++)
      acfg->agents[i].enabled = acfg->agents[i].enabled &&
                                (by_name >= 0 ? strcmp(acfg->agents[i].name, provider) == 0
                                              : strcmp(acfg->agents[i].provider, provider) == 0);

   snprintf(acfg->default_agent, sizeof(acfg->default_agent), "%s", acfg->agents[match].name);
   acfg->fallback_count = 0;
   if (selected && selected_len > 0)
      snprintf(selected, selected_len, "%s", acfg->agents[match].name);
   return 0;
}

static const char *chat_provider_lookup_name(const char *provider)
{
   if (provider && strcmp(provider, "codex-oauth") == 0)
      return "codex";
   return provider;
}

/* Tool-event hook: stream each tool call as a `tool_call.started` /
 * `tool_call.completed` SSE event while the (blocking) turn runs. */
static void chat_tool_event_cb(const char *phase, const char *name, void *ud)
{
   compute_ctx_t *cctx = (compute_ctx_t *)ud;
   char ev[48];
   snprintf(ev, sizeof(ev), "tool_call.%s", phase ? phase : "");
   stream_event(cctx, ev, "name", name ? name : "");
}

static void chat_stream_worker_agent(compute_ctx_t *cctx, const char *message, const char *cwd,
                                     const char *aimee_sid, const char *provider,
                                     const char *model_override, const config_t *cfg)
{
   compute_pool_set_job(POOL_JOB_CHAT, "provider=%s", provider && provider[0] ? provider : "agent");

   char *system_prompt = read_webchat_system_prompt(cctx);

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      memset(&acfg, 0, sizeof(acfg));
   chat_agent_add_builtin_provider(&acfg, provider, cfg);

   char selected[MAX_AGENT_NAME];
   const char *lookup_provider = chat_provider_lookup_name(provider);
   if (chat_agent_select_provider(&acfg, lookup_provider, selected, sizeof(selected)) != 0)
   {
      char err[256];
      snprintf(err, sizeof(err), "provider '%s' is not configured as an aimee agent",
               lookup_provider && lookup_provider[0] ? lookup_provider : "default");
      free(system_prompt);
      compute_error(cctx, err);
      compute_ctx_free(cctx);
      return;
   }
   if (chat_model_passthrough_allowed(model_override))
   {
      agent_t *ag = agent_find(&acfg, selected);
      if (ag)
         snprintf(ag->model, sizeof(ag->model), "%s", model_override);
   }

   /* If this turn's workspace is registered `detached`, route its file/exec
    * tools over the reverse channel to the serving client (no-op for shared). */
   int detached_bound = workspace_turn_bind_active(cwd);
   int trusted_local = (cctx->conn_caps == (uint32_t)CAPS_ALL);
   /* AC #6 — close the worktree_cwd-trust hole: a remote peer must act within a
    * registered `detached` workspace; the server must never open its raw
    * client-supplied path on its own filesystem. */
   if (workspace_turn_reject_foreign_cwd(detached_bound, trusted_local, cwd))
   {
      workspace_turn_unbind_active();
      free(system_prompt);
      compute_error(cctx, "workspace: a remote session must act within a registered `detached` "
                          "workspace; raw server-side path not accepted");
      compute_ctx_free(cctx);
      return;
   }
   /* A `mirror` workspace remaps the turn into a server-side reconstructed
    * worktree; use that path (a server-controlled location) in place of the
    * client-supplied cwd. Otherwise run in the (validated) client cwd. */
   const char *eff_cwd = workspace_turn_active_cwd();
   const char *use_cwd = eff_cwd ? eff_cwd : cwd;
   if (aimee_path_is_absolute(use_cwd) && !strstr(use_cwd, "/.."))
      run_cmd_set_cwd(use_cwd);
   if (aimee_sid && aimee_sid[0])
      session_id_set_override(aimee_sid);

   stream_event(cctx, "turn_start", NULL, NULL);
   /* Surface mirror drift (client head vs server mirror) before the turn acts —
    * AC #5: drift is shown, never silently merged. */
   const char *drift = workspace_turn_drift_notice();
   if (drift)
      stream_event(cctx, "text", "content", drift);
   agent_tools_set_tool_event_cb(chat_tool_event_cb, cctx);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = agent_run_with_tools(&acfg, "code", system_prompt ? system_prompt : "", message,
                                 AGENT_DEFAULT_MAX_TOKENS, &result);

   agent_tools_set_tool_event_cb(NULL, NULL);
   session_id_clear_override();
   workspace_turn_unbind_active();
   run_cmd_set_cwd(NULL);
   free(system_prompt);

   if (rc != 0)
   {
      compute_error(cctx, result.error[0] ? result.error : "agent provider failed");
      free(result.response);
      compute_ctx_free(cctx);
      return;
   }

   if (result.response && result.response[0])
      stream_event(cctx, "text", "content", result.response);
   stream_event(cctx, "turn_end", NULL, NULL);
   stream_event(cctx, "done", NULL, NULL);
   free(result.response);
   compute_ok(cctx);
   compute_ctx_free(cctx);
}

static void chat_stream_worker_primary_session(compute_ctx_t *cctx, const char *message,
                                               const char *provider_sid, const char *cwd,
                                               const char *aimee_sid, const char *provider,
                                               const char *model_override, const config_t *cfg)
{
   compute_pool_set_job(POOL_JOB_CHAT, "provider=%s session=%s",
                        provider && provider[0] ? provider : "agent",
                        provider_sid && provider_sid[0] ? provider_sid : "new");

   char *system_prompt = read_webchat_system_prompt(cctx);

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      memset(&acfg, 0, sizeof(acfg));
   chat_agent_add_builtin_provider(&acfg, provider, cfg);

   char selected[MAX_AGENT_NAME];
   const char *lookup_provider = chat_provider_lookup_name(provider);
   if (chat_agent_select_provider(&acfg, lookup_provider, selected, sizeof(selected)) != 0)
   {
      char err[256];
      snprintf(err, sizeof(err), "provider '%s' is not configured as an aimee agent",
               lookup_provider && lookup_provider[0] ? lookup_provider : "default");
      free(system_prompt);
      compute_error(cctx, err);
      compute_ctx_free(cctx);
      return;
   }

   agent_t *ag = agent_find(&acfg, selected);
   if (!ag || !primary_session_adapter_can_handle(ag))
   {
      char err[256];
      snprintf(err, sizeof(err), "provider '%s' is not a direct primary session adapter",
               lookup_provider && lookup_provider[0] ? lookup_provider : "default");
      free(system_prompt);
      compute_error(cctx, err);
      compute_ctx_free(cctx);
      return;
   }
   if (chat_model_passthrough_allowed(model_override))
      snprintf(ag->model, sizeof(ag->model), "%s", model_override);

   /* If this turn's workspace is registered `detached`, route its file/exec
    * tools over the reverse channel to the serving client (no-op for shared).
    * primary_session_adapter_turn runs the agent loop inline on this thread, so
    * the thread-local provider binding applies to its tool calls — the same
    * seam chat_stream_worker_agent uses. Without this, a direct primary agent
    * (minimax/mistral/...) ran bash/file tools on the server's own fs. */
   int detached_bound = workspace_turn_bind_active(cwd);
   int trusted_local = (cctx->conn_caps == (uint32_t)CAPS_ALL);
   /* AC #6 — a remote peer must act within a registered `detached` workspace;
    * never open its raw client-supplied path on the server's filesystem. */
   if (workspace_turn_reject_foreign_cwd(detached_bound, trusted_local, cwd))
   {
      workspace_turn_unbind_active();
      free(system_prompt);
      compute_error(cctx, "workspace: a remote session must act within a registered `detached` "
                          "workspace; raw server-side path not accepted");
      compute_ctx_free(cctx);
      return;
   }
   /* A `mirror` workspace remaps the turn into a server-side reconstructed
    * worktree; otherwise act in the (validated) client cwd. */
   const char *eff_cwd = workspace_turn_active_cwd();
   const char *use_cwd = eff_cwd ? eff_cwd : cwd;

   stream_event(cctx, "turn_start", NULL, NULL);

   primary_session_request_t preq;
   memset(&preq, 0, sizeof(preq));
   preq.agent = ag;
   preq.network = &acfg.network;
   preq.provider_session_id = provider_sid;
   preq.aimee_session_id = aimee_sid;
   preq.cwd = use_cwd;
   preq.system_prompt = system_prompt ? system_prompt : "";
   preq.user_prompt = message;
   preq.max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   preq.temperature = 0.3;

   char session_id[128];
   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = primary_session_adapter_turn(&preq, &result, session_id, sizeof(session_id));
   workspace_turn_unbind_active();
   free(system_prompt);

   if (rc != 0)
   {
      compute_error(cctx, result.error[0] ? result.error : "primary session adapter failed");
      free(result.response);
      compute_ctx_free(cctx);
      return;
   }

   if (session_id[0])
      stream_event(cctx, "session", "id", session_id);
   if (result.response && result.response[0])
      stream_event(cctx, "text", "content", result.response);
   stream_event(cctx, "turn_end", NULL, NULL);
   stream_event(cctx, "done", NULL, NULL);
   free(result.response);
   compute_ok(cctx);
   compute_ctx_free(cctx);
}

static int chat_provider_uses_codex_cli(const char *provider)
{
   if (!provider)
      return 0;
   if (strcmp(provider, "codex-cli") == 0)
      return 1;
   if (strcmp(provider, "codex") != 0 && strcmp(provider, "codex-oauth") != 0)
      return 0;

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      return 1;

   agent_t *ag = agent_find(&acfg, "codex");
   if (!ag)
      return 1;
   return !agent_adapter_agent_is_direct(ag);
}

static int chat_provider_uses_primary_session(const char *provider, const config_t *cfg)
{
   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      memset(&acfg, 0, sizeof(acfg));
   chat_agent_add_builtin_provider(&acfg, provider, cfg);

   char selected[MAX_AGENT_NAME];
   const char *lookup_provider = chat_provider_lookup_name(provider);
   if (chat_agent_select_provider(&acfg, lookup_provider, selected, sizeof(selected)) != 0)
      return 0;
   agent_t *ag = agent_find(&acfg, selected);
   return primary_session_adapter_can_handle(ag);
}

static void chat_stream_worker_codex_compact(compute_ctx_t *cctx, const char *thread_id,
                                             const char *cwd, const config_t *cfg)
{
   if (!thread_id || !thread_id[0])
   {
      compute_error(cctx, "no active codex thread to compact");
      compute_ctx_free(cctx);
      return;
   }

   compute_pool_set_job(POOL_JOB_CHAT, "provider=codex compact thread=%s", thread_id);

   cli_codex_compact_request_t creq;
   memset(&creq, 0, sizeof(creq));
   creq.thread_id = thread_id;
   creq.cwd = cwd && cwd[0] ? cwd : NULL;
   creq.timeout_ms = -1;
   creq.autonomous = cfg && cfg->autonomous;

   compute_ctx_release_budget(cctx);

   stream_event(cctx, "turn_start", NULL, NULL);
   cli_codex_compact_result_t result;
   int rc = cli_codex_compact_thread(&creq, &result);
   if (rc != 0)
   {
      compute_error(cctx, result.error[0] ? result.error : "codex compaction failed");
      compute_ctx_free(cctx);
      return;
   }

   if (result.thread_id[0])
      stream_event(cctx, "session", "id", result.thread_id);
   stream_event(cctx, "text", "content", "Context compacted.");
   stream_event(cctx, "turn_end", NULL, NULL);
   stream_event(cctx, "done", NULL, NULL);
   compute_ok(cctx);
   compute_ctx_free(cctx);
}

static void chat_stream_worker_primary_session_compact(compute_ctx_t *cctx,
                                                       const char *provider_sid,
                                                       const char *aimee_sid, const char *provider,
                                                       const char *model_override,
                                                       const config_t *cfg)
{
   compute_pool_set_job(POOL_JOB_CHAT, "provider=%s compact session=%s",
                        provider && provider[0] ? provider : "agent",
                        provider_sid && provider_sid[0] ? provider_sid : "current");

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0)
      memset(&acfg, 0, sizeof(acfg));
   chat_agent_add_builtin_provider(&acfg, provider, cfg);

   char selected[MAX_AGENT_NAME];
   const char *lookup_provider = chat_provider_lookup_name(provider);
   if (chat_agent_select_provider(&acfg, lookup_provider, selected, sizeof(selected)) != 0)
   {
      char err[256];
      snprintf(err, sizeof(err), "provider '%s' is not configured as an aimee agent",
               lookup_provider && lookup_provider[0] ? lookup_provider : "default");
      compute_error(cctx, err);
      compute_ctx_free(cctx);
      return;
   }

   agent_t *ag = agent_find(&acfg, selected);
   if (!ag || !primary_session_adapter_can_handle(ag))
   {
      char err[256];
      snprintf(err, sizeof(err), "provider '%s' is not a direct primary session adapter",
               lookup_provider && lookup_provider[0] ? lookup_provider : "default");
      compute_error(cctx, err);
      compute_ctx_free(cctx);
      return;
   }
   if (chat_model_passthrough_allowed(model_override))
      snprintf(ag->model, sizeof(ag->model), "%s", model_override);

   primary_session_request_t preq;
   memset(&preq, 0, sizeof(preq));
   preq.agent = ag;
   preq.provider_session_id = provider_sid;
   preq.aimee_session_id = aimee_sid;

   char session_id[128];
   char err[256];
   session_compact_result_t result;
   stream_event(cctx, "turn_start", NULL, NULL);
   int rc = primary_session_adapter_compact(&preq, &result, session_id, sizeof(session_id), err,
                                            sizeof(err));
   if (rc != 0)
   {
      compute_error(cctx, err[0] ? err : "primary session compaction failed");
      compute_ctx_free(cctx);
      return;
   }

   if (session_id[0])
      stream_event(cctx, "session", "id", session_id);
   if (result.compacted)
   {
      char msg[256];
      snprintf(msg, sizeof(msg), "Context compacted: %d to %d messages.", result.messages_before,
               result.messages_after);
      stream_event(cctx, "text", "content", msg);
   }
   else
      stream_event(cctx, "text", "content", "Conversation is already compact enough.");
   stream_event(cctx, "turn_end", NULL, NULL);
   stream_event(cctx, "done", NULL, NULL);
   compute_ok(cctx);
   compute_ctx_free(cctx);
}

void chat_stream_worker(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   compute_ctx_begin_budget(cctx);
   cJSON *req = cctx->req;

   cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(req, "message");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jpsid = cJSON_GetObjectItemCaseSensitive(req, "provider_session_id");
   cJSON *jcsid = cJSON_GetObjectItemCaseSensitive(req, "claude_session_id");
   cJSON *jasid = cJSON_GetObjectItemCaseSensitive(req, "aimee_session_id");
   cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(req, "model");
   cJSON *jeffort = cJSON_GetObjectItemCaseSensitive(req, "reasoning_effort");
   cJSON *jcompact = cJSON_GetObjectItemCaseSensitive(req, "compact");
   if (!cJSON_IsString(jeffort))
      jeffort = cJSON_GetObjectItemCaseSensitive(req, "effort");
   if (!cJSON_IsString(jeffort))
      jeffort = cJSON_GetObjectItemCaseSensitive(req, "model_reasoning_effort");

   const char *message = cJSON_IsString(jmsg) ? jmsg->valuestring : "";
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";
   const char *provider_sid = cJSON_IsString(jpsid) ? jpsid->valuestring : "";
   if (!provider_sid[0] && cJSON_IsString(jcsid))
      provider_sid = jcsid->valuestring;
   const char *aimee_sid = cJSON_IsString(jasid) ? jasid->valuestring : "";
   const char *model_override = cJSON_IsString(jmodel) ? jmodel->valuestring : "";
   const char *effort_override = cJSON_IsString(jeffort) ? jeffort->valuestring : "";
   int compact = cJSON_IsTrue(jcompact);

   if (!message[0] && !compact)
   {
      compute_error(cctx, "missing message");
      compute_ctx_free(cctx);
      return;
   }

   config_t cfg;
   config_load(&cfg);
   const char *provider = cfg.provider[0] ? cfg.provider : "claude";
   /* A session-pinned primary agent (set via POST /v1/sessions/<id>/primary)
    * overrides the global default provider for this session, so the active
    * primary can be switched at runtime without touching durable config. */
   char primary_buf[MAX_AGENT_NAME];
   if (aimee_sid && aimee_sid[0] &&
       session_primary_get(aimee_sid, primary_buf, sizeof(primary_buf)) && primary_buf[0])
      provider = primary_buf;
   if (chat_provider_uses_codex_cli(provider))
   {
      compute_ctx_release_budget(cctx);
      if (compact)
         chat_stream_worker_codex_compact(cctx, provider_sid, cwd, &cfg);
      else
         chat_stream_worker_codex(cctx, message, provider_sid, cwd, model_override, effort_override,
                                  &cfg);
      return;
   }
   if (strcmp(provider, "claude") != 0 && strcmp(provider, "claude-code") != 0)
   {
      compute_ctx_release_budget(cctx);
      if (chat_provider_uses_primary_session(provider, &cfg))
      {
         if (compact)
            chat_stream_worker_primary_session_compact(cctx, provider_sid, aimee_sid, provider,
                                                       model_override, &cfg);
         else
            chat_stream_worker_primary_session(cctx, message, provider_sid, cwd, aimee_sid,
                                               provider, model_override, &cfg);
      }
      else
      {
         if (compact)
         {
            compute_error(cctx, "conversation compaction is not supported for this provider");
            compute_ctx_free(cctx);
         }
         else
            chat_stream_worker_agent(cctx, message, cwd, aimee_sid, provider, model_override, &cfg);
      }
      return;
   }

   if (compact)
   {
      compute_error(cctx, "conversation compaction is not supported for claude CLI chat");
      compute_ctx_free(cctx);
      return;
   }

   /* Detached (thin-client) workspace: the `claude` CLI, its login, tmux, and the
    * working tree live on the CLIENT, not this (possibly containerized) server.
    * Run the STANDARD `claude` CLI over tmux on the client — the cli_session
    * driver marshals its tmux commands over the reverse channel — rather than
    * the local `claude -p` path below. Co-located turns fall through to the
    * local claude path. */
   {
      int detached_bound = workspace_turn_bind_active(cwd);
      const workspace_provider_t *wsp = workspace_provider_active();
      if (detached_bound && wsp && wsp->kind == WS_PROVIDER_DETACHED && wsp->exec_shell)
      {
         if (aimee_path_is_absolute(cwd) && !strstr(cwd, "/.."))
            run_cmd_set_cwd(cwd);
         if (aimee_sid && aimee_sid[0])
            session_id_set_override(aimee_sid);

         char *sys = read_webchat_system_prompt(cctx);
         agent_t cag;
         memset(&cag, 0, sizeof(cag));
         snprintf(cag.name, sizeof(cag.name), "claude");
         snprintf(cag.backend, sizeof(cag.backend), "%s", AGENT_BACKEND_TMUX_CLI);
         snprintf(cag.cli_kind, sizeof(cag.cli_kind), "claude");
         if (cfg.claude_model[0])
            snprintf(cag.cli_cmd, sizeof(cag.cli_cmd), "claude --model %s", cfg.claude_model);
         else
            snprintf(cag.cli_cmd, sizeof(cag.cli_cmd), "claude");
         cag.session_reuse = 1;

         stream_event(cctx, "turn_start", NULL, NULL);
         agent_result_t result;
         memset(&result, 0, sizeof(result));
         int rc = agent_execute_cli_session(&cag, NULL, sys ? sys : "", message,
                                            AGENT_DEFAULT_MAX_TOKENS, 0.3, &result);

         session_id_clear_override();
         workspace_turn_unbind_active();
         run_cmd_set_cwd(NULL);
         free(sys);

         if (rc != 0)
         {
            compute_error(cctx, result.error[0] ? result.error : "claude tmux session failed");
            free(result.response);
            compute_ctx_free(cctx);
            return;
         }
         if (result.response && result.response[0])
            stream_event(cctx, "text", "content", result.response);
         stream_event(cctx, "turn_end", NULL, NULL);
         stream_event(cctx, "done", NULL, NULL);
         free(result.response);
         compute_ok(cctx);
         compute_ctx_free(cctx);
         return;
      }
      workspace_turn_unbind_active();
   }

   const char *claude_sid = provider_sid;
   compute_pool_set_job(POOL_JOB_CHAT, "provider=%s sid=%s", provider,
                        claude_sid && claude_sid[0] ? claude_sid : "new");

   char *system_prompt = read_webchat_system_prompt(cctx);

   char *argv[32];
   int argc = 0;
   argv[argc++] = "claude";
   argv[argc++] = "-p";
   argv[argc++] = "--output-format";
   argv[argc++] = "stream-json";
   argv[argc++] = "--verbose";
   argv[argc++] = "--include-partial-messages";
   /* Do not whitelist tools — allow all configured tools (including aimee MCP)
    * so the primary-agent chat has the full aimee toolset available. Only
    * disallow tools that would deadlock or recurse in an unattended context. */
   argv[argc++] = "--disallowedTools";
   argv[argc++] = "AskUserQuestion,Agent,RemoteTrigger";
   if (system_prompt && system_prompt[0])
   {
      argv[argc++] = "--append-system-prompt";
      argv[argc++] = system_prompt;
   }
   const char *claude_model =
       chat_model_passthrough_allowed(model_override) ? model_override : cfg.claude_model;
   if (claude_model && claude_model[0])
   {
      argv[argc++] = "--model";
      argv[argc++] = (char *)claude_model;
   }
   int claude_effort_explicit = chat_claude_effort_allowed(effort_override);
   const char *claude_effort =
       claude_effort_explicit ? effort_override : cfg.model_reasoning_effort;
   /* §5: cap (only lower) the config-derived effort by turn complexity; leave an
    * explicit per-request override untouched. */
   if (cfg.reasoning_cap_enabled && !claude_effort_explicit)
   {
      int score = reasoning_complexity_score(1, message ? strlen(message) : 0, 1);
      claude_effort = reasoning_effort_capped(claude_effort, score);
   }
   if (chat_claude_effort_allowed(claude_effort))
   {
      argv[argc++] = "--effort";
      argv[argc++] = (char *)claude_effort;
   }
   if (claude_sid[0])
   {
      argv[argc++] = "--resume";
      argv[argc++] = (char *)claude_sid;
   }
   if (cfg.autonomous)
      argv[argc++] = "--dangerously-skip-permissions";
   argv[argc] = NULL;

   /* Create pipes */
   int in_pipe[2] = {-1, -1};
   int out_pipe[2] = {-1, -1};
   if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0)
   {
      free(system_prompt);
      compute_error(cctx, "pipe failed");
      compute_ctx_free(cctx);
      return;
   }

   char err_path[64] = {0};
   snprintf(err_path, sizeof(err_path), "/tmp/aimee-claude-%d-XXXXXX", (int)getpid());
   int err_fd = mkstemp(err_path);
   if (err_fd < 0)
      err_path[0] = '\0';

   pid_t pid = fork();
   if (pid < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      if (err_fd >= 0)
      {
         close(err_fd);
         unlink(err_path);
      }
      free(system_prompt);
      compute_error(cctx, "fork failed");
      compute_ctx_free(cctx);
      return;
   }

   if (pid == 0)
   {
      /* Child */
#ifdef __linux__
      /* Die when the daemon dies, so a clean redeploy doesn't leave
       * orphaned shells running streaming sub-processes. */
      prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
      if (getppid() == 1)
         _exit(0);
#endif
      close(in_pipe[1]);
      close(out_pipe[0]);
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(in_pipe[0]);
      close(out_pipe[1]);
      if (err_fd >= 0)
      {
         dup2(err_fd, STDERR_FILENO);
         close(err_fd);
      }
      else
      {
         int devnull = open("/dev/null", O_WRONLY);
         if (devnull >= 0)
         {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
         }
      }
      if (cwd && cwd[0] && chdir(cwd) != 0)
      {
         dprintf(STDERR_FILENO, "aimee: chdir(%s) failed: %s\n", cwd, strerror(errno));
         _exit(126);
      }
      /* Expose the aimee session to the MCP server so it can resolve
       * session context via AIMEE_SESSION_ID rather than the PPID file
       * (which would be keyed to aimee-server's PID, not the CLI's). */
      if (aimee_sid && aimee_sid[0])
         setenv("AIMEE_SESSION_ID", aimee_sid, 1);
      char claude_bin_buf[1024];
      const char *claude_bin = resolve_claude_bin(claude_bin_buf, sizeof(claude_bin_buf));
      execvp(claude_bin, argv);
      dprintf(STDERR_FILENO, "aimee: exec(%s) failed: %s\n", claude_bin, strerror(errno));
      _exit(127);
   }

   /* Parent */
   close(in_pipe[0]);
   close(out_pipe[1]);
   if (err_fd >= 0)
      close(err_fd);
   free(system_prompt);

   /* Register the child with the per-turn cancel registry (this worker is the
    * sole reaper) and make the read end non-blocking so the poll-driven loop
    * below can re-check the cancel flag instead of blocking in read(). */
   turn_registry_set_child(cctx->turn_entry, pid);
   {
      int fl = fcntl(out_pipe[0], F_GETFL, 0);
      if (fl >= 0)
         fcntl(out_pipe[0], F_SETFL, fl | O_NONBLOCK);
   }

   /* Write message to stdin */
   size_t msg_len = strlen(message);
   size_t written = 0;
   while (written < msg_len)
   {
      ssize_t w = write(in_pipe[1], message + written, msg_len - written);
      if (w <= 0)
         break;
      written += (size_t)w;
   }
   close(in_pipe[1]);

   /* Release the compute budget slot now that the subprocess is running.
    * The read loop below is pure I/O; holding server compute budget here
    * would starve MCP/tool callbacks that the provider makes back into
    * aimee-server while it waits for their results. */
   compute_ctx_release_budget(cctx);

   /* Read stream-json output via a cancellable, non-blocking line reader.
    * NB: the loop no longer consults conn_alive — a dropped client connection
    * detaches but does NOT abort the turn (server-owned turn lifecycle); the
    * turn ends only on provider EOF or an explicit cancel. */
   char *line = malloc(CLAUDE_LINE_MAX);
   cli_linereader_t *lr = malloc(sizeof(*lr));
   if (!line || !lr)
   {
      free(line);
      free(lr);
      close(out_pipe[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      turn_registry_mark_reaped(cctx->turn_entry);
      compute_error(cctx, "out of memory");
      compute_ctx_free(cctx);
      return;
   }
   lr->len = 0;

   int had_tool_result = 0;
   int first_message = 1;
   int saw_result = 0;
   int saw_content = 0;
   int saw_delta_text = 0;
   int cancelled = 0;
   int eof = 0;
   char provider_error[1024] = "";

   while (cli_read_line(out_pipe[0], lr, cctx->turn_entry, line, CLAUDE_LINE_MAX, &cancelled, &eof))
   {
      cJSON *obj = cJSON_Parse(line);
      if (!obj)
         continue;

      cJSON *type_j = cJSON_GetObjectItem(obj, "type");
      const char *type = (type_j && cJSON_IsString(type_j)) ? type_j->valuestring : "";

      if (strcmp(type, "stream_event") == 0)
      {
         cJSON *event = cJSON_GetObjectItem(obj, "event");
         if (event)
         {
            cJSON *etype = cJSON_GetObjectItem(event, "type");
            const char *et = (etype && cJSON_IsString(etype)) ? etype->valuestring : "";

            if (strcmp(et, "message_start") == 0)
            {
               if (first_message || had_tool_result)
               {
                  stream_event(cctx, "turn_start", NULL, NULL);
                  first_message = 0;
               }
               had_tool_result = 0;
               saw_delta_text = 0;
            }
            else if (strcmp(et, "content_block_delta") == 0)
            {
               cJSON *delta = cJSON_GetObjectItem(event, "delta");
               cJSON *dt = delta ? cJSON_GetObjectItem(delta, "type") : NULL;
               const char *dts = (dt && cJSON_IsString(dt)) ? dt->valuestring : "";

               if (strcmp(dts, "text_delta") == 0)
               {
                  cJSON *text = cJSON_GetObjectItem(delta, "text");
                  if (text && cJSON_IsString(text) && text->valuestring[0])
                  {
                     saw_content = 1;
                     saw_delta_text = 1;
                     stream_event(cctx, "text", "content", text->valuestring);
                  }
               }
               else if (strcmp(dts, "thinking_delta") == 0)
               {
                  cJSON *text = cJSON_GetObjectItem(delta, "thinking");
                  if (text && cJSON_IsString(text) && text->valuestring[0])
                  {
                     saw_content = 1;
                     stream_event(cctx, "thinking", "content", text->valuestring);
                  }
               }
            }
            else if (strcmp(et, "message_stop") == 0)
            {
               stream_event(cctx, "turn_end", NULL, NULL);
            }
         }
      }
      else if (strcmp(type, "tool_result") == 0 || strcmp(type, "user") == 0)
      {
         had_tool_result = 1;
      }
      else if (strcmp(type, "assistant") == 0)
      {
         cJSON *message = cJSON_GetObjectItem(obj, "message");
         cJSON *content = message ? cJSON_GetObjectItem(message, "content")
                                  : cJSON_GetObjectItem(obj, "content");
         if (!saw_delta_text)
            chat_claude_stream_content(cctx, content, &saw_content);
         cJSON *err = cJSON_GetObjectItem(obj, "error");
         if (cJSON_IsString(err) && err->valuestring[0] && !provider_error[0])
            snprintf(provider_error, sizeof(provider_error), "%s", err->valuestring);
      }
      else if (strcmp(type, "result") == 0)
      {
         saw_result = 1;
         cJSON *sid = cJSON_GetObjectItem(obj, "session_id");
         if (sid && cJSON_IsString(sid))
            stream_event(cctx, "session", "id", sid->valuestring);
         cJSON *is_error = cJSON_GetObjectItem(obj, "is_error");
         cJSON *api_status = cJSON_GetObjectItem(obj, "api_error_status");
         if ((cJSON_IsBool(is_error) && cJSON_IsTrue(is_error)) ||
             (cJSON_IsNumber(api_status) && api_status->valuedouble > 0))
         {
            cJSON *result = cJSON_GetObjectItem(obj, "result");
            if (cJSON_IsString(result) && result->valuestring[0])
               snprintf(provider_error, sizeof(provider_error), "%s", result->valuestring);
            else if (!provider_error[0])
               snprintf(provider_error, sizeof(provider_error), "claude provider request failed");
         }
         cJSON *jusage = cJSON_GetObjectItem(obj, "usage");
         if (cJSON_IsObject(jusage))
         {
            token_usage_t tu;
            memset(&tu, 0, sizeof(tu));
            cJSON *jin = cJSON_GetObjectItem(jusage, "input_tokens");
            cJSON *jout = cJSON_GetObjectItem(jusage, "output_tokens");
            cJSON *jcw = cJSON_GetObjectItem(jusage, "cache_creation_input_tokens");
            cJSON *jcr = cJSON_GetObjectItem(jusage, "cache_read_input_tokens");
            if (cJSON_IsNumber(jin))
               tu.input_tokens = (int)jin->valuedouble;
            if (cJSON_IsNumber(jout))
               tu.output_tokens = (int)jout->valuedouble;
            if (cJSON_IsNumber(jcw))
               tu.cache_write_tokens = (int)jcw->valuedouble;
            if (cJSON_IsNumber(jcr))
               tu.cache_read_tokens = (int)jcr->valuedouble;
            double cost = token_estimate_cost(
                claude_model && claude_model[0] ? claude_model : cfg.claude_model, &tu);
            int in_total = tu.input_tokens + tu.cache_write_tokens + tu.cache_read_tokens;
            stream_event_usage(cctx, in_total, tu.output_tokens, cost);
         }
      }

      cJSON_Delete(obj);
   }

   (void)eof; /* natural EOF is the normal completion path */
   free(line);
   free(lr);
   close(out_pipe[0]);

   /* Single-reap: this worker is the sole reaper of its child (the racing
    * disconnect-kill is gone). On cancel, SIGTERM then a bounded wait then
    * SIGKILL; otherwise reap the naturally-exited child. EINTR is retried;
    * the reaped flag guards against any double-reap. */
   int status = 0;
   if (!cctx->turn_entry || !cctx->turn_entry->reaped)
   {
      if (cancelled && pid > 0)
      {
         kill(pid, SIGTERM);
         for (int _w = 0; _w < 50; _w++)
         {
            int _ws = 0;
            int wr = waitpid(pid, &_ws, WNOHANG);
            if (wr > 0)
            {
               status = _ws;
               pid = -1;
               break;
            }
            if (wr < 0 && errno != EINTR)
            {
               pid = -1;
               break;
            }
            usleep(100000); /* 100 ms per try, 5 s total */
         }
         if (pid > 0)
            kill(pid, SIGKILL);
      }
      if (pid > 0)
      {
         int wr;
         do
         {
            wr = waitpid(pid, &status, 0);
         } while (wr < 0 && errno == EINTR);
      }
      turn_registry_mark_reaped(cctx->turn_entry);
   }

   char *stderr_text = err_path[0] ? read_optional_text_file(err_path) : NULL;
   if (err_path[0])
      unlink(err_path);

   if (cancelled)
   {
      /* The turn was cancelled (session close / shutdown / graceful_cancel). */
      free(stderr_text);
      stream_event(cctx, "error", "message", "turn cancelled");
      compute_error(cctx, "turn cancelled");
      compute_ctx_free(cctx);
      return;
   }

   if (provider_error[0] || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
       (!saw_result && !saw_content))
   {
      char msg[1024];
      if (provider_error[0])
         snprintf(msg, sizeof(msg), "%s", provider_error);
      else if (!WIFEXITED(status))
         snprintf(msg, sizeof(msg), "claude exited abnormally%s%s",
                  stderr_text && stderr_text[0] ? ": " : "",
                  stderr_text && stderr_text[0] ? stderr_text : "");
      else if (WEXITSTATUS(status) != 0)
         snprintf(msg, sizeof(msg), "claude exited with status %d%s%s", WEXITSTATUS(status),
                  stderr_text && stderr_text[0] ? ": " : "",
                  stderr_text && stderr_text[0] ? stderr_text : "");
      else
         snprintf(msg, sizeof(msg), "claude exited without producing a result%s%s",
                  stderr_text && stderr_text[0] ? ": " : "",
                  stderr_text && stderr_text[0] ? stderr_text : "");
      stream_event(cctx, "error", "message", msg);
      free(stderr_text);
      compute_error(cctx, msg);
      compute_ctx_free(cctx);
      return;
   }
   free(stderr_text);
   stream_event(cctx, "done", NULL, NULL);
   compute_ok(cctx);
   compute_ctx_free(cctx);
}

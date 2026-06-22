/* server_compute.c: POSIX chat.send_stream worker for primary-agent streaming. */
#include "aimee.h"
#include "agent_adapter.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_tools.h" /* agent_tools_set_tool_event_cb — stream tool events */
#include "cli_codex.h"
#include "cli_session.h" /* cli_session_set_stream_cb — incremental tmux CLI streaming */
#include "config.h"
#include "db1.h"
#include "log.h"
#include "primary_session_adapter.h"
#include "workspace.h" /* session_isolation_target — per-session worktree redirect */
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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
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

static int chat_codex_effort_allowed(const char *effort)
{
   return effort && (strcmp(effort, "low") == 0 || strcmp(effort, "medium") == 0 ||
                     strcmp(effort, "high") == 0 || strcmp(effort, "xhigh") == 0);
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

/* Register a built-in tmux-CLI agent (claude/codex driven as a persistent TUI
 * over tmux) for an OAuth/CLI provider so webchat can resolve it even when no
 * agent was explicitly configured. `name`==`provider` so it is selectable by
 * either. cli_kind drives the response parser; cli_cmd is the launch command. */
static void chat_agent_add_builtin_tmux_cli(agent_config_t *acfg, const char *name,
                                            const char *cli_kind, const char *cli_cmd,
                                            const char *model)
{
   if (!acfg || acfg->agent_count >= MAX_AGENTS || agent_find(acfg, name) ||
       chat_agent_has_provider(acfg, name))
      return;

   agent_t *ag = &acfg->agents[acfg->agent_count++];
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, sizeof(ag->name), "%s", name);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "bearer");
   snprintf(ag->provider, sizeof(ag->provider), "%s", name);
   snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_TMUX_CLI);
   snprintf(ag->cli_kind, sizeof(ag->cli_kind), "%s", cli_kind);
   snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "%s", cli_cmd);
   /* The configured default model; agent_execute_cli_session appends it as
    * `--model` (claude/codex both accept it). A per-request model_override
    * replaces this in chat_stream_worker_agent before the turn runs. Without
    * this, the CLI launched with its own default, silently ignoring config. */
   if (model && model[0])
      snprintf(ag->model, sizeof(ag->model), "%s", model);
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
      snprintf(acfg->default_agent, sizeof(acfg->default_agent), "%s", name);
}

static void chat_agent_add_builtin_provider(agent_config_t *acfg, const char *provider,
                                            const config_t *cfg)
{
   if (!provider)
      return;
   const char *claude_model = cfg ? cfg->claude_model : "";
   const char *codex_model = cfg ? cfg->codex_model : "";
   if (strcmp(provider, "openai") == 0)
      chat_agent_add_legacy_openai(acfg, cfg);
   else if (strcmp(provider, "claude-code") == 0)
      chat_agent_add_builtin_tmux_cli(acfg, "claude-code", "claude-code", "claude", claude_model);
   else if (strcmp(provider, "claude") == 0 || strcmp(provider, "claude-oauth") == 0)
      chat_agent_add_builtin_tmux_cli(acfg, provider, "claude", "claude", claude_model);
   else if (strcmp(provider, "codex-oauth") == 0)
      chat_agent_add_builtin_tmux_cli(acfg, "codex-oauth", "codex", "codex", codex_model);
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

/* Incremental stream sink for tmux CLI turns: cli_session_recv calls this with
 * each newly produced chunk of the clean reply, which we forward as a `text`
 * SSE event so the webchat streams the answer as it is produced (instead of one
 * blob at turn end). `emitted` records that streaming delivered the reply, so
 * the worker skips the trailing full-text emit and avoids duplicating it. */
typedef struct
{
   compute_ctx_t *cctx;
   int emitted;
} chat_cli_stream_ctx_t;

static void chat_cli_stream_cb(const char *delta, void *ud)
{
   chat_cli_stream_ctx_t *c = (chat_cli_stream_ctx_t *)ud;
   if (!c || !delta || !delta[0])
      return;
   stream_event(c->cctx, "text", "content", delta);
   c->emitted = 1;
}

/* Cancel predicate for the tmux CLI driver: reflects this turn's registry cancel
 * flag (set by chat.interrupt / graceful_cancel / session close). Lets
 * cli_session_recv abort a wedged or steered generation promptly without
 * coupling cli_session.c to the turn registry. */
static int chat_cli_cancel_check(void *ud)
{
   (void)ud;
   return agent_request_cancelled();
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
   /* Plain local project (no detached/mirror remap): isolate this session in its
    * OWN sibling worktree on a per-session branch off the repo's default branch,
    * created on demand. Without this, concurrent sessions on the same project
    * share one checkout and clobber each other. No-op when cwd is not a git repo
    * or is already a managed worktree (session_isolation_target guards both). */
   char session_wt[MAX_PATH_LEN];
   if (!eff_cwd && !detached_bound && aimee_sid && aimee_sid[0] &&
       session_isolation_target(use_cwd, aimee_sid, session_wt, sizeof(session_wt),
                                1 /*create_if_missing*/))
      use_cwd = session_wt;
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
   /* Stream a tmux CLI turn's reply incrementally (no-op for non-tmux agents,
    * whose runtime never drives a cli_session). Save/restore any outer cb so a
    * nested turn on this thread can't leave a dangling context behind. */
   chat_cli_stream_ctx_t sctx = {cctx, 0};
   void *prev_stream_ud = NULL;
   cli_session_stream_cb_t prev_stream_cb = cli_session_get_stream_cb(&prev_stream_ud);
   cli_session_set_stream_cb(chat_cli_stream_cb, &sctx);
   /* Let the tmux CLI driver observe this turn's cancel flag so a steer/interrupt
    * stops the running generation promptly. Save/restore around any outer turn. */
   void *prev_cancel_ud = NULL;
   cli_session_cancel_cb_t prev_cancel_cb = cli_session_get_cancel_check(&prev_cancel_ud);
   cli_session_set_cancel_check(chat_cli_cancel_check, NULL);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = agent_run_with_tools(&acfg, "code", system_prompt ? system_prompt : "", message,
                                 AGENT_DEFAULT_MAX_TOKENS, &result);

   cli_session_set_cancel_check(prev_cancel_cb, prev_cancel_ud);
   cli_session_set_stream_cb(prev_stream_cb, prev_stream_ud);
   agent_tools_set_tool_event_cb(NULL, NULL);
   session_id_clear_override();
   workspace_turn_unbind_active();
   run_cmd_set_cwd(NULL);
   free(system_prompt);

   if (rc != 0)
   {
      /* A cancelled turn (steering/interrupt) ends quietly: the running CLI was
       * stopped with the conversation intact, and the steer continuation streams
       * as the next turn. Emit a clean turn boundary, not an error event. */
      if (agent_request_cancelled())
      {
         /* Mark the partial that already streamed as cut off, so the transcript
          * distinguishes a steered/interrupted reply from a complete one (the
          * steer continuation follows as the next turn). */
         stream_event(cctx, "text", "content", "\n\n_(interrupted)_");
         stream_event(cctx, "turn_end", NULL, NULL);
         stream_event(cctx, "done", NULL, NULL);
         free(result.response);
         /* compute_ok is a no-op for a server-initiated turn (compute_respond
          * early-returns on conn_fd < 0); the pooled wrapper still emits the
          * presence-ring turn_done that the webchat events stream listens for. */
         compute_ok(cctx);
         compute_ctx_free(cctx);
         return;
      }
      compute_error(cctx, result.error[0] ? result.error : "agent provider failed");
      free(result.response);
      compute_ctx_free(cctx);
      return;
   }

   /* If the reply already streamed incrementally, don't re-emit it whole. */
   if (!sctx.emitted && result.response && result.response[0])
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
   /* Plain local project (no detached/mirror remap): isolate this session in its
    * OWN sibling worktree on a per-session branch off the repo's default branch,
    * created on demand — so concurrent sessions on the same project never share a
    * checkout. No-op when cwd is not a git repo / already a managed worktree. */
   char session_wt[MAX_PATH_LEN];
   if (!eff_cwd && !detached_bound && aimee_sid && aimee_sid[0] &&
       session_isolation_target(use_cwd, aimee_sid, session_wt, sizeof(session_wt),
                                1 /*create_if_missing*/))
      use_cwd = session_wt;

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
   /* codex-oauth runs the codex CLI as a persistent TUI over tmux (1:1 per aimee
    * session) via the agent worker — NOT the one-shot app-server stream — so it
    * must NOT be claimed here. */
   if (strcmp(provider, "codex-oauth") == 0)
      return 0;
   if (strcmp(provider, "codex-cli") == 0)
      return 1;
   if (strcmp(provider, "codex") != 0)
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
   /* Everything else — claude-oauth / codex-oauth / claude / claude-code (the
    * persistent CLI TUIs, driven over tmux 1:1 per aimee session through the
    * agent worker -> agent_execute_cli_session) and the in-process HTTP API
    * providers (primary-session adapters). NEVER `claude -p`: a one-shot print
    * process cannot multiplex concurrent sessions into isolated persistent
    * panes, which is the whole point of a per-session tmux CLI session. */
   compute_ctx_release_budget(cctx);
   if (chat_provider_uses_primary_session(provider, &cfg))
   {
      if (compact)
         chat_stream_worker_primary_session_compact(cctx, provider_sid, aimee_sid, provider,
                                                    model_override, &cfg);
      else
         chat_stream_worker_primary_session(cctx, message, provider_sid, cwd, aimee_sid, provider,
                                            model_override, &cfg);
   }
   else if (compact)
   {
      compute_error(cctx, "conversation compaction is not supported for this provider");
      compute_ctx_free(cctx);
   }
   else
      chat_stream_worker_agent(cctx, message, cwd, aimee_sid, provider, model_override, &cfg);
   return;
}

/* server_compute_async.c: dedicated async lanes for tool.execute and chat.send_stream */
#include "server_compute_impl.h"
#include "aimee.h"
#include "agent_config.h" /* agent_set_request_codex_creds */
#include "json_fluent.h"  /* jo_ok, jo_str */
#include "agent_tools.h"
#include "compute_pool.h"
#include "guardrails.h"
#include "presence.h"
#include "turn_registry.h"
#include "log.h"
#include "workspace_turn.h"
#include "cJSON.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SERVER_TOOL_THREAD_MAX 4
#define SERVER_ASYNC_SLOT_MAX  SERVER_MAX_CONNECTIONS

typedef struct
{
   int active;
   char lane[16];
   char session_id[128];
   char provider_session_id[128];
   char descriptor[160];
   time_t started_at;
} async_slot_t;

static pthread_mutex_t g_chat_threads_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_chat_threads_idle = PTHREAD_COND_INITIALIZER;
static int g_chat_threads_active = 0;
static pthread_mutex_t g_tool_threads_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tool_threads_idle = PTHREAD_COND_INITIALIZER;
static int g_tool_threads_active = 0;
static pthread_mutex_t g_async_slots_lock = PTHREAD_MUTEX_INITIALIZER;
static async_slot_t g_async_slots[SERVER_ASYNC_SLOT_MAX];

static const char *async_request_session_id(cJSON *req, char *fallback, size_t fallback_len)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (cJSON_IsString(item) && item->valuestring[0])
      return item->valuestring;
   item = cJSON_GetObjectItemCaseSensitive(req, "aimee_session_id");
   if (cJSON_IsString(item) && item->valuestring[0])
      return item->valuestring;
   item = cJSON_GetObjectItemCaseSensitive(req, "claude_session_id");
   if (cJSON_IsString(item) && item->valuestring[0])
      return item->valuestring;
   item = cJSON_GetObjectItemCaseSensitive(req, "provider_session_id");
   if (cJSON_IsString(item) && item->valuestring[0])
      return item->valuestring;

   if (fallback && fallback_len > 0)
   {
      snprintf(fallback, fallback_len, "request-%p", (void *)req);
      return fallback;
   }
   return NULL;
}

static const char *json_string(cJSON *obj, const char *name)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
   return cJSON_IsString(item) ? item->valuestring : "";
}

static int async_slot_claim(const char *lane, compute_ctx_t *cctx)
{
   if (!lane || !cctx || !cctx->req)
      return -1;

   char session_id[128] = "";
   char provider_session_id[128] = "";
   char descriptor[160] = "";

   if (strcmp(lane, "tool") == 0)
   {
      const char *tool = json_string(cctx->req, "tool");
      const char *sid = json_string(cctx->req, "session_id");
      snprintf(session_id, sizeof(session_id), "%s", sid && sid[0] ? sid : "?");
      snprintf(descriptor, sizeof(descriptor), "tool=%s", tool && tool[0] ? tool : "?");
   }
   else
   {
      const char *asid = json_string(cctx->req, "aimee_session_id");
      const char *psid = json_string(cctx->req, "provider_session_id");
      const char *csid = json_string(cctx->req, "claude_session_id");
      const char *model = json_string(cctx->req, "model");
      int compact = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cctx->req, "compact"));
      if (!psid[0] && csid[0])
         psid = csid;
      snprintf(session_id, sizeof(session_id), "%s", asid && asid[0] ? asid : "?");
      snprintf(provider_session_id, sizeof(provider_session_id), "%s", psid && psid[0] ? psid : "");
      snprintf(descriptor, sizeof(descriptor), "%s%s%s", compact ? "compact" : "chat",
               model && model[0] ? " model=" : "", model && model[0] ? model : "");
   }

   pthread_mutex_lock(&g_async_slots_lock);
   int slot = -1;
   for (int i = 0; i < SERVER_ASYNC_SLOT_MAX; i++)
   {
      if (!g_async_slots[i].active)
      {
         slot = i;
         break;
      }
   }
   if (slot >= 0)
   {
      async_slot_t *s = &g_async_slots[slot];
      memset(s, 0, sizeof(*s));
      s->active = 1;
      s->started_at = time(NULL);
      snprintf(s->lane, sizeof(s->lane), "%s", lane);
      snprintf(s->session_id, sizeof(s->session_id), "%s", session_id);
      snprintf(s->provider_session_id, sizeof(s->provider_session_id), "%s", provider_session_id);
      snprintf(s->descriptor, sizeof(s->descriptor), "%s", descriptor);
   }
   pthread_mutex_unlock(&g_async_slots_lock);

   cctx->async_slot = slot;
   return slot;
}

static void async_slot_release(int slot)
{
   if (slot < 0 || slot >= SERVER_ASYNC_SLOT_MAX)
      return;
   pthread_mutex_lock(&g_async_slots_lock);
   memset(&g_async_slots[slot], 0, sizeof(g_async_slots[slot]));
   pthread_mutex_unlock(&g_async_slots_lock);
}

/* --- tool.execute worker --- */

static void tool_execute_worker(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   compute_ctx_begin_budget(cctx);
   cJSON *req = cctx->req;

   cJSON *jtool = cJSON_GetObjectItemCaseSensitive(req, "tool");
   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(req, "arguments");
   cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(req, "cwd");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jtimeout = cJSON_GetObjectItemCaseSensitive(req, "timeout_ms");

   const char *tool = cJSON_IsString(jtool) ? jtool->valuestring : "";
   const char *args = cJSON_IsString(jargs) ? jargs->valuestring : "{}";
   const char *cwd = cJSON_IsString(jcwd) ? jcwd->valuestring : "";
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : "unknown";
   int timeout_ms = cJSON_IsNumber(jtimeout) ? (int)jtimeout->valuedouble : 30000;

   compute_pool_set_job(POOL_JOB_TOOL, "tool=%s sess=%s", tool[0] ? tool : "?",
                        sid && sid[0] ? sid : "?");

   if (!tool[0])
   {
      compute_error(cctx, "missing tool");
      compute_ctx_free(cctx);
      return;
   }
   if (!is_safe_id(sid))
   {
      compute_error(cctx, "invalid session_id");
      compute_ctx_free(cctx);
      return;
   }

   /* If the tool's cwd is a registered `detached` workspace, bind that workspace's
    * provider so the tool's file/exec ops marshal to the serving client (just as
    * a chat turn does). AC #6: a remote peer with a raw foreign cwd that binds no
    * detached workspace is refused — the server must not open it on its own fs. */
   int detached_bound = workspace_turn_bind_active(cwd);
   int trusted_local = (cctx->conn_caps == (uint32_t)CAPS_ALL);
   if (workspace_turn_reject_foreign_cwd(detached_bound, trusted_local, cwd))
   {
      workspace_turn_unbind_active();
      compute_error(cctx, "workspace: a remote session must act within a registered `detached` "
                          "workspace; raw server-side path not accepted");
      compute_ctx_free(cctx);
      return;
   }

   /* Set thread-local CWD for tool execution (validate: absolute, no traversal).
    * A `mirror` workspace remaps into the server-side reconstructed worktree. */
   const char *eff_cwd = workspace_turn_active_cwd();
   const char *use_cwd = eff_cwd ? eff_cwd : cwd;
   if (aimee_path_is_absolute(use_cwd) && !strstr(use_cwd, "/../") && !strstr(use_cwd, "/.."))
      run_cmd_set_cwd(use_cwd);

   /* Guardrail pre-check */
   config_t cfg;
   config_load(&cfg);

   session_state_t state;
   session_state_load(&state, sid);

   char msg[1024] = "";
   int rc = pre_tool_check(tool, args, &state, config_guardrail_mode(&cfg), cwd, msg, sizeof(msg));
   session_state_save(&state, sid);

   if (rc != 0)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "blocked");
      cJSON_AddStringToObject(resp, "message", msg);
      cJSON_AddNumberToObject(resp, "exit_code", rc);
      compute_respond(cctx, resp);
      run_cmd_set_cwd(NULL);
      workspace_turn_unbind_active();
      compute_ctx_free(cctx);
      return;
   }

   /* Execute tool */
   char *result = dispatch_tool_call(tool, args, timeout_ms);

   /* Clear thread-local CWD + the detached provider binding */
   run_cmd_set_cwd(NULL);
   workspace_turn_unbind_active();

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "result", result ? result : "");
   free(result);
   compute_respond(cctx, resp);
   compute_ctx_free(cctx);
}

static int tool_thread_limit(server_ctx_t *ctx)
{
   return ctx && ctx->session_threads > 0 ? ctx->session_threads : CONFIG_DEFAULT_SESSION_THREADS;
}

static int tool_thread_reserve(server_ctx_t *ctx)
{
   (void)ctx;
   pthread_mutex_lock(&g_tool_threads_lock);
   g_tool_threads_active++;
   pthread_mutex_unlock(&g_tool_threads_lock);
   return 0;
}

static void tool_thread_release(void)
{
   pthread_mutex_lock(&g_tool_threads_lock);
   if (g_tool_threads_active > 0)
      g_tool_threads_active--;
   if (g_tool_threads_active == 0)
      pthread_cond_broadcast(&g_tool_threads_idle);
   pthread_mutex_unlock(&g_tool_threads_lock);
}

static void tool_thread_drain(void)
{
   pthread_mutex_lock(&g_tool_threads_lock);
   while (g_tool_threads_active > 0)
      pthread_cond_wait(&g_tool_threads_idle, &g_tool_threads_lock);
   pthread_mutex_unlock(&g_tool_threads_lock);
}

static void tool_execute_worker_pooled(void *arg)
{
   int async_slot = ((compute_ctx_t *)arg)->async_slot;
   tool_execute_worker(arg);
   async_slot_release(async_slot);
   tool_thread_release();
}

int (*g_tool_dispatch_override)(compute_ctx_t *cctx) = NULL;

int tool_execute_dispatch(server_ctx_t *ctx, compute_ctx_t *cctx)
{
   if (g_tool_dispatch_override)
      return g_tool_dispatch_override(cctx);

   char fallback[64];
   const char *sid = async_request_session_id(cctx->req, fallback, sizeof(fallback));
   cctx->compute_executor_threads =
       ctx && ctx->session_threads > 0 ? ctx->session_threads : CONFIG_DEFAULT_SESSION_THREADS;
   if (tool_thread_reserve(ctx) != 0)
      return -1;

   async_slot_claim("tool", cctx);

   if (server_session_pool_submit(ctx, sid, tool_execute_worker_pooled, cctx, NULL) != 0)
   {
      async_slot_release(cctx->async_slot);
      cctx->async_slot = -1;
      tool_thread_release();
      cctx->compute_executor_threads = 0;
      return -1;
   }
   return 0;
}

/* --- chat.send_stream worker --- */

/* chat_stream_worker is implemented in posix/server_compute.c and windows/server_compute.c. */

/* Streaming providers can make MCP/tool callbacks into aimee-server while
 * their primary request is still blocked on provider I/O. If those streams
 * occupy the fixed compute pool, the callbacks queue behind them and both
 * sides wait forever. Run chat streams on tracked pthreads so the compute
 * pool remains available for callbacks.
 *
 * Chat streams run on the session's dedicated pool. That gives each aimee
 * session its own bounded lanes while preserving independent progress across
 * sessions. */
static int chat_thread_limit(server_ctx_t *ctx)
{
   return ctx && ctx->session_threads > 0 ? ctx->session_threads : CONFIG_DEFAULT_SESSION_THREADS;
}

static int chat_thread_reserve(server_ctx_t *ctx)
{
   (void)ctx;
   pthread_mutex_lock(&g_chat_threads_lock);
   g_chat_threads_active++;
   pthread_mutex_unlock(&g_chat_threads_lock);
   return 0;
}

static void chat_thread_release(void)
{
   pthread_mutex_lock(&g_chat_threads_lock);
   if (g_chat_threads_active > 0)
      g_chat_threads_active--;
   if (g_chat_threads_active == 0)
      pthread_cond_broadcast(&g_chat_threads_idle);
   pthread_mutex_unlock(&g_chat_threads_lock);
}

static void chat_thread_drain(void)
{
   pthread_mutex_lock(&g_chat_threads_lock);
   while (g_chat_threads_active > 0)
      pthread_cond_wait(&g_chat_threads_idle, &g_chat_threads_lock);
   pthread_mutex_unlock(&g_chat_threads_lock);
}

void server_compute_async_drain(void)
{
   chat_thread_drain();
   tool_thread_drain();
}

cJSON *server_compute_async_json(server_ctx_t *ctx)
{
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return NULL;

   cJSON_AddStringToObject(obj, "role", "async");
   cJSON_AddNumberToObject(obj, "chat_limit", chat_thread_limit(ctx));
   cJSON_AddNumberToObject(obj, "tool_limit", tool_thread_limit(ctx));

   pthread_mutex_lock(&g_chat_threads_lock);
   cJSON_AddNumberToObject(obj, "chat_active", g_chat_threads_active);
   pthread_mutex_unlock(&g_chat_threads_lock);

   pthread_mutex_lock(&g_tool_threads_lock);
   cJSON_AddNumberToObject(obj, "tool_active", g_tool_threads_active);
   pthread_mutex_unlock(&g_tool_threads_lock);

   cJSON *slots = cJSON_AddArrayToObject(obj, "slots");
   if (slots)
   {
      pthread_mutex_lock(&g_async_slots_lock);
      time_t now = time(NULL);
      for (int i = 0; i < SERVER_ASYNC_SLOT_MAX; i++)
      {
         async_slot_t *s = &g_async_slots[i];
         if (!s->active)
            continue;
         cJSON *slot = cJSON_CreateObject();
         if (!slot)
            continue;
         cJSON_AddNumberToObject(slot, "index", i);
         cJSON_AddBoolToObject(slot, "active", 1);
         cJSON_AddStringToObject(slot, "lane", s->lane);
         cJSON_AddStringToObject(slot, "session_id", s->session_id);
         if (s->provider_session_id[0])
            cJSON_AddStringToObject(slot, "provider_session_id", s->provider_session_id);
         cJSON_AddStringToObject(slot, "descriptor", s->descriptor);
         cJSON_AddNumberToObject(slot, "elapsed_secs", (double)(now - s->started_at));
         cJSON_AddItemToArray(slots, slot);
      }
      pthread_mutex_unlock(&g_async_slots_lock);
   }

   return obj;
}

/* Monotonic per-chat-turn id source for the presence-event stream. */
static atomic_ulong g_chat_turn_seq;

static void chat_stream_worker_pooled(void *arg)
{
   compute_ctx_t *cctx = (compute_ctx_t *)arg;
   int async_slot = cctx->async_slot;

   /* Copy what we need before chat_stream_worker runs — its sub-workers free
    * cctx (and its req) on completion.
    *
    * Turn boundaries on the presence-event stream (GET /v1/sessions/{id}/events)
    * come from one of two sources, never both:
    *   - Arbitration path (cctx->presence_locked): the turn lock was acquired in
    *     handle_chat_send_stream, which already published turn_started; we
    *     release it here, which publishes turn_done.
    *   - Default path: no attach_id / no presence, so we emit synthesized
    *     turn_started/turn_done bracketing events. presence_emit_* is a no-op
    *     when nothing is attached, so the ordinary single-connection path is
    *     unchanged. */
   int locked = cctx->presence_locked;
   char lock_session[128];
   char lock_turn[64];
   snprintf(lock_session, sizeof(lock_session), "%s", cctx->presence_session);
   snprintf(lock_turn, sizeof(lock_turn), "%s", cctx->presence_turn_id);

   char fallback[64];
   char sid[PRESENCE_SESSION_ID_MAX];
   snprintf(sid, sizeof(sid), "%s",
            async_request_session_id(cctx->req, fallback, sizeof(fallback)));
   char turn_id[48];
   snprintf(turn_id, sizeof(turn_id), "turn-%lu",
            (unsigned long)atomic_fetch_add(&g_chat_turn_seq, 1) + 1);

   /* Always mirror the full turn stream to the presence ring for any
    * presence-tracked session (not only when a 2nd surface is attached): the
    * ring is the durable source of truth so a dropped connection detaches
    * without losing the turn, and a reconnecting client replays from it. The
    * worker reads presence_session / presence_turn_id / presence_emit_deltas
    * from cctx; set them before it runs. On the locked path they were already
    * set by handle_chat_send_stream (real turn id). */
   const char *delta_session = locked ? lock_session : sid;
   const char *delta_turn = locked ? lock_turn : turn_id;
   if (delta_session[0])
   {
      cctx->presence_emit_deltas = 1;
      if (!locked)
      {
         snprintf(cctx->presence_session, sizeof(cctx->presence_session), "%s", delta_session);
         snprintf(cctx->presence_turn_id, sizeof(cctx->presence_turn_id), "%s", delta_turn);
      }
   }

   /* Register this turn in the per-turn cancel registry BEFORE turn_started /
    * dispatch, so a cancel arriving in the window right after turn_started is
    * never dropped. The worker caches cctx->turn_entry (CLI path polls it;
    * in-process path is driven via agent_set_request_cancel). NULL = collision
    * (should be impossible under the turn lock) or table full: run the turn
    * without a registry entry rather than failing it — the turn still has
    * intrinsic token/deadline bounds; the condition is logged. */
   turn_entry_t *cancel_entry = NULL;
   if (delta_session[0])
   {
      cancel_entry = turn_registry_publish(delta_session, delta_turn);
      if (cancel_entry)
         cctx->turn_entry = cancel_entry; /* owner is set inside publish, under the lock */
      else
         LOG_WARN("chat", "turn registry rejected session %s; turn not cancellable", delta_session);
   }
   agent_set_request_cancel(cancel_entry ? &cancel_entry->cancel : NULL);

   if (!locked && sid[0])
      presence_emit_turn_started(sid, turn_id);
   /* Per-turn credential context: the credential-session id (for the RAM
    * keyring the client pushed once per session — a dedicated field decoupled
    * from the chat session id) + any per-turn Codex creds (legacy direct push).
    * Empty/absent clears the thread-locals so a turn on this pooled thread can't
    * reuse a prior turn's creds. */
   {
      const char *cred_sid = jo_str(cctx->req, "cred_session_id", NULL);
      agent_set_request_session((cred_sid && cred_sid[0]) ? cred_sid : sid);
   }
   /* Clear any per-turn codex creds on this pooled thread; the vault is the source
    * now (delegate_credential_retry sets them from the vault) and the client no
    * longer pushes a codex token in the request body (P4b). */
   agent_set_request_codex_creds(NULL, NULL);
   /* WP-C.2c(3): carry the attested vault principal across the conn-decoupled
    * agent loop so a delegate this chat spawns reaches the user's vault — both a
    * same-thread delegate (via create_compute_ctx's thread-local fallback when its
    * loopback conn carries no restored identity) and a parallel fan-out delegate
    * (which inherits it via agent_request_creds_snapshot, now extended to capture
    * this thread-local). Two layers keep it from leaking across turns on this
    * pooled thread: (1) this set is UNCONDITIONAL at the top of every turn — an
    * empty cctx->vault_principal clears it, so the next turn never sees a prior
    * turn's value; (2) we clear it after the worker. chat_stream_worker is a plain
    * call with no longjmp/pthread_exit/cancellation in its tree, so the post-call
    * clear always runs on return; together the two make the guard robust. */
   agent_set_request_vault_principal(cctx->vault_principal);
   chat_stream_worker(arg);
   agent_set_request_vault_principal(NULL);
   agent_set_request_cancel(NULL);
   if (locked)
      presence_turn_release(lock_session, lock_turn);
   else if (sid[0])
      presence_emit_turn_done(sid, turn_id); /* turn_done reaches the ring even on cancel */
   /* Clear the cancel-registry entry after the worker reaped its child and
    * turn_done was published. cancel_entry is a LOCAL pointer into the static
    * turn registry table (NOT into cctx, which the worker already freed), so
    * this is not a use-after-free. */
   turn_registry_clear(cancel_entry);

   async_slot_release(async_slot);
   chat_thread_release();
}

int (*g_chat_dispatch_override)(compute_ctx_t *cctx) = NULL;

static const char *chat_skip_ws(const char *p)
{
   while (p && *p && isspace((unsigned char)*p))
      p++;
   return p;
}

static int chat_json_is_transcript_record(const cJSON *obj)
{
   if (!cJSON_IsObject(obj))
      return 0;
   cJSON *text = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, "text");
   cJSON *ts = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, "ts");
   return cJSON_IsString(text) && (cJSON_IsNumber(ts) || cJSON_IsString(ts));
}

static int chat_transcript_jsonl_record_count(const char *suffix)
{
   int records = 0;
   const char *p = chat_skip_ws(suffix);
   while (p && *p)
   {
      const char *end = NULL;
      cJSON *obj = cJSON_ParseWithOpts(p, &end, 0);
      if (!obj || !end || end == p)
      {
         cJSON_Delete(obj);
         return 0;
      }
      int is_record = chat_json_is_transcript_record(obj);
      cJSON_Delete(obj);
      if (!is_record)
         return 0;
      records++;
      p = chat_skip_ws(end);
   }
   return records;
}

static const char *chat_trim_boundary_before(const char *message, const char *boundary)
{
   while (boundary > message && isspace((unsigned char)boundary[-1]))
      boundary--;
   return boundary;
}

static const char *chat_find_valid_transcript_tail(const char *message, int *record_count)
{
   const char *p = message;
   if (record_count)
      *record_count = 0;

   while ((p = strstr(p, "{\"text\"")) != NULL)
   {
      if (p > message)
      {
         int records = chat_transcript_jsonl_record_count(p);
         if (records > 0)
         {
            if (record_count)
               *record_count = records;
            return p;
         }
      }
      p++;
   }

   return NULL;
}

static const char *chat_find_malformed_transcript_tail(const char *message)
{
   int records = 0;
   const char *json_tail = chat_find_valid_transcript_tail(message, &records);
   if (!json_tail || records <= 0 || json_tail <= message)
      return NULL;

   const char *line = json_tail;
   while (line > message && line[-1] != '\n' && line[-1] != '\r')
      line--;
   if (line <= message)
      return NULL;

   const char *prev_end = line;
   while (prev_end > message && (prev_end[-1] == '\n' || prev_end[-1] == '\r'))
      prev_end--;
   const char *prev_start = prev_end;
   while (prev_start > message && prev_start[-1] != '\n' && prev_start[-1] != '\r')
      prev_start--;

   if (!strstr(prev_start, "\",\"ts\":"))
      return NULL;

   const char *q = strstr(prev_start, "\":\"");
   if (!q || q <= message || q >= prev_end)
      return NULL;

   return q - 1;
}

static char *chat_sanitize_message_transcript_tail(const char *message)
{
   if (!message || !message[0])
      return NULL;

   const char *boundary = chat_find_malformed_transcript_tail(message);
   if (!boundary)
   {
      int records = 0;
      const char *json_tail = chat_find_valid_transcript_tail(message, &records);
      if (json_tail)
      {
         int preceded_by_line_break =
             json_tail > message && (json_tail[-1] == '\n' || json_tail[-1] == '\r');
         int preceded_by_ws = json_tail > message && isspace((unsigned char)json_tail[-1]);
         if (preceded_by_line_break || (preceded_by_ws && records > 1))
            boundary = json_tail;
      }
   }
   if (!boundary)
      return NULL;

   boundary = chat_trim_boundary_before(message, boundary);
   if (boundary <= message)
      return NULL;

   size_t len = (size_t)(boundary - message);
   char *clean = malloc(len + 1);
   if (!clean)
      return NULL;
   memcpy(clean, message, len);
   clean[len] = '\0';
   return clean;
}

static int chat_stream_dispatch(server_ctx_t *ctx, compute_ctx_t *cctx)
{
   if (g_chat_dispatch_override)
      return g_chat_dispatch_override(cctx);

   char fallback[64];
   const char *sid = async_request_session_id(cctx->req, fallback, sizeof(fallback));
   cctx->compute_executor_threads = chat_thread_limit(ctx);
   if (chat_thread_reserve(ctx) != 0)
      return -1;

   async_slot_claim("chat", cctx);

   if (server_session_pool_submit(ctx, sid, chat_stream_worker_pooled, cctx, NULL) != 0)
   {
      async_slot_release(cctx->async_slot);
      cctx->async_slot = -1;
      chat_thread_release();
      cctx->compute_executor_threads = 0;
      return -1;
   }
   return 0;
}

int handle_chat_send_stream(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *dispatch_req = req;
   cJSON *owned_req = NULL;
   char *clean_message = NULL;
   cJSON *jmessage = cJSON_GetObjectItemCaseSensitive(req, "message");
   if (cJSON_IsString(jmessage))
      clean_message = chat_sanitize_message_transcript_tail(jmessage->valuestring);
   if (clean_message)
   {
      owned_req = cJSON_Duplicate(req, 1);
      if (owned_req)
      {
         cJSON *replacement = cJSON_CreateString(clean_message);
         if (replacement)
         {
            cJSON_ReplaceItemInObjectCaseSensitive(owned_req, "message", replacement);
            dispatch_req = owned_req;
         }
      }
   }

   compute_ctx_t *cctx = create_compute_ctx(ctx, conn, dispatch_req);
   cJSON_Delete(owned_req);
   free(clean_message);
   if (!cctx)
      return server_send_error(conn, "out of memory", NULL);

   /* Unified-presence turn arbitration (opt-in). When the request identifies
    * its attachment and a presence exists for the session, serialize turns:
    * acquire the per-session turn lock before dispatch and decline with
    * presence_busy if another surface holds it. Requests without an attach_id
    * fall straight through and run as before (the pooled worker then emits the
    * synthesized turn boundary events instead). */
   const char *attach_id = json_string(req, "attach_id");
   if (attach_id[0])
   {
      char fb[64];
      const char *sid = async_request_session_id(cctx->req, fb, sizeof(fb));
      char turn_id[64] = "";
      char inflight[64] = "";
      /* Optional queueing: with "queue":true the submit waits (FIFO-fair) up to
       * queue_wait_ms for the in-flight turn to finish rather than being
       * declined immediately. Bounded + capped; the wait runs here on the
       * handler thread (pre-dispatch), never on a turn worker. Default off →
       * phase-4 immediate presence_busy. */
      cJSON *jq = cJSON_GetObjectItemCaseSensitive(req, "queue");
      int want_queue = cJSON_IsTrue(jq);
      cJSON *jw = cJSON_GetObjectItemCaseSensitive(req, "queue_wait_ms");
      int wait_ms = cJSON_IsNumber(jw) ? (int)jw->valuedouble : 30000;
      if (wait_ms < 0)
         wait_ms = 0;
      if (wait_ms > 120000)
         wait_ms = 120000;
      presence_turn_result_t tr =
          want_queue ? presence_turn_acquire_wait(sid, attach_id, wait_ms, turn_id, sizeof(turn_id),
                                                  inflight, sizeof(inflight))
                     : presence_turn_acquire(sid, attach_id, 0, turn_id, sizeof(turn_id), inflight,
                                             sizeof(inflight), NULL);
      if (tr == PRESENCE_TURN_BUSY)
      {
         compute_ctx_free(cctx);
         char msg[160];
         snprintf(msg, sizeof(msg),
                  "presence_busy: a turn (%s) is already in flight for this "
                  "session",
                  inflight[0] ? inflight : "unknown");
         return server_send_error(conn, msg, NULL);
      }
      if (tr == PRESENCE_TURN_ACQUIRED)
      {
         cctx->presence_locked = 1;
         snprintf(cctx->presence_session, sizeof(cctx->presence_session), "%s", sid ? sid : "");
         snprintf(cctx->presence_turn_id, sizeof(cctx->presence_turn_id), "%s", turn_id);
      }
      /* PRESENCE_TURN_ERR (no presence for the session / bad args): no lock —
       * proceed unarbitrated, exactly as a request without an attach_id. */
   }

   if (chat_stream_dispatch(ctx, cctx) != 0)
   {
      if (cctx->presence_locked)
         presence_turn_release(cctx->presence_session, cctx->presence_turn_id);
      compute_ctx_free(cctx);
      return server_send_error(conn, "chat worker unavailable", NULL);
   }

   return 0;
}

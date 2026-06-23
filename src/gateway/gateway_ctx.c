/* gateway_ctx.c: gateway runtime context and message handling.
 *
 * Connects inbound platform messages to the co-located aimee-server over its
 * /v1 HTTP surface (POST /v1/chat/stream for turns, first-class /v1 routes for launch.run
 * and other unary methods), then delivers streamed responses back through the
 * platform delivery router.
 */
#include "gateway_ctx.h"
#include "gateway_platform.h"
#include "delivery_router.h"
#include "stt.h"
#include "tts.h"
#include "mirror.h"
#include "gateway_pairing.h"
#include "log.h"
#include "aimee.h"
#include "aimee_home.h"
#include "cli_client.h"
#include <cJSON.h>
#include <unistd.h>

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- session registry ---- */

#define SESSION_MAX         256
#define SESSION_TTL_SECONDS 86400 /* 24 hours */

typedef struct
{
   char gateway_key[256];
   char aimee_sid[64];
   time_t last_used;
} session_entry_t;

struct gateway_ctx
{
   session_entry_t sessions[SESSION_MAX];
   int session_count;
   pthread_mutex_t session_mutex;
};

static const char *session_lookup_locked(gateway_ctx_t *ctx, const char *key)
{
   time_t now = time(NULL);
   for (int i = 0; i < ctx->session_count; i++)
   {
      if (strcmp(ctx->sessions[i].gateway_key, key) == 0)
      {
         if (now - ctx->sessions[i].last_used > SESSION_TTL_SECONDS)
         {
            ctx->sessions[i].gateway_key[0] = '\0';
            ctx->sessions[i].aimee_sid[0] = '\0';
            return NULL;
         }
         ctx->sessions[i].last_used = now;
         return ctx->sessions[i].aimee_sid;
      }
   }
   return NULL;
}

static void session_store_locked(gateway_ctx_t *ctx, const char *key, const char *sid)
{
   time_t now = time(NULL);
   for (int i = 0; i < SESSION_MAX; i++)
   {
      if (ctx->sessions[i].gateway_key[0] == '\0' ||
          now - ctx->sessions[i].last_used > SESSION_TTL_SECONDS)
      {
         snprintf(ctx->sessions[i].gateway_key, sizeof(ctx->sessions[i].gateway_key), "%s", key);
         snprintf(ctx->sessions[i].aimee_sid, sizeof(ctx->sessions[i].aimee_sid), "%s", sid);
         ctx->sessions[i].last_used = now;
         if (i >= ctx->session_count)
            ctx->session_count = i + 1;
         return;
      }
   }
   int oldest = 0;
   for (int i = 1; i < SESSION_MAX; i++)
      if (ctx->sessions[i].last_used < ctx->sessions[oldest].last_used)
         oldest = i;
   snprintf(ctx->sessions[oldest].gateway_key, sizeof(ctx->sessions[oldest].gateway_key), "%s",
            key);
   snprintf(ctx->sessions[oldest].aimee_sid, sizeof(ctx->sessions[oldest].aimee_sid), "%s", sid);
   ctx->sessions[oldest].last_used = now;
}

/* Drop the mapping for a key so the next message starts a fresh aimee session. */
static void session_forget_locked(gateway_ctx_t *ctx, const char *key)
{
   for (int i = 0; i < ctx->session_count; i++)
   {
      if (strcmp(ctx->sessions[i].gateway_key, key) == 0)
      {
         ctx->sessions[i].gateway_key[0] = '\0';
         ctx->sessions[i].aimee_sid[0] = '\0';
         return;
      }
   }
}

/* ---- streaming accumulator ---- */

typedef struct
{
   char *buf;
   size_t len;
   size_t cap;
} stream_buf_t;

static int stream_text_cb(cJSON *event, void *userdata)
{
   stream_buf_t *sb = (stream_buf_t *)userdata;
   cJSON *jtext = cJSON_GetObjectItemCaseSensitive(event, "text");
   if (!cJSON_IsString(jtext))
      return 0;
   const char *chunk = jtext->valuestring;
   size_t chunk_len = strlen(chunk);
   if (sb->len + chunk_len + 1 > sb->cap)
   {
      size_t need = sb->len + chunk_len + 1;
      if (need < sb->len)
         return -1;
      size_t new_cap = (need <= (size_t)-1 / 2) ? need * 2 : need;
      char *newbuf = realloc(sb->buf, new_cap);
      if (!newbuf)
         return -1;
      sb->buf = newbuf;
      sb->cap = new_cap;
   }
   memcpy(sb->buf + sb->len, chunk, chunk_len);
   sb->len += chunk_len;
   sb->buf[sb->len] = '\0';
   return 0;
}

/* Co-located /v1 HTTP endpoint ("unix:<home>/aimee-http.sock"). The gateway runs
 * alongside aimee-server and reaches it over the local /v1 surface now that the
 * NDJSON RPC socket is gone. Returns 0 on success. */
static int gw_v1_endpoint(char *out, size_t n)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   if (snprintf(out, n, "unix:%s/aimee-http.sock", home) >= (int)n)
      return -1;
   return 0;
}

static int rpc_launch_run(char *sid_out, size_t sid_cap)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "launch.run");
   cJSON_AddStringToObject(req, "cwd", "/");
   cJSON *resp = cli_v1_dispatch_local(req, 15000);
   cJSON_Delete(req);
   if (!resp)
   {
      LOG_WARN("gateway", "launch.run: no response from server");
      return -1;
   }
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(resp, "session_id");
   if (!cJSON_IsString(jsid) || !jsid->valuestring[0])
   {
      LOG_WARN("gateway", "launch.run: no session_id in response");
      cJSON_Delete(resp);
      return -1;
   }
   snprintf(sid_out, sid_cap, "%s", jsid->valuestring);
   cJSON_Delete(resp);
   return 0;
}

static char *rpc_chat_send_stream(const char *aimee_sid, const char *message)
{
   stream_buf_t sb;
   memset(&sb, 0, sizeof(sb));
   sb.buf = malloc(512);
   if (!sb.buf)
      return NULL;
   sb.buf[0] = '\0';
   sb.cap = 512;
   char endpoint[4200];
   if (gw_v1_endpoint(endpoint, sizeof(endpoint)) != 0)
   {
      free(sb.buf);
      return NULL;
   }
   cJSON *req = cJSON_CreateObject();
   /* No "method" — the POST /v1/chat/stream route maps to chat.send_stream. */
   cJSON_AddStringToObject(req, "message", message);
   cJSON_AddStringToObject(req, "aimee_session_id", aimee_sid);
   cJSON_AddStringToObject(req, "cwd", "/");
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   int status = 0;
   cJSON *resp =
       body ? cli_http_request_stream_ndjson(endpoint, "POST", "/v1/chat/stream", body, NULL,
                                             120000, &status, stream_text_cb, &sb, NULL, NULL)
            : NULL;
   free(body);
   cJSON_Delete(resp);
   if (sb.len == 0)
   {
      free(sb.buf);
      return NULL;
   }
   return sb.buf;
}

gateway_ctx_t *gateway_ctx_new(void)
{
   gateway_ctx_t *ctx = calloc(1, sizeof(gateway_ctx_t));
   if (!ctx)
      return NULL;
   pthread_mutex_init(&ctx->session_mutex, NULL);
   return ctx;
}

void gateway_ctx_free(gateway_ctx_t *ctx)
{
   if (!ctx)
      return;
   for (int i = 0; i < gateway_platform_count(); i++)
   {
      platform_adapter_t *a = gateway_platform_get(i);
      if (a && a->shutdown)
         a->shutdown(a);
   }
   pthread_mutex_destroy(&ctx->session_mutex);
   free(ctx);
}

/* Reply helper: deliver a one-shot message back to the originating channel. */
static void reply_origin(gateway_ctx_t *ctx, const session_source_t *src, const char *text)
{
   delivery_target_t origin;
   memset(&origin, 0, sizeof(origin));
   snprintf(origin.platform, sizeof(origin.platform), "%s", src->platform);
   snprintf(origin.chat_id, sizeof(origin.chat_id), "%s", src->chat_id);
   if (src->thread_id[0])
      snprintf(origin.thread_id, sizeof(origin.thread_id), "%s", src->thread_id);
   gateway_send_text(ctx, &origin, text);
   mirror_record(src->platform, src->chat_id, "out", text);
}

/* Returns 1 if the attachment path looks like an audio clip worth transcribing. */
static int attachment_is_audio(const attachment_t *a)
{
   if (!a)
      return 0;
   if (a->mime[0] && strncmp(a->mime, "audio/", 6) == 0)
      return 1;
   const char *p = a->path;
   const char *dot = strrchr(p, '.');
   if (!dot)
      return 0;
   return strcmp(dot, ".oga") == 0 || strcmp(dot, ".ogg") == 0 || strcmp(dot, ".mp3") == 0 ||
          strcmp(dot, ".wav") == 0 || strcmp(dot, ".m4a") == 0 || strcmp(dot, ".flac") == 0;
}

int gateway_parse_pr_command(const char *args, char *ws, size_t ws_cap, char *title,
                             size_t title_cap)
{
   if (ws && ws_cap)
      ws[0] = '\0';
   if (title && title_cap)
      title[0] = '\0';
   if (!args || !ws || ws_cap == 0)
      return -1;
   while (*args == ' ')
      args++;
   if (!*args)
      return -1; /* no workspace token */
   size_t i = 0;
   for (; args[i] && args[i] != ' ' && i < ws_cap - 1; i++)
      ws[i] = args[i];
   ws[i] = '\0';
   /* advance past the workspace token (even if it overflowed ws_cap) + spaces */
   const char *rest = args;
   while (*rest && *rest != ' ')
      rest++;
   while (*rest == ' ')
      rest++;
   if (title && title_cap)
      snprintf(title, title_cap, "%s", rest);
   return 0;
}

/* Resolve a registered workspace by exact path or basename, via workspace.list.
 * Writes the full server-side path into out. Returns 0 on a match, -1 otherwise. */
static int gw_resolve_workspace(const char *name, char *out, size_t out_cap)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "workspace.list");
   cJSON *resp = cli_v1_dispatch_local(req, 10000);
   cJSON_Delete(req);

   int found = -1;
   if (resp)
   {
      cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "workspaces");
      cJSON *ws;
      cJSON_ArrayForEach(ws, arr)
      {
         cJSON *p = cJSON_GetObjectItemCaseSensitive(ws, "path");
         if (!cJSON_IsString(p) || !p->valuestring[0])
            continue;
         const char *path = p->valuestring;
         const char *base = strrchr(path, '/');
         base = base ? base + 1 : path;
         if (strcmp(path, name) == 0 || strcmp(base, name) == 0)
         {
            snprintf(out, out_cap, "%s", path);
            found = 0;
            break;
         }
      }
      cJSON_Delete(resp);
   }
   return found;
}

/* `/pr <workspace> [title]`: open a PR for the current branch of a registered
 * (server-resident) workspace, deterministically and without an LLM turn —
 * the §6 "a filesystem-poor surface drives a build/PR on an instance-held
 * workspace" path. git_pr runs the verify gate (build) and authenticates via the
 * workspace's brokered / server-held forge identity. (A `mirror`-kind
 * workspace's reconstructed worktree is a follow-up — that needs the work_dir,
 * not the registered client path.) */
static void handle_pr_command(gateway_ctx_t *ctx, const session_source_t *src, const char *text)
{
   char wsname[256], title[512];
   if (gateway_parse_pr_command(text + 3, wsname, sizeof(wsname), title, sizeof(title)) != 0)
   {
      reply_origin(ctx, src, "usage: /pr <workspace> [title]");
      return;
   }
   char default_title[160];
   if (!title[0])
   {
      snprintf(default_title, sizeof(default_title), "PR from %s via aimee", src->platform);
      snprintf(title, sizeof(title), "%s", default_title);
   }

   char wspath[MAX_PATH_LEN];
   if (gw_resolve_workspace(wsname, wspath, sizeof(wspath)) != 0)
   {
      reply_origin(ctx, src,
                   "Unknown workspace. Use a registered workspace name (see `aimee workspace "
                   "list`).");
      return;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.call");
   cJSON_AddStringToObject(req, "tool", "git_pr");
   cJSON_AddStringToObject(req, "cwd", wspath);
   cJSON *a = cJSON_AddObjectToObject(req, "arguments");
   cJSON_AddStringToObject(a, "action", "create");
   cJSON_AddStringToObject(a, "title", title);
   cJSON *resp = cli_v1_dispatch_local(req, 300000); /* gh pr create + verify gate */
   cJSON_Delete(req);

   const char *reply = "PR command sent (no response from server).";
   char buf[1024];
   if (resp)
   {
      cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
      cJSON *item = content ? cJSON_GetArrayItem(content, 0) : NULL;
      cJSON *t = item ? cJSON_GetObjectItemCaseSensitive(item, "text") : NULL;
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(t) && t->valuestring[0])
      {
         snprintf(buf, sizeof(buf), "%s", t->valuestring);
         reply = buf;
      }
      else if (cJSON_IsString(msg) && msg->valuestring[0])
      {
         snprintf(buf, sizeof(buf), "%s", msg->valuestring);
         reply = buf;
      }
   }
   reply_origin(ctx, src, reply);
   cJSON_Delete(resp);
}

/* `/build <workspace>`: run the project's verify (build/test) gate for a
 * registered (server-resident) workspace, deterministically and without an LLM
 * turn, and relay the pass/fail summary. This is the read-only counterpart of
 * `/pr` — it issues the same git_verify gate that the rest of the system uses
 * but does NOT open a PR. Workspace -> cwd resolution mirrors `/pr`, so it
 * inherits the same mirror/detached cwd handling. */
static void handle_build_command(gateway_ctx_t *ctx, const session_source_t *src, const char *text)
{
   char wsname[256], ignore[512];
   if (gateway_parse_pr_command(text + 6, wsname, sizeof(wsname), ignore, sizeof(ignore)) != 0)
   {
      reply_origin(ctx, src, "usage: /build <workspace>");
      return;
   }

   char wspath[MAX_PATH_LEN];
   if (gw_resolve_workspace(wsname, wspath, sizeof(wspath)) != 0)
   {
      reply_origin(ctx, src,
                   "Unknown workspace. Use a registered workspace name (see `aimee workspace "
                   "list`).");
      return;
   }

   cli_conn_t conn;
   /* The co-located server socket is filesystem-trusted; the separate
    * NDJSON authenticate step was removed in the /v1-only cutover (#2708). */
   if (cli_connect(&conn, NULL) != 0)
   {
      reply_origin(ctx, src, "Could not reach aimee-server.");
      return;
   }
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.call");
   cJSON_AddStringToObject(req, "tool", "git_verify");
   cJSON_AddStringToObject(req, "cwd", wspath);
   cJSON_AddObjectToObject(req, "arguments");
   cJSON *resp = cli_request(&conn, req, 300000); /* verify gate (build/test) */
   cJSON_Delete(req);
   cli_close(&conn);

   const char *reply = "Build command sent (no response from server).";
   char buf[1024];
   if (resp)
   {
      cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
      cJSON *item = content ? cJSON_GetArrayItem(content, 0) : NULL;
      cJSON *t = item ? cJSON_GetObjectItemCaseSensitive(item, "text") : NULL;
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(t) && t->valuestring[0])
      {
         snprintf(buf, sizeof(buf), "%s", t->valuestring);
         reply = buf;
      }
      else if (cJSON_IsString(msg) && msg->valuestring[0])
      {
         snprintf(buf, sizeof(buf), "%s", msg->valuestring);
         reply = buf;
      }
   }
   reply_origin(ctx, src, reply);
   cJSON_Delete(resp);
}

/* Dispatch a recognised slash command. Returns 1 if handled (caller returns),
 * 0 if the text is not a slash command and should flow to the agent. */
static int handle_slash_command(gateway_ctx_t *ctx, const session_source_t *src,
                                const char *session_key, const char *text)
{
   if (!text || text[0] != '/')
      return 0;

   /* Match a command word up to the first space. */
   char cmd[32] = {0};
   size_t i = 0;
   for (; text[i] && text[i] != ' ' && i < sizeof(cmd) - 1; i++)
      cmd[i] = text[i];

   if (strcmp(cmd, "/stop") == 0)
   {
      pthread_mutex_lock(&ctx->session_mutex);
      const char *sid = session_lookup_locked(ctx, session_key);
      char sid_copy[64] = {0};
      if (sid)
         snprintf(sid_copy, sizeof(sid_copy), "%s", sid);
      pthread_mutex_unlock(&ctx->session_mutex);
      if (sid_copy[0])
      {
         cJSON *req = cJSON_CreateObject();
         cJSON_AddStringToObject(req, "method", "chat.graceful_cancel");
         cJSON_AddStringToObject(req, "aimee_session_id", sid_copy);
         cJSON_Delete(cli_v1_dispatch_local(req, 5000));
         cJSON_Delete(req);
      }
      reply_origin(ctx, src, "Cancelled.");
      return 1;
   }

   if (strcmp(cmd, "/new") == 0 || strcmp(cmd, "/reset") == 0)
   {
      pthread_mutex_lock(&ctx->session_mutex);
      session_forget_locked(ctx, session_key);
      pthread_mutex_unlock(&ctx->session_mutex);
      reply_origin(ctx, src, "Started a fresh session. Send your next message to begin.");
      return 1;
   }

   if (strcmp(cmd, "/pr") == 0)
   {
      handle_pr_command(ctx, src, text);
      return 1;
   }

   if (strcmp(cmd, "/build") == 0)
   {
      handle_build_command(ctx, src, text);
      return 1;
   }

   if (strcmp(cmd, "/retry") == 0 || strcmp(cmd, "/undo") == 0 || strcmp(cmd, "/skill") == 0 ||
       strcmp(cmd, "/personality") == 0)
   {
      reply_origin(ctx, src,
                   "That command is not available on this channel yet. Supported: "
                   "/stop, /new, /reset, /pr, /build.");
      return 1;
   }

   return 0; /* unknown slash text — let it flow to the agent */
}

int gateway_handle_message(gateway_ctx_t *ctx, const session_source_t *src, const char *text,
                           const attachment_t *attachments, int nattach)
{
   if (!ctx || !src || !src->platform[0])
      return -1;

   /* 1. Authorization. Unknown users are issued an out-of-band pairing code
    * the operator approves via `aimee gateway pair approve <code>`. */
   if (gateway_authorize_user(ctx, src) != 0)
   {
      LOG_INFO("gateway", "unauthorized message from %s/%s", src->platform, src->user_id);
      char code[16] = {0};
      char reply[256];
      if (src->user_id[0] &&
          gateway_pairing_issue_if_absent(src->platform, src->user_id, code, sizeof(code)) == 0)
         snprintf(reply, sizeof(reply),
                  "You're not paired yet. Your pairing code is %s. Ask the operator to run: "
                  "aimee gateway pair approve %s",
                  code, code);
      else
         snprintf(reply, sizeof(reply), "Unauthorized. Contact the operator to request access.");
      reply_origin(ctx, src, reply);
      return -1;
   }

   /* 2. Session key */
   char *_sk = gateway_session_key(src);
   if (!_sk)
   {
      LOG_WARN("gateway", "failed to build session key for %s/%s", src->platform, src->chat_id);
      return -1;
   }
   char session_key[256];
   snprintf(session_key, sizeof(session_key), "%s", _sk);
   free(_sk);

   /* 3. Transcribe a voice/audio attachment into the message text, when present. */
   char transcript[4096] = {0};
   const char *effective_text = text ? text : "";
   int voice_in = (nattach > 0 && attachments && attachment_is_audio(&attachments[0]));
   if (voice_in)
   {
      if (stt_transcribe_file(attachments[0].path, transcript, sizeof(transcript)) == 0 &&
          transcript[0])
      {
         if (stt_is_hallucination(transcript))
         {
            LOG_INFO("gateway", "dropping likely-hallucinated transcript for %s/%s", src->platform,
                     src->chat_id);
            reply_origin(ctx, src,
                         "I couldn't make out that voice message. Could you type it instead?");
            return 0;
         }
         effective_text = transcript;
      }
      else
      {
         LOG_WARN("gateway", "voice transcription failed for %s", attachments[0].path);
         reply_origin(ctx, src, "Voice transcription is unavailable. Please type your message.");
         return 0;
      }
   }

   /* Record the inbound message in the delivery mirror. */
   mirror_record(src->platform, src->chat_id, "in", effective_text[0] ? effective_text : "(empty)");

   /* 3b. Slash commands routed before agent dispatch. */
   if (handle_slash_command(ctx, src, session_key, effective_text))
      return 0;

   /* 4. The gateway reaches the co-located aimee-server over its /v1 surface
    * (each call below is a stateless POST to the local aimee-http.sock). */

   /* 5. Get or create aimee session */
   char aimee_sid[64] = {0};
   pthread_mutex_lock(&ctx->session_mutex);
   const char *cached = session_lookup_locked(ctx, session_key);
   if (cached)
      snprintf(aimee_sid, sizeof(aimee_sid), "%s", cached);
   pthread_mutex_unlock(&ctx->session_mutex);
   if (!aimee_sid[0])
   {
      if (rpc_launch_run(aimee_sid, sizeof(aimee_sid)) != 0)
         return -1;
      pthread_mutex_lock(&ctx->session_mutex);
      session_store_locked(ctx, session_key, aimee_sid);
      pthread_mutex_unlock(&ctx->session_mutex);
      LOG_INFO("gateway", "created session %s for key %s", aimee_sid, session_key);
   }

   /* 6. Build message. A non-audio attachment is noted by path; audio was
    * already transcribed into effective_text above. */
   const char *msg = effective_text;
   char *full_msg = NULL;
   if (nattach > 0 && attachments && attachments[0].path[0] &&
       !attachment_is_audio(&attachments[0]))
   {
      size_t extra = strlen(msg) + 128;
      full_msg = malloc(extra);
      if (full_msg)
      {
         snprintf(full_msg, extra, "[Attachment: %s]\n%s", attachments[0].path, msg);
         msg = full_msg;
      }
   }

   /* 7. Send turn, collect streamed response */
   char *response = rpc_chat_send_stream(aimee_sid, msg);
   free(full_msg);

   /* 8. Deliver response back and mirror it */
   delivery_target_t origin;
   memset(&origin, 0, sizeof(origin));
   snprintf(origin.platform, sizeof(origin.platform), "%s", src->platform);
   snprintf(origin.chat_id, sizeof(origin.chat_id), "%s", src->chat_id);
   if (src->thread_id[0])
      snprintf(origin.thread_id, sizeof(origin.thread_id), "%s", src->thread_id);
   if (response && response[0])
   {
      delivery_router_send(ctx, &origin, response, NULL, 0);
      mirror_record(src->platform, src->chat_id, "out", response);

      /* Voice-out (§13): when the operator sent a voice memo and TTS is
       * enabled, also deliver a spoken reply. Best-effort, never fatal. */
      if (voice_in && tts_enabled())
      {
         char *audio_path = NULL;
         if (tts_synthesize(response, &audio_path) == 0 && audio_path)
         {
            attachment_t voice_out;
            memset(&voice_out, 0, sizeof(voice_out));
            snprintf(voice_out.path, sizeof(voice_out.path), "%s", audio_path);
            const char *ext = strrchr(audio_path, '.');
            snprintf(voice_out.mime, sizeof(voice_out.mime), "%s",
                     (ext && strcmp(ext, ".mp3") == 0) ? "audio/mpeg" : "audio/wav");
            delivery_router_send(ctx, &origin, NULL, &voice_out, 1);
            unlink(audio_path);
            free(audio_path);
         }
         else
            LOG_WARN("gateway", "TTS synthesis failed for session %s", aimee_sid);
      }
   }
   else
      LOG_WARN("gateway", "empty response from session %s", aimee_sid);
   free(response);
   return 0;
}

int gateway_send_text(gateway_ctx_t *ctx, const delivery_target_t *target, const char *text)
{
   (void)ctx;
   if (!target || !text)
      return -1;
   platform_adapter_t *adapter = gateway_platform_find(target->platform);
   if (!adapter)
      return -1;
   return adapter->send_text(adapter, target, text);
}

int gateway_authorize_user(gateway_ctx_t *ctx, const session_source_t *src)
{
   (void)ctx;
   if (!src || !src->platform[0])
      return -1;
   platform_adapter_t *adapter = gateway_platform_find(src->platform);
   if (!adapter)
      return -1;
   /* Static env allowlist takes precedence (headless deployments). */
   if (adapter->authorize_user &&
       adapter->authorize_user(adapter, src->platform, src->chat_id, src->user_id) == 0)
      return 0;
   /* Otherwise honour an out-of-band DM pairing approved via the CLI. */
   if (src->user_id[0] && gateway_pairing_is_approved(src->platform, src->user_id))
      return 0;
   /* No env helper and no pairing record: allow only when neither gate is
    * configured (adapter has no authorize_user at all). */
   return adapter->authorize_user ? -1 : 0;
}

char *gateway_session_key(const session_source_t *src)
{
   if (!src)
      return NULL;
   char buf[512];
   int n;
   /* Thread id takes absolute priority over chat_type. */
   if (src->thread_id[0])
      n = snprintf(buf, sizeof(buf), "%s:thread:%s:%s", src->platform, src->chat_id,
                   src->thread_id);
   else if (strcmp(src->chat_type, "private") == 0 || !src->chat_type[0])
      n = snprintf(buf, sizeof(buf), "%s:dm:%s", src->platform, src->chat_id);
   else
      n = snprintf(buf, sizeof(buf), "%s:%s:%s", src->platform, src->chat_type, src->chat_id);
   if (n <= 0 || (size_t)n >= sizeof(buf))
      return NULL;
   return strdup(buf);
}

int gateway_set_typing(gateway_ctx_t *ctx, const delivery_target_t *target, int typing)
{
   (void)ctx;
   if (!target || !target->platform[0])
      return -1;
   platform_adapter_t *adapter = gateway_platform_find(target->platform);
   if (!adapter || !adapter->set_typing)
      return -1;
   return adapter->set_typing(adapter, target, typing);
}

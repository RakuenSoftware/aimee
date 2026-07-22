/* cli_chat_stream.c: headless /v1 chat streaming for non-interactive callers.
 *
 * Was cli_tui.c, which hosted the OpenCode frontend, the native fallback loop
 * and `aimee chat`. Those are gone and nothing here touches a terminal, so the
 * TUI name no longer described the file. acp-serve is the only caller. */
#include "aimee_home.h"
#include "cli_client.h"
#include "cli_agent_keys.h"
#include "cli_chat_stream.h"
#include "aimee_client.h"
#include "history.h"
#include "markdown.h"
#include "platform.h"
#include "platform_path.h"
#include "platform_process.h"
#include "session_compact.h"
#include "cJSON.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef AIMEE_POSIX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>

#endif

/* Formerly in the retired frontend's internal header, which went with it. Only
 * this file uses them now, so they live here rather than in a header. */
#define CHAT_STREAM_PATH_MAX              4096
#define BUILTIN_CHAT_SEND_TRANSPORT_ERROR 2

typedef struct
{
   int (*should_abort)(void *userdata);
   void (*set_stream_fd)(int fd, void *userdata);
   void *userdata;
   int aborted;
} builtin_chat_stream_control_t;

#define BUILTIN_CHAT_STREAM_IDLE_TIMEOUT_MS -1
#define BUILTIN_CHAT_SEND_OK                0
#define BUILTIN_CHAT_SEND_ERROR             1
typedef struct
{
   char *reply;
   size_t reply_len;
   size_t reply_cap;
   char provider_session_id[256];
   md_stream_t *md;
   FILE *out;
   void (*text_cb)(const char *text, void *userdata);
   void (*event_cb)(const char *event, void *userdata);
   void (*tool_cb)(const char *phase, const char *tool_name, void *userdata);
   void *text_cb_data;
   builtin_chat_stream_control_t *control;
   int wrote_text;
   int rendered_flushed;
   int final_newline;
   int saw_error;
} builtin_chat_stream_t;

static int append_text(char **buf, size_t *len, size_t *cap, const char *text)
{
   if (!buf || !len || !cap || !text)
      return -1;
   size_t n = strlen(text);
   if (n == 0)
      return 0;
   if (*len + n + 1 > *cap)
   {
      size_t next = *cap ? *cap : 1024;
      while (*len + n + 1 > next)
         next *= 2;
      char *tmp = realloc(*buf, next);
      if (!tmp)
         return -1;
      *buf = tmp;
      *cap = next;
   }
   memcpy(*buf + *len, text, n);
   *len += n;
   (*buf)[*len] = '\0';
   return 0;
}
/* on_open hook for cli_http_request_stream_ndjson: surface the live stream
 * socket fd to the TUI's abort machinery (an input thread force-closes it to
 * interrupt a turn). ud is the chat stream control, or NULL when unused. */
static void chat_stream_on_open(int fd, void *ud)
{
   builtin_chat_stream_control_t *control = (builtin_chat_stream_control_t *)ud;
   if (control && control->set_stream_fd)
      control->set_stream_fd(fd, control->userdata);
}

/* Resolve the /v1 HTTP endpoint for chat. A configured remote aimee-server is
 * used when set (the agent runs there; this client serves its working tree over
 * the reverse-channel set up by cli_main); otherwise the co-located server's
 * HTTP UDS ("unix:<home>/aimee-http.sock"). Returns 0 on success. */
static int chat_v1_endpoint(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   if (cli_v1_has_remote_endpoint())
   {
      char *ep = cli_v1_client_endpoint();
      if (ep)
      {
         int n = snprintf(out, out_len, "%s", ep);
         free(ep);
         return (n > 0 && (size_t)n < out_len) ? 0 : -1;
      }
   }
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   if (snprintf(out, out_len, "unix:%s/aimee-http.sock", home) >= (int)out_len)
      return -1;
   return 0;
}

/* Bearer for the chat /v1 endpoint: the configured token for a remote endpoint,
 * NULL for the local UDS (no auth needed). Caller frees. */
static char *chat_v1_bearer(void)
{
   return cli_v1_has_remote_endpoint() ? cli_v1_client_bearer() : NULL;
}

static int chat_stream_event_cb(cJSON *event, void *userdata)
{
   builtin_chat_stream_t *st = (builtin_chat_stream_t *)userdata;
   if (st && st->control && st->control->should_abort &&
       st->control->should_abort(st->control->userdata))
   {
      st->control->aborted = 1;
      return -1;
   }
   FILE *out = st ? st->out : NULL;
   cJSON *jev = cJSON_GetObjectItemCaseSensitive(event, "event");
   const char *ev = cJSON_IsString(jev) ? jev->valuestring : "";

   if (strcmp(ev, "text") == 0)
   {
      cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(event, "content");
      const char *content = cJSON_IsString(jcontent) ? jcontent->valuestring : "";
      if (content[0])
      {
         if (out)
         {
            if (st->md)
               md_stream_feed(st->md, content, strlen(content), out);
            else
            {
               fputs(content, out);
               fflush(out);
            }
            st->wrote_text = 1;
         }
         (void)append_text(&st->reply, &st->reply_len, &st->reply_cap, content);
         if (st->text_cb)
            st->text_cb(content, st->text_cb_data);
      }
   }
   else if (strcmp(ev, "error") == 0)
   {
      cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(event, "message");
      fprintf(stderr, "aimee chat: %s\n",
              cJSON_IsString(jmsg) ? jmsg->valuestring : "server error");
      st->saw_error = 1;
   }
   else if (strcmp(ev, "turn_end") == 0 || strcmp(ev, "done") == 0)
   {
      if (out && st->md && !st->rendered_flushed)
      {
         md_stream_flush(st->md, out);
         st->rendered_flushed = 1;
      }
      if (out && st->wrote_text && !st->final_newline &&
          (st->reply_len == 0 || st->reply[st->reply_len - 1] != '\n'))
      {
         fputc('\n', out);
         fflush(out);
         st->final_newline = 1;
      }
   }
   else if (strcmp(ev, "turn_start") == 0)
   {
      if (st->event_cb)
         st->event_cb(ev, st->text_cb_data);
   }
   else if (strncmp(ev, "tool_call.", 10) == 0)
   {
      /* tool_call.started / tool_call.completed — surface phase + tool name. */
      if (st->tool_cb)
      {
         cJSON *jname = cJSON_GetObjectItemCaseSensitive(event, "name");
         st->tool_cb(ev + 10, cJSON_IsString(jname) ? jname->valuestring : "", st->text_cb_data);
      }
   }
   else if (strcmp(ev, "session") == 0)
   {
      cJSON *jid = cJSON_GetObjectItemCaseSensitive(event, "id");
      if (cJSON_IsString(jid) && jid->valuestring[0])
         snprintf(st->provider_session_id, sizeof(st->provider_session_id), "%s", jid->valuestring);
   }

   return 0;
}
static void chat_stream_finish(builtin_chat_stream_t *st)
{
   if (!st)
      return;
   FILE *out = st->out;
   if (out && st->md && !st->rendered_flushed)
   {
      md_stream_flush(st->md, out);
      st->rendered_flushed = 1;
   }
   if (out && st->wrote_text && !st->final_newline &&
       (st->reply_len == 0 || st->reply[st->reply_len - 1] != '\n'))
   {
      fputc('\n', out);
      fflush(out);
      st->final_newline = 1;
   }
}

static int
builtin_chat_send_ex(const char *sock, const char *provider_session_id,
                     const char *aimee_session_id, const char *message, char **reply_out,
                     char *provider_session_out, size_t provider_session_out_len, FILE *out,
                     int render_markdown, void (*text_cb)(const char *text, void *userdata),
                     void (*event_cb)(const char *event, void *userdata),
                     void (*tool_cb)(const char *phase, const char *tool_name, void *userdata),
                     void *text_cb_data, builtin_chat_stream_control_t *control)
{
   if (reply_out)
      *reply_out = NULL;
   if (provider_session_out && provider_session_out_len > 0)
      provider_session_out[0] = '\0';
   if (!message || !message[0])
      return 0;
   if (control)
      control->aborted = 0;

   (void)sock; /* chat targets the /v1 HTTP endpoint (local UDS or remote) */
   if (control && control->should_abort && control->should_abort(control->userdata))
   {
      control->aborted = 1;
      return BUILTIN_CHAT_SEND_ERROR;
   }

   char endpoint[CHAT_STREAM_PATH_MAX + 32];
   if (chat_v1_endpoint(endpoint, sizeof(endpoint)) != 0)
   {
      fprintf(stderr, "aimee chat: cannot resolve server endpoint\n");
      return BUILTIN_CHAT_SEND_TRANSPORT_ERROR;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "message", message);
   char cwd[CHAT_STREAM_PATH_MAX];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   /* Ephemeral operating mode (engineer/novel) from the /novel|/engineer TUI
    * toggle; the server resolves this per turn in chat_ctx_mode. */
   {
      const char *mode_env = getenv("AIMEE_MODE");
      if (mode_env && mode_env[0])
         cJSON_AddStringToObject(req, "mode", mode_env);
   }
   /* When this turn runs under a unified-presence attachment, forward the attach
    * id so the server serializes turns across surfaces — declining a racing
    * submit with presence_busy. Absent → unarbitrated. The interactive surface
    * that used to set this transiently is gone; acp-serve does not, so today this
    * is always absent. Kept because the server contract still honours it. */
   {
      const char *attach_env = getenv("AIMEE_ATTACH_ID");
      if (attach_env && attach_env[0])
         cJSON_AddStringToObject(req, "attach_id", attach_env);
   }
   /* Client type of the host driving this turn (e.g. "acp" for an editor over the
    * ACP serve loop). The server records it on the server_sessions row via
    * chat_session_register, so a turn is logged under its real surface instead of
    * the generic "chat" default. Absent → server defaults to "chat", as before. */
   {
      const char *ct_env = getenv("AIMEE_CLIENT_TYPE");
      if (ct_env && ct_env[0])
         cJSON_AddStringToObject(req, "client_type", ct_env);
   }
   if (aimee_session_id && aimee_session_id[0])
      cJSON_AddStringToObject(req, "aimee_session_id", aimee_session_id);
   if (provider_session_id && provider_session_id[0])
   {
      cJSON_AddStringToObject(req, "provider_session_id", provider_session_id);
      /* Keep the existing webchat/server field working while chat state is
       * renamed away from Claude-specific storage. */
      cJSON_AddStringToObject(req, "claude_session_id", provider_session_id);
   }

   builtin_chat_stream_t st;
   memset(&st, 0, sizeof(st));
   st.out = out;
   st.text_cb = text_cb;
   st.event_cb = event_cb;
   st.tool_cb = tool_cb;
   st.text_cb_data = text_cb_data;
   st.control = control;
   if (out)
      st.md = md_stream_new(render_markdown);
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   int http_status = 0;
   /* on_open surfaces the live stream fd to control->set_stream_fd (and -1 on
    * close) so the TUI's abort path can interrupt the turn exactly as before. */
   char *bearer = chat_v1_bearer();
   cJSON *resp = body ? cli_http_request_stream_ndjson(endpoint, "POST", "/v1/chat/stream", body,
                                                       bearer, BUILTIN_CHAT_STREAM_IDLE_TIMEOUT_MS,
                                                       &http_status, chat_stream_event_cb, &st,
                                                       chat_stream_on_open, control)
                      : NULL;
   free(bearer);
   free(body);
   chat_stream_finish(&st);
   md_stream_free(st.md);

   if (!resp)
   {
      if (control && control->should_abort && control->should_abort(control->userdata))
         control->aborted = 1;
      free(st.reply);
      if (!(control && control->aborted))
         fprintf(stderr, "aimee chat: no final response from server\n");
      return BUILTIN_CHAT_SEND_TRANSPORT_ERROR;
   }
   if (control && control->should_abort && control->should_abort(control->userdata))
      control->aborted = 1;
   if (control && control->aborted)
   {
      cJSON_Delete(resp);
      free(st.reply);
      return BUILTIN_CHAT_SEND_ERROR;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
   if (!ok)
   {
      cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (!st.saw_error)
         fprintf(stderr, "aimee chat: %s\n",
                 cJSON_IsString(jmsg) ? jmsg->valuestring : "server error");
      cJSON_Delete(resp);
      free(st.reply);
      return BUILTIN_CHAT_SEND_ERROR;
   }

   cJSON_Delete(resp);
   if (provider_session_out && provider_session_out_len > 0 && st.provider_session_id[0])
      snprintf(provider_session_out, provider_session_out_len, "%s", st.provider_session_id);
   if (reply_out)
      *reply_out = st.reply;
   else
      free(st.reply);
   return BUILTIN_CHAT_SEND_OK;
}

/* Like cli_chat_once, but invokes `text_cb(delta, ud)` for each incremental text
 * chunk as the turn streams (used by acp-serve to emit session/update events).
 * Still returns the full reply (heap, caller frees) or NULL on error; no stdout. */
char *cli_chat_stream(const char *sock, const char *session_id, const char *message,
                      void (*text_cb)(const char *delta, void *ud),
                      void (*tool_cb)(const char *phase, const char *tool_name, void *ud), void *ud)
{
   char *reply = NULL;
   char provider_session[256] = "";
   int rc =
       builtin_chat_send_ex(sock, "", session_id, message, &reply, provider_session,
                            sizeof(provider_session), NULL, 0, text_cb, NULL, tool_cb, ud, NULL);
   if (rc != BUILTIN_CHAT_SEND_OK)
   {
      free(reply);
      return NULL;
   }
   return reply;
}

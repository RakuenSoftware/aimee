/* cli_tui.c: built-in terminal UI for aimee's default chat command */
#include "aimee_home.h"
#include "cli_client.h"
#include "cli_agent_keys.h"
#include "cli_tui.h"
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

#define CLI_TUI_PATH_MAX                    4096
#define CLI_TUI_MOUSE_SCROLL_LINES          3
#define CLI_TUI_COMPOSER_PAD_TOP            1
#define CLI_TUI_COMPOSER_PAD_BOTTOM         1
#define BUILTIN_CHAT_STREAM_IDLE_TIMEOUT_MS -1
#define BUILTIN_CHAT_SEND_OK                0
#define BUILTIN_CHAT_SEND_ERROR             1
#define BUILTIN_CHAT_SEND_TRANSPORT_ERROR   2
/* Read all of stdin into a buffer. Returns malloc'd string, caller frees. */
static char *read_stdin(void)
{
   size_t cap = 65536;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t len = 0;
   while (len < cap - 1)
   {
      int n = (int)read(fileno(stdin), buf + len, (unsigned int)(cap - 1 - len));
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   buf[len] = '\0';
   return buf;
}
typedef struct
{
   int (*should_abort)(void *userdata);
   void (*set_stream_fd)(int fd, void *userdata);
   void *userdata;
   int aborted;
} builtin_chat_stream_control_t;

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

/* Build "/v1/sessions/<pct-encoded session_id><suffix>" into out. Returns 0 on
 * success. Used by the presence attach/detach helpers (formerly NDJSON RPCs). */
static int chat_v1_session_path(char *out, size_t n, const char *session_id, const char *suffix)
{
   char enc[256];
   if (!session_id || !session_id[0] || cli_v1_pct_encode(session_id, enc, sizeof(enc)) != 0)
      return -1;
   if (snprintf(out, n, "/v1/sessions/%s%s", enc, suffix) >= (int)n)
      return -1;
   return 0;
}

/* POST a unary /v1 request to the co-located server over the local HTTP UDS and
 * return the parsed JSON response (caller cJSON_Delete()s it), or NULL on
 * transport failure or a non-2xx status. body_obj may be NULL. */
static cJSON *chat_v1_post(const char *path, cJSON *body_obj)
{
   char endpoint[CLI_TUI_PATH_MAX + 32];
   if (chat_v1_endpoint(endpoint, sizeof(endpoint)) != 0)
      return NULL;
   char *body = body_obj ? cJSON_PrintUnformatted(body_obj) : NULL;
   char *bearer = chat_v1_bearer();
   int status = 0;
   cJSON *resp =
       cli_http_request(endpoint, "POST", path, body, bearer, CLIENT_DEFAULT_TIMEOUT_MS, &status);
   free(bearer);
   free(body);
   if (resp && !(status >= 200 && status < 300))
   {
      cJSON_Delete(resp);
      return NULL;
   }
   return resp;
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

#include "cli_tui_misc.inc"
static void chat_detect_model(const char *provider, const char *model, char *out, size_t out_len);
static void chat_detect_effort(const char *provider, char *out, size_t out_len);
#ifdef AIMEE_POSIX /* defined and used only by the POSIX-only OpenCode TUI */
static void chat_format_aimee_model_label(const char *provider, const char *model,
                                          const char *effort, char *out, size_t out_len);
#endif
static int chat_config_read_scalar(const char *key, char *out, size_t out_len);

static const char *chat_canonical_model_alias(const char *model)
{
   if (!model)
      return "";
   if (strcmp(model, "chatgpt 5.5") == 0)
      return "gpt-5.5";
   return model;
}

static void chat_copy_canonical_model(char *out, size_t out_len, const char *model)
{
   if (!out || out_len == 0)
      return;
   const char *canonical = chat_canonical_model_alias(model);
   if (canonical == out)
      return;
   snprintf(out, out_len, "%s", canonical);
}

#ifdef AIMEE_POSIX /* only the OpenCode TUI (POSIX-only) uses this label helper */
static void chat_format_aimee_model_label(const char *provider, const char *model,
                                          const char *effort, char *out, size_t out_len)
{
   const char *p = provider && provider[0] ? provider : "primary";
   const char *m = model && model[0] ? model : "aimee";
   const char *e = effort && effort[0] ? effort : "default";
   if (!out || out_len == 0)
      return;
   snprintf(out, out_len, "aimee %s %s %s", p, m, e);
}
#endif

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

   char endpoint[CLI_TUI_PATH_MAX + 32];
   if (chat_v1_endpoint(endpoint, sizeof(endpoint)) != 0)
   {
      fprintf(stderr, "aimee chat: cannot resolve server endpoint\n");
      return BUILTIN_CHAT_SEND_TRANSPORT_ERROR;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "message", message);
   char cwd[CLI_TUI_PATH_MAX];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   /* Ephemeral operating mode (engineer/novel) from the /novel|/engineer TUI
    * toggle; the server resolves this per turn in chat_ctx_mode. */
   {
      const char *mode_env = getenv("AIMEE_MODE");
      if (mode_env && mode_env[0])
         cJSON_AddStringToObject(req, "mode", mode_env);
   }
   /* When this turn runs under a unified-presence attachment (set transiently by
    * builtin_chat_loop around the one-shot paths), forward the attach id so the
    * server serializes turns across surfaces — declining a racing submit with
    * presence_busy. Absent → unarbitrated, exactly as before. */
   {
      const char *attach_env = getenv("AIMEE_ATTACH_ID");
      if (attach_env && attach_env[0])
         cJSON_AddStringToObject(req, "attach_id", attach_env);
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

static int builtin_chat_send(const char *sock, const char *provider_session_id,
                             const char *aimee_session_id, const char *message, char **reply_out,
                             char *provider_session_out, size_t provider_session_out_len)
{
   return builtin_chat_send_ex(sock, provider_session_id, aimee_session_id, message, reply_out,
                               provider_session_out, provider_session_out_len, stdout,
                               isatty(STDOUT_FILENO) ? 1 : 0, NULL, NULL, NULL, NULL, NULL);
}

/* Attach `surface` to `session_id` so the following chat turn participates in
 * unified-presence turn arbitration. Best-effort: returns 1 and writes the
 * minted attach id on success, 0 on any failure (the chat then proceeds
 * unarbitrated). */
static int cli_chat_presence_attach(const char *sock, const char *session_id, const char *surface,
                                    char *out_attach, size_t out_n)
{
   (void)sock; /* presence attach is a co-located /v1 call */
   if (out_attach && out_n)
      out_attach[0] = '\0';
   if (!session_id || !session_id[0] || !out_attach || !out_n)
      return 0;
   char path[512];
   if (chat_v1_session_path(path, sizeof(path), session_id, "/attach") != 0)
      return 0;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "surface", (surface && surface[0]) ? surface : "cli");
   cJSON *resp = chat_v1_post(path, req);
   cJSON_Delete(req);
   int ok = 0;
   if (resp)
   {
      cJSON *aid = cJSON_GetObjectItemCaseSensitive(resp, "attach_id");
      if (cJSON_IsString(aid) && aid->valuestring[0])
      {
         snprintf(out_attach, out_n, "%s", aid->valuestring);
         ok = 1;
      }
      cJSON_Delete(resp);
   }
   return ok;
}

/* Detach a "cli" surface previously registered by cli_chat_presence_attach.
 * Best-effort; ignores failures. */
static void cli_chat_presence_detach(const char *sock, const char *session_id,
                                     const char *attach_id)
{
   (void)sock; /* presence detach is a co-located /v1 call */
   if (!session_id || !session_id[0] || !attach_id || !attach_id[0])
      return;
   char path[512];
   if (chat_v1_session_path(path, sizeof(path), session_id, "/detach") != 0)
      return;
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "attach_id", attach_id);
   cJSON *resp = chat_v1_post(path, req);
   cJSON_Delete(req);
   cJSON_Delete(resp);
}

/* Run `message` as a one-shot chat turn bracketed by a presence attach/detach so
 * the turn is arbitrated (a concurrent surface submitting to the same session is
 * declined with presence_busy). Falls back to a plain unarbitrated turn if the
 * attach fails. */
static int cli_chat_send_attached(const char *sock, const char *provider_session_id,
                                  const char *session_id, const char *message,
                                  char *provider_session_out, size_t provider_session_out_len)
{
   char attach_id[64] = "";
   int attached = cli_chat_presence_attach(sock, session_id, "cli", attach_id, sizeof(attach_id));
   if (attached)
      platform_setenv("AIMEE_ATTACH_ID", attach_id);
   int rc = builtin_chat_send(sock, provider_session_id, session_id, message, NULL,
                              provider_session_out, provider_session_out_len);
   if (attached)
   {
      platform_setenv("AIMEE_ATTACH_ID", ""); /* clear; the builder skips empty */
      cli_chat_presence_detach(sock, session_id, attach_id);
   }
   return rc;
}

/* One-shot chat turn for non-TUI callers (e.g. the ACP server): run `message`
 * through aimee-server and return the reply text (heap, caller frees), or NULL
 * on error. out=NULL keeps stdout clean for callers that own a protocol stream. */
char *cli_chat_once(const char *sock, const char *session_id, const char *message)
{
   return cli_chat_stream(sock, session_id, message, NULL, NULL, NULL);
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

#ifdef AIMEE_POSIX /* only the OpenCode TUI (POSIX-only) drives streaming control */
static int builtin_chat_send_streaming_control(const char *sock, const char *provider_session_id,
                                               const char *aimee_session_id, const char *message,
                                               char **reply_out, char *provider_session_out,
                                               size_t provider_session_out_len,
                                               void (*text_cb)(const char *text, void *userdata),
                                               void (*event_cb)(const char *event, void *userdata),
                                               void *text_cb_data,
                                               builtin_chat_stream_control_t *control)
{
   return builtin_chat_send_ex(sock, provider_session_id, aimee_session_id, message, reply_out,
                               provider_session_out, provider_session_out_len, NULL, 0, text_cb,
                               event_cb, NULL, text_cb_data, control);
}
#endif

#include "cli_tui_opencode_v2.inc"

static int chat_is_codex_provider(const char *provider)
{
   return provider && (strcmp(provider, "codex") == 0 || strcmp(provider, "codex-oauth") == 0 ||
                       strcmp(provider, "chatgpt") == 0);
}

static int chat_is_claude_code_provider(const char *provider)
{
   return provider && (strcmp(provider, "claude") == 0 || strcmp(provider, "claude-code") == 0);
}

static int chat_claude_code_config_read_model(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return -1;
   char path[CLI_TUI_PATH_MAX];
   snprintf(path, sizeof(path), "%s/.claude/settings.json", home);
   FILE *fp = fopen(path, "r");
   if (!fp)
      return -1;
   char buf[8192];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   if (n == 0)
      return -1;
   buf[n] = '\0';
   cJSON *root = cJSON_Parse(buf);
   if (!root)
      return -1;
   cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
   int ok = -1;
   if (cJSON_IsString(model) && model->valuestring && model->valuestring[0])
   {
      snprintf(out, out_len, "%s", model->valuestring);
      ok = 0;
   }
   cJSON_Delete(root);
   return ok;
}

static void chat_trim_value(char *s)
{
   if (!s)
      return;
   char *p = s;
   while (*p && isspace((unsigned char)*p))
      p++;
   if (p != s)
      memmove(s, p, strlen(p) + 1);
   size_t n = strlen(s);
   while (n > 0 && isspace((unsigned char)s[n - 1]))
      s[--n] = '\0';
   if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\'')))
   {
      memmove(s, s + 1, n - 2);
      s[n - 2] = '\0';
   }
}

static int chat_config_path(char *out, size_t out_len)
{
   const char *home = aimee_home();
   if (!home || !home[0] || !out || out_len == 0)
      return -1;
   snprintf(out, out_len, "%s/aimee.yaml", home);
   return 0;
}

static int chat_config_read_scalar(const char *key, char *out, size_t out_len)
{
   if (!key || !key[0] || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   char path[CLI_TUI_PATH_MAX];
   if (chat_config_path(path, sizeof(path)) != 0)
      return -1;
   FILE *fp = fopen(path, "r");
   if (!fp)
      return -1;
   char line[512];
   size_t key_len = strlen(key);
   while (fgets(line, sizeof(line), fp))
   {
      if (strncmp(line, key, key_len) != 0 || line[key_len] != ':')
         continue;
      char val[256];
      snprintf(val, sizeof(val), "%s", line + key_len + 1);
      chat_trim_value(val);
      snprintf(out, out_len, "%s", val);
      fclose(fp);
      return out[0] ? 0 : -1;
   }
   fclose(fp);
   return -1;
}

static void chat_strip_toml_comment(char *s)
{
   int quote = 0;
   for (char *p = s; p && *p; p++)
   {
      if ((*p == '"' || *p == '\'') && (p == s || p[-1] != '\\'))
      {
         if (!quote)
            quote = *p;
         else if (quote == *p)
            quote = 0;
      }
      else if (*p == '#' && !quote)
      {
         *p = '\0';
         return;
      }
   }
}

static int chat_codex_config_path(char *out, size_t out_len)
{
   const char *home = getenv("HOME");
   if (!home || !home[0] || !out || out_len == 0)
      return -1;
   int n = snprintf(out, out_len, "%s/.codex/config.toml", home);
   return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

static int chat_codex_config_find_scalar(const char *key, const char *section, char *out,
                                         size_t out_len)
{
   if (!key || !key[0] || !out || out_len == 0)
      return -1;
   out[0] = '\0';
   char path[CLI_TUI_PATH_MAX];
   if (chat_codex_config_path(path, sizeof(path)) != 0)
      return -1;
   FILE *fp = fopen(path, "r");
   if (!fp)
      return -1;
   char current_section[128] = "";
   char line[512];
   while (fgets(line, sizeof(line), fp))
   {
      chat_strip_toml_comment(line);
      chat_trim_value(line);
      if (!line[0])
         continue;
      size_t line_len = strlen(line);
      if (line_len >= 2 && line[0] == '[' && line[line_len - 1] == ']')
      {
         line[line_len - 1] = '\0';
         snprintf(current_section, sizeof(current_section), "%s", line + 1);
         chat_trim_value(current_section);
         continue;
      }
      if ((section && strcmp(current_section, section) != 0) || (!section && current_section[0]))
         continue;
      char *eq = strchr(line, '=');
      if (!eq)
         continue;
      *eq = '\0';
      chat_trim_value(line);
      if (strcmp(line, key) != 0)
         continue;
      char val[256];
      snprintf(val, sizeof(val), "%s", eq + 1);
      chat_trim_value(val);
      if (val[0])
      {
         snprintf(out, out_len, "%s", val);
         fclose(fp);
         return 0;
      }
   }
   fclose(fp);
   return -1;
}

static int chat_codex_config_read_scalar(const char *key, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   char active_profile[128] = "";
   (void)chat_codex_config_find_scalar("profile", NULL, active_profile, sizeof(active_profile));
   if (active_profile[0])
   {
      char section[160];
      int n = snprintf(section, sizeof(section), "profiles.%s", active_profile);
      if (n > 0 && (size_t)n < sizeof(section) &&
          chat_codex_config_find_scalar(key, section, out, out_len) == 0)
         return 0;
   }
   return chat_codex_config_find_scalar(key, NULL, out, out_len);
}

static void chat_detect_model(const char *provider, const char *model, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (model && model[0])
   {
      chat_copy_canonical_model(out, out_len, model);
      return;
   }

   const char *env = getenv("AIMEE_MODEL");
   if ((!env || !env[0]) && chat_is_codex_provider(provider))
      env = getenv("CODEX_MODEL");
   if (env && env[0])
   {
      chat_copy_canonical_model(out, out_len, env);
      return;
   }

   if (chat_is_codex_provider(provider))
   {
      if (chat_config_read_scalar("codex_model", out, out_len) == 0 && out[0])
      {
         chat_copy_canonical_model(out, out_len, out);
         return;
      }
      if (chat_codex_config_read_scalar("model", out, out_len) == 0 && out[0])
      {
         chat_copy_canonical_model(out, out_len, out);
         return;
      }
   }

   if (chat_is_claude_code_provider(provider))
   {
      if (chat_claude_code_config_read_model(out, out_len) == 0 && out[0])
         return;
   }
}

static void chat_detect_effort(const char *provider, char *out, size_t out_len)
{
   const char *env = getenv("AIMEE_EFFORT");
   if (!env || !env[0])
      env = getenv("CODEX_REASONING_EFFORT");
   if (!env || !env[0])
      env = getenv("OPENAI_REASONING_EFFORT");
   if (env && env[0])
   {
      snprintf(out, out_len, "%s", env);
      return;
   }

   if (chat_config_read_scalar("model_reasoning_effort", out, out_len) == 0 && out[0])
   {
      return;
   }

   if (chat_is_codex_provider(provider))
   {
      if (chat_codex_config_read_scalar("model_reasoning_effort", out, out_len) == 0 && out[0])
         return;
      if (chat_codex_config_read_scalar("reasoning_effort", out, out_len) == 0 && out[0])
         return;
   }

   snprintf(out, out_len, "default");
}

/* Portable line reader: POSIX getline() is absent on MinGW/Windows. Grows *line
 * (caller frees) and returns the length excluding the trailing NUL, or -1 at
 * EOF with nothing read. Keeps the newline like getline so the caller's trim
 * loop is unchanged. */
static ssize_t tui_read_line(char **line, size_t *cap, FILE *f)
{
   size_t len = 0;
   int c;
   while ((c = fgetc(f)) != EOF)
   {
      if (len + 2 > *cap)
      {
         size_t ncap = *cap ? *cap * 2 : 128;
         char *nb = realloc(*line, ncap);
         if (!nb)
            return -1;
         *line = nb;
         *cap = ncap;
      }
      (*line)[len++] = (char)c;
      if (c == '\n')
         break;
   }
   if (len == 0)
      return -1;
   (*line)[len] = '\0';
   return (ssize_t)len;
}

static int builtin_chat_native_loop(const char *sock, const char *provider_session_id,
                                    const char *session_id, char *provider_session_out,
                                    size_t provider_session_out_len)
{
   char *line = NULL;
   size_t cap = 0;
   fprintf(stderr, "aimee chat: native TUI. Type /quit to exit.\n");
   for (;;)
   {
      fprintf(stderr, "> ");
      fflush(stderr);
      ssize_t n = tui_read_line(&line, &cap, stdin);
      if (n < 0)
         break;
      while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
         line[--n] = '\0';
      if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0)
         break;
      if (!line[0])
         continue;
      int rc = builtin_chat_send(sock, provider_session_id, session_id, line, NULL,
                                 provider_session_out, provider_session_out_len);
      if (rc != 0)
      {
         if (rc == BUILTIN_CHAT_SEND_TRANSPORT_ERROR)
         {
            fprintf(stderr,
                    "aimee chat: session is still active; retry after the server is back.\n");
            continue;
         }
         free(line);
         return rc;
      }
      provider_session_id = provider_session_out;
   }
   free(line);
   return 0;
}

int builtin_chat_loop(const char *sock, const char *agent_name, const char *model,
                      const char *session_id, int autonomous, int debug, int default_launch,
                      int argc, char **argv)
{
   char provider_session_id[256] = "";
   char display_model[128] = "";
   char effort[64] = "";

   if (sock && sock[0])
      platform_setenv("AIMEE_SOCK", sock);

   if (argc > 0)
   {
      if (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0)
      {
         fprintf(stderr, "Usage: aimee chat [message...]\n"
                         "       aimee                 Start the default interactive TUI\n\n"
                         "Interactive chat uses the OpenCode v2 TUI.\n"
                         "Provider routing, memory, guardrails, and tool execution stay owned by "
                         "aimee-server.\n");
         return 0;
      }
      char *msg = join_message_args(argc, argv);
      if (!msg)
         return 1;
      int rc = cli_chat_send_attached(sock, provider_session_id, session_id, msg,
                                      provider_session_id, sizeof(provider_session_id));
      free(msg);
      return rc;
   }

   if (!isatty(STDIN_FILENO))
   {
      char *msg = read_stdin();
      if (!msg)
         return 1;
      int rc = msg[0] ? cli_chat_send_attached(sock, provider_session_id, session_id, msg,
                                               provider_session_id, sizeof(provider_session_id))
                      : 0;
      free(msg);
      return rc;
   }

   if (debug)
      fprintf(stderr, "aimee: built-in chat using primary agent %s\n",
              agent_name && agent_name[0] ? agent_name : "default");

   chat_detect_model(agent_name, model, display_model, sizeof(display_model));
   chat_detect_effort(agent_name, effort, sizeof(effort));

   int opencode_handled = 0;
   int opencode_rc = opencode_exec_tui(sock, agent_name, display_model[0] ? display_model : model,
                                       effort, session_id, autonomous, debug, default_launch, argc,
                                       argv, &opencode_handled);
   if (opencode_handled)
      return opencode_rc;
   if (default_launch)
      return builtin_chat_native_loop(sock, provider_session_id, session_id, provider_session_id,
                                      sizeof(provider_session_id));
   fprintf(stderr, "aimee chat: could not start OpenCode TUI\n");
   return 1;
}

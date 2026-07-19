/*
 * mcp_client.c: MCP client — transport abstraction, JSON-RPC framing,
 * session handshake / tool dispatch, and a stdio transport.
 */
#include "mcp_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "sse_parser.h"

typedef int (*mcp_http_stream_cb)(const char *data, size_t len, void *userdata);
int agent_http_get_stream(const char *url, const char *extra_headers, mcp_http_stream_cb callback,
                          void *userdata, int timeout_ms);
int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers);

#ifdef AIMEE_POSIX
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

/* --- JSON-RPC framing ------------------------------------------------------- */

char *mcp_jsonrpc_build_request(int id, const char *method, const cJSON *params)
{
   if (!method)
      return NULL;

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;

   if (!cJSON_AddStringToObject(root, "jsonrpc", "2.0") ||
       !cJSON_AddNumberToObject(root, "id", (double)id) ||
       !cJSON_AddStringToObject(root, "method", method))
   {
      cJSON_Delete(root);
      return NULL;
   }

   if (params)
   {
      cJSON *dup = cJSON_Duplicate(params, 1);
      if (!dup)
      {
         cJSON_Delete(root);
         return NULL;
      }
      cJSON_AddItemToObject(root, "params", dup);
   }

   char *body = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!body)
      return NULL;

   size_t blen = strlen(body);
   char *framed = malloc(blen + 2);
   if (!framed)
   {
      free(body);
      return NULL;
   }
   memcpy(framed, body, blen);
   framed[blen] = '\n';
   framed[blen + 1] = '\0';
   free(body);
   return framed;
}

int mcp_jsonrpc_parse_response(const char *frame, int *out_id, cJSON **out_root, cJSON **out_result,
                               char *err_buf, size_t err_buf_len)
{
   if (!frame || !out_root || !out_result)
      return -1;

   *out_root = NULL;
   *out_result = NULL;

   cJSON *root = cJSON_Parse(frame);
   if (!root)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "invalid json response");
      return -1;
   }

   cJSON *jid = cJSON_GetObjectItem(root, "id");
   if (out_id && cJSON_IsNumber(jid))
      *out_id = (int)jid->valuedouble;

   cJSON *jerr = cJSON_GetObjectItem(root, "error");
   if (jerr && cJSON_IsObject(jerr))
   {
      if (err_buf && err_buf_len > 0)
      {
         cJSON *jmsg = cJSON_GetObjectItem(jerr, "message");
         const char *msg = cJSON_IsString(jmsg) ? jmsg->valuestring : "unknown error";
         snprintf(err_buf, err_buf_len, "%s", msg);
      }
      cJSON_Delete(root);
      return -1;
   }

   cJSON *jres = cJSON_GetObjectItem(root, "result");
   if (!jres)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "missing result");
      cJSON_Delete(root);
      return -1;
   }

   *out_root = root;
   *out_result = jres;
   return 0;
}

/* --- Stdio transport (POSIX) ------------------------------------------------ */

#ifdef AIMEE_POSIX

typedef struct
{
   pid_t pid;
   int in_fd;  /* parent -> child stdin */
   int out_fd; /* child stdout -> parent */
   char *read_buf;
   size_t read_len;
   size_t read_cap;
} stdio_state_t;

static int stdio_send(mcp_transport_t *t, const char *json, size_t len)
{
   stdio_state_t *st = (stdio_state_t *)t->state;
   if (st->in_fd < 0)
      return -1;

   const char *p = json;
   size_t remaining = len;
   while (remaining > 0)
   {
      ssize_t n = write(st->in_fd, p, remaining);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         return -1;
      }
      p += n;
      remaining -= (size_t)n;
   }
   return 0;
}

/* Ensure the read buffer has room for at least |want| more bytes. */
static int stdio_reserve(stdio_state_t *st, size_t want)
{
   size_t need = st->read_len + want + 1;
   if (need <= st->read_cap)
      return 0;
   size_t new_cap = st->read_cap ? st->read_cap * 2 : 4096;
   while (new_cap < need)
      new_cap *= 2;
   char *nb = realloc(st->read_buf, new_cap);
   if (!nb)
      return -1;
   st->read_buf = nb;
   st->read_cap = new_cap;
   return 0;
}

static int stdio_recv(mcp_transport_t *t, char *buf, size_t buflen, int timeout_ms)
{
   stdio_state_t *st = (stdio_state_t *)t->state;
   if (st->out_fd < 0 || buflen == 0)
      return -1;

   for (;;)
   {
      /* First check for a complete line already buffered. */
      char *nl = st->read_len > 0 ? memchr(st->read_buf, '\n', st->read_len) : NULL;
      if (nl)
      {
         size_t line_len = (size_t)(nl - st->read_buf);
         size_t copy = line_len < buflen - 1 ? line_len : buflen - 1;
         memcpy(buf, st->read_buf, copy);
         buf[copy] = '\0';
         size_t advance = line_len + 1; /* consume the newline */
         memmove(st->read_buf, st->read_buf + advance, st->read_len - advance);
         st->read_len -= advance;
         return (int)copy;
      }

      /* Wait for more bytes. */
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(st->out_fd, &rfds);
      struct timeval tv;
      struct timeval *ptv = NULL;
      if (timeout_ms >= 0)
      {
         tv.tv_sec = timeout_ms / 1000;
         tv.tv_usec = (timeout_ms % 1000) * 1000;
         ptv = &tv;
      }
      int rv = select(st->out_fd + 1, &rfds, NULL, NULL, ptv);
      if (rv < 0)
      {
         if (errno == EINTR)
            continue;
         return -1;
      }
      if (rv == 0)
         return 0; /* timeout */

      if (stdio_reserve(st, 4096) != 0)
         return -1;

      ssize_t n = read(st->out_fd, st->read_buf + st->read_len, 4096);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         return -1;
      }
      if (n == 0)
      {
         /* EOF: surface any remaining buffered data without a newline, then fail. */
         if (st->read_len > 0)
         {
            size_t copy = st->read_len < buflen - 1 ? st->read_len : buflen - 1;
            memcpy(buf, st->read_buf, copy);
            buf[copy] = '\0';
            st->read_len = 0;
            return (int)copy;
         }
         return -1;
      }
      st->read_len += (size_t)n;
   }
}

static void stdio_close(mcp_transport_t *t)
{
   stdio_state_t *st = (stdio_state_t *)t->state;
   if (!st)
      return;
   if (st->in_fd >= 0)
      close(st->in_fd);
   if (st->out_fd >= 0)
      close(st->out_fd);
   if (st->pid > 0)
   {
      kill(st->pid, SIGTERM);
      /* Best-effort reap with a short grace window. */
      for (int i = 0; i < 50; i++)
      {
         int status = 0;
         pid_t r = waitpid(st->pid, &status, WNOHANG);
         if (r == st->pid || r < 0)
            break;
         struct timespec ts = {0, 10 * 1000 * 1000};
         nanosleep(&ts, NULL);
      }
   }
   free(st->read_buf);
   free(st);
   t->state = NULL;
}

static const mcp_transport_vtable_t stdio_vt = {
    .send = stdio_send,
    .recv = stdio_recv,
    .close = stdio_close,
};

typedef struct sse_msg_node
{
   char *frame;
   struct sse_msg_node *next;
} sse_msg_node_t;

typedef struct
{
   char stream_url[512];
   char message_url[512];
   char auth_header[256];

   pthread_t thread;
   pthread_mutex_t lock;
   pthread_cond_t cond;

   sse_parser_t parser;
   char event_name[64];
   char *event_data;
   size_t event_len;
   size_t event_cap;

   sse_msg_node_t *head;
   sse_msg_node_t *tail;

   int running;
   int ready;
   int consecutive_failures;
} sse_state_t;

static int sse_event_append(sse_state_t *st, const char *data, size_t len)
{
   size_t need = st->event_len + len + (st->event_len > 0 ? 1 : 0) + 1;
   if (need > st->event_cap)
   {
      size_t new_cap = st->event_cap ? st->event_cap * 2 : 512;
      while (new_cap < need)
         new_cap *= 2;
      char *tmp = realloc(st->event_data, new_cap);
      if (!tmp)
         return -1;
      st->event_data = tmp;
      st->event_cap = new_cap;
   }
   if (st->event_len > 0)
      st->event_data[st->event_len++] = '\n';
   memcpy(st->event_data + st->event_len, data, len);
   st->event_len += len;
   st->event_data[st->event_len] = '\0';
   return 0;
}

static void sse_event_reset(sse_state_t *st)
{
   st->event_name[0] = '\0';
   st->event_len = 0;
   if (st->event_data)
      st->event_data[0] = '\0';
}

static void sse_queue_frame_locked(sse_state_t *st, const char *frame)
{
   sse_msg_node_t *node = calloc(1, sizeof(*node));
   if (!node)
      return;
   node->frame = strdup(frame);
   if (!node->frame)
   {
      free(node);
      return;
   }
   if (st->tail)
      st->tail->next = node;
   else
      st->head = node;
   st->tail = node;
   pthread_cond_broadcast(&st->cond);
}

static int sse_parser_line_cb(const char *line, size_t len, void *userdata)
{
   sse_state_t *st = (sse_state_t *)userdata;
   if (!st)
      return -1;

   if (len == 0)
   {
      pthread_mutex_lock(&st->lock);
      if (strcmp(st->event_name, "endpoint") == 0 && st->event_data && st->event_data[0])
      {
         snprintf(st->message_url, sizeof(st->message_url), "%s", st->event_data);
         st->ready = 1;
         st->consecutive_failures = 0;
         pthread_cond_broadcast(&st->cond);
      }
      else if (strcmp(st->event_name, "message") == 0 && st->event_data && st->event_data[0])
      {
         sse_queue_frame_locked(st, st->event_data);
      }
      sse_event_reset(st);
      int running = st->running;
      pthread_mutex_unlock(&st->lock);
      return running ? 0 : 1;
   }

   if (line[0] == ':')
      return 0;

   if (strncmp(line, "event:", 6) == 0)
   {
      const char *value = line + 6;
      while (*value == ' ')
         value++;
      pthread_mutex_lock(&st->lock);
      snprintf(st->event_name, sizeof(st->event_name), "%s", value);
      pthread_mutex_unlock(&st->lock);
   }
   else if (strncmp(line, "data:", 5) == 0)
   {
      const char *value = line + 5;
      while (*value == ' ')
         value++;
      pthread_mutex_lock(&st->lock);
      int rc = sse_event_append(st, value, strlen(value));
      pthread_mutex_unlock(&st->lock);
      if (rc != 0)
         return -1;
   }

   return 0;
}

static int sse_stream_cb(const char *data, size_t len, void *userdata)
{
   sse_state_t *st = (sse_state_t *)userdata;
   return sse_parser_feed(&st->parser, data, len, sse_parser_line_cb, userdata);
}

static void *sse_reader_main(void *userdata)
{
   sse_state_t *st = (sse_state_t *)userdata;

   while (1)
   {
      pthread_mutex_lock(&st->lock);
      int running = st->running;
      pthread_mutex_unlock(&st->lock);
      if (!running)
         break;

      char headers[512];
      headers[0] = '\0';
      snprintf(headers, sizeof(headers), "Accept: text/event-stream\r\nCache-Control: no-cache%s%s",
               st->auth_header[0] ? "\r\n" : "", st->auth_header);

      sse_parser_reset(&st->parser);
      int status = agent_http_get_stream(st->stream_url, headers, sse_stream_cb, st, 5000);

      pthread_mutex_lock(&st->lock);
      if (!st->running)
      {
         pthread_mutex_unlock(&st->lock);
         break;
      }
      if (status < 0 || status >= 500)
         st->consecutive_failures++;
      else
         st->consecutive_failures = 0;
      pthread_cond_broadcast(&st->cond);
      pthread_mutex_unlock(&st->lock);

      struct timespec ts = {0, 200 * 1000 * 1000};
      nanosleep(&ts, NULL);
   }

   return NULL;
}

static int sse_send(mcp_transport_t *t, const char *json, size_t len)
{
   sse_state_t *st = (sse_state_t *)t->state;
   if (!st || !json || len == 0)
      return -1;

   char message_url[512];
   pthread_mutex_lock(&st->lock);
   if (!st->ready)
   {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += 5;
      while (st->running && !st->ready)
      {
         if (pthread_cond_timedwait(&st->cond, &st->lock, &deadline) == ETIMEDOUT)
            break;
      }
   }
   snprintf(message_url, sizeof(message_url), "%s", st->message_url);
   pthread_mutex_unlock(&st->lock);

   if (!message_url[0])
      return -1;

   char *body = malloc(len + 1);
   if (!body)
      return -1;
   memcpy(body, json, len);
   body[len] = '\0';

   char *resp = NULL;
   int status = agent_http_post(message_url, st->auth_header[0] ? st->auth_header : NULL, body,
                                &resp, 5000, NULL);
   free(resp);
   free(body);
   return (status >= 200 && status < 300) ? 0 : -1;
}

static int sse_recv(mcp_transport_t *t, char *buf, size_t buflen, int timeout_ms)
{
   sse_state_t *st = (sse_state_t *)t->state;
   if (!st || !buf || buflen == 0)
      return -1;

   pthread_mutex_lock(&st->lock);
   if (!st->head)
   {
      if (timeout_ms < 0)
      {
         while (st->running && !st->head && st->consecutive_failures < 3)
            pthread_cond_wait(&st->cond, &st->lock);
      }
      else if (timeout_ms > 0)
      {
         struct timespec deadline;
         clock_gettime(CLOCK_REALTIME, &deadline);
         deadline.tv_sec += timeout_ms / 1000;
         deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
         if (deadline.tv_nsec >= 1000000000L)
         {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
         }
         while (st->running && !st->head && st->consecutive_failures < 3)
         {
            if (pthread_cond_timedwait(&st->cond, &st->lock, &deadline) == ETIMEDOUT)
               break;
         }
      }
   }

   if (st->head)
   {
      sse_msg_node_t *node = st->head;
      st->head = node->next;
      if (!st->head)
         st->tail = NULL;
      size_t copy = strlen(node->frame);
      if (copy >= buflen)
         copy = buflen - 1;
      memcpy(buf, node->frame, copy);
      buf[copy] = '\0';
      free(node->frame);
      free(node);
      pthread_mutex_unlock(&st->lock);
      return (int)copy;
   }

   int failures = st->consecutive_failures;
   int running = st->running;
   pthread_mutex_unlock(&st->lock);
   if (!running || failures >= 3)
      return -1;
   return 0;
}

static void sse_close(mcp_transport_t *t)
{
   sse_state_t *st = (sse_state_t *)t->state;
   if (!st)
      return;

   pthread_mutex_lock(&st->lock);
   st->running = 0;
   pthread_cond_broadcast(&st->cond);
   pthread_mutex_unlock(&st->lock);

   pthread_join(st->thread, NULL);

   sse_msg_node_t *node = st->head;
   while (node)
   {
      sse_msg_node_t *next = node->next;
      free(node->frame);
      free(node);
      node = next;
   }

   sse_parser_free(&st->parser);
   free(st->event_data);
   pthread_cond_destroy(&st->cond);
   pthread_mutex_destroy(&st->lock);
   free(st);
   t->state = NULL;
}

static const mcp_transport_vtable_t sse_vt = {
    .send = sse_send,
    .recv = sse_recv,
    .close = sse_close,
};

mcp_transport_t *mcp_transport_stdio_open(const char *const argv[], const char *cwd)
{
   if (!argv || !argv[0])
      return NULL;

   int in_pipe[2] = {-1, -1};  /* parent writes in_pipe[1], child reads in_pipe[0] */
   int out_pipe[2] = {-1, -1}; /* child writes out_pipe[1], parent reads out_pipe[0] */

   if (pipe(in_pipe) != 0)
      return NULL;
   if (pipe(out_pipe) != 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      return NULL;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      return NULL;
   }

   if (pid == 0)
   {
      /* Child. */
      if (cwd && chdir(cwd) != 0)
         _exit(127);
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      execvp(argv[0], (char *const *)argv);
      _exit(127);
   }

   /* Parent. */
   close(in_pipe[0]);
   close(out_pipe[1]);

   mcp_transport_t *t = calloc(1, sizeof(*t));
   stdio_state_t *st = calloc(1, sizeof(*st));
   if (!t || !st)
   {
      free(t);
      free(st);
      close(in_pipe[1]);
      close(out_pipe[0]);
      kill(pid, SIGTERM);
      waitpid(pid, NULL, 0);
      return NULL;
   }

   st->pid = pid;
   st->in_fd = in_pipe[1];
   st->out_fd = out_pipe[0];
   t->kind = MCP_TRANSPORT_STDIO;
   t->vt = &stdio_vt;
   t->state = st;
   return t;
}

mcp_transport_t *mcp_transport_sse_open(const char *url, const char *bearer_token)
{
   if (!url || !url[0])
      return NULL;

   mcp_transport_t *t = calloc(1, sizeof(*t));
   sse_state_t *st = calloc(1, sizeof(*st));
   if (!t || !st)
   {
      free(t);
      free(st);
      return NULL;
   }

   snprintf(st->stream_url, sizeof(st->stream_url), "%s", url);
   if (bearer_token && bearer_token[0])
      snprintf(st->auth_header, sizeof(st->auth_header), "Authorization: Bearer %s", bearer_token);
   pthread_mutex_init(&st->lock, NULL);
   pthread_cond_init(&st->cond, NULL);
   sse_parser_init(&st->parser);
   st->running = 1;

   t->kind = MCP_TRANSPORT_SSE;
   t->vt = &sse_vt;
   t->state = st;

   if (pthread_create(&st->thread, NULL, sse_reader_main, st) != 0)
   {
      sse_parser_free(&st->parser);
      pthread_cond_destroy(&st->cond);
      pthread_mutex_destroy(&st->lock);
      free(st);
      free(t);
      return NULL;
   }
   return t;
}

#else /* !AIMEE_POSIX */

mcp_transport_t *mcp_transport_stdio_open(const char *const argv[], const char *cwd)
{
   (void)argv;
   (void)cwd;
   return NULL;
}

mcp_transport_t *mcp_transport_sse_open(const char *url, const char *bearer_token)
{
   (void)url;
   (void)bearer_token;
   return NULL;
}

#endif /* AIMEE_POSIX */

void mcp_transport_close(mcp_transport_t *t)
{
   if (!t)
      return;
   if (t->vt && t->vt->close)
      t->vt->close(t);
   free(t);
}

/* --- Session ---------------------------------------------------------------- */

int mcp_client_session_init(mcp_client_session_t *s, const char *name, mcp_transport_t *transport)
{
   if (!s || !transport)
      return -1;
   memset(s, 0, sizeof(*s));
   if (name)
   {
      s->name = strdup(name);
      if (!s->name)
         return -1;
   }
   s->transport = transport;
   s->next_id = 1;
   return 0;
}

void mcp_client_session_close(mcp_client_session_t *s)
{
   if (!s)
      return;
   if (s->transport)
   {
      mcp_transport_close(s->transport);
      s->transport = NULL;
   }
   if (s->tool_schemas)
   {
      cJSON_Delete(s->tool_schemas);
      s->tool_schemas = NULL;
   }
   free(s->name);
   s->name = NULL;
}

/* Perform a single request/response roundtrip. On success, *out_root and
 * *out_result are populated (caller cJSON_Delete(*out_root)). */
static int session_roundtrip(mcp_client_session_t *s, const char *method, const cJSON *params,
                             int timeout_ms, cJSON **out_root, cJSON **out_result, char *err_buf,
                             size_t err_buf_len)
{
   if (!s || !s->transport || !method)
      return -1;

   int id = s->next_id++;
   char *req = mcp_jsonrpc_build_request(id, method, params);
   if (!req)
      return -1;
   size_t rlen = strlen(req);

   if (s->transport->vt->send(s->transport, req, rlen) != 0)
   {
      free(req);
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "transport send failed");
      return -1;
   }
   free(req);

   /* Ignore mismatched-id frames (e.g. async notifications). Cap iterations so
    * a chatty server can't spin us forever. */
   for (int i = 0; i < 32; i++)
   {
      char buf[65536];
      int n = s->transport->vt->recv(s->transport, buf, sizeof(buf), timeout_ms);
      if (n <= 0)
      {
         if (err_buf && err_buf_len > 0)
            snprintf(err_buf, err_buf_len, n == 0 ? "transport timeout" : "transport recv failed");
         return -1;
      }

      int got_id = 0;
      cJSON *root = NULL;
      cJSON *result = NULL;
      if (mcp_jsonrpc_parse_response(buf, &got_id, &root, &result, err_buf, err_buf_len) != 0)
      {
         if (root)
            cJSON_Delete(root);
         /* A parseable error response is terminal. An unparseable frame might
          * be a notification — keep reading. */
         if (strncmp(buf, "{", 1) == 0 && strstr(buf, "\"error\""))
            return -1;
         continue;
      }
      if (got_id != id)
      {
         cJSON_Delete(root);
         continue;
      }
      *out_root = root;
      *out_result = result;
      return 0;
   }
   if (err_buf && err_buf_len > 0)
      snprintf(err_buf, err_buf_len, "response id mismatch");
   return -1;
}

int mcp_client_initialize(mcp_client_session_t *s, int timeout_ms)
{
   if (!s)
      return -1;
   if (s->initialized)
      return 0;

   cJSON *params = cJSON_CreateObject();
   if (!params)
      return -1;
   cJSON_AddStringToObject(params, "protocolVersion", "2024-11-05");
   cJSON *caps = cJSON_CreateObject();
   cJSON_AddItemToObject(params, "capabilities", caps);
   cJSON *client_info = cJSON_CreateObject();
   cJSON_AddStringToObject(client_info, "name", "aimee");
   cJSON_AddStringToObject(client_info, "version", "0.1");
   cJSON_AddItemToObject(params, "clientInfo", client_info);

   cJSON *root = NULL;
   cJSON *result = NULL;
   int rc = session_roundtrip(s, "initialize", params, timeout_ms, &root, &result, NULL, 0);
   cJSON_Delete(params);
   if (rc != 0)
   {
      if (root)
         cJSON_Delete(root);
      return -1;
   }
   cJSON_Delete(root);
   s->initialized = 1;
   return 0;
}

int mcp_client_list_tools(mcp_client_session_t *s, int timeout_ms)
{
   if (!s)
      return -1;

   cJSON *root = NULL;
   cJSON *result = NULL;
   if (session_roundtrip(s, "tools/list", NULL, timeout_ms, &root, &result, NULL, 0) != 0)
   {
      if (root)
         cJSON_Delete(root);
      return -1;
   }

   /* Detach the result subtree so we can release the rest of the envelope. */
   cJSON *detached = cJSON_DetachItemFromObject(root, "result");
   cJSON_Delete(root);
   if (!detached)
      return -1;

   if (s->tool_schemas)
      cJSON_Delete(s->tool_schemas);
   s->tool_schemas = detached;
   return 0;
}

int mcp_client_call_tool(mcp_client_session_t *s, const char *tool, const cJSON *args,
                         int timeout_ms, cJSON **out_result, char *err_buf, size_t err_buf_len)
{
   if (!s || !tool || !out_result)
      return -1;
   *out_result = NULL;

   cJSON *params = cJSON_CreateObject();
   if (!params)
      return -1;
   cJSON_AddStringToObject(params, "name", tool);
   if (args)
   {
      cJSON *dup = cJSON_Duplicate(args, 1);
      if (dup)
         cJSON_AddItemToObject(params, "arguments", dup);
   }

   cJSON *root = NULL;
   cJSON *result = NULL;
   int rc =
       session_roundtrip(s, "tools/call", params, timeout_ms, &root, &result, err_buf, err_buf_len);
   cJSON_Delete(params);
   if (rc != 0)
   {
      if (root)
         cJSON_Delete(root);
      return -1;
   }
   cJSON *detached = cJSON_DetachItemFromObject(root, "result");
   cJSON_Delete(root);
   if (!detached)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "missing result");
      return -1;
   }
   *out_result = detached;
   return 0;
}

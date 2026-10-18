/* Shared bounded model listener. The caller supplies its authenticated transport;
 * this owner never reads global remote configuration, Vault or provider keys. */
#include "cli_proxy.h"
#include "platform_process.h"
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/socket.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef AIMEE_WINDOWS
#include <process.h>
typedef HANDLE proxy_thread_t;
typedef CRITICAL_SECTION proxy_mutex_t;
#define mutex_init(m)    InitializeCriticalSection(m)
#define mutex_lock(m)    EnterCriticalSection(m)
#define mutex_unlock(m)  LeaveCriticalSection(m)
#define mutex_destroy(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t proxy_thread_t;
typedef pthread_mutex_t proxy_mutex_t;
#define mutex_init(m)    pthread_mutex_init(m, NULL)
#define mutex_lock(m)    pthread_mutex_lock(m)
#define mutex_unlock(m)  pthread_mutex_unlock(m)
#define mutex_destroy(m) pthread_mutex_destroy(m)
#endif

#define PROXY_WORKERS    8
#define PROXY_HEADER_MAX 65536
#define PROXY_BODY_MAX   (4u * 1024u * 1024u)

typedef struct
{
   struct cli_proxy *proxy;
   proxy_thread_t thread;
   int downstream, upstream, sent;
} proxy_worker_t;

struct cli_proxy
{
   int listener, started;
   int monitor_started;
   proxy_thread_t monitor;
   unsigned port;
   cli_proxy_forward_fn forward;
   void *forward_context;
   char token[256], session_id[128];
   atomic_int stopping;
   proxy_mutex_t mutex;
   proxy_worker_t workers[PROXY_WORKERS];
};

static void proxy_error(int fd, int status, const char *message)
{
   char body[512], response[1024];
   int len = snprintf(body, sizeof(body),
                      "{\"error\":{\"message\":\"%s\",\"type\":\"aimee_proxy_error\"}}", message);
   int n = snprintf(response, sizeof(response),
                    "HTTP/1.1 %d Error\r\nContent-Type: application/json\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                    status, len, body);
   (void)aimee_core_socket_write_all(fd, response, (size_t)n);
}

static int model_route(const char *method, const char *path)
{
   size_t n = strcspn(path, "?");
   if (strcmp(method, "GET") == 0)
      return n == strlen("/v1/models") && strncmp(path, "/v1/models", n) == 0;
   if (strcmp(method, "POST") != 0)
      return 0;
   static const char *const routes[] = {
       "/v1/responses",  "/v1/responses/compact", "/v1/chat/completions",      "/v1/completions",
       "/v1/embeddings", "/v1/messages",          "/v1/messages/count_tokens", NULL};
   for (int i = 0; routes[i]; i++)
      if (n == strlen(routes[i]) && strncmp(path, routes[i], n) == 0)
         return 1;
   return 0;
}

static int protocol_header(const char *name)
{
   static const char *const allowed[] = {
       "content-type",   "content-encoding", "accept", "anthropic-version",
       "anthropic-beta", "openai-beta",      NULL};
   for (int i = 0; allowed[i]; i++)
      if (strcasecmp(name, allowed[i]) == 0)
         return 1;
   return 0;
}

static int session_valid(const char *value)
{
   if (!value || strlen(value) >= 128)
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; p++)
      if (!isalnum(*p) && *p != '-' && *p != '_')
         return 0;
   return 1;
}

static void proxy_upstream(int fd, void *context)
{
   proxy_worker_t *worker = context;
   mutex_lock(&worker->proxy->mutex);
   worker->upstream = fd;
   if (fd >= 0 && atomic_load(&worker->proxy->stopping))
      aimee_core_socket_shutdown(fd);
   mutex_unlock(&worker->proxy->mutex);
}

/* A client can cancel while the upstream is quiet. Detect its EOF separately
 * from response writes so the remote connection is closed promptly. */
#ifdef AIMEE_WINDOWS
static unsigned __stdcall proxy_monitor(void *context)
#else
static void *proxy_monitor(void *context)
#endif
{
   cli_proxy_t *proxy = context;
   while (!atomic_load(&proxy->stopping))
   {
      mutex_lock(&proxy->mutex);
      for (int i = 0; i < proxy->started; i++)
      {
         proxy_worker_t *worker = &proxy->workers[i];
         if (worker->downstream >= 0 && worker->upstream >= 0 &&
             aimee_core_socket_peer_closed(worker->downstream))
            aimee_core_socket_shutdown(worker->upstream);
      }
      mutex_unlock(&proxy->mutex);
#ifdef AIMEE_WINDOWS
      Sleep(100);
#else
      usleep(100000);
#endif
   }
   return 0;
}

static int proxy_response(const void *data, unsigned long length, void *context)
{
   proxy_worker_t *worker = context;
   worker->sent = 1;
   return aimee_core_socket_write_all(worker->downstream, data, length);
}

static void proxy_response_started(void *context)
{
   proxy_worker_t *worker = context;
   worker->sent = 1;
}

static int proxy_cancelled(void *context)
{
   proxy_worker_t *worker = context;
   return atomic_load(&worker->proxy->stopping) ||
          aimee_core_socket_peer_closed(worker->downstream);
}

static void proxy_request(proxy_worker_t *worker)
{
   cli_proxy_t *proxy = worker->proxy;
   int fd = worker->downstream;
   char *buffer = malloc(PROXY_HEADER_MAX + 1);
   if (!buffer)
      return;
   size_t received = 0, header_len = 0;
   while (received < PROXY_HEADER_MAX)
   {
      long n = aimee_core_socket_read(fd, buffer + received, PROXY_HEADER_MAX - received);
      if (n <= 0)
         goto done;
      received += (size_t)n;
      buffer[received] = '\0';
      char *end = strstr(buffer, "\r\n\r\n");
      if (end)
      {
         header_len = (size_t)(end - buffer) + 4;
         break;
      }
      if (memchr(buffer, '\0', received))
         goto malformed;
   }
   if (!header_len)
   {
      proxy_error(fd, 431, "request headers too large");
      goto done;
   }
   char *line_end = strstr(buffer, "\r\n");
   if (!line_end)
      goto malformed;
   *line_end = '\0';
   char method[16], path[2048], version[16], extra;
   if (sscanf(buffer, "%15s %2047s %15s %c", method, path, version, &extra) != 3 ||
       strcmp(version, "HTTP/1.1") != 0)
      goto malformed;
   if (!model_route(method, path))
   {
      proxy_error(fd, 404, "only model API routes are available through the local proxy");
      goto done;
   }
   char forwarded[8192] = "", session[128] = "";
   size_t forwarded_len = 0, content_length = 0;
   int have_length = 0, have_auth = 0, have_host = 0, have_session = 0;
   for (char *line = line_end + 2; line < buffer + header_len - 2; line = line_end + 2)
   {
      line_end = strstr(line, "\r\n");
      if (!line_end || line_end >= buffer + header_len || *line == ' ' || *line == '\t')
         goto malformed;
      *line_end = '\0';
      char *colon = strchr(line, ':');
      if (!colon || colon == line)
         goto malformed;
      *colon = '\0';
      for (char *p = line; *p; p++)
         if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p))
            goto malformed;
      char *value = colon + 1;
      while (*value == ' ' || *value == '\t')
         value++;
      char *tail = line_end;
      while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t'))
         *--tail = '\0';
      for (char *p = value; *p; p++)
         if (((unsigned char)*p < 32 && *p != '\t') || (unsigned char)*p == 127)
            goto malformed;
      if (strcasecmp(line, "Authorization") == 0 || strcasecmp(line, "x-api-key") == 0)
      {
         const char *token =
             strcasecmp(line, "Authorization") == 0 ? aimee_core_bearer_token(value) : value;
         if (have_auth++ || !token || !aimee_core_credential_equal(token, proxy->token))
         {
            proxy_error(fd, 401, "a valid local proxy token is required");
            goto done;
         }
      }
      else if (strcasecmp(line, "Host") == 0)
      {
         char expected[64];
         snprintf(expected, sizeof(expected), "127.0.0.1:%u", proxy->port);
         if (have_host++ || strcmp(value, expected) != 0)
            goto malformed;
      }
      else if (strcasecmp(line, "Content-Length") == 0)
      {
         if (have_length++ || !*value)
            goto malformed;
         for (char *p = value; *p; p++)
         {
            if (*p < '0' || *p > '9')
               goto malformed;
            content_length = content_length * 10 + (size_t)(*p - '0');
            if (content_length > PROXY_BODY_MAX)
            {
               proxy_error(fd, 413, "request body exceeds 4 MiB");
               goto done;
            }
         }
      }
      else if (strcasecmp(line, "Transfer-Encoding") == 0 || strcasecmp(line, "Expect") == 0 ||
               strcasecmp(line, "Upgrade") == 0 || strcasecmp(line, "Origin") == 0)
         goto malformed;
      else if (strcasecmp(line, "session_id") == 0)
      {
         if (have_session++ || !session_valid(value))
            goto malformed;
         snprintf(session, sizeof(session), "%s", value);
      }
      else if (protocol_header(line))
      {
         int n = snprintf(forwarded + forwarded_len, sizeof(forwarded) - forwarded_len,
                          "%s: %s\r\n", line, value);
         if (n < 0 || (size_t)n >= sizeof(forwarded) - forwarded_len)
            goto malformed;
         forwarded_len += (size_t)n;
      }
   }
   if (!have_auth)
   {
      proxy_error(fd, 401, "a valid local proxy token is required");
      goto done;
   }
   if (!have_host || (strcmp(method, "POST") == 0 && !have_length) ||
       received - header_len > content_length)
      goto malformed;
   char *body = malloc(content_length ? content_length : 1);
   if (!body)
      goto done;
   size_t used = received - header_len;
   memcpy(body, buffer + header_len, used);
   while (used < content_length)
   {
      long n = aimee_core_socket_read(fd, body + used, content_length - used);
      if (n <= 0)
      {
         free(body);
         goto done;
      }
      used += (size_t)n;
   }
   cli_proxy_io_t io = {.downstream_fd = fd,
                        .context = worker,
                        .write_response = proxy_response,
                        .on_upstream = proxy_upstream,
                        .response_started = proxy_response_started,
                        .cancelled = proxy_cancelled};
   int rc = proxy->forward(method, path, forwarded, body, content_length,
                           proxy->session_id[0] ? proxy->session_id : session, &io,
                           proxy->forward_context);
   free(body);
   if ((rc != 0 || !worker->sent) && !worker->sent)
      proxy_error(fd, 502,
                  "Aimee upstream connection failed; check remote identity and reachability");
   goto done;
malformed:
   proxy_error(fd, 400, "invalid or unsupported HTTP request framing");
done:
   free(buffer);
}

#ifdef AIMEE_WINDOWS
static unsigned __stdcall proxy_worker(void *context)
#else
static void *proxy_worker(void *context)
#endif
{
   proxy_worker_t *worker = context;
   cli_proxy_t *proxy = worker->proxy;
   while (!atomic_load(&proxy->stopping))
   {
      if (aimee_core_socket_wait_readable(proxy->listener, 200) != 1)
         continue;
      int fd = aimee_core_socket_accept(proxy->listener);
      if (fd < 0)
         continue;
      mutex_lock(&proxy->mutex);
      worker->downstream = fd;
      if (atomic_load(&proxy->stopping))
         aimee_core_socket_shutdown(fd);
      mutex_unlock(&proxy->mutex);
      worker->sent = 0;
      aimee_core_socket_set_timeouts(fd, 30000, 30000);
      proxy_request(worker);
      mutex_lock(&proxy->mutex);
      worker->downstream = -1;
      aimee_core_socket_close(fd);
      mutex_unlock(&proxy->mutex);
   }
   return 0;
}

cli_proxy_t *cli_proxy_start_bound(unsigned port, const char *local_token, const char *session_id,
                                   unsigned *bound_port, cli_proxy_forward_fn forward,
                                   void *forward_context)
{
   char auth[272];
   if (!bound_port || !local_token || strlen(local_token) >= 256 ||
       aimee_core_bearer_value(auth, sizeof(auth), local_token) != 0 ||
       (session_id && !session_valid(session_id)) || !forward)
      return NULL;
   cli_proxy_t *proxy = calloc(1, sizeof(*proxy));
   if (!proxy)
      return NULL;
   proxy->forward = forward;
   proxy->forward_context = forward_context;
   proxy->listener = aimee_core_socket_listen_loopback(port, &proxy->port);
   if (proxy->listener < 0)
   {
      free(proxy);
      return NULL;
   }
   snprintf(proxy->token, sizeof(proxy->token), "%s", local_token);
   snprintf(proxy->session_id, sizeof(proxy->session_id), "%s", session_id ? session_id : "");
   atomic_init(&proxy->stopping, 0);
   mutex_init(&proxy->mutex);
   for (int i = 0; i < PROXY_WORKERS; i++)
   {
      proxy_worker_t *worker = &proxy->workers[i];
      worker->proxy = proxy;
      worker->downstream = worker->upstream = -1;
#ifdef AIMEE_WINDOWS
      worker->thread = (HANDLE)_beginthreadex(NULL, 0, proxy_worker, worker, 0, NULL);
      if (!worker->thread)
#else
      if (pthread_create(&worker->thread, NULL, proxy_worker, worker) != 0)
#endif
      {
         cli_proxy_stop(proxy);
         return NULL;
      }
      proxy->started++;
   }
#ifdef AIMEE_WINDOWS
   proxy->monitor = (HANDLE)_beginthreadex(NULL, 0, proxy_monitor, proxy, 0, NULL);
   if (!proxy->monitor)
#else
   if (pthread_create(&proxy->monitor, NULL, proxy_monitor, proxy) != 0)
#endif
   {
      cli_proxy_stop(proxy);
      return NULL;
   }
   proxy->monitor_started = 1;
   *bound_port = proxy->port;
   return proxy;
}

void cli_proxy_stop(cli_proxy_t *proxy)
{
   if (!proxy)
      return;
   atomic_store(&proxy->stopping, 1);
   if (proxy->monitor_started)
   {
#ifdef AIMEE_WINDOWS
      WaitForSingleObject(proxy->monitor, INFINITE);
      CloseHandle(proxy->monitor);
#else
      pthread_join(proxy->monitor, NULL);
#endif
   }
   mutex_lock(&proxy->mutex);
   for (int i = 0; i < proxy->started; i++)
   {
      aimee_core_socket_shutdown(proxy->workers[i].downstream);
      aimee_core_socket_shutdown(proxy->workers[i].upstream);
   }
   mutex_unlock(&proxy->mutex);
   for (int i = 0; i < proxy->started; i++)
   {
#ifdef AIMEE_WINDOWS
      WaitForSingleObject(proxy->workers[i].thread, INFINITE);
      CloseHandle(proxy->workers[i].thread);
#else
      pthread_join(proxy->workers[i].thread, NULL);
#endif
   }
   aimee_core_socket_close(proxy->listener);
   mutex_destroy(&proxy->mutex);
   memset(proxy->token, 0, sizeof(proxy->token));
   free(proxy);
}

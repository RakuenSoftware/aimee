/* kb_metrics_listener.c: isolated, authenticated Prometheus endpoint for aimee-kb. */

#include "kb_metrics_listener.h"

#include "kb_http_telemetry.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define KB_METRICS_REQUEST_MAX 8192
#define KB_METRICS_RESPONSE_MAX (1024 * 1024)
#define KB_METRICS_BACKLOG 32
#define KB_METRICS_MAX_WORKERS 8
#define KB_METRICS_TOKEN_MAX 512

typedef struct
{
   int fd;
   SSL *ssl;
} metrics_connection_t;

static pthread_t g_metrics_thread;
static int g_metrics_fd = -1;
static atomic_int g_metrics_running;
static int g_metrics_trusted_transport = 0;
static int g_metrics_require_bearer = 0;
static SSL_CTX *g_metrics_tls_ctx = NULL;
static pthread_mutex_t g_metrics_worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_metrics_worker_idle = PTHREAD_COND_INITIALIZER;
static unsigned int g_metrics_workers = 0;
static char g_metrics_token_hash[65];
static char g_metrics_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static dev_t g_metrics_socket_dev;
static ino_t g_metrics_socket_ino;

static const char *status_text(int status)
{
   switch (status)
   {
   case 200:
      return "OK";
   case 400:
      return "Bad Request";
   case 401:
      return "Unauthorized";
   case 404:
      return "Not Found";
   case 405:
      return "Method Not Allowed";
   case 503:
      return "Service Unavailable";
   default:
      return "Internal Server Error";
   }
}

static ssize_t connection_read(metrics_connection_t *connection, void *data, size_t len)
{
   if (!connection->ssl)
      return read(connection->fd, data, len);
   int got = SSL_read(connection->ssl, data, len > INT_MAX ? INT_MAX : (int)len);
   if (got > 0)
      return got;
   int ssl_error = SSL_get_error(connection->ssl, got);
   if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
      errno = EINTR;
   return -1;
}

static ssize_t connection_write(metrics_connection_t *connection, const void *data, size_t len)
{
   if (!connection->ssl)
      return write(connection->fd, data, len);
   int wrote = SSL_write(connection->ssl, data, len > INT_MAX ? INT_MAX : (int)len);
   if (wrote > 0)
      return wrote;
   int ssl_error = SSL_get_error(connection->ssl, wrote);
   if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
      errno = EINTR;
   return -1;
}

static void write_all(metrics_connection_t *connection, const char *data, size_t len)
{
   while (len > 0)
   {
      ssize_t wrote = connection_write(connection, data, len);
      if (wrote < 0 && errno == EINTR)
         continue;
      if (wrote <= 0)
         return;
      data += wrote;
      len -= (size_t)wrote;
   }
}

static void send_response(metrics_connection_t *connection, int status, const char *body)
{
   const char *content_type = status == 200 ? "text/plain; version=0.0.4; charset=utf-8"
                                             : "application/json";
   size_t body_len = body ? strlen(body) : 0;
   char header[640];
   int header_len = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %zu\r\n"
                             "Cache-Control: no-store\r\n"
                             "X-Content-Type-Options: nosniff\r\n"
                             "Connection: close\r\n\r\n",
                             status, status_text(status), content_type, body_len);
   if (header_len > 0 && (size_t)header_len < sizeof(header))
      write_all(connection, header, (size_t)header_len);
   if (body_len)
      write_all(connection, body, body_len);
}

/* Returns 0 for a valid header set, -1 for duplicate/malformed auth or a body. */
static int inspect_headers(char *request, char *bearer, size_t bearer_cap)
{
   bearer[0] = '\0';
   unsigned int authorization_count = 0;
   unsigned int host_count = 0;
   char *line = strstr(request, "\r\n");
   while (line)
   {
      line += 2;
      if (line[0] == '\r' && line[1] == '\n')
         break;
      char *end = strstr(line, "\r\n");
      if (!end)
         return -1;
      if (strncasecmp(line, "Authorization:", 14) == 0)
      {
         authorization_count++;
         if (authorization_count > 1)
            return -1;
         char *value = line + 14;
         while (value < end && (*value == ' ' || *value == '\t'))
            value++;
         if ((size_t)(end - value) <= 7 || strncasecmp(value, "Bearer ", 7) != 0)
            return -1;
         value += 7;
         size_t len = (size_t)(end - value);
         if (!len || len >= bearer_cap)
            return -1;
         memcpy(bearer, value, len);
         bearer[len] = '\0';
      }
      else if (strncasecmp(line, "Host:", 5) == 0)
         host_count++;
      else if (strncasecmp(line, "Content-Length:", 15) == 0 ||
               strncasecmp(line, "Transfer-Encoding:", 18) == 0)
         return -1;
      line = end;
   }
   return host_count == 1 ? 0 : -1;
}

static void serve_metrics_connection(metrics_connection_t *connection)
{
   struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
   (void)setsockopt(connection->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   (void)setsockopt(connection->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

   char request[KB_METRICS_REQUEST_MAX];
   size_t received = 0;
   while (received < sizeof(request) - 1)
   {
      ssize_t chunk =
          connection_read(connection, request + received, sizeof(request) - 1 - received);
      if (chunk < 0 && errno == EINTR)
         continue;
      if (chunk <= 0)
         return;
      received += (size_t)chunk;
      request[received] = '\0';
      if (strstr(request, "\r\n\r\n"))
         break;
   }
   if (!strstr(request, "\r\n\r\n"))
   {
      send_response(connection, 400, "{\"error\":\"request headers too large\"}");
      return;
   }

   char *first_end = strstr(request, "\r\n");
   if (!first_end)
   {
      send_response(connection, 400, "{\"error\":\"invalid request\"}");
      return;
   }
   *first_end = '\0';
   char method[16] = "", path[256] = "", version[16] = "", extra[2] = "";
   int fields = sscanf(request, "%15s %255s %15s %1s", method, path, version, extra);
   *first_end = '\r';
   if (fields != 3 || strcmp(version, "HTTP/1.1") != 0)
   {
      send_response(connection, 400, "{\"error\":\"invalid request line\"}");
      return;
   }
   if (strcmp(path, "/metrics") != 0)
   {
      send_response(connection, 404, "{\"error\":\"not found\"}");
      return;
   }
   if (strcmp(method, "GET") != 0)
   {
      send_response(connection, 405, "{\"error\":\"method not allowed\"}");
      return;
   }

   char bearer[KB_METRICS_TOKEN_MAX + 1];
   if (inspect_headers(request, bearer, sizeof(bearer)) != 0)
   {
      send_response(connection, 400, "{\"error\":\"invalid request headers\"}");
      return;
   }
   char *body = malloc(KB_METRICS_RESPONSE_MAX);
   if (!body)
   {
      send_response(connection, 500, "{\"error\":\"out of memory\"}");
      return;
   }
   int status = kb_http_telemetry_scrape(bearer, g_metrics_trusted_transport,
                                         g_metrics_require_bearer, body,
                                         KB_METRICS_RESPONSE_MAX);
   OPENSSL_cleanse(bearer, sizeof(bearer));
   send_response(connection, status, body);
   free(body);
}

static void worker_finished(void)
{
   pthread_mutex_lock(&g_metrics_worker_lock);
   if (g_metrics_workers > 0)
      g_metrics_workers--;
   pthread_cond_broadcast(&g_metrics_worker_idle);
   pthread_mutex_unlock(&g_metrics_worker_lock);
}

static void *metrics_connection_worker(void *arg)
{
   metrics_connection_t *connection = arg;
   if (g_metrics_tls_ctx)
   {
      connection->ssl = SSL_new(g_metrics_tls_ctx);
      if (!connection->ssl || SSL_set_fd(connection->ssl, connection->fd) != 1 ||
          SSL_accept(connection->ssl) != 1)
      {
         if (connection->ssl)
            SSL_free(connection->ssl);
         close(connection->fd);
         free(connection);
         worker_finished();
         return NULL;
      }
   }
   serve_metrics_connection(connection);
   if (connection->ssl)
   {
      (void)SSL_shutdown(connection->ssl);
      SSL_free(connection->ssl);
   }
   close(connection->fd);
   free(connection);
   worker_finished();
   return NULL;
}

static void *metrics_listener_thread(void *unused)
{
   (void)unused;
   while (atomic_load_explicit(&g_metrics_running, memory_order_acquire))
   {
      int fd = accept(g_metrics_fd, NULL, NULL);
      if (fd < 0)
      {
         if (errno == EINTR)
            continue;
         if (atomic_load_explicit(&g_metrics_running, memory_order_relaxed))
            LOG_WARN("kb_metrics", "accept failed: %s", strerror(errno));
         break;
      }
      (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

      pthread_mutex_lock(&g_metrics_worker_lock);
      if (g_metrics_workers >= KB_METRICS_MAX_WORKERS)
      {
         pthread_mutex_unlock(&g_metrics_worker_lock);
         close(fd);
         continue;
      }
      g_metrics_workers++;
      pthread_mutex_unlock(&g_metrics_worker_lock);

      metrics_connection_t *connection = calloc(1, sizeof(*connection));
      if (!connection)
      {
         close(fd);
         worker_finished();
         continue;
      }
      connection->fd = fd;
      pthread_t worker;
      if (pthread_create(&worker, NULL, metrics_connection_worker, connection) != 0)
      {
         close(fd);
         free(connection);
         worker_finished();
         continue;
      }
      (void)pthread_detach(worker);
   }
   return NULL;
}

static int ensure_socket_parent(const char *path)
{
   char parent[sizeof(g_metrics_socket_path)];
   snprintf(parent, sizeof(parent), "%s", path);
   char *last = strrchr(parent, '/');
   if (!last || last == parent)
      return 0;
   *last = '\0';
   for (char *cursor = parent + 1;; cursor++)
   {
      if (*cursor != '/' && *cursor != '\0')
         continue;
      char saved = *cursor;
      *cursor = '\0';
      if (mkdir(parent, 0700) != 0 && errno != EEXIST)
         return -1;
      *cursor = saved;
      if (saved == '\0')
         return 0;
   }
}

static int listen_unix(const char *path)
{
   if (!path || path[0] != '/' || strlen(path) >= sizeof(g_metrics_socket_path))
      return -1;
   if (ensure_socket_parent(path) != 0)
      return -1;
   struct stat existing;
   if (lstat(path, &existing) == 0 || errno != ENOENT)
      return -1;

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_un address;
   memset(&address, 0, sizeof(address));
   address.sun_family = AF_UNIX;
   snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
   if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || chmod(path, 0600) != 0 ||
       listen(fd, KB_METRICS_BACKLOG) != 0)
   {
      close(fd);
      unlink(path);
      return -1;
   }
   struct stat created;
   if (lstat(path, &created) != 0)
   {
      close(fd);
      unlink(path);
      return -1;
   }
   snprintf(g_metrics_socket_path, sizeof(g_metrics_socket_path), "%s", path);
   g_metrics_socket_dev = created.st_dev;
   g_metrics_socket_ino = created.st_ino;
   return fd;
}

static int sockaddr_is_loopback(const struct sockaddr *address)
{
   if (address->sa_family == AF_INET)
   {
      const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
      return (ntohl(v4->sin_addr.s_addr) >> 24) == 127;
   }
   if (address->sa_family == AF_INET6)
   {
      const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
      if (IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr))
         return 1;
      return IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr) && v6->sin6_addr.s6_addr[12] == 127;
   }
   return 0;
}

static int listen_tcp(const char *authority, int *loopback_out)
{
   char host[256] = "", port[16] = "";
   const char *port_start = NULL;
   if (authority[0] == '[')
   {
      const char *close = strchr(authority, ']');
      if (!close || close[1] != ':')
         return -1;
      size_t host_len = (size_t)(close - authority - 1);
      if (host_len >= sizeof(host))
         return -1;
      memcpy(host, authority + 1, host_len);
      host[host_len] = '\0';
      port_start = close + 2;
   }
   else
   {
      const char *colon = strrchr(authority, ':');
      if (!colon)
         return -1;
      size_t host_len = (size_t)(colon - authority);
      if (host_len >= sizeof(host))
         return -1;
      memcpy(host, authority, host_len);
      host[host_len] = '\0';
      port_start = colon + 1;
   }
   char *end = NULL;
   long parsed_port = strtol(port_start, &end, 10);
   if (!port_start[0] || !end || *end || parsed_port < 1 || parsed_port > 65535)
      return -1;
   snprintf(port, sizeof(port), "%ld", parsed_port);

   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_PASSIVE;
   struct addrinfo *addresses = NULL;
   if (getaddrinfo(host[0] ? host : NULL, port, &hints, &addresses) != 0)
      return -1;
   int fd = -1;
   for (struct addrinfo *candidate = addresses; candidate; candidate = candidate->ai_next)
   {
      fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
      if (fd < 0)
         continue;
      int reuse = 1;
      (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      if (bind(fd, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
          listen(fd, KB_METRICS_BACKLOG) == 0)
      {
         *loopback_out = sockaddr_is_loopback(candidate->ai_addr);
         break;
      }
      close(fd);
      fd = -1;
   }
   freeaddrinfo(addresses);
   return fd;
}

static int regular_file(const char *path)
{
   struct stat file;
   return path && path[0] && stat(path, &file) == 0 && S_ISREG(file.st_mode);
}

static int private_regular_file(const char *path)
{
   struct stat file;
   return regular_file(path) && stat(path, &file) == 0 && (file.st_mode & 077) == 0;
}

static int valid_token_hash(const char *hash)
{
   if (!hash || strnlen(hash, 65) != 64)
      return 0;
   for (size_t i = 0; i < 64; i++)
      if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f')))
         return 0;
   return 1;
}

static int load_token_file(const char *path, char hash_out[65])
{
   if (!private_regular_file(path))
      return -1;
   int fd = open(path, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return -1;
   unsigned char token[KB_METRICS_TOKEN_MAX + 3];
   ssize_t got = read(fd, token, sizeof(token));
   int saved_errno = errno;
   char extra;
   ssize_t overflow = got >= 0 && (size_t)got == sizeof(token) ? read(fd, &extra, 1) : 0;
   close(fd);
   errno = saved_errno;
   if (got < 0 || overflow > 0)
   {
      OPENSSL_cleanse(token, sizeof(token));
      return -1;
   }
   size_t len = (size_t)got;
   if (len && token[len - 1] == '\n')
      len--;
   if (len && token[len - 1] == '\r')
      len--;
   if (len < 32 || len > KB_METRICS_TOKEN_MAX)
   {
      OPENSSL_cleanse(token, sizeof(token));
      return -1;
   }
   for (size_t i = 0; i < len; i++)
      if (token[i] < 0x21 || token[i] > 0x7e)
      {
         OPENSSL_cleanse(token, sizeof(token));
         return -1;
      }
   unsigned char digest[EVP_MAX_MD_SIZE];
   unsigned int digest_len = 0;
   int ok = EVP_Digest(token, len, digest, &digest_len, EVP_sha256(), NULL) == 1 &&
            digest_len == 32;
   OPENSSL_cleanse(token, sizeof(token));
   if (!ok)
      return -1;
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; i++)
   {
      hash_out[i * 2] = hex[digest[i] >> 4];
      hash_out[i * 2 + 1] = hex[digest[i] & 15];
   }
   hash_out[64] = '\0';
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

static SSL_CTX *create_tls_context(const kb_metrics_listener_config_t *config)
{
   if (!regular_file(config->tls_certificate_file) || !private_regular_file(config->tls_key_file) ||
       ((config->tls_client_ca_file && config->tls_client_ca_file[0]) &&
        !regular_file(config->tls_client_ca_file)))
      return NULL;
   SSL_CTX *context = SSL_CTX_new(TLS_server_method());
   if (!context)
      return NULL;
   if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
       SSL_CTX_set_cipher_list(context, "HIGH:!aNULL:!MD5:!RC4") != 1 ||
       SSL_CTX_use_certificate_chain_file(context, config->tls_certificate_file) != 1 ||
       SSL_CTX_use_PrivateKey_file(context, config->tls_key_file, SSL_FILETYPE_PEM) != 1 ||
       SSL_CTX_check_private_key(context) != 1)
   {
      SSL_CTX_free(context);
      return NULL;
   }
   SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
   SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
#ifdef SSL_OP_NO_RENEGOTIATION
   SSL_CTX_set_options(context, SSL_OP_NO_RENEGOTIATION);
#endif
#ifdef SSL_CTX_set_max_early_data
   (void)SSL_CTX_set_max_early_data(context, 0);
#endif
   if (config->tls_client_ca_file && config->tls_client_ca_file[0])
   {
      if (SSL_CTX_load_verify_locations(context, config->tls_client_ca_file, NULL) != 1)
      {
         SSL_CTX_free(context);
         return NULL;
      }
      STACK_OF(X509_NAME) *names = SSL_load_client_CA_file(config->tls_client_ca_file);
      if (!names)
      {
         SSL_CTX_free(context);
         return NULL;
      }
      SSL_CTX_set_client_CA_list(context, names);
      SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
      SSL_CTX_set_verify_depth(context, 5);
   }
   return context;
}

static int has_value(const char *value)
{
   return value && value[0];
}

int kb_metrics_listener_start(const kb_metrics_listener_config_t *config)
{
   if (!config)
      return -1;
   int has_security = has_value(config->tls_certificate_file) || has_value(config->tls_key_file) ||
                      has_value(config->tls_client_ca_file) ||
                      has_value(config->bearer_token_file);
   if (!has_value(config->endpoint))
      return has_security ? -1 : 0;
   if (atomic_load_explicit(&g_metrics_running, memory_order_acquire))
      return -1;

   int certificate = has_value(config->tls_certificate_file);
   int key = has_value(config->tls_key_file);
   int client_ca = has_value(config->tls_client_ca_file);
   if (certificate != key || (client_ca && !certificate))
      return -1;

   g_metrics_token_hash[0] = '\0';
   if (has_value(config->bearer_token_hash))
   {
      if (!valid_token_hash(config->bearer_token_hash))
         return -1;
      memcpy(g_metrics_token_hash, config->bearer_token_hash, sizeof(g_metrics_token_hash));
   }
   if (has_value(config->bearer_token_file))
   {
      char file_hash[65] = "";
      if (load_token_file(config->bearer_token_file, file_hash) != 0 ||
          (g_metrics_token_hash[0] &&
           CRYPTO_memcmp(file_hash, g_metrics_token_hash, sizeof(file_hash)) != 0))
      {
         OPENSSL_cleanse(file_hash, sizeof(file_hash));
         OPENSSL_cleanse(g_metrics_token_hash, sizeof(g_metrics_token_hash));
         return -1;
      }
      memcpy(g_metrics_token_hash, file_hash, sizeof(g_metrics_token_hash));
      OPENSSL_cleanse(file_hash, sizeof(file_hash));
   }
   g_metrics_require_bearer = g_metrics_token_hash[0] != '\0';

   int is_unix = strncmp(config->endpoint, "unix://", 7) == 0;
   int is_tcp = strncmp(config->endpoint, "tcp://", 6) == 0;
   if ((!is_unix && !is_tcp) || (is_unix && (certificate || client_ca)))
      goto fail;

   int loopback = 0;
   if (is_unix)
   {
      g_metrics_trusted_transport = 1;
      g_metrics_fd = listen_unix(config->endpoint + 7);
   }
   else
   {
      if (certificate)
      {
         g_metrics_tls_ctx = create_tls_context(config);
         if (!g_metrics_tls_ctx)
            goto fail;
      }
      g_metrics_fd = listen_tcp(config->endpoint + 6, &loopback);
      g_metrics_trusted_transport = loopback || client_ca;
      if (g_metrics_fd >= 0 && !loopback && (!certificate || (!client_ca && !g_metrics_require_bearer)))
         goto fail;
   }
   if (g_metrics_fd < 0)
      goto fail;
   (void)fcntl(g_metrics_fd, F_SETFD, FD_CLOEXEC);

   kb_http_set_telemetry_token(g_metrics_token_hash);
   atomic_store_explicit(&g_metrics_running, 1, memory_order_release);
   if (pthread_create(&g_metrics_thread, NULL, metrics_listener_thread, NULL) != 0)
   {
      atomic_store_explicit(&g_metrics_running, 0, memory_order_release);
      goto fail;
   }
   LOG_INFO("kb_metrics", "Prometheus metrics listening on %s (%s)", config->endpoint,
            g_metrics_tls_ctx ? (client_ca ? "TLS + mTLS" : "TLS") : "local transport");
   return 0;

fail:
   if (g_metrics_fd >= 0)
   {
      close(g_metrics_fd);
      g_metrics_fd = -1;
   }
   kb_metrics_listener_stop();
   return -1;
}

void kb_metrics_listener_stop(void)
{
   if (atomic_load_explicit(&g_metrics_running, memory_order_acquire))
   {
      atomic_store_explicit(&g_metrics_running, 0, memory_order_release);
      if (g_metrics_fd >= 0)
      {
         shutdown(g_metrics_fd, SHUT_RDWR);
         close(g_metrics_fd);
         g_metrics_fd = -1;
      }
      pthread_join(g_metrics_thread, NULL);
   }
   pthread_mutex_lock(&g_metrics_worker_lock);
   while (g_metrics_workers > 0)
      pthread_cond_wait(&g_metrics_worker_idle, &g_metrics_worker_lock);
   pthread_mutex_unlock(&g_metrics_worker_lock);

   if (g_metrics_tls_ctx)
   {
      SSL_CTX_free(g_metrics_tls_ctx);
      g_metrics_tls_ctx = NULL;
   }
   if (g_metrics_socket_path[0])
   {
      struct stat current;
      if (lstat(g_metrics_socket_path, &current) == 0 && current.st_dev == g_metrics_socket_dev &&
          current.st_ino == g_metrics_socket_ino)
         unlink(g_metrics_socket_path);
      g_metrics_socket_path[0] = '\0';
      g_metrics_socket_dev = 0;
      g_metrics_socket_ino = 0;
   }
   OPENSSL_cleanse(g_metrics_token_hash, sizeof(g_metrics_token_hash));
   g_metrics_require_bearer = 0;
   g_metrics_trusted_transport = 0;
}

#include "kb_metrics_listener.h"

#include "log.h"

#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static const char k_token[] = "0123456789abcdef0123456789abcdef";
static char g_configured_hash[65];

static const char *test_tmpdir(void)
{
   const char *path = getenv("TMPDIR");
   return path && path[0] ? path : "/tmp";
}

void aimee_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   (void)module;
   (void)format;
}

void kb_http_set_telemetry_token(const char *hash)
{
   snprintf(g_configured_hash, sizeof(g_configured_hash), "%s", hash ? hash : "");
}

void kb_http_set_telemetry_enabled(int enabled)
{
   (void)enabled;
}

int kb_http_telemetry_scrape(const char *presented, int trusted_transport, int require_bearer,
                             char *out, int cap)
{
   if (!trusted_transport)
      return snprintf(out, (size_t)cap, "{\"error\":\"untrusted\"}"), 503;
   if (require_bearer && strcmp(presented ? presented : "", k_token) != 0)
      return snprintf(out, (size_t)cap, "{\"error\":\"unauthorized\"}"), 401;
   snprintf(out, (size_t)cap, "aimee_test_total 1\n");
   return 200;
}

static unsigned short reserve_loopback_port(void)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in address;
   memset(&address, 0, sizeof(address));
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = 0;
   assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
   socklen_t len = sizeof(address);
   assert(getsockname(fd, (struct sockaddr *)&address, &len) == 0);
   unsigned short port = ntohs(address.sin_port);
   close(fd);
   return port;
}

static int tcp_request(unsigned short port, const char *authorization)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in address;
   memset(&address, 0, sizeof(address));
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = htons(port);
   assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
   char request[1024];
   int request_len =
       snprintf(request, sizeof(request), "GET /metrics HTTP/1.1\r\nHost: localhost\r\n%s%s%s\r\n",
                authorization ? "Authorization: Bearer " : "", authorization ? authorization : "",
                authorization ? "\r\n" : "");
   assert(request_len > 0 && (size_t)request_len < sizeof(request));
   assert(write(fd, request, (size_t)request_len) == request_len);
   char response[1024] = "";
   ssize_t got = read(fd, response, sizeof(response) - 1);
   assert(got > 0);
   close(fd);
   int status = 0;
   assert(sscanf(response, "HTTP/1.1 %d", &status) == 1);
   return status;
}

static int unix_request(const char *path)
{
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_un address;
   memset(&address, 0, sizeof(address));
   address.sun_family = AF_UNIX;
   snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
   assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
   static const char request[] = "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n";
   assert(write(fd, request, sizeof(request) - 1) == (ssize_t)(sizeof(request) - 1));
   char response[1024] = "";
   assert(read(fd, response, sizeof(response) - 1) > 0);
   close(fd);
   int status = 0;
   assert(sscanf(response, "HTTP/1.1 %d", &status) == 1);
   return status;
}

static void test_disabled_and_fail_closed(void)
{
   kb_metrics_listener_config_t disabled = {0};
   assert(kb_metrics_listener_start(&disabled) == 0);

   kb_metrics_listener_config_t stray = {.bearer_token_file = "/not/read"};
   assert(kb_metrics_listener_start(&stray) == -1);

   char endpoint[64];
   snprintf(endpoint, sizeof(endpoint), "tcp://0.0.0.0:%u", reserve_loopback_port());
   kb_metrics_listener_config_t public_plaintext = {.endpoint = endpoint};
   assert(kb_metrics_listener_start(&public_plaintext) == -1);
}

static void test_loopback_bearer(void)
{
   char token_path[PATH_MAX];
   assert(snprintf(token_path, sizeof(token_path), "%s/aimee-metrics-token-XXXXXX", test_tmpdir()) <
          (int)sizeof(token_path));
   int token_fd = mkstemp(token_path);
   assert(token_fd >= 0);
   assert(fchmod(token_fd, 0600) == 0);
   assert(write(token_fd, k_token, sizeof(k_token) - 1) == (ssize_t)(sizeof(k_token) - 1));
   close(token_fd);

   unsigned short port = reserve_loopback_port();
   char endpoint[64];
   snprintf(endpoint, sizeof(endpoint), "tcp://127.0.0.1:%u", port);
   kb_metrics_listener_config_t config = {
       .endpoint = endpoint,
       .bearer_token_file = token_path,
   };
   assert(kb_metrics_listener_start(&config) == 0);
   assert(strlen(g_configured_hash) == 64);
   assert(tcp_request(port, NULL) == 401);
   assert(tcp_request(port, "wrong-wrong-wrong-wrong-wrong-wrong") == 401);
   assert(tcp_request(port, k_token) == 200);
   kb_metrics_listener_stop();
   unlink(token_path);
}

static void test_unix_lifecycle(void)
{
   char directory[PATH_MAX];
   assert(snprintf(directory, sizeof(directory), "%s/aimee-metrics-dir-XXXXXX", test_tmpdir()) <
          (int)sizeof(directory));
   assert(mkdtemp(directory) != NULL);
   char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
   snprintf(path, sizeof(path), "%s/metrics.sock", directory);
   char endpoint[sizeof(path) + 8];
   snprintf(endpoint, sizeof(endpoint), "unix://%s", path);
   kb_metrics_listener_config_t config = {.endpoint = endpoint};
   assert(kb_metrics_listener_start(&config) == 0);
   struct stat socket_info;
   assert(lstat(path, &socket_info) == 0);
   assert((socket_info.st_mode & 0777) == 0600);
   assert(unix_request(path) == 200);
   kb_metrics_listener_stop();
   assert(lstat(path, &socket_info) == -1);
   rmdir(directory);
}

int main(void)
{
   test_disabled_and_fail_closed();
   test_loopback_bearer();
   test_unix_lifecycle();
   puts("test_kb_metrics_listener: OK");
   return 0;
}

/* cli_client.c: Windows named-pipe client for aimee-server RPC, plus the Winsock
 * /v1 HTTP client the thin client's shared code (cli_main/cli_tui/cli_mcp_serve)
 * now calls. Windows has no Unix-domain socket, so the local aimee-http.sock
 * ("unix:" / absolute-path) transport is unsupported here — those callers fail
 * gracefully; the cross-platform remote path goes through aimee_client_request. */
#include "cli_client.h"
/* winsock2 must precede windows.h to avoid the legacy winsock.h being pulled in. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "aimee_client.h"
#include "aimee_home.h"
#include "aimee_version.h"
#include "cli_server_compat.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "cJSON.h"
#define RPC_PROTOCOL_VERSION 1
#include <direct.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define getcwd                  _getcwd
#define SERVER_READY_TIMEOUT_MS 30000

static const char *cli_config_dir(void)
{
   static char dir[MAX_PATH];
   if (dir[0])
      return dir;

   const char *cfg = aimee_home();
   if (cfg && cfg[0])
      snprintf(dir, sizeof(dir), "%s", cfg);
   else
   {
      const char *home = platform_home_dir();
      if (!home)
         home = ".";
      snprintf(dir, sizeof(dir), "%s\\AppData\\Roaming\\aimee", home);
   }
   return dir;
}

const char *cli_default_socket_path(void)
{
   static char path[MAX_PATH];
   if (path[0])
      return path;

   snprintf(path, sizeof(path), "%s\\aimee.sock", cli_config_dir());
   return path;
}

static int last_connect_errno = 0;
static char last_connect_path[MAX_PATH] = "";

static int cli_win32_connect_errno(void)
{
   DWORD err = GetLastError();
   if (err == ERROR_ACCESS_DENIED)
      return EACCES;
   if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
      return ENOENT;
   if (err == ERROR_PIPE_BUSY)
      return EBUSY;
   if (err == WAIT_TIMEOUT || err == ERROR_SEM_TIMEOUT)
      return ETIMEDOUT;
   return err ? EIO : 0;
}

static void cli_record_connect_errno(const char *socket_path, int err)
{
   last_connect_errno = err;
   snprintf(last_connect_path, sizeof(last_connect_path), "%s", socket_path ? socket_path : "");
}

static void cli_clear_connect_errno(void)
{
   last_connect_errno = 0;
   last_connect_path[0] = '\0';
}

int cli_last_connect_errno(void)
{
   return last_connect_errno;
}

const char *cli_last_connect_path(void)
{
   return last_connect_path;
}

int cli_connect_errno_is_permission_denied(int err)
{
   return err == EACCES || err == EPERM;
}

int cli_connect_timeout(cli_conn_t *conn, const char *socket_path, int timeout_ms)
{
   if (!socket_path)
      socket_path = cli_default_socket_path();

   cli_clear_connect_errno();
   memset(conn, 0, sizeof(*conn));
   conn->fd = -1;

   int fd = platform_ipc_connect(socket_path, timeout_ms);
   if (fd < 0)
   {
      cli_record_connect_errno(socket_path, cli_win32_connect_errno());
      return -1;
   }

   conn->fd = fd;
   conn->read_len = 0;
   return 0;
}

int cli_connect(cli_conn_t *conn, const char *socket_path)
{
   return cli_connect_timeout(conn, socket_path, CLIENT_CONNECT_TIMEOUT_MS);
}

static int win_write_all(int fd, const char *buf, size_t len)
{
   HANDLE h = (HANDLE)(intptr_t)fd;
   size_t total = 0;
   while (total < len)
   {
      DWORD chunk = (DWORD)((len - total) > 65536 ? 65536 : (len - total));
      DWORD written = 0;
      if (!WriteFile(h, buf + total, chunk, &written, NULL) || written == 0)
         return -1;
      total += (size_t)written;
   }
   return 0;
}

static int win_read_some(int fd, char *buf, size_t cap, int timeout_ms)
{
   HANDLE h = (HANDLE)(intptr_t)fd;
   DWORD waited = 0;
   DWORD sleep_ms = 10;
   DWORD timeout = timeout_ms <= 0 ? INFINITE : (DWORD)timeout_ms;

   for (;;)
   {
      DWORD avail = 0;
      if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
         return -1;
      if (avail > 0)
      {
         DWORD want = (DWORD)(cap < (size_t)avail ? cap : (size_t)avail);
         DWORD got = 0;
         if (!ReadFile(h, buf, want, &got, NULL) || got == 0)
            return -1;
         return (int)got;
      }
      if (timeout != INFINITE && waited >= timeout)
         return -1;
      Sleep(sleep_ms);
      if (timeout != INFINITE)
         waited += sleep_ms;
      if (sleep_ms < 50)
         sleep_ms *= 2;
   }
}

static cJSON *read_json_line(cli_conn_t *conn, int timeout_ms, cli_stream_cb cb, void *userdata,
                             int streaming)
{
   conn->read_len = 0;
   size_t cap = CLIENT_READ_BUF_SIZE;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t len = 0;
   for (;;)
   {
      for (size_t i = 0; i < len; i++)
      {
         if (buf[i] == '\n')
         {
            buf[i] = '\0';
            cJSON *msg = cJSON_Parse(buf);

            size_t remain = len - i - 1;
            if (remain > 0)
               memmove(buf, buf + i + 1, remain);
            len = remain;

            if (!streaming)
            {
               free(buf);
               return msg;
            }
            if (!msg)
               continue;
            if (cJSON_GetObjectItem(msg, "status"))
            {
               free(buf);
               return msg;
            }
            if (cb)
            {
               int rc = cb(msg, userdata);
               cJSON_Delete(msg);
               if (rc != 0)
               {
                  free(buf);
                  return NULL;
               }
            }
            else
               cJSON_Delete(msg);
            i = (size_t)-1;
         }
      }

      if (len >= cap - 1)
      {
         if (cap >= CLIENT_MAX_RESPONSE_SIZE)
         {
            free(buf);
            return NULL;
         }
         size_t next = cap * 2;
         if (next > CLIENT_MAX_RESPONSE_SIZE)
            next = CLIENT_MAX_RESPONSE_SIZE;
         char *grown = realloc(buf, next);
         if (!grown)
         {
            free(buf);
            return NULL;
         }
         buf = grown;
         cap = next;
      }

      int n = win_read_some(conn->fd, buf + len, cap - 1 - len, timeout_ms);
      if (n <= 0)
      {
         free(buf);
         return NULL;
      }
      len += (size_t)n;
   }
}

cJSON *cli_request(cli_conn_t *conn, cJSON *request, int timeout_ms)
{
   if (!conn || conn->fd < 0 || !request)
      return NULL;

   char *json_str = cJSON_PrintUnformatted(request);
   if (!json_str)
      return NULL;

   size_t json_len = strlen(json_str);
   int rc = win_write_all(conn->fd, json_str, json_len);
   free(json_str);
   if (rc != 0 || win_write_all(conn->fd, "\n", 1) != 0)
      return NULL;

   return read_json_line(conn, timeout_ms, NULL, NULL, 0);
}

int cli_server_available(const char *socket_path)
{
   (void)socket_path; /* Windows reaches the server over the /v1 HTTP transport */
   int status = 0;
   char *resp = aimee_client_request("GET", "/v1/health", NULL, &status);
   int available = resp && status >= 200 && status < 300;
   free(resp);
   return available;
}

void cli_close(cli_conn_t *conn)
{
   if (conn && conn->fd >= 0)
   {
      platform_ipc_close(conn->fd);
      conn->fd = -1;
   }
}

static int get_server_pid(int fd)
{
   ULONG pid = 0;
   HANDLE h = (HANDLE)(intptr_t)fd;
   if (GetNamedPipeServerProcessId(h, &pid))
      return (int)pid;
   return 0;
}

static void wait_for_stale_server_shutdown(int pid, int timeout_ms)
{
   if (pid <= 0)
   {
      Sleep((DWORD)timeout_ms);
      return;
   }

   HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
   if (!h)
   {
      Sleep(300);
      return;
   }
   WaitForSingleObject(h, (DWORD)timeout_ms);
   CloseHandle(h);
}

static int try_server(const char *socket_path, int timeout_ms, const char *required_method)
{
   cli_conn_t conn;
   if (cli_connect_timeout(&conn, socket_path, timeout_ms) != 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "server.info");
   cJSON *resp = cli_request(&conn, req, timeout_ms);
   cJSON_Delete(req);

   int ok = 0;
   int killed_stale = 0;
   int killed_pid = 0;
   if (resp)
   {
      ok = cli_server_info_is_compatible(resp, required_method, AIMEE_VERSION);
      if (!ok)
      {
         cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
         char reason[256] = "";
         if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 &&
             cli_server_info_restart_reason(resp, required_method, AIMEE_VERSION, reason,
                                            sizeof(reason)))
         {
            int server_pid = get_server_pid(conn.fd);
            fprintf(stderr, "aimee: %s; restarting\n", reason);
            if (server_pid > 0 && platform_signal_send_term(server_pid) == 0)
            {
               killed_stale = 1;
               killed_pid = server_pid;
            }
         }
      }
      cJSON_Delete(resp);
   }
   cli_close(&conn);

   if (killed_stale)
      wait_for_stale_server_shutdown(killed_pid, 6000);
   return ok;
}

static int wait_for_ready(const char *sock_path, int timeout_ms, const char *required_method)
{
   int elapsed = 0;
   int backoff = 10;

   while (elapsed < timeout_ms)
   {
      if (try_server(sock_path, 500, required_method))
         return 1;
      Sleep((DWORD)backoff);
      elapsed += backoff;
      if (backoff < 200)
         backoff *= 2;
   }
   return 0;
}

static void server_path_next_to_client(char *buf, size_t buflen)
{
   if (platform_get_exe_path(buf, buflen) != 0)
   {
      snprintf(buf, buflen, "aimee-server.exe");
      return;
   }
   char *slash = strrchr(buf, '\\');
   char *slash2 = strrchr(buf, '/');
   if (!slash || (slash2 && slash2 > slash))
      slash = slash2;
   if (!slash)
   {
      snprintf(buf, buflen, "aimee-server.exe");
      return;
   }
   snprintf(slash + 1, buflen - (size_t)(slash - buf) - 1, "aimee-server.exe");
}

static const char *spawn_server(const char *required_method)
{
   static char sock_path[MAX_PATH];
   snprintf(sock_path, sizeof(sock_path), "%s", cli_default_socket_path());
   platform_mkdir_p(cli_config_dir(), 0700);

   if (try_server(sock_path, 500, required_method))
      return sock_path;

   char sock_arg[MAX_PATH + 16];
   snprintf(sock_arg, sizeof(sock_arg), "--socket=%s", sock_path);

   char server_path[MAX_PATH];
   server_path_next_to_client(server_path, sizeof(server_path));
   const char *argv_same_dir[] = {server_path, sock_arg, NULL};
   pid_t pid = platform_spawn_daemon(argv_same_dir);

   if (pid < 0)
   {
      const char *argv_path[] = {"aimee-server.exe", sock_arg, NULL};
      pid = platform_spawn_daemon(argv_path);
   }
   if (pid < 0)
      return NULL;

   if (wait_for_ready(sock_path, SERVER_READY_TIMEOUT_MS, required_method))
      return sock_path;

   fprintf(stderr, "aimee: server did not become ready (30s timeout)\n");
   return NULL;
}

const char *cli_ensure_server(void)
{
   return cli_ensure_server_for_method(NULL);
}

/* Local server lifecycle is owned by systemd/launchd on POSIX; on Windows the
 * thin client targets a remote/Docker aimee-server, so these are informative
 * stubs (the POSIX implementations live in posix/cli_client.c). */
int cli_start_server(void)
{
   fprintf(stderr, "aimee: starting a local server is not supported on Windows; "
                   "point the client at a Linux/Docker aimee-server.\n");
   return 1;
}

int cli_restart_server(void)
{
   fprintf(stderr, "aimee: restarting a local server is not supported on Windows.\n");
   return 1;
}

const char *cli_ensure_server_for_method(const char *method)
{
   const char *sock = cli_existing_server_for_method(method);
   if (sock)
      return sock;

   const char *no_auto = getenv("AIMEE_NO_AUTOSTART");
   if (no_auto && no_auto[0])
      return NULL;

   return spawn_server(method);
}

const char *cli_existing_server(void)
{
   return cli_existing_server_for_method(NULL);
}

const char *cli_existing_server_for_method(const char *method)
{
   const char *env_sock = getenv("AIMEE_SOCK");
   if (env_sock && env_sock[0] && try_server(env_sock, 100, method))
      return env_sock;

   const char *well_known = cli_default_socket_path();
   if (try_server(well_known, 200, method))
      return well_known;

   return NULL;
}

/* ── /v1 HTTP client (Winsock) ──────────────────────────────────────────────
 * Windows counterparts to the POSIX helpers in posix/cli_client.c. There is no
 * Unix-domain socket on Windows, so only TCP endpoints ("tcp:host:port" or
 * "host:port") are supported; a "unix:"/absolute-path endpoint (the co-located
 * aimee-http.sock used by interactive chat) returns NULL — the Windows thin
 * client reaches aimee-server remotely. */

int cli_v1_pct_encode(const char *in, char *out, size_t cap)
{
   static const char *hex = "0123456789ABCDEF";
   size_t o = 0;
   for (size_t i = 0; in && in[i]; i++)
   {
      unsigned char ch = (unsigned char)in[i];
      int unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
      if (unreserved)
      {
         if (o + 1 >= cap)
            return -1;
         out[o++] = (char)ch;
      }
      else
      {
         if (o + 3 >= cap)
            return -1;
         out[o++] = '%';
         out[o++] = hex[ch >> 4];
         out[o++] = hex[ch & 0xF];
      }
   }
   if (o >= cap)
      return -1;
   out[o] = '\0';
   return 0;
}

/* Connect a TCP socket to a "[tcp:]host:port" endpoint. Returns INVALID_SOCKET
 * for UDS/local endpoints (unsupported on Windows) or on failure. host_out gets
 * the bare host for the HTTP Host header. */
static SOCKET cli_win_http_connect(const char *endpoint, char *host_out, size_t host_n,
                                   int timeout_ms)
{
   if (host_out && host_n)
      snprintf(host_out, host_n, "localhost");
   if (!endpoint || !endpoint[0])
      return INVALID_SOCKET;
   if (strncmp(endpoint, "unix:", 5) == 0 || endpoint[0] == '/')
      return INVALID_SOCKET; /* no Unix-domain socket on Windows */

   const char *hp = endpoint;
   if (strncmp(hp, "tcp:", 4) == 0)
      hp += 4;
   char host[256];
   const char *port_str;
   if (hp[0] == '[')
   {
      const char *close_br = strchr(hp, ']');
      if (!close_br || close_br[1] != ':')
         return INVALID_SOCKET;
      size_t hl = (size_t)(close_br - hp - 1);
      if (hl == 0 || hl >= sizeof(host))
         return INVALID_SOCKET;
      memcpy(host, hp + 1, hl);
      host[hl] = '\0';
      port_str = close_br + 2;
   }
   else
   {
      const char *colon = strrchr(hp, ':');
      if (!colon)
         return INVALID_SOCKET;
      size_t hl = (size_t)(colon - hp);
      if (hl == 0 || hl >= sizeof(host))
         return INVALID_SOCKET;
      memcpy(host, hp, hl);
      host[hl] = '\0';
      port_str = colon + 1;
   }
   if (!port_str[0] || atoi(port_str) <= 0 || atoi(port_str) > 65535)
      return INVALID_SOCKET;
   if (host_out && host_n)
      snprintf(host_out, host_n, "%s", host);

   static int wsa_started = 0;
   if (!wsa_started)
   {
      WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
         return INVALID_SOCKET;
      wsa_started = 1;
   }

   struct addrinfo hints, *res = NULL;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
      return INVALID_SOCKET;
   SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (s == INVALID_SOCKET)
   {
      freeaddrinfo(res);
      return INVALID_SOCKET;
   }
   DWORD tmo = (DWORD)(timeout_ms > 0 ? timeout_ms : CLIENT_DEFAULT_TIMEOUT_MS);
   setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
   if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0)
   {
      closesocket(s);
      freeaddrinfo(res);
      return INVALID_SOCKET;
   }
   freeaddrinfo(res);
   return s;
}

/* Send a fully-built HTTP request over s and read the whole response into a
 * heap buffer (Connection: close). Returns the buffer (caller frees) + length;
 * NULL on failure. */
static char *cli_win_http_exec(const char *endpoint, const char *method, const char *path,
                               const char *body, const char *bearer, int timeout_ms,
                               size_t *len_out)
{
   if (len_out)
      *len_out = 0;
   char host[256];
   SOCKET s = cli_win_http_connect(endpoint, host, sizeof(host), timeout_ms);
   if (s == INVALID_SOCKET)
      return NULL;
   size_t blen = body ? strlen(body) : 0;
   size_t reqcap = blen + strlen(path) + strlen(host) + (bearer ? strlen(bearer) : 0) + 256;
   char *req = malloc(reqcap);
   if (!req)
   {
      closesocket(s);
      return NULL;
   }
   int reqlen;
   if (bearer && bearer[0])
      reqlen = snprintf(req, reqcap,
                        "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
                        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        method, path, host, bearer, blen);
   else
      reqlen = snprintf(req, reqcap,
                        "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                        method, path, host, blen);
   if (reqlen < 0 || (size_t)reqlen >= reqcap || (blen && (size_t)reqlen + blen >= reqcap))
   {
      free(req);
      closesocket(s);
      return NULL;
   }
   if (blen)
   {
      memcpy(req + reqlen, body, blen);
      reqlen += (int)blen;
   }
   int off = 0;
   while (off < reqlen)
   {
      int n = send(s, req + off, reqlen - off, 0);
      if (n <= 0)
      {
         free(req);
         closesocket(s);
         return NULL;
      }
      off += n;
   }
   free(req);

   size_t cap = 8192, len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      closesocket(s);
      return NULL;
   }
   for (;;)
   {
      if (len + 4096 > cap)
      {
         if (cap >= CLIENT_MAX_RESPONSE_SIZE)
            break;
         size_t next = cap * 2;
         char *grown = realloc(buf, next);
         if (!grown)
         {
            free(buf);
            closesocket(s);
            return NULL;
         }
         buf = grown;
         cap = next;
      }
      int n = recv(s, buf + len, (int)(cap - len - 1), 0);
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   closesocket(s);
   buf[len] = '\0';
   if (len_out)
      *len_out = len;
   return buf;
}

cJSON *cli_http_request(const char *endpoint, const char *method, const char *path,
                        const char *body_json, const char *bearer, int timeout_ms, int *http_status)
{
   if (http_status)
      *http_status = 0;
   if (!endpoint || !method || !path)
      return NULL;
   char *resp = cli_win_http_exec(endpoint, method, path, body_json, bearer, timeout_ms, NULL);
   if (!resp)
      return NULL;
   int status = 0;
   if (strncmp(resp, "HTTP/", 5) == 0)
   {
      const char *sp = strchr(resp, ' ');
      if (sp)
         status = atoi(sp + 1);
   }
   if (http_status)
      *http_status = status;
   const char *bodyp = strstr(resp, "\r\n\r\n");
   cJSON *parsed = bodyp ? cJSON_Parse(bodyp + 4) : NULL;
   free(resp);
   return parsed;
}

cJSON *cli_http_request_stream_ndjson(const char *endpoint, const char *method, const char *path,
                                      const char *body_json, const char *bearer, int timeout_ms,
                                      int *http_status, cli_stream_cb cb, void *userdata,
                                      void (*on_open)(int fd, void *ud), void *on_open_ud)
{
   /* The streaming body is read in one pass after the response completes; the
    * synchronous Winsock read above already buffers it. Windows interactive
    * chat is remote-only, so on_open (the TUI abort fd hook) is unused here. */
   (void)on_open;
   (void)on_open_ud;
   if (http_status)
      *http_status = 0;
   if (!endpoint || !method || !path)
      return NULL;
   /* The caller passes the chat idle-timeout sentinel (-1 = "block until the
    * server streams / closes"). cli_win_http_connect maps any non-positive value
    * to the tiny 5s default SO_RCVTIMEO, which truncates a chat turn the moment
    * the provider takes >5s to respond — the stream is read in one synchronous
    * pass, so a long recv window is required. Use a generous bound instead. */
   if (timeout_ms <= 0)
      timeout_ms = 600000; /* 10 min — covers provider latency + tool round-trips */
   size_t len = 0;
   char *resp = cli_win_http_exec(endpoint, method, path, body_json, bearer, timeout_ms, &len);
   if (!resp)
      return NULL;
   if (http_status && strncmp(resp, "HTTP/", 5) == 0)
   {
      const char *sp = strchr(resp, ' ');
      if (sp)
         *http_status = atoi(sp + 1);
   }
   char *bodyp = strstr(resp, "\r\n\r\n");
   cJSON *final = NULL;
   if (bodyp)
   {
      char *line = bodyp + 4;
      while (line && *line)
      {
         char *nl = strchr(line, '\n');
         if (nl)
            *nl = '\0';
         size_t ll = strlen(line);
         if (ll > 0 && line[ll - 1] == '\r')
            line[ll - 1] = '\0';
         if (line[0])
         {
            cJSON *msg = cJSON_Parse(line);
            if (msg)
            {
               if (cJSON_GetObjectItem(msg, "status"))
               {
                  if (final)
                     cJSON_Delete(final);
                  final = msg;
               }
               else
               {
                  if (cb)
                     (void)cb(msg, userdata);
                  cJSON_Delete(msg);
               }
            }
         }
         line = nl ? nl + 1 : NULL;
      }
   }
   free(resp);
   return final;
}

/* cli_v1_dispatch_local now lives in the shared cli_v1_routes.inc — it resolves the
 * method's first-class /v1 route (no more POST /v1/rpc bridge). On Windows
 * cli_v1_send routes it through aimee_client_request to the configured remote.
 *
 * cmd_workspace_serve + the reverse-channel helpers now build on Windows too
 * (cli_workspace_serve.c, native-threaded via _beginthreadex), so the Windows
 * thin client serves its working tree to a remote aimee-server just like POSIX. */

#include "../cli_v1_routes.inc"

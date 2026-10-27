/* cli_client.c: Windows named-pipe client for aimee-server RPC, plus the Winsock
 * /v1 HTTP client the thin client's shared code (cli_main/cli_chat_stream/cli_mcp_serve)
 * now calls. Windows has no Unix-domain socket, so the local aimee-http.sock
 * ("unix:" / absolute-path) transport is unsupported here — those callers fail
 * gracefully; the cross-platform remote path goes through aimee_client_request. */
#include "cli_client.h"
#include <aimee/ir/session_event.h>
#include "json_wire.h"
#include "aimee_client.h"
#include "aimee_home.h"
#include "aimee_version.h"
#include "cli_server_compat.h"
#include "platform_ipc.h"
#include "platform_path.h"
#include "platform_process.h"
#include "cJSON.h"
#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/endpoint.h>
#include <aimee/core/connection/http1.h>
#include <aimee/core/connection/socket.h>
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
   static char path[MAX_PATH + sizeof("\\aimee.sock")];
   if (path[0])
      return path;

   snprintf(path, sizeof(path), "%s\\aimee.sock", cli_config_dir());
   return path;
}

static int last_connect_errno = 0;
static char last_connect_path[MAX_PATH + sizeof("\\aimee.sock")] = "";

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

/* wait_for_ready / server_path_next_to_client / spawn_server removed: the
 * Windows client no longer starts a local aimee-server. cli_start_server()
 * and cli_restart_server() below already refused to, and said so; the
 * implicit spawn behind cli_ensure_server_for_method contradicted them. */

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
   /* Pure lookup. The POSIX client stopped spawning aimee-server on demand in
    * #1660 -- every CLI invocation being a potential server-spawner was the root
    * of five orphan-listener fixes -- and stopped DISCOVERING one in the
    * remote-only cutover. Windows kept both, so a Windows client still started a
    * local server and bound itself to whatever it found: the topology aimee does
    * not have, since the server runs in its own container, remotely. */
   return cli_existing_server_for_method(method);
}

const char *cli_existing_server(void)
{
   return cli_existing_server_for_method(NULL);
}

const char *cli_existing_server_for_method(const char *method)
{
   /* AIMEE_SOCK only: an EXPLICIT operator/entrypoint choice, which is how the
    * CLI inside the server's own container reaches it. Probing the well-known
    * socket was discovery -- a client that finds a server by looking around the
    * filesystem cannot tell its own server from a stray one, and answers
    * confidently either way. Same rule the POSIX client enforces. */
   const char *env_sock = getenv("AIMEE_SOCK");
   if (env_sock && env_sock[0] && try_server(env_sock, 100, method))
      return env_sock;
   return NULL;
}

/* ── /v1 HTTP client (Winsock) ──────────────────────────────────────────────
 * Windows counterparts to the POSIX helpers in posix/cli_client.c. There is no
 * Unix-domain socket on Windows, so only TCP endpoints ("tcp:host:port" or
 * "host:port") are supported; a "unix:"/absolute-path endpoint (the co-located
 * aimee-http.sock used by interactive chat) returns NULL — the Windows thin
 * client reaches aimee-server remotely. */


/* cli_v1_dispatch_local now lives in the shared cli_v1_routes.inc — it resolves the
 * method's first-class /v1 route (no more POST /v1/rpc bridge). On Windows
 * cli_v1_send routes it through aimee_client_request to the configured remote.
 *
 * cmd_workspace_serve + the reverse-channel helpers now build on Windows too
 * (cli_workspace_serve.c, native-threaded via _beginthreadex), so the Windows
 * thin client serves its working tree to a remote aimee-server just like POSIX. */

#include "cli_v1_routes.h"

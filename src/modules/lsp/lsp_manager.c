/* lsp_manager.c: per-workspace LSP server lifecycle management.
 *
 * Manages a pool of LSP server processes, one per (workspace, extension)
 * pair. Servers are spawned lazily on first file touch and shut down
 * cleanly via lsp_manager_shutdown_all() at session end.
 *
 * JSON-RPC framing is handled by lsp_client.c. Config is read from the
 * global aimee config (lsp_servers array in aimee.yaml).
 *
 * Layer 0 (Foundation) — depends on lsp_client, config, platform, cJSON.
 * No agent or command layer includes permitted here.
 */
#include "aimee.h"
#include "lsp.h"

#ifdef AIMEE_WINDOWS

#include <stdio.h>

static int lsp_manager_unsupported(char *errbuf, size_t errbuf_size)
{
   if (errbuf && errbuf_size > 0)
      snprintf(errbuf, errbuf_size, "LSP manager is not supported on Windows builds");
   return -1;
}

void lsp_manager_init(void)
{
}

void lsp_manager_shutdown_all(void)
{
}

int lsp_manager_diagnostics(const char *workspace, const char *file, lsp_diag_t *out, int max)
{
   (void)workspace;
   (void)file;
   (void)out;
   (void)max;
   return 0;
}

int lsp_manager_definition(const char *workspace, const char *file, int line, int col,
                           lsp_location_t *out, int max, char *errbuf, size_t errbuf_size)
{
   (void)workspace;
   (void)file;
   (void)line;
   (void)col;
   (void)out;
   (void)max;
   return lsp_manager_unsupported(errbuf, errbuf_size);
}

int lsp_manager_references(const char *workspace, const char *file, int line, int col,
                           lsp_location_t *out, int max, char *errbuf, size_t errbuf_size)
{
   (void)workspace;
   (void)file;
   (void)line;
   (void)col;
   (void)out;
   (void)max;
   return lsp_manager_unsupported(errbuf, errbuf_size);
}

int lsp_manager_rename(const char *workspace, const char *file, int line, int col,
                       const char *new_name, char *out, size_t out_size, char *errbuf,
                       size_t errbuf_size)
{
   (void)workspace;
   (void)file;
   (void)line;
   (void)col;
   (void)new_name;
   if (out && out_size > 0)
      out[0] = '\0';
   return lsp_manager_unsupported(errbuf, errbuf_size);
}

void lsp_manager_diag_summary(int *errors, int *warnings, int *active_servers)
{
   if (errors)
      *errors = 0;
   if (warnings)
      *warnings = 0;
   if (active_servers)
      *active_servers = 0;
}

#else

#include "config.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define LSP_MAX_SERVERS        16
#define LSP_MAX_DIAGS_PER_FILE 64
#define LSP_MAX_FILES          128
#define LSP_REQUEST_TIMEOUT_MS 5000
#define LSP_STARTUP_TIMEOUT_MS 10000

/* -----------------------------------------------------------------------
 * Types
 * ----------------------------------------------------------------------- */

/* Per-file diagnostic store */
typedef struct
{
   char path[LSP_PATH_MAX];
   lsp_diag_t diags[LSP_MAX_DIAGS_PER_FILE];
   int ndiags;
} lsp_file_diags_t;

/* A single active LSP server connection */
typedef struct
{
   int active; /* 1 if slot is in use */
   char workspace[LSP_PATH_MAX];
   char extensions[16][16]; /* e.g. ".rs", ".go" */
   int next_id;             /* JSON-RPC request id counter */
   pid_t pid;
   int stdin_fd;  /* write end → server stdin */
   int stdout_fd; /* read end  ← server stdout */
   pthread_t reader_tid;
   int initialized;

   /* Diagnostic store for all files managed by this server */
   lsp_file_diags_t files[LSP_MAX_FILES];
   int nfiles;
   pthread_mutex_t diag_lock;
   pthread_mutex_t io_lock;
   int reader_started;
} lsp_server_t;

/* -----------------------------------------------------------------------
 * Global state
 * ----------------------------------------------------------------------- */

static lsp_server_t g_servers[LSP_MAX_SERVERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

static void dispatch_message(lsp_server_t *srv, cJSON *msg, const char *raw_json);
static cJSON *read_response_locked(lsp_server_t *srv, int req_id, int timeout_ms);

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

static const char *extension_of(const char *file)
{
   const char *dot = strrchr(file, '.');
   return dot ? dot : "";
}

/* Build a file:// URI from an absolute path */
static void path_to_uri(const char *path, char *uri, size_t uri_size)
{
   snprintf(uri, uri_size, "file://%s", path);
}

/* Build a minimal JSON-RPC 2.0 notification (no id) */
static char *build_notification(const char *method, cJSON *params)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
   cJSON_AddStringToObject(msg, "method", method);
   if (params)
      cJSON_AddItemToObject(msg, "params", params);
   else
      cJSON_AddObjectToObject(msg, "params");
   char *json = cJSON_PrintUnformatted(msg);
   cJSON_Delete(msg);
   return json;
}

/* Build a minimal JSON-RPC 2.0 request (with id) */
static char *build_request(int id, const char *method, cJSON *params)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(msg, "id", id);
   cJSON_AddStringToObject(msg, "method", method);
   if (params)
      cJSON_AddItemToObject(msg, "params", params);
   else
      cJSON_AddObjectToObject(msg, "params");
   char *json = cJSON_PrintUnformatted(msg);
   cJSON_Delete(msg);
   return json;
}

/* -----------------------------------------------------------------------
 * Background reader thread — drains server stdout, dispatches notifications
 * ----------------------------------------------------------------------- */

static void store_diagnostics(lsp_server_t *srv, const char *uri, const char *json_notification)
{
   /* Convert URI to path: strip "file://" prefix */
   const char *file_path = uri;
   if (strncmp(uri, "file://", 7) == 0)
      file_path = uri + 7;

   lsp_diag_t tmp[LSP_MAX_DIAGS_PER_FILE];
   int n = lsp_parse_diagnostics(json_notification, file_path, tmp, LSP_MAX_DIAGS_PER_FILE);

   pthread_mutex_lock(&srv->diag_lock);

   /* Find or create file slot */
   lsp_file_diags_t *slot = NULL;
   for (int i = 0; i < srv->nfiles; i++)
   {
      if (strcmp(srv->files[i].path, file_path) == 0)
      {
         slot = &srv->files[i];
         break;
      }
   }
   if (!slot && srv->nfiles < LSP_MAX_FILES)
   {
      slot = &srv->files[srv->nfiles++];
      snprintf(slot->path, sizeof(slot->path), "%s", file_path);
   }
   if (slot)
   {
      slot->ndiags = n < LSP_MAX_DIAGS_PER_FILE ? n : LSP_MAX_DIAGS_PER_FILE;
      memcpy(slot->diags, tmp, (size_t)slot->ndiags * sizeof(lsp_diag_t));
   }

   pthread_mutex_unlock(&srv->diag_lock);
}

static void dispatch_message(lsp_server_t *srv, cJSON *msg, const char *raw_json)
{
   cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
   if (cJSON_IsString(method) &&
       strcmp(method->valuestring, "textDocument/publishDiagnostics") == 0)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
      cJSON *uri_j = params ? cJSON_GetObjectItemCaseSensitive(params, "uri") : NULL;
      const char *uri = cJSON_IsString(uri_j) ? uri_j->valuestring : "";
      store_diagnostics(srv, uri, raw_json);
   }
}

static cJSON *read_response_locked(lsp_server_t *srv, int req_id, int timeout_ms)
{
   char buf[512 * 1024];
   struct timespec start;
   if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
      memset(&start, 0, sizeof(start));

   for (;;)
   {
      struct timespec now;
      if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
         break;

      long elapsed_ms =
          (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
      if (elapsed_ms >= timeout_ms)
         break;

      long remaining_ms = timeout_ms - elapsed_ms;
      struct timeval tv = {(int)(remaining_ms / 1000), (int)((remaining_ms % 1000) * 1000)};
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(srv->stdout_fd, &rfds);

      int sel = select(srv->stdout_fd + 1, &rfds, NULL, NULL, &tv);
      if (sel == 0)
         continue;
      if (sel < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }

      int n = lsp_frame_read(srv->stdout_fd, buf, sizeof(buf));
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (n == 0)
         continue;

      cJSON *msg = cJSON_Parse(buf);
      if (!msg)
         continue;

      cJSON *mid = cJSON_GetObjectItemCaseSensitive(msg, "id");
      if (cJSON_IsNumber(mid) && (int)mid->valuedouble == req_id)
         return msg;

      dispatch_message(srv, msg, buf);
      cJSON_Delete(msg);
   }

   return NULL;
}

static void *reader_thread(void *arg)
{
   lsp_server_t *srv = (lsp_server_t *)arg;
   char buf[1024 * 1024]; /* 1MB read buffer — sufficient for most LSP messages */

   while (srv->active && srv->stdout_fd >= 0)
   {
      struct timeval tv = {0, 200 * 1000};
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(srv->stdout_fd, &rfds);
      int sel = select(srv->stdout_fd + 1, &rfds, NULL, NULL, &tv);
      if (sel == 0)
         continue;
      if (sel < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }

      pthread_mutex_lock(&srv->io_lock);
      if (!srv->active || srv->stdout_fd < 0)
      {
         pthread_mutex_unlock(&srv->io_lock);
         break;
      }
      int n = lsp_frame_read(srv->stdout_fd, buf, sizeof(buf));
      pthread_mutex_unlock(&srv->io_lock);
      if (n < 0)
         break; /* EOF or error — server likely exited */
      if (n == 0)
         continue;

      /* Parse and dispatch */
      cJSON *msg = cJSON_Parse(buf);
      if (!msg)
         continue;
      dispatch_message(srv, msg, buf);
      cJSON_Delete(msg);
   }

   return NULL;
}

/* -----------------------------------------------------------------------
 * Send the LSP initialize request and wait for the response
 * ----------------------------------------------------------------------- */

static int do_initialize(lsp_server_t *srv)
{
   cJSON *params = cJSON_CreateObject();

   /* processId */
   cJSON_AddNumberToObject(params, "processId", (double)getpid());

   /* rootUri */
   char root_uri[LSP_PATH_MAX + 8];
   path_to_uri(srv->workspace, root_uri, sizeof(root_uri));
   cJSON_AddStringToObject(params, "rootUri", root_uri);
   cJSON_AddStringToObject(params, "rootPath", srv->workspace);

   /* clientInfo */
   cJSON *ci = cJSON_AddObjectToObject(params, "clientInfo");
   cJSON_AddStringToObject(ci, "name", "aimee");
   cJSON_AddStringToObject(ci, "version", AIMEE_VERSION);

   /* capabilities (minimal) */
   cJSON *caps = cJSON_AddObjectToObject(params, "capabilities");
   cJSON *text = cJSON_AddObjectToObject(caps, "textDocument");
   cJSON *sync = cJSON_AddObjectToObject(text, "synchronization");
   cJSON_AddBoolToObject(sync, "didSave", 1);
   cJSON *diag_cap = cJSON_AddObjectToObject(text, "publishDiagnostics");
   cJSON_AddBoolToObject(diag_cap, "relatedInformation", 0);

   int req_id = srv->next_id++;
   char *json = build_request(req_id, "initialize", params);
   if (!json)
      return -1;

   pthread_mutex_lock(&srv->io_lock);
   int rc = lsp_frame_write(srv->stdin_fd, json);
   free(json);
   if (rc != 0)
   {
      pthread_mutex_unlock(&srv->io_lock);
      return -1;
   }

   cJSON *resp = read_response_locked(srv, req_id, LSP_STARTUP_TIMEOUT_MS);
   pthread_mutex_unlock(&srv->io_lock);
   if (!resp)
      return -1;

   cJSON *resp_id = cJSON_GetObjectItemCaseSensitive(resp, "id");
   int ok = cJSON_IsNumber(resp_id) && (int)resp_id->valuedouble == req_id;
   cJSON_Delete(resp);

   if (!ok)
      return -1;

   /* Send initialized notification */
   char *notif = build_notification("initialized", cJSON_CreateObject());
   if (notif)
   {
      lsp_frame_write(srv->stdin_fd, notif);
      free(notif);
   }

   return 0;
}

/* -----------------------------------------------------------------------
 * Spawn an LSP server for the given workspace and server config
 * ----------------------------------------------------------------------- */

static lsp_server_t *spawn_server(const config_lsp_server_t *cfg, const char *workspace)
{
   /* Find a free slot */
   lsp_server_t *srv = NULL;
   for (int i = 0; i < LSP_MAX_SERVERS; i++)
   {
      if (!g_servers[i].active)
      {
         srv = &g_servers[i];
         break;
      }
   }
   if (!srv)
      return NULL;

   memset(srv, 0, sizeof(*srv));
   srv->next_id = 1;
   pthread_mutex_init(&srv->diag_lock, NULL);
   pthread_mutex_init(&srv->io_lock, NULL);

   snprintf(srv->workspace, sizeof(srv->workspace), "%s", workspace);
   for (int i = 0; i < cfg->extension_count && i < 16; i++)
      snprintf(srv->extensions[i], sizeof(srv->extensions[i]), "%s", cfg->extensions[i]);

   /* Build argv: command + args[] + NULL */
   const char *argv[64];
   int argc = 0;
   argv[argc++] = cfg->command;
   for (int i = 0; i < cfg->arg_count && argc < 62; i++)
      argv[argc++] = cfg->args[i];
   argv[argc] = NULL;

   /* Create stdin and stdout pipes */
   int stdin_pipe[2];  /* [0]=read(server), [1]=write(us) */
   int stdout_pipe[2]; /* [0]=read(us),     [1]=write(server) */

   if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0)
      return NULL;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      return NULL;
   }

   if (pid == 0)
   {
      /* Child: wire up pipes and exec */
      dup2(stdin_pipe[0], STDIN_FILENO);
      dup2(stdout_pipe[1], STDOUT_FILENO);

      /* Redirect stderr to /dev/null to avoid polluting our output */
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0)
         dup2(devnull, STDERR_FILENO);

      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      if (devnull >= 0)
         close(devnull);

      /* Set working directory to workspace */
      if (chdir(workspace) != 0)
         _exit(1);

      execvp(argv[0], (char *const *)argv);
      _exit(127);
   }

   /* Parent */
   close(stdin_pipe[0]);
   close(stdout_pipe[1]);

   srv->pid = pid;
   srv->stdin_fd = stdin_pipe[1];
   srv->stdout_fd = stdout_pipe[0];

   if (do_initialize(srv) != 0)
   {
      kill(pid, SIGTERM);
      close(srv->stdin_fd);
      close(srv->stdout_fd);
      waitpid(pid, NULL, 0);
      pthread_mutex_destroy(&srv->diag_lock);
      pthread_mutex_destroy(&srv->io_lock);
      return NULL;
   }

   srv->initialized = 1;
   srv->active = 1;

   /* Start background reader thread */
   if (pthread_create(&srv->reader_tid, NULL, reader_thread, srv) == 0)
      srv->reader_started = 1;

   return srv;
}

/* -----------------------------------------------------------------------
 * Find or spawn a server for the given (workspace, file extension) pair
 * ----------------------------------------------------------------------- */

static lsp_server_t *get_or_spawn(const char *workspace, const char *ext)
{
   /* Look for an existing active server for this workspace+extension */
   for (int i = 0; i < LSP_MAX_SERVERS; i++)
   {
      lsp_server_t *srv = &g_servers[i];
      if (!srv->active)
         continue;
      if (strcmp(srv->workspace, workspace) != 0)
         continue;
      for (int j = 0; j < 16; j++)
      {
         if (srv->extensions[j][0] == '\0')
            break;
         if (strcmp(srv->extensions[j], ext) == 0)
            return srv;
      }
   }

   /* No existing server — try to spawn one from config */
   config_t cfg;
   if (config_load(&cfg) != 0)
      return NULL;

   for (int i = 0; i < cfg.lsp_server_count; i++)
   {
      const config_lsp_server_t *sc = &cfg.lsp_servers[i];
      for (int j = 0; j < sc->extension_count; j++)
      {
         if (strcmp(sc->extensions[j], ext) == 0)
            return spawn_server(sc, workspace);
      }
   }

   return NULL; /* no LSP server configured for this extension */
}

/* -----------------------------------------------------------------------
 * Send a blocking JSON-RPC request and return the parsed response.
 * Caller must cJSON_Delete the returned object.
 * Returns NULL on timeout or error.
 * ----------------------------------------------------------------------- */

static cJSON *send_request(lsp_server_t *srv, const char *method, cJSON *params)
{
   int req_id = srv->next_id++;
   char *json = build_request(req_id, method, params);
   if (!json)
      return NULL;

   pthread_mutex_lock(&srv->io_lock);
   int rc = lsp_frame_write(srv->stdin_fd, json);
   free(json);
   if (rc != 0)
   {
      pthread_mutex_unlock(&srv->io_lock);
      return NULL;
   }

   cJSON *result = read_response_locked(srv, req_id, LSP_REQUEST_TIMEOUT_MS);
   pthread_mutex_unlock(&srv->io_lock);
   return result;
}

/* -----------------------------------------------------------------------
 * Parse an array of Location objects from an LSP response
 * ----------------------------------------------------------------------- */

static int parse_locations(cJSON *result_arr, const char *symbol, lsp_location_t *out, int max)
{
   if (!cJSON_IsArray(result_arr))
   {
      /* Single Location object */
      if (!cJSON_IsObject(result_arr))
         return 0;
      cJSON *uri = cJSON_GetObjectItemCaseSensitive(result_arr, "uri");
      cJSON *range = cJSON_GetObjectItemCaseSensitive(result_arr, "range");
      if (!cJSON_IsString(uri) || !cJSON_IsObject(range))
         return 0;

      lsp_location_t *loc = &out[0];
      memset(loc, 0, sizeof(*loc));
      if (symbol)
         snprintf(loc->symbol, sizeof(loc->symbol), "%s", symbol);
      const char *path = uri->valuestring;
      if (strncmp(path, "file://", 7) == 0)
         path += 7;
      snprintf(loc->file, sizeof(loc->file), "%s", path);

      cJSON *start = cJSON_GetObjectItemCaseSensitive(range, "start");
      if (cJSON_IsObject(start))
      {
         cJSON *ln = cJSON_GetObjectItemCaseSensitive(start, "line");
         cJSON *ch = cJSON_GetObjectItemCaseSensitive(start, "character");
         if (cJSON_IsNumber(ln))
            loc->line = (int)ln->valuedouble;
         if (cJSON_IsNumber(ch))
            loc->col = (int)ch->valuedouble;
      }
      return 1;
   }

   int count = 0;
   const cJSON *loc_j;
   cJSON_ArrayForEach(loc_j, result_arr)
   {
      if (count >= max)
         break;
      cJSON *uri = cJSON_GetObjectItemCaseSensitive(loc_j, "uri");
      cJSON *range = cJSON_GetObjectItemCaseSensitive(loc_j, "range");
      if (!cJSON_IsString(uri) || !cJSON_IsObject(range))
         continue;

      lsp_location_t *loc = &out[count];
      memset(loc, 0, sizeof(*loc));
      if (symbol)
         snprintf(loc->symbol, sizeof(loc->symbol), "%s", symbol);
      const char *path = uri->valuestring;
      if (strncmp(path, "file://", 7) == 0)
         path += 7;
      snprintf(loc->file, sizeof(loc->file), "%s", path);

      cJSON *start = cJSON_GetObjectItemCaseSensitive(range, "start");
      if (cJSON_IsObject(start))
      {
         cJSON *ln = cJSON_GetObjectItemCaseSensitive(start, "line");
         cJSON *ch = cJSON_GetObjectItemCaseSensitive(start, "character");
         if (cJSON_IsNumber(ln))
            loc->line = (int)ln->valuedouble;
         if (cJSON_IsNumber(ch))
            loc->col = (int)ch->valuedouble;
      }
      count++;
   }
   return count;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

void lsp_manager_init(void)
{
   pthread_mutex_lock(&g_lock);
   if (!g_initialized)
   {
      memset(g_servers, 0, sizeof(g_servers));
      g_initialized = 1;
   }
   pthread_mutex_unlock(&g_lock);
}

void lsp_manager_shutdown_all(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < LSP_MAX_SERVERS; i++)
   {
      lsp_server_t *srv = &g_servers[i];
      if (!srv->active)
         continue;

      srv->active = 0;

      /* Send shutdown request then exit notification */
      if (srv->stdin_fd >= 0)
      {
         /* shutdown */
         cJSON *p = cJSON_CreateObject();
         char *json = build_request(srv->next_id++, "shutdown", p);
         if (json)
         {
            lsp_frame_write(srv->stdin_fd, json);
            free(json);
         }
         /* exit notification */
         json = build_notification("exit", NULL);
         if (json)
         {
            lsp_frame_write(srv->stdin_fd, json);
            free(json);
         }
         close(srv->stdin_fd);
         srv->stdin_fd = -1;
      }

      if (srv->stdout_fd >= 0)
      {
         close(srv->stdout_fd);
         srv->stdout_fd = -1;
      }

      if (srv->pid > 0)
      {
         /* Give the server 500ms to exit cleanly before SIGTERM */
         struct timespec ts = {0, 500 * 1000 * 1000};
         nanosleep(&ts, NULL);
         int status;
         if (waitpid(srv->pid, &status, WNOHANG) == 0)
         {
            kill(srv->pid, SIGTERM);
            waitpid(srv->pid, NULL, 0);
         }
         srv->pid = 0;
      }

      if (srv->reader_started)
      {
         pthread_join(srv->reader_tid, NULL);
         srv->reader_started = 0;
      }

      pthread_mutex_destroy(&srv->io_lock);
      pthread_mutex_destroy(&srv->diag_lock);
   }
   pthread_mutex_unlock(&g_lock);
}

int lsp_manager_diagnostics(const char *workspace, const char *file, lsp_diag_t *out, int max)
{
   if (!workspace || !out || max <= 0)
      return 0;

   int count = 0;

   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < LSP_MAX_SERVERS && count < max; i++)
   {
      lsp_server_t *srv = &g_servers[i];
      if (!srv->active || strcmp(srv->workspace, workspace) != 0)
         continue;

      pthread_mutex_lock(&srv->diag_lock);
      for (int fi = 0; fi < srv->nfiles && count < max; fi++)
      {
         lsp_file_diags_t *fd = &srv->files[fi];
         if (file && strcmp(fd->path, file) != 0)
            continue;
         for (int di = 0; di < fd->ndiags && count < max; di++)
            out[count++] = fd->diags[di];
      }
      pthread_mutex_unlock(&srv->diag_lock);
   }
   pthread_mutex_unlock(&g_lock);

   return count;
}

int lsp_manager_definition(const char *workspace, const char *file, int line, int col,
                           lsp_location_t *out, int max, char *errbuf, size_t errbuf_size)
{
   if (!workspace || !file || !out || max <= 0)
      return -1;

   const char *ext = extension_of(file);

   pthread_mutex_lock(&g_lock);
   lsp_server_t *srv = get_or_spawn(workspace, ext);
   pthread_mutex_unlock(&g_lock);

   if (!srv)
   {
      if (errbuf)
         snprintf(errbuf, errbuf_size, "no LSP server configured for extension '%s'", ext);
      return -1;
   }

   char uri[LSP_PATH_MAX + 8];
   path_to_uri(file, uri, sizeof(uri));

   cJSON *params = cJSON_CreateObject();
   cJSON *td = cJSON_AddObjectToObject(params, "textDocument");
   cJSON_AddStringToObject(td, "uri", uri);
   cJSON *pos = cJSON_AddObjectToObject(params, "position");
   cJSON_AddNumberToObject(pos, "line", line);
   cJSON_AddNumberToObject(pos, "character", col);

   cJSON *resp = send_request(srv, "textDocument/definition", params);
   if (!resp)
   {
      if (errbuf)
         snprintf(errbuf, errbuf_size, "LSP request timed out");
      return -1;
   }

   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   int n = 0;
   if (result && !cJSON_IsNull(result))
      n = parse_locations(result, NULL, out, max);

   cJSON_Delete(resp);
   return n;
}

int lsp_manager_references(const char *workspace, const char *file, int line, int col,
                           lsp_location_t *out, int max, char *errbuf, size_t errbuf_size)
{
   if (!workspace || !file || !out || max <= 0)
      return -1;

   const char *ext = extension_of(file);

   pthread_mutex_lock(&g_lock);
   lsp_server_t *srv = get_or_spawn(workspace, ext);
   pthread_mutex_unlock(&g_lock);

   if (!srv)
   {
      if (errbuf)
         snprintf(errbuf, errbuf_size, "no LSP server configured for extension '%s'", ext);
      return -1;
   }

   char uri[LSP_PATH_MAX + 8];
   path_to_uri(file, uri, sizeof(uri));

   cJSON *params = cJSON_CreateObject();
   cJSON *td = cJSON_AddObjectToObject(params, "textDocument");
   cJSON_AddStringToObject(td, "uri", uri);
   cJSON *pos = cJSON_AddObjectToObject(params, "position");
   cJSON_AddNumberToObject(pos, "line", line);
   cJSON_AddNumberToObject(pos, "character", col);
   cJSON *ctx = cJSON_AddObjectToObject(params, "context");
   cJSON_AddBoolToObject(ctx, "includeDeclaration", 1);

   cJSON *resp = send_request(srv, "textDocument/references", params);
   if (!resp)
   {
      if (errbuf)
         snprintf(errbuf, errbuf_size, "LSP request timed out");
      return -1;
   }

   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   int n = 0;
   if (result && !cJSON_IsNull(result))
      n = parse_locations(result, NULL, out, max);

   cJSON_Delete(resp);
   return n;
}

/* -----------------------------------------------------------------------
 * Apply a single TextEdit to a file's in-memory line array.
 * Edits must be applied in reverse order (highest line first) to preserve offsets.
 * ----------------------------------------------------------------------- */

/* Apply workspace/rename edits stored in the LSP response.
 * |result| is the "result" field of the rename response (a WorkspaceEdit).
 * For each file in result.changes (uri → TextEdit[]), read the file, apply
 * edits in reverse order, and write it back.
 * Returns the number of files modified. */
static int apply_workspace_edit(cJSON *result, char *out, size_t out_size)
{
   if (!result || !cJSON_IsObject(result))
      return 0;

   cJSON *changes = cJSON_GetObjectItemCaseSensitive(result, "changes");
   if (!cJSON_IsObject(changes))
      return 0;

   int files_changed = 0;
   char *p = out;
   size_t left = out_size;

   cJSON *entry;
   cJSON_ArrayForEach(entry, changes)
   {
      /* Key is the file URI */
      const char *uri = entry->string;
      if (!uri)
         continue;

      const char *path = uri;
      if (strncmp(uri, "file://", 7) == 0)
         path = uri + 7;

      if (!cJSON_IsArray(entry))
         continue;

      /* Read file into memory */
      FILE *f = fopen(path, "r");
      if (!f)
         continue;

      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);

      if (sz < 0 || sz > 64 * 1024 * 1024) /* 64MB limit */
      {
         fclose(f);
         continue;
      }

      char *content = malloc((size_t)sz + 1);
      if (!content)
      {
         fclose(f);
         continue;
      }

      size_t rd = fread(content, 1, (size_t)sz, f);
      content[rd] = '\0';
      fclose(f);

      /* Split content into lines (keep newlines attached to each line) */
      char **lines = NULL;
      int nlines = 0;
      int lcap = 64;
      lines = malloc((size_t)lcap * sizeof(char *));
      if (!lines)
      {
         free(content);
         continue;
      }

      char *lp = content;
      while (*lp)
      {
         char *nl = strchr(lp, '\n');
         size_t llen = nl ? (size_t)(nl - lp + 1) : strlen(lp);
         char *line = strndup(lp, llen);
         if (!line)
         {
            free(content);
            for (int i = 0; i < nlines; i++)
               free(lines[i]);
            free(lines);
            goto next_entry;
         }
         if (nlines >= lcap)
         {
            lcap *= 2;
            char **tmp = realloc(lines, (size_t)lcap * sizeof(char *));
            if (!tmp)
            {
               free(line);
               free(content);
               for (int i = 0; i < nlines; i++)
                  free(lines[i]);
               free(lines);
               goto next_entry;
            }
            lines = tmp;
         }
         lines[nlines++] = line;
         lp = nl ? nl + 1 : lp + llen;
      }
      free(content);

      /* Count edits and sort them in reverse order (highest line first) */
      int nedits = cJSON_GetArraySize(entry);
      typedef struct
      {
         int sl;
         int sc;
         int el;
         int ec;
         const char *new_text;
      } edit_t;
      edit_t *edits = calloc((size_t)nedits, sizeof(edit_t));
      if (!edits)
      {
         for (int i = 0; i < nlines; i++)
            free(lines[i]);
         free(lines);
         goto next_entry;
      }

      int ei = 0;
      const cJSON *te;
      cJSON_ArrayForEach(te, entry)
      {
         cJSON *range = cJSON_GetObjectItemCaseSensitive(te, "range");
         cJSON *new_text_j = cJSON_GetObjectItemCaseSensitive(te, "newText");
         if (!cJSON_IsObject(range))
            continue;
         cJSON *start = cJSON_GetObjectItemCaseSensitive(range, "start");
         cJSON *end_j = cJSON_GetObjectItemCaseSensitive(range, "end");
         if (!cJSON_IsObject(start) || !cJSON_IsObject(end_j))
            continue;
         cJSON *sl_j = cJSON_GetObjectItemCaseSensitive(start, "line");
         cJSON *sc_j = cJSON_GetObjectItemCaseSensitive(start, "character");
         cJSON *el_j = cJSON_GetObjectItemCaseSensitive(end_j, "line");
         cJSON *ec_j = cJSON_GetObjectItemCaseSensitive(end_j, "character");
         if (!cJSON_IsNumber(sl_j) || !cJSON_IsNumber(sc_j) || !cJSON_IsNumber(el_j) ||
             !cJSON_IsNumber(ec_j))
            continue;
         edits[ei].sl = (int)sl_j->valuedouble;
         edits[ei].sc = (int)sc_j->valuedouble;
         edits[ei].el = (int)el_j->valuedouble;
         edits[ei].ec = (int)ec_j->valuedouble;
         edits[ei].new_text = cJSON_IsString(new_text_j) ? new_text_j->valuestring : "";
         ei++;
      }
      nedits = ei;

      /* Sort edits by start line descending (simple insertion sort, edits are few) */
      for (int a = 1; a < nedits; a++)
      {
         edit_t tmp = edits[a];
         int b = a - 1;
         while (b >= 0 && (edits[b].sl < tmp.sl || (edits[b].sl == tmp.sl && edits[b].sc < tmp.sc)))
         {
            edits[b + 1] = edits[b];
            b--;
         }
         edits[b + 1] = tmp;
      }

      /* Apply each edit to the line array */
      for (int e = 0; e < nedits; e++)
      {
         int sl = edits[e].sl, sc = edits[e].sc;
         int el = edits[e].el, ec = edits[e].ec;
         const char *nt = edits[e].new_text;

         if (sl < 0 || sl >= nlines || el < 0 || el >= nlines)
            continue;

         /* Build the replacement: prefix of start line + newText + suffix of end line */
         const char *start_line = lines[sl];
         const char *end_line = lines[el];

         /* Clamp character offsets (work in bytes, rename symbols are ASCII) */
         int sl_len = (int)strlen(start_line);
         int el_len = (int)strlen(end_line);
         if (sc > sl_len)
            sc = sl_len;
         if (ec > el_len)
            ec = el_len;

         size_t prefix_len = (size_t)sc;
         size_t nt_len = strlen(nt);
         size_t suffix_len = (size_t)(el_len - ec);
         char *replacement = malloc(prefix_len + nt_len + suffix_len + 1);
         if (!replacement)
            continue;
         memcpy(replacement, start_line, prefix_len);
         memcpy(replacement + prefix_len, nt, nt_len);
         memcpy(replacement + prefix_len + nt_len, end_line + ec, suffix_len);
         replacement[prefix_len + nt_len + suffix_len] = '\0';

         /* Free lines sl..el and replace with the single replacement line */
         for (int li = sl; li <= el; li++)
            free(lines[li]);
         /* Shift lines[el+1..] down by (el-sl) positions */
         int removed = el - sl;
         if (removed > 0)
         {
            memmove(&lines[sl + 1], &lines[el + 1], (size_t)(nlines - el - 1) * sizeof(char *));
            nlines -= removed;
         }
         lines[sl] = replacement;
      }
      free(edits);

      /* Write the modified content back */
      f = fopen(path, "w");
      if (f)
      {
         for (int li = 0; li < nlines; li++)
            fputs(lines[li], f);
         fclose(f);
         files_changed++;

         /* Record in summary output */
         const char *basename = strrchr(path, '/');
         if (left > 2)
         {
            int n = snprintf(p, left, "%s%s", files_changed > 1 ? ", " : "",
                             basename ? basename + 1 : path);
            if (n > 0 && (size_t)n < left)
            {
               p += n;
               left -= (size_t)n;
            }
         }
      }

      for (int i = 0; i < nlines; i++)
         free(lines[i]);
      free(lines);
      continue;

   next_entry:
      continue;
   }

   return files_changed;
}

int lsp_manager_rename(const char *workspace, const char *file, int line, int col,
                       const char *new_name, char *out, size_t out_size, char *errbuf,
                       size_t errbuf_size)
{
   if (!workspace || !file || !new_name || !out || out_size == 0)
      return -1;

   out[0] = '\0';

   const char *ext = extension_of(file);

   pthread_mutex_lock(&g_lock);
   lsp_server_t *srv = get_or_spawn(workspace, ext);
   pthread_mutex_unlock(&g_lock);

   if (!srv)
   {
      if (errbuf)
         snprintf(errbuf, errbuf_size, "no LSP server configured for extension '%s'", ext);
      return -1;
   }

   char uri[LSP_PATH_MAX + 8];
   path_to_uri(file, uri, sizeof(uri));

   cJSON *params = cJSON_CreateObject();
   cJSON *td = cJSON_AddObjectToObject(params, "textDocument");
   cJSON_AddStringToObject(td, "uri", uri);
   cJSON *pos = cJSON_AddObjectToObject(params, "position");
   cJSON_AddNumberToObject(pos, "line", line);
   cJSON_AddNumberToObject(pos, "character", col);
   cJSON_AddStringToObject(params, "newName", new_name);

   cJSON *resp = send_request(srv, "textDocument/rename", params);
   if (!resp)
   {
      if (errbuf)
         snprintf(errbuf, errbuf_size, "LSP request timed out");
      return -1;
   }

   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   int n = 0;
   if (result && !cJSON_IsNull(result))
      n = apply_workspace_edit(result, out, out_size);
   else if (cJSON_GetObjectItemCaseSensitive(resp, "error"))
   {
      cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
      if (errbuf && cJSON_IsString(msg))
         snprintf(errbuf, errbuf_size, "LSP error: %s", msg->valuestring);
      else if (errbuf)
         snprintf(errbuf, errbuf_size, "LSP rename failed");
      cJSON_Delete(resp);
      return -1;
   }

   cJSON_Delete(resp);
   return n;
}

void lsp_manager_diag_summary(int *errors, int *warnings, int *active_servers)
{
   int nerrors = 0, nwarnings = 0, nactive = 0;

   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < LSP_MAX_SERVERS; i++)
   {
      lsp_server_t *srv = &g_servers[i];
      if (!srv->active)
         continue;
      nactive++;

      pthread_mutex_lock(&srv->diag_lock);
      for (int fi = 0; fi < srv->nfiles; fi++)
      {
         lsp_file_diags_t *fd = &srv->files[fi];
         for (int di = 0; di < fd->ndiags; di++)
         {
            if (fd->diags[di].severity == LSP_SEV_ERROR)
               nerrors++;
            else if (fd->diags[di].severity == LSP_SEV_WARNING)
               nwarnings++;
         }
      }
      pthread_mutex_unlock(&srv->diag_lock);
   }
   pthread_mutex_unlock(&g_lock);

   if (errors)
      *errors = nerrors;
   if (warnings)
      *warnings = nwarnings;
   if (active_servers)
      *active_servers = nactive;
}

#endif /* AIMEE_WINDOWS */

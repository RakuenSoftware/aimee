/* cli_mcp_serve.c: MCP stdio proxy -- forwards tool traffic to aimee-server
 *
 * This replaces the retired standalone MCP binary. It handles MCP protocol
 * framing locally and forwards tools/list and tools/call to aimee-server
 * over the Unix socket. If the server disconnects, it reconnects
 * transparently. */
#include "aimee_home.h"
#include "cli_client.h"
#include "cli_mcp_serve.h"
#include "client_constants.h"
#include "client_session_worktree.h"
#include "platform_path.h"
#include "platform_random.h"
#include "util.h"
#include "cJSON.h"
#include <aimee/protocols/mcp/mcp_tools.h> /* mcp_compact_tool_prose */
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MCP_LINE_MAX         65536
#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_VERSION          AIMEE_VERSION

#define DEFAULT_TIMEOUT_MS  30000
#define DELEGATE_TIMEOUT_MS 300000
/* Reconnect: exponential backoff up to RECONNECT_RETRIES attempts.
 * Total budget covers a normal aimee-server restart cycle (~30s). 3 × 500ms
 * was too tight: a server restart between two MCP calls would surface as
 * "aimee-server unavailable" to the caller and leave Claude's tool list
 * marked stale until the next call happened to land cleanly. */
#define RECONNECT_RETRIES       8
#define RECONNECT_DELAY_INIT_US 250000  /* 250ms initial */
#define RECONNECT_DELAY_MAX_US  8000000 /* 8s cap */

/* --- MCP JSON-RPC helpers ---
 *
 * The MCP stdio transport spec requires newline-delimited JSON. Some older
 * test harnesses (and the original aimee MCP implementation) used LSP-style
 * Content-Length framing. We auto-detect which framing the client is using
 * based on the first message we read (see read_mcp_message) and mirror it on
 * output. Default is NDJSON, so spec-compliant clients work even if we have
 * to emit an error before reading anything. */

static int g_output_ndjson = 1;

static void mcp_send(cJSON *msg)
{
   char *s = cJSON_PrintUnformatted(msg);
   if (s)
   {
      size_t len = strlen(s);
      if (g_output_ndjson)
      {
         fwrite(s, 1, len, stdout);
         fputc('\n', stdout);
      }
      else
      {
         fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
         fwrite(s, 1, len, stdout);
      }
      fflush(stdout);
      free(s);
   }
   cJSON_Delete(msg);
}

static void mcp_respond(cJSON *id, cJSON *result)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
   cJSON_AddItemToObject(resp, "result", result);
   mcp_send(resp);
}

static void mcp_error(cJSON *id, int code, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   if (id)
      cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
   else
      cJSON_AddNullToObject(resp, "id");
   cJSON *err = cJSON_CreateObject();
   cJSON_AddNumberToObject(err, "code", code);
   cJSON_AddStringToObject(err, "message", message);
   cJSON_AddItemToObject(resp, "error", err);
   mcp_send(resp);
}

/* --- Server connection with auto-reconnect --- */

static cli_conn_t g_conn = {.fd = -1};
static const char *g_sock_path = NULL;

static const char *client_session_id(void)
{
   static char id[64];
   static int loaded = 0;
   if (loaded)
      return id[0] ? id : NULL;
   loaded = 1;

   const char *env = getenv("AIMEE_SESSION_ID");
   if (env && env[0])
   {
      snprintf(id, sizeof(id), "%s", env);
      return id;
   }

   const char *base = aimee_home();
   if (!base)
      return NULL;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/session-ppid-%d", base, platform_getppid());
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;

   if (fgets(id, sizeof(id), fp))
   {
      size_t len = strlen(id);
      while (len > 0 && (id[len - 1] == '\n' || id[len - 1] == '\r' || id[len - 1] == ' '))
         id[--len] = '\0';
   }
   fclose(fp);
   return id[0] ? id : NULL;
}

/* Read the session id published at `path` into out[cap]. 0 on success. */
static int session_id_read_published(const char *path, char *out, size_t cap)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return -1;
   char buf[64] = "";
   if (fgets(buf, sizeof(buf), fp))
   {
      size_t len = strlen(buf);
      while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
         buf[--len] = '\0';
   }
   fclose(fp);
   if (!buf[0])
      return -1;
   snprintf(out, cap, "%s", buf);
   return 0;
}

/* How long the proxy will wait for the host's SessionStart hook to publish the
 * real session id before minting one of its own, and how often it looks.
 *
 * Only paid when the parent IS the host (see session_id_host_parent), and only
 * on the first startup of a session -- once the id is published the read hits
 * immediately. Two seconds is far longer than the hook needs to reach its
 * publish, which happens before it chooses a transport or contacts a server. */
#define SESSION_ID_HOST_WAIT_MS 2000
#define SESSION_ID_HOST_POLL_MS 25

/* Is `pid` the agent host that runs a SessionStart hook? Same signal the hook's
 * own ancestor walk uses, so the writer and the waiter agree on who to expect.
 * Linux-only: elsewhere there is no /proc to ask and the proxy mints as before,
 * which is the pre-existing behaviour rather than a regression. */
static int session_id_host_parent(int pid)
{
#if defined(__linux__)
   char path[64];
   snprintf(path, sizeof(path), "/proc/%d/comm", pid);
   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0;
   char buf[64] = "";
   if (!fgets(buf, sizeof(buf), fp))
   {
      fclose(fp);
      return 0;
   }
   fclose(fp);
   buf[strcspn(buf, "\r\n")] = '\0';
   return strcmp(buf, "claude") == 0;
#else
   (void)pid;
   return 0;
#endif
}

/* Mint this agent session's id and publish it at session-ppid-<ppid>, so every
 * process of the session (hook, this proxy, delegates) resolves the SAME id.
 * Mirrors config.c's session_id(), which the thin client does not link.
 *
 * The create is O_EXCL: when two sibling processes race, exactly one wins and
 * the loser re-reads the winner's id, so they converge instead of each keying a
 * worktree on its own invented string. ppid <= 1 means orphaned — refuse rather
 * than key on session-ppid-1, which unrelated daemons would all share.
 * Returns 0 and fills out[cap] on success, -1 when no stable id is available. */
static int client_session_id_ensure(char *out, size_t cap)
{
   if (!out || !cap)
      return -1;
   out[0] = '\0';

   /* Reads env and file directly rather than through client_session_id(), whose
    * result is memoised on first call — by the time this runs that cache may
    * already hold a "no id" answer from before the file existed. */
   const char *env = getenv("AIMEE_SESSION_ID");
   if (env && env[0])
   {
      snprintf(out, cap, "%s", env);
      return 0;
   }

   int ppid = (int)platform_getppid();
   const char *base = aimee_home();
   if (ppid <= 1 || !base)
      return -1;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/session-ppid-%d", base, ppid);

   /* Already published by a sibling (or an earlier run of this session). */
   if (session_id_read_published(path, out, cap) == 0)
      return 0;

   /* Nothing published YET is not the same as nothing coming.
    *
    * The host fires its SessionStart hook and starts this proxy at roughly the
    * same moment, and only the hook knows the host's session id. Minting the
    * instant the file is missing loses that race about as often as it wins it,
    * and the loss is expensive: the proxy keys a SECOND worktree, so the
    * session runs on two and everything behind the proxy -- delegates, `aimee
    * git` -- operates on the empty one. Measured on a repro box, running the
    * proxy before the hook produced exactly that: two worktrees, one keyed on a
    * minted id and one on the host's.
    *
    * So wait briefly for the hook, but only when there is a hook to wait for:
    * the wait is gated on the parent being the host process, which is the same
    * signal client_session_id_publish walks to. Any other MCP host -- one with
    * no aimee hook installed -- mints immediately as before and pays nothing. */
   if (session_id_host_parent(ppid))
   {
      for (int waited_ms = 0; waited_ms < SESSION_ID_HOST_WAIT_MS;
           waited_ms += SESSION_ID_HOST_POLL_MS)
      {
         usleep(SESSION_ID_HOST_POLL_MS * 1000);
         if (session_id_read_published(path, out, cap) == 0)
            return 0;
      }
   }

   unsigned char rnd[16];
   if (platform_random_bytes(rnd, sizeof(rnd)) != 0)
      return -1;
   char id[64];
   snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7], rnd[8], rnd[9], rnd[10],
            rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);

   int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
   if (fd >= 0)
   {
      ssize_t wrote = write(fd, id, strlen(id));
      close(fd);
      if (wrote == (ssize_t)strlen(id))
      {
         snprintf(out, cap, "%s", id);
         return 0;
      }
      return -1;
   }

   /* Lost the O_EXCL race: adopt the id the winner published. */
   return session_id_read_published(path, out, cap);
}

/* Ensure a co-located aimee-server is reachable over the /v1 HTTP UDS. The thin
 * client is connectionless per call now (each server_request is a one-shot /v1
 * dispatch), so this is a pure availability probe — no persistent socket. */
static int ensure_connection(void)
{
   if (!g_sock_path)
      g_sock_path = cli_ensure_server_for_method("mcp.call");
   return g_sock_path ? 0 : -1;
}

static cJSON *server_request(cJSON *req, int timeout_ms)
{
   /* Remote aimee-server: POST mcp.call to its first-class /v1 route. The local
    * cli_v1_dispatch_local path only speaks the co-located UDS, so a thin client
    * configured with a remote endpoint must route here. Memory/kb/search tools
    * resolve on the server (which proxies DB2 to aimee-kb). File/exec tools whose
    * cwd is a registered detached workspace marshal back to this client over the
    * workspace reverse-channel (Phase 2b); on a non-served cwd they fail safe
    * server-side (the path does not exist there). */
   if (cli_v1_has_remote_endpoint())
   {
      char *endpoint = cli_v1_client_endpoint();
      char *bearer = cli_v1_client_bearer();
      /* Route by the request's ACTUAL method, not a hardcoded "mcp.call": e.g.
       * mcp.tools_list -> GET /v1/mcp/tools_list, mcp.call -> POST /v1/mcp/call.
       * Hardcoding mcp.call sent tools/list to /v1/mcp/call (which needs a `tool`
       * arg) and failed with "Failed to list tools". */
      cJSON *jmethod = cJSON_GetObjectItemCaseSensitive(req, "method");
      const char *method =
          (cJSON_IsString(jmethod) && jmethod->valuestring[0]) ? jmethod->valuestring : "mcp.call";
      const char *verb = NULL;
      const char *path = cli_v1_route_for_method(method, &verb);
      cJSON *resp = NULL;
      if (endpoint && path)
      {
         char *body = cJSON_PrintUnformatted(req);
         if (body)
         {
            /* Catalog/discovery GETs are safe to retry when a TLS accept worker
             * is momentarily saturated.  A burst of fresh MCP bridges used to
             * fail tools/list at roughly the server's 64-connection ceiling,
             * even though the listener recovered milliseconds later.  Never
             * retry POST tool calls here: a lost response must not duplicate a
             * mutation whose first attempt may have completed. */
            const char *http_verb = verb ? verb : "POST";
            int attempts = strcmp(http_verb, "GET") == 0 ? RECONNECT_RETRIES : 1;
            useconds_t delay = RECONNECT_DELAY_INIT_US;
            for (int attempt = 0; attempt < attempts; attempt++)
            {
               int status = 0;
               resp =
                   cli_http_request(endpoint, http_verb, path, body, bearer, timeout_ms, &status);
               if (resp)
                  break;
               if (attempt < attempts - 1)
               {
                  usleep(delay);
                  delay = delay * 2 < RECONNECT_DELAY_MAX_US ? delay * 2 : RECONNECT_DELAY_MAX_US;
               }
            }
            free(body);
         }
      }
      free(endpoint);
      free(bearer);
      return resp;
   }

   useconds_t delay = RECONNECT_DELAY_INIT_US;
   for (int retry = 0; retry < RECONNECT_RETRIES; retry++)
   {
      if (ensure_connection() != 0)
      {
         if (retry < RECONNECT_RETRIES - 1)
         {
            usleep(delay);
            delay = delay * 2 < RECONNECT_DELAY_MAX_US ? delay * 2 : RECONNECT_DELAY_MAX_US;
         }
         continue;
      }

      /* Each call is a one-shot /v1 dispatch to the co-located server (mcp.call
       * is reached over the local UDS, which permits the full dispatch surface). */
      cJSON *resp = cli_v1_dispatch_local(req, timeout_ms);
      if (resp)
         return resp;

      /* Server unreachable -- re-discover it on the next attempt. */
      g_sock_path = NULL;
      if (retry < RECONNECT_RETRIES - 1)
      {
         usleep(delay);
         delay = delay * 2 < RECONNECT_DELAY_MAX_US ? delay * 2 : RECONNECT_DELAY_MAX_US;
      }
   }
   return NULL;
}

static cJSON *forward_to_server(const char *tool, cJSON *args, int timeout_ms)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.call");
   cJSON_AddStringToObject(req, "tool", tool);
   const char *sid = client_session_id();
   if (sid)
      cJSON_AddStringToObject(req, "session_id", sid);
   char cwd[MAX_PATH_LEN] = "";
   if (getcwd(cwd, sizeof(cwd)) && cwd[0])
      cJSON_AddStringToObject(req, "cwd", cwd);

   cJSON *arguments = args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject();
   if (!arguments)
      arguments = cJSON_CreateObject();
   /* cwd is transport identity, not a value the model must invent. This makes
    * ordinary no-override memory and code-intelligence calls local-first even
    * when the long-running server has a different process cwd. The transport
    * value is authoritative: a model-supplied cwd must not retarget an ordered
    * read to another checkout. Explicit project/workspace selectors remain
    * untouched. */
   if (cwd[0] && cJSON_IsObject(arguments))
   {
      cJSON_DeleteItemFromObjectCaseSensitive(arguments, "cwd");
      cJSON_AddStringToObject(arguments, "cwd", cwd);
   }
   cJSON_AddItemToObject(req, "arguments", arguments);

   cJSON *resp = server_request(req, timeout_ms);
   cJSON_Delete(req);
   return resp;
}

static int respond_json_resource(cJSON *id, const char *uri, cJSON *payload)
{
   char *text = cJSON_PrintUnformatted(payload);
   if (!text)
   {
      mcp_error(id, -32603, "Failed to serialize resource");
      return -1;
   }

   cJSON *contents = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "uri", uri);
   cJSON_AddStringToObject(item, "mimeType", "application/json");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(contents, item);

   cJSON *result = cJSON_CreateObject();
   cJSON_AddItemToObject(result, "contents", contents);
   mcp_respond(id, result);
   free(text);
   return 0;
}

static void add_prompt_message(cJSON *messages, const char *role, const char *text)
{
   cJSON *message = cJSON_CreateObject();
   cJSON *content = cJSON_CreateObject();
   cJSON_AddStringToObject(message, "role", role);
   cJSON_AddStringToObject(content, "type", "text");
   cJSON_AddStringToObject(content, "text", text);
   cJSON_AddItemToObject(message, "content", content);
   cJSON_AddItemToArray(messages, message);
}

/* --- Local protocol handlers --- */

/* Place this MCP session on its own branch + worktree, and ENTER it.
 *
 * An MCP-hosted agent has no SessionStart hook, so nothing else would isolate
 * it: it would drive aimee's file/exec tools straight against the shared
 * checkout. Unlike a hook — which cannot chdir its host — this proxy IS the
 * process those tools resolve their paths in, so it can complete the handoff
 * itself by chdir'ing into the worktree.
 *
 * Runs once, at `initialize`, before any tool traffic. Writes the entered path
 * into out[cap] and returns 1; returns 0 when no worktree was entered (isolation
 * disabled, already inside one, not a git repo, or creation failed — in which
 * case client_session_worktree_ensure has already explained itself on stderr,
 * which the MCP host surfaces as server log output). Never fatal: a session that
 * cannot be isolated still serves read-only tools, and the attention guard
 * remains the backstop that refuses its writes. */
static int mcp_enter_session_worktree(char *out, size_t cap)
{
   const char *sid = client_session_id();
   char fallback[64];
   if (!sid || !sid[0])
   {
      /* No host-provided id (no AIMEE_SESSION_ID, no session-ppid file yet).
       * Mint one the way config.c does and PERSIST it to session-ppid-<ppid>,
       * rather than inventing a private "mcp-ppid-N" string.
       *
       * Both halves matter. Processes of one agent session share a PPID, and
       * that file is how the hook, this proxy and the delegates agree on one
       * session id — an id invented here would be seen by nobody else, so the
       * proxy would work in a different worktree from its own session. And a
       * bare ppid is not unique enough to key a worktree on: two proxies under
       * one host process would derive the same key and land in the SAME
       * worktree, overwriting each other. The file's O_EXCL create settles that
       * race — whoever loses re-reads the winner's id. */
      if (client_session_id_ensure(fallback, sizeof(fallback)) != 0 || !fallback[0])
         return 0; /* no stable identity -> better unisolated than colliding */
      sid = fallback;
   }

   if (client_session_worktree_ensure(sid, out, cap) != 0)
      return 0;
   if (chdir(out) != 0)
   {
      fprintf(stderr, "aimee: prepared session worktree %s but could not enter it\n", out);
      return 0;
   }
   fprintf(stderr, "aimee: MCP session isolated in %s\n", out);
   return 1;
}

/* A remote MCP bridge starts in the developer's canonical checkout, then enters
 * a hidden per-session worktree for isolation. If the first code-index request
 * is forwarded only after that chdir, the remote server sees a hidden
 * `.aimee/worktrees/.../main` cwd while its KB has no canonical project to map
 * it to. Bootstrap the canonical repository before isolation. The remote helper
 * first asks index.list and uploads only when the project/root is absent, so an
 * ordinary initialize is one cheap read rather than a full rescan. */
static void mcp_ensure_remote_repo_index(void)
{
   if (!cli_v1_remote_endpoint_is_network())
      return;

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)) || !cwd[0])
      return;
   const char *const argv[] = {"git", "-C", cwd, "rev-parse", "--show-toplevel", NULL};
   char *root = NULL;
   if (safe_exec_capture(argv, &root, MAX_PATH_LEN) != 0 || !root)
   {
      free(root);
      return; /* not a repository: there is no project to index */
   }
   size_t len = strlen(root);
   while (len > 0 && (root[len - 1] == '\n' || root[len - 1] == '\r' || root[len - 1] == ' ' ||
                      root[len - 1] == '\t'))
      root[--len] = '\0';
   if (root[0] && cli_index_ensure_remote(root) != 0)
      fprintf(stderr, "aimee: warning: could not bootstrap the remote code index for %s\n", root);
   free(root);
}

static void handle_initialize(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);

   cJSON *caps = cJSON_CreateObject();
   cJSON *tools_cap = cJSON_CreateObject();
   /* The presented list is dynamic (presentation profile + plugin/remote tools +
    * runtime config), so advertise listChanged: a client may re-list to pick up
    * changes. */
   cJSON_AddBoolToObject(tools_cap, "listChanged", 1);
   cJSON_AddItemToObject(caps, "tools", tools_cap);
   cJSON *res_cap = cJSON_CreateObject();
   cJSON_AddBoolToObject(res_cap, "subscribe", 0);
   cJSON_AddBoolToObject(res_cap, "listChanged", 0);
   cJSON_AddItemToObject(caps, "resources", res_cap);
   cJSON *prompts_cap = cJSON_CreateObject();
   cJSON_AddBoolToObject(prompts_cap, "listChanged", 0);
   cJSON_AddItemToObject(caps, "prompts", prompts_cap);
   cJSON_AddItemToObject(result, "capabilities", caps);

   cJSON *info = cJSON_CreateObject();
   cJSON_AddStringToObject(info, "name", "aimee");
   cJSON_AddStringToObject(info, "version", MCP_VERSION);
   cJSON_AddItemToObject(result, "serverInfo", info);

   /* EVERY TOOL IN tools/list IS DIRECTLY CALLABLE. SAY SO FIRST.
    *
    * This text used to open with "call get_help() before trying anything else"
    * and then describe find_tools -> describe_tool -> call_tool as the way to
    * reach tools. Agents read that as the normal path and spent their tool budget
    * on the protocol rather than the work. Measured twice now: once at five of
    * fourteen calls, and again on a benchmark cell that made two find_tools, two
    * describe_tool and two call_tool calls and NOT ONE direct find_symbol -- while
    * find_symbol was in the advertised list the whole time, one call away.
    *
    * Discovery is for what is NOT in the list. Leading with it taxes every session
    * to buy something almost none of them need. */
   /* AND SAY WHICH SURFACE IS CHEAPER, because this text is the reason agents
    * pick the expensive one.
    *
    * It used to say the listed tools were the "first move on repository
    * questions", full stop. That is the ONE instruction guaranteed to reach every
    * MCP session -- it rides the initialize handshake, before any work, whether or
    * not the agent calls a tool we happen to hook. Measured result: 13.3 MCP tool
    * calls per benchmark cell and ZERO `aimee ...` invocations across 13 cells,
    * with the binary on PATH throughout.
    *
    * An MCP call is one call, one turn, and the whole conversation re-sent. The
    * same lookups as commands join with && into a shell call the agent is already
    * making. The tools are not worse; the SURFACE is, whenever more than one
    * lookup is wanted. So the cheaper form leads and the tools stay named for the
    * cases that have no command form.
    *
    * Hooking memory_recall(session_start) instead was tried first and did not
    * work: that tool is optional, the agent simply never called it, and the
    * guidance never arrived. Only the handshake is unskippable. */
   static const char *const base_instructions =
       "The tools in tools/list are directly callable — call them directly, by "
       "name, with their arguments. Do not route a listed tool through call_tool, "
       "and do not look one up before using it. "
       "IF YOU CAN RUN A SHELL COMMAND, USE THE COMMAND FORM, NOT THE TOOL. "
       "`aimee index find <symbol>`, `aimee index callers <symbol>`, `aimee index "
       "blast-radius <file>`, `aimee index structure <file>`, `aimee index span "
       "<file> <start> <end>`, `aimee index investigate \"<question>\"`, `aimee "
       "index deps <file>`, `aimee index hybrid \"<phrase>\"` and `aimee memory "
       "search <terms>` answer the same questions as the "
       "listed tools. They are ordinary commands, so they join with && inside a "
       "shell call you are already making — one round trip for as many lookups as "
       "you want. Every tool call is instead its own turn and re-sends the whole "
       "conversation, so N tool calls cost N times what the same N commands cost. "
       "This holds for a single lookup too: fold it into the command you were "
       "already going to run. "
       "Use the listed tools when there is no command form — ast_grep_search — or when "
       "you genuinely cannot run a shell command. Tool calls cannot be chained, so "
       "when you do call one, use its plural argument (spans, queries, symbols, "
       "identifiers) instead of repeating the call. "
       "Only when you need a tool that is NOT listed: find_tools("
       "\"<keyword>\") to locate it, describe_tool(\"<name>\") for its schema, "
       "then call_tool with that name and matching arguments. get_help(\"<topic>\") "
       "explains how aimee itself works — work queue, delegation, memory, git, "
       "build, conventions — when you are unsure; it is not a required first step. "
       "Do not use provider-native sub-agent tools such as spawn_agent or Agent; "
       "use the aimee delegate tool for delegated work.";

   /* The canonical checkout must be known to the remote index before isolation
    * moves this process underneath its hidden .aimee/worktrees directory. */
   mcp_ensure_remote_repo_index();

   /* Isolate before serving any tool call, and tell the caller where its work
    * will land — the host's own idea of the cwd is now stale for aimee's tools. */
   char wt[4200];
   if (mcp_enter_session_worktree(wt, sizeof(wt)))
   {
      char instructions[8192];
      snprintf(instructions, sizeof(instructions),
               "%s\n\nThis session has its own isolated checkout — a branch cut from the "
               "repository's default branch, in a dedicated worktree at %s. aimee's file and "
               "shell tools already run there; use RELATIVE paths, or absolute paths under that "
               "root. Do not edit the shared checkout.",
               base_instructions, wt);
      cJSON_AddStringToObject(result, "instructions", instructions);
   }
   else
      cJSON_AddStringToObject(result, "instructions", base_instructions);

   mcp_respond(id, result);
}

static void handle_tools_list(cJSON *id)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.tools_list");

   cJSON *resp = server_request(req, DEFAULT_TIMEOUT_MS);
   cJSON_Delete(req);
   if (!resp)
   {
      mcp_error(id, -32000,
                "aimee-server unavailable after retries. Restart aimee-server and retry.");
      return;
   }

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tools");
   if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 || !cJSON_IsArray(tools))
   {
      /* Preserve an actionable /v1 error (notably authz failures) instead of
       * collapsing every non-list response to an opaque startup error. */
      const char *detail = NULL;
      cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
      cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
      if (cJSON_IsString(message))
         detail = message->valuestring;
      else if (cJSON_IsObject(error))
      {
         message = cJSON_GetObjectItemCaseSensitive(error, "message");
         if (cJSON_IsString(message))
            detail = message->valuestring;
      }

      char errmsg[640];
      snprintf(errmsg, sizeof(errmsg), "Failed to list tools%s%s", detail ? ": " : "",
               detail ? detail : "");
      cJSON_Delete(resp);
      mcp_error(id, -32603, errmsg);
      return;
   }

   /* Trim guidance prose on the way out, when asked. The server applies the same
    * function at its own choke point, but this is the one that decides what THIS
    * consumer's context carries, and a thin client can face a server it does not
    * control. Hides no tool and alters no callable shape; see mcp_compact_tool_prose.
    * Off unless AIMEE_MCP_TOOL_PROSE=lean, so the default payload is unchanged. */
   if (mcp_tool_prose_lean())
      (void)mcp_compact_tool_prose(tools);

   cJSON *result = cJSON_CreateObject();
   cJSON_AddItemToObject(result, "tools", cJSON_DetachItemFromObjectCaseSensitive(resp, "tools"));
   mcp_respond(id, result);
   cJSON_Delete(resp);
}

static void handle_tools_call(cJSON *id, cJSON *req)
{
   cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
   if (!params)
   {
      mcp_error(id, -32602, "Missing params");
      return;
   }
   cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
   cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

   if (!cJSON_IsString(name))
   {
      mcp_error(id, -32602, "Missing tool name");
      return;
   }

   const char *tool = name->valuestring;

   /* A remote bridge only needs the detached-workspace long poll once an
    * actual tool may touch its working tree. Starting it at process startup
    * consumed one TLS worker per short-lived discovery client; the abandoned
    * polls then filled the 64-worker cap and made an unrelated tools/list fail.
    * The helper is idempotent, so subsequent tool calls are cheap. */
   cli_workspace_reverse_channel_start();

   /* A remote Git tool runs in the server's reconstructed mirror. Refresh its
    * immutable client snapshot on every call: unchanged trees reuse the same
    * generation (preserving server-side commits/index state), while local
    * commits or edits select a fresh worktree. Fail closed if the refresh cannot
    * be acknowledged; forwarding would knowingly act on stale source. */
   if (strcmp(tool, "git") == 0 && cli_workspace_reverse_channel_sync() != 0)
   {
      cJSON *result = cJSON_CreateObject();
      cJSON *content = cJSON_AddArrayToObject(result, "content");
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      cJSON_AddStringToObject(
          block, "text",
          "could not refresh the remote workspace mirror; refusing to run Git against a stale "
          "checkout. Verify the Aimee server connection and retry.");
      cJSON_AddItemToArray(content, block);
      cJSON_AddBoolToObject(result, "isError", 1);
      mcp_respond(id, result);
      return;
   }

   int timeout = DEFAULT_TIMEOUT_MS;
   if (strcmp(tool, "delegate") == 0)
      timeout = DELEGATE_TIMEOUT_MS;

   cJSON *resp = forward_to_server(tool, args, timeout);
   if (!resp)
   {
      mcp_error(id, -32000,
                "aimee-server unavailable after retries. Restart aimee-server and retry.");
      return;
   }

   /* Check server response status */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if ((cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0) ||
       cJSON_IsObject(error))
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (!cJSON_IsString(msg) && cJSON_IsObject(error))
         msg = cJSON_GetObjectItemCaseSensitive(error, "message");
      const char *errmsg = (msg && cJSON_IsString(msg)) ? msg->valuestring : "server error";
      /* Server-side delegation handlers already attach actionable Fix
       * guidance for known error patterns (see delegation_error_guidance),
       * so we forward the message as-is. */
      /* Return as tool result with isError flag, not protocol error */
      cJSON *result = cJSON_CreateObject();
      cJSON *content = cJSON_CreateArray();
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      cJSON_AddStringToObject(block, "text", errmsg);
      cJSON_AddItemToArray(content, block);
      cJSON_AddItemToObject(result, "content", content);
      cJSON_AddBoolToObject(result, "isError", 1);
      mcp_respond(id, result);
      cJSON_Delete(resp);
      return;
   }

   /* Server returns {"status":"ok", "content":[...]} for synchronous tools.
    * For delegate (async), the server responds with the delegation result
    * which also has "status":"ok" and we wrap the response text. */
   cJSON *content = cJSON_DetachItemFromObjectCaseSensitive(resp, "content");
   if (!content)
   {
      /* Wrap the entire server response as text for non-standard formats */
      cJSON *response_text = cJSON_GetObjectItemCaseSensitive(resp, "response");
      content = cJSON_CreateArray();
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      if (response_text && cJSON_IsString(response_text))
         cJSON_AddStringToObject(block, "text", response_text->valuestring);
      else
      {
         char *raw = cJSON_PrintUnformatted(resp);
         cJSON_AddStringToObject(block, "text", raw ? raw : "{}");
         free(raw);
      }
      cJSON_AddItemToArray(content, block);
   }

   cJSON *result = cJSON_CreateObject();
   cJSON_AddItemToObject(result, "content", content);
   cJSON *structured = cJSON_DetachItemFromObjectCaseSensitive(resp, "structuredContent");
   if (structured)
      cJSON_AddItemToObject(result, "structuredContent", structured);
   mcp_respond(id, result);
   cJSON_Delete(resp);
}

static void handle_resources_list(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON *resources = cJSON_CreateArray();

   const char *tiers[] = {"L0", "L1", "L2", "L3"};
   for (int i = 0; i < 4; i++)
   {
      cJSON *r = cJSON_CreateObject();
      char uri[64], rname[64];
      snprintf(uri, sizeof(uri), "aimee://memories/%s", tiers[i]);
      snprintf(rname, sizeof(rname), "Memories (%s)", tiers[i]);
      cJSON_AddStringToObject(r, "uri", uri);
      cJSON_AddStringToObject(r, "name", rname);
      cJSON_AddStringToObject(r, "description",
                              "Tier-filtered memory entries for the selected aimee memory tier.");
      cJSON_AddStringToObject(r, "mimeType", "application/json");
      cJSON_AddItemToArray(resources, r);
   }

   cJSON *facts = cJSON_CreateObject();
   cJSON_AddStringToObject(facts, "uri", "aimee://facts");
   cJSON_AddStringToObject(facts, "name", "All stored facts");
   cJSON_AddStringToObject(facts, "description", "All long-term fact memories (L2/fact).");
   cJSON_AddStringToObject(facts, "mimeType", "application/json");
   cJSON_AddItemToArray(resources, facts);

   cJSON *cfg_res = cJSON_CreateObject();
   cJSON_AddStringToObject(cfg_res, "uri", "aimee://config");
   cJSON_AddStringToObject(cfg_res, "name", "Current configuration");
   cJSON_AddStringToObject(cfg_res, "description",
                           "Basic MCP proxy configuration and session metadata.");
   cJSON_AddStringToObject(cfg_res, "mimeType", "application/json");
   cJSON_AddItemToArray(resources, cfg_res);

   cJSON_AddItemToObject(result, "resources", resources);
   mcp_respond(id, result);
}

static void handle_resources_read(cJSON *id, cJSON *req)
{
   cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
   if (!params)
   {
      mcp_error(id, -32602, "Missing params");
      return;
   }
   cJSON *juri = cJSON_GetObjectItemCaseSensitive(params, "uri");
   if (!cJSON_IsString(juri))
   {
      mcp_error(id, -32602, "Missing uri parameter");
      return;
   }

   const char *uri = juri->valuestring;
   if (strcmp(uri, "aimee://config") == 0)
   {
      cJSON *cfg = cJSON_CreateObject();
      cJSON_AddStringToObject(cfg, "name", "aimee");
      cJSON_AddStringToObject(cfg, "version", AIMEE_VERSION);
      cJSON_AddStringToObject(cfg, "mcpVersion", MCP_VERSION);
      cJSON_AddStringToObject(cfg, "protocolVersion", MCP_PROTOCOL_VERSION);
      const char *sid = client_session_id();
      if (sid)
         cJSON_AddStringToObject(cfg, "sessionId", sid);
      respond_json_resource(id, uri, cfg);
      cJSON_Delete(cfg);
      return;
   }

   if (strncmp(uri, "aimee://memories/", 17) == 0)
   {
      const char *tier = uri + 17;
      if (strcmp(tier, "L0") != 0 && strcmp(tier, "L1") != 0 && strcmp(tier, "L2") != 0 &&
          strcmp(tier, "L3") != 0)
      {
         mcp_error(id, -32002, "Resource not found");
         return;
      }

      cJSON *sreq = cJSON_CreateObject();
      cJSON_AddStringToObject(sreq, "method", "memory.list");
      cJSON_AddStringToObject(sreq, "tier", tier);
      cJSON_AddNumberToObject(sreq, "limit", 50);

      cJSON *resp = server_request(sreq, DEFAULT_TIMEOUT_MS);
      cJSON_Delete(sreq);
      if (!resp)
      {
         mcp_error(id, -32603, "aimee server unavailable");
         return;
      }

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      cJSON *memories = cJSON_GetObjectItemCaseSensitive(resp, "memories");
      if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 ||
          !cJSON_IsArray(memories))
      {
         cJSON_Delete(resp);
         mcp_error(id, -32603, "Failed to load resource");
         return;
      }

      cJSON *payload = cJSON_Duplicate(memories, 1);
      cJSON_Delete(resp);
      if (!payload)
      {
         mcp_error(id, -32603, "Failed to load resource");
         return;
      }

      respond_json_resource(id, uri, payload);
      cJSON_Delete(payload);
      return;
   }

   if (strcmp(uri, "aimee://facts") == 0)
   {
      cJSON *sreq = cJSON_CreateObject();
      cJSON_AddStringToObject(sreq, "method", "memory.list");
      cJSON_AddStringToObject(sreq, "tier", "L2");
      cJSON_AddStringToObject(sreq, "kind", "fact");
      cJSON_AddNumberToObject(sreq, "limit", 100);

      cJSON *resp = server_request(sreq, DEFAULT_TIMEOUT_MS);
      cJSON_Delete(sreq);
      if (!resp)
      {
         mcp_error(id, -32603, "aimee server unavailable");
         return;
      }

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      cJSON *memories = cJSON_GetObjectItemCaseSensitive(resp, "memories");
      if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 ||
          !cJSON_IsArray(memories))
      {
         cJSON_Delete(resp);
         mcp_error(id, -32603, "Failed to load resource");
         return;
      }

      cJSON *payload = cJSON_Duplicate(memories, 1);
      cJSON_Delete(resp);
      if (!payload)
      {
         mcp_error(id, -32603, "Failed to load resource");
         return;
      }

      respond_json_resource(id, uri, payload);
      cJSON_Delete(payload);
      return;
   }

   if (strncmp(uri, "aimee://memory/", 15) == 0)
   {
      const char *id_text = uri + 15;
      char *end = NULL;
      long long memory_id = strtoll(id_text, &end, 10);
      if (!end || *end != '\0' || memory_id <= 0)
      {
         mcp_error(id, -32002, "Resource not found");
         return;
      }

      cJSON *sreq = cJSON_CreateObject();
      cJSON_AddStringToObject(sreq, "method", "memory.get");
      cJSON_AddNumberToObject(sreq, "id", (double)memory_id);

      cJSON *resp = server_request(sreq, DEFAULT_TIMEOUT_MS);
      cJSON_Delete(sreq);
      if (!resp)
      {
         mcp_error(id, -32603, "aimee server unavailable");
         return;
      }

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      if (!cJSON_IsString(status))
      {
         cJSON_Delete(resp);
         mcp_error(id, -32603, "Failed to load resource");
         return;
      }
      if (strcmp(status->valuestring, "ok") != 0)
      {
         cJSON_Delete(resp);
         mcp_error(id, -32002, "Resource not found");
         return;
      }

      cJSON *payload = cJSON_Duplicate(resp, 1);
      cJSON_Delete(resp);
      if (!payload)
      {
         mcp_error(id, -32603, "Failed to load resource");
         return;
      }
      cJSON_DeleteItemFromObjectCaseSensitive(payload, "status");
      respond_json_resource(id, uri, payload);
      cJSON_Delete(payload);
      return;
   }

   mcp_error(id, -32002, "Resource not found");
}

static void handle_resources_templates_list(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON *templates = cJSON_CreateArray();

   cJSON *tier_template = cJSON_CreateObject();
   cJSON_AddStringToObject(tier_template, "uriTemplate", "aimee://memories/{tier}");
   cJSON_AddStringToObject(tier_template, "name", "Memories by Tier");
   cJSON_AddStringToObject(tier_template, "description",
                           "Read aimee memories for a specific tier (L0-L3).");
   cJSON_AddStringToObject(tier_template, "mimeType", "application/json");
   cJSON_AddItemToArray(templates, tier_template);

   cJSON *memory_template = cJSON_CreateObject();
   cJSON_AddStringToObject(memory_template, "uriTemplate", "aimee://memory/{id}");
   cJSON_AddStringToObject(memory_template, "name", "Memory by ID");
   cJSON_AddStringToObject(memory_template, "description",
                           "Read a single aimee memory record by numeric ID.");
   cJSON_AddStringToObject(memory_template, "mimeType", "application/json");
   cJSON_AddItemToArray(templates, memory_template);

   cJSON_AddItemToObject(result, "resourceTemplates", templates);
   mcp_respond(id, result);
}

static void handle_prompts_list(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON *prompts = cJSON_CreateArray();

   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", "search-and-summarize");
      cJSON_AddStringToObject(p, "description", "Search memories and summarize results");
      cJSON *a = cJSON_CreateArray();
      cJSON *a1 = cJSON_CreateObject();
      cJSON_AddStringToObject(a1, "name", "query");
      cJSON_AddStringToObject(a1, "description", "Search terms");
      cJSON_AddBoolToObject(a1, "required", 1);
      cJSON_AddItemToArray(a, a1);
      cJSON_AddItemToObject(p, "arguments", a);
      cJSON_AddItemToArray(prompts, p);
   }

   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", "delegate-task");
      cJSON_AddStringToObject(p, "description", "Delegate a task through aimee");
      cJSON *a = cJSON_CreateArray();
      cJSON *a1 = cJSON_CreateObject();
      cJSON_AddStringToObject(a1, "name", "role");
      cJSON_AddStringToObject(a1, "description", "Agent role (execute, review, etc.)");
      cJSON_AddBoolToObject(a1, "required", 1);
      cJSON_AddItemToArray(a, a1);
      cJSON *a2 = cJSON_CreateObject();
      cJSON_AddStringToObject(a2, "name", "prompt");
      cJSON_AddStringToObject(a2, "description", "Task prompt for the delegate");
      cJSON_AddBoolToObject(a2, "required", 1);
      cJSON_AddItemToArray(a, a2);
      cJSON_AddItemToObject(p, "arguments", a);
      cJSON_AddItemToArray(prompts, p);
   }

   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", "diagnose-issue");
      cJSON_AddStringToObject(p, "description", "Run a diagnostic workflow");
      cJSON *a = cJSON_CreateArray();
      cJSON *a1 = cJSON_CreateObject();
      cJSON_AddStringToObject(a1, "name", "description");
      cJSON_AddStringToObject(a1, "description", "Issue description");
      cJSON_AddBoolToObject(a1, "required", 1);
      cJSON_AddItemToArray(a, a1);
      cJSON_AddItemToObject(p, "arguments", a);
      cJSON_AddItemToArray(prompts, p);
   }

   cJSON_AddItemToObject(result, "prompts", prompts);
   mcp_respond(id, result);
}

static void handle_prompts_get(cJSON *id, cJSON *req)
{
   cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
   if (!params)
   {
      mcp_error(id, -32602, "Missing params");
      return;
   }

   cJSON *jname = cJSON_GetObjectItemCaseSensitive(params, "name");
   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(params, "arguments");
   cJSON *orig_args = jargs;
   if (!cJSON_IsString(jname))
   {
      mcp_error(id, -32602, "Missing prompt name");
      return;
   }

   if (!jargs)
      jargs = cJSON_CreateObject();

   cJSON *result = cJSON_CreateObject();
   cJSON *messages = cJSON_CreateArray();
   char buf[4096];

   if (strcmp(jname->valuestring, "search-and-summarize") == 0)
   {
      cJSON *jq = cJSON_GetObjectItemCaseSensitive(jargs, "query");
      if (!cJSON_IsString(jq) || !jq->valuestring[0])
      {
         if (jargs != orig_args)
            cJSON_Delete(jargs);
         cJSON_Delete(result);
         cJSON_Delete(messages);
         mcp_error(id, -32602, "Missing required argument: query");
         return;
      }

      cJSON_AddStringToObject(result, "description", "Search aimee memories and summarize them.");
      snprintf(buf, sizeof(buf),
               "Use the aimee MCP tool `search_memory` with the query \"%s\". "
               "Summarize the relevant facts you find, call out uncertainty, and keep citations "
               "grounded in the returned memory content.",
               jq->valuestring);
      add_prompt_message(messages, "user", buf);
   }
   else if (strcmp(jname->valuestring, "delegate-task") == 0)
   {
      cJSON *jr = cJSON_GetObjectItemCaseSensitive(jargs, "role");
      cJSON *jp = cJSON_GetObjectItemCaseSensitive(jargs, "prompt");
      int missing_role = !cJSON_IsString(jr) || !jr->valuestring[0];
      int missing_prompt = !cJSON_IsString(jp) || !jp->valuestring[0];
      if (missing_role || missing_prompt)
      {
         if (jargs != orig_args)
            cJSON_Delete(jargs);
         cJSON_Delete(result);
         cJSON_Delete(messages);
         mcp_error(id, -32602, "Missing required arguments: role, prompt");
         return;
      }

      cJSON_AddStringToObject(result, "description", "Delegate a task through aimee.");
      snprintf(buf, sizeof(buf),
               "Use the aimee MCP tool `delegate` with role \"%s\" and prompt:\n\n%s\n\n"
               "Do not use provider-native sub-agent tools such as spawn_agent or Agent.",
               jr->valuestring, jp->valuestring);
      add_prompt_message(messages, "user", buf);
   }
   else if (strcmp(jname->valuestring, "diagnose-issue") == 0)
   {
      cJSON *jd = cJSON_GetObjectItemCaseSensitive(jargs, "description");
      if (!cJSON_IsString(jd) || !jd->valuestring[0])
      {
         if (jargs != orig_args)
            cJSON_Delete(jargs);
         cJSON_Delete(result);
         cJSON_Delete(messages);
         mcp_error(id, -32602, "Missing required argument: description");
         return;
      }

      cJSON_AddStringToObject(result, "description", "Investigate an issue with aimee.");
      snprintf(buf, sizeof(buf),
               "Diagnose the following issue with aimee:\n\n%s\n\n"
               "Start with `get_help()` if you need workflow guidance. Gather evidence first, "
               "identify the smallest credible root cause, then propose or apply a fix.",
               jd->valuestring);
      add_prompt_message(messages, "user", buf);
   }
   else
   {
      if (jargs != orig_args)
         cJSON_Delete(jargs);
      cJSON_Delete(result);
      cJSON_Delete(messages);
      mcp_error(id, -32602, "Invalid prompt name");
      return;
   }

   cJSON_AddItemToObject(result, "messages", messages);
   mcp_respond(id, result);
   if (jargs != orig_args)
      cJSON_Delete(jargs);
}

/* --- Request dispatch --- */

static void handle_request(cJSON *req)
{
   cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");

   if (!cJSON_IsString(method))
   {
      if (id)
         mcp_error(id, -32600, "Invalid request: missing method");
      return;
   }

   const char *m = method->valuestring;

   /* Notifications (no id) -- silently accept */
   if (!id)
   {
      if (strncmp(m, "notifications/", 14) == 0)
         return;
      return;
   }

   if (strcmp(m, "initialize") == 0)
      handle_initialize(id);
   else if (strcmp(m, "tools/list") == 0)
      handle_tools_list(id);
   else if (strcmp(m, "tools/call") == 0)
      handle_tools_call(id, req);
   else if (strcmp(m, "resources/list") == 0)
      handle_resources_list(id);
   else if (strcmp(m, "resources/templates/list") == 0)
      handle_resources_templates_list(id);
   else if (strcmp(m, "resources/read") == 0)
      handle_resources_read(id, req);
   else if (strcmp(m, "prompts/list") == 0)
      handle_prompts_list(id);
   else if (strcmp(m, "prompts/get") == 0)
      handle_prompts_get(id, req);
   else
      mcp_error(id, -32601, "Method not found");
}

static int read_json_line(FILE *in, int first_char, char **out)
{
   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
      return -1;

   buf[len++] = (char)first_char;
   while (1)
   {
      int ch = fgetc(in);
      if (ch == EOF)
         break;
      if (ch == '\n')
         break;
      if (ch == '\r')
      {
         int next = fgetc(in);
         if (next != '\n' && next != EOF)
            ungetc(next, in);
         break;
      }
      if (len + 1 >= cap)
      {
         size_t new_cap = cap * 2;
         char *tmp = realloc(buf, new_cap);
         if (!tmp)
         {
            free(buf);
            return -1;
         }
         buf = tmp;
         cap = new_cap;
      }
      buf[len++] = (char)ch;
   }

   buf[len] = '\0';
   *out = buf;
   return 1;
}

static int read_framed_json(FILE *in, int first_char, char **out)
{
   size_t content_length = 0;
   int saw_content_length = 0;
   char header[MCP_LINE_MAX];

   if (first_char != EOF)
      ungetc(first_char, in);

   while (fgets(header, sizeof(header), in))
   {
      if (strcmp(header, "\n") == 0 || strcmp(header, "\r\n") == 0)
      {
         if (!saw_content_length)
            return -1;
         break;
      }

      if (strncasecmp(header, "Content-Length:", 15) == 0)
      {
         char *p = header + 15;
         while (*p && isspace((unsigned char)*p))
            p++;
         char *end = NULL;
         unsigned long n = strtoul(p, &end, 10);
         if (!end || n == 0)
            return -1;
         content_length = (size_t)n;
         saw_content_length = 1;
      }
   }

   if (!saw_content_length)
      return 0;

   char *buf = malloc(content_length + 1);
   if (!buf)
      return -1;

   size_t got = fread(buf, 1, content_length, in);
   if (got != content_length)
   {
      free(buf);
      return -1;
   }
   buf[content_length] = '\0';
   *out = buf;
   return 1;
}

static int read_mcp_message(FILE *in, char **out)
{
   int ch;

   do
   {
      ch = fgetc(in);
      if (ch == EOF)
         return 0;
   } while (ch == '\n' || ch == '\r');

   if (ch == '{' || ch == '[')
   {
      g_output_ndjson = 1;
      return read_json_line(in, ch, out);
   }

   g_output_ndjson = 0;
   return read_framed_json(in, ch, out);
}

/* --- Entry point --- */

int cli_mcp_serve(void)
{
   /* Unbuffered stdout for MCP protocol. SIGPIPE is ignored process-wide
    * by cli_main so write() to a dead server socket surfaces EPIPE rather
    * than killing the bridge. */
   setvbuf(stdout, NULL, _IONBF, 0);

   char *message = NULL;
   int rc;

   while ((rc = read_mcp_message(stdin, &message)) > 0)
   {
      cJSON *req = cJSON_Parse(message);
      free(message);
      message = NULL;
      if (!req)
      {
         mcp_error(NULL, -32700, "Parse error");
         continue;
      }

      handle_request(req);
      cJSON_Delete(req);
   }

   free(message);
   if (rc < 0)
      mcp_error(NULL, -32700, "Parse error");
   cli_workspace_reverse_channel_stop();
   cli_close(&g_conn);
   return 0;
}

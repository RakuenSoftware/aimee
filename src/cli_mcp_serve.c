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
#include "platform_path.h"
#include "cJSON.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MCP_LINE_MAX         65536
#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_VERSION          "0.2.0"

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

/* Ensure a co-located aimee-server is reachable over the /v1 HTTP UDS. The thin
 * client is connectionless per call now (each server_request is a one-shot POST
 * /v1/rpc), so this is a pure availability probe — no persistent socket. */
static int ensure_connection(void)
{
   if (!g_sock_path)
      g_sock_path = cli_ensure_server_for_method("mcp.call");
   return g_sock_path ? 0 : -1;
}

static cJSON *server_request(cJSON *req, int timeout_ms)
{
   /* Remote aimee-server: POST mcp.call to its first-class /v1 route. The local
    * cli_v1_rpc_local path only speaks the co-located UDS, so a thin client
    * configured with a remote endpoint must route here. Memory/kb/search tools
    * resolve on the server (which proxies DB2 to aimee-kb). File/exec tools whose
    * cwd is a registered detached workspace marshal back to this client over the
    * workspace reverse-channel (Phase 2b); on a non-served cwd they fail safe
    * server-side (the path does not exist there). */
   if (cli_rpc_has_remote_endpoint())
   {
      char *endpoint = cli_rpc_client_endpoint();
      char *bearer = cli_rpc_client_bearer();
      const char *verb = NULL;
      const char *path = cli_v1_route_for_method("mcp.call", &verb);
      cJSON *resp = NULL;
      if (endpoint && path)
      {
         char *body = cJSON_PrintUnformatted(req);
         if (body)
         {
            int status = 0;
            resp = cli_http_request(endpoint, verb ? verb : "POST", path, body, bearer,
                                    timeout_ms, &status);
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

      /* Each call is a one-shot POST /v1/rpc to the co-located server (mcp.call
       * is reached over the local UDS, which permits the full dispatch surface). */
      cJSON *resp = cli_v1_rpc_local(req, timeout_ms);
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
   if (cwd[0] && cJSON_IsObject(arguments) && !cJSON_GetObjectItemCaseSensitive(arguments, "cwd"))
      cJSON_AddStringToObject(arguments, "cwd", cwd);
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

static void handle_initialize(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);

   cJSON *caps = cJSON_CreateObject();
   cJSON *tools_cap = cJSON_CreateObject();
   cJSON_AddBoolToObject(tools_cap, "listChanged", 0);
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

   cJSON_AddStringToObject(result, "instructions",
                           "When you are unsure how aimee works — work queue, "
                           "delegation, memory, git, build, conventions — call "
                           "get_help() before trying anything else. It returns "
                           "the authoritative topic index. Pass a topic name "
                           "for details (e.g. get_help(\"work queue\")). Do not use "
                           "provider-native sub-agent tools such as spawn_agent or Agent; "
                           "use the aimee delegate tool for delegated work.");

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
      cJSON_Delete(resp);
      mcp_error(id, -32603, "Failed to list tools");
      return;
   }

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
   if (status && cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
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

/* --- Phase 2b: workspace reverse-channel for a remote aimee-server ---
 *
 * When mcp-serve targets a remote server, register the client's cwd as a
 * `detached` workspace and serve it on a background thread. The server binds
 * that workspace per mcp.call (by cwd) and marshals file/exec tools back here
 * over runner.poll/respond, so tools run on the client's tree while the agent +
 * memory/kb stay on the server. No-op for a co-located server. */
static volatile sig_atomic_t g_ws_serve_stop = 0;
static pthread_t g_ws_serve_thread;
static int g_ws_serve_active = 0;

struct ws_serve_args
{
   char *id;
   char *endpoint;
   char *bearer;
};

static void *ws_serve_thread_main(void *arg)
{
   struct ws_serve_args *a = arg;
   cli_workspace_serve_loop(a->id, NULL, a->endpoint, a->bearer, &g_ws_serve_stop);
   free(a->id);
   free(a->endpoint);
   free(a->bearer);
   free(a);
   return NULL;
}

static void mcp_workspace_reverse_channel_start(void)
{
   if (!cli_rpc_has_remote_endpoint())
      return;
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)) || !cwd[0])
      return;
   char *endpoint = cli_rpc_client_endpoint();
   if (!endpoint)
      return;
   char *bearer = cli_rpc_client_bearer();

   /* Register the detached workspace (idempotent: re-registering the same root
    * is harmless). Best-effort — if it fails, file tools simply won't route. */
   cJSON *reg = cJSON_CreateObject();
   cJSON_AddStringToObject(reg, "root", cwd);
   cJSON_AddStringToObject(reg, "provider", "detached");
   char *body = cJSON_PrintUnformatted(reg);
   cJSON_Delete(reg);
   if (body)
   {
      int status = 0;
      cJSON *resp = cli_http_request(endpoint, "POST", "/v1/workspaces", body, bearer, 15000,
                                     &status);
      cJSON_Delete(resp);
      free(body);
   }

   struct ws_serve_args *a = calloc(1, sizeof(*a));
   if (!a)
   {
      free(endpoint);
      free(bearer);
      return;
   }
   a->id = strdup(cwd);
   a->endpoint = endpoint; /* ownership passes to the thread */
   a->bearer = bearer;
   if (pthread_create(&g_ws_serve_thread, NULL, ws_serve_thread_main, a) == 0)
      g_ws_serve_active = 1;
   else
   {
      free(a->id);
      free(a->endpoint);
      free(a->bearer);
      free(a);
   }
}

static void mcp_workspace_reverse_channel_stop(void)
{
   if (!g_ws_serve_active)
      return;
   g_ws_serve_stop = 1;
   /* The poll may be mid-flight (~30s); the process is exiting, so detach rather
    * than block on join. */
   pthread_detach(g_ws_serve_thread);
   g_ws_serve_active = 0;
}

int cli_mcp_serve(void)
{
   /* Unbuffered stdout for MCP protocol. SIGPIPE is ignored process-wide
    * by cli_main so write() to a dead server socket surfaces EPIPE rather
    * than killing the bridge. */
   setvbuf(stdout, NULL, _IONBF, 0);

   /* If pointed at a remote aimee-server, serve this client's working tree to it
    * over the reverse-channel so file/exec tools route back here (Phase 2b). */
   mcp_workspace_reverse_channel_start();

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
   mcp_workspace_reverse_channel_stop();
   cli_close(&g_conn);
   return 0;
}

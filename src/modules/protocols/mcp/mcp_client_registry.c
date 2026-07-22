#include "mcp_client_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "log.h"
#include "mcp_osv_cache.h"
#include "osv_check.h"
#include "platform.h"

#ifdef AIMEE_POSIX
#include <pthread.h>
static pthread_mutex_t g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
#define REGISTRY_LOCK()   pthread_mutex_lock(&g_registry_lock)
#define REGISTRY_UNLOCK() pthread_mutex_unlock(&g_registry_lock)
#else
#define REGISTRY_LOCK()   ((void)0)
#define REGISTRY_UNLOCK() ((void)0)
#endif

typedef struct
{
   char name[64];
   mcp_client_session_t session;
   int live;
} mcp_registry_entry_t;

static mcp_registry_entry_t g_registry[CONFIG_MCP_MAX_CLIENTS];
static int g_registry_count;
static int g_registry_booted;
static int g_registry_atexit_registered;

static void registry_warn(const char *name, const char *fmt, ...)
{
   char detail[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(detail, sizeof(detail), fmt, ap);
   va_end(ap);
   if (name && name[0])
      LOG_WARN("mcp-registry", "%s: %s", name, detail);
   else
      LOG_WARN("mcp-registry", "%s", detail);
}

static int registry_find_locked(const char *name)
{
   if (!name || !name[0])
      return -1;
   for (int i = 0; i < g_registry_count; i++)
   {
      if (g_registry[i].live && strcmp(g_registry[i].name, name) == 0)
         return i;
   }
   return -1;
}

static int split_namespaced_tool(const char *qualified_name, char *client_name,
                                 size_t client_name_len, char *tool_name, size_t tool_name_len)
{
   if (!qualified_name || !qualified_name[0])
      return -1;

   const char *sep = strchr(qualified_name, ':');
   if (!sep || sep == qualified_name || !sep[1])
      return -1;

   snprintf(client_name, client_name_len, "%.*s", (int)(sep - qualified_name), qualified_name);
   snprintf(tool_name, tool_name_len, "%s", sep + 1);
   return client_name[0] && tool_name[0] ? 0 : -1;
}

static cJSON *registry_copy_tool_locked(const char *client_name, const char *tool_name,
                                        int timeout_ms, char *err_buf, size_t err_buf_len)
{
   int idx = registry_find_locked(client_name);
   if (idx < 0)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "unknown mcp client");
      return NULL;
   }

   mcp_client_session_t *session = &g_registry[idx].session;
   if (!session->tool_schemas && mcp_client_list_tools(session, timeout_ms) != 0)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "tools/list failed");
      return NULL;
   }

   cJSON *tools = session->tool_schemas
                      ? cJSON_GetObjectItemCaseSensitive(session->tool_schemas, "tools")
                      : NULL;
   if (!cJSON_IsArray(tools))
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "tools/list returned invalid payload");
      return NULL;
   }

   cJSON *tool = NULL;
   cJSON_ArrayForEach(tool, tools)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, tool_name) == 0)
      {
         cJSON *dup = cJSON_Duplicate(tool, 1);
         if (!dup)
         {
            if (err_buf && err_buf_len > 0)
               snprintf(err_buf, err_buf_len, "failed to copy tool schema");
            return NULL;
         }

         char qualified[160];
         snprintf(qualified, sizeof(qualified), "%s:%s", client_name, tool_name);
         cJSON_ReplaceItemInObject(dup, "name", cJSON_CreateString(qualified));

         if (!cJSON_GetObjectItemCaseSensitive(dup, "inputSchema"))
         {
            cJSON *schema = cJSON_CreateObject();
            cJSON_AddStringToObject(schema, "type", "object");
            cJSON_AddObjectToObject(schema, "properties");
            cJSON_AddItemToObject(dup, "inputSchema", schema);
         }
         return dup;
      }
   }

   if (err_buf && err_buf_len > 0)
      snprintf(err_buf, err_buf_len, "unknown remote tool");
   return NULL;
}

static int registry_target_allowlisted(const config_t *cfg, const osv_target_t *target)
{
   if (!cfg || !target || !target->ecosystem[0] || !target->name[0])
      return 0;

   char key[256];
   snprintf(key, sizeof(key), "%s:%s", target->ecosystem, target->name);
   for (int i = 0; i < cfg->mcp_osv_allow_count; i++)
   {
      if (strcmp(cfg->mcp_osv_allow[i], key) == 0)
         return 1;
   }
   return 0;
}

static int registry_osv_blocks_client(const config_t *cfg, const config_mcp_client_t *client)
{
   if (!cfg || !client || !cfg->mcp_osv_enabled ||
       client->transport != CONFIG_MCP_TRANSPORT_STDIO || client->command_count <= 0)
      return 0;

   const char *argv[CONFIG_MCP_MAX_COMMAND_ARGS + 1];
   for (int i = 0; i < client->command_count; i++)
      argv[i] = client->command[i];
   argv[client->command_count] = NULL;

   osv_target_t target;
   if (osv_infer_target_from_argv(client->command_count, argv, &target) != 0)
      return 0;

   osv_result_t result = osv_check_cached(
       cfg->mcp_osv_endpoint, &target, cfg->mcp_osv_cache_ttl_hours, cfg->mcp_osv_offline, 10000);
   int allowlisted = registry_target_allowlisted(cfg, &target);
   if (allowlisted)
   {
      if (result.verdict == OSV_VERDICT_MALWARE)
         registry_warn(client->name, "OSV allowlist permits %s:%s despite advisories: %s",
                       target.ecosystem, target.name,
                       result.advisory_ids[0] ? result.advisory_ids : "MAL-*");
      (void)db1_mcp_osv_audit(client->name, target.ecosystem, target.name, target.version,
                              result.verdict == OSV_VERDICT_MALWARE ? "malware"
                              : result.verdict == OSV_VERDICT_CLEAN ? "clean"
                                                                    : "unknown",
                              result.verdict == OSV_VERDICT_MALWARE ? "allow_allowlisted" : "allow",
                              result.advisory_ids);
      return 0;
   }

   if (result.verdict == OSV_VERDICT_MALWARE)
   {
      registry_warn(client->name, "%s %s:%s has malware advisories: %s",
                    cfg->mcp_osv_enforce ? "blocked" : "shadow-block", target.ecosystem,
                    target.name, result.advisory_ids[0] ? result.advisory_ids : "MAL-*");
      (void)db1_mcp_osv_audit(client->name, target.ecosystem, target.name, target.version,
                              "malware", cfg->mcp_osv_enforce ? "block" : "shadow_block",
                              result.advisory_ids);
      return cfg->mcp_osv_enforce ? 1 : 0;
   }
   (void)db1_mcp_osv_audit(client->name, target.ecosystem, target.name, target.version,
                           result.verdict == OSV_VERDICT_CLEAN ? "clean" : "unknown", "allow",
                           result.advisory_ids);
   if (result.verdict == OSV_VERDICT_UNKNOWN)
   {
      if (cfg->mcp_osv_offline)
         registry_warn(client->name, "OSV offline/cache miss: allowing %s:%s", target.ecosystem,
                       target.name);
      else
         registry_warn(client->name, "OSV check unavailable: allowing %s:%s", target.ecosystem,
                       target.name);
   }
   return 0;
}

static int registry_start_client(const config_t *cfg, const config_mcp_client_t *client)
{
   if (!client || !client->name[0])
      return -1;

   if (registry_find_locked(client->name) >= 0)
   {
      registry_warn(client->name, "duplicate client name ignored");
      return -1;
   }
   if (g_registry_count >= CONFIG_MCP_MAX_CLIENTS)
   {
      registry_warn(client->name, "registry capacity reached");
      return -1;
   }

   if (registry_osv_blocks_client(cfg, client))
      return -1;

   mcp_transport_t *transport = NULL;
   if (client->transport == CONFIG_MCP_TRANSPORT_STDIO)
   {
      const char *argv[CONFIG_MCP_MAX_COMMAND_ARGS + 1];
      for (int i = 0; i < client->command_count; i++)
         argv[i] = client->command[i];
      argv[client->command_count] = NULL;
      transport = mcp_transport_stdio_open(argv, client->cwd[0] ? client->cwd : NULL);
   }
   else if (client->transport == CONFIG_MCP_TRANSPORT_SSE)
   {
      const char *bearer = NULL;
      if (client->bearer_token_env[0])
         bearer = getenv(client->bearer_token_env);
      transport = mcp_transport_sse_open(client->url, bearer);
   }

   if (!transport)
   {
      registry_warn(client->name, "failed to open %s transport",
                    client->transport == CONFIG_MCP_TRANSPORT_SSE ? "sse" : "stdio");
      return -1;
   }

   mcp_registry_entry_t entry;
   memset(&entry, 0, sizeof(entry));
   snprintf(entry.name, sizeof(entry.name), "%s", client->name);

   if (mcp_client_session_init(&entry.session, client->name, transport) != 0)
   {
      mcp_transport_close(transport);
      registry_warn(client->name, "failed to initialize session");
      return -1;
   }
   if (mcp_client_initialize(&entry.session, 3000) != 0)
   {
      mcp_client_session_close(&entry.session);
      registry_warn(client->name, "initialize handshake failed");
      return -1;
   }

   entry.live = 1;
   g_registry[g_registry_count++] = entry;
   return 0;
}

int mcp_client_registry_boot(const config_t *cfg)
{
   if (!cfg)
      return 0;

   REGISTRY_LOCK();
   if (g_registry_booted)
   {
      int count = g_registry_count;
      REGISTRY_UNLOCK();
      return count;
   }

   if (!g_registry_atexit_registered)
   {
      atexit(mcp_client_registry_shutdown);
      g_registry_atexit_registered = 1;
   }

   for (int i = 0; i < cfg->mcp_client_count; i++)
      (void)registry_start_client(cfg, &cfg->mcp_clients[i]);

   g_registry_booted = 1;
   int count = g_registry_count;
   REGISTRY_UNLOCK();
   return count;
}

void mcp_client_registry_shutdown(void)
{
   REGISTRY_LOCK();
   for (int i = 0; i < g_registry_count; i++)
   {
      if (!g_registry[i].live)
         continue;
      mcp_client_session_close(&g_registry[i].session);
      memset(&g_registry[i], 0, sizeof(g_registry[i]));
   }
   g_registry_count = 0;
   g_registry_booted = 0;
   REGISTRY_UNLOCK();
}

mcp_client_session_t *mcp_client_registry_get(const char *name)
{
   mcp_client_session_t *session = NULL;
   REGISTRY_LOCK();
   int idx = registry_find_locked(name);
   if (idx >= 0)
      session = &g_registry[idx].session;
   REGISTRY_UNLOCK();
   return session;
}

int mcp_client_registry_count(void)
{
   REGISTRY_LOCK();
   int count = g_registry_count;
   REGISTRY_UNLOCK();
   return count;
}

const char *mcp_client_registry_name_at(int index)
{
   const char *name = NULL;
   REGISTRY_LOCK();
   if (index >= 0 && index < g_registry_count && g_registry[index].live)
      name = g_registry[index].name;
   REGISTRY_UNLOCK();
   return name;
}

int mcp_client_registry_list_tools(const char *name, int timeout_ms, cJSON **out_tools,
                                   char *err_buf, size_t err_buf_len)
{
   if (out_tools)
      *out_tools = NULL;
   if (err_buf && err_buf_len > 0)
      err_buf[0] = '\0';

   REGISTRY_LOCK();
   int idx = registry_find_locked(name);
   if (idx < 0)
   {
      REGISTRY_UNLOCK();
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "unknown mcp client");
      return -1;
   }

   mcp_client_session_t *session = &g_registry[idx].session;
   if (!session->tool_schemas && mcp_client_list_tools(session, timeout_ms) != 0)
   {
      REGISTRY_UNLOCK();
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "tools/list failed");
      return -1;
   }

   if (out_tools && session->tool_schemas)
      *out_tools = cJSON_Duplicate(session->tool_schemas, 1);
   REGISTRY_UNLOCK();

   if (out_tools && session->tool_schemas && !*out_tools)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "failed to copy tool schemas");
      return -1;
   }
   return 0;
}

cJSON *mcp_client_registry_build_namespaced_tools(int timeout_ms)
{
   cJSON *out = cJSON_CreateArray();
   if (!out)
      return NULL;

   REGISTRY_LOCK();
   for (int i = 0; i < g_registry_count; i++)
   {
      if (!g_registry[i].live)
         continue;

      char err[128] = "";
      if (!g_registry[i].session.tool_schemas &&
          mcp_client_list_tools(&g_registry[i].session, timeout_ms) != 0)
      {
         registry_warn(g_registry[i].name, "%s", err[0] ? err : "tools/list failed");
         continue;
      }

      cJSON *tools =
          g_registry[i].session.tool_schemas
              ? cJSON_GetObjectItemCaseSensitive(g_registry[i].session.tool_schemas, "tools")
              : NULL;
      if (!cJSON_IsArray(tools))
      {
         registry_warn(g_registry[i].name, "tools/list returned invalid payload");
         continue;
      }

      cJSON *tool = NULL;
      cJSON_ArrayForEach(tool, tools)
      {
         cJSON *name = cJSON_GetObjectItemCaseSensitive(tool, "name");
         if (!cJSON_IsString(name) || !name->valuestring[0])
            continue;

         cJSON *dup = cJSON_Duplicate(tool, 1);
         if (!dup)
            continue;

         char qualified[160];
         snprintf(qualified, sizeof(qualified), "%s:%s", g_registry[i].name, name->valuestring);
         cJSON_ReplaceItemInObject(dup, "name", cJSON_CreateString(qualified));
         if (!cJSON_GetObjectItemCaseSensitive(dup, "inputSchema"))
         {
            cJSON *schema = cJSON_CreateObject();
            cJSON_AddStringToObject(schema, "type", "object");
            cJSON_AddObjectToObject(schema, "properties");
            cJSON_AddItemToObject(dup, "inputSchema", schema);
         }
         cJSON_AddItemToArray(out, dup);
      }
   }
   REGISTRY_UNLOCK();
   return out;
}

int mcp_client_registry_get_tool_schema(const char *qualified_name, int timeout_ms,
                                        cJSON **out_tool, char *err_buf, size_t err_buf_len)
{
   if (out_tool)
      *out_tool = NULL;
   if (err_buf && err_buf_len > 0)
      err_buf[0] = '\0';

   char client_name[64];
   char tool_name[128];
   if (split_namespaced_tool(qualified_name, client_name, sizeof(client_name), tool_name,
                             sizeof(tool_name)) != 0)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "not a namespaced mcp tool");
      return -1;
   }

   REGISTRY_LOCK();
   cJSON *tool =
       registry_copy_tool_locked(client_name, tool_name, timeout_ms, err_buf, err_buf_len);
   REGISTRY_UNLOCK();
   if (!tool)
      return -1;

   if (out_tool)
      *out_tool = tool;
   else
      cJSON_Delete(tool);
   return 0;
}

int mcp_client_registry_call_tool(const char *qualified_name, const cJSON *args, int timeout_ms,
                                  cJSON **out_result, char *err_buf, size_t err_buf_len)
{
   if (out_result)
      *out_result = NULL;
   if (err_buf && err_buf_len > 0)
      err_buf[0] = '\0';

   char client_name[64];
   char tool_name[128];
   if (split_namespaced_tool(qualified_name, client_name, sizeof(client_name), tool_name,
                             sizeof(tool_name)) != 0)
   {
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "not a namespaced mcp tool");
      return -1;
   }

   REGISTRY_LOCK();
   int idx = registry_find_locked(client_name);
   if (idx < 0)
   {
      REGISTRY_UNLOCK();
      if (err_buf && err_buf_len > 0)
         snprintf(err_buf, err_buf_len, "unknown mcp client");
      return -1;
   }

   int rc = mcp_client_call_tool(&g_registry[idx].session, tool_name, args, timeout_ms, out_result,
                                 err_buf, err_buf_len);
   REGISTRY_UNLOCK();
   return rc;
}

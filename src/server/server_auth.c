/* server_auth.c: authentication, capability tokens, and per-method capability checks */
#include "aimee.h"
#include "db1.h"
#include "server.h"
#include "log.h"
#include "platform_process.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Declarative method-to-capability registry --- */

/* Exact matches are checked first, then prefix matches (trailing '*').
 * Order within each group matters: first match wins for prefixes.
 * memory.store must appear before memory.* to get the correct capability. */
const method_policy_t method_registry[] = {
    /* No capability required */
    {"server.info", 0, "server information"},
    {"server.health", 0, "server health check"},
    {"api.status", CAP_SESSION_READ, "public /v1 API status"},
    {"api.enable", CAP_SESSION_ADMIN, "enable public /v1 API listener"},
    {"api.disable", CAP_SESSION_ADMIN, "disable public /v1 API listener"},
    {"init.run", CAP_TOOL_EXECUTE, "initialize local stores"},
    {"launch.run", CAP_TOOL_EXECUTE, "launch session"},
    {"hud.status", CAP_SESSION_READ, "HUD status"},
    {"agent.*", CAP_DELEGATE, "agent configuration"},
    {"webchat.*", CAP_DASHBOARD_READ, "webchat client operation"},
    {"auth", 0, "authenticate"},
    /* Hooks */
    {"hooks.pre", CAP_TOOL_EXECUTE, "pre-tool hook"},
    {"hooks.post", CAP_TOOL_EXECUTE, "post-tool hook"},
    {"hooks.session_start", CAP_TOOL_EXECUTE, "session-start hook"},
    /* Sessions (prefix) */
    {"session.credentials", CAP_CHAT, "push session agent credentials"},
    {"session.*", CAP_SESSION_READ, "session operation"},
    {"trajectory.export", CAP_SESSION_READ, "trajectory export"},
    {"trajectory.batch", CAP_DELEGATE, "trajectory batch generation"},
    /* Memory (exact before prefix) */
    {"memory.store", CAP_MEMORY_WRITE, "store memory"},
    {"memory.*", CAP_MEMORY_READ, "memory operation"},
    /* Index (prefix) */
    {"blast_radius.preview", CAP_INDEX_READ, "blast radius preview"},
    {"index.*", CAP_INDEX_READ, "index operation"},
    /* Rules (exact admin before read prefix) */
    {"rules.delete", CAP_RULES_ADMIN, "delete rule"},
    {"rules.*", CAP_RULES_READ, "rules operation"},
    {"skill.list", CAP_SESSION_READ, "skill list"},
    {"skill.show", CAP_SESSION_READ, "skill show"},
    {"skill.*", CAP_TOOL_WRITE, "skill mutation"},
    {"toolset.*", CAP_SESSION_READ, "toolset operation"},
    /* Collab rules (exact admin before read prefix) */
    {"collab_rules.approve", CAP_RULES_ADMIN, "approve collab rule"},
    {"collab_rules.reject", CAP_RULES_ADMIN, "reject collab rule"},
    {"collab_rules.retire", CAP_RULES_ADMIN, "retire collab rule"},
    {"collab_rules.*", CAP_RULES_READ, "collab rules operation"},
    /* Working memory (prefix) */
    {"wm.*", CAP_SESSION_READ, "working memory operation"},
    /* Per-session primary agent selection */
    {"primary.*", CAP_SESSION_READ, "primary agent selection"},
    {"work.list", CAP_SESSION_READ, "work queue list"},
    {"work.stats", CAP_SESSION_READ, "work queue stats"},
    {"work.*", CAP_TOOL_EXECUTE, "work queue operation"},
    {"attempt.*", CAP_SESSION_READ, "attempt log operation"},
    /* Dashboard (prefix) */
    {"dashboard.*", CAP_DASHBOARD_READ, "dashboard operation"},
    {"plugin.list", CAP_DASHBOARD_READ, "plugin list"},
    {"plugin.enable", CAP_TOOL_EXECUTE, "enable plugin"},
    {"plugin.disable", CAP_TOOL_EXECUTE, "disable plugin"},
    {"lsp.*", CAP_DASHBOARD_READ, "lsp status"},
    /* Workspace. Reads (context/get/list) are index:read; register/remove
     * mutate the instance-scoped registry and a detached client performs them
     * remotely, so they are tool:execute (a read-scoped bearer is denied) but
     * NOT local-UDS-only — see the /v1/workspaces rows. */
    {"workspace.context", CAP_INDEX_READ, "workspace context"},
    {"workspace.get", CAP_INDEX_READ, "workspace manifest"},
    {"workspace.list", CAP_INDEX_READ, "list workspaces"},
    {"workspace.add", CAP_TOOL_EXECUTE, "register a workspace"},
    {"workspace.remove", CAP_TOOL_EXECUTE, "remove a workspace"},
    {"workspace.mirror-sync", CAP_TOOL_EXECUTE, "sync client diff to a mirror workspace"},
    {"runner.poll", CAP_TOOL_EXECUTE, "detached workspace runner poll"},
    {"runner.respond", CAP_TOOL_EXECUTE, "detached workspace runner respond"},
    /* Compute */
    {"tool.execute", CAP_TOOL_EXECUTE, "execute tool"},
    {"delegate", CAP_DELEGATE, "delegate task"},
    {"delegate.aggregate", CAP_DELEGATE, "Mixture-of-Agents ensemble aggregate"},
    {"delegate.roundtable", CAP_DELEGATE, "multi-round agent roundtable"},
    {"delegate.status", CAP_DELEGATE, "delegate status"},
    /* Credential vault (WP-C.1): UDS-only in practice — the service layer refuses
     * any non-ATTEST_UDS_PEERCRED principal — but gated here as CAP_DELEGATE so a
     * scoped/read-only TCP bearer cannot even reach the route. */
    {"vault.unlock", CAP_DELEGATE, "unlock the credential vault"},
    {"vault.set", CAP_DELEGATE, "store a vault credential"},
    {"vault.list", CAP_DELEGATE, "list vault credential names"},
    {"vault.delete", CAP_DELEGATE, "delete a vault credential"},
    {"vault.lock", CAP_DELEGATE, "lock the credential vault"},
    {"jobs.list", CAP_DELEGATE, "list delegate jobs"},
    {"jobs.status", CAP_DELEGATE, "delegate job status"},
    {"jobs.logs", CAP_DELEGATE, "delegate job logs"},
    {"jobs.cancel", CAP_DELEGATE, "cancel delegate job"},
    {"aux.config_show", CAP_SESSION_READ, "auxiliary model config"},
    {"config.show", CAP_SESSION_READ, "show configuration"},
    {"config.get", CAP_SESSION_READ, "read configuration value"},
    {"config.set", CAP_SESSION_ADMIN, "set configuration value"},
    {"pipeline.status", CAP_SESSION_READ, "roundtable authoring pipeline status"},
    {"pipeline.list", CAP_SESSION_READ, "list roundtable authoring pipelines"},
    {"pipeline.*", CAP_DELEGATE, "roundtable authoring pipeline control"},
    {"aux.test", CAP_DELEGATE, "execute auxiliary model test"},
    {"delegate.reply", CAP_DELEGATE, "delegate reply"},
    {"delegate.log", CAP_DELEGATE, "delegation episode log"},
    {"delegate.backend_list", CAP_DELEGATE, "list delegate execution backends"},
    {"delegate.backend_exec", CAP_DELEGATE, "execute through a delegate backend"},
    {"episode.list", CAP_DELEGATE, "list delegation episodes"},
    {"agent.episodes", CAP_DELEGATE, "agent episode history"},
    {"eval.*", CAP_DELEGATE, "eval harness"},
    {"chat.send_stream", CAP_CHAT, "chat stream"},
    {"mcp.tools_list", CAP_TOOL_EXECUTE, "MCP tool list"},
    {"mcp.audit", CAP_TOOL_EXECUTE, "MCP OSV audit"},
    {"mcp.recheck", CAP_TOOL_EXECUTE, "MCP OSV recheck"},
    {"mcp.call", CAP_TOOL_EXECUTE, "MCP tool call"},
    /* Triggers */
    {"trigger.*", CAP_TOOL_EXECUTE, "trigger operation"},

    /* ── op-parity capability assignments (P2) ───────────────────────────────
     * Every dispatch method gets an explicit, intentional capability so the
     * /v1 route gate (which inherits caps from these via the op twin) and the
     * NDJSON gate both stop falling back to CAPS_ALL (deny-by-default = UDS-
     * only by accident). Reads get a *_READ cap so query/scoped bearers can
     * reach them; ordinary mutations get execute/admin (in CAPS_AUTHENTICATED,
     * so an authenticated remote bearer may invoke them). Full corpus/index
     * REBUILDS use CAP_INDEX_ADMIN, which is deliberately outside
     * CAPS_AUTHENTICATED — i.e. UDS / local-trust only. */
    /* Knowledge base: search/status read; build/ingest/update rebuild the store. */
    {"kb.search", CAP_INDEX_READ, "knowledge search"},
    {"kb.status", CAP_DASHBOARD_READ, "knowledge base status"},
    {"optimize.export", CAP_DASHBOARD_READ, "bandit optimization export"},
    {"optimize.promote", CAP_INDEX_ADMIN, "promote a bandit arm to default"},
    {"optimize.replay_record", CAP_INDEX_ADMIN, "record bandit replay attribution"},
    {"calibration.readiness", CAP_DASHBOARD_READ, "calibration readiness report"},
    {"demotion.check", CAP_DASHBOARD_READ, "demotion dry-run report"},
    {"kb.build", CAP_INDEX_ADMIN, "build knowledge base"},
    {"kb.ingest", CAP_INDEX_ADMIN, "ingest corpus"},
    {"kb.update", CAP_INDEX_ADMIN, "update knowledge base"},
    {"kb.docs.push", CAP_INDEX_ADMIN, "push documents into the knowledge base (ingest)"},
    {"kb.ingest.status", CAP_INDEX_READ, "knowledge-base ingest status (read)"},
    /* Code index/graph rebuilds (mutating) — distinct from the index.* reads. */
    {"index.scan", CAP_INDEX_ADMIN, "scan / re-index the codebase"},
    {"graph.sync_code", CAP_INDEX_ADMIN, "sync the code graph"},
    {"graph.*", CAP_INDEX_READ, "code graph query"},
    /* Curator: queries read; synthesize rebuilds artifacts (LLM). */
    {"curator.synthesize", CAP_INDEX_ADMIN, "curator synthesis"},
    {"curator.*", CAP_INDEX_READ, "curator query"},
    /* Cron: list/show/history read; mutations schedule or trigger jobs. */
    {"cron.list", CAP_SESSION_READ, "list cron jobs"},
    {"cron.show", CAP_SESSION_READ, "show cron job"},
    {"cron.history", CAP_SESSION_READ, "cron job history"},
    {"cron.*", CAP_TOOL_EXECUTE, "cron job mutation"},
    /* Providers / models: catalogs read; configuration mutations are admin. */
    {"provider.list", CAP_SESSION_READ, "list providers"},
    {"provider.get", CAP_SESSION_READ, "get provider"},
    {"provider.show", CAP_SESSION_READ, "show provider"},
    {"provider.models", CAP_SESSION_READ, "provider models"},
    {"provider.quota", CAP_SESSION_READ, "provider quota"},
    {"provider.*", CAP_SESSION_ADMIN, "provider configuration"},
    {"model.list", CAP_SESSION_READ, "list models"},
    {"model.show", CAP_SESSION_READ, "show model"},
    {"model.refresh", CAP_SESSION_ADMIN, "refresh model catalog"},
    /* Identity: show/diff read; snapshot mutates. */
    {"identity.snapshot", CAP_SESSION_ADMIN, "snapshot identity"},
    {"identity.*", CAP_SESSION_READ, "identity query"},
    /* Dogfood: report is a read view; review/tag write feedback. */
    {"dogfood.report", CAP_DASHBOARD_READ, "dogfood report"},
    {"dogfood.*", CAP_TOOL_EXECUTE, "dogfood feedback"},
    /* Insights overview (read). */
    {"insights.overview", CAP_DASHBOARD_READ, "insights overview"},
    /* Server worker-pool status (read). */
    {"workers", CAP_DASHBOARD_READ, "server worker-pool status"},
    /* Delegate launch + the singular job.* alias of jobs.*. */
    {"delegate.launch", CAP_DELEGATE, "launch delegate"},
    {"job.*", CAP_DELEGATE, "delegate job operation"},
    /* Rules generation creates rules via LLM (admin), unlike the rules.* reads. */
    {"rules.generate", CAP_RULES_ADMIN, "generate collab rules"},
    /* Work board is a read view (the work.* default is tool:execute). */
    {"work.board", CAP_SESSION_READ, "work board view"},
    /* Delegate worktree maintenance. */
    {"worktree.gc", CAP_TOOL_EXECUTE, "garbage-collect delegate worktrees"},
    /* Sentinel */
    {NULL, 0, NULL}};

const int method_registry_count =
    (int)(sizeof(method_registry) / sizeof(method_registry[0])) - 1; /* exclude sentinel */

/* --- Capability check --- */

uint32_t server_capability_for_method(const char *method)
{
   /* Same exact-then-prefix lookup as server_policy_for_method; derive the
    * cap from the matched entry, or deny-by-default for an unknown method. */
   const method_policy_t *policy = server_policy_for_method(method);
   return policy ? policy->required_caps : CAPS_ALL;
}

/* Return the policy entry for a method, or NULL if not registered */
const method_policy_t *server_policy_for_method(const char *method)
{
   /* Pass 1: exact matches */
   for (int i = 0; i < method_registry_count; i++)
   {
      const char *pat = method_registry[i].method;
      size_t plen = strlen(pat);
      if (plen > 0 && pat[plen - 1] == '*')
         continue;
      if (strcmp(method, pat) == 0)
         return &method_registry[i];
   }

   /* Pass 2: prefix matches */
   for (int i = 0; i < method_registry_count; i++)
   {
      const char *pat = method_registry[i].method;
      size_t plen = strlen(pat);
      if (plen > 0 && pat[plen - 1] == '*')
      {
         if (strncmp(method, pat, plen - 1) == 0)
            return &method_registry[i];
      }
   }

   return NULL;
}

/* --- Auth handler --- */

int server_ct_equal(const char *a, const char *b)
{
   size_t alen = a ? strlen(a) : 0;
   size_t blen = b ? strlen(b) : 0;
   size_t len = alen > blen ? alen : blen;
   unsigned char diff = (alen != blen) ? 1 : 0;
   for (size_t i = 0; i < len; i++)
   {
      unsigned char x = i < alen ? (unsigned char)a[i] : 0;
      unsigned char y = i < blen ? (unsigned char)b[i] : 0;
      diff |= x ^ y;
   }
   return diff == 0;
}

/* server_session.c: server-side session management handlers */
#include "platform_process.h"

/* Generate a UUID using platform random */
static void generate_uuid(char *buf, size_t len)
{
   unsigned char raw[16];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      memset(raw, 0, sizeof(raw));
   snprintf(buf, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
            raw[11], raw[12], raw[13], raw[14], raw[15]);
}

int handle_session_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jct = cJSON_GetObjectItemCaseSensitive(req, "client_type");
   const char *client_type = cJSON_IsString(jct) ? jct->valuestring : "cli";

   (void)ctx;

   /* Generate session ID (persisted in DB, not RAM) */
   char sid[64];
   generate_uuid(sid, sizeof(sid));

   /* Build principal from peer UID */
   char principal[32];
   snprintf(principal, sizeof(principal), "uid:%d", (int)conn->peer_uid);

   char ts[32];
   now_utc(ts, sizeof(ts));

   if (db1_server_session_create(sid, client_type, principal) != 0)
      return server_send_error(conn, "failed to create session", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "session_id", sid);
   cJSON_AddStringToObject(resp, "created_at", ts);
   return server_send_ok(conn, resp);
}

int handle_session_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(jlimit) ? (int)jlimit->valuedouble : 100;
   if (limit < 1)
      limit = 100;
   if (limit > 100)
      limit = 100;

   db1_server_session_t rows[100];
   int n = db1_server_session_list_recent(rows, limit);
   if (n < 0)
      return server_send_error(conn, "failed to list sessions", NULL);

   cJSON *sessions = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "id", rows[i].id);
      cJSON_AddStringToObject(s, "client_type", rows[i].client_type);
      cJSON_AddStringToObject(s, "principal", rows[i].principal);
      cJSON_AddStringToObject(s, "title", rows[i].title);
      cJSON_AddStringToObject(s, "created_at", rows[i].created_at);
      cJSON_AddStringToObject(s, "last_activity_at", rows[i].last_activity_at);
      cJSON_AddStringToObject(s, "outcome", rows[i].outcome);
      if (rows[i].claude_session_id[0])
         cJSON_AddStringToObject(s, "claude_session_id", rows[i].claude_session_id);
      cJSON_AddItemToArray(sessions, s);
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "sessions", sessions);
   return server_send_ok(conn, resp);
}

int handle_session_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : NULL;
   if (!sid || !sid[0])
      return server_send_error(conn, "missing session_id", NULL);

   db1_server_session_t row;
   if (db1_server_session_get(sid, &row) != 0)
      return server_send_error(conn, "session not found", NULL);

   cJSON *resp = jo_ok();
   cJSON *s = cJSON_CreateObject();
   cJSON_AddStringToObject(s, "id", row.id);
   cJSON_AddStringToObject(s, "client_type", row.client_type);
   cJSON_AddStringToObject(s, "principal", row.principal);
   cJSON_AddStringToObject(s, "title", row.title);
   cJSON_AddStringToObject(s, "created_at", row.created_at);
   cJSON_AddStringToObject(s, "last_activity_at", row.last_activity_at);
   cJSON_AddStringToObject(s, "outcome", row.outcome);
   if (row.claude_session_id[0])
      cJSON_AddStringToObject(s, "claude_session_id", row.claude_session_id);
   cJSON_AddItemToObject(resp, "session", s);

   return server_send_ok(conn, resp);
}

#include "client_constants.h"
#include "client_integrations.h"
#include "aimee_client.h"
#include "cli_client.h"
#include "aimee_home.h"
#include "client_config.h"
#include "platform_path.h"
#include "platform_process.h"
#include "cJSON.h"
#include "dstr.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Injected by the CLI (client_integrations_set_delegate_probe): reports whether
 * usable aimee delegates exist (1), none do (0), or the answer is unknown (-1,
 * server unreachable). CORE must not make a /v1 call directly — it links into
 * both the DB-free client and the server — so the CLI supplies the real probe. */
static int (*g_delegate_probe)(void) = NULL;

void client_integrations_set_delegate_probe(int (*probe)(void))
{
   g_delegate_probe = probe;
}

typedef enum
{
   CLIENT_TOOL_TRANSPORT_CLI_FIRST = 0,
   CLIENT_TOOL_TRANSPORT_MCP_FIRST,
} client_tool_transport_preference_t;

typedef struct
{
   int cli;
   int mcp;
} client_tool_registration_plan_t;

typedef struct
{
   char *cli_only;
   char *mcp_only;
   int complete;
} client_tool_surface_requirements_t;

/* Select a surface per capability, then aggregate the selected surfaces into a
 * host registration plan. `requires_*_only` comes from the projected module
 * closure: it keeps a future one-surface server/KB capability reachable without
 * presenting every dual-surface capability twice. */
static client_tool_registration_plan_t
client_tool_registration_plan(client_tool_transport_preference_t preference, int cli_supported,
                              int mcp_supported, int requires_cli_only, int requires_mcp_only)
{
   client_tool_registration_plan_t plan = {0};
   if (preference == CLIENT_TOOL_TRANSPORT_MCP_FIRST)
   {
      if (mcp_supported)
         plan.mcp = 1;
      else if (cli_supported)
         plan.cli = 1;
   }
   else
   {
      if (cli_supported)
         plan.cli = 1;
      else if (mcp_supported)
         plan.mcp = 1;
   }

   if (requires_cli_only && cli_supported)
      plan.cli = 1;
   if (requires_mcp_only && mcp_supported)
      plan.mcp = 1;
   return plan;
}

static client_tool_transport_preference_t client_tool_transport_parse(const char *value)
{
   return value && (strcmp(value, "mcp-first") == 0 || strcmp(value, "mcp_first") == 0 ||
                    strcmp(value, "mcp") == 0)
              ? CLIENT_TOOL_TRANSPORT_MCP_FIRST
              : CLIENT_TOOL_TRANSPORT_CLI_FIRST;
}

static client_tool_transport_preference_t client_tool_transport_preference(void);

static char *projected_names_join(cJSON *array)
{
   size_t total = 1;
   size_t count = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, array)
   {
      if (!cJSON_IsString(item) || !item->valuestring[0] || strchr(item->valuestring, ','))
         continue;
      size_t n = strlen(item->valuestring);
      size_t separator = count ? 1u : 0u;
      if (n > (size_t)-1 - total - separator)
         return NULL;
      total += n + separator;
      count++;
   }

   char *out = malloc(total);
   if (!out)
      return NULL;
   size_t used = 0;
   cJSON_ArrayForEach(item, array)
   {
      if (!cJSON_IsString(item) || !item->valuestring[0] || strchr(item->valuestring, ','))
         continue;
      size_t n = strlen(item->valuestring);
      if (used)
         out[used++] = ',';
      memcpy(out + used, item->valuestring, n);
      used += n;
   }
   out[used] = '\0';
   return out;
}

static void client_tool_surface_requirements_dispose(client_tool_surface_requirements_t *req)
{
   if (!req)
      return;
   free(req->cli_only);
   free(req->mcp_only);
   req->cli_only = NULL;
   req->mcp_only = NULL;
   req->complete = 0;
}

static client_tool_surface_requirements_t client_tool_surface_requirements_from_json(cJSON *root)
{
   client_tool_surface_requirements_t req = {.complete = 1};
   cJSON *surfaces =
       cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "agent_surfaces") : NULL;
   if (!cJSON_IsObject(surfaces))
      return req;
   req.cli_only = projected_names_join(cJSON_GetObjectItemCaseSensitive(surfaces, "cli_only"));
   req.mcp_only = projected_names_join(cJSON_GetObjectItemCaseSensitive(surfaces, "mcp_only"));
   if (!req.cli_only || !req.mcp_only)
   {
      fprintf(stderr, "aimee: unable to preserve the complete projected tool surface; keeping the "
                      "existing client registration\n");
      free(req.cli_only);
      free(req.mcp_only);
      req.cli_only = NULL;
      req.mcp_only = NULL;
      req.complete = 0;
   }
   return req;
}

/* Read the authoritative Runtime projection. When the Runtime has merged a
 * Control-Plane projection, its agent_surfaces already includes aimee-kb module
 * descriptors; the thin client never contacts the KB directly. Older/unreachable
 * servers simply contribute no one-surface requirements for this registration
 * pass and are retried on the next Aimee invocation. */
static client_tool_surface_requirements_t client_projected_surface_requirements(void)
{
   client_tool_surface_requirements_t req = {.complete = 1};
   /* A thin client pointed at a remote must not reach for the local socket.
    * aimee_client_request falls back to the UDS when it cannot resolve the
    * remote itself -- which it cannot when the endpoint arrived as a flag
    * rather than config -- and that fallback is exactly what the
    * remote-exclusive contract forbids. Skipping leaves the conservative
    * default already in `req`: no known MCP-only requirement, so planning
    * stays CLI-first, which is where it should land without evidence anyway. */
   if (cli_v1_remote_endpoint_is_network())
      return req;
   int status = 0;
   aimee_client_suppress_conn_errors(1);
   char *body = aimee_client_request("GET", "/v1/capabilities", NULL, &status);
   aimee_client_suppress_conn_errors(0);
   cJSON *response = body ? cJSON_Parse(body) : NULL;
   free(body);
   if (status == 200 && response)
      req = client_tool_surface_requirements_from_json(response);
   cJSON_Delete(response);
   return req;
}

static void ensure_parent_dir(const char *path, mode_t mode)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", path);
   char *last_slash = strrchr(dir, '/');
   if (!last_slash)
      return;
   *last_slash = '\0';
   for (char *p = dir + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         platform_mkdir_p(dir, mode);
         *p = '/';
      }
   }
   platform_mkdir_p(dir, mode);
}

static int write_text_file(const char *path, const char *content, mode_t mode)
{
   dstr_t existing;
   dstr_init(&existing);
   if (dstr_read_file(&existing, path) == 0 && dstr_equals_cstr(&existing, content))
   {
      dstr_free(&existing);
      return 0;
   }
   dstr_free(&existing);

   ensure_parent_dir(path, 0700);

   /* Atomic write: write to a temp file then rename into place so readers
    * never see a truncated/empty file (avoids race with Claude Code reading
    * settings.json while we rewrite it). */
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
   FILE *fp = fopen(tmp, "w");
   if (!fp)
      return -1;
   fputs(content, fp);
   if (fclose(fp) != 0)
   {
      unlink(tmp);
      return -1;
   }
   chmod(tmp, mode);
   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

/* Codex discovers .mcp.json and SKILL.md by convention even when the plugin
 * manifest does not reference them. A transport switch therefore has to remove
 * Aimee's generated file, not merely narrow plugin.json, or the previous surface
 * remains active as an undeclared duplicate. These paths are generator-owned;
 * user-authored client files live outside the Aimee plugin bundle. */
static int sync_generated_surface_file(const char *path, const char *content, int enabled)
{
   if (enabled)
      return write_text_file(path, content, 0644);
   if (unlink(path) != 0 && errno != ENOENT && errno != ENOTDIR)
   {
      fprintf(stderr, "aimee: unable to retire unselected generated surface %s: %s\n", path,
              strerror(errno));
      return -1;
   }
   return 0;
}

/* Probe the destination directory, not just the host's nominal feature set.
 * Codex supports both surfaces, but a managed install can expose the plugin's
 * root while making the nested skill directory unavailable. In that case the
 * registration plan must select MCP instead of claiming CLI succeeded. */
static int generated_surface_path_writable(const char *path)
{
   ensure_parent_dir(path, 0700);
   char probe[MAX_PATH_LEN];
   int n = snprintf(probe, sizeof(probe), "%s.aimee-probe.XXXXXX", path);
   if (n < 0 || (size_t)n >= sizeof(probe))
      return 0;
   int fd = mkstemp(probe);
   if (fd < 0)
      return 0;
   int close_rc = close(fd);
   int unlink_rc = unlink(probe);
   return close_rc == 0 && unlink_rc == 0;
}

static int generated_surface_paths_writable(const char *const *paths, size_t count)
{
   for (size_t i = 0; i < count; i++)
      if (!generated_surface_path_writable(paths[i]))
         return 0;
   return 1;
}

/* Path to write into the user's GLOBAL client config (Claude Code hooks, MCP
 * server command). Those files outlive whatever binary happens to be running,
 * so the path must outlive it too.
 *
 * This used to return the running executable's own path whenever that binary was
 * named `aimee`. Build a client in a throwaway worktree, run it once, and it
 * rewrote ~/.claude/settings.json to point every hook at that worktree — then the
 * worktree was deleted and EVERY hook in EVERY session began failing with
 * "/bin/sh: 1: /home/.../aimee-<sha>/aimee: not found", in projects that had
 * nothing to do with the build. A transient path must never be persisted into
 * durable config.
 *
 * Split from the getenv/exe-path plumbing so the decision itself is testable:
 * |installed| is the installed client (NULL/empty when absent) and |exe| is the
 * running binary. Prefer the install; fall back to the running binary only when
 * there is nothing installed to point at — the first-run bootstrap the exe path
 * was there to serve. Returns 0 and fills |out| on success. */
int client_integrations_pick_bin_path(const char *installed, const char *exe, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   if (installed && installed[0])
      return (size_t)snprintf(out, cap, "%s", installed) < cap ? 0 : -1;
   if (exe && exe[0])
      return (size_t)snprintf(out, cap, "%s", exe) < cap ? 0 : -1;
   return -1;
}

/* The installed client, or NULL when there is none. */
static const char *aimee_installed_bin_path(char *buf, size_t cap)
{
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return NULL;
   if ((size_t)snprintf(buf, cap, "%s/.local/bin/aimee", home) >= cap)
      return NULL;
   return access(buf, X_OK) == 0 ? buf : NULL;
}

static const char *resolved_aimee_bin_path(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;

   char installed_buf[MAX_PATH_LEN];
   const char *installed = aimee_installed_bin_path(installed_buf, sizeof(installed_buf));

   char exe[MAX_PATH_LEN] = "";
   if (platform_get_exe_path(exe, sizeof(exe)) == 0)
   {
      char *base = strrchr(exe, '/');
      base = base ? base + 1 : exe;
      if (strcmp(base, "aimee-server") == 0)
         snprintf(base, sizeof(exe) - (size_t)(base - exe), "aimee");
      else if (strcmp(base, "aimee-server.exe") == 0)
         snprintf(base, sizeof(exe) - (size_t)(base - exe), "aimee.exe");
      else if (strcmp(base, "aimee") != 0 && strcmp(base, "aimee.exe") != 0 &&
               strcmp(base, "aimee-client") != 0 && strcmp(base, "aimee-client.exe") != 0)
         exe[0] = '\0'; /* not a client binary — not a usable fallback */
   }

   if (client_integrations_pick_bin_path(installed, exe, path, sizeof(path)) == 0)
      return path;

   const char *home = getenv("HOME");
   path[0] = '\0';
   if (home)
      snprintf(path, sizeof(path), "%s/.local/bin/aimee", home);
   return path;
}

/* The agent host spawns `aimee mcp-serve` itself, with an environment of its own
 * choosing, and the generated config is the only place we can state what the
 * server needs. Where AIMEE_HOME is what locates the config -- every
 * containerised or managed-server install -- leaving it out means the server
 * starts, cannot reach aimee-server, and answers tools/list with an EMPTY list.
 * The agent is offered no tools at all and falls back to grep, which looks
 * exactly like deciding the index was not worth calling. Measured on a
 * container install: 18 tools with AIMEE_HOME present, 0 without it, whatever
 * HOME is set to.
 *
 * Only when the operator set it explicitly: with AIMEE_HOME unset the default
 * resolution already works, and pinning a value they never chose would freeze
 * this machine's layout into a config that may be copied elsewhere. */
static const char *explicit_aimee_home(void)
{
   const char *home = getenv("AIMEE_HOME");
   return (home && *home) ? home : NULL;
}

static int format_mcp_json(char *buf, size_t cap, const char *aimee_bin, const char *tool_allowlist)
{
   const char *aimee_home = explicit_aimee_home();
   if (!buf || cap == 0)
      return -1;

   cJSON *root = cJSON_CreateObject();
   cJSON *servers = root ? cJSON_AddObjectToObject(root, "mcpServers") : NULL;
   cJSON *server = servers ? cJSON_AddObjectToObject(servers, "aimee") : NULL;
   cJSON *args = server ? cJSON_AddArrayToObject(server, "args") : NULL;
   if (!root || !servers || !server || !args ||
       !cJSON_AddStringToObject(server, "command", aimee_bin ? aimee_bin : "aimee") ||
       !cJSON_AddItemToArray(args, cJSON_CreateString("mcp-serve")))
   {
      cJSON_Delete(root);
      return -1;
   }
   if ((aimee_home && aimee_home[0]) || (tool_allowlist && tool_allowlist[0]))
   {
      cJSON *env = cJSON_AddObjectToObject(server, "env");
      if (!env ||
          (aimee_home && aimee_home[0] &&
           !cJSON_AddStringToObject(env, "AIMEE_HOME", aimee_home)) ||
          (tool_allowlist && tool_allowlist[0] &&
           !cJSON_AddStringToObject(env, "AIMEE_MCP_TOOL_ALLOWLIST", tool_allowlist)))
      {
         cJSON_Delete(root);
         return -1;
      }
   }
   char *json = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json)
      return -1;
   int n = snprintf(buf, cap, "%s\n", json);
   free(json);
   return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static cJSON *create_aimee_mcp_server(const char *aimee_bin)
{
   cJSON *aimee_server = cJSON_CreateObject();
   if (!aimee_server)
      return NULL;
   cJSON_AddStringToObject(aimee_server, "command", aimee_bin ? aimee_bin : "aimee");
   cJSON *a = cJSON_CreateArray();
   cJSON_AddItemToArray(a, cJSON_CreateString("mcp-serve"));
   cJSON_AddItemToObject(aimee_server, "args", a);
   const char *aimee_home = explicit_aimee_home();
   if (aimee_home)
   {
      cJSON *env = cJSON_CreateObject();
      if (env)
      {
         cJSON_AddStringToObject(env, "AIMEE_HOME", aimee_home);
         cJSON_AddItemToObject(aimee_server, "env", env);
      }
   }
   return aimee_server;
}

static cJSON *build_marketplace_root(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "name", "local");
   cJSON *iface = cJSON_AddObjectToObject(root, "interface");
   cJSON_AddStringToObject(iface, "displayName", "Local Plugins");
   cJSON_AddItemToObject(root, "plugins", cJSON_CreateArray());
   return root;
}

static cJSON *build_aimee_plugin_entry(void)
{
   cJSON *entry = cJSON_CreateObject();
   cJSON_AddStringToObject(entry, "name", "aimee");
   cJSON *source = cJSON_AddObjectToObject(entry, "source");
   cJSON_AddStringToObject(source, "source", "local");
   cJSON_AddStringToObject(source, "path", "./plugins/aimee");
   cJSON *policy = cJSON_AddObjectToObject(entry, "policy");
   cJSON_AddStringToObject(policy, "installation", "INSTALLED_BY_DEFAULT");
   cJSON_AddStringToObject(policy, "authentication", "ON_USE");
   cJSON_AddStringToObject(entry, "category", "Coding");
   return entry;
}

/* In MCP-first mixed mode the CLI registration is a narrow compatibility
 * surface for commands that have no MCP representation. Keeping the generated
 * skill exact prevents its instructions from re-advertising dual-surface
 * capabilities through shell as well. */
static int format_codex_cli_skill(char *buf, size_t cap, const char *cli_only,
                                  const char *aimee_bin)
{
   if (!buf || cap == 0)
      return -1;
   const char *cli = aimee_bin && aimee_bin[0] ? aimee_bin : "aimee";
   if (!cli_only || !cli_only[0])
   {
      int n = snprintf(buf, cap,
                       "---\n"
                       "name: aimee\n"
                       "description: Use Aimee's registered CLI for repository memory, indexed "
                       "lookup, blast-radius analysis, and delegated work.\n"
                       "---\n\n"
                       "# Aimee CLI\n\n"
                       "The registered executable is `%s`; do not assume `aimee` is on PATH. "
                       "REQUIRED FIRST STEP: before any repository read, search, edit, build, "
                       "or test, run `%s index investigate \"<plain-language summary of the "
                       "task>\"`. If it reports unavailable or has no answer, use `%s index "
                       "hybrid \"<phrase>\"` to locate conceptually related code. Use `%s index "
                       "find <symbol>`, `%s index callers <symbol>`, `%s index "
                       "blast-radius <file>`, `%s index span <file> <start> <end>`, and `%s "
                       "memory search <terms>` for targeted follow-up. Chain independent Aimee "
                       "commands with `&&` in one shell call.\n\n"
                       "For repairs, preserve the existing success-path contract and inspect "
                       "confirmed matching production call sites before completing. Treat a "
                       "production instance you independently confirm as the same defect as part "
                       "of the repair unless the user explicitly excludes it. Do not add unrelated "
                       "preconditions while fixing a validation defect.\n\n"
                       "Use ordinary shell commands for builds, tests, and edits. If the exact "
                       "registered executable is unavailable, report the registration failure "
                       "instead of inventing an MCP tool name.\n",
                       cli, cli, cli, cli, cli, cli, cli, cli);
      return n >= 0 && (size_t)n < cap ? 0 : -1;
   }

   int n = snprintf(buf, cap,
                    "---\n"
                    "name: aimee\n"
                    "description: Use projected Aimee CLI-only module commands.\n"
                    "---\n\n"
                    "# Aimee CLI-only commands\n\n"
                    "MCP is the preferred Aimee surface. Use the shell only for these "
                    "capabilities, which have no MCP representation:\n\n");
   size_t used = n > 0 ? (size_t)n : 0;
   if (used >= cap)
      return -1;
   for (const char *p = cli_only; *p && used + 1 < cap;)
   {
      const char *end = strchr(p, ',');
      size_t len = end ? (size_t)(end - p) : strlen(p);
      n = snprintf(buf + used, cap - used, "- `%s %.*s`\n", cli, (int)len, p);
      if (n < 0 || (size_t)n >= cap - used)
         return -1;
      used += (size_t)n;
      if (!end)
         break;
      p = end + 1;
   }
   if (used + 1 < cap)
   {
      n = snprintf(buf + used, cap - used,
                   "\nUse MCP for every other Aimee capability; do not invoke its dual-surface "
                   "CLI equivalent.\n");
      if (n < 0 || (size_t)n >= cap - used)
         return -1;
   }
   return 0;
}

/* Codex PreToolUse registration. Persona delivery happens at shared model
 * ingress, not through a client lifecycle hook. aimee already HAS the guard -- `aimee hooks`
 * implements the full wire contract (permissionDecision / permissionDecisionReason,
 * and updatedInput where the client supports it), and require_aimee_git is ON by
 * default with a deny that names git_status / git_log / git_diff_summary and the
 * rest. It simply never ran under codex, because this plugin shipped no hooks at
 * all: only .mcp.json and the skill.
 *
 * Observed consequence in ordinary coding sessions was repeated shell `git` use
 * and no calls to the already-registered Aimee git route. The rule was written,
 * defaulted on, and left unwired.
 *
 * Codex does not honour updatedInput on PreToolUse, so the guard's codex path
 * denies with an instruction to retry through the tool. That costs one turn and
 * redirects the remaining ones. */
static const char *codex_hooks_json(const char *aimee_bin, const char *transport)
{
   static char buf[1536];
   snprintf(buf, sizeof(buf),
            "{\n"
            "  \"hooks\": {\n"
            "    \"PreToolUse\": [\n"
            "      {\n"
            "        \"hooks\": [\n"
            "          {\n"
            "            \"type\": \"command\",\n"
            /* `hooks` alone exits with "hooks requires 'pre' or 'post'" and codex then
             * allows the tool. The subcommand is the whole difference between a
             * registered hook and an enforcing one. */
            "            \"command\": \"AIMEE_HOOK_CLIENT=codex "
            "AIMEE_HOOK_TRANSPORT=%s AIMEE_CLI_PATH=%s %s hooks pre\",\n"
            "            \"timeout\": 10\n"
            "          }\n"
            "        ]\n"
            "      }\n"
            "    ]\n"
            "  }\n"
            "}\n",
            transport && strcmp(transport, "mcp") == 0 ? "mcp" : "cli",
            aimee_bin && aimee_bin[0] ? aimee_bin : "aimee",
            aimee_bin && aimee_bin[0] ? aimee_bin : "aimee");
   return buf;
}

static void ensure_codex_marketplace(const char *path)
{
   cJSON *root = NULL;
   FILE *fp = fopen(path, "r");
   if (fp)
   {
      fseek(fp, 0, SEEK_END);
      long sz = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      if (sz >= 0 && sz < (long)(1 << 20))
      {
         char *buf = malloc((size_t)sz + 1);
         if (buf)
         {
            if (fread(buf, 1, (size_t)sz, fp) == (size_t)sz)
            {
               buf[sz] = '\0';
               root = cJSON_Parse(buf);
            }
            free(buf);
         }
      }
      fclose(fp);
   }

   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = build_marketplace_root();
   }

   cJSON *plugins = cJSON_GetObjectItemCaseSensitive(root, "plugins");
   if (!cJSON_IsArray(plugins))
   {
      if (plugins)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "plugins");
      plugins = cJSON_CreateArray();
      cJSON_AddItemToObject(root, "plugins", plugins);
   }

   int replaced = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, plugins)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, "aimee") == 0)
      {
         cJSON *entry = build_aimee_plugin_entry();
         cJSON_ReplaceItemViaPointer(plugins, item, entry);
         replaced = 1;
         break;
      }
   }
   if (!replaced)
      cJSON_AddItemToArray(plugins, build_aimee_plugin_entry());

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_codex_plugin_enabled(const char *path)
{
   const char *section = "[plugins.\"aimee@local\"]";
   const char *enabled_true = "enabled = true";

   FILE *fp = fopen(path, "r");
   if (!fp)
   {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s\n%s\n", section, enabled_true);
      write_text_file(path, buf, 0600);
      return;
   }

   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0 || sz >= (long)(1 << 20))
   {
      fclose(fp);
      return;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';

   char *section_pos = strstr(buf, section);
   if (!section_pos)
   {
      size_t len = strlen(buf);
      int needs_nl = (len > 0 && buf[len - 1] != '\n');
      size_t extra = (needs_nl ? 1u : 0u) + (len > 0 ? 1u : 0u) + strlen(section) + 1u +
                     strlen(enabled_true) + 1u;
      char *out = malloc(len + extra + 1u);
      if (out)
      {
         size_t pos = 0;
         memcpy(out + pos, buf, len);
         pos += len;
         if (needs_nl)
            out[pos++] = '\n';
         if (len > 0)
            out[pos++] = '\n';
         memcpy(out + pos, section, strlen(section));
         pos += strlen(section);
         out[pos++] = '\n';
         memcpy(out + pos, enabled_true, strlen(enabled_true));
         pos += strlen(enabled_true);
         out[pos++] = '\n';
         out[pos] = '\0';
         write_text_file(path, out, 0600);
         free(out);
      }
      free(buf);
      return;
   }

   char *next_section = strstr(section_pos + strlen(section), "\n[");
   size_t section_len = next_section ? (size_t)(next_section - section_pos) : strlen(section_pos);
   char *enabled_pos = strstr(section_pos, enabled_true);
   if (enabled_pos && (size_t)(enabled_pos - section_pos) < section_len)
   {
      free(buf);
      return;
   }

   char *enabled_false = strstr(section_pos, "enabled = false");
   if (enabled_false && (size_t)(enabled_false - section_pos) < section_len)
   {
      size_t prefix_len = (size_t)(enabled_false - buf);
      size_t suffix_off = prefix_len + strlen("enabled = false");
      size_t suffix_len = strlen(buf + suffix_off);
      char *out = malloc(prefix_len + strlen(enabled_true) + suffix_len + 1u);
      if (out)
      {
         memcpy(out, buf, prefix_len);
         memcpy(out + prefix_len, enabled_true, strlen(enabled_true));
         memcpy(out + prefix_len + strlen(enabled_true), buf + suffix_off, suffix_len + 1u);
         write_text_file(path, out, 0600);
         free(out);
      }
      free(buf);
      return;
   }

   char *line_end = strchr(section_pos, '\n');
   if (!line_end)
   {
      free(buf);
      return;
   }

   size_t insert_off = (size_t)(line_end - buf) + 1u;
   size_t len = strlen(buf);
   size_t insert_len = strlen(enabled_true) + 1u;
   char *out = malloc(len + insert_len + 1u);
   if (out)
   {
      memcpy(out, buf, insert_off);
      memcpy(out + insert_off, enabled_true, strlen(enabled_true));
      out[insert_off + strlen(enabled_true)] = '\n';
      memcpy(out + insert_off + insert_len, buf + insert_off, len - insert_off + 1u);
      write_text_file(path, out, 0600);
      free(out);
   }
   free(buf);
}

static int codex_project_header(const char *project_root, char *out, size_t out_len)
{
   char escaped[MAX_PATH_LEN * 2];
   size_t pos = 0;
   for (const char *p = project_root; p && *p; p++)
   {
      if (*p == '"' || *p == '\\')
      {
         if (pos + 2 >= sizeof(escaped))
            return -1;
         escaped[pos++] = '\\';
         escaped[pos++] = *p;
      }
      else
      {
         if (pos + 1 >= sizeof(escaped))
            return -1;
         escaped[pos++] = *p;
      }
   }
   escaped[pos] = '\0';

   int n = snprintf(out, out_len, "[projects.\"%s\"]", escaped);
   return (n >= 0 && (size_t)n < out_len) ? 0 : -1;
}

static int codex_find_project_section(const char *buf, const char *header, const char **section_out,
                                      const char **section_end_out)
{
   size_t header_len = strlen(header);
   const char *p = buf;
   while ((p = strstr(p, header)) != NULL)
   {
      int at_line_start = (p == buf || p[-1] == '\n');
      char after = p[header_len];
      int header_ends = (after == '\0' || after == '\n' || after == '\r');
      if (at_line_start && header_ends)
      {
         const char *next = strstr(p + header_len, "\n[");
         *section_out = p;
         *section_end_out = next ? next : buf + strlen(buf);
         return 1;
      }
      p += header_len;
   }
   return 0;
}

static const char *codex_find_trust_line(const char *section, const char *section_end,
                                         const char **line_after_out)
{
   const char *p = section;
   while (p < section_end)
   {
      const char *line_end = memchr(p, '\n', (size_t)(section_end - p));
      if (!line_end)
         line_end = section_end;

      const char *s = p;
      while (s < line_end && isspace((unsigned char)*s))
         s++;
      const char key[] = "trust_level";
      size_t key_len = sizeof(key) - 1;
      if ((size_t)(line_end - s) >= key_len && strncmp(s, key, key_len) == 0)
      {
         const char *q = s + key_len;
         while (q < line_end && isspace((unsigned char)*q))
            q++;
         if (q < line_end && *q == '=')
         {
            if (line_after_out)
               *line_after_out = line_end < section_end ? line_end + 1 : line_end;
            return p;
         }
      }

      p = line_end < section_end ? line_end + 1 : section_end;
   }
   return NULL;
}

static int codex_line_is_trusted(const char *line, const char *line_after)
{
   size_t len = (size_t)(line_after - line);
   const char *trusted = strstr(line, "trust_level = \"trusted\"");
   return len >= strlen("trust_level = \"trusted\"") && trusted && trusted < line_after;
}

static int ensure_codex_trusted_project_in_config(const char *config_path, const char *project_root)
{
   if (!config_path || !config_path[0] || !project_root || !project_root[0])
      return -1;

   char header[MAX_PATH_LEN * 2 + 32];
   if (codex_project_header(project_root, header, sizeof(header)) != 0)
      return -1;

   dstr_t input;
   dstr_init(&input);
   if (dstr_read_file(&input, config_path) != 0)
      dstr_reset(&input);
   const char *buf = dstr_cstr(&input);

   const char *section = NULL;
   const char *section_end = NULL;
   if (!codex_find_project_section(buf, header, &section, &section_end))
   {
      dstr_t out;
      dstr_init(&out);
      size_t len = strlen(buf);
      if (len > 0)
      {
         dstr_append(&out, buf, len);
         if (buf[len - 1] != '\n')
            dstr_append_char(&out, '\n');
         dstr_append_char(&out, '\n');
      }
      dstr_appendf(&out, "%s\ntrust_level = \"trusted\"\n", header);
      int rc = write_text_file(config_path, dstr_cstr(&out), 0600);
      dstr_free(&out);
      dstr_free(&input);
      return rc;
   }

   const char *trust_after = NULL;
   const char *trust_line = codex_find_trust_line(section, section_end, &trust_after);
   if (trust_line && codex_line_is_trusted(trust_line, trust_after))
   {
      dstr_free(&input);
      return 0;
   }

   dstr_t out;
   dstr_init(&out);
   if (trust_line)
   {
      dstr_append(&out, buf, (size_t)(trust_line - buf));
      dstr_append_str(&out, "trust_level = \"trusted\"\n");
      dstr_append_str(&out, trust_after);
   }
   else
   {
      const char *header_end = strchr(section, '\n');
      const char *insert_at = header_end && header_end < section_end ? header_end + 1 : section_end;
      dstr_append(&out, buf, (size_t)(insert_at - buf));
      if (insert_at == section_end && (insert_at == buf || insert_at[-1] != '\n'))
         dstr_append_char(&out, '\n');
      dstr_append_str(&out, "trust_level = \"trusted\"\n");
      dstr_append_str(&out, insert_at);
   }

   int rc = write_text_file(config_path, dstr_cstr(&out), 0600);
   dstr_free(&out);
   dstr_free(&input);
   return rc;
}

void ensure_codex_project_trusted(const char *codex_home, const char *project_root)
{
   if (!codex_home || !codex_home[0] || !project_root || !project_root[0])
      return;

   char root[MAX_PATH_LEN];
#ifdef _WIN32
   if (_fullpath(root, project_root, sizeof(root)) == NULL)
      snprintf(root, sizeof(root), "%s", project_root);
#else
   if (realpath(project_root, root) == NULL)
      snprintf(root, sizeof(root), "%s", project_root);
#endif

   char config_path[MAX_PATH_LEN];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", codex_home);
   (void)ensure_codex_trusted_project_in_config(config_path, root);
}

static char *codex_shell_quote(const char *raw)
{
   if (!raw)
      return NULL;
   size_t len = 2; /* surrounding single quotes */
   for (const char *p = raw; *p; p++)
      len += (*p == '\'') ? 4u : 1u;
   char *out = malloc(len + 1);
   if (!out)
      return NULL;
   size_t pos = 0;
   out[pos++] = '\'';
   for (const char *p = raw; *p; p++)
   {
      if (*p == '\'')
      {
         out[pos++] = '\'';
         out[pos++] = '\\';
         out[pos++] = '\'';
         out[pos++] = '\'';
      }
      else
         out[pos++] = *p;
   }
   out[pos++] = '\'';
   out[pos] = '\0';
   return out;
}

void ensure_codex_current_project_trusted(const char *codex_home)
{
   if (!codex_home || !codex_home[0])
      return;

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      return;

   ensure_codex_project_trusted(codex_home, cwd);

   char *esc = codex_shell_quote(cwd);
   if (!esc)
      return;
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --show-toplevel 2>/dev/null", esc);
   free(esc);

   char *git_root = NULL;
   size_t git_root_len = 0;
   int rc = platform_exec_capture(cmd, &git_root, &git_root_len, 5000);
   if (rc == 0 && git_root && git_root[0])
   {
      size_t len = strlen(git_root);
      while (len > 0 && (git_root[len - 1] == '\n' || git_root[len - 1] == '\r'))
         git_root[--len] = '\0';
      if (git_root[0])
         ensure_codex_project_trusted(codex_home, git_root);
   }
   free(git_root);
}

static int format_codex_plugin_json(char *buf, size_t cap, int compat,
                                    client_tool_registration_plan_t plan,
                                    const char *cli_only_allowlist, const char *aimee_bin)
{
   const char *skills_registration =
       plan.cli ? (compat ? "  \"skills\": \"./aimee/\",\n" : "  \"skills\": \"./skills/\",\n")
                : "";
   const char *mcp_registration = plan.mcp ? "  \"mcpServers\": \"./.mcp.json\",\n" : "";
   const char *hooks_registration = compat ? "  \"hooks\": \"../hooks/codex-hooks.json\",\n"
                                           : "  \"hooks\": \"./hooks/codex-hooks.json\",\n";
   size_t prompt_cap = (cli_only_allowlist ? strlen(cli_only_allowlist) : 0) +
                       (aimee_bin ? strlen(aimee_bin) : 0) + 1024;
   char *cli_default_prompt = calloc(1, prompt_cap);
   if (!cli_default_prompt)
      return -1;
   if (plan.cli && cli_only_allowlist && cli_only_allowlist[0])
      snprintf(cli_default_prompt, prompt_cap,
               "    \"defaultPrompt\": [\n"
               "      \"MCP is Aimee's preferred surface. REQUIRED FIRST STEP: before any "
               "repository read, search, edit, build, or test, call Aimee MCP index with "
               "command `investigate` and a plain-language summary of the task. If unavailable, "
               "continue after the attempted call. The CLI is registered only for "
               "these comma-separated command suffixes: `%s`; invoke each with the registered "
               "executable `%s`. Use MCP for every other Aimee capability. For repairs, preserve "
               "the existing success-path contract and inspect confirmed matching production "
               "call sites before completing; matching instances are part of the repair unless "
               "the user explicitly excludes them.\"\n"
               "    ],\n",
               cli_only_allowlist, aimee_bin && aimee_bin[0] ? aimee_bin : "aimee");
   else if (plan.cli)
      snprintf(cli_default_prompt, prompt_cap,
               "    \"defaultPrompt\": [\n"
               "      \"Aimee's CLI is registered. REQUIRED FIRST STEP: before any repository "
               "read, search, edit, build, or test, run `%s index investigate \\\"<plain-"
               "language summary of the task>\\\"`. If unavailable, continue after the "
               "attempt. Use the same exact executable for targeted index and memory commands "
               "before broad shell searches. Do not assume `aimee` is on PATH. For repairs, "
               "preserve the existing success-path contract and inspect confirmed matching "
               "production call sites before completing; matching instances are part of the "
               "repair unless the user explicitly excludes them.\"\n"
               "    ],\n",
               aimee_bin && aimee_bin[0] ? aimee_bin : "aimee");
   else if (plan.mcp)
      snprintf(cli_default_prompt, prompt_cap,
               "    \"defaultPrompt\": [\n"
               "      \"Aimee MCP is the registered repository-intelligence surface. REQUIRED "
               "FIRST STEP: before any repository read, search, edit, build, or test, call its "
               "index capability with command `investigate` and a plain-language summary of "
               "the task. If unavailable, continue after the attempted call. For repairs, "
               "preserve the existing success-path contract and inspect confirmed matching "
               "production call sites before completing; matching instances are part of the "
               "repair unless the user explicitly excludes them.\"\n"
               "    ],\n");

   int n =
       snprintf(buf, cap,
                "{\n"
                "  \"name\": \"aimee\",\n"
                "  \"version\": \"%s\",\n"
                "  \"description\": \"Persistent memory, code search, blast-radius preview, and "
                "delegation for local coding sessions.\",\n"
                "  \"author\": {\n"
                "    \"name\": \"aimee\",\n"
                "    \"email\": \"support@example.invalid\",\n"
                "    \"url\": \"https://github.com/RakuenSoftware/aimee\"\n"
                "  },\n"
                "  \"homepage\": \"https://github.com/RakuenSoftware/aimee\",\n"
                "  \"repository\": \"https://github.com/RakuenSoftware/aimee\",\n"
                "  \"license\": \"MIT\",\n"
                "  \"keywords\": [\"memory\", \"mcp\", \"coding\", \"search\", \"delegation\"],\n"
                "%s%s%s"
                "  \"interface\": {\n"
                "    \"displayName\": \"aimee\",\n"
                "    \"shortDescription\": \"Memory, search, and delegation for Codex\",\n"
                "    \"longDescription\": \"Register Aimee's preferred command surface so Codex "
                "can search memory, inspect indexed code, preview blast radius, and delegate "
                "sub-tasks through the same local backend.\",\n"
                "    \"developerName\": \"aimee\",\n"
                "    \"category\": \"Coding\",\n"
                "    \"capabilities\": [\"Interactive\", \"Write\"],\n"
                "%s"
                "    \"websiteURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
                "    \"privacyPolicyURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
                "    \"termsOfServiceURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
                "    \"brandColor\": \"#1F6FEB\",\n"
                "    \"screenshots\": []\n"
                "  }\n"
                "}\n",
                AIMEE_VERSION, skills_registration, mcp_registration, hooks_registration,
                cli_default_prompt);
   free(cli_default_prompt);
   return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static void ensure_codex_plugin_files(const char *home,
                                      client_tool_transport_preference_t preference,
                                      const client_tool_surface_requirements_t *requirements)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   if (requirements && !requirements->complete)
      return;

   char plugin_json[MAX_PATH_LEN];
   char marketplace_plugin_json[MAX_PATH_LEN];
   char installed_plugin_json[MAX_PATH_LEN];
   char mcp_json[MAX_PATH_LEN];
   char marketplace_mcp_json[MAX_PATH_LEN];
   char installed_mcp_json[MAX_PATH_LEN];
   char compat_plugin_json[MAX_PATH_LEN];
   char marketplace_compat_plugin_json[MAX_PATH_LEN];
   char installed_compat_plugin_json[MAX_PATH_LEN];
   char skill_md[MAX_PATH_LEN];
   char marketplace_skill_md[MAX_PATH_LEN];
   char installed_skill_md[MAX_PATH_LEN];
   char compat_mcp_json[MAX_PATH_LEN];
   char marketplace_compat_mcp_json[MAX_PATH_LEN];
   char installed_compat_mcp_json[MAX_PATH_LEN];
   char marketplace[MAX_PATH_LEN];
   char config_toml[MAX_PATH_LEN];
   snprintf(plugin_json, sizeof(plugin_json), "%s/plugins/aimee/.codex-plugin/plugin.json", home);
   snprintf(marketplace_plugin_json, sizeof(marketplace_plugin_json),
            "%s/.agents/plugins/plugins/aimee/.codex-plugin/plugin.json", home);
   snprintf(installed_plugin_json, sizeof(installed_plugin_json),
            "%s/.codex/plugins/cache/local/aimee/.codex-plugin/plugin.json", home);
   snprintf(mcp_json, sizeof(mcp_json), "%s/plugins/aimee/.mcp.json", home);
   snprintf(marketplace_mcp_json, sizeof(marketplace_mcp_json),
            "%s/.agents/plugins/plugins/aimee/.mcp.json", home);
   snprintf(installed_mcp_json, sizeof(installed_mcp_json),
            "%s/.codex/plugins/cache/local/aimee/.mcp.json", home);
   snprintf(compat_plugin_json, sizeof(compat_plugin_json),
            "%s/plugins/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(marketplace_compat_plugin_json, sizeof(marketplace_compat_plugin_json),
            "%s/.agents/plugins/plugins/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(installed_compat_plugin_json, sizeof(installed_compat_plugin_json),
            "%s/.codex/plugins/cache/local/aimee/skills/.codex-plugin/plugin.json", home);
   char hooks_json[MAX_PATH_LEN];
   char marketplace_hooks_json[MAX_PATH_LEN];
   char installed_hooks_json[MAX_PATH_LEN];
   snprintf(hooks_json, sizeof(hooks_json), "%s/plugins/aimee/hooks/codex-hooks.json", home);
   snprintf(marketplace_hooks_json, sizeof(marketplace_hooks_json),
            "%s/.agents/plugins/plugins/aimee/hooks/codex-hooks.json", home);
   snprintf(installed_hooks_json, sizeof(installed_hooks_json),
            "%s/.codex/plugins/cache/local/aimee/hooks/codex-hooks.json", home);
   snprintf(skill_md, sizeof(skill_md), "%s/plugins/aimee/skills/aimee/SKILL.md", home);
   snprintf(marketplace_skill_md, sizeof(marketplace_skill_md),
            "%s/.agents/plugins/plugins/aimee/skills/aimee/SKILL.md", home);
   snprintf(installed_skill_md, sizeof(installed_skill_md),
            "%s/.codex/plugins/cache/local/aimee/skills/aimee/SKILL.md", home);
   snprintf(compat_mcp_json, sizeof(compat_mcp_json), "%s/plugins/aimee/skills/.mcp.json", home);
   snprintf(marketplace_compat_mcp_json, sizeof(marketplace_compat_mcp_json),
            "%s/.agents/plugins/plugins/aimee/skills/.mcp.json", home);
   snprintf(installed_compat_mcp_json, sizeof(installed_compat_mcp_json),
            "%s/.codex/plugins/cache/local/aimee/skills/.mcp.json", home);
   snprintf(marketplace, sizeof(marketplace), "%s/.agents/plugins/marketplace.json", home);
   snprintf(config_toml, sizeof(config_toml), "%s/.codex/config.toml", home);

   const char *cli_surface_paths[] = {skill_md, marketplace_skill_md, installed_skill_md};
   const char *mcp_surface_paths[] = {
       mcp_json,        marketplace_mcp_json,        installed_mcp_json,
       compat_mcp_json, marketplace_compat_mcp_json, installed_compat_mcp_json};
   int cli_supported = generated_surface_paths_writable(
       cli_surface_paths, sizeof(cli_surface_paths) / sizeof(cli_surface_paths[0]));
   int mcp_supported = generated_surface_paths_writable(
       mcp_surface_paths, sizeof(mcp_surface_paths) / sizeof(mcp_surface_paths[0]));
   int requires_cli_only = requirements && requirements->cli_only && requirements->cli_only[0];
   int requires_mcp_only = requirements && requirements->mcp_only && requirements->mcp_only[0];
   client_tool_registration_plan_t plan = client_tool_registration_plan(
       preference, cli_supported, mcp_supported, requires_cli_only, requires_mcp_only);
   if (!plan.cli && !plan.mcp)
   {
      fprintf(stderr, "aimee: neither CLI nor MCP can be registered for Codex\n");
      return;
   }

   const char *cli_allowlist =
       preference == CLIENT_TOOL_TRANSPORT_MCP_FIRST && plan.cli && plan.mcp && requirements
           ? requirements->cli_only
           : NULL;
   const char *mcp_allowlist =
       preference == CLIENT_TOOL_TRANSPORT_CLI_FIRST && plan.cli && plan.mcp && requirements
           ? requirements->mcp_only
           : NULL;

   size_t cli_len = cli_allowlist ? strlen(cli_allowlist) : 0;
   size_t mcp_len = mcp_allowlist ? strlen(mcp_allowlist) : 0;
   size_t bin_len = strlen(aimee_bin);
   if (cli_len > ((size_t)-1 - 4096) / 8 || bin_len > ((size_t)-1 - cli_len * 8 - 4096) / 8 ||
       mcp_len > (size_t)-1 - bin_len - MAX_PATH_LEN - 4096)
   {
      fprintf(stderr, "aimee: projected tool surface is too large to register safely\n");
      return;
   }
   size_t plugin_cap = cli_len + bin_len + 4096;
   size_t skill_cap = cli_len * 8 + bin_len * 8 + 4096;
   size_t mcp_cap = mcp_len + bin_len + MAX_PATH_LEN + 4096;
   char *plugin_buf = malloc(plugin_cap);
   char *compat_plugin_buf = malloc(plugin_cap);
   char *skill = plan.cli ? malloc(skill_cap) : NULL;
   char *mcp_buf = malloc(mcp_cap);
   if (!plugin_buf || !compat_plugin_buf || (plan.cli && !skill) || !mcp_buf)
   {
      fprintf(stderr, "aimee: unable to serialize the complete projected tool surface; keeping the "
                      "existing client registration\n");
      free(plugin_buf);
      free(compat_plugin_buf);
      free(skill);
      free(mcp_buf);
      return;
   }
   if (format_codex_plugin_json(plugin_buf, plugin_cap, 0, plan, cli_allowlist, aimee_bin) != 0 ||
       format_codex_plugin_json(compat_plugin_buf, plugin_cap, 1, plan, cli_allowlist, aimee_bin) !=
           0 ||
       format_mcp_json(mcp_buf, mcp_cap, aimee_bin, mcp_allowlist) != 0 ||
       (plan.cli && format_codex_cli_skill(skill, skill_cap, cli_allowlist, aimee_bin) != 0))
   {
      fprintf(stderr, "aimee: unable to serialize the complete projected tool surface; keeping the "
                      "existing client registration\n");
      free(plugin_buf);
      free(compat_plugin_buf);
      free(skill);
      free(mcp_buf);
      return;
   }

   int registration_error = 0;
   registration_error |= write_text_file(plugin_json, plugin_buf, 0644) != 0;
   registration_error |= write_text_file(marketplace_plugin_json, plugin_buf, 0644) != 0;
   registration_error |= write_text_file(installed_plugin_json, plugin_buf, 0644) != 0;
   registration_error |= sync_generated_surface_file(mcp_json, mcp_buf, plan.mcp) != 0;
   registration_error |= sync_generated_surface_file(marketplace_mcp_json, mcp_buf, plan.mcp) != 0;
   registration_error |= sync_generated_surface_file(installed_mcp_json, mcp_buf, plan.mcp) != 0;
   registration_error |= write_text_file(compat_plugin_json, compat_plugin_buf, 0644) != 0;
   registration_error |=
       write_text_file(marketplace_compat_plugin_json, compat_plugin_buf, 0644) != 0;
   registration_error |=
       write_text_file(installed_compat_plugin_json, compat_plugin_buf, 0644) != 0;
   registration_error |= sync_generated_surface_file(compat_mcp_json, mcp_buf, plan.mcp) != 0;
   registration_error |=
       sync_generated_surface_file(marketplace_compat_mcp_json, mcp_buf, plan.mcp) != 0;
   registration_error |=
       sync_generated_surface_file(installed_compat_mcp_json, mcp_buf, plan.mcp) != 0;
   registration_error |= sync_generated_surface_file(skill_md, skill, plan.cli) != 0;
   registration_error |= sync_generated_surface_file(marketplace_skill_md, skill, plan.cli) != 0;
   registration_error |= sync_generated_surface_file(installed_skill_md, skill, plan.cli) != 0;
   free(plugin_buf);
   free(compat_plugin_buf);
   free(skill);
   free(mcp_buf);
   if (registration_error)
   {
      fprintf(stderr,
              "aimee: unable to apply the selected Codex tool transport; registration may be "
              "incomplete and will be retried on the next invocation\n");
      return;
   }
   const char *effective_transport =
       plan.mcp && (preference == CLIENT_TOOL_TRANSPORT_MCP_FIRST || !plan.cli) ? "mcp" : "cli";
   const char *hooks_buf = codex_hooks_json(aimee_bin, effective_transport);
   write_text_file(hooks_json, hooks_buf, 0644);
   write_text_file(marketplace_hooks_json, hooks_buf, 0644);
   write_text_file(installed_hooks_json, hooks_buf, 0644);
   ensure_codex_marketplace(marketplace);
   ensure_codex_plugin_enabled(config_toml);
}

/* --- Claude Code integration ---
 * Registers aimee MCP server in ~/.claude/settings.json and installs
 * custom slash commands to ~/.claude/commands/. */

static cJSON *read_json_file(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0 || sz >= (long)(1 << 20))
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   return root;
}

/* Merge Aimee into Claude Code's USER MCP registry (~/.claude.json).  Current
 * Claude releases do not read mcpServers from ~/.claude/settings.json; that
 * file is for settings/hooks.  Keeping the JSON merge separate from binary
 * discovery makes the on-disk contract directly testable. */
static void ensure_claude_code_mcp_entry(const char *config_path, const char *aimee_bin,
                                         const char *tool_allowlist)
{
   if (!config_path || !config_path[0] || !aimee_bin || !aimee_bin[0])
      return;

   cJSON *root = read_json_file(config_path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   /* Check if mcpServers.aimee already exists with the correct command */
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (cJSON_IsObject(servers))
   {
      cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
      if (cJSON_IsObject(aimee))
      {
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(aimee, "command");
         cJSON *cmd_args = cJSON_GetObjectItemCaseSensitive(aimee, "args");
         cJSON *type = cJSON_GetObjectItemCaseSensitive(aimee, "type");
         if (cJSON_IsString(type) && strcmp(type->valuestring, "stdio") == 0 &&
             cJSON_IsString(cmd) && strcmp(cmd->valuestring, aimee_bin) == 0 &&
             cJSON_IsArray(cmd_args) && cJSON_GetArraySize(cmd_args) == 1)
         {
            cJSON *arg0 = cJSON_GetArrayItem(cmd_args, 0);
            if (cJSON_IsString(arg0) && strcmp(arg0->valuestring, "mcp-serve") == 0)
            {
               if (!tool_allowlist || !tool_allowlist[0])
               {
                  cJSON_Delete(root);
                  return; /* Already configured correctly */
               }
               /* A narrowed registration is rewritten below so its allowlist
                * cannot be stale after a module projection changes. */
            }
         }
      }
   }

   /* Ensure mcpServers object exists */
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   /* Create or replace aimee entry */
   cJSON *existing = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (existing)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = create_aimee_mcp_server(aimee_bin);
   if (tool_allowlist && tool_allowlist[0])
   {
      cJSON *env = cJSON_GetObjectItemCaseSensitive(aimee_server, "env");
      if (!cJSON_IsObject(env))
         env = cJSON_AddObjectToObject(aimee_server, "env");
      if (env)
         cJSON_AddStringToObject(env, "AIMEE_MCP_TOOL_ALLOWLIST", tool_allowlist);
   }
   cJSON_AddStringToObject(aimee_server, "type", "stdio");
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(config_path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_claude_code_mcp(const char *config_path, const char *tool_allowlist)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;
   ensure_claude_code_mcp_entry(config_path, aimee_bin, tool_allowlist);
}

static void remove_claude_code_mcp(const char *config_path)
{
   cJSON *root = read_json_file(config_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return;
   }
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers) || !cJSON_GetObjectItemCaseSensitive(servers, "aimee"))
   {
      cJSON_Delete(root);
      return;
   }
   cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");
   if (cJSON_GetArraySize(servers) == 0)
      cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(config_path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

/* Remove only Aimee's obsolete settings.json registration after migrating it
 * to ~/.claude.json.  Preserve unrelated keys and any other legacy entries. */
static void remove_legacy_claude_settings_mcp(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return;
   }
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers) || !cJSON_GetObjectItemCaseSensitive(servers, "aimee"))
   {
      cJSON_Delete(root);
      return;
   }
   cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");
   if (cJSON_GetArraySize(servers) == 0)
      cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(settings_path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

/* Ensure `hooks.<event>` contains an entry running
 * `AIMEE_HOOK_CLIENT=claude <aimee> <subcommand>`, optionally scoped to
 * `matcher` (NULL = fire on every event of this type). Idempotent (keyed on the
 * subcommand substring); sets *dirty when it adds the array or the entry. Used
 * for the context-pre-injection hooks (UserPromptSubmit, PreCompact — no
 * matcher) and the attention guard (PreToolUse — matcher-scoped). */
static void ensure_aimee_event_hook(cJSON *hooks, const char *event, const char *subcommand,
                                    const char *matcher, int *dirty)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   char cmd[512];
   if (strcmp(subcommand, "attention-guard") == 0 || strcmp(subcommand, "user-prompt-submit") == 0)
   {
      const char *transport =
          client_tool_transport_preference() == CLIENT_TOOL_TRANSPORT_MCP_FIRST ? "mcp" : "cli";
      snprintf(cmd, sizeof(cmd),
               "AIMEE_HOOK_CLIENT=claude AIMEE_HOOK_TRANSPORT=%s AIMEE_CLI_PATH=%s %s %s",
               transport, aimee_bin ? aimee_bin : "aimee", aimee_bin ? aimee_bin : "aimee",
               subcommand);
   }
   else
      snprintf(cmd, sizeof(cmd), "AIMEE_HOOK_CLIENT=claude %s %s", aimee_bin ? aimee_bin : "aimee",
               subcommand);

   cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, event);
   if (!cJSON_IsArray(arr))
   {
      if (arr)
         cJSON_DeleteItemFromObjectCaseSensitive(hooks, event);
      arr = cJSON_AddArrayToObject(hooks, event);
      *dirty = 1;
   }
   for (int i = 0; i < cJSON_GetArraySize(arr); i++)
   {
      cJSON *existing_entry = cJSON_GetArrayItem(arr, i);
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(existing_entry, "hooks");
      if (!cJSON_IsArray(hook_arr))
         continue;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *cmdj = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hook_arr, j), "command");
         if (cJSON_IsString(cmdj) && strstr(cmdj->valuestring, subcommand))
         {
            /* Found this aimee hook. Re-point it if its command references a
             * different (e.g. stale or transient) binary path, so a reinstall
             * to a new location heals the hook instead of leaving it dangling. */
            if (strcmp(cmdj->valuestring, cmd) != 0)
            {
               cJSON_SetValuestring(cmdj, cmd);
               *dirty = 1;
            }
            if (matcher && matcher[0])
            {
               cJSON *existing_matcher =
                   cJSON_GetObjectItemCaseSensitive(existing_entry, "matcher");
               if (!cJSON_IsString(existing_matcher) ||
                   strcmp(existing_matcher->valuestring, matcher) != 0)
               {
                  if (existing_matcher)
                     cJSON_DeleteItemFromObjectCaseSensitive(existing_entry, "matcher");
                  cJSON_AddStringToObject(existing_entry, "matcher", matcher);
                  *dirty = 1;
               }
            }
            return;
         }
      }
   }
   cJSON *entry = cJSON_CreateObject();
   cJSON *hook_arr = cJSON_CreateArray();
   cJSON *hook = cJSON_CreateObject();
   if (entry && hook_arr && hook)
   {
      if (matcher && matcher[0])
         cJSON_AddStringToObject(entry, "matcher", matcher);
      cJSON_AddStringToObject(hook, "type", "command");
      cJSON_AddStringToObject(hook, "command", cmd);
      cJSON_AddItemToArray(hook_arr, hook);
      cJSON_AddItemToObject(entry, "hooks", hook_arr);
      cJSON_AddItemToArray(arr, entry);
      *dirty = 1;
   }
   else
   {
      cJSON_Delete(entry);
      cJSON_Delete(hook_arr);
      cJSON_Delete(hook);
   }
}

/* Remove any PreToolUse (or other event) hook entry whose command runs the given
 * aimee subcommand — the inverse of ensure_aimee_event_hook, for un-installing a
 * hook when its gate no longer holds. */
static void remove_aimee_event_hook(cJSON *hooks, const char *event, const char *subcommand,
                                    int *dirty)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, event);
   if (!cJSON_IsArray(arr))
      return;
   for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--)
   {
      cJSON *hlist = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i), "hooks");
      if (!cJSON_IsArray(hlist))
         continue;
      int match = 0;
      for (int j = 0; j < cJSON_GetArraySize(hlist); j++)
      {
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hlist, j), "command");
         if (cJSON_IsString(cmd) && strstr(cmd->valuestring, subcommand))
         {
            match = 1;
            break;
         }
      }
      if (match)
      {
         cJSON_DeleteItemFromArray(arr, i);
         *dirty = 1;
      }
   }
}

/* Ensure root.permissions.deny[] contains `tool` (creating permissions/deny as
 * needed). Idempotent; sets *dirty on any change. */
static void ensure_permissions_deny_tool(cJSON *root, const char *tool, int *dirty)
{
   cJSON *perms = cJSON_GetObjectItemCaseSensitive(root, "permissions");
   if (!cJSON_IsObject(perms))
   {
      if (perms)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "permissions");
      perms = cJSON_AddObjectToObject(root, "permissions");
      *dirty = 1;
   }
   cJSON *deny = cJSON_GetObjectItemCaseSensitive(perms, "deny");
   if (!cJSON_IsArray(deny))
   {
      if (deny)
         cJSON_DeleteItemFromObjectCaseSensitive(perms, "deny");
      deny = cJSON_AddArrayToObject(perms, "deny");
      *dirty = 1;
   }
   for (int i = 0; i < cJSON_GetArraySize(deny); i++)
   {
      cJSON *e = cJSON_GetArrayItem(deny, i);
      if (cJSON_IsString(e) && strcmp(e->valuestring, tool) == 0)
         return; /* already denied */
   }
   cJSON_AddItemToArray(deny, cJSON_CreateString(tool));
   *dirty = 1;
}

/* Remove `tool` from root.permissions.deny[] if present, leaving other entries
 * (and other permissions) intact. */
static void remove_permissions_deny_tool(cJSON *root, const char *tool, int *dirty)
{
   cJSON *perms = cJSON_GetObjectItemCaseSensitive(root, "permissions");
   if (!cJSON_IsObject(perms))
      return;
   cJSON *deny = cJSON_GetObjectItemCaseSensitive(perms, "deny");
   if (!cJSON_IsArray(deny))
      return;
   for (int i = cJSON_GetArraySize(deny) - 1; i >= 0; i--)
   {
      cJSON *e = cJSON_GetArrayItem(deny, i);
      if (cJSON_IsString(e) && strcmp(e->valuestring, tool) == 0)
      {
         cJSON_DeleteItemFromArray(deny, i);
         *dirty = 1;
      }
   }
}

/* Materialize (or tear down) the sub-agent ban in the Claude Code settings.
 * The gate is evaluated ONCE here at client setup: config `subagent_ban_enabled`
 * (default on) AND the injected delegate probe reporting usable delegates. When
 * the gate holds we install a dedicated `subagent-guard` PreToolUse hook (carries
 * the actionable "use aimee delegate" message) PLUS a static permissions.deny
 * [Task, Agent] backstop that blocks the spawn even if the hook fails to run.
 * When the gate does not hold (config opt-out or no delegates) we remove both, so
 * a config/delegate change un-installs on the next setup / session-start. A probe
 * result of "unknown" (server unreachable) leaves settings untouched — we neither
 * install nor tear down on a transient outage. */
static void ensure_subagent_ban(cJSON *root, cJSON *hooks, int *dirty)
{
   /* Config opt-out is checked FIRST through the server config contract. So
    * `subagent_ban_enabled: false` reliably tears the ban down even when the
    * server is unreachable, and we never probe when the operator has opted out. */
   if (!client_config_bool("subagent_ban_enabled", 1))
   {
      remove_aimee_event_hook(hooks, "PreToolUse", "subagent-guard", dirty);
      remove_permissions_deny_tool(root, "Task", dirty);
      remove_permissions_deny_tool(root, "Agent", dirty);
      return;
   }

   int probe = g_delegate_probe ? g_delegate_probe() : -1; /* 1 avail, 0 none, -1 unknown */
   if (probe < 0)
      return; /* delegate availability unknown (server down / no probe): leave as-is */
   if (probe == 1)
   {
      /* Matcher covers every tool client_tool_is_subagent recognizes, incl.
       * RemoteTrigger. permissions.deny lists Task+Agent (the Claude-native
       * spawns); the hook backstops the rest with the actionable message. */
      ensure_aimee_event_hook(hooks, "PreToolUse", "subagent-guard",
                              "Agent|Task|Subagent|spawn_agent|RemoteTrigger", dirty);
      ensure_permissions_deny_tool(root, "Task", dirty);
      ensure_permissions_deny_tool(root, "Agent", dirty);
   }
   else /* probe == 0: no usable delegate to redirect to -> don't ban */
   {
      remove_aimee_event_hook(hooks, "PreToolUse", "subagent-guard", dirty);
      remove_permissions_deny_tool(root, "Task", dirty);
      remove_permissions_deny_tool(root, "Agent", dirty);
   }
}

/* Ensure PostToolUse hooks include EnterWorktree|ExitWorktree so that
 * aimee's CWD tracking file gets updated when the session enters/exits
 * a worktree. Without this, MCP git tools won't follow worktree changes. */
static void ensure_claude_code_hooks(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      root = cJSON_CreateObject();
      if (!root)
         return;
   }

   int dirty = 0;
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   if (!cJSON_IsObject(hooks))
   {
      if (hooks)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "hooks");
      hooks = cJSON_AddObjectToObject(root, "hooks");
      dirty = 1;
   }

   cJSON *post = cJSON_GetObjectItemCaseSensitive(hooks, "PostToolUse");
   if (!cJSON_IsArray(post))
   {
      if (post)
         cJSON_DeleteItemFromObjectCaseSensitive(hooks, "PostToolUse");
      post = cJSON_AddArrayToObject(hooks, "PostToolUse");
      dirty = 1;
   }

   /* Find the aimee hooks post entry and check its matcher */
   int found_post_entry = 0;
   int n = cJSON_GetArraySize(post);
   for (int i = 0; i < n; i++)
   {
      cJSON *entry = cJSON_GetArrayItem(post, i);
      if (!cJSON_IsObject(entry))
         continue;
      cJSON *matcher = cJSON_GetObjectItemCaseSensitive(entry, "matcher");
      if (!cJSON_IsString(matcher))
         continue;
      /* Check if this is an aimee hooks entry */
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(entry, "hooks");
      if (!cJSON_IsArray(hook_arr))
         continue;
      int found_aimee = 0;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *h = cJSON_GetArrayItem(hook_arr, j);
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(h, "command");
         if (cJSON_IsString(cmd) && (strstr(cmd->valuestring, "aimee hooks post") ||
                                     strstr(cmd->valuestring, "aimee-client hooks post")))
         {
            found_aimee = 1;
            /* Re-point a stale/transient binary path to the resolved one, so a
             * reinstall heals this hook (mirrors ensure_aimee_event_hook). */
            const char *bin = resolved_aimee_bin_path();
            char want[512];
            snprintf(want, sizeof(want), "AIMEE_HOOK_CLIENT=claude %s hooks post",
                     bin ? bin : "aimee");
            if (strcmp(cmd->valuestring, want) != 0)
            {
               cJSON_SetValuestring(cmd, want);
               dirty = 1;
            }
         }
      }
      if (!found_aimee)
         continue;
      found_post_entry = 1;

      /* This is the aimee PostToolUse entry — ensure Enter/ExitWorktree are in matcher. */
      int has_enter = strstr(matcher->valuestring, "EnterWorktree") != NULL;
      int has_exit = strstr(matcher->valuestring, "ExitWorktree") != NULL;
      if (!has_enter || !has_exit)
      {
         char new_matcher[512];
         snprintf(new_matcher, sizeof(new_matcher), "%s%s%s", matcher->valuestring,
                  has_enter ? "" : "|EnterWorktree", has_exit ? "" : "|ExitWorktree");
         cJSON_SetValuestring(matcher, new_matcher);
         dirty = 1;
      }
   }

   if (!found_post_entry)
   {
      const char *aimee_bin = resolved_aimee_bin_path();
      char post_cmd[512];
      snprintf(post_cmd, sizeof(post_cmd), "AIMEE_HOOK_CLIENT=claude %s hooks post",
               aimee_bin ? aimee_bin : "aimee");

      cJSON *entry = cJSON_CreateObject();
      cJSON *hook_arr = cJSON_CreateArray();
      cJSON *hook = cJSON_CreateObject();
      if (entry && hook_arr && hook)
      {
         cJSON_AddStringToObject(entry, "matcher",
                                 "Edit|Write|MultiEdit|EnterWorktree|ExitWorktree");
         cJSON_AddStringToObject(hook, "type", "command");
         cJSON_AddStringToObject(hook, "command", post_cmd);
         cJSON_AddItemToArray(hook_arr, hook);
         cJSON_AddItemToObject(entry, "hooks", hook_arr);
         cJSON_AddItemToArray(post, entry);
         dirty = 1;
      }
      else
      {
         cJSON_Delete(entry);
         cJSON_Delete(hook_arr);
         cJSON_Delete(hook);
      }
   }

   /* Remove the former persona-delivery hook from existing installations.
    * Persona content is prepended at shared model ingress; leaving this entry
    * behind would make delivery client- and version-dependent again. */
   remove_aimee_event_hook(hooks, "SessionStart", "session-start", &dirty);
   /* Context pre-injection hooks: the P1 per-turn UserPromptSubmit envelope and
    * the P3 PreCompact re-prime. Both fire with no matcher and soft-fail, so
    * they never block a turn. */
   ensure_aimee_event_hook(hooks, "UserPromptSubmit", "user-prompt-submit", NULL, &dirty);
   ensure_aimee_event_hook(hooks, "PreCompact", "pre-compact", NULL, &dirty);
   /* P3 attention guard: PreToolUse hook scoped to read/edit/destructive tools;
    * accrues per-file attention and blocks hard-destructive ops on files the
    * session has actively touched. It does NOT gate sub-agent tools — that is the
    * dedicated `subagent-guard` hook installed by ensure_subagent_ban below (this
    * matcher deliberately no longer lists Task|Agent). */
   ensure_aimee_event_hook(hooks, "PreToolUse", "attention-guard",
                           "Read|Edit|Write|MultiEdit|NotebookEdit|Bash|Grep|Glob|"
                           "mcp__aimee__.*|aimee__.*",
                           &dirty);

   /* Sub-agent ban (delegate-only): gated at setup on subagent_ban_enabled AND a
    * one-shot delegate probe; installs/removes the subagent-guard hook + the
    * permissions.deny [Task, Agent] backstop accordingly. */
   ensure_subagent_ban(root, hooks, &dirty);

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(settings_path, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

/* Normalize an aimee server URL into an Anthropic base URL: Claude Code appends
 * "/v1/messages", so we want the origin without a trailing "/" or "/v1".
 * Writes into out (caller-sized). */
static void normalize_anthropic_base(const char *server_url, char *out, size_t out_len)
{
   size_t n;
   snprintf(out, out_len, "%s", server_url ? server_url : "");
   n = strlen(out);
   while (n > 0 && out[n - 1] == '/')
      out[--n] = '\0';
   if (n >= 3 && strcmp(out + n - 3, "/v1") == 0)
      out[n - 3] = '\0';
}

/* Enable or disable routing Claude Code through aimee's Anthropic Messages
 * ingress by writing (enable) or removing (disable) ANTHROPIC_BASE_URL and
 * ANTHROPIC_AUTH_TOKEN under the "env" key of ~/.claude/settings.json.
 *
 * Enabling reroutes ALL of the operator's Claude Code traffic — including any
 * live session — off Anthropic to aimee's primary model, so it is only ever
 * invoked explicitly via `aimee claude-proxy enable`. server_url is the
 * aimee-server origin (required on enable); token is the server bearer (may be
 * NULL/empty → a local placeholder is written, since Claude Code always sends
 * an auth value). Returns 0 on success, -1 on error. */
int claude_code_proxy_configure(const char *server_url, const char *token, int enable)
{
   const char *home = getenv("HOME");
   char settings_path[MAX_PATH_LEN];
   cJSON *root, *env;
   char *json;
   int rc = -1;

   if (!home || !home[0])
      return -1;
   if (enable && (!server_url || !server_url[0]))
      return -1;

   snprintf(settings_path, sizeof(settings_path), "%s/.claude/settings.json", home);
   root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      if (!enable)
         return 0; /* disabling a never-enabled proxy is a no-op success */
      root = cJSON_CreateObject();
      if (!root)
         return -1;
   }

   env = cJSON_GetObjectItemCaseSensitive(root, "env");
   if (!cJSON_IsObject(env))
   {
      if (env)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "env");
      if (!enable)
      {
         cJSON_Delete(root); /* nothing to remove */
         return 0;
      }
      env = cJSON_AddObjectToObject(root, "env");
   }

   cJSON_DeleteItemFromObjectCaseSensitive(env, "ANTHROPIC_BASE_URL");
   cJSON_DeleteItemFromObjectCaseSensitive(env, "ANTHROPIC_AUTH_TOKEN");
   if (enable)
   {
      char base[MAX_PATH_LEN];
      normalize_anthropic_base(server_url, base, sizeof(base));
      cJSON_AddStringToObject(env, "ANTHROPIC_BASE_URL", base);
      cJSON_AddStringToObject(env, "ANTHROPIC_AUTH_TOKEN",
                              (token && token[0]) ? token : "aimee-local");
   }

   json = cJSON_Print(root);
   if (json)
   {
      write_text_file(settings_path, json, 0600);
      free(json);
      rc = 0;
   }
   cJSON_Delete(root);
   return rc;
}

/* Ensure the "env" key in settings.json has required environment variables.
 * Currently sets CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR=0 so that cd
 * commands persist across Bash calls, enabling worktree workflows. */
static void ensure_claude_code_env(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return;
   }

   int dirty = 0;
   cJSON *env = cJSON_GetObjectItemCaseSensitive(root, "env");
   if (!cJSON_IsObject(env))
   {
      if (env)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "env");
      env = cJSON_AddObjectToObject(root, "env");
      dirty = 1;
   }

   cJSON *cwd_var =
       cJSON_GetObjectItemCaseSensitive(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR");
   if (!cJSON_IsString(cwd_var) || strcmp(cwd_var->valuestring, "0") != 0)
   {
      if (cwd_var)
         cJSON_DeleteItemFromObjectCaseSensitive(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR");
      cJSON_AddStringToObject(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR", "0");
      dirty = 1;
   }

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(settings_path, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

/* Ensure ~/.claude.json has hasTrustDialogAccepted=true and
 * enabledMcpjsonServers=["aimee"] for the given directory path.
 *
 * Claude Code shows a trust dialog when a session opens a directory containing
 * a .mcp.json for the first time. Delegate worktrees are spawned non-
 * interactively by aimee-server, so no user is present to accept the dialog —
 * Claude Code silently rejects the project-scoped MCP server and the session
 * starts without aimee tools. Pre-accepting trust here fixes that. */
void ensure_claude_code_trust(const char *dir)
{
   if (!dir || !dir[0])
      return;
   const char *home = getenv("HOME");
   if (!home)
      return;

   char claude_json[MAX_PATH_LEN];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", home);

   cJSON *root = read_json_file(claude_json);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return; /* Don't create .claude.json from scratch */
   }

   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   if (!cJSON_IsObject(projects))
   {
      cJSON_Delete(root);
      return;
   }

   int dirty = 0;
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, dir);
   if (!cJSON_IsObject(proj))
   {
      if (proj)
         cJSON_DeleteItemFromObjectCaseSensitive(projects, dir);
      proj = cJSON_AddObjectToObject(projects, dir);
      dirty = 1;
   }

   /* Ensure hasTrustDialogAccepted is true */
   cJSON *trust = cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted");
   if (!cJSON_IsTrue(trust))
   {
      if (trust)
         cJSON_DeleteItemFromObjectCaseSensitive(proj, "hasTrustDialogAccepted");
      cJSON_AddTrueToObject(proj, "hasTrustDialogAccepted");
      dirty = 1;
   }

   /* Ensure enabledMcpjsonServers array exists and contains "aimee" */
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   if (!cJSON_IsArray(enabled))
   {
      if (enabled)
         cJSON_DeleteItemFromObjectCaseSensitive(proj, "enabledMcpjsonServers");
      enabled = cJSON_AddArrayToObject(proj, "enabledMcpjsonServers");
      dirty = 1;
   }
   int found_aimee = 0;
   for (int i = 0; i < cJSON_GetArraySize(enabled); i++)
   {
      cJSON *item = cJSON_GetArrayItem(enabled, i);
      if (cJSON_IsString(item) && strcmp(item->valuestring, "aimee") == 0)
      {
         found_aimee = 1;
         break;
      }
   }
   if (!found_aimee)
   {
      cJSON_AddItemToArray(enabled, cJSON_CreateString("aimee"));
      dirty = 1;
   }

   /* Ensure disabledMcpjsonServers array exists (Claude Code expects it) */
   cJSON *disabled = cJSON_GetObjectItemCaseSensitive(proj, "disabledMcpjsonServers");
   if (!cJSON_IsArray(disabled))
   {
      if (disabled)
         cJSON_DeleteItemFromObjectCaseSensitive(proj, "disabledMcpjsonServers");
      cJSON_AddArrayToObject(proj, "disabledMcpjsonServers");
      dirty = 1;
   }

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(claude_json, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

static void ensure_claude_code_integration(const char *home)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char settings_path[MAX_PATH_LEN];
   snprintf(settings_path, sizeof(settings_path), "%s/.claude/settings.json", home);
   char user_config_path[MAX_PATH_LEN];
   snprintf(user_config_path, sizeof(user_config_path), "%s/.claude.json", home);
   client_tool_surface_requirements_t requirements = client_projected_surface_requirements();
   client_tool_transport_preference_t preference = client_tool_transport_preference();
   int mcp_supported = generated_surface_path_writable(user_config_path);
   client_tool_registration_plan_t plan = client_tool_registration_plan(
       preference, 1, mcp_supported, requirements.cli_only && requirements.cli_only[0],
       requirements.mcp_only && requirements.mcp_only[0]);
   const char *mcp_allowlist = preference == CLIENT_TOOL_TRANSPORT_CLI_FIRST && plan.cli && plan.mcp
                                   ? requirements.mcp_only
                                   : NULL;
   if (plan.mcp)
      ensure_claude_code_mcp(user_config_path, mcp_allowlist);
   else
      remove_claude_code_mcp(user_config_path);
   client_tool_surface_requirements_dispose(&requirements);
   remove_legacy_claude_settings_mcp(settings_path);
   ensure_claude_code_hooks(settings_path);
   ensure_claude_code_env(settings_path);
}

/* Retire only byte-identical Claude commands from the old generator. An existing
 * file may have been customized after aimee created it; absence of provenance
 * means it belongs to the user and must survive. Codex skills are no longer
 * referenced by the generated manifests, but likewise cannot be safely deleted
 * merely because they occupy a formerly generated path. */
static void retire_generated_markdown(const char *path, const char *generated)
{
   dstr_t content;
   dstr_init(&content);
   if (dstr_read_file(&content, path) == 0 && dstr_equals_cstr(&content, generated))
      (void)unlink(path);
   dstr_free(&content);
}

static void retire_client_markdown(const char *home)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof path, "%s/.claude/commands/aimee-search.md", home);
   retire_generated_markdown(
       path, "Search aimee memory for project facts, prior decisions, and stored context.\n\n"
             "Use the aimee MCP tool `search_memory` with the query: $ARGUMENTS\n\n"
             "If no query is provided, use `list_facts` to show all stored facts.\n");
   snprintf(path, sizeof path, "%s/.claude/commands/aimee-delegate.md", home);
   retire_generated_markdown(
       path, "Delegate a bounded sub-task to an aimee delegate agent.\n\n"
             "Use the aimee MCP tool `delegate` with the task: $ARGUMENTS\n\n"
             "Do not use provider-native sub-agent tools such as Claude Agent.\n\n"
             "The delegate will execute the task using the cheapest suitable model\n"
             "and return the result. Only delegate bounded, well-defined tasks.\n");
   snprintf(path, sizeof path, "%s/.claude/commands/aimee-blast-radius.md", home);
   retire_generated_markdown(
       path, "Preview the blast radius of a multi-file edit before making changes.\n\n"
             "Use the aimee MCP tool `preview_blast_radius` for: $ARGUMENTS\n\n"
             "This shows which files and symbols would be affected by the change,\n"
             "helping you understand the impact before editing.\n");
}

static void ensure_gemini_integration(const char *home)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.gemini/settings.json", home);

   cJSON *root = read_json_file(path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (aimee)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = create_aimee_mcp_server(aimee_bin);
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_copilot_integration(const char *home)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.copilot/mcp-config.json", home);

   cJSON *root = read_json_file(path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (aimee)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = create_aimee_mcp_server(aimee_bin);
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static client_tool_transport_preference_t client_tool_transport_preference(void)
{
   char value[32];
   client_config_string("client_tool_transport_preference", value, sizeof(value), "cli-first");
   return client_tool_transport_parse(value);
}

/* Whether aimee is allowed to auto-register itself into external AI-tool user
 * configs. The env var AIMEE_NO_CLIENT_INTEGRATIONS overrides the persisted
 * config: any non-empty value other than "0"/"false" forces the integrations
 * off for this run (useful for CI or a one-off install). Otherwise the
 * default-ON client_integrations_enabled config key decides. */
static int client_integrations_allowed(void)
{
   const char *env = getenv("AIMEE_NO_CLIENT_INTEGRATIONS");
   if (env && env[0] && strcmp(env, "0") != 0 && strcmp(env, "false") != 0)
      return 0;

   return client_config_bool("client_integrations_enabled", 1);
}

void ensure_client_integrations(void)
{
   if (!client_integrations_allowed())
      return;

   const char *home = platform_home_dir();
   if (!home || !home[0])
      return;

   retire_client_markdown(home);

   struct stat st;

   char codex_dir[MAX_PATH_LEN];
   snprintf(codex_dir, sizeof(codex_dir), "%s/.codex", home);
   if (stat(codex_dir, &st) == 0 && S_ISDIR(st.st_mode))
   {
      client_tool_surface_requirements_t requirements = client_projected_surface_requirements();
      ensure_codex_plugin_files(home, client_tool_transport_preference(), &requirements);
      client_tool_surface_requirements_dispose(&requirements);
   }

   char claude_dir[MAX_PATH_LEN];
   snprintf(claude_dir, sizeof(claude_dir), "%s/.claude", home);
   if (stat(claude_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_claude_code_integration(home);

   char gemini_dir[MAX_PATH_LEN];
   snprintf(gemini_dir, sizeof(gemini_dir), "%s/.gemini", home);
   if (stat(gemini_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_gemini_integration(home);

   char copilot_dir[MAX_PATH_LEN];
   snprintf(copilot_dir, sizeof(copilot_dir), "%s/.copilot", home);
   if (stat(copilot_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_copilot_integration(home);
}

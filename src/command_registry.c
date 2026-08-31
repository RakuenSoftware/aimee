/* command_registry.c: see command_registry.h.
 *
 * A flat array with linear lookup, deliberately. The table is a few hundred
 * entries registered once at startup and read on a control path that already
 * costs a JSON parse and usually an IPC round trip; a hash map here would buy
 * nothing measurable and cost a rehash story on a structure whose correctness is
 * the entire point. If lookup ever shows up in a profile, sort it once after
 * registration closes and binary-search -- the API does not change.
 */
#include "command_registry.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Grown geometrically; the registry is append-only until reset. */
static aimee_command_t *g_cmds;
static size_t g_count;
static size_t g_cap;

typedef struct
{
   const char *name;
   unsigned surface;
   const char *module;
} agent_surface_t;

static unsigned long g_generation;

static agent_surface_t *g_agent_surfaces;
static size_t g_agent_surface_count;
static size_t g_agent_surface_cap;

static int cmd_name_ok(const char *s)
{
   /* Lowercase, digits and underscore. Both spellings the surfaces build from it
    * -- `aimee <group> <verb>` and "<group>.<verb>" -- have to be unambiguous, so
    * a dot or a space in a component would make the dotted form parse two ways. */
   if (!s || !s[0])
      return 0;
   for (const char *p = s; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
         return 0;
   return 1;
}

int aimee_command_register(const aimee_command_t *cmd)
{
   if (!cmd || !cmd_name_ok(cmd->group) || !cmd_name_ok(cmd->verb) || !cmd->fn)
   {
      LOG_WARN("commands", "refusing malformed registration (group=%s verb=%s)",
               cmd && cmd->group ? cmd->group : "(null)", cmd && cmd->verb ? cmd->verb : "(null)");
      return -1;
   }
   if (!cmd->surfaces)
   {
      /* A command on no surface is unreachable. That is never what was meant, and
       * silently keeping it would put an entry in the table that answers nothing. */
      LOG_WARN("commands", "refusing %s.%s: registered on no surface", cmd->group, cmd->verb);
      return -1;
   }
   const aimee_command_t *dup = aimee_command_find(cmd->group, cmd->verb);
   if (dup)
   {
      LOG_WARN("commands", "refusing duplicate %s.%s (already registered by %s)", cmd->group,
               cmd->verb, dup->module ? dup->module : "(unknown)");
      return -1;
   }
   if ((cmd->surfaces & AIMEE_SURFACE_MCP) && !(cmd->surfaces & AIMEE_SURFACE_CLI))
   {
      LOG_WARN("commands", "refusing %s.%s: MCP capabilities require a CLI route", cmd->group,
               cmd->verb);
      return -1;
   }
   if (g_count == g_cap)
   {
      size_t cap = g_cap ? g_cap * 2 : 64;
      aimee_command_t *grown = realloc(g_cmds, cap * sizeof *grown);
      if (!grown)
         return -1;
      g_cmds = grown;
      g_cap = cap;
   }
   g_cmds[g_count++] = *cmd;
   g_generation++;
   return 0;
}

int aimee_command_register_many(const aimee_command_t *cmds, size_t n)
{
   if (!cmds)
      return -1;
   for (size_t i = 0; i < n; i++)
      if (aimee_command_register(&cmds[i]) != 0)
         return -1;
   return 0;
}

size_t aimee_command_unregister_module(const char *module)
{
   if (!module || !module[0])
      return 0;
   size_t kept = 0;
   size_t removed = 0;
   for (size_t i = 0; i < g_count; i++)
   {
      if (g_cmds[i].module && strcmp(g_cmds[i].module, module) == 0)
      {
         removed++;
         continue;
      }
      if (kept != i)
         g_cmds[kept] = g_cmds[i];
      kept++;
   }
   g_count = kept;
   if (removed)
   {
      g_generation++;
      LOG_INFO("commands", "withdrew %zu command(s) registered by %s", removed, module);
   }
   return removed;
}

unsigned long aimee_command_registry_generation(void)
{
   return g_generation;
}

const aimee_command_t *aimee_command_find(const char *group, const char *verb)
{
   if (!group || !verb)
      return NULL;
   for (size_t i = 0; i < g_count; i++)
      if (strcmp(g_cmds[i].group, group) == 0 && strcmp(g_cmds[i].verb, verb) == 0)
         return &g_cmds[i];
   return NULL;
}

const aimee_command_t *aimee_command_find_method(const char *method)
{
   if (!method)
      return NULL;
   const char *dot = strchr(method, '.');
   if (!dot || dot == method || !dot[1])
      return NULL;
   size_t glen = (size_t)(dot - method);
   for (size_t i = 0; i < g_count; i++)
      if (strncmp(g_cmds[i].group, method, glen) == 0 && g_cmds[i].group[glen] == '\0' &&
          strcmp(g_cmds[i].verb, dot + 1) == 0)
         return &g_cmds[i];
   return NULL;
}

size_t aimee_command_count(void)
{
   return g_count;
}

const aimee_command_t *aimee_command_at(size_t index)
{
   return index < g_count ? &g_cmds[index] : NULL;
}

int aimee_agent_surface_register(const char *name, unsigned surface, const char *module)
{
   if (!name || !name[0] || (surface != AIMEE_SURFACE_CLI && surface != AIMEE_SURFACE_MCP))
   {
      LOG_WARN("commands", "refusing malformed external surface registration (name=%s)",
               name ? name : "(null)");
      return -1;
   }
   for (const char *p = name; *p; p++)
      if ((unsigned char)*p < 0x20 || *p == 0x7f)
         return -1;
   for (size_t i = 0; i < g_agent_surface_count; i++)
      if (strcmp(g_agent_surfaces[i].name, name) == 0)
      {
         LOG_WARN("commands", "refusing duplicate external surface %s (already registered by %s)",
                  name, g_agent_surfaces[i].module ? g_agent_surfaces[i].module : "(unknown)");
         return -1;
      }
   if (g_agent_surface_count == g_agent_surface_cap)
   {
      size_t cap = g_agent_surface_cap ? g_agent_surface_cap * 2 : 16;
      agent_surface_t *grown = realloc(g_agent_surfaces, cap * sizeof *grown);
      if (!grown)
         return -1;
      g_agent_surfaces = grown;
      g_agent_surface_cap = cap;
   }
   g_agent_surfaces[g_agent_surface_count++] =
       (agent_surface_t){.name = name, .surface = surface, .module = module};
   return 0;
}

struct cJSON *aimee_command_agent_surfaces_json(void)
{
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   cJSON *cli_only = cJSON_AddArrayToObject(out, "cli_only");
   cJSON *mcp_only = cJSON_AddArrayToObject(out, "mcp_only");
   if (!cli_only || !mcp_only)
   {
      cJSON_Delete(out);
      return NULL;
   }

   for (size_t i = 0; i < g_count; i++)
   {
      const aimee_command_t *cmd = &g_cmds[i];
      int cli = (cmd->surfaces & AIMEE_SURFACE_CLI) != 0;
      int mcp = (cmd->surfaces & AIMEE_SURFACE_MCP) != 0;
      if (cli && !mcp)
      {
         char name[160];
         snprintf(name, sizeof(name), "%s %s", cmd->group, cmd->verb);
         cJSON_AddItemToArray(cli_only, cJSON_CreateString(name));
      }
      /* mcp && !cli cannot enter the internal command table. */
   }
   for (size_t i = 0; i < g_agent_surface_count; i++)
   {
      cJSON *dst = g_agent_surfaces[i].surface == AIMEE_SURFACE_CLI ? cli_only : mcp_only;
      cJSON_AddItemToArray(dst, cJSON_CreateString(g_agent_surfaces[i].name));
   }
   return out;
}

void aimee_command_registry_reset(void)
{
   free(g_cmds);
   g_cmds = NULL;
   g_count = 0;
   g_cap = 0;
   free(g_agent_surfaces);
   g_agent_surfaces = NULL;
   g_agent_surface_count = 0;
   g_agent_surface_cap = 0;
   /* Bump rather than zero: a cached view taken before the reset must not
    * compare equal to one taken after it. */
   g_generation++;
}

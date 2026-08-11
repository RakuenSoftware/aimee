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
#include "log.h"
#include <stdlib.h>
#include <string.h>

/* Grown geometrically; the registry is append-only until reset. */
static aimee_command_t *g_cmds;
static size_t g_count;
static size_t g_cap;

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

void aimee_command_registry_reset(void)
{
   free(g_cmds);
   g_cmds = NULL;
   g_count = 0;
   g_cap = 0;
}

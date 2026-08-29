/* cli_command_defs.c: the command catalogue the thin client renders.
 *
 * WHY THIS IS SERVER-SIDE. The client used to carry this table (src/cli_help_data.h)
 * and generate docs/gen/cli-commands.md from it, which made the CLIENT the source
 * of truth for what the server can do. A client one release behind advertised the
 * wrong command set and had to be rebuilt to learn a new one -- the same coupling
 * that made a client 324 commits behind its server unable to reach anything added
 * in between.
 *
 * The client is a presentation surface. It knows how to reach a server and how to
 * render what comes back; the catalogue is the server's to state.
 *
 * NOT the same thing as the route map. The route map (rh_cli_manifest) says how to
 * ADDRESS a method. This says what a human is shown: names, one-line summaries,
 * the tier that decides whether `aimee` lists it by default, and the subcommand
 * text. Both ride the same manifest because both are answers to "what can I do?".
 */
#include "cli_command_defs.h"

#include "cJSON.h"

#include <stddef.h>
#include <string.h>

typedef struct
{
   const char *name;
   const char *help;
   aimee_cmd_tier_t tier;
   /* Listed only under `aimee help --all`. Not secrecy: these are commands whose
    * audience is narrow enough that showing them by default costs every other
    * reader attention. */
   int hidden_default;
   const char *subcommands;
} cli_command_def_t;

static const cli_command_def_t g_cli_commands[] = {
/* Rows live in cli_command_defs_data.h to keep this TU under the source
 * line-count limit (same pattern as agent_help_data.h). */
#include "cli_command_defs_data.h"
};

static const char *tier_name(aimee_cmd_tier_t t)
{
   switch (t)
   {
   case AIMEE_CMD_TIER_CORE:
      return "core";
   case AIMEE_CMD_TIER_ADVANCED:
      return "advanced";
   case AIMEE_CMD_TIER_ADMIN:
      return "admin";
   }
   return "core";
}

/* The display catalogue's `subcommands` field also carries flag and positional
 * usage text, so it cannot by itself tell the client whether an empty argv is
 * valid. Keep that routing semantic explicit in the served manifest. */
static int command_has_bare_default(const char *name)
{
   return name && (strcmp(name, "presence") == 0 || strcmp(name, "primary") == 0);
}

size_t cli_command_defs_count(void)
{
   return sizeof(g_cli_commands) / sizeof(g_cli_commands[0]);
}

cJSON *cli_command_defs_to_json(void)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   for (size_t i = 0; i < cli_command_defs_count(); i++)
   {
      const cli_command_def_t *c = &g_cli_commands[i];
      if (!c->name)
         continue;
      cJSON *row = cJSON_CreateObject();
      if (!row)
         continue;
      cJSON_AddStringToObject(row, "name", c->name);
      cJSON_AddStringToObject(row, "summary", c->help ? c->help : "");
      cJSON_AddStringToObject(row, "tier", tier_name(c->tier));
      /* Emitted only when true: absent means "listed by default", which is the
       * common case, and a false on every row is noise on the wire. */
      if (c->hidden_default)
         cJSON_AddBoolToObject(row, "hidden_default", 1);
      if (c->subcommands && c->subcommands[0])
         cJSON_AddStringToObject(row, "subcommands", c->subcommands);
      if (command_has_bare_default(c->name))
         cJSON_AddBoolToObject(row, "bare_default", 1);
      cJSON_AddItemToArray(arr, row);
   }
   return arr;
}

/* cmd_capabilities.c: generated CLI capabilities reference text */
#include "aimee.h"
#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build the aimee capabilities reference text. Caller owns the returned string. */
char *build_capabilities_text(void)
{
   size_t cap = 4096;
   char *buf = malloc(cap);
   size_t pos = 0;

   pos += (size_t)snprintf(buf + pos, cap - pos,
                           "# aimee Commands\n"
                           "Use `aimee <command>` for all operations.\n"
                           "Common shortcuts: `aimee use <provider>`, `aimee provider [name]`, "
                           "`aimee verify on|off`.\n\n");

   static const struct
   {
      cmd_tier_t tier;
      const char *label;
   } tiers[] = {
       {CMD_TIER_CORE, "Core"},
       {CMD_TIER_ADVANCED, "Advanced"},
       {CMD_TIER_ADMIN, "Admin"},
   };

   for (int t = 0; t < 3 && pos < cap - 256; t++)
   {
      pos += (size_t)snprintf(buf + pos, cap - pos, "\n## %s\n", tiers[t].label);
      for (int i = 0; commands[i].name != NULL && pos < cap - 256; i++)
      {
         if (commands[i].tier != tiers[t].tier)
            continue;
         if (command_is_hidden_default(commands[i].name) ||
             strcmp(commands[i].name, "version") == 0)
            continue;
         pos += (size_t)snprintf(buf + pos, cap - pos, "- `aimee %s` -- %s\n", commands[i].name,
                                 commands[i].help);
      }
   }
   pos += (size_t)snprintf(buf + pos, cap - pos, "\n");

   struct
   {
      const char *parent;
      const subcmd_t *(*getter)(void);
   } sub_tables[] = {
       {"index", get_index_subcmds},
       {"wm", get_wm_subcmds},
       {"agent", get_agent_subcmds},
   };

   for (size_t t = 0; t < sizeof(sub_tables) / sizeof(sub_tables[0]) && pos < cap - 256; t++)
   {
      const subcmd_t *subs = sub_tables[t].getter();
      pos += (size_t)snprintf(buf + pos, cap - pos, "## aimee %s\n", sub_tables[t].parent);
      for (int j = 0; subs[j].name && pos < cap - 128; j++)
      {
         if (subs[j].help)
            pos += (size_t)snprintf(buf + pos, cap - pos, "- `%s` -- %s\n", subs[j].name,
                                    subs[j].help);
         else
            pos += (size_t)snprintf(buf + pos, cap - pos, "- `%s`\n", subs[j].name);
      }
      pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
   }

   pos += (size_t)snprintf(
       buf + pos, cap - pos,
       "## Usage Notes\n"
       "- Use `aimee git status`, `aimee git log`, `aimee git diff` for compact git info.\n"
       "- Use `aimee git pr <list|view|create|edit|checks|watch>` for GitHub PR/issue operations.\n"
       "- Use `aimee delegate <role> \"prompt\"` for deploy, validate, test, "
       "diagnose, execute tasks when sub-agents are configured.\n"
       "- Record feedback with `aimee + \"what went well\"` or `aimee - \"what went wrong\"`.\n"
       "- Add `--json` to any command for machine-readable output.\n\n");

   buf[pos] = '\0';
   return buf;
}

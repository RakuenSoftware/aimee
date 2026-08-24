/* cmd_util.c: command handler utilities (subcommand dispatch) */
#include "aimee.h"
#include "commands.h"
#include "config.h"
#include "db1_client/db1.h"

/* --- cmd_require_db1 --- */

/* Load config and open DB1, aborting with `errmsg` on failure. Folds the
 * config_t + config_load + db1_init + fatal prolog the CLI command handlers
 * otherwise repeat verbatim; callers that also need the config keep their own
 * config_load. */
void cmd_require_db1(const char *errmsg)
{
   if (!db1_store_ready())
      fatal("%s", errmsg);
}

/* --- subcmd_usage --- */

void subcmd_usage(const char *parent, const subcmd_t *table)
{
   fprintf(stderr, "Usage: aimee %s <subcommand> [args...]\n\nSubcommands:\n", parent);
   for (int i = 0; table[i].name; i++)
   {
      if (table[i].help)
         fprintf(stderr, "  %-16s %s\n", table[i].name, table[i].help);
      else
         fprintf(stderr, "  %s\n", table[i].name);
   }
}

/* --- subcmd_dispatch --- */

int subcmd_dispatch(const subcmd_t *table, const char *name, app_ctx_t *ctx, int argc, char **argv)
{
   if (!name || strcmp(name, "help") == 0 || strcmp(name, "--help") == 0)
   {
      /* Derive parent name from table context is not possible here,
       * so just print a generic header. Callers that want a specific
       * parent name should call subcmd_usage() directly. */
      fprintf(stderr, "Subcommands:\n");
      for (int i = 0; table[i].name; i++)
      {
         if (table[i].help)
            fprintf(stderr, "  %-16s %s\n", table[i].name, table[i].help);
         else
            fprintf(stderr, "  %s\n", table[i].name);
      }
      return 0;
   }

   for (int i = 0; table[i].name; i++)
   {
      if (strcmp(name, table[i].name) == 0)
      {
         table[i].handler(ctx, argc, argv);
         return 0;
      }
   }
   return -1;
}

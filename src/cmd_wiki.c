/* cmd_wiki.c: `aimee wiki` subcommand family.
 *
 * Subcommands:
 *   render   — write deterministic markdown projection of the memory store */

#include "aimee.h"
#include "commands.h"
#include "db1_client/db1.h"
#include "wiki_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wiki_render_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   const char *out_dir = NULL;
   for (int i = 0; i < argc - 1; i++)
   {
      if (strcmp(argv[i], "--out") == 0)
      {
         out_dir = argv[i + 1];
         break;
      }
   }
   if (!out_dir)
   {
      fprintf(stderr, "Usage: aimee wiki render --out <dir>\n");
      exit(1);
   }
   if (wiki_render(out_dir) != 0)
      fatal("wiki render failed: could not write to %s", out_dir);
   printf("wiki written to %s\n", out_dir);
}

static const subcmd_t cmd_wiki_subs[] = {
    {"render", "render the wiki to a directory", wiki_render_cmd},
    {NULL, NULL, NULL},
};

void cmd_wiki(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee wiki <subcommand> [args]\n"
                      "Subcommands: render --out <dir>\n");
      exit(1);
   }

   cmd_require_db1("cannot initialize DB1");

   const char *sub = argv[0];
   argc--;
   argv++;

   if (subcmd_dispatch(cmd_wiki_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown wiki subcommand: %s", sub);
}

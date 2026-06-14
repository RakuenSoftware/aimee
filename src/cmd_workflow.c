/* cmd_workflow.c -- `aimee workflow` client command (local, no server):
 * inspect the block catalog, validate/show workflow definitions, list and
 * scaffold workflows. The engine that runs workflows lands in later slices. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee_home.h"
#include "cli_client.h"
#include "wfe_def.h"

#ifndef _WIN32
#include <dirent.h>
#endif

static void print_blocks(void)
{
   static const wfe_block_type_t order[] = {
       WFE_BLK_AUTHOR_PROPOSAL, WFE_BLK_AUTHOR_PLAN, WFE_BLK_IMPLEMENT,
       WFE_BLK_DOCUMENT,        WFE_BLK_FREEZE,      WFE_BLK_GATE_ROUNDTABLE,
       WFE_BLK_GATE_HUMAN,      WFE_BLK_PR_OPEN,     WFE_BLK_MERGE};
   printf("Workflow block catalog:\n");
   for (size_t i = 0; i < sizeof order / sizeof order[0]; i++)
   {
      wfe_block_type_t t = order[i];
      printf("  %-18s produces %-12s accepts:", wfe_block_name(t),
             wfe_artifact_name(wfe_block_output(t)));
      int any = 0;
      for (wfe_artifact_type_t a = WFE_ART_PROPOSAL; a < WFE_ART__COUNT; a++)
         if (wfe_block_accepts_input(t, a))
         {
            printf(" %s", wfe_artifact_name(a));
            any = 1;
         }
      if (!any)
         printf(" (none)");
      printf("\n");
   }
}

static int load_or_report(const char *path, wfe_def_t **out)
{
   char err[256];
   wfe_def_t *def = wfe_def_load_file(path, err, sizeof err);
   if (!def)
   {
      fprintf(stderr, "workflow: %s\n", err);
      return 1;
   }
   *out = def;
   return 0;
}

static int cmd_validate(const char *path, int json_output)
{
   wfe_def_t *def = NULL;
   if (load_or_report(path, &def) != 0)
      return 1;
   char err[256];
   int rc = wfe_def_validate(def, err, sizeof err);
   char ver[65] = "";
   if (rc == 0)
      wfe_def_compute_version(def, ver);
   if (json_output)
   {
      printf("{\"valid\":%s,\"name\":\"%s\",\"version\":\"%s\"", rc == 0 ? "true" : "false",
             def->name, ver);
      if (rc != 0)
         printf(",\"error\":\"%s\"", err);
      printf("}\n");
   }
   else if (rc == 0)
   {
      printf("ok: '%s' valid (%d nodes), version %s\n", def->name, def->n_nodes, ver);
   }
   else
   {
      fprintf(stderr, "invalid: %s\n", err);
   }
   wfe_def_free(def);
   return rc == 0 ? 0 : 1;
}

static int cmd_show(const char *path)
{
   wfe_def_t *def = NULL;
   if (load_or_report(path, &def) != 0)
      return 1;
   char ver[65] = "";
   wfe_def_compute_version(def, ver);
   char *canon = wfe_def_canonical(def);
   printf("# name: %s\n# version: %s\n%s", def->name, ver, canon ? canon : "");
   free(canon);
   wfe_def_free(def);
   return 0;
}

static int cmd_list(void)
{
#ifndef _WIN32
   char dir[1024];
   snprintf(dir, sizeof dir, "%s/workflows", aimee_home());
   DIR *d = opendir(dir);
   if (!d)
   {
      printf("(no workflows in %s)\n", dir);
      return 0;
   }
   struct dirent *e;
   int n = 0;
   while ((e = readdir(d)))
   {
      const char *dot = strrchr(e->d_name, '.');
      if (dot && strcmp(dot, ".yaml") == 0)
      {
         char path[2048];
         snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
         char err[256];
         wfe_def_t *def = wfe_def_load_file(path, err, sizeof err);
         char ver[65] = "";
         int valid = def && wfe_def_validate(def, err, sizeof err) == 0;
         if (valid)
            wfe_def_compute_version(def, ver);
         printf("  %-24s %s %s\n", e->d_name, valid ? "valid" : "INVALID", ver);
         if (def)
            wfe_def_free(def);
         n++;
      }
   }
   closedir(d);
   if (n == 0)
      printf("(no .yaml workflows in %s)\n", dir);
   return 0;
#else
   printf("workflow list: not supported on this platform\n");
   return 0;
#endif
}

static const char *TEMPLATE = "name: %s\n"
                              "start: draft\n"
                              "nodes:\n"
                              "  - id: draft\n"
                              "    block: author.proposal\n"
                              "    params:\n"
                              "      with_user: true\n"
                              "    next: review\n"
                              "  - id: review\n"
                              "    block: gate.roundtable\n"
                              "    in:\n"
                              "      src: draft.out\n"
                              "    params:\n"
                              "      panel:\n"
                              "        required:\n"
                              "          - security\n"
                              "          - architect\n"
                              "      quorum: 2\n"
                              "      max_rounds: 6\n"
                              "    on_pass: done\n"
                              "    on_fail: draft\n"
                              "  - id: done\n"
                              "    block: merge\n"
                              "    in:\n"
                              "      pr: draft.out\n";

static int cmd_new(const char *path)
{
   FILE *f = fopen(path, "wx");
   if (!f)
   {
      fprintf(stderr, "workflow: cannot create '%s' (exists?)\n", path);
      return 1;
   }
   const char *base = strrchr(path, '/');
   char name[64];
   snprintf(name, sizeof name, "%s", base ? base + 1 : path);
   char *dot = strrchr(name, '.');
   if (dot)
      *dot = '\0';
   fprintf(f, TEMPLATE, name);
   fclose(f);
   printf("created %s (edit, then `aimee workflow validate %s`)\n", path, path);
   return 0;
}

static void usage(void)
{
   fprintf(stderr, "Usage: aimee workflow <subcommand>\n"
                   "  blocks                 list the composable block catalog\n"
                   "  validate <file.yaml>   typed-graph validate a workflow\n"
                   "  show <file.yaml>       print the canonical form + version\n"
                   "  list                   list workflows under $AIMEE_HOME/workflows\n"
                   "  new <file.yaml>        scaffold a starter workflow\n");
}

int cmd_workflow_client_run(int argc, char **argv, int json_output)
{
   if (argc < 1)
   {
      usage();
      return 2;
   }
   const char *sub = argv[0];
   if (strcmp(sub, "blocks") == 0)
   {
      print_blocks();
      return 0;
   }
   if (strcmp(sub, "list") == 0)
      return cmd_list();
   if (strcmp(sub, "validate") == 0)
   {
      if (argc < 2)
      {
         usage();
         return 2;
      }
      return cmd_validate(argv[1], json_output);
   }
   if (strcmp(sub, "show") == 0)
   {
      if (argc < 2)
      {
         usage();
         return 2;
      }
      return cmd_show(argv[1]);
   }
   if (strcmp(sub, "new") == 0)
   {
      if (argc < 2)
      {
         usage();
         return 2;
      }
      return cmd_new(argv[1]);
   }
   usage();
   return 2;
}

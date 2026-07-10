/* test_wfe_router_catalog.c -- the router catalog I/O layer: enumerate
 * $AIMEE_HOME/workflows/<name>.yaml, read router metadata, add the built-in lanes,
 * skip symlinks (escape guard), and reject a workflow that claims `default`. */
#include "wfe_test_home.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wfe_router.h"

static void writef(const char *path, const char *content)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   fputs(content, f);
   fclose(f);
}

int main(void)
{
   printf("wfe-router-catalog: ");
   char tmpl[] = "/tmp/wfe_cat_XXXXXX";
   char *dir = wfe_test_mkdtemp(tmpl);
   assert(dir);
   char wf[512];
   snprintf(wf, sizeof wf, "%s/workflows", dir);
   mkdir(wf, 0755);
   setenv("AIMEE_HOME", dir, 1);

   char p[700];
   snprintf(p, sizeof p, "%s/managed-change.yaml", wf);
   writef(p, "name: managed-change\nintent_tags:\n  - feature\n  - refactor\n"
             "enforced: true\nstart: x\nnodes:\n  - id: x\n    block: understand\n");
   snprintf(p, sizeof p, "%s/audit.yaml", wf);
   writef(p, "name: audit\nread_only: true\nintent_tags:\n  - inspect\n");

   /* symlink escape: an external file symlinked INTO the workflows dir must be
    * skipped, so its 'name: sneaky' is NOT loaded. */
   char ext[700];
   snprintf(ext, sizeof ext, "%s/external.yaml", dir);
   writef(ext, "name: sneaky\n");
   char link[700];
   snprintf(link, sizeof link, "%s/sneaky.yaml", wf);
   (void)symlink(ext, link);

   /* a workflow with an invalid (non-charset) name is skipped, not loaded. */
   snprintf(p, sizeof p, "%s/weird.yaml", wf);
   writef(p, "name: \"has space\"\nintent_tags:\n  - x\n");

   wfe_router_catalog_t cat;
   char err[256] = "";
   assert(wfe_router_catalog_load(&cat, err, sizeof err) == 0);

   /* built-in converse + research + managed-change + audit = 4 (sneaky symlink +
    * the invalid-name file both skipped) */
   assert(cat.n == 4);
   assert(wfe_router_find(&cat, "converse"));
   assert(wfe_router_find(&cat, "sneaky") == NULL);    /* symlink escape blocked */
   assert(wfe_router_find(&cat, "has space") == NULL); /* invalid id skipped */

   const wfe_router_wf_t *mc = wfe_router_find(&cat, "managed-change");
   assert(mc && mc->n_tags == 2 && strcmp(mc->tags[0], "feature") == 0);
   const wfe_router_wf_t *au = wfe_router_find(&cat, "audit");
   assert(au && au->read_only == 1);

   const wfe_router_wf_t *df = wfe_router_default(&cat);
   assert(df && strcmp(df->id, "research") == 0 && df->read_only == 1);

   /* a workflow YAML that claims `default` is rejected (research is the default) */
   snprintf(p, sizeof p, "%s/bad.yaml", wf);
   writef(p, "name: badwf\ndefault: true\n");
   wfe_router_catalog_t cat2;
   char err2[256] = "";
   assert(wfe_router_catalog_load(&cat2, err2, sizeof err2) != 0);
   assert(strstr(err2, "default") != NULL);

   printf("ok\n");
   return 0;
}

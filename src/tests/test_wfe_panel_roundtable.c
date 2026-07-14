/* test_wfe_panel_roundtable.c -- verified roundtable items -> per-lens wfe
 * verdicts (the risk-bearing mapping of the engine-backed panel). Proves:
 * per-seat attribution (token compare, not substring), blocking items ->
 * REQUEST_CHANGES with re-grounded blocker counts, file:line locations must
 * ground in the worktree or the item demotes to a suggestion, unattributable
 * blocking items fail closed onto lens 0, the artifact hash is stamped for the
 * gate integrity check, and feedback carries the rendered items. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server/wfe_panel_roundtable.h"

static const char *LENS[2] = {"security", "qa"};
static const char *SEAT[2] = {"codex", "mimo"};

static void add_item(roundtable_result_t *rt, const char *sev, const char *loc, const char *sum,
                     const char *sources)
{
   roundtable_review_item_t *it = &rt->items[rt->item_count++];
   memset(it, 0, sizeof *it);
   snprintf(it->severity, sizeof it->severity, "%s", sev);
   snprintf(it->location, sizeof it->location, "%s", loc);
   snprintf(it->summary, sizeof it->summary, "%s", sum);
   snprintf(it->recommendation, sizeof it->recommendation, "fix it");
   snprintf(it->sources, sizeof it->sources, "%s", sources);
}

int main(void)
{
   printf("wfe-panel-roundtable: ");

   char dir[] = "/tmp/wfe-panel-rt-XXXXXX";
   assert(mkdtemp(dir) != NULL);
   char sub[600];
   snprintf(sub, sizeof sub, "%s/src", dir);
   assert(mkdir(sub, 0755) == 0);
   {
      char path[600];
      snprintf(path, sizeof path, "%s/src/a.c", dir);
      FILE *fp = fopen(path, "w");
      assert(fp);
      fputs("one\ntwo\nthree\n", fp);
      fclose(fp);
   }

   roundtable_result_t *rt = calloc(1, sizeof *rt);
   assert(rt);

   /* clean panel: everyone approves, hash stamped */
   {
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "HASH1", dir, v) == 2);
      for (int i = 0; i < 2; i++)
      {
         assert(v[i].kind == WFE_V_APPROVE && v[i].high_sev_blockers == 0);
         assert(strcmp(v[i].reviewed_content_hash, "HASH1") == 0);
         assert(v[i].schema_version == WFE_VERDICT_SCHEMA);
         assert(strcmp(v[i].persona, LENS[i]) == 0 && strcmp(v[i].model, SEAT[i]) == 0);
      }
   }

   /* a grounded blocking item requests changes ONLY for its source's lens */
   {
      memset(rt, 0, sizeof *rt);
      add_item(rt, "blocking", "src/a.c:2", "bad two", "codex");
      add_item(rt, "suggestion", "src/a.c:1", "style nit", "mimo");
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "H", dir, v) == 2);
      assert(v[0].kind == WFE_V_REQUEST_CHANGES && v[0].high_sev_blockers == 1);
      assert(strstr(v[0].feedback, "bad two") && strstr(v[0].feedback, "src/a.c:2"));
      assert(v[1].kind == WFE_V_APPROVE && v[1].high_sev_blockers == 0);
      assert(strstr(v[1].feedback, "style nit")); /* non-blocking still threaded */
   }

   /* a blocking item shared by both panelists blocks both lenses */
   {
      memset(rt, 0, sizeof *rt);
      add_item(rt, "blocking", "src/a.c:3", "bad three", "codex, mimo");
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "H", dir, v) == 2);
      assert(v[0].kind == WFE_V_REQUEST_CHANGES && v[1].kind == WFE_V_REQUEST_CHANGES);
   }

   /* attribution is a token compare: agent "mimo" never matches "mimo-pro" */
   {
      memset(rt, 0, sizeof *rt);
      add_item(rt, "blocking", "src/a.c:1", "bad one", "mimo-pro");
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "H", dir, v) == 2);
      /* unattributable blocking item fails closed onto lens 0 */
      assert(v[0].kind == WFE_V_REQUEST_CHANGES && v[0].high_sev_blockers == 1);
      assert(v[1].kind == WFE_V_APPROVE);
   }

   /* a blocking item whose file:line does NOT ground demotes to a suggestion:
    * fabricated file, and line past EOF */
   {
      memset(rt, 0, sizeof *rt);
      add_item(rt, "blocking", "src/ghost.c:2", "phantom", "codex");
      add_item(rt, "blocking", "src/a.c:40", "past eof", "mimo");
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "H", dir, v) == 2);
      assert(v[0].kind == WFE_V_APPROVE && v[1].kind == WFE_V_APPROVE);
      assert(strstr(v[0].feedback, "phantom")); /* still visible as a suggestion */
   }
   /* traversal / absolute locations never ground */
   {
      memset(rt, 0, sizeof *rt);
      add_item(rt, "blocking", "../a.c:1", "escape", "codex");
      add_item(rt, "blocking", "/etc/hostname:1", "abs", "mimo");
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "H", dir, v) == 2);
      assert(v[0].kind == WFE_V_APPROVE && v[1].kind == WFE_V_APPROVE);
   }

   /* a non-file:line location stays blocking (evidence replay already vetted
    * it — only fabricated citations demote) */
   {
      memset(rt, 0, sizeof *rt);
      add_item(rt, "blocking", "artifact section 2", "design flaw", "mimo");
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "H", dir, v) == 2);
      assert(v[1].kind == WFE_V_REQUEST_CHANGES && v[1].high_sev_blockers == 1);
   }

   /* NULL workdir skips grounding rather than demoting */
   {
      memset(rt, 0, sizeof *rt);
      add_item(rt, "blocking", "src/ghost.c:2", "cannot check", "codex");
      wfe_verdict_t v[2];
      assert(wfe_panel_verdicts_from_roundtable(rt, LENS, SEAT, 2, "H", NULL, v) == 2);
      assert(v[0].kind == WFE_V_REQUEST_CHANGES);
   }

   /* NULL args fail */
   assert(wfe_panel_verdicts_from_roundtable(NULL, LENS, SEAT, 2, "H", dir, NULL) == -1);

   free(rt);
   printf("ok\n");
   return 0;
}

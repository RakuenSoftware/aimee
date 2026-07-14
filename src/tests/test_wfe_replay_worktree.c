/* test_wfe_replay_worktree.c -- the worktree-grounded evidence-replay backend:
 * symbol/callers/search counts over a fixture tree, word-boundary and call-site
 * discrimination, .git exclusion, and the no-root fail-safe (INDEX_UNAVAILABLE
 * so items degrade instead of being falsely contradicted). Also proves the
 * backend composes with evidence_replay_with end-to-end. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server/wfe_replay_worktree.h"

static void write_file(const char *dir, const char *rel, const char *content)
{
   char path[512];
   snprintf(path, sizeof path, "%s/%s", dir, rel);
   FILE *fp = fopen(path, "w");
   assert(fp);
   fputs(content, fp);
   fclose(fp);
}

int main(void)
{
   printf("wfe-replay-worktree: ");

   char dir[] = "/tmp/wfe-replay-wt-XXXXXX";
   assert(mkdtemp(dir) != NULL);
   char sub[600];
   snprintf(sub, sizeof sub, "%s/src", dir);
   assert(mkdir(sub, 0755) == 0);
   snprintf(sub, sizeof sub, "%s/.git", dir);
   assert(mkdir(sub, 0755) == 0);

   write_file(dir, "src/a.c",
              "int frobnicate(int x)\n{\n   return x + 1;\n}\n"
              "int use(void)\n{\n   return frobnicate(2) + frobnicate(3);\n}\n");
   write_file(dir, "src/b.c", "/* frobnicate_helper is unrelated */\nint y = frobnicate(9);\n");
   write_file(dir, ".git/blob", "frobnicate frobnicate frobnicate\n");

   const replay_backend_t *be = wfe_replay_worktree_backend();

   /* no root bound -> "no index here" (project_count 0) */
   assert(be->project_count() == 0);

   wfe_replay_worktree_set_root(dir);
   assert(be->project_count() == 1);

   /* symbol: word-boundary lines mentioning frobnicate; frobnicate_helper and
    * anything under .git must NOT count. Lines: a.c 1, a.c 7, b.c 2 = 3. */
   {
      term_hit_t hits[16];
      int n = be->find_symbol("frobnicate", hits, 16);
      assert(n == 3);
      assert(hits[0].line >= 1 && hits[0].file_path[0] != '\0');
   }
   /* callers: only call-shaped occurrences (name followed by '('), which still
    * includes the definition line — grep-level grounding re-grounds counts, it
    * does not distinguish definition from call. Lines: a.c 1, a.c 7, b.c 2. */
   {
      caller_hit_t hits[16];
      int n = be->find_callers("", "frobnicate", hits, 16);
      assert(n == 3);
   }
   /* search: plain substring, so the helper comment line now matches too. */
   {
      code_search_hit_t hits[16];
      int n = be->code_search("frobnicate", "", hits, 16);
      assert(n == 4);
      n = be->code_search("no-such-string-anywhere", "", hits, 16);
      assert(n == 0);
   }
   /* hit sink smaller than the match count: count is still the true total */
   {
      term_hit_t hits[1];
      int n = be->find_symbol("frobnicate", hits, 1);
      assert(n == 3);
   }

   /* end-to-end through the replay engine: a factual symbol claim reproduces */
   {
      review_evidence_t ev;
      memset(&ev, 0, sizeof ev);
      ev.kind = EV_SYMBOL;
      snprintf(ev.target, sizeof ev.target, "frobnicate");
      ev.factual = 1;
      reduced_record_t rec;
      assert(evidence_replay_with(be, &ev, &rec) == REPLAY_MATCH);
      assert(rec.count == 3 && rec.idkey[0] != '\0');

      /* a fabricated symbol is CONTRADICTED (index populated, no hits) */
      snprintf(ev.target, sizeof ev.target, "no_such_symbol_xyz");
      assert(evidence_replay_with(be, &ev, &rec) == REPLAY_CONTRADICTED);
   }
   /* no root -> INDEX_UNAVAILABLE (degrade, never contradict) */
   {
      wfe_replay_worktree_set_root(NULL);
      review_evidence_t ev;
      memset(&ev, 0, sizeof ev);
      ev.kind = EV_SYMBOL;
      snprintf(ev.target, sizeof ev.target, "frobnicate");
      reduced_record_t rec;
      assert(evidence_replay_with(be, &ev, &rec) == REPLAY_INDEX_UNAVAILABLE);
   }

   printf("ok\n");
   return 0;
}

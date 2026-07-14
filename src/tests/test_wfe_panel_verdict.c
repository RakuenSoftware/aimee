/* test_wfe_panel_verdict.c -- the live panel's review->verdict mapping (the
 * risk-bearing part of the panel provider; the per-persona dispatch itself is
 * integration-gated). Proves: the last JSON line wins, the reviewed hash is stamped
 * for the gate's integrity check, and anything unparseable/unknown FAILS CLOSED to
 * MALFORMED (never a default approve). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server/wfe_panel_verdict.h"

static wfe_verdict_t map(const char *review)
{
   wfe_verdict_t v;
   wfe_panel_verdict_from_review("security", "HASH123", review, &v);
   return v;
}

/* A request_changes verdict citing one blocker at file:line with `quote`. */
static wfe_verdict_t map_blocker(const char *file, int line, const char *quote)
{
   char review[512];
   snprintf(review, sizeof review,
            "found a bug.\n{\"verdict\":\"request_changes\",\"high_sev_blockers\":3,"
            "\"blockers\":[{\"file\":\"%s\",\"line\":%d,\"quote\":\"%s\",\"summary\":\"bad\"}]}",
            file, line, quote);
   return map(review);
}

int main(void)
{
   printf("wfe-panel-verdict: ");

   /* approve */
   {
      wfe_verdict_t v = map("looks good overall.\n{\"verdict\":\"approve\"}");
      assert(v.kind == WFE_V_APPROVE);
      assert(strcmp(v.persona, "security") == 0);
      assert(strcmp(v.reviewed_content_hash, "HASH123") == 0); /* gate integrity check */
      assert(v.schema_version == WFE_VERDICT_SCHEMA);
      assert(v.high_sev_blockers == 0);
      assert(strcmp(v.feedback, "looks good overall.") == 0); /* critique captured */
   }
   /* request_changes with blocker count + captured critique (threaded to re-author) */
   {
      wfe_verdict_t v =
          map("found a bug.\n{\"verdict\":\"request_changes\",\"high_sev_blockers\":2}");
      assert(v.kind == WFE_V_REQUEST_CHANGES);
      assert(v.high_sev_blockers == 2);
      assert(strcmp(v.feedback, "found a bug.") == 0);
   }
   /* no reasoning (verdict line only) -> feedback empty */
   assert(map("{\"verdict\":\"request_changes\"}").feedback[0] == '\0');
   /* comment */
   assert(map("{\"verdict\":\"comment\"}").kind == WFE_V_COMMENT);

   /* FAIL CLOSED: empty / unparseable / unknown verdict / missing field -> MALFORMED */
   assert(map("").kind == WFE_V_MALFORMED);
   assert(map("no json here at all").kind == WFE_V_MALFORMED);
   assert(map("{\"verdict\":\"looks_ok\"}").kind == WFE_V_MALFORMED);
   assert(map("{\"note\":\"hi\"}").kind == WFE_V_MALFORMED);
   assert(map("{\"verdict\": 1}").kind == WFE_V_MALFORMED);

   /* the LAST non-empty line wins: quoted approve in the reasoning must not
    * false-approve when the real verdict line requests changes. */
   {
      wfe_verdict_t v = map("I first thought {\"verdict\":\"approve\"} but then...\n"
                            "{\"verdict\":\"request_changes\",\"high_sev_blockers\":1}");
      assert(v.kind == WFE_V_REQUEST_CHANGES && v.high_sev_blockers == 1);
   }
   /* trailing whitespace/newlines after the JSON line are tolerated */
   assert(map("{\"verdict\":\"approve\"}\n\n  \n").kind == WFE_V_APPROVE);

   /* a markdown-fenced verdict (the common provider habit) is still parsed:
    * trailing fence-only lines are skipped, not treated as the verdict line */
   assert(map("blockers noted.\n```json\n{\"verdict\":\"request_changes\"}\n```\n").kind ==
          WFE_V_REQUEST_CHANGES);
   assert(map("```\n{\"verdict\":\"approve\"}\n```").kind == WFE_V_APPROVE);
   /* a fence-only response stays MALFORMED (nothing above the fences) */
   assert(map("```\n```").kind == WFE_V_MALFORMED);
   /* prose after the fenced verdict still wins as the last line (no rescan) */
   assert(map("```\n{\"verdict\":\"approve\"}\n```\nnot json").kind == WFE_V_MALFORMED);

   /* a null artifact hash still yields a (fail-closed) verdict with an empty hash */
   {
      wfe_verdict_t v;
      wfe_panel_verdict_from_review("qa", NULL, "{\"verdict\":\"approve\"}", &v);
      assert(v.kind == WFE_V_APPROVE && v.reviewed_content_hash[0] == '\0');
   }

   /* ---- blocker citations: parse ---- */
   {
      wfe_verdict_t v = map_blocker("src/a.c", 12, "free(p);");
      assert(v.kind == WFE_V_REQUEST_CHANGES && v.blocker_count == 1);
      assert(strcmp(v.blockers[0].file, "src/a.c") == 0 && v.blockers[0].line == 12);
      assert(strcmp(v.blockers[0].quote, "free(p);") == 0);
      assert(strcmp(v.blockers[0].summary, "bad") == 0 && v.blockers[0].verified == 0);
   }
   /* entries without a file or a positive line are interpretive -> not stored */
   {
      wfe_verdict_t v = map("x\n{\"verdict\":\"request_changes\",\"blockers\":["
                            "{\"line\":3,\"quote\":\"q\"},{\"file\":\"a.c\",\"line\":0},"
                            "{\"file\":\"a.c\",\"line\":7}]}");
      assert(v.blocker_count == 1 && v.blockers[0].line == 7);
   }
   /* no blockers key -> zero */
   assert(map("{\"verdict\":\"request_changes\"}").blocker_count == 0);

   /* ---- blocker citations: replay against a worktree fixture ---- */
   {
      char dir[] = "/tmp/wfe-panel-verdict-XXXXXX";
      assert(mkdtemp(dir) != NULL);
      char sub[300], path[300];
      snprintf(sub, sizeof sub, "%s/src", dir);
      assert(mkdir(sub, 0755) == 0);
      snprintf(path, sizeof path, "%s/src/a.c", dir);
      FILE *fp = fopen(path, "w");
      assert(fp);
      fputs("int main(void)\n{\n   char *p = malloc(4);\n   free(p);\n   return 0;\n}\n", fp);
      fclose(fp);

      /* exact file:line + quote reproduces -> blocker stands, weight re-grounded */
      {
         wfe_verdict_t v = map_blocker("src/a.c", 4, "free(p);");
         assert(wfe_panel_blockers_verify(&v, dir) == 1);
         assert(v.kind == WFE_V_REQUEST_CHANGES && v.blockers[0].verified == 1);
         assert(v.high_sev_blockers == 1); /* claimed 3, one reproduced */
      }
      /* off-by-two line still reproduces (diff-counting tolerance) */
      {
         wfe_verdict_t v = map_blocker("src/a.c", 6, "free(p);");
         assert(wfe_panel_blockers_verify(&v, dir) == 1);
      }
      /* quote-less citation: cited line exists -> reproduces at the weak level */
      {
         wfe_verdict_t v = map_blocker("src/a.c", 4, "");
         assert(wfe_panel_blockers_verify(&v, dir) == 1);
      }
      /* quote-less citation past EOF -> re-graded to comment */
      {
         wfe_verdict_t v = map_blocker("src/a.c", 40, "");
         assert(wfe_panel_blockers_verify(&v, dir) == 0);
         assert(v.kind == WFE_V_COMMENT && v.high_sev_blockers == 0);
         assert(strstr(v.feedback, "[panel-verify]"));
      }
      /* fabricated quote -> re-graded to comment */
      {
         wfe_verdict_t v = map_blocker("src/a.c", 4, "unlock(&mu);");
         assert(wfe_panel_blockers_verify(&v, dir) == 0 && v.kind == WFE_V_COMMENT);
      }
      /* fabricated file -> re-graded to comment */
      {
         wfe_verdict_t v = map_blocker("src/nope.c", 4, "free(p);");
         assert(wfe_panel_blockers_verify(&v, dir) == 0 && v.kind == WFE_V_COMMENT);
      }
      /* traversal / absolute citations never reproduce */
      {
         wfe_verdict_t v = map_blocker("../a.c", 1, "int");
         assert(wfe_panel_blockers_verify(&v, dir) == 0 && v.kind == WFE_V_COMMENT);
         v = map_blocker("/etc/hostname", 1, "");
         assert(v.blocker_count == 1); /* parsed, but... */
         assert(wfe_panel_blockers_verify(&v, dir) == 0 && v.kind == WFE_V_COMMENT);
      }
      /* uncited request_changes (no blockers at all) can no longer block */
      {
         wfe_verdict_t v = map("bad vibes.\n{\"verdict\":\"request_changes\","
                               "\"high_sev_blockers\":2}");
         assert(wfe_panel_blockers_verify(&v, dir) == 0);
         assert(v.kind == WFE_V_COMMENT && v.high_sev_blockers == 0);
      }
      /* one real + one fabricated citation: stands, unverified one discarded */
      {
         wfe_verdict_t v =
             map("x\n{\"verdict\":\"request_changes\",\"high_sev_blockers\":2,\"blockers\":["
                 "{\"file\":\"src/a.c\",\"line\":4,\"quote\":\"free(p);\"},"
                 "{\"file\":\"src/ghost.c\",\"line\":9,\"quote\":\"boom\"}]}");
         assert(wfe_panel_blockers_verify(&v, dir) == 1);
         assert(v.kind == WFE_V_REQUEST_CHANGES && v.high_sev_blockers == 1);
         assert(v.blockers[0].verified == 1 && v.blockers[1].verified == 0);
      }
      /* NULL/empty workdir: nothing to replay against -> verdict untouched */
      {
         wfe_verdict_t v = map_blocker("src/ghost.c", 9, "boom");
         assert(wfe_panel_blockers_verify(&v, NULL) == 1);
         assert(v.kind == WFE_V_REQUEST_CHANGES && v.high_sev_blockers == 3);
      }
      /* non-request_changes verdicts are never re-graded */
      {
         wfe_verdict_t v = map("{\"verdict\":\"approve\"}");
         assert(wfe_panel_blockers_verify(&v, dir) == 0 && v.kind == WFE_V_APPROVE);
      }

      remove(path);
      rmdir(sub);
      rmdir(dir);
   }

   printf("ok\n");
   return 0;
}

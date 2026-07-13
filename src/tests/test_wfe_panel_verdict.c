/* test_wfe_panel_verdict.c -- the live panel's review->verdict mapping (the
 * risk-bearing part of the panel provider; the per-persona dispatch itself is
 * integration-gated). Proves: the last JSON line wins, the reviewed hash is stamped
 * for the gate's integrity check, and anything unparseable/unknown FAILS CLOSED to
 * MALFORMED (never a default approve). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "server/wfe_panel_verdict.h"

static wfe_verdict_t map(const char *review)
{
   wfe_verdict_t v;
   wfe_panel_verdict_from_review("security", "HASH123", review, &v);
   return v;
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

   printf("ok\n");
   return 0;
}

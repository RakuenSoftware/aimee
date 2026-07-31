/* test_kb_surprising_judge.c: hermetic test of the §4 surprising-links judge.
 *
 * The two DB accessors (node_key->path, file symbols) are stubbed here so the test
 * needs no database; the LLM call is faked through the curator sidecar seam (cfg=NULL
 * + a `printf` judge_cmd whose stdout IS the model response) — the same seam
 * test_curator_judge.c uses. So this exercises the REAL request build + response parse
 * in kb_surprising_judge.c, with only the network/DB faked. */

#include "kb/kb_surprising_judge.h"
#include "headers/index.h" /* definition_t */

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── stubbed DB accessors ─────────────────────────────────────────────────── */

int pgvec_code_node_path(const char *project, const char *node_key, char *out, int out_cap)
{
   (void)project;
   if (!node_key || !out || out_cap <= 0)
      return -1;
   /* file:p:x -> src/x.c ; file:p:y -> src/y.c ; anything else unresolved (-1). */
   if (strcmp(node_key, "file:p:x") == 0)
   {
      snprintf(out, (size_t)out_cap, "src/x.c");
      return 0;
   }
   if (strcmp(node_key, "file:p:y") == 0)
   {
      snprintf(out, (size_t)out_cap, "src/y.c");
      return 0;
   }
   return -1;
}

int canonical_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max)
{
   (void)project;
   if (!out || max <= 0)
      return 0;
   /* src/x.c and src/y.c share "shared_fn" (1 shared symbol) plus a unique each. */
   int n = 0;
   const char *uniq = strcmp(file_path, "src/x.c") == 0 ? "x_only" : "y_only";
   snprintf(out[n].name, sizeof(out[n].name), "shared_fn");
   snprintf(out[n].kind, sizeof(out[n].kind), "function");
   n++;
   if (n < max)
   {
      snprintf(out[n].name, sizeof(out[n].name), "%s", uniq);
      snprintf(out[n].kind, sizeof(out[n].kind), "function");
      n++;
   }
   return n;
}

/* ── tests ────────────────────────────────────────────────────────────────── */

static kb_graph_surprising_t mk_link(const char *a, const char *b, double cos, int hops)
{
   kb_graph_surprising_t l;
   memset(&l, 0, sizeof(l));
   snprintf(l.a, sizeof(l.a), "%s", a);
   snprintf(l.b, sizeof(l.b), "%s", b);
   l.cosine = cos;
   l.hops = hops;
   return l;
}

/* Faked model response confirms link 0, rejects link 1. */
static void test_judge_parses_verdicts(void)
{
   kb_graph_surprising_t links[2] = {mk_link("file:p:x", "file:p:y", 0.93, -1),
                                     mk_link("file:p:x", "file:p:y", 0.80, 5)};
   kb_surprising_verdict_t out[2];
   char err[256];
   const char *cmd =
       "printf '{\"verdicts\":[{\"i\":0,\"surprising\":true,\"reason\":\"dup logic\"},"
       "{\"i\":1,\"surprising\":false,\"reason\":\"coincidental\"}]}'";
   int judged = kb_surprising_judge(cmd, "p", links, 2, out, err, sizeof(err));
   assert(judged == 2);
   assert(out[0].judged == 1 && out[0].confirmed == 1);
   assert(strcmp(out[0].reason, "dup logic") == 0);
   assert(out[1].judged == 1 && out[1].confirmed == 0);
   /* both files share exactly "shared_fn" -> the cross-check counts 1. */
   assert(out[0].shared_symbols == 1);
   printf("  PASS: judge parses per-pair verdicts + shared-symbol cross-check\n");
}

/* Unparseable model output is a hard error (-1); a missing verdicts array judges 0. */
static void test_judge_error_paths(void)
{
   kb_graph_surprising_t links[1] = {mk_link("file:p:x", "file:p:y", 0.9, -1)};
   kb_surprising_verdict_t out[1];
   char err[256];
   int r1 =
       kb_surprising_judge("printf 'not json at all'", "p", links, 1, out, err, sizeof(err));
   assert(r1 == -1);
   int r2 =
       kb_surprising_judge("printf '{\"other\":1}'", "p", links, 1, out, err, sizeof(err));
   assert(r2 == 0);
   assert(out[0].judged == 0); /* no verdict -> left unconfirmed */
   printf("  PASS: unparseable -> -1, no-verdicts -> 0 (unconfirmed)\n");
}

/* A pair whose node keys don't resolve to paths is never sent to the LLM. */
static void test_judge_skips_unresolved(void)
{
   kb_graph_surprising_t links[1] = {mk_link("file:p:unknown", "file:p:y", 0.9, -1)};
   kb_surprising_verdict_t out[1];
   char err[256];
   int r = kb_surprising_judge("printf '{\"verdicts\":[{\"i\":0,\"surprising\":true}]}'", "p",
                               links, 1, out, err, sizeof(err));
   assert(r == 0); /* nothing judgeable -> no LLM call */
   assert(out[0].sent == 0 && out[0].judged == 0);
   printf("  PASS: unresolved node keys are skipped (no LLM call)\n");
}

/* A verdict for an index that was NOT sent (here link 1, whose keys don't resolve)
 * is rejected — the model can't fabricate a confirmation for an unjudged pair. */
static void test_judge_rejects_unsent_verdict(void)
{
   kb_graph_surprising_t links[2] = {mk_link("file:p:x", "file:p:y", 0.93, -1),
                                     mk_link("file:p:x", "file:p:unknown", 0.80, 5)};
   kb_surprising_verdict_t out[2];
   char err[256];
   /* link 0 is sent; link 1 is skipped (unknown). The model echoes BOTH i=0 and i=1. */
   const char *cmd = "printf '{\"verdicts\":[{\"i\":0,\"surprising\":true},"
                     "{\"i\":1,\"surprising\":true,\"reason\":\"fabricated\"}]}'";
   int judged = kb_surprising_judge(cmd, "p", links, 2, out, err, sizeof(err));
   assert(judged == 1); /* only the sent pair is judged */
   assert(out[0].sent == 1 && out[0].judged == 1 && out[0].confirmed == 1);
   assert(out[1].sent == 0 && out[1].judged == 0); /* the unsent verdict was rejected */
   printf("  PASS: verdict for an unsent pair is rejected\n");
}

int main(void)
{
   printf("test_kb_surprising_judge\n");
   test_judge_parses_verdicts();
   test_judge_error_paths();
   test_judge_skips_unresolved();
   test_judge_rejects_unsent_verdict();
   printf("  all tests passed\n");
   return 0;
}
